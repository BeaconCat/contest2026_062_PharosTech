/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_isp.c
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
 * RK3576 ISP (rkisp) driver — minimum viable Bayer pipeline.
 *
 * Sits behind the CSI-2 receiver and turns a raw Bayer stream into NV12
 * frames in DRAM using the shortest chain that still produces a usable
 * picture:
 *
 *   black level subtraction -> demosaic -> static white balance ->
 *   output gamma -> RGB-to-YUV -> memory interface (main path)
 *
 * Everything else the hardware offers — 3A statistics blocks, lens shading
 * correction, denoise, sharpening, HDR merge, the dedicated self path — is
 * left disabled; see the TODO list at the bottom of this comment.  Without
 * an auto-exposure and auto-white-balance loop the picture is only as good
 * as the fixed gains installed here, which is enough to prove the pipeline
 * end to end and to feed a downstream consumer such as the NPU.
 *
 * Output frames are double buffered: the memory interface base address is
 * a shadow register, so the driver points it at the other buffer inside
 * the frame-done interrupt and requests a soft update, giving continuous
 * capture without dropping frames.
 *
 * The block shares the VI power domain with the VICAP and VPSS blocks.
 * Its IOMMU stays in pass-through, which is valid because every buffer is
 * physically contiguous below 4GB (rk3576_dma_alloc).
 *
 * TODO: 3A statistics (AE/AWB/AF measurement windows and their readout),
 * lens shading, 2D/3D denoise and sharpening.  Those need the tuning
 * register blocks whose offsets are not yet confirmed for RK3576.
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
#include <stddef.h>
#include <stdint.h>
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_isp.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_isp.h"
#include "rk3576_pd.h"

#ifdef CONFIG_RK3576_ISP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NuttX IRQ numbers: GIC SPI 322 (memory interface) and 323 (ISP core). */

#define RK3576_ISP_MI_IRQ   RK3576_IRQ_ISP_MI
#define RK3576_ISP_CORE_IRQ RK3576_IRQ_ISP

/* The memory interface works on 16-byte groups, so the luma stride is
 * rounded up; NV12 additionally needs an even line count.
 */

#define RK3576_ISP_STRIDE_ALIGN   16
#define RK3576_ISP_ALIGN_UP(v, a) (((v) + ((a)-1)) & ~((a)-1))

/* Default static white balance: a mild daylight preset (unity green, a
 * little extra red and blue) so that a first frame is not visibly green.
 */

#define RK3576_ISP_WB_DEFAULT_R 0x160
#define RK3576_ISP_WB_DEFAULT_G RK3576_ISP_WB_UNITY
#define RK3576_ISP_WB_DEFAULT_B 0x148

/* Default black level in raw counts (typical 12-bit sensor pedestal).
 * TODO: replace with a per-sensor value once a sensor driver exists.
 */

#define RK3576_ISP_BLS_DEFAULT 64

/* Demosaic edge threshold: mid-range, favours detail over false colour. */

#define RK3576_ISP_DEMOSAIC_TH_DEFAULT 4

/* Shadow-register latch poll budget, in loop iterations. */

