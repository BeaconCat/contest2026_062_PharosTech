/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_vicap.c
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
 * RK3576 VICAP (rkcif) image capture driver — bypass path.
 *
 * The VICAP block takes a pixel stream from either the parallel DVP
 * interface or one virtual channel of a MIPI CSI-2 host and writes it
 * directly to DRAM, with no ISP processing in between.  That "bypass"
 * route is the shortest possible camera path and is what this driver
 * implements: RAW8/10/12 straight from a Bayer sensor, or NV12/NV16 from
 * a sensor that already emits YUV.
 *
 * The hardware owns two frame base-address pairs (FRM0/FRM1) and walks
 * between them by itself in ping-pong mode, raising a frame-end interrupt
 * after each one.  The driver allocates both buffers from the DMA-safe
 * heap (rk3576_dma_alloc), invalidates the D-cache over the completed
 * frame and hands it to the registered callback; the buffer returns to
 * the hardware as soon as the callback returns, so a consumer that needs
 * to keep the data must copy it.
 *
 * The block sits in the VI power domain, shared with the ISP, the VPSS
 * and the CSI-2 hosts.  Its IOMMU is left disabled (pass-through), which
 * is valid because every buffer comes from the physically-contiguous DMA
 * heap below 4GB.
 *
 * The NuttX video stack (drivers/video, struct imgdata_s) is supported as
 * an optional front end: when CONFIG_RK3576_VICAP_VIDEO is selected the
 * driver exposes an imgdata lower half that video_register() can bind to
 * an image sensor, giving a standard V4L2-style /dev/videoN.  Without a
 * sensor lower half the native API below is used directly.
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
#include <string.h>
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>

#ifdef CONFIG_RK3576_VICAP_VIDEO
#include <nuttx/video/imgdata.h>
#endif

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_isp.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_pd.h"
#include "rk3576_vicap.h"

#ifdef CONFIG_RK3576_VICAP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NuttX IRQ number of the VICAP frame interrupt (GIC SPI 318 + 32). */

#define RK3576_VICAP_IRQ RK3576_IRQ_VICAP

/* The write DMA works on 8-pixel groups, so the line stride is rounded up. */

#define RK3576_VICAP_STRIDE_ALIGN   8
#define RK3576_VICAP_ALIGN_UP(v, a) (((v) + ((a)-1)) & ~((a)-1))

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_vicap_dev_s
{
  uintptr_t base;          /* Register block base            */
  mutex_t lock;            /* Serialises the control path    */
  bool initialized;        /* rk3576_vicap_initialize() done */
  bool configured;         /* A valid format is programmed   */
  volatile bool streaming; /* Capture DMA is running         */

  struct rk3576_vicap_format_s fmt; /* Active format                  */
  uint32_t stride;                  /* Line stride, in pixels         */
  size_t ysize;                     /* Luma / RAW plane size, bytes   */
  size_t framesize;                 /* Whole frame size, bytes        */

  void *buf[RK3576_VICAP_NBUFFERS]; /* Ping-pong DMA buffers          */
  volatile uint8_t next;            /* Buffer the hardware fills next */
  volatile uint32_t seq;            /* Completed frame counter        */

  rk3576_vicap_frame_cb_t callback; /* Frame-complete callback        */
  void *cbarg;                      /* Callback context               */

#ifdef CONFIG_RK3576_VICAP_VIDEO
  struct imgdata_s data;    /* Video stack lower half         */
  bool extbuf;              /* Video stack owns the buffers   */
  uintptr_t extaddr;        /* Externally supplied frame VA   */
  size_t extsize;           /* Size of that buffer, bytes     */
  imgdata_capture_t datacb; /* Video stack frame callback     */
  void *dataarg;            /* Video stack callback context   */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_vicap_getreg(unsigned int offset);
static inline void rk3576_vicap_putreg(unsigned int offset, uint32_t value);
static int rk3576_vicap_clk_init(void);
static int rk3576_vicap_calc_geometry(struct rk3576_vicap_dev_s *priv,
                                      const struct rk3576_vicap_format_s *fmt);
static uint32_t
rk3576_vicap_build_for(const struct rk3576_vicap_format_s *fmt);
static void rk3576_vicap_free_buffers(struct rk3576_vicap_dev_s *priv);
static int rk3576_vicap_alloc_buffers(struct rk3576_vicap_dev_s *priv);
static void rk3576_vicap_program_buffers(struct rk3576_vicap_dev_s *priv);
static int rk3576_vicap_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_vicap_dev_s g_rk3576_vicap = {
  .base = RK3576_VICAP_ADDR,
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_vicap_getreg(unsigned int offset)
{
  return getreg32(g_rk3576_vicap.base + offset);
}

static inline void rk3576_vicap_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, g_rk3576_vicap.base + offset);
}

