/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_rga.c
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
 * RK3576 RGA2 (Raster Graphic Acceleration) 2D engine driver.
 *
 * The RK3576 carries two identical RGA2 cores.  Each is programmed in
 * command-list mode: a 32-word descriptor is written into DMA-safe memory
 * obtained from rk3576_dma_alloc(), the D-cache is cleaned over it, its
 * physical address is published in RGA_CMD_BASE and the core is kicked by
 * RGA_CMD_CTRL.  Completion arrives as an interrupt which posts a
 * semaphore the submitting thread is blocked on.
 *
 * NuttX has no generic 2D-blitter subsystem to plug into, so the driver
 * offers a character device with private ioctls plus a set of direct
 * in-kernel entry points (see rk3576_rga.h).  Both are serialised per core
 * by the same mutex.
 *
 * The per-core IOMMU (iommu@27920f00 / iommu@27930f00) is left disabled:
 * all surfaces are addressed by physical address, which is why every
 * buffer handed to this driver must come from rk3576_dma_alloc() or be a
 * physically contiguous frame buffer below the 4GB line.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_rga.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_rga.h"

#ifdef CONFIG_RK3576_RGA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Longest time a single operation may take before it is declared dead.  A
 * full-screen 1080p blit needs well under a millisecond at 300 MHz, so half
 * a second is a very generous ceiling.
 */

#define RK3576_RGA_TIMEOUT_MS 500

/* Number of polling iterations while waiting for a soft reset to retire. */

#define RK3576_RGA_RESET_RETRIES 1000
#define RK3576_RGA_RESET_DELAY_US 1

/* Size of the DMA-resident command list, in bytes. */

#define RK3576_RGA_CMD_BYTES (RK3576_RGA_CMD_NWORDS * sizeof(uint32_t))

/* Fixed-point one used by the scaler factor computation. */

#define RK3576_RGA_FIXED_ONE 65536

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-core static description (address / interrupt / clock name suffix). */

struct rk3576_rga_desc_s
{
  uintptr_t base;     /* Register window base address           */
  int irq;            /* GIC interrupt number                   */
  const char *aclk;   /* AXI clock gate name                    */
  const char *hclk;   /* AHB clock gate name                    */
  const char *cclk;   /* Core functional clock gate name        */
  const char *devpath;/* Character device path                  */
};

/* Per-core run-time state. */