#define RK3576_ISP_UPD_TIMEOUT 100000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_isp_dev_s
{
  uintptr_t base;          /* Register block base               */
  mutex_t lock;            /* Serialises the control path       */
  bool initialized;        /* rk3576_isp_initialize() done      */
  bool configured;         /* A valid format is programmed      */
  volatile bool streaming; /* Pipeline is running               */

  struct rk3576_isp_format_s fmt; /* Active format                     */
  uint32_t stride;                /* Luma stride, in pixels            */
  size_t ysize;                   /* Luma plane size, bytes            */
  size_t framesize;               /* NV12 frame size, bytes            */

  void *buf[RK3576_ISP_NBUFFERS]; /* Output ping-pong buffers          */
  volatile uint8_t active;        /* Buffer the hardware is filling    */
  volatile uint32_t seq;          /* Completed frame counter           */

  rk3576_isp_frame_cb_t callback; /* Frame-complete callback           */
  void *cbarg;                    /* Callback context                  */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_isp_getreg(unsigned int offset);
static inline void rk3576_isp_putreg(unsigned int offset, uint32_t value);
static inline void rk3576_isp_modifyreg(unsigned int offset,
                                        uint32_t clearbits, uint32_t setbits);
static int rk3576_isp_clk_init(void);
static void rk3576_isp_cfg_update(void);
static void rk3576_isp_mi_update(void);
static int rk3576_isp_calc_geometry(struct rk3576_isp_dev_s *priv,
                                    const struct rk3576_isp_format_s *fmt);
static void rk3576_isp_free_buffers(struct rk3576_isp_dev_s *priv);
static int rk3576_isp_alloc_buffers(struct rk3576_isp_dev_s *priv);
static void rk3576_isp_program_mi(struct rk3576_isp_dev_s *priv,
                                  uint8_t index);
static void rk3576_isp_setup_pipeline(struct rk3576_isp_dev_s *priv);
static int rk3576_isp_mi_interrupt(int irq, void *context, void *arg);
static int rk3576_isp_core_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_isp_dev_s g_rk3576_isp = {
  .base = RK3576_ISP_ADDR,
  .lock = NXMUTEX_INITIALIZER,
};

/* Built-in output gamma curve: 17 equidistant 10-bit sample points that
 * approximate the sRGB transfer function (gamma 2.2 with a linear toe).
 */

static const uint16_t g_rk3576_isp_gamma_srgb[RK3576_ISP_GAMMA_POINTS] = {
  0,   67,  132, 190, 240, 283, 322, 358, 392,
  424, 454, 483, 511, 537, 563, 588, 1023
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_isp_getreg(unsigned int offset)
{
  return getreg32(g_rk3576_isp.base + offset);
}

static inline void rk3576_isp_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, g_rk3576_isp.base + offset);
}

static inline void rk3576_isp_modifyreg(unsigned int offset,
                                        uint32_t clearbits, uint32_t setbits)
{
  uint32_t regval;

  regval = rk3576_isp_getreg(offset);
  regval &= ~clearbits;
  regval |= setbits;
  rk3576_isp_putreg(offset, regval);
}

/****************************************************************************
 * Name: rk3576_isp_clk_init
 *
 * Description:
 *   Enable every clock the ISP needs.  All clock handling in this driver
 *   lives here and nowhere else.
 *
 *   aclk_isp / hclk_isp        - AXI read/write master and register bus
 *   clk_isp_core               - pipeline core clock
 *   clk_isp_core_marvin        - core clock branch feeding the Marvin
 *                                processing blocks
 *   clk_isp_core_vicap         - core clock branch feeding the VICAP
 *                                hand-off port
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_isp_clk_init(void)
{
  static const char *g_gates[] = {
    "aclk_isp_en",           "hclk_isp_en",
    "clk_isp_core_en",       "clk_isp_core_marvin_en",
    "clk_isp_core_vicap_en",
  };

  struct clk_s *clk;
  unsigned int i;
  int ret;

  for (i = 0; i < nitems(g_gates); i++)
    {
      clk = clk_get(g_gates[i]);
      if (clk == NULL)
        {
          verr("ERROR: failed to get %s\n", g_gates[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          verr("ERROR: failed to enable %s: %d\n", g_gates[i], ret);
          return ret;
        }
    }

  clk = clk_get("clk_isp_core_en");
  if (clk != NULL)
    {
      vinfo("ISP: core clock %" PRIu32 " Hz\n", clk_get_rate(clk));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_cfg_update
 *
 * Description:
 *   Latch the pipeline shadow registers.  Every tuning block is double
 *   buffered; the values written take effect only after this handshake.
 *
 ****************************************************************************/

static void rk3576_isp_cfg_update(void)
{
  unsigned int i;

  rk3576_isp_modifyreg(RK3576_ISP_CTRL_OFFSET, 0, RK3576_ISP_CTRL_CFG_UPD);

  /* The bit is self-clearing once the hardware has taken the new
   * configuration.  Bail out rather than spin forever if it does not.
   */

  for (i = 0; i < RK3576_ISP_UPD_TIMEOUT; i++)
    {
      if ((rk3576_isp_getreg(RK3576_ISP_CTRL_OFFSET) &
           RK3576_ISP_CTRL_CFG_UPD) == 0)
        {
          return;
        }
    }

  verr("ERROR: ISP shadow register latch timed out\n");
}