/****************************************************************************
 * Name: rk3576_vicap_clk_init
 *
 * Description:
 *   Enable every clock the capture block needs.  All clock handling in
 *   this driver lives here and nowhere else.
 *
 *   aclk_cif / hclk_cif  - AXI write master and APB register interface
 *   dclk_cif             - capture pixel clock (600 MHz per the vendor DT)
 *   clk_cif_i0..i4       - per-input sampling clocks; the driver enables
 *                          all of them because the input a given board
 *                          uses is a board-level pinmux decision.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_vicap_clk_init(void)
{
  static const char *g_gates[] = {
    "aclk_cif_en",   "hclk_cif_en",   "dclk_cif_en",   "clk_cif_i0_en",
    "clk_cif_i1_en", "clk_cif_i2_en", "clk_cif_i3_en", "clk_cif_i4_en",
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

  clk = clk_get("dclk_cif_en");
  if (clk != NULL)
    {
      vinfo("VICAP: dclk %" PRIu32 " Hz\n", clk_get_rate(clk));
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_calc_geometry
 *
 * Description:
 *   Validate the requested format and derive the line stride, the luma /
 *   RAW plane size and the total frame size from it.
 *
 * Returned Value:
 *   OK on success, -EINVAL for an unsupported or out-of-range format.
 *
 ****************************************************************************/