struct rk3576_rga_dev_s
{
  const struct rk3576_rga_desc_s *desc; /* Static description        */
  uint32_t *cmd;                        /* DMA-safe command list     */
  uintptr_t cmdpa;                      /* Physical address of cmd   */
  uint32_t coreclk;                     /* Core clock rate, in Hz    */
  mutex_t lock;                         /* Serialises submissions    */
  sem_t donesem;                        /* Posted by the ISR         */
  uint32_t intstatus;                   /* Raw INT bits latched here */
  bool initialized;                     /* Set once bring-up is done */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_rga_getreg(struct rk3576_rga_dev_s *priv,
                                  unsigned int offset);
static void rk3576_rga_putreg(struct rk3576_rga_dev_s *priv,
                              unsigned int offset, uint32_t value);

static int rk3576_rga_clk_init(struct rk3576_rga_dev_s *priv);
static int rk3576_rga_reset(struct rk3576_rga_dev_s *priv);
static int rk3576_rga_interrupt(int irq, void *context, void *arg);

static unsigned int rk3576_rga_bpp(uint8_t format);
static bool rk3576_rga_is_yuv(uint8_t format);
static bool rk3576_rga_is_420(uint8_t format);
static bool rk3576_rga_surface_valid(const struct rk3576_rga_surface_s *s);
static uint32_t rk3576_rga_plane_offset(
    const struct rk3576_rga_surface_s *s);
static uint32_t rk3576_rga_chroma_offset(
    const struct rk3576_rga_surface_s *s);
static uint32_t rk3576_rga_scale_factor(uint32_t src, uint32_t dst,
                                        uint32_t *mode);
static uint8_t rk3576_rga_pick_csc(const struct rk3576_rga_surface_s *src,
                                   const struct rk3576_rga_surface_s *dst,
                                   int override);

static void rk3576_rga_build_src(struct rk3576_rga_dev_s *priv,
                                 const struct rk3576_rga_op_s *op);
static void rk3576_rga_build_dst(struct rk3576_rga_dev_s *priv,
                                 const struct rk3576_rga_op_s *op);
static void rk3576_rga_build_alpha(struct rk3576_rga_dev_s *priv,
                                   const struct rk3576_rga_op_s *op);
static int rk3576_rga_submit(struct rk3576_rga_dev_s *priv);
static int rk3576_rga_run(struct rk3576_rga_dev_s *priv,
                          const struct rk3576_rga_op_s *op,
                          uint32_t fillcolor, bool fill);

static struct rk3576_rga_dev_s *rk3576_rga_default_dev(void);

static int rk3576_rga_fops_open(struct file *filep);
static int rk3576_rga_fops_close(struct file *filep);
static int rk3576_rga_fops_ioctl(struct file *filep, int cmd,
                                 unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rk3576_rga_desc_s g_rga_desc[RK3576_RGA_NCORES] =
{
  {
    .base    = RK3576_RGA0_ADDR,
    .irq     = RK3576_IRQ_RGA0,
    .aclk    = "aclk_rga2_0_en",
    .hclk    = "hclk_rga2_0_en",
    .cclk    = "clk_rga2_0_en",
    .devpath = "/dev/rga0",
  },
  {
    .base    = RK3576_RGA1_ADDR,
    .irq     = RK3576_IRQ_RGA1,
    .aclk    = "aclk_rga2_1_en",
    .hclk    = "hclk_rga2_1_en",
    .cclk    = "clk_rga2_1_en",
    .devpath = "/dev/rga1",
  },
};

static struct rk3576_rga_dev_s g_rga_dev[RK3576_RGA_NCORES];

static const struct file_operations g_rk3576_rga_fops =
{
  .open  = rk3576_rga_fops_open,
  .close = rk3576_rga_fops_close,
  .ioctl = rk3576_rga_fops_ioctl,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rga_getreg / rk3576_rga_putreg
 *
 * Description:
 *   Access one of the core's global (non command-list) registers.
 *
 ****************************************************************************/

static uint32_t rk3576_rga_getreg(struct rk3576_rga_dev_s *priv,
                                  unsigned int offset)
{
  return getreg32(priv->desc->base + offset);
}

static void rk3576_rga_putreg(struct rk3576_rga_dev_s *priv,
                              unsigned int offset, uint32_t value)
{
  putreg32(value, priv->desc->base + offset);
}

/****************************************************************************
 * Name: rk3576_rga_clk_init
 *
 * Description:
 *   Enable every clock the core needs.  All clock handling for this driver
 *   lives in this one function, so a change to the CLK API only has to be
 *   applied here.
 *
 * Input Parameters:
 *   priv - Core state; priv->coreclk is filled in on success.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_rga_clk_init(struct rk3576_rga_dev_s *priv)
{
  const struct rk3576_rga_desc_s *desc = priv->desc;
  struct clk_s *aclk;
  struct clk_s *hclk;
  struct clk_s *cclk;
  int ret;

  /* AXI bus clock */

  aclk = clk_get(desc->aclk);
  if (aclk == NULL)
    {
      gerr("ERROR: failed to get %s\n", desc->aclk);
      return -ENODEV;
    }

  ret = clk_enable(aclk);
  if (ret < 0)
    {
      gerr("ERROR: failed to enable %s: %d\n", desc->aclk, ret);
      return ret;
    }

  /* AHB register-interface clock */

  hclk = clk_get(desc->hclk);
  if (hclk == NULL)
    {
      gerr("ERROR: failed to get %s\n", desc->hclk);
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      gerr("ERROR: failed to enable %s: %d\n", desc->hclk, ret);
      return ret;
    }

  /* Core (pixel pipeline) clock */

  cclk = clk_get(desc->cclk);
  if (cclk == NULL)
    {
      gerr("ERROR: failed to get %s\n", desc->cclk);
      return -ENODEV;
    }

  ret = clk_enable(cclk);
  if (ret < 0)
    {
      gerr("ERROR: failed to enable %s: %d\n", desc->cclk, ret);
      return ret;
    }

  /* Record the real rate rather than assuming a constant. */

  priv->coreclk = clk_get_rate(cclk);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rga_reset
 *
 * Description:
 *   Issue a soft reset and wait for the core to report itself idle.
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if the reset never retires.
 *
 ****************************************************************************/

static int rk3576_rga_reset(struct rk3576_rga_dev_s *priv)
{
  unsigned int i;

  rk3576_rga_putreg(priv, RK3576_RGA_SYS_CTRL_OFFSET,
                    RK3576_RGA_SYS_CTRL_SOFT_RESET |
                    RK3576_RGA_SYS_CTRL_CCLK_SRESET |
                    RK3576_RGA_SYS_CTRL_ACLK_SRESET);

  for (i = 0; i < RK3576_RGA_RESET_RETRIES; i++)
    {
      if ((rk3576_rga_getreg(priv, RK3576_RGA_SYS_CTRL_OFFSET) &
           RK3576_RGA_SYS_CTRL_RST_BUSY) == 0)
        {
          rk3576_rga_putreg(priv, RK3576_RGA_SYS_CTRL_OFFSET, 0);
          return OK;
        }

      up_udelay(RK3576_RGA_RESET_DELAY_US);
    }

  gerr("ERROR: RGA soft reset timed out\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_rga_interrupt
 *
 * Description:
 *   Command-list completion / error interrupt handler.  Latches the raw
 *   status for the submitter and releases it.
 *
 ****************************************************************************/

static int rk3576_rga_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_rga_dev_s *priv = (struct rk3576_rga_dev_s *)arg;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_rga_getreg(priv, RK3576_RGA_INT_OFFSET);

  /* Acknowledge everything that is pending. */

  rk3576_rga_putreg(priv, RK3576_RGA_INT_OFFSET,
                    RK3576_RGA_INT_CLR_ALL | RK3576_RGA_INT_EN_ALL);

  if ((status & RK3576_RGA_INT_RAW_ALL) != 0)
    {
      priv->intstatus = status;
      nxsem_post(&priv->donesem);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rga_bpp
 *
 * Description:
 *   Bits per pixel of the first (Y or packed RGB) plane of a format.  For
 *   planar and semi-planar YUV this is the 8-bit luma plane.
 *
 ****************************************************************************/

static unsigned int rk3576_rga_bpp(uint8_t format)
{
  switch (format)
    {
      case RK3576_RGA_FMT_RGBA8888:
      case RK3576_RGA_FMT_RGBX8888:
      case RK3576_RGA_FMT_BGRA8888:
        return 32;

      case RK3576_RGA_FMT_RGB888:
      case RK3576_RGA_FMT_BGR888:
        return 24;

      case RK3576_RGA_FMT_RGB565:
      case RK3576_RGA_FMT_RGBA5551:
      case RK3576_RGA_FMT_RGBA4444:
      case RK3576_RGA_FMT_YUYV422:
      case RK3576_RGA_FMT_UYVY422:
        return 16;

      case RK3576_RGA_FMT_YUV422SP:
      case RK3576_RGA_FMT_YUV422P:
      case RK3576_RGA_FMT_YUV420SP:
      case RK3576_RGA_FMT_YUV420P:
      case RK3576_RGA_FMT_YUYV420:
      case RK3576_RGA_FMT_UYVY420:
        return 8;

      default:
        return 0;
    }
}

/****************************************************************************
 * Name: rk3576_rga_is_yuv
 *
 * Description:
 *   True when the format code denotes a YUV (rather than RGB) format.
 *
 ****************************************************************************/

static bool rk3576_rga_is_yuv(uint8_t format)
{
  return format >= RK3576_RGA_FMT_YUV422SP;
}

/****************************************************************************
 * Name: rk3576_rga_is_420
 *
 * Description:
 *   True when the format is vertically chroma-subsampled, which halves the
 *   Y offset that has to be applied to the chroma planes.
 *
 ****************************************************************************/

static bool rk3576_rga_is_420(uint8_t format)
{
  return format == RK3576_RGA_FMT_YUV420SP ||
         format == RK3576_RGA_FMT_YUV420P  ||
         format == RK3576_RGA_FMT_YUYV420  ||
         format == RK3576_RGA_FMT_UYVY420;
}

/****************************************************************************
 * Name: rk3576_rga_surface_valid
 *
 * Description:
 *   Sanity-check a surface descriptor supplied by a caller (possibly from
 *   user space through an ioctl).
 *
 ****************************************************************************/

static bool rk3576_rga_surface_valid(const struct rk3576_rga_surface_s *s)
{
  unsigned int bpp = rk3576_rga_bpp(s->format);

  if (bpp == 0)
    {
      gerr("ERROR: unsupported RGA pixel format %u\n", s->format);
      return false;
    }

  if (s->yrgb == 0 || (s->yrgb & 0x3) != 0)
    {
      gerr("ERROR: RGA plane address %08" PRIx32 " unusable\n", s->yrgb);
      return false;
    }

  if (s->width == 0 || s->height == 0 ||
      s->width > RK3576_RGA_MAX_WIDTH || s->height > RK3576_RGA_MAX_HEIGHT)
    {
      gerr("ERROR: RGA rectangle %ux%u out of range\n",
           s->width, s->height);
      return false;
    }

  /* The virtual stride must be a whole number of 32-bit words and must
   * cover the active rectangle plus its X origin.
   */

  if ((s->stride & 0x3) != 0 ||
      s->stride * 8 < ((uint32_t)s->xoffset + s->width) * bpp)
    {
      gerr("ERROR: RGA stride %" PRIu32 " too small for %u+%u pixels\n",
           s->stride, s->xoffset, s->width);
      return false;
    }

  if (rk3576_rga_is_yuv(s->format) && s->format != RK3576_RGA_FMT_YUYV422 &&
      s->format != RK3576_RGA_FMT_UYVY422 && s->cb == 0)
    {
      gerr("ERROR: planar YUV surface without a chroma plane\n");
      return false;
    }

  return true;
}

/****************************************************************************
 * Name: rk3576_rga_plane_offset
 *
 * Description:
 *   Byte offset of the active rectangle inside the Y / RGB plane.  RGA2
 *   has no separate X/Y origin registers, so the origin is folded into the
 *   plane base address.
 *
 ****************************************************************************/

static uint32_t rk3576_rga_plane_offset(const struct rk3576_rga_surface_s *s)
{
  unsigned int bpp = rk3576_rga_bpp(s->format);

  return (uint32_t)s->yoffset * s->stride +
         ((uint32_t)s->xoffset * bpp) / 8;
}

/****************************************************************************
 * Name: rk3576_rga_chroma_offset
 *
 * Description:
 *   Byte offset of the active rectangle inside a chroma plane.  Both
 *   semi-planar (one byte pair per chroma sample) and fully planar (one
 *   byte per chroma sample) layouts start their line at the same byte
 *   position, so only the vertical subsampling has to be accounted for.
 *
 ****************************************************************************/

static uint32_t rk3576_rga_chroma_offset(
    const struct rk3576_rga_surface_s *s)
{
  uint32_t yoff = s->yoffset;

  if (rk3576_rga_is_420(s->format))
    {
      yoff /= 2;
    }

  return yoff * s->stride + s->xoffset;
}

/****************************************************************************
 * Name: rk3576_rga_scale_factor
 *
 * Description:
 *   Compute the RGA2 resampling factor for one axis and report which
 *   scaling mode the pipeline has to select.
 *
 *   Up-scaling uses a (src-1)/(dst-1) phase step; down-scaling uses the
 *   dst/src ratio with the averaging filter.
 *
 * Input Parameters:
 *   src  - Source extent in pixels
 *   dst  - Destination extent in pixels
 *   mode - Receives RK3576_RGA_SCL_OFF / _UP / _DOWN_AVG
 *
 * Returned Value:
 *   The 16-bit factor to program, or 0 when no scaling is required.
 *
 ****************************************************************************/

static uint32_t rk3576_rga_scale_factor(uint32_t src, uint32_t dst,
                                        uint32_t *mode)
{
  uint32_t factor;

  if (src == dst)
    {
      *mode = RK3576_RGA_SCL_OFF;
      return 0;
    }

  if (src < dst)
    {
      /* Up-scale: step through the source by less than one pixel. */

      *mode = RK3576_RGA_SCL_UP;
      if (dst < 2)
        {
          return 0;
        }

      factor = ((src - 1) * RK3576_RGA_FIXED_ONE) / (dst - 1);
    }
  else
    {
      /* Down-scale: the factor is the (fractional) output/input ratio. */

      *mode = RK3576_RGA_SCL_DOWN_AVG;
      factor = (dst * RK3576_RGA_FIXED_ONE) / src;
    }

  if (factor >= RK3576_RGA_FIXED_ONE)
    {
      factor = RK3576_RGA_FIXED_ONE - 1;
    }

  return factor;
}

/****************************************************************************
 * Name: rk3576_rga_pick_csc
 *
 * Description:
 *   Resolve the colour-space conversion matrix to program.  An explicit
 *   request always wins; otherwise a matrix is only needed when exactly one
 *   side of the transfer is YUV, in which case BT.601 limited range (the
 *   RGA2 reset default and the common case for camera and display data) is
 *   selected.
 *
 ****************************************************************************/

static uint8_t rk3576_rga_pick_csc(const struct rk3576_rga_surface_s *src,
                                   const struct rk3576_rga_surface_s *dst,
                                   int override)
{
  if (override != RK3576_RGA_CSC_AUTO)
    {
      return (uint8_t)override;
    }

  if (rk3576_rga_is_yuv(src->format) != rk3576_rga_is_yuv(dst->format))
    {
      return RK3576_RGA_CSC_BT601_LIMIT;
    }

  return RK3576_RGA_CSC_BYPASS;
}

/****************************************************************************
 * Name: rk3576_rga_build_src
 *
 * Description:
 *   Fill the source half of the command list: format/transform word, the
 *   three plane addresses, the virtual and active geometry and the two
 *   scaling factors.
 *
 ****************************************************************************/

static void rk3576_rga_build_src(struct rk3576_rga_dev_s *priv,
                                 const struct rk3576_rga_op_s *op)
{
  const struct rk3576_rga_surface_s *src = &op->src;
  const struct rk3576_rga_surface_s *dst = &op->dst;
  uint32_t *cmd = priv->cmd;
  uint32_t rot = op->flags & RK3576_RGA_ROTATE_MASK;
  uint32_t dstw;
  uint32_t dsth;
  uint32_t hmode;
  uint32_t vmode;
  uint32_t hfactor;
  uint32_t vfactor;
  uint32_t info;

  /* A 90 or 270 degree rotation transposes the destination rectangle, so
   * the scaler must compare the source width against the destination
   * height and vice versa.
   */

  if (rot == RK3576_RGA_ROTATE_90 || rot == RK3576_RGA_ROTATE_270)
    {
      dstw = dst->height;
      dsth = dst->width;
    }
  else
    {
      dstw = dst->width;
      dsth = dst->height;
    }

  hfactor = rk3576_rga_scale_factor(src->width, dstw, &hmode);
  vfactor = rk3576_rga_scale_factor(src->height, dsth, &vmode);

  info = ((uint32_t)src->format << RK3576_RGA_SRC_FMT_SHIFT) |
         ((uint32_t)rk3576_rga_pick_csc(src, dst, src->csc_mode)
          << RK3576_RGA_SRC_CSC_SHIFT) |
         (rot << RK3576_RGA_SRC_ROT_SHIFT) |
         (hmode << RK3576_RGA_SRC_HSCL_SHIFT) |
         (vmode << RK3576_RGA_SRC_VSCL_SHIFT);

  if (src->rb_swap)
    {
      info |= RK3576_RGA_SRC_RB_SWAP;
    }

  if (src->uv_swap)
    {
      info |= RK3576_RGA_SRC_UV_SWAP;
    }

  if ((op->flags & RK3576_RGA_MIRROR_X) != 0)
    {
      info |= RK3576_RGA_SRC_MIRROR_X;
    }

  if ((op->flags & RK3576_RGA_MIRROR_Y) != 0)
    {
      info |= RK3576_RGA_SRC_MIRROR_Y;
    }

  cmd[RK3576_RGA_CMD_SRC_INFO] = info;

  cmd[RK3576_RGA_CMD_SRC_Y_RGB_BASE] =
      src->yrgb + rk3576_rga_plane_offset(src);
  cmd[RK3576_RGA_CMD_SRC_CB_BASE] =
      src->cb != 0 ? src->cb + rk3576_rga_chroma_offset(src) : 0;
  cmd[RK3576_RGA_CMD_SRC_CR_BASE] =
      src->cr != 0 ? src->cr + rk3576_rga_chroma_offset(src) : 0;

  /* The virtual stride is expressed in 32-bit words. */

  cmd[RK3576_RGA_CMD_SRC_VIR_INFO] =
      ((src->stride / 4) << RK3576_RGA_SRC_VIR_STRIDE_SHIFT) &
      RK3576_RGA_SRC_VIR_STRIDE_MASK;

  cmd[RK3576_RGA_CMD_SRC_ACT_INFO] =
      (((uint32_t)src->width - 1) << RK3576_RGA_ACT_WIDTH_SHIFT) |
      (((uint32_t)src->height - 1) << RK3576_RGA_ACT_HEIGHT_SHIFT);

  cmd[RK3576_RGA_CMD_SRC_X_FACTOR] = hfactor & RK3576_RGA_FACTOR_MASK;
  cmd[RK3576_RGA_CMD_SRC_Y_FACTOR] = vfactor & RK3576_RGA_FACTOR_MASK;
}

/****************************************************************************
 * Name: rk3576_rga_build_dst
 *
 * Description:
 *   Fill the destination half of the command list.
 *
 ****************************************************************************/

static void rk3576_rga_build_dst(struct rk3576_rga_dev_s *priv,
                                 const struct rk3576_rga_op_s *op)
{
  const struct rk3576_rga_surface_s *dst = &op->dst;
  uint32_t *cmd = priv->cmd;
  uint32_t info;

  info = ((uint32_t)dst->format << RK3576_RGA_DST_FMT_SHIFT) |
         ((uint32_t)rk3576_rga_pick_csc(&op->src, dst, dst->csc_mode)
          << RK3576_RGA_DST_CSC_SHIFT);

  if (dst->rb_swap)
    {
      info |= RK3576_RGA_DST_RB_SWAP;
    }

  if (dst->uv_swap)
    {
      info |= RK3576_RGA_DST_UV_SWAP;
    }

  cmd[RK3576_RGA_CMD_DST_INFO] = info;

  cmd[RK3576_RGA_CMD_DST_Y_RGB_BASE] =
      dst->yrgb + rk3576_rga_plane_offset(dst);
  cmd[RK3576_RGA_CMD_DST_CB_BASE] =
      dst->cb != 0 ? dst->cb + rk3576_rga_chroma_offset(dst) : 0;
  cmd[RK3576_RGA_CMD_DST_CR_BASE] =
      dst->cr != 0 ? dst->cr + rk3576_rga_chroma_offset(dst) : 0;

  cmd[RK3576_RGA_CMD_DST_VIR_INFO] =
      ((dst->stride / 4) << RK3576_RGA_DST_VIR_STRIDE_SHIFT) &
      RK3576_RGA_DST_VIR_STRIDE_MASK;

  cmd[RK3576_RGA_CMD_DST_ACT_INFO] =
      (((uint32_t)dst->width - 1) << RK3576_RGA_ACT_WIDTH_SHIFT) |
      (((uint32_t)dst->height - 1) << RK3576_RGA_ACT_HEIGHT_SHIFT);

  /* The second source operand of a two-operand BitBLT is the destination
   * surface itself; that is how alpha blending and ROP read the existing
   * pixels back.
   */

  cmd[RK3576_RGA_CMD_SRC1_RGB_BASE] = cmd[RK3576_RGA_CMD_DST_Y_RGB_BASE];
  cmd[RK3576_RGA_CMD_SRC_VIR_INFO] |=
      ((dst->stride / 4) << RK3576_RGA_SRC1_VIR_STRIDE_SHIFT) &
      RK3576_RGA_SRC1_VIR_STRIDE_MASK;
}

/****************************************************************************
 * Name: rk3576_rga_build_alpha
 *
 * Description:
 *   Program the alpha / ROP block according to the operation flags.  When
 *   neither blending nor ROP is requested the block is disabled, which is
 *   the plain "overwrite the destination" behaviour.
 *
 ****************************************************************************/

static void rk3576_rga_build_alpha(struct rk3576_rga_dev_s *priv,
                                   const struct rk3576_rga_op_s *op)
{
  uint32_t *cmd = priv->cmd;
  uint32_t ctrl0 = 0;
  uint32_t ctrl1 = 0;

  if ((op->flags & RK3576_RGA_ROP_ENABLE) != 0)
    {
      ctrl0 = RK3576_RGA_ALPHA_ROP_EN | RK3576_RGA_ALPHA_ROP_SEL |
              (RK3576_RGA_ROP_MODE_2 << RK3576_RGA_ALPHA_ROP_MODE_SHIFT);

      cmd[RK3576_RGA_CMD_ROP_CTRL0] =
          ((uint32_t)op->rop << RK3576_RGA_ROP_CODE_SHIFT) &
          RK3576_RGA_ROP_CODE_MASK;
    }
  else if ((op->flags &
            (RK3576_RGA_BLEND_SRCOVER | RK3576_RGA_BLEND_PREMUL)) != 0)
    {
      ctrl0 = RK3576_RGA_ALPHA_ROP_EN;
      ctrl1 = (op->flags & RK3576_RGA_BLEND_PREMUL) != 0 ?
              RK3576_RGA_BLEND_SRC_OVER_PREMUL : RK3576_RGA_BLEND_SRC_OVER;
    }

  if ((op->flags & RK3576_RGA_GLOBAL_ALPHA) != 0)
    {
      ctrl0 |= RK3576_RGA_ALPHA_ROP_EN |
               (((uint32_t)op->galpha << RK3576_RGA_ALPHA_GLOBAL_SHIFT) &
                RK3576_RGA_ALPHA_GLOBAL_MASK);
    }

  cmd[RK3576_RGA_CMD_ALPHA_CTRL0] = ctrl0;
  cmd[RK3576_RGA_CMD_ALPHA_CTRL1] = ctrl1;
}

/****************************************************************************
 * Name: rk3576_rga_submit
 *
 * Description:
 *   Publish the command list to the hardware and block until the core
 *   reports completion.  The caller must already hold priv->lock and must
 *   have populated priv->cmd.
 *
 * Returned Value:
 *   OK on success, -EIO on a hardware-reported error, -ETIMEDOUT if the
 *   core never raised an interrupt.
 *
 ****************************************************************************/

static int rk3576_rga_submit(struct rk3576_rga_dev_s *priv)
{
  int ret;

  /* Make the descriptor visible to the (non coherent) command fetcher. */

  up_clean_dcache((uintptr_t)priv->cmd,
                  (uintptr_t)priv->cmd + RK3576_RGA_CMD_BYTES);

  priv->intstatus = 0;

  /* Drain a stale post from a previous, timed-out submission. */

  while (nxsem_trywait(&priv->donesem) == OK)
    {
    }

  /* Take the core out of register mode, point it at the list and clear any
   * latent interrupt before enabling the completion sources.
   */

  rk3576_rga_putreg(priv, RK3576_RGA_SYS_CTRL_OFFSET, 0);
  rk3576_rga_putreg(priv, RK3576_RGA_CMD_BASE_OFFSET,
                    (uint32_t)priv->cmdpa);
  rk3576_rga_putreg(priv, RK3576_RGA_SYS_CTRL_OFFSET,
                    RK3576_RGA_SYS_CTRL_CMD_MODE |
                    RK3576_RGA_SYS_CTRL_AUTO_CKG);
  rk3576_rga_putreg(priv, RK3576_RGA_INT_OFFSET,
                    RK3576_RGA_INT_CLR_ALL | RK3576_RGA_INT_EN_ALL);

  /* One command in the list, then go. */

  rk3576_rga_putreg(priv, RK3576_RGA_CMD_CTRL_OFFSET,
                    (1 << RK3576_RGA_CMD_CTRL_NR_SHIFT) |
                    RK3576_RGA_CMD_CTRL_START);

  ret = nxsem_tickwait_uninterruptible(&priv->donesem,
                                       MSEC2TICK(RK3576_RGA_TIMEOUT_MS));
  if (ret < 0)
    {
      gerr("ERROR: RGA operation timed out, status=%08" PRIx32 "\n",
           rk3576_rga_getreg(priv, RK3576_RGA_STATUS_OFFSET));
      rk3576_rga_reset(priv);
      return -ETIMEDOUT;
    }

  if ((priv->intstatus &
       (RK3576_RGA_INT_RAW_ERROR | RK3576_RGA_INT_RAW_MMU_ERROR)) != 0)
    {
      gerr("ERROR: RGA reported int=%08" PRIx32 "\n", priv->intstatus);
      rk3576_rga_reset(priv);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rga_run
 *
 * Description:
 *   Validate, build and execute one operation.  This is the single funnel
 *   used by every public entry point and by the ioctl handler.
 *
 * Input Parameters:
 *   priv      - Core to run on
 *   op        - Operation descriptor; op->src is ignored when fill is true
 *   fillcolor - ARGB8888 colour used when fill is true
 *   fill      - true to run a colour fill, false to run a BitBLT
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_rga_run(struct rk3576_rga_dev_s *priv,
                          const struct rk3576_rga_op_s *op,
                          uint32_t fillcolor, bool fill)
{
  uint32_t mode;
  int ret;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  if (!rk3576_rga_surface_valid(&op->dst))
    {
      return -EINVAL;
    }

  if (!fill && !rk3576_rga_surface_valid(&op->src))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(priv->cmd, 0, RK3576_RGA_CMD_BYTES);

  if (fill)
    {
      mode = RK3576_RGA_MODE_RENDER_FILL;
      priv->cmd[RK3576_RGA_CMD_SRC_FG_COLOR] = fillcolor;
    }
  else
    {
      mode = RK3576_RGA_MODE_RENDER_BITBLT;
      rk3576_rga_build_src(priv, op);

      /* Blending and ROP both need the destination as a second operand. */

      if ((op->flags & (RK3576_RGA_BLEND_SRCOVER |
                        RK3576_RGA_BLEND_PREMUL |
                        RK3576_RGA_ROP_ENABLE)) != 0)
        {
          mode |= RK3576_RGA_MODE_BITBLT_2SRC <<
                  RK3576_RGA_MODE_BITBLT_SHIFT;
        }
    }

  rk3576_rga_build_dst(priv, op);
  rk3576_rga_build_alpha(priv, op);

  priv->cmd[RK3576_RGA_CMD_MODE_CTRL] = mode;

  /* IOMMU bypass: all addresses in the list are physical. */

  priv->cmd[RK3576_RGA_CMD_MMU_CTRL1] = 0;

  ret = rk3576_rga_submit(priv);

  /* The destination was written behind the D-cache's back. */

  if (ret == OK)
    {
      up_invalidate_dcache((uintptr_t)op->dst.yrgb,
                           (uintptr_t)op->dst.yrgb +
                           (uintptr_t)op->dst.stride *
                           (op->dst.yoffset + op->dst.height));
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_rga_default_dev
 *
 * Description:
 *   Return the first initialized core.  The two cores are functionally
 *   identical, so the direct-call API simply uses whichever is available.
 *
 ****************************************************************************/

static struct rk3576_rga_dev_s *rk3576_rga_default_dev(void)
{
  int i;

  for (i = 0; i < RK3576_RGA_NCORES; i++)
    {
      if (g_rga_dev[i].initialized)
        {
          return &g_rga_dev[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: rk3576_rga_fops_open / rk3576_rga_fops_close
 *
 * Description:
 *   The character device carries no per-open state; the handlers exist so
 *   that open()/close() succeed.
 *
 ****************************************************************************/

static int rk3576_rga_fops_open(struct file *filep)
{
  UNUSED(filep);
  return OK;
}

static int rk3576_rga_fops_close(struct file *filep)
{
  UNUSED(filep);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rga_fops_ioctl
 *
 * Description:
 *   Character-device entry point for the RK3576_RGAIOC_* commands.
 *
 ****************************************************************************/

static int rk3576_rga_fops_ioctl(struct file *filep, int cmd,
                                 unsigned long arg)
{
  struct rk3576_rga_dev_s *priv = filep->f_inode->i_private;
  struct rk3576_rga_op_s *op = (struct rk3576_rga_op_s *)arg;
  struct rk3576_rga_fill_s *fillop = (struct rk3576_rga_fill_s *)arg;
  struct rk3576_rga_op_s local;
  int ret;

  DEBUGASSERT(priv != NULL);

  switch (cmd)
    {
      case RK3576_RGAIOC_BLIT:
      case RK3576_RGAIOC_SCALE:
      case RK3576_RGAIOC_ROTATE:
      case RK3576_RGAIOC_CSC:
        if (op == NULL)
          {
            return -EINVAL;
          }

        if ((op->flags & ~RK3576_RGA_FLAGS_ALL) != 0)
          {
            return -EINVAL;
          }

        ret = rk3576_rga_run(priv, op, 0, false);
        break;

      case RK3576_RGAIOC_FILL:
        if (fillop == NULL)
          {
            return -EINVAL;
          }

        memset(&local, 0, sizeof(local));
        local.dst = fillop->dst;
        ret = rk3576_rga_run(priv, &local, fillop->color, true);
        break;

      case RK3576_RGAIOC_SYNC:

        /* Taking and releasing the submission mutex is enough: it is only
         * held while an operation is in flight.
         */

        ret = nxmutex_lock(&priv->lock);
        if (ret >= 0)
          {
            nxmutex_unlock(&priv->lock);
          }
        break;

      case RK3576_RGAIOC_VERSION:
        if (arg == 0)
          {
            return -EINVAL;
          }

        *(uint32_t *)arg =
            rk3576_rga_getreg(priv, RK3576_RGA_VERSION_OFFSET);
        ret = OK;
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rga_initialize
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_initialize(int core)
{
  struct rk3576_rga_dev_s *priv;
  int ret;

  if (core < 0 || core >= RK3576_RGA_NCORES)
    {
      return -EINVAL;
    }

  priv = &g_rga_dev[core];
  if (priv->initialized)
    {
      return OK;
    }

  priv->desc = &g_rga_desc[core];

  ret = rk3576_rga_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* The command list is fetched by the core itself, so it must live in
   * physically contiguous memory below 4GB.
   */

  priv->cmd = rk3576_dma_alloc(RK3576_RGA_CMD_BYTES);
  if (priv->cmd == NULL)
    {
      gerr("ERROR: out of DMA memory for the RGA command list\n");
      return -ENOMEM;
    }

  priv->cmdpa = up_addrenv_va_to_pa(priv->cmd);
  DEBUGASSERT((priv->cmdpa & (RK3576_RGA_CMD_ALIGN - 1)) == 0);
  memset(priv->cmd, 0, RK3576_RGA_CMD_BYTES);

  nxmutex_init(&priv->lock);
  nxsem_init(&priv->donesem, 0, 0);

  ret = rk3576_rga_reset(priv);
  if (ret < 0)
    {
      goto err_free;
    }

  /* Keep the internal IOMMU switched off: surfaces are addressed
   * physically.
   */

  rk3576_rga_putreg(priv, RK3576_RGA_MMU_CTRL0_OFFSET, 0);

  ret = irq_attach(priv->desc->irq, rk3576_rga_interrupt, priv);
  if (ret < 0)
    {
      gerr("ERROR: irq_attach(%d) failed: %d\n", priv->desc->irq, ret);
      goto err_free;
    }

  up_enable_irq(priv->desc->irq);

  ret = register_driver(priv->desc->devpath, &g_rk3576_rga_fops, 0666,
                        priv);
  if (ret < 0)
    {
      gerr("ERROR: register_driver(%s) failed: %d\n",
           priv->desc->devpath, ret);
      up_disable_irq(priv->desc->irq);
      irq_detach(priv->desc->irq);
      goto err_free;
    }

  priv->initialized = true;

  ginfo("RGA core%d at %08lx: version %08" PRIx32 ", core clock %" PRIu32
        " Hz\n", core, (unsigned long)priv->desc->base,
        rk3576_rga_getreg(priv, RK3576_RGA_VERSION_OFFSET), priv->coreclk);

  return OK;

err_free:
  nxsem_destroy(&priv->donesem);
  nxmutex_destroy(&priv->lock);
  rk3576_dma_free(priv->cmd, RK3576_RGA_CMD_BYTES);
  priv->cmd = NULL;
  return ret;
}

/****************************************************************************
 * Name: rk3576_rga_blit
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_blit(const struct rk3576_rga_surface_s *src,
                    const struct rk3576_rga_surface_s *dst,
                    uint32_t flags)
{
  struct rk3576_rga_dev_s *priv = rk3576_rga_default_dev();
  struct rk3576_rga_op_s op;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (src == NULL || dst == NULL || (flags & ~RK3576_RGA_FLAGS_ALL) != 0)
    {
      return -EINVAL;
    }

  memset(&op, 0, sizeof(op));
  op.src = *src;
  op.dst = *dst;
  op.flags = flags;
  op.galpha = UINT8_MAX;

  return rk3576_rga_run(priv, &op, 0, false);
}

/****************************************************************************
 * Name: rk3576_rga_fill
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_fill(const struct rk3576_rga_surface_s *dst, uint32_t color)
{
  struct rk3576_rga_dev_s *priv = rk3576_rga_default_dev();
  struct rk3576_rga_op_s op;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (dst == NULL)
    {
      return -EINVAL;
    }

  memset(&op, 0, sizeof(op));
  op.dst = *dst;

  return rk3576_rga_run(priv, &op, color, true);
}

/****************************************************************************
 * Name: rk3576_rga_scale
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_scale(const struct rk3576_rga_surface_s *src,
                     const struct rk3576_rga_surface_s *dst)
{
  return rk3576_rga_blit(src, dst, RK3576_RGA_ROTATE_0);
}

/****************************************************************************
 * Name: rk3576_rga_rotate
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_rotate(const struct rk3576_rga_surface_s *src,
                      const struct rk3576_rga_surface_s *dst,
                      int degrees)
{
  uint32_t flags;

  switch (degrees)
    {
      case 0:
        flags = RK3576_RGA_ROTATE_0;
        break;

      case 90:
        flags = RK3576_RGA_ROTATE_90;
        break;

      case 180:
        flags = RK3576_RGA_ROTATE_180;
        break;

      case 270:
        flags = RK3576_RGA_ROTATE_270;
        break;

      default:
        gerr("ERROR: unsupported rotation %d\n", degrees);
        return -EINVAL;
    }

  return rk3576_rga_blit(src, dst, flags);
}

/****************************************************************************
 * Name: rk3576_rga_csc
 *
 * Description:
 *   See rk3576_rga.h.
 *
 ****************************************************************************/

int rk3576_rga_csc(const struct rk3576_rga_surface_s *src,
                   const struct rk3576_rga_surface_s *dst,
                   int mode)
{
  struct rk3576_rga_dev_s *priv = rk3576_rga_default_dev();
  struct rk3576_rga_op_s op;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (src == NULL || dst == NULL ||
      mode < RK3576_RGA_CSC_AUTO || mode > RK3576_RGA_CSC_BT709L)
    {
      return -EINVAL;
    }

  memset(&op, 0, sizeof(op));
  op.src = *src;
  op.dst = *dst;
  op.flags = RK3576_RGA_ROTATE_0;
  op.galpha = UINT8_MAX;

  /* An explicit matrix request overrides whatever the surfaces carry. */

  if (mode != RK3576_RGA_CSC_AUTO)
    {
      op.src.csc_mode = (uint8_t)mode;
      op.dst.csc_mode = (uint8_t)mode;
    }

  return rk3576_rga_run(priv, &op, 0, false);
}

#endif /* CONFIG_RK3576_RGA */
