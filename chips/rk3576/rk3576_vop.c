/****************************************************************************
 * chips/rk3576/rk3576_vop.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * RK3576 VOP (Video Output Processor) framebuffer driver.
 *
 * The RK3576 display controller is a Rockchip "VOP2" instance.  A single
 * VOP block feeds three video ports (VP0..VP2); a set of overlay windows
 * (Cluster0/1, Esmart0/1) can be assigned to any of them.
 *
 * This driver implements the minimum useful configuration for a single
 * linear framebuffer:
 *
 *   Esmart0 window (1:1, no scaling) -> overlay layer 0 -> VP0 -> HDMI TX0
 *
 * and exports it through the standard NuttX framebuffer interface
 * (include/nuttx/video/fb.h): up_fbinitialize() / up_fbgetvplane() /
 * up_fbuninitialize().  The framebuffer memory comes from the shared
 * RK3576 DMA heap so that its physical address is guaranteed to be below
 * 4GB and 64-byte (cache line) aligned, which the VOP AXI master requires.
 *
 * The display timing is not negotiated here: the board (or an encoder
 * driver such as HDMI TX) selects a mode with rk3576_vop_set_timing()
 * before up_fbinitialize() runs, and reads it back with
 * rk3576_vop_get_timing() to program its own PHY.  1920x1080p60 is the
 * default.
 *
 * The VOP IOMMU is left in bypass (paging disabled) so the window scans
 * out physical addresses directly; see rk3576_vop_iommu_bypass().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/semaphore.h>
#include <nuttx/video/fb.h>

#include <nuttx/clk/clk.h>

#include "arm64_internal.h"
#include "hardware/rk3576_vop.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_pd.h"
#include "rk3576_vop.h"

#ifdef CONFIG_RK3576_VOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Hardware resources this driver drives.  Only one video port and one
 * window are used; the constants are named so that a second display can be
 * added later without hunting for magic numbers.
 */

#define RK3576_VOP_FB_VP     0                   /* Video port 0      */
#define RK3576_VOP_FB_ESMART 0                   /* Esmart0 window    */
#define RK3576_VOP_FB_WINID  VOP_OVL_WIN_ESMART0 /* Its overlay ID    */
#define RK3576_VOP_FB_LAYER  0                   /* Bottom Z layer    */

/* Framebuffer colour depth.  32bpp (ARGB8888) by default; 24bpp packs
 * three bytes per pixel and needs a stride rounded up to a whole word.
 */

#ifdef CONFIG_RK3576_VOP_FB_RGB888
#define RK3576_VOP_FB_BPP  24
#define RK3576_VOP_FB_FMT  FB_FMT_RGB24
#define RK3576_VOP_WIN_FMT VOP_WIN_FMT_RGB888
#else
#define RK3576_VOP_FB_BPP  32
#define RK3576_VOP_FB_FMT  FB_FMT_RGB32
#define RK3576_VOP_WIN_FMT VOP_WIN_FMT_ARGB8888
#endif

/* Opaque global alpha for the single visible window. */

#define RK3576_VOP_ALPHA_OPAQUE 0xff

/* Display background colour (black) programmed into VP DSP_BG. */

#define RK3576_VOP_BG_BLACK 0x00000000u

/* Maximum time to wait for the shadow registers to be taken, and for the
 * standby bit to actually stop the scan-out, expressed in polling loops of
 * one microsecond each.
 */

#define RK3576_VOP_STANDBY_TIMEOUT_US 50000

/* Round x up to the next multiple of a (a must be a power of two). */

#define RK3576_VOP_ALIGN_UP(x, a) (((x) + ((a)-1)) & ~((a)-1))

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Driver state.  The first member is the FB upper-half vtable so a
 * struct fb_vtable_s pointer can be up-cast to this structure.
 */

struct rk3576_vop_s
{
  struct fb_vtable_s vtable; /* FB operations (must be first) */

  bool initialized;                      /* up_fbinitialize() has run     */
  int iface;                             /* RK3576_VOP_IF_* destination   */
  struct rk3576_display_timing_s timing; /* Active display mode           */

  uint8_t *fbmem;  /* Framebuffer virtual address   */
  size_t fblen;    /* Framebuffer size in bytes     */
  uint32_t stride; /* Framebuffer stride in bytes   */