static int rk3576_vicap_calc_geometry(struct rk3576_vicap_dev_s *priv,
                                      const struct rk3576_vicap_format_s *fmt)
{
  uint32_t stride;
  size_t bytes_per_pixel_num;
  size_t bytes_per_pixel_den;

  if (fmt->width < RK3576_VICAP_MIN_WIDTH ||
      fmt->width > RK3576_VICAP_MAX_WIDTH ||
      fmt->height < RK3576_VICAP_MIN_HEIGHT ||
      fmt->height > RK3576_VICAP_MAX_HEIGHT)
    {
      verr("ERROR: unsupported geometry %ux%u\n", fmt->width, fmt->height);
      return -EINVAL;
    }

  stride = RK3576_VICAP_ALIGN_UP(fmt->width, RK3576_VICAP_STRIDE_ALIGN);

  switch (fmt->pixelformat)
    {
      case RK3576_VICAP_FMT_NV12:

        /* Semi-planar 4:2:0: full luma plane plus a half-height
         * interleaved chroma plane.
         */

        bytes_per_pixel_num = 3;
        bytes_per_pixel_den = 2;
        break;

      case RK3576_VICAP_FMT_NV16:
      case RK3576_VICAP_FMT_UYVY:
      case RK3576_VICAP_FMT_YUYV:

        /* Semi-planar 4:2:2 output.  UYVY / YUYV select the packed input
         * byte order; the capture DMA always writes semi-planar.
         */

        bytes_per_pixel_num = 2;
        bytes_per_pixel_den = 1;
        break;

      case RK3576_VICAP_FMT_SRGGB8:
        bytes_per_pixel_num = 1;
        bytes_per_pixel_den = 1;
        break;

      case RK3576_VICAP_FMT_SRGGB10:
      case RK3576_VICAP_FMT_SRGGB12:

        /* RAW10 / RAW12 are unpacked to one 16-bit word per pixel. */

        bytes_per_pixel_num = 2;
        bytes_per_pixel_den = 1;
        break;

      default:
        verr("ERROR: unsupported pixel format 0x%08" PRIx32 "\n",
             fmt->pixelformat);
        return -EINVAL;
    }

  priv->stride = stride;
  priv->ysize = (size_t)stride * fmt->height;
  priv->framesize = priv->ysize * bytes_per_pixel_num / bytes_per_pixel_den;

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_build_for
 *
 * Description:
 *   Translate a format request into the CIF_FOR register value: input
 *   timing polarity, input mode and byte order, and the output storage
 *   layout.
 *
 * Returned Value:
 *   The CIF_FOR register value.
 *
 ****************************************************************************/

static uint32_t rk3576_vicap_build_for(const struct rk3576_vicap_format_s *fmt)
{
  uint32_t regval = 0;

  if (fmt->vsync_active_high)
    {
      regval |= RK3576_CIF_FOR_VSY_HIGH_ACTIVE;
    }

  if (fmt->hsync_active_low)
    {
      regval |= RK3576_CIF_FOR_HSY_LOW_ACTIVE;
    }

  switch (fmt->pixelformat)
    {
      case RK3576_VICAP_FMT_NV12:
        regval |= RK3576_CIF_FOR_INPUT_MODE_YUV |
                  RK3576_CIF_FOR_YUV_ORDER_UYVY |
                  RK3576_CIF_FOR_YUV_OUTPUT_420;
        break;

      case RK3576_VICAP_FMT_NV16:
      case RK3576_VICAP_FMT_UYVY:
        regval |=
            RK3576_CIF_FOR_INPUT_MODE_YUV | RK3576_CIF_FOR_YUV_ORDER_UYVY;
        break;

      case RK3576_VICAP_FMT_YUYV:
        regval |=
            RK3576_CIF_FOR_INPUT_MODE_YUV | RK3576_CIF_FOR_YUV_ORDER_YUYV;
        break;

      case RK3576_VICAP_FMT_SRGGB8:
        regval |= RK3576_CIF_FOR_INPUT_MODE_RAW | RK3576_CIF_FOR_RAW_WIDTH_8;
        break;

      case RK3576_VICAP_FMT_SRGGB10:
        regval |= RK3576_CIF_FOR_INPUT_MODE_RAW | RK3576_CIF_FOR_RAW_WIDTH_10;
        break;

      case RK3576_VICAP_FMT_SRGGB12:
        regval |= RK3576_CIF_FOR_INPUT_MODE_RAW | RK3576_CIF_FOR_RAW_WIDTH_12;
        break;

      default:

        /* Filtered out by rk3576_vicap_calc_geometry() already. */

        DEBUGPANIC();
        break;
    }

  return regval;
}

/****************************************************************************
 * Name: rk3576_vicap_free_buffers
 *
 * Description:
 *   Release the ping-pong DMA buffers, if any are held.
 *
 ****************************************************************************/

static void rk3576_vicap_free_buffers(struct rk3576_vicap_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < RK3576_VICAP_NBUFFERS; i++)
    {
      if (priv->buf[i] != NULL)
        {
          rk3576_dma_free(priv->buf[i], priv->framesize);
          priv->buf[i] = NULL;
        }
    }
}

/****************************************************************************
 * Name: rk3576_vicap_alloc_buffers
 *
 * Description:
 *   Allocate the two ping-pong frame buffers from the DMA-safe heap.  The
 *   heap guarantees 64-byte alignment and a physical address below 4GB,
 *   which the 32-bit VICAP write master requires.
 *
 * Returned Value:
 *   OK on success, -ENOMEM if the DMA heap is exhausted.
 *
 ****************************************************************************/