/****************************************************************************
 * Name: rk3576_isp_mi_update
 *
 * Description:
 *   Latch the memory-interface shadow registers, which is how a new frame
 *   base address is handed to the write master.
 *
 ****************************************************************************/

static void rk3576_isp_mi_update(void)
{
  rk3576_isp_putreg(RK3576_ISP_MI_INIT_OFFSET,
                    RK3576_ISP_MI_CTRL_INIT_BASE_EN |
                        RK3576_ISP_MI_CTRL_INIT_OFFS_EN |
                        RK3576_ISP_MI_INIT_SOFT_UPD);
}

/****************************************************************************
 * Name: rk3576_isp_calc_geometry
 *
 * Description:
 *   Validate the requested format and derive the NV12 stride and plane
 *   sizes from it.
 *
 * Returned Value:
 *   OK on success, -EINVAL for an unsupported or out-of-range format.
 *
 ****************************************************************************/

static int rk3576_isp_calc_geometry(struct rk3576_isp_dev_s *priv,
                                    const struct rk3576_isp_format_s *fmt)
{
  if (fmt->width < RK3576_ISP_MIN_WIDTH || fmt->width > RK3576_ISP_MAX_WIDTH ||
      fmt->height < RK3576_ISP_MIN_HEIGHT ||
      fmt->height > RK3576_ISP_MAX_HEIGHT || (fmt->height & 1) != 0)
    {
      verr("ERROR: unsupported geometry %ux%u\n", fmt->width, fmt->height);
      return -EINVAL;
    }

  if (fmt->bayer > RK3576_ISP_BAYER_BGGR)
    {
      verr("ERROR: invalid Bayer phase %u\n", fmt->bayer);
      return -EINVAL;
    }

  if (fmt->depth != RK3576_ISP_RAW8 && fmt->depth != RK3576_ISP_RAW10 &&
      fmt->depth != RK3576_ISP_RAW12)
    {
      verr("ERROR: invalid raw depth %u\n", fmt->depth);
      return -EINVAL;
    }

  priv->stride = RK3576_ISP_ALIGN_UP(fmt->width, RK3576_ISP_STRIDE_ALIGN);
  priv->ysize = (size_t)priv->stride * fmt->height;

  /* NV12: full luma plane plus a half-height interleaved chroma plane. */

  priv->framesize = priv->ysize + priv->ysize / 2;
  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_free_buffers
 *
 * Description:
 *   Release the output ping-pong buffers, if any are held.
 *
 ****************************************************************************/

static void rk3576_isp_free_buffers(struct rk3576_isp_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < RK3576_ISP_NBUFFERS; i++)
    {
      if (priv->buf[i] != NULL)
        {
          rk3576_dma_free(priv->buf[i], priv->framesize);
          priv->buf[i] = NULL;
        }
    }
}

/****************************************************************************
 * Name: rk3576_isp_alloc_buffers
 *
 * Description:
 *   Allocate the NV12 output buffers from the DMA-safe heap: 64-byte
 *   aligned and physically below 4GB, as the 32-bit ISP write master
 *   requires.
 *
 * Returned Value:
 *   OK on success, -ENOMEM if the DMA heap is exhausted.
 *
 ****************************************************************************/