  uint32_t aclk_hz; /* Measured aclk_vop rate        */
  uint32_t dclk_hz; /* Measured dclk_vp0 rate        */

#ifdef CONFIG_FB_SYNC
  sem_t vsync; /* Posted from the VP0 ISR       */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_vop_getreg(uint32_t offset);
static void rk3576_vop_putreg(uint32_t offset, uint32_t value);
static void rk3576_vop_modifyreg(uint32_t offset, uint32_t clrbits,
                                 uint32_t setbits);
static void rk3576_vop_cfg_done(void);

static int rk3576_vop_clk_init(struct rk3576_vop_s *priv);
static void rk3576_vop_iommu_bypass(void);
static int
rk3576_vop_check_timing(const struct rk3576_display_timing_s *timing);
static void rk3576_vop_set_standby(bool standby);
static void rk3576_vop_config_timing(struct rk3576_vop_s *priv);
static void rk3576_vop_config_overlay(void);
static void rk3576_vop_config_window(struct rk3576_vop_s *priv);
static void rk3576_vop_config_output(struct rk3576_vop_s *priv);
static int rk3576_vop_interrupt(int irq, void *context, void *arg);

static int rk3576_vop_getvideoinfo(struct fb_vtable_s *vtable,
                                   struct fb_videoinfo_s *vinfo);
static int rk3576_vop_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                                   struct fb_planeinfo_s *pinfo);
#ifdef CONFIG_FB_SYNC
static int rk3576_vop_waitforvsync(struct fb_vtable_s *vtable);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CEA-861 1920x1080p60: 148.5 MHz, hsync/vsync both active high. */

static const struct rk3576_display_timing_s g_rk3576_vop_1080p60 = {
  .pixclk = 148500000,
  .hact = 1920,
  .hfp = 88,
  .hsync = 44,
  .hbp = 148,
  .htotal = 2200,
  .vact = 1080,
  .vfp = 4,
  .vsync = 5,
  .vbp = 36,
  .vtotal = 1125,
  .hsync_active_high = true,
  .vsync_active_high = true,
};

/* CEA-861 1280x720p60: 74.25 MHz, hsync/vsync both active high. */

static const struct rk3576_display_timing_s g_rk3576_vop_720p60 = {
  .pixclk = 74250000,
  .hact = 1280,
  .hfp = 110,
  .hsync = 40,
  .hbp = 220,
  .htotal = 1650,
  .vact = 720,
  .vfp = 5,
  .vsync = 5,
  .vbp = 20,
  .vtotal = 750,
  .hsync_active_high = true,
  .vsync_active_high = true,
};

static struct rk3576_vop_s g_rk3576_vop =
{
  .vtable =
  {
    .getvideoinfo  = rk3576_vop_getvideoinfo,
    .getplaneinfo  = rk3576_vop_getplaneinfo,
#ifdef CONFIG_FB_SYNC
    .waitforvsync  = rk3576_vop_waitforvsync,
#endif
  },
  .iface  = RK3576_VOP_IF_HDMI0,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_getreg / rk3576_vop_putreg / rk3576_vop_modifyreg
 *
 * Description:
 *   Access one 32-bit register in the VOP main register file.  The offset
 *   is relative to RK3576_VOP_ADDR and is always the sum of a block base
 *   (RK3576_VOP_VP(n), RK3576_VOP_ESMART(n), ...) and a register offset.
 *
 ****************************************************************************/

static uint32_t rk3576_vop_getreg(uint32_t offset)
{
  return getreg32(RK3576_VOP_ADDR + offset);
}

static void rk3576_vop_putreg(uint32_t offset, uint32_t value)
{
  putreg32(value, RK3576_VOP_ADDR + offset);
}

static void rk3576_vop_modifyreg(uint32_t offset, uint32_t clrbits,
                                 uint32_t setbits)
{
  uint32_t regval;

  regval = rk3576_vop_getreg(offset);
  regval &= ~clrbits;
  regval |= setbits;
  rk3576_vop_putreg(offset, regval);
}

/****************************************************************************
 * Name: rk3576_vop_cfg_done
 *
 * Description:
 *   Commit the shadowed timing/window registers of the framebuffer video
 *   port.  Every write to a shadowed register is buffered until this pulse
 *   is issued, which makes a multi-register update atomic with respect to
 *   the scan-out.
 *
 ****************************************************************************/

static void rk3576_vop_cfg_done(void)
{
  rk3576_vop_putreg(RK3576_VOP_REG_CFG_DONE,
                    VOP_REG_CFG_DONE_EN |
                        VOP_REG_CFG_DONE_VP(RK3576_VOP_FB_VP));
}

/****************************************************************************
 * Name: rk3576_vop_clk_init
 *
 * Description:
 *   Bring up every clock the VOP needs.  All clock handling of this driver
 *   is confined to this one function: the AXI and AHB bus clocks of the
 *   VOP block, and the pixel clock of the framebuffer video port.
 *
 *   The pixel clock rate request may be refused by the clock tree when the
 *   mode needs a fractional PLL rate that only the encoder PHY can
 *   generate (this is the normal case for HDMI, where dclk_vp0 is sourced
 *   from the HDMI PHY PLL).  That is not fatal: the achieved rate is
 *   recorded and reported, and the encoder driver owns the final rate.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_vop_clk_init(struct rk3576_vop_s *priv)
{
  struct clk_s *aclk;
  struct clk_s *hclk;
  struct clk_s *dclk;
  int ret;

  /* AXI clock of the VOP memory masters */