static int rk3576_vicap_alloc_buffers(struct rk3576_vicap_dev_s *priv)
{
  unsigned int i;

  for (i = 0; i < RK3576_VICAP_NBUFFERS; i++)
    {
      priv->buf[i] = rk3576_dma_alloc(priv->framesize);
      if (priv->buf[i] == NULL)
        {
          verr("ERROR: no DMA memory for %zu-byte frame buffer %u\n",
               priv->framesize, i);
          rk3576_vicap_free_buffers(priv);
          return -ENOMEM;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_program_buffers
 *
 * Description:
 *   Write the physical base addresses of both ping-pong buffers into the
 *   FRM0 / FRM1 address registers.  The chroma plane of a semi-planar
 *   format directly follows the luma plane inside the same allocation.
 *
 ****************************************************************************/

static void rk3576_vicap_program_buffers(struct rk3576_vicap_dev_s *priv)
{
  uintptr_t phys;

  phys = up_addrenv_va_to_pa(priv->buf[0]);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_Y_OFFSET, (uint32_t)phys);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_UV_OFFSET,
                      (uint32_t)(phys + priv->ysize));

  phys = up_addrenv_va_to_pa(priv->buf[1]);
  rk3576_vicap_putreg(RK3576_CIF_FRM1_ADDR_Y_OFFSET, (uint32_t)phys);
  rk3576_vicap_putreg(RK3576_CIF_FRM1_ADDR_UV_OFFSET,
                      (uint32_t)(phys + priv->ysize));
}

/****************************************************************************
 * Name: rk3576_vicap_interrupt
 *
 * Description:
 *   VICAP interrupt handler.  On every frame-end the completed ping-pong
 *   buffer is invalidated from the D-cache and handed to the registered
 *   callback; the hardware keeps writing into the other buffer meanwhile,
 *   so nothing has to be re-armed.
 *
 * Returned Value:
 *   Always OK.
 *
 ****************************************************************************/

static int rk3576_vicap_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_vicap_dev_s *priv = arg;
  uint32_t status;
  uint8_t done;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_vicap_getreg(RK3576_CIF_INTSTAT_OFFSET);
  if (status == 0)
    {
      return OK;
    }

  /* The status register is write-1-to-clear. */

  rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, status);

  if ((status & (RK3576_CIF_INT_LINE_ERR | RK3576_CIF_INT_BUS_ERR)) != 0)
    {
      verr("ERROR: VICAP capture error, INTSTAT 0x%08" PRIx32 "\n", status);
    }

  if ((status & RK3576_CIF_INT_FRAME_END) == 0 || !priv->streaming)
    {
      return OK;
    }

#ifdef CONFIG_RK3576_VICAP_VIDEO
  if (priv->extbuf)
    {
      struct timespec ts;

      /* Single-shot mode: the video stack owns the buffer, so capture
       * stops here and is re-armed when the stack hands over the next
       * one through set_buf() / start_capture().
       */

      priv->streaming = false;
      rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, 0);
      rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, 0);

      up_invalidate_dcache(priv->extaddr, priv->extaddr + priv->framesize);

      clock_systime_timespec(&ts);
      if (priv->datacb != NULL)
        {
          priv->datacb(0, (uint32_t)priv->framesize, &ts, priv->dataarg);
        }

      priv->seq++;
      return OK;
    }
#endif

  done = priv->next;
  priv->next = (uint8_t)((done + 1) % RK3576_VICAP_NBUFFERS);

  /* The frame was written by a bus master, so drop any stale cache lines
   * before the consumer reads it.
   */

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
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vicap_initialize
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

int rk3576_vicap_initialize(void)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
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

  /* ISP, VICAP, VPSS and the CSI-2 hosts all live in the VI power
   * domain.  Bring it up before touching any register.
   */

  ret = rk3576_pd_on(RK3576_PD_VI);
  if (ret < 0)
    {
      verr("ERROR: failed to power up PD_VI: %d\n", ret);
      goto errout;
    }

  ret = rk3576_vicap_clk_init();
  if (ret < 0)
    {
      goto errout;
    }

  /* Park the block: capture off, all interrupts masked, any latched
   * status cleared.
   *
   * TODO: assert the aclk/hclk/dclk soft resets here once the RK3576
   * reset identifiers are confirmed against the TRM.  Disabling capture
   * is enough for a cold boot, but a warm restart after a hung sensor
   * would benefit from a real reset.
   */

  rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, 0);
  rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, 0);
  rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, 0xffffffff);

  /* The scaler is unused on the bypass path. */

  rk3576_vicap_putreg(RK3576_CIF_SCL_CTRL_OFFSET, 0);

  ret = irq_attach(RK3576_VICAP_IRQ, rk3576_vicap_interrupt, priv);
  if (ret < 0)
    {
      verr("ERROR: failed to attach IRQ %d: %d\n", RK3576_VICAP_IRQ, ret);
      goto errout;
    }

  up_enable_irq(RK3576_VICAP_IRQ);

  priv->initialized = true;
  nxmutex_unlock(&priv->lock);

  vinfo("VICAP initialized at 0x%08" PRIxPTR "\n", priv->base);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_vicap_set_format
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