static int rk3576_isp_alloc_buffers(struct rk3576_isp_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < RK3576_ISP_NBUFFERS; i++)
    {
      priv->buf[i] = rk3576_dma_alloc(priv->framesize);
      if (priv->buf[i] == NULL)
        {
          verr("ERROR: no DMA memory for %zu-byte frame buffer %u\n",
               priv->framesize, i);
          rk3576_isp_free_buffers(priv);
          return -ENOMEM;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_program_mi
 *
 * Description:
 *   Point the main-path write master at one of the output buffers and
 *   latch it.  The chroma plane follows the luma plane inside the same
 *   allocation, and the (unused in NV12) Cr pointer is parked on it too.
 *
 ****************************************************************************/

static void rk3576_isp_program_mi(struct rk3576_isp_dev_s *priv, uint8_t index)
{
  uintptr_t phys;

  phys = up_addrenv_va_to_pa(priv->buf[index]);

  rk3576_isp_putreg(RK3576_ISP_MI_MP_Y_BASE_OFFSET, (uint32_t)phys);
  rk3576_isp_putreg(RK3576_ISP_MI_MP_Y_SIZE_OFFSET, (uint32_t)priv->ysize);
  rk3576_isp_putreg(RK3576_ISP_MI_MP_Y_OFFS_OFFSET, 0);

  rk3576_isp_putreg(RK3576_ISP_MI_MP_CB_BASE_OFFSET,
                    (uint32_t)(phys + priv->ysize));
  rk3576_isp_putreg(RK3576_ISP_MI_MP_CB_SIZE_OFFSET,
                    (uint32_t)(priv->ysize / 2));
  rk3576_isp_putreg(RK3576_ISP_MI_MP_CB_OFFS_OFFSET, 0);

  rk3576_isp_putreg(RK3576_ISP_MI_MP_CR_BASE_OFFSET,
                    (uint32_t)(phys + priv->ysize));
  rk3576_isp_putreg(RK3576_ISP_MI_MP_CR_SIZE_OFFSET, 0);

  rk3576_isp_mi_update();
}

/****************************************************************************
 * Name: rk3576_isp_setup_pipeline
 *
 * Description:
 *   Program the minimum viable processing chain for the active format:
 *   acquisition window, fixed black level, demosaic, static white balance
 *   and the output gamma curve.  The caller latches the shadow registers.
 *
 ****************************************************************************/

static void rk3576_isp_setup_pipeline(struct rk3576_isp_dev_s *priv)
{
  static const uint32_t g_bayer[] = {
    RK3576_ISP_ACQ_BAYER_RGGB,
    RK3576_ISP_ACQ_BAYER_GRBG,
    RK3576_ISP_ACQ_BAYER_GBRG,
    RK3576_ISP_ACQ_BAYER_BGGR,
  };

  struct rk3576_isp_wbgain_s wb;
  uint32_t regval;

  /* Acquisition: whole sensor window, Bayer phase from the caller. */

  regval = RK3576_ISP_ACQ_SAMPL_EDGE_POS | g_bayer[priv->fmt.bayer];
  if (priv->fmt.depth == RK3576_ISP_RAW12)
    {
      regval |= RK3576_ISP_ACQ_INPUT_SEL_12B;
    }

  rk3576_isp_putreg(RK3576_ISP_ACQ_PROP_OFFSET, regval);
  rk3576_isp_putreg(RK3576_ISP_ACQ_H_OFFS_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_ACQ_V_OFFS_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_ACQ_H_SIZE_OFFSET, priv->fmt.width);
  rk3576_isp_putreg(RK3576_ISP_ACQ_V_SIZE_OFFSET, priv->fmt.height);
  rk3576_isp_putreg(RK3576_ISP_ACQ_NR_FRAMES_OFF, 0); /* free running */

  /* Output window matches the acquisition window: no crop, no scaling. */

  rk3576_isp_putreg(RK3576_ISP_OUT_H_OFFS_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_OUT_V_OFFS_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_OUT_H_SIZE_OFFSET, priv->fmt.width);
  rk3576_isp_putreg(RK3576_ISP_OUT_V_SIZE_OFFSET, priv->fmt.height);

  /* Black level: fixed pedestal on all four Bayer positions. */

  rk3576_isp_set_black_level(RK3576_ISP_BLS_DEFAULT, RK3576_ISP_BLS_DEFAULT,
                             RK3576_ISP_BLS_DEFAULT, RK3576_ISP_BLS_DEFAULT);

  /* Demosaic on, mid-range edge threshold. */

  rk3576_isp_putreg(RK3576_ISP_DEMOSAIC_OFFSET,
                    RK3576_ISP_DEMOSAIC_TH_DEFAULT &
                        RK3576_ISP_DEMOSAIC_TH_MASK);

  /* Static white balance preset — no AWB loop yet. */

  wb.r = RK3576_ISP_WB_DEFAULT_R;
  wb.gr = RK3576_ISP_WB_DEFAULT_G;
  wb.gb = RK3576_ISP_WB_DEFAULT_G;
  wb.b = RK3576_ISP_WB_DEFAULT_B;
  rk3576_isp_set_wbgain(&wb);

  /* Default output gamma. */

  rk3576_isp_set_gamma(NULL);

  /* Memory interface: main path writing semi-planar YUV (NV12). */

  rk3576_isp_putreg(
      RK3576_ISP_MI_CTRL_OFFSET,
      RK3576_ISP_MI_CTRL_MP_ENABLE | RK3576_ISP_MI_CTRL_MP_WRITE_YUV |
          RK3576_ISP_MI_CTRL_MP_FMT_SEMI | RK3576_ISP_MI_CTRL_BURST_LEN_16);
}

/****************************************************************************
 * Name: rk3576_isp_mi_interrupt
 *
 * Description:
 *   Memory-interface interrupt: one processed frame has been written out.
 *   The completed buffer is invalidated and reported, and the write
 *   master is flipped onto the other buffer for the next frame.
 *
 * Returned Value:
 *   Always OK.
 *
 ****************************************************************************/

static int rk3576_isp_mi_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_isp_dev_s *priv = arg;
  uint32_t status;
  uint8_t done;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_isp_getreg(RK3576_ISP_MI_MIS_OFFSET);
  if (status == 0)
    {
      return OK;
    }

  rk3576_isp_putreg(RK3576_ISP_MI_ICR_OFFSET, status);

  if ((status & RK3576_ISP_MI_INT_MP_FRAME) == 0 || !priv->streaming)
    {
      return OK;
    }

  done = priv->active;
  priv->active = (uint8_t)((done + 1) % RK3576_ISP_NBUFFERS);

  /* Hand the next buffer to the write master before touching the caches,
   * so the hardware never idles waiting for an address.
   */

  rk3576_isp_program_mi(priv, priv->active);

  up_invalidate_dcache((uintptr_t)priv->buf[done],
                       (uintptr_t)priv->buf[done] + priv->framesize);

  if (priv->callback != NULL)
    {
      priv->callback(priv->cbarg, priv->buf[done], priv->framesize, priv->seq);
    }

  priv->seq++;
  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_core_interrupt
 *
 * Description:
 *   ISP core interrupt: reports pipeline level conditions.  Only errors
 *   are acted upon; the frame accounting lives in the memory-interface
 *   handler.
 *
 * Returned Value:
 *   Always OK.
 *
 ****************************************************************************/

static int rk3576_isp_core_interrupt(int irq, void *context, void *arg)
{
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  status = rk3576_isp_getreg(RK3576_ISP_MIS_OFFSET);
  if (status == 0)
    {
      return OK;
    }

  rk3576_isp_putreg(RK3576_ISP_ICR_OFFSET, status);

  if ((status & (RK3576_ISP_INT_DATA_LOSS | RK3576_ISP_INT_PIC_SIZE_ERR)) != 0)
    {
      verr("ERROR: ISP pipeline error, MIS 0x%08" PRIx32 "\n", status);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_isp_initialize
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_initialize(void)
{
  struct rk3576_isp_dev_s *priv = &g_rk3576_isp;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->initialized)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  ret = rk3576_pd_on(RK3576_PD_VI);
  if (ret < 0)
    {
      verr("ERROR: failed to power up PD_VI: %d\n", ret);
      goto errout;
    }

  ret = rk3576_isp_clk_init();
  if (ret < 0)
    {
      goto errout;
    }

  /* Park the pipeline: processing off, both interrupt sources masked and
   * any latched status cleared.
   */

  rk3576_isp_putreg(RK3576_ISP_CTRL_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_IMSC_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_ICR_OFFSET, RK3576_ISP_INT_ALL);
  rk3576_isp_putreg(RK3576_ISP_MI_CTRL_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_MI_IMSC_OFFSET, 0);
  rk3576_isp_putreg(RK3576_ISP_MI_ICR_OFFSET, RK3576_ISP_MI_INT_ALL);

  ret = irq_attach(RK3576_ISP_MI_IRQ, rk3576_isp_mi_interrupt, priv);
  if (ret < 0)
    {
      verr("ERROR: failed to attach IRQ %d: %d\n", RK3576_ISP_MI_IRQ, ret);
      goto errout;
    }

  ret = irq_attach(RK3576_ISP_CORE_IRQ, rk3576_isp_core_interrupt, priv);
  if (ret < 0)
    {
      verr("ERROR: failed to attach IRQ %d: %d\n", RK3576_ISP_CORE_IRQ, ret);
      irq_detach(RK3576_ISP_MI_IRQ);
      goto errout;
    }

  up_enable_irq(RK3576_ISP_MI_IRQ);
  up_enable_irq(RK3576_ISP_CORE_IRQ);

  priv->initialized = true;
  nxmutex_unlock(&priv->lock);

  vinfo("ISP initialized at 0x%08" PRIxPTR "\n", priv->base);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_isp_set_format
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_set_format(const struct rk3576_isp_format_s *fmt)
{
  struct rk3576_isp_dev_s *priv = &g_rk3576_isp;
  size_t oldsize;
  int ret;

  DEBUGASSERT(fmt != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->initialized)
    {
      ret = -EPERM;
      goto errout;
    }

  if (priv->streaming)
    {
      ret = -EBUSY;
      goto errout;
    }

  /* Free the old buffers while priv->framesize still describes them. */

  oldsize = priv->framesize;
  if (priv->configured)
    {
      rk3576_isp_free_buffers(priv);
      priv->configured = false;
    }

  ret = rk3576_isp_calc_geometry(priv, fmt);
  if (ret < 0)
    {
      priv->framesize = oldsize;
      goto errout;
    }

  priv->fmt = *fmt;

  ret = rk3576_isp_alloc_buffers(priv);
  if (ret < 0)
    {
      goto errout;
    }

  rk3576_isp_setup_pipeline(priv);

  priv->active = 0;
  rk3576_isp_program_mi(priv, priv->active);
  rk3576_isp_cfg_update();

  priv->configured = true;
  nxmutex_unlock(&priv->lock);

  vinfo("ISP format %ux%u raw%u bayer %u -> NV12, frame %zu bytes\n",
        fmt->width, fmt->height, fmt->depth, fmt->bayer, priv->framesize);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_isp_get_framesize
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

size_t rk3576_isp_get_framesize(void)
{
  struct rk3576_isp_dev_s *priv = &g_rk3576_isp;

  return priv->configured ? priv->framesize : 0;
}

/****************************************************************************
 * Name: rk3576_isp_set_black_level
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_set_black_level(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
  rk3576_isp_putreg(RK3576_ISP_BLS_A_FIXED_OFFSET, a);
  rk3576_isp_putreg(RK3576_ISP_BLS_B_FIXED_OFFSET, b);
  rk3576_isp_putreg(RK3576_ISP_BLS_C_FIXED_OFFSET, c);
  rk3576_isp_putreg(RK3576_ISP_BLS_D_FIXED_OFFSET, d);

  rk3576_isp_putreg(RK3576_ISP_BLS_CTRL_OFFSET,
                    RK3576_ISP_BLS_CTRL_ENABLE | RK3576_ISP_BLS_CTRL_MODE_FIX);
  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_set_wbgain
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_set_wbgain(const struct rk3576_isp_wbgain_s *gain)
{
  if (gain == NULL || gain->r > RK3576_ISP_WB_MAX ||
      gain->gr > RK3576_ISP_WB_MAX || gain->gb > RK3576_ISP_WB_MAX ||
      gain->b > RK3576_ISP_WB_MAX)
    {
      return -EINVAL;
    }

  rk3576_isp_putreg(RK3576_ISP_AWB_GAIN_G_OFFSET,
                    ((uint32_t)gain->gr << 16) | gain->gb);
  rk3576_isp_putreg(RK3576_ISP_AWB_GAIN_RB_OFFSET,
                    ((uint32_t)gain->r << 16) | gain->b);
  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_set_gamma
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_set_gamma(const uint16_t *curve)
{
  unsigned int i;

  if (curve == NULL)
    {
      curve = g_rk3576_isp_gamma_srgb;
    }

  rk3576_isp_putreg(RK3576_ISP_GAMMA_OUT_MODE_OFF,
                    RK3576_ISP_GAMMA_OUT_EQU_SEG);

  for (i = 0; i < RK3576_ISP_GAMMA_POINTS; i++)
    {
      rk3576_isp_putreg(RK3576_ISP_GAMMA_OUT_Y_OFFSET + (i * 4), curve[i]);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_isp_start_streaming
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_start_streaming(rk3576_isp_frame_cb_t callback, void *arg)
{
  struct rk3576_isp_dev_s *priv = &g_rk3576_isp;
  unsigned int i;
  int ret;

  if (callback == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->configured)
    {
      ret = -EPERM;
      goto errout;
    }

  if (priv->streaming)
    {
      ret = -EBUSY;
      goto errout;
    }

  priv->callback = callback;
  priv->cbarg = arg;
  priv->active = 0;
  priv->seq = 0;

  /* The buffers are about to be written by a bus master: clean any dirty
   * lines so a later eviction cannot corrupt a captured frame.
   */

  for (i = 0; i < RK3576_ISP_NBUFFERS; i++)
    {
      up_clean_dcache((uintptr_t)priv->buf[i],
                      (uintptr_t)priv->buf[i] + priv->framesize);
    }

  rk3576_isp_program_mi(priv, priv->active);

  rk3576_isp_putreg(RK3576_ISP_ICR_OFFSET, RK3576_ISP_INT_ALL);
  rk3576_isp_putreg(RK3576_ISP_MI_ICR_OFFSET, RK3576_ISP_MI_INT_ALL);
  rk3576_isp_putreg(RK3576_ISP_MI_IMSC_OFFSET, RK3576_ISP_MI_INT_MP_FRAME);
  rk3576_isp_putreg(RK3576_ISP_IMSC_OFFSET, RK3576_ISP_INT_FRAME |
                                                RK3576_ISP_INT_DATA_LOSS |
                                                RK3576_ISP_INT_PIC_SIZE_ERR);

  priv->streaming = true;

  rk3576_isp_modifyreg(RK3576_ISP_CTRL_OFFSET, RK3576_ISP_CTRL_MODE_MASK,
                       RK3576_ISP_CTRL_MODE_BAYER |
                           RK3576_ISP_CTRL_INFORM_ENABLE |
                           RK3576_ISP_CTRL_ENABLE);
  rk3576_isp_cfg_update();

  nxmutex_unlock(&priv->lock);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_isp_stop_streaming
 *
 * Description:
 *   See rk3576_isp.h.
 *
 ****************************************************************************/

int rk3576_isp_stop_streaming(void)
{
  struct rk3576_isp_dev_s *priv = &g_rk3576_isp;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->streaming)
    {
      priv->streaming = false;

      rk3576_isp_putreg(RK3576_ISP_MI_IMSC_OFFSET, 0);
      rk3576_isp_putreg(RK3576_ISP_IMSC_OFFSET, 0);

      rk3576_isp_modifyreg(
          RK3576_ISP_CTRL_OFFSET,
          RK3576_ISP_CTRL_ENABLE | RK3576_ISP_CTRL_INFORM_ENABLE, 0);
      rk3576_isp_cfg_update();

      rk3576_isp_putreg(RK3576_ISP_MI_ICR_OFFSET, RK3576_ISP_MI_INT_ALL);
      rk3576_isp_putreg(RK3576_ISP_ICR_OFFSET, RK3576_ISP_INT_ALL);

      priv->callback = NULL;
      priv->cbarg = NULL;
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

#endif /* CONFIG_RK3576_ISP */