  aclk = clk_get("aclk_vop_en");
  if (aclk == NULL)
    {
      lcderr("ERROR: failed to get aclk_vop_en\n");
      return -ENODEV;
    }

  ret = clk_enable(aclk);
  if (ret < 0)
    {
      lcderr("ERROR: failed to enable aclk_vop_en: %d\n", ret);
      return ret;
    }

  /* AHB clock of the VOP register file */

  hclk = clk_get("hclk_vop_en");
  if (hclk == NULL)
    {
      lcderr("ERROR: failed to get hclk_vop_en\n");
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      lcderr("ERROR: failed to enable hclk_vop_en: %d\n", ret);
      return ret;
    }

  /* Pixel clock of the framebuffer video port */

  dclk = clk_get("dclk_vp0_en");
  if (dclk == NULL)
    {
      lcderr("ERROR: failed to get dclk_vp0_en\n");
      return -ENODEV;
    }

  ret = clk_set_rate(dclk, priv->timing.pixclk);
  if (ret < 0)
    {
      lcdwarn("WARNING: dclk_vp0_en cannot be set to %" PRIu32 " Hz (%d); "
              "relying on the encoder PLL\n",
              priv->timing.pixclk, ret);
    }

  ret = clk_enable(dclk);
  if (ret < 0)
    {
      lcderr("ERROR: failed to enable dclk_vp0_en: %d\n", ret);
      return ret;
    }

  priv->aclk_hz = clk_get_rate(aclk);
  priv->dclk_hz = clk_get_rate(dclk);