int rk3576_vicap_set_format(const struct rk3576_vicap_format_s *fmt)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
  size_t oldsize;
  uint32_t regval;
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

  /* Release the previous buffers first: rk3576_dma_free() must be given
   * the size the allocation was made with, which the new format is about
   * to overwrite.
   */

  oldsize = priv->framesize;
  if (priv->configured)
    {
      rk3576_vicap_free_buffers(priv);
      priv->configured = false;
    }

  ret = rk3576_vicap_calc_geometry(priv, fmt);
  if (ret < 0)
    {
      priv->framesize = oldsize;
      goto errout;
    }

  priv->fmt = *fmt;

  ret = rk3576_vicap_alloc_buffers(priv);
  if (ret < 0)
    {
      goto errout;
    }

  /* Input / output format and frame geometry */

  rk3576_vicap_putreg(RK3576_CIF_FOR_OFFSET, rk3576_vicap_build_for(fmt));
  rk3576_vicap_putreg(RK3576_CIF_VIR_LINE_WIDTH_OFFSET, priv->stride);
  rk3576_vicap_putreg(RK3576_CIF_SET_SIZE_OFFSET,
                      RK3576_CIF_SET_SIZE(fmt->width, fmt->height));

  /* CSI-2 virtual channel / data type filter, unused on the DVP input */

  regval = 0;
  if (fmt->input == RK3576_VICAP_INPUT_CSI2)
    {
      regval = ((uint32_t)fmt->vc << RK3576_CIF_MULTI_ID_VC_SHIFT) &
               RK3576_CIF_MULTI_ID_VC_MASK;
      if (fmt->datatype != 0)
        {
          regval |=
              (((uint32_t)fmt->datatype << RK3576_CIF_MULTI_ID_DT_SHIFT) &
               RK3576_CIF_MULTI_ID_DT_MASK) |
              RK3576_CIF_MULTI_ID_EN;
        }
    }

  rk3576_vicap_putreg(RK3576_CIF_MULTI_ID_OFFSET, regval);

  rk3576_vicap_program_buffers(priv);

  priv->configured = true;
  nxmutex_unlock(&priv->lock);

  vinfo("VICAP format %ux%u fourcc 0x%08" PRIx32 " stride %" PRIu32
        " frame %zu bytes\n",
        fmt->width, fmt->height, fmt->pixelformat, priv->stride,
        priv->framesize);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_vicap_get_framesize
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

size_t rk3576_vicap_get_framesize(void)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;

  return priv->configured ? priv->framesize : 0;
}

/****************************************************************************
 * Name: rk3576_vicap_start_streaming
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

int rk3576_vicap_start_streaming(rk3576_vicap_frame_cb_t callback, void *arg)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
  uint32_t regval;
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
  priv->next = 0;
  priv->seq = 0;

  /* Both buffers are about to be written by a bus master.  Clean any
   * dirty lines so that a later eviction cannot corrupt captured data.
   */

  up_clean_dcache((uintptr_t)priv->buf[0],
                  (uintptr_t)priv->buf[0] + priv->framesize);
  up_clean_dcache((uintptr_t)priv->buf[1],
                  (uintptr_t)priv->buf[1] + priv->framesize);

  rk3576_vicap_program_buffers(priv);

  rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, 0xffffffff);
  rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, RK3576_CIF_INT_FRAME_END |
                                                   RK3576_CIF_INT_LINE_ERR |
                                                   RK3576_CIF_INT_BUS_ERR);

  regval = RK3576_CIF_CTRL_MODE_PINGPONG | RK3576_CIF_CTRL_AXI_BURST_16 |
           RK3576_CIF_CTRL_ENABLE_CAPTURE;

  if (priv->fmt.input == RK3576_VICAP_INPUT_CSI2)
    {
      regval |= RK3576_CIF_CTRL_MIPI_MODE;
    }

  priv->streaming = true;
  rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, regval);

  nxmutex_unlock(&priv->lock);
  return OK;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_vicap_stop_streaming
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