  lcdinfo("aclk_vop %" PRIu32 " Hz, dclk_vp0 %" PRIu32 " Hz "
          "(mode needs %" PRIu32 " Hz)\n",
          priv->aclk_hz, priv->dclk_hz, priv->timing.pixclk);

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_iommu_bypass
 *
 * Description:
 *   Put the VOP IOMMU into bypass.  The framebuffer is allocated from the
 *   physically contiguous RK3576 DMA heap, so there is nothing to scatter
 *   and no page table is required: disabling paging makes the window
 *   masters issue the physical address in WIN_YRGB_MST unmodified.
 *
 *   Page-fault reporting is masked as well, otherwise the shared
 *   "vop-sys" interrupt line would be asserted by a stale fault latched
 *   before this driver took over from the bootloader display.
 *
 ****************************************************************************/

static void rk3576_vop_iommu_bypass(void)
{
  uint32_t status;

  status = getreg32(RK3576_VOP_MMU_ADDR + RK3576_VOP_MMU_STATUS);
  if ((status & VOP_MMU_STATUS_PAGING_ENABLED) != 0)
    {
      putreg32(VOP_MMU_CMD_DISABLE_PAGING,
               RK3576_VOP_MMU_ADDR + RK3576_VOP_MMU_COMMAND);
    }

  /* Acknowledge whatever the previous owner left pending and mask the
   * IOMMU interrupt sources.
   */

  status = getreg32(RK3576_VOP_MMU_ADDR + RK3576_VOP_MMU_INT_STATUS);
  putreg32(status, RK3576_VOP_MMU_ADDR + RK3576_VOP_MMU_INT_STATUS);
  putreg32(0, RK3576_VOP_MMU_ADDR + RK3576_VOP_MMU_INT_MASK);
}

/****************************************************************************
 * Name: rk3576_vop_check_timing
 *
 * Description:
 *   Validate a display timing: the totals must agree with the porches and
 *   the active area must fit both the hardware and the driver limits.
 *
 * Returned Value:
 *   OK if the timing is usable, -EINVAL otherwise.
 *
 ****************************************************************************/

static int
rk3576_vop_check_timing(const struct rk3576_display_timing_s *timing)
{
  if (timing->hact == 0 || timing->vact == 0)
    {
      return -EINVAL;
    }

  if (timing->hact > RK3576_VOP_MAX_HACT || timing->vact > RK3576_VOP_MAX_VACT)
    {
      return -EINVAL;
    }

  if (timing->htotal !=
      (uint16_t)(timing->hact + timing->hfp + timing->hsync + timing->hbp))
    {
      return -EINVAL;
    }

  if (timing->vtotal !=
      (uint16_t)(timing->vact + timing->vfp + timing->vsync + timing->vbp))
    {
      return -EINVAL;
    }

  if (timing->hsync == 0 || timing->vsync == 0 || timing->pixclk == 0)
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_set_standby
 *
 * Description:
 *   Enter or leave display standby on the framebuffer video port.  In
 *   standby the video port stops fetching and drives blanking level, which
 *   is the state the timing registers must be reprogrammed in.
 *
 *   Leaving standby is immediate; entering it only takes effect at the end
 *   of the current frame, so the caller polls the raw frame-start status
 *   to be sure the scan-out has stopped before touching the window.
 *
 ****************************************************************************/

static void rk3576_vop_set_standby(bool standby)
{
  uint32_t vp = RK3576_VOP_VP(RK3576_VOP_FB_VP);
  int elapsed;

  if (standby)
    {
      rk3576_vop_modifyreg(vp + RK3576_VOP_VP_DSP_CTRL, 0,
                           VOP_VP_DSP_CTRL_STANDBY);
    }
  else
    {
      rk3576_vop_modifyreg(vp + RK3576_VOP_VP_DSP_CTRL,
                           VOP_VP_DSP_CTRL_STANDBY, 0);
    }

  rk3576_vop_cfg_done();

  if (!standby)
    {
      return;
    }

  /* Wait for the frame in flight to finish.  A missing pixel clock (no
   * encoder running yet) means no frame start will ever be reported, so
   * the wait is bounded and a timeout is not an error.
   */

  rk3576_vop_putreg(vp + RK3576_VOP_VP_INT_CLR,
                    VOP_INT_HIWORD_CLR(VOP_VP_INT_FS));

  for (elapsed = 0; elapsed < RK3576_VOP_STANDBY_TIMEOUT_US; elapsed++)
    {
      if ((rk3576_vop_getreg(vp + RK3576_VOP_VP_INT_RAW_STATUS) &
           VOP_VP_INT_FS) != 0)
        {
          break;
        }

      up_udelay(1);
    }
}

/****************************************************************************
 * Name: rk3576_vop_config_timing
 *
 * Description:
 *   Program the display timing of the framebuffer video port.  Every VOP
 *   timing register packs two 16-bit values; the horizontal and vertical
 *   registers hold "total"/"sync end" and "active start"/"active end",
 *   all counted from the leading edge of the sync pulse.
 *
 ****************************************************************************/

static void rk3576_vop_config_timing(struct rk3576_vop_s *priv)
{
  const struct rk3576_display_timing_s *t = &priv->timing;
  uint32_t vp = RK3576_VOP_VP(RK3576_VOP_FB_VP);
  uint16_t hact_st;
  uint16_t hact_end;
  uint16_t vact_st;
  uint16_t vact_end;

  hact_st = t->hsync + t->hbp;
  hact_end = hact_st + t->hact;
  vact_st = t->vsync + t->vbp;
  vact_end = vact_st + t->vact;

  rk3576_vop_putreg(vp + RK3576_VOP_VP_DSP_HTOTAL_HS_END,
                    VOP_TIMING_PACK(t->htotal, t->hsync));
  rk3576_vop_putreg(vp + RK3576_VOP_VP_DSP_HACT_ST_END,
                    VOP_TIMING_PACK(hact_st, hact_end));
  rk3576_vop_putreg(vp + RK3576_VOP_VP_DSP_VTOTAL_VS_END,
                    VOP_TIMING_PACK(t->vtotal, t->vsync));
  rk3576_vop_putreg(vp + RK3576_VOP_VP_DSP_VACT_ST_END,
                    VOP_TIMING_PACK(vact_st, vact_end));

  /* The post-processing block is used in bypass (no post scaling), so its
   * active window is the same as the display active window.
   */

  rk3576_vop_putreg(vp + RK3576_VOP_VP_POST_DSP_HACT_INFO,
                    VOP_TIMING_PACK(hact_st, hact_end));
  rk3576_vop_putreg(vp + RK3576_VOP_VP_POST_DSP_VACT_INFO,
                    VOP_TIMING_PACK(vact_st, vact_end));
  rk3576_vop_putreg(vp + RK3576_VOP_VP_POST_SCL_CTRL, 0);

  /* Background shown wherever no window is visible, and no colour bar. */

  rk3576_vop_putreg(vp + RK3576_VOP_VP_DSP_BG, RK3576_VOP_BG_BLACK);
  rk3576_vop_putreg(vp + RK3576_VOP_VP_COLOR_BAR_CTRL, 0);

  /* Progressive 24-bit RGB output, no dithering, no gamma LUT. */

  rk3576_vop_modifyreg(
      vp + RK3576_VOP_VP_DSP_CTRL,
      VOP_VP_DSP_CTRL_OUT_MODE_MASK | VOP_VP_DSP_CTRL_INTERLACE |
          VOP_VP_DSP_CTRL_P2I_EN | VOP_VP_DSP_CTRL_DSP_BLANK |
          VOP_VP_DSP_CTRL_DSP_LUT_EN | VOP_VP_DSP_CTRL_PRE_DITHER_DOWN_EN |
          VOP_VP_DSP_CTRL_DITHER_DOWN_EN,
      VOP_VP_DSP_CTRL_OUT_MODE(VOP_VP_OUT_MODE_P888));
}

/****************************************************************************
 * Name: rk3576_vop_config_overlay
 *
 * Description:
 *   Route the single window through the overlay engine: overlay layer 0
 *   (the bottom of the Z stack) is fed by the Esmart0 window, and that
 *   layer is assigned to the framebuffer video port.  Every other video
 *   port gets zero layers.
 *
 ****************************************************************************/

static void rk3576_vop_config_overlay(void)
{
  uint32_t portsel;
  int vp;

  /* Layer 0 <- Esmart0 */

  rk3576_vop_modifyreg(
      RK3576_VOP_OVL_LAYER_SEL, VOP_OVL_LAYER_SEL_MASK(RK3576_VOP_FB_LAYER),
      VOP_OVL_LAYER_SEL(RK3576_VOP_FB_LAYER, RK3576_VOP_FB_WINID));

  /* One layer on the framebuffer video port, none on the others, and
   * layer 0 routed to it.
   */

  portsel = rk3576_vop_getreg(RK3576_VOP_OVL_PORT_SEL);

  for (vp = 0; vp < RK3576_VOP_NVP; vp++)
    {
      portsel &= ~VOP_OVL_PORT_SEL_NLAYER_MASK(vp);
      portsel |= VOP_OVL_PORT_SEL_NLAYER(vp, vp == RK3576_VOP_FB_VP ? 1 : 0);
    }

  portsel &= ~VOP_OVL_PORT_SEL_LAYER_MASK(RK3576_VOP_FB_LAYER);
  portsel |= VOP_OVL_PORT_SEL_LAYER(RK3576_VOP_FB_LAYER, RK3576_VOP_FB_VP);

  rk3576_vop_putreg(RK3576_VOP_OVL_PORT_SEL, portsel);

  /* The overlay mixer works in RGB for this port. */

  rk3576_vop_modifyreg(RK3576_VOP_OVL_CTRL,
                       VOP_OVL_CTRL_YUV_MODE_VP(RK3576_VOP_FB_VP), 0);
}

/****************************************************************************
 * Name: rk3576_vop_config_window
 *
 * Description:
 *   Program the Esmart0 window to scan the framebuffer out 1:1 over the
 *   whole active area.  Only region 0 of the window is used, which turns
 *   the four-region Esmart into a plain single-rectangle plane.
 *
 ****************************************************************************/

static void rk3576_vop_config_window(struct rk3576_vop_s *priv)
{
  const struct rk3576_display_timing_s *t = &priv->timing;
  uint32_t win = RK3576_VOP_ESMART(RK3576_VOP_FB_ESMART);
  uintptr_t fbphys;
  uint16_t hact_st;
  uint16_t vact_st;

  fbphys = up_addrenv_va_to_pa(priv->fbmem);

  hact_st = t->hsync + t->hbp;
  vact_st = t->vsync + t->vbp;

  /* No colour space conversion and no mirroring on an RGB framebuffer. */

  rk3576_vop_putreg(win + RK3576_VOP_ESMART_CTRL0, 0);

  /* Source: framebuffer base address and stride.  VIR counts 32-bit
   * words, which is why the stride is word aligned at allocation time.
   */

  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_YRGB_MST,
                    (uint32_t)fbphys);
  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_CBR_MST, 0);
  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_VIR,
                    VOP_ESMART_VIR_STRIDE(priv->stride / 4));

  /* Source rectangle, destination rectangle and destination position.
   * They are identical in size, so the scaler stays in bypass.
   */

  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_ACT_INFO,
                    VOP_WIN_SIZE_PACK(t->hact, t->vact));
  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_DSP_INFO,
                    VOP_WIN_SIZE_PACK(t->hact, t->vact));
  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_DSP_ST,
                    VOP_WIN_POS_PACK(hact_st, vact_st));
  rk3576_vop_putreg(win + RK3576_VOP_ESMART_REGION0_SCL_CTRL,
                    VOP_ESMART_SCL_CTRL_BYPASS);

  /* Enable the region with the framebuffer pixel format, fully opaque. */

  rk3576_vop_putreg(
      win + RK3576_VOP_ESMART_REGION0_CTRL,
      VOP_ESMART_REGION_CTRL_WIN_EN |
          VOP_ESMART_REGION_CTRL_FMT(RK3576_VOP_WIN_FMT) |
          VOP_ESMART_REGION_CTRL_GLOBAL_ALPHA(RK3576_VOP_ALPHA_OPAQUE));
}