int rk3576_vicap_stop_streaming(void)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->streaming)
    {
      priv->streaming = false;

      rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, 0);
      rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, 0);
      rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, 0xffffffff);

      priv->callback = NULL;
      priv->cbarg = NULL;
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

#ifdef CONFIG_RK3576_VICAP_VIDEO

/****************************************************************************
 * NuttX video stack (struct imgdata_s) adaptor.
 *
 * The video stack allocates the frame buffers itself and hands them over
 * one at a time, so the capture is driven in single-shot mode: set_buf()
 * points FRM0 at the buffer, start_capture() enables the DMA for exactly
 * one frame, and the frame-end interrupt reports it back.  The internal
 * ping-pong buffers are not used in this mode.
 ****************************************************************************/

static int rk3576_vicap_data_init(struct imgdata_s *data);
static int rk3576_vicap_data_uninit(struct imgdata_s *data);
static int rk3576_vicap_data_setbuf(struct imgdata_s *data, uint8_t *addr,
                                    uint32_t size);
static int rk3576_vicap_data_validate(struct imgdata_s *data,
                                      uint8_t nr_datafmt,
                                      imgdata_format_t *datafmt,
                                      imgdata_interval_t *interval);
static int rk3576_vicap_data_start(struct imgdata_s *data, uint8_t nr_datafmt,
                                   imgdata_format_t *datafmt,
                                   imgdata_interval_t *interval,
                                   imgdata_capture_t callback, void *arg);
static int rk3576_vicap_data_stop(struct imgdata_s *data);

static const struct imgdata_ops_s g_rk3576_vicap_dataops = {
  .init = rk3576_vicap_data_init,
  .uninit = rk3576_vicap_data_uninit,
  .set_buf = rk3576_vicap_data_setbuf,
  .validate_frame_setting = rk3576_vicap_data_validate,
  .start_capture = rk3576_vicap_data_start,
  .stop_capture = rk3576_vicap_data_stop,
};

/****************************************************************************
 * Name: rk3576_vicap_data_fmt
 *
 * Description:
 *   Fill a native format descriptor from a video-stack format request.
 *   Only the still/video main format (index 0) is honoured; the RK3576
 *   bypass path has no second output stream.
 *
 ****************************************************************************/

static void rk3576_vicap_data_fmt(struct rk3576_vicap_format_s *out,
                                  const imgdata_format_t *datafmt)
{
  memset(out, 0, sizeof(*out));

  out->width = datafmt->width;
  out->height = datafmt->height;
  out->pixelformat = datafmt->pixelformat;
  out->input = CONFIG_RK3576_VICAP_VIDEO_INPUT;
  out->vsync_active_high = true;
}

static int rk3576_vicap_data_init(struct imgdata_s *data)
{
  UNUSED(data);

  return rk3576_vicap_initialize();
}

static int rk3576_vicap_data_uninit(struct imgdata_s *data)
{
  UNUSED(data);

  return rk3576_vicap_stop_streaming();
}

static int rk3576_vicap_data_setbuf(struct imgdata_s *data, uint8_t *addr,
                                    uint32_t size)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
  uintptr_t phys;

  UNUSED(data);

  if (addr == NULL || size == 0 ||
      (priv->framesize != 0 && size < priv->framesize))
    {
      return -EINVAL;
    }

  priv->extbuf = true;
  priv->extaddr = (uintptr_t)addr;
  priv->extsize = size;

  /* The buffer is about to be filled by a bus master. */

  up_clean_dcache(priv->extaddr, priv->extaddr + size);

  phys = up_addrenv_va_to_pa(addr);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_Y_OFFSET, (uint32_t)phys);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_UV_OFFSET,
                      (uint32_t)(phys + priv->ysize));

  return OK;
}

static int rk3576_vicap_data_validate(struct imgdata_s *data,
                                      uint8_t nr_datafmt,
                                      imgdata_format_t *datafmt,
                                      imgdata_interval_t *interval)
{
  struct rk3576_vicap_dev_s probe;
  struct rk3576_vicap_format_s fmt;

  UNUSED(data);
  UNUSED(interval);

  if (nr_datafmt == 0 || datafmt == NULL)
    {
      return -EINVAL;
    }

  rk3576_vicap_data_fmt(&fmt, &datafmt[0]);

  /* Geometry validation only — probe is a scratch descriptor so that the
   * live device state is untouched by a rejected request.
   */

  memset(&probe, 0, sizeof(probe));
  return rk3576_vicap_calc_geometry(&probe, &fmt);
}

static int rk3576_vicap_data_start(struct imgdata_s *data, uint8_t nr_datafmt,
                                   imgdata_format_t *datafmt,
                                   imgdata_interval_t *interval,
                                   imgdata_capture_t callback, void *arg)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;
  struct rk3576_vicap_format_s fmt;
  uintptr_t phys;
  uint32_t regval;
  int ret;

  UNUSED(data);
  UNUSED(interval);

  if (nr_datafmt == 0 || datafmt == NULL || callback == NULL)
    {
      return -EINVAL;
    }

  if (priv->streaming)
    {
      return -EBUSY;
    }

  rk3576_vicap_data_fmt(&fmt, &datafmt[0]);

  ret = rk3576_vicap_calc_geometry(priv, &fmt);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->extaddr == 0 || priv->extsize < priv->framesize)
    {
      verr("ERROR: no buffer queued for a %zu-byte frame\n", priv->framesize);
      return -EINVAL;
    }

  priv->fmt = fmt;
  priv->datacb = callback;
  priv->dataarg = arg;

  /* Re-point the frame base registers now that the plane split is known. */

  phys = up_addrenv_va_to_pa((void *)priv->extaddr);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_Y_OFFSET, (uint32_t)phys);
  rk3576_vicap_putreg(RK3576_CIF_FRM0_ADDR_UV_OFFSET,
                      (uint32_t)(phys + priv->ysize));

  rk3576_vicap_putreg(RK3576_CIF_FOR_OFFSET, rk3576_vicap_build_for(&fmt));
  rk3576_vicap_putreg(RK3576_CIF_VIR_LINE_WIDTH_OFFSET, priv->stride);
  rk3576_vicap_putreg(RK3576_CIF_SET_SIZE_OFFSET,
                      RK3576_CIF_SET_SIZE(fmt.width, fmt.height));
  rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, 0xffffffff);
  rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, RK3576_CIF_INT_FRAME_END |
                                                   RK3576_CIF_INT_LINE_ERR |
                                                   RK3576_CIF_INT_BUS_ERR);

  regval = RK3576_CIF_CTRL_MODE_ONEFRAME | RK3576_CIF_CTRL_AXI_BURST_16 |
           RK3576_CIF_CTRL_ENABLE_CAPTURE;

  if (fmt.input == RK3576_VICAP_INPUT_CSI2)
    {
      regval |= RK3576_CIF_CTRL_MIPI_MODE;
    }

  priv->streaming = true;
  rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, regval);

  return OK;
}

static int rk3576_vicap_data_stop(struct imgdata_s *data)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;

  UNUSED(data);

  priv->streaming = false;
  rk3576_vicap_putreg(RK3576_CIF_INTEN_OFFSET, 0);
  rk3576_vicap_putreg(RK3576_CIF_CTRL_OFFSET, 0);
  rk3576_vicap_putreg(RK3576_CIF_INTSTAT_OFFSET, 0xffffffff);

  priv->datacb = NULL;
  priv->dataarg = NULL;
  return OK;
}

/****************************************************************************
 * Name: rk3576_vicap_imgdata_get
 *
 * Description:
 *   See rk3576_vicap.h.
 *
 ****************************************************************************/

struct imgdata_s *rk3576_vicap_imgdata_get(void)
{
  struct rk3576_vicap_dev_s *priv = &g_rk3576_vicap;

  priv->data.ops = &g_rk3576_vicap_dataops;
  return &priv->data;
}

#endif /* CONFIG_RK3576_VICAP_VIDEO */

#endif /* CONFIG_RK3576_VICAP */