/****************************************************************************
 * Name: rk3576_vop_config_output
 *
 * Description:
 *   Connect the framebuffer video port to its display interface and set
 *   the sync polarity the mode asks for.  Only HDMI TX0 is wired up so
 *   far; the other interfaces need their own encoder driver before the
 *   routing bits can be validated on hardware.
 *
 ****************************************************************************/

static void rk3576_vop_config_output(struct rk3576_vop_s *priv)
{
  uint32_t pol = 0;

  if (priv->timing.hsync_active_high)
    {
      pol |= VOP_DSP_IF_POL_HSYNC_HIGH;
    }

  if (priv->timing.vsync_active_high)
    {
      pol |= VOP_DSP_IF_POL_VSYNC_HIGH;
    }

  pol |= VOP_DSP_IF_POL_DEN_HIGH;

  /* TODO: MIPI/LVDS/eDP routing is rejected by rk3576_vop_set_interface()
   * until the corresponding encoder driver exists, so only the HDMI TX0
   * path is programmed here.
   */

  rk3576_vop_modifyreg(RK3576_VOP_SYS_DSP_IF_POL, VOP_DSP_IF_POL_HDMI0_MASK,
                       (pol << VOP_DSP_IF_POL_HDMI0_SHIFT) &
                           VOP_DSP_IF_POL_HDMI0_MASK);

  /* One pixel per dclk on the HDMI interface. */

  rk3576_vop_modifyreg(RK3576_VOP_SYS_DSP_IF_CTRL,
                       VOP_DSP_IF_CTRL_HDMI0_DCLK_DIV_MASK, 0);

  /* Enable HDMI TX0 and select the framebuffer video port as its source */

  rk3576_vop_modifyreg(RK3576_VOP_SYS_DSP_IF_EN, VOP_DSP_IF_EN_HDMI0_MUX_MASK,
                       VOP_DSP_IF_EN_HDMI0 |
                           VOP_DSP_IF_EN_HDMI0_MUX(RK3576_VOP_FB_VP));
}

/****************************************************************************
 * Name: rk3576_vop_interrupt
 *
 * Description:
 *   Framebuffer video port interrupt handler.  Only the frame-start event
 *   is enabled; it releases anybody blocked in FBIO_WAITFORVSYNC.
 *
 ****************************************************************************/

static int rk3576_vop_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_vop_s *priv = (struct rk3576_vop_s *)arg;
  uint32_t vp = RK3576_VOP_VP(RK3576_VOP_FB_VP);
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(priv);

  status = rk3576_vop_getreg(vp + RK3576_VOP_VP_INT_STATUS);
  if (status == 0)
    {
      return OK;
    }

  rk3576_vop_putreg(vp + RK3576_VOP_VP_INT_CLR, VOP_INT_HIWORD_CLR(status));

#ifdef CONFIG_FB_SYNC
  if ((status & VOP_VP_INT_FS) != 0)
    {
      int semcount = 0;

      /* Only ever hand out one credit: a late waiter must block until the
       * next frame instead of returning on a stale one.
       */

      nxsem_get_value(&priv->vsync, &semcount);
      if (semcount <= 0)
        {
          nxsem_post(&priv->vsync);
        }
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_getvideoinfo
 *
 * Description:
 *   FB upper-half method: report the geometry and pixel format of the
 *   display.
 *
 ****************************************************************************/

static int rk3576_vop_getvideoinfo(struct fb_vtable_s *vtable,
                                   struct fb_videoinfo_s *vinfo)
{
  struct rk3576_vop_s *priv = (struct rk3576_vop_s *)vtable;

  if (priv == NULL || vinfo == NULL || !priv->initialized)
    {
      return -EINVAL;
    }

  memset(vinfo, 0, sizeof(*vinfo));

  vinfo->fmt = RK3576_VOP_FB_FMT;
  vinfo->xres = priv->timing.hact;
  vinfo->yres = priv->timing.vact;
  vinfo->nplanes = 1;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_getplaneinfo
 *
 * Description:
 *   FB upper-half method: report the memory layout of the single colour
 *   plane.
 *
 ****************************************************************************/

static int rk3576_vop_getplaneinfo(struct fb_vtable_s *vtable, int planeno,
                                   struct fb_planeinfo_s *pinfo)
{
  struct rk3576_vop_s *priv = (struct rk3576_vop_s *)vtable;

  if (priv == NULL || pinfo == NULL || !priv->initialized || planeno != 0)
    {
      return -EINVAL;
    }

  memset(pinfo, 0, sizeof(*pinfo));

  pinfo->fbmem = priv->fbmem;
  pinfo->fblen = priv->fblen;
  pinfo->stride = priv->stride;
  pinfo->display = 0;
  pinfo->bpp = RK3576_VOP_FB_BPP;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_waitforvsync
 *
 * Description:
 *   FB upper-half method: block until the next frame starts.
 *
 ****************************************************************************/

#ifdef CONFIG_FB_SYNC
static int rk3576_vop_waitforvsync(struct fb_vtable_s *vtable)
{
  struct rk3576_vop_s *priv = (struct rk3576_vop_s *)vtable;

  if (priv == NULL || !priv->initialized)
    {
      return -EINVAL;
    }

  return nxsem_wait_uninterruptible(&priv->vsync);
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_timing_1080p60
 ****************************************************************************/

const struct rk3576_display_timing_s *rk3576_vop_timing_1080p60(void)
{
  return &g_rk3576_vop_1080p60;
}

/****************************************************************************
 * Name: rk3576_vop_timing_720p60
 ****************************************************************************/

const struct rk3576_display_timing_s *rk3576_vop_timing_720p60(void)
{
  return &g_rk3576_vop_720p60;
}

/****************************************************************************
 * Name: rk3576_vop_set_timing
 ****************************************************************************/

int rk3576_vop_set_timing(const struct rk3576_display_timing_s *timing)
{
  int ret;

  if (timing == NULL)
    {
      return -EINVAL;
    }

  if (g_rk3576_vop.initialized)
    {
      return -EBUSY;
    }

  ret = rk3576_vop_check_timing(timing);
  if (ret < 0)
    {
      lcderr("ERROR: rejected %ux%u timing\n", timing->hact, timing->vact);
      return ret;
    }

  g_rk3576_vop.timing = *timing;
  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_set_interface
 ****************************************************************************/

int rk3576_vop_set_interface(int iface)
{
  if (g_rk3576_vop.initialized)
    {
      return -EBUSY;
    }

  /* TODO: accept RK3576_VOP_IF_MIPI0/MIPI1/LVDS0/EDP0 once the matching
   * encoder drivers exist and their SYS_DSP_IF routing has been verified
   * on hardware.
   */

  if (iface != RK3576_VOP_IF_HDMI0)
    {
      return -EINVAL;
    }

  g_rk3576_vop.iface = iface;
  return OK;
}

/****************************************************************************
 * Name: rk3576_vop_get_timing
 ****************************************************************************/

const struct rk3576_display_timing_s *rk3576_vop_get_timing(void)
{
  if (g_rk3576_vop.timing.hact == 0)
    {
      return &g_rk3576_vop_1080p60;
    }

  return &g_rk3576_vop.timing;
}

/****************************************************************************
 * Name: rk3576_vop_get_fbmem
 ****************************************************************************/

uintptr_t rk3576_vop_get_fbmem(size_t *fbsize)
{
  if (fbsize != NULL)
    {
      *fbsize = g_rk3576_vop.fblen;
    }

  if (g_rk3576_vop.fbmem == NULL)
    {
      return 0;
    }

  return up_addrenv_va_to_pa(g_rk3576_vop.fbmem);
}

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Initialize the framebuffer video device for the specified display.
 *   Brings up the VOP power domain and clocks, allocates the framebuffer
 *   from the DMA heap, programs timing/overlay/window and starts the
 *   scan-out.
 *
 * Input Parameters:
 *   display - Display number; only display 0 exists on this SoC port.
 *
 * Returned Value:
 *   Zero is returned on success; a negated errno value is returned on any
 *   failure.
 *
 ****************************************************************************/

int up_fbinitialize(int display)
{
  struct rk3576_vop_s *priv = &g_rk3576_vop;
  uint32_t vp = RK3576_VOP_VP(RK3576_VOP_FB_VP);
  uint32_t version;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (priv->initialized)
    {
      return OK;
    }

  /* Fall back to the default mode when the board did not pick one. */

  if (priv->timing.hact == 0)
    {
      priv->timing = g_rk3576_vop_1080p60;
    }

  /* The VOP sits in its own power domain, which is off after reset. */

  ret = rk3576_pd_on(RK3576_PD_VOP);
  if (ret < 0)
    {
      lcderr("ERROR: failed to power up the VOP domain: %d\n", ret);
      return ret;
    }

  ret = rk3576_vop_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  version = rk3576_vop_getreg(RK3576_VOP_VERSION_INFO);
  lcdinfo("VOP version 0x%08" PRIx32 ", mode %ux%u@%" PRIu32 "Hz\n", version,
          priv->timing.hact, priv->timing.vact, priv->timing.pixclk);
  UNUSED(version);

  /* Allocate the framebuffer.  The stride is padded to a whole 32-bit
   * word because the window stride register counts words.
   */

  priv->stride = RK3576_VOP_ALIGN_UP(
      (uint32_t)priv->timing.hact * RK3576_VOP_FB_BPP / 8, 4);
  priv->fblen = (size_t)priv->stride * priv->timing.vact;

  priv->fbmem = rk3576_dma_alloc(priv->fblen);
  if (priv->fbmem == NULL)
    {
      lcderr("ERROR: failed to allocate a %zu byte framebuffer\n",
             priv->fblen);
      return -ENOMEM;
    }

  /* Start from a black screen and push it out of the D-cache: the VOP AXI
   * master is not coherent with the CPU caches.
   */

  memset(priv->fbmem, 0, priv->fblen);
  up_clean_dcache((uintptr_t)priv->fbmem,
                  (uintptr_t)priv->fbmem + priv->fblen);

#ifdef CONFIG_FB_SYNC
  nxsem_init(&priv->vsync, 0, 0);
#endif

  /* Take the display down while the timing registers are rewritten. */

  rk3576_vop_set_standby(true);

  rk3576_vop_iommu_bypass();

  /* Automatic clock gating of idle sub-blocks: pure power saving, safe to
   * leave on for a static framebuffer.
   */

  rk3576_vop_putreg(RK3576_VOP_SYS_AUTO_GATING, VOP_SYS_AUTO_GATING_EN);

  rk3576_vop_config_timing(priv);
  rk3576_vop_config_overlay();
  rk3576_vop_config_window(priv);
  rk3576_vop_config_output(priv);

  /* Interrupts: frame start only, cleared before being unmasked. */

  rk3576_vop_putreg(vp + RK3576_VOP_VP_INT_CLR, VOP_INT_HIWORD_CLR(0xffff));

  ret = irq_attach(RK3576_IRQ_VOP_VP0, rk3576_vop_interrupt, priv);
  if (ret < 0)
    {
      lcderr("ERROR: failed to attach the VP0 interrupt: %d\n", ret);
      goto err_free;
    }

  rk3576_vop_putreg(vp + RK3576_VOP_VP_INT_EN, VOP_INT_HIWORD(VOP_VP_INT_FS));
  up_enable_irq(RK3576_IRQ_VOP_VP0);

  /* Commit everything and start scanning out. */

  rk3576_vop_set_standby(false);

  priv->initialized = true;
  return OK;

err_free:
#ifdef CONFIG_FB_SYNC
  nxsem_destroy(&priv->vsync);
#endif
  rk3576_dma_free(priv->fbmem, priv->fblen);
  priv->fbmem = NULL;
  priv->fblen = 0;
  return ret;
}

/****************************************************************************
 * Name: up_fbgetvplane
 *
 * Description:
 *   Return a reference to the framebuffer object for the specified video
 *   plane of the specified display.
 *
 * Input Parameters:
 *   display - Display number; only display 0 exists.
 *   vplane  - Video plane number; only plane 0 exists.
 *
 * Returned Value:
 *   A non-NULL pointer to the frame buffer access structure is returned on
 *   success; NULL is returned on any failure.
 *
 ****************************************************************************/

struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || !g_rk3576_vop.initialized)
    {
      return NULL;
    }

  return &g_rk3576_vop.vtable;
}

/****************************************************************************
 * Name: up_fbuninitialize
 *
 * Description:
 *   Stop the scan-out, release the framebuffer and detach the interrupt.
 *
 * Input Parameters:
 *   display - Display number; only display 0 exists.
 *
 ****************************************************************************/

void up_fbuninitialize(int display)
{
  struct rk3576_vop_s *priv = &g_rk3576_vop;
  uint32_t vp = RK3576_VOP_VP(RK3576_VOP_FB_VP);
  uint32_t win = RK3576_VOP_ESMART(RK3576_VOP_FB_ESMART);

  if (display != 0 || !priv->initialized)
    {
      return;
    }

  /* Mask and detach the frame-start interrupt. */

  rk3576_vop_putreg(vp + RK3576_VOP_VP_INT_EN, VOP_INT_HIWORD_CLR(0xffff));
  up_disable_irq(RK3576_IRQ_VOP_VP0);
  irq_detach(RK3576_IRQ_VOP_VP0);

  /* Disable the window, stop the video port and drop the interface. */

  rk3576_vop_modifyreg(win + RK3576_VOP_ESMART_REGION0_CTRL,
                       VOP_ESMART_REGION_CTRL_WIN_EN, 0);
  rk3576_vop_modifyreg(RK3576_VOP_SYS_DSP_IF_EN, VOP_DSP_IF_EN_HDMI0, 0);
  rk3576_vop_set_standby(true);

#ifdef CONFIG_FB_SYNC
  nxsem_destroy(&priv->vsync);
#endif

  rk3576_dma_free(priv->fbmem, priv->fblen);
  priv->fbmem = NULL;
  priv->fblen = 0;
  priv->stride = 0;
  priv->initialized = false;
}

#endif /* CONFIG_RK3576_VOP */
