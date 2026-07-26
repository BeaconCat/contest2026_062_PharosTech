/****************************************************************************
 * chips/rk3576/rk3576_pdm.c
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
 * RK3576 PDM (pulse-density-modulation) microphone-array capture driver.
 *
 * The block drives a PDM bit clock out to an external MEMS microphone
 * array, samples the returned 1-bit streams on both clock edges (one data
 * line carries two microphones) and performs the PDM to PCM decimation in
 * hardware.  The decimated PCM samples appear in a 32-bit wide receive
 * FIFO which is drained by the PL330 through its "rx" peripheral request
 * line.
 *
 * The driver implements the receive half of the standard NuttX I2S
 * interface (include/nuttx/audio/i2s.h).  The transmit methods are stubbed
 * out because the PDM block has no playback path.  The default
 * configuration is the one the product needs for voice wake-word capture:
 * 16 kHz, 16-bit, two microphones on data line 0.
 *
 * Capture data is DMA'd into a bounce buffer taken from the DMA-safe heap
 * (rk3576_dma_alloc(), physical address < 4 GB as required by the PL330
 * 32-bit SAR/DAR) and then copied into the caller's audio pipeline buffer,
 * whose backing store has no such guarantee.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/audio/audio.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/dma/dma.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_internal.h"
#include "hardware/rk3576_pdm.h"
#include "rk3576_dma.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_pdm.h"

#ifdef CONFIG_RK3576_PDM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Default capture format: 16 kHz / 16-bit / two microphones.  This is what
 * the on-board wake-word engine consumes.
 */

#define RK3576_PDM_DEFAULT_RATE     16000
#define RK3576_PDM_DEFAULT_WIDTH    16
#define RK3576_PDM_DEFAULT_CHANNELS 2

/* Over-sampling ratio of the CIC decimator, in PDM bit-clock cycles per PCM
 * sample.  128 is the highest ratio the block supports and gives the best
 * SNR at the low voice sample rates used here.
 */

#define RK3576_PDM_OSR 128

/* The PDM bit clock is derived from the "pdm_clk_out" root clock by a fixed
 * fractional divider, either 4.0 or 3.5 (PDM_CLK_CTRL_FD_RATIO_*).  The
 * driver always programs the integral 4.0 setting so that the root-clock
 * arithmetic stays exact.
 */

#define RK3576_PDM_FD_RATIO 4

/* Largest relative error, in parts per thousand, tolerated between the
 * requested sample rate and the rate the clock tree can actually produce.
 */

#define RK3576_PDM_RATE_TOLERANCE_PPT 20

/* The receive FIFO is 32 bits wide; every DMA beat moves one word. */

#define RK3576_PDM_FIFO_WIDTH 4

/* Reset (RX_CLR) is self-clearing.  Bound the poll so a dead block cannot
 * hang the caller.
 */

#define RK3576_PDM_RESET_RETRIES 1000

/* Time to wait for one DMA-backed capture to complete, expressed as a
 * multiple of the nominal duration of the buffer.  A capture that takes
 * more than this is treated as a hardware failure.
 */

#define RK3576_PDM_TIMEOUT_FACTOR 4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Immutable per-controller description. */

struct rk3576_pdm_desc_s
{
  uintptr_t base;          /* Register base address                     */
  int irq;                 /* GIC interrupt number                      */
  const char *hclk_name;   /* AHB bus clock gate                        */
  const char *clk_name;    /* Controller functional clock gate          */
  const char *clkout_name; /* PDM bit-clock root ("pdm_clk_out")        */
};

/* One PDM controller instance.  The I2S device must be the first member so
 * that a struct i2s_dev_s pointer can be up-cast to this structure.
 */

struct rk3576_pdm_s
{
  struct i2s_dev_s dev;                 /* Must be first               */
  const struct rk3576_pdm_desc_s *desc; /* Static description          */
  mutex_t lock;                         /* Serialises capture requests */
  sem_t done;                           /* Signalled by the DMA callback */
  struct dma_dev_s *dmadev;             /* PL330 controller            */
  struct dma_chan_s *dmach;             /* Receive channel             */
  struct clk_s *clkout;                 /* Cached PDM bit-clock root   */
  void *dmabuf;                         /* DMA-safe bounce buffer      */
  size_t dmabuflen;                     /* Size of the bounce buffer   */
  uint32_t samplerate;                  /* Configured sample rate      */
  uint8_t datawidth;                    /* Configured sample width     */
  uint8_t nchannels;                    /* Configured channel count    */
  int result;                           /* Result of the last transfer */
  bool clk_enabled;                     /* Clock gates already enabled */
  bool started;                         /* Hardware currently running  */
  bool configured;                      /* Format applied to hardware  */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_pdm_getreg(struct rk3576_pdm_s *priv, unsigned int off);
static void rk3576_pdm_putreg(struct rk3576_pdm_s *priv, unsigned int off,
                              uint32_t val);
static void rk3576_pdm_modifyreg(struct rk3576_pdm_s *priv, unsigned int off,
                                 uint32_t clrbits, uint32_t setbits);

static int rk3576_pdm_clk_init(struct rk3576_pdm_s *priv, uint32_t rate);
static int rk3576_pdm_reset(struct rk3576_pdm_s *priv);
static int rk3576_pdm_configure(struct rk3576_pdm_s *priv);
static void rk3576_pdm_hw_start(struct rk3576_pdm_s *priv);
static void rk3576_pdm_hw_stop(struct rk3576_pdm_s *priv);
static int rk3576_pdm_interrupt(int irq, void *context, void *arg);
static void rk3576_pdm_dma_callback(struct dma_chan_s *chan, void *arg,
                                    ssize_t result);
static int rk3576_pdm_bounce_buffer(struct rk3576_pdm_s *priv, size_t len);
static int rk3576_pdm_capture(struct rk3576_pdm_s *priv, size_t len);

static uint32_t rk3576_pdm_rxchannels(struct i2s_dev_s *dev, uint8_t channels);
static uint32_t rk3576_pdm_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t rk3576_pdm_rxdatawidth(struct i2s_dev_s *dev, int bits);
static int rk3576_pdm_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                              i2s_callback_t callback, void *arg,
                              uint32_t timeout);
static uint32_t rk3576_pdm_txchannels(struct i2s_dev_s *dev, uint8_t channels);
static uint32_t rk3576_pdm_txsamplerate(struct i2s_dev_s *dev, uint32_t rate);
static uint32_t rk3576_pdm_txdatawidth(struct i2s_dev_s *dev, int bits);
static int rk3576_pdm_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           i2s_callback_t callback, void *arg,
                           uint32_t timeout);
static int rk3576_pdm_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2s_ops_s g_rk3576_pdm_ops = {
  .i2s_rxchannels = rk3576_pdm_rxchannels,
  .i2s_rxsamplerate = rk3576_pdm_rxsamplerate,
  .i2s_rxdatawidth = rk3576_pdm_rxdatawidth,
  .i2s_receive = rk3576_pdm_receive,
  .i2s_txchannels = rk3576_pdm_txchannels,
  .i2s_txsamplerate = rk3576_pdm_txsamplerate,
  .i2s_txdatawidth = rk3576_pdm_txdatawidth,
  .i2s_send = rk3576_pdm_send,
  .i2s_ioctl = rk3576_pdm_ioctl,
};

/* Static per-controller description.  Clock names follow the RK3576 clock
 * tree convention and match the "pdm_hclk" / "pdm_clk" / "pdm_clk_out"
 * clock-names of the vendor device tree nodes.
 */

static const struct rk3576_pdm_desc_s g_rk3576_pdm_desc[RK3576_PDM_NCTRL] = {
  {
      .base = RK3576_PDM0_ADDR,
      .irq = RK3576_IRQ_PDM0,
      .hclk_name = "hclk_pdm0_en",
      .clk_name = "clk_pdm0_en",
      .clkout_name = "clk_pdm0_out_en",
  },
  {
      .base = RK3576_PDM1_ADDR,
      .irq = RK3576_IRQ_PDM1,
      .hclk_name = "hclk_pdm1_en",
      .clk_name = "clk_pdm1_en",
      .clkout_name = "clk_pdm1_out_en",
  },
};

static struct rk3576_pdm_s g_rk3576_pdm[RK3576_PDM_NCTRL];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pdm_getreg
 *
 * Description:
 *   Read one controller register.
 ****************************************************************************/

static uint32_t rk3576_pdm_getreg(struct rk3576_pdm_s *priv, unsigned int off)
{
  return getreg32(priv->desc->base + off);
}

/****************************************************************************
 * Name: rk3576_pdm_putreg
 *
 * Description:
 *   Write one controller register.
 ****************************************************************************/

static void rk3576_pdm_putreg(struct rk3576_pdm_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->desc->base + off);
}

/****************************************************************************
 * Name: rk3576_pdm_modifyreg
 *
 * Description:
 *   Read-modify-write one controller register.  The PDM block uses plain
 *   read/write registers (no hiword write mask), so the read-modify-write
 *   has to be done in software.
 ****************************************************************************/

static void rk3576_pdm_modifyreg(struct rk3576_pdm_s *priv, unsigned int off,
                                 uint32_t clrbits, uint32_t setbits)
{
  uint32_t regval = rk3576_pdm_getreg(priv, off);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_pdm_putreg(priv, off, regval);
}

/****************************************************************************
 * Name: rk3576_pdm_clk_init
 *
 * Description:
 *   Single point of contact with the NuttX CLK framework for this driver:
 *   enable the bus and functional clock gates, request the PDM bit-clock
 *   root rate needed for the given PCM sample rate, and cache the rate the
 *   tree actually produced.
 *
 *   The bit clock seen by the microphones is
 *
 *     f_bit = clkout / fd_ratio,  f_bit = rate * OSR
 *
 *   with fd_ratio fixed at 4.0 by the register default, so the root clock
 *   must run at rate * OSR * 4.
 *
 * Input Parameters:
 *   priv - Controller instance
 *   rate - Requested PCM sample rate in Hz
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 ****************************************************************************/

static int rk3576_pdm_clk_init(struct rk3576_pdm_s *priv, uint32_t rate)
{
  struct clk_s *hclk;
  struct clk_s *fclk;
  struct clk_s *clkout;
  uint32_t wanted;
  uint32_t actual;
  uint32_t produced;
  uint32_t error;
  int ret;

  if (priv->clk_enabled)
    {
      /* Gates are already on; only the bit-clock rate has to be revisited
       * for the new sample rate.
       */

      clkout = priv->clkout;
      goto set_rate;
    }

  /* AHB bus interface clock */

  hclk = clk_get(priv->desc->hclk_name);
  if (hclk == NULL)
    {
      auderr("ERROR: failed to get %s\n", priv->desc->hclk_name);
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      auderr("ERROR: failed to enable %s: %d\n", priv->desc->hclk_name, ret);
      return ret;
    }

  /* Controller functional clock */

  fclk = clk_get(priv->desc->clk_name);
  if (fclk == NULL)
    {
      auderr("ERROR: failed to get %s\n", priv->desc->clk_name);
      return -ENODEV;
    }

  ret = clk_enable(fclk);
  if (ret < 0)
    {
      auderr("ERROR: failed to enable %s: %d\n", priv->desc->clk_name, ret);
      return ret;
    }

  /* PDM bit-clock root */

  clkout = clk_get(priv->desc->clkout_name);
  if (clkout == NULL)
    {
      auderr("ERROR: failed to get %s\n", priv->desc->clkout_name);
      return -ENODEV;
    }

  ret = clk_enable(clkout);
  if (ret < 0)
    {
      auderr("ERROR: failed to enable %s: %d\n", priv->desc->clkout_name, ret);
      return ret;
    }

  priv->clkout = clkout;
  priv->clk_enabled = true;

set_rate:
  wanted = rate * RK3576_PDM_OSR * RK3576_PDM_FD_RATIO;

  ret = clk_set_rate(clkout, wanted);
  if (ret < 0)
    {
      auderr("ERROR: failed to set %s to %" PRIu32 " Hz: %d\n",
             priv->desc->clkout_name, wanted, ret);
      return ret;
    }

  /* Never assume the requested rate was granted - read it back. */

  actual = clk_get_rate(clkout);
  if (actual == 0)
    {
      auderr("ERROR: %s reports a zero rate\n", priv->desc->clkout_name);
      return -EIO;
    }

  produced = actual / (RK3576_PDM_FD_RATIO * RK3576_PDM_OSR);
  error = produced > rate ? produced - rate : rate - produced;

  if ((uint64_t)error * 1000 > (uint64_t)rate * RK3576_PDM_RATE_TOLERANCE_PPT)
    {
      auderr("ERROR: %s = %" PRIu32 " Hz yields %" PRIu32 " Hz, "
             "wanted %" PRIu32 " Hz\n",
             priv->desc->clkout_name, actual, produced, rate);
      return -ERANGE;
    }

  audinfo("PDM clkout %" PRIu32 " Hz -> %" PRIu32 " Hz sample rate\n", actual,
          produced);
  return OK;
}

/****************************************************************************
 * Name: rk3576_pdm_reset
 *
 * Description:
 *   Pulse the self-clearing receive-path reset and wait for it to complete.
 *
 * Returned Value:
 *   OK on success, -ETIMEDOUT if the bit never clears.
 ****************************************************************************/

static int rk3576_pdm_reset(struct rk3576_pdm_s *priv)
{
  int retries;

  rk3576_pdm_modifyreg(priv, RK3576_PDM_SYSCONFIG, PDM_SYSCONFIG_RX_START,
                       PDM_SYSCONFIG_RX_CLR);

  for (retries = 0; retries < RK3576_PDM_RESET_RETRIES; retries++)
    {
      if ((rk3576_pdm_getreg(priv, RK3576_PDM_SYSCONFIG) &
           PDM_SYSCONFIG_RX_CLR) == 0)
        {
          return OK;
        }

      up_udelay(1);
    }

  auderr("ERROR: receive path reset did not complete\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_pdm_configure
 *
 * Description:
 *   Apply the cached format (sample rate, sample width, channel count) to
 *   the hardware and leave the block reset and idle, ready to be started.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 ****************************************************************************/

static int rk3576_pdm_configure(struct rk3576_pdm_s *priv)
{
  uint32_t ctrl0;
  uint32_t paths;
  uint32_t valid;
  unsigned int i;
  int ret;

  ret = rk3576_pdm_clk_init(priv, priv->samplerate);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_pdm_reset(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* One data line carries two microphones, so enable ceil(nchannels / 2)
   * paths starting from path 0.
   */

  paths = 0;
  valid = 0;
  for (i = 0;
       i * RK3576_PDM_MICS_PER_PATH < priv->nchannels && i < RK3576_PDM_NPATHS;
       i++)
    {
      paths |= 1u << (PDM_CTRL0_PATH_SHIFT + i);
      valid |= 1u << i;
    }

  ctrl0 = PDM_CTRL0_VDW(priv->datawidth) | PDM_CTRL0_MODE_LJ | paths;
  rk3576_pdm_putreg(priv, RK3576_PDM_CTRL0, ctrl0);
  rk3576_pdm_putreg(priv, RK3576_PDM_DATA_VALID, valid);

  /* CIC decimation filter gain */

  rk3576_pdm_putreg(priv, RK3576_PDM_CTRL1,
                    PDM_CTRL1_FILTER_GAIN(RK3576_PDM_FILTER_GAIN_DEFAULT));

  /* Decimation ratio and bit-clock generation.  The fractional divider is
   * left at 4.0, which is what rk3576_pdm_clk_init() assumed when it
   * requested the root clock rate.
   */

  rk3576_pdm_putreg(priv, RK3576_PDM_CLK_CTRL,
                    PDM_CLK_CTRL_DS_RATIO_128 | PDM_CLK_CTRL_CKP_NORMAL |
                        PDM_CLK_CTRL_FD_RATIO_40 | PDM_CLK_CTRL_CLK_EN);

  /* Remove the microphone DC offset.  60 Hz is below the voice band and
   * well above the MEMS 1/f corner.
   */

  rk3576_pdm_putreg(priv, RK3576_PDM_HPF_CTRL,
                    PDM_HPF_CTRL_CF_60 | PDM_HPF_CTRL_RXL_EN |
                        PDM_HPF_CTRL_RXR_EN);

  /* Raise a DMA request once the FIFO is half full. */

  rk3576_pdm_putreg(priv, RK3576_PDM_DMA_CTRL,
                    PDM_DMA_CTRL_RDL(RK3576_PDM_DMA_RDL));

  /* Only the overrun interrupt is of interest; the data path is driven by
   * DMA requests, not by the FIFO threshold interrupt.
   */

  rk3576_pdm_putreg(priv, RK3576_PDM_INT_CLR, PDM_INT_ALL);
  rk3576_pdm_putreg(priv, RK3576_PDM_INT_EN, PDM_INT_RXOI);

  priv->configured = true;
  return OK;
}

/****************************************************************************
 * Name: rk3576_pdm_hw_start
 *
 * Description:
 *   Enable the receive DMA request and start the receive path.
 ****************************************************************************/

static void rk3576_pdm_hw_start(struct rk3576_pdm_s *priv)
{
  rk3576_pdm_modifyreg(priv, RK3576_PDM_DMA_CTRL, 0, PDM_DMA_CTRL_RDE);
  rk3576_pdm_modifyreg(priv, RK3576_PDM_SYSCONFIG, 0, PDM_SYSCONFIG_RX_START);
  priv->started = true;
}

/****************************************************************************
 * Name: rk3576_pdm_hw_stop
 *
 * Description:
 *   Stop the receive path, drop the DMA request and flush the FIFO so that
 *   the next capture starts on a sample boundary.
 ****************************************************************************/

static void rk3576_pdm_hw_stop(struct rk3576_pdm_s *priv)
{
  rk3576_pdm_modifyreg(priv, RK3576_PDM_SYSCONFIG, PDM_SYSCONFIG_RX_START, 0);
  rk3576_pdm_modifyreg(priv, RK3576_PDM_DMA_CTRL, PDM_DMA_CTRL_RDE, 0);
  rk3576_pdm_putreg(priv, RK3576_PDM_INT_CLR, PDM_INT_ALL);
  rk3576_pdm_reset(priv);
  priv->started = false;
}

/****************************************************************************
 * Name: rk3576_pdm_interrupt
 *
 * Description:
 *   Controller interrupt handler.  Only the receive-FIFO overrun is
 *   enabled; it means the DMA could not keep up, which corrupts the stream,
 *   so the in-flight capture is failed and the caller is woken.
 ****************************************************************************/

static int rk3576_pdm_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_pdm_s *priv = arg;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_pdm_getreg(priv, RK3576_PDM_INT_ST) & PDM_INT_ALL;
  if (status == 0)
    {
      return OK;
    }

  rk3576_pdm_putreg(priv, RK3576_PDM_INT_CLR, status);

  if ((status & PDM_INT_RXOI) != 0 && priv->started)
    {
      auderr("ERROR: receive FIFO overrun\n");
      priv->result = -EIO;
      nxsem_post(&priv->done);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pdm_dma_callback
 *
 * Description:
 *   PL330 completion callback, invoked from interrupt context.  A negative
 *   result is a channel fault; a positive one is the transferred length.
 ****************************************************************************/

static void rk3576_pdm_dma_callback(struct dma_chan_s *chan, void *arg,
                                    ssize_t result)
{
  struct rk3576_pdm_s *priv = arg;

  UNUSED(chan);

  priv->result = result < 0 ? (int)result : OK;
  nxsem_post(&priv->done);
}

/****************************************************************************
 * Name: rk3576_pdm_bounce_buffer
 *
 * Description:
 *   Make sure the DMA-safe bounce buffer is at least len bytes long,
 *   growing it if necessary.
 *
 * Returned Value:
 *   OK on success, -ENOMEM if the DMA heap is exhausted.
 ****************************************************************************/

static int rk3576_pdm_bounce_buffer(struct rk3576_pdm_s *priv, size_t len)
{
  void *buf;

  if (priv->dmabuf != NULL && priv->dmabuflen >= len)
    {
      return OK;
    }

  buf = rk3576_dma_alloc(len);
  if (buf == NULL)
    {
      auderr("ERROR: failed to allocate %zu DMA bytes\n", len);
      return -ENOMEM;
    }

  if (priv->dmabuf != NULL)
    {
      rk3576_dma_free(priv->dmabuf, priv->dmabuflen);
    }

  priv->dmabuf = buf;
  priv->dmabuflen = len;
  return OK;
}

/****************************************************************************
 * Name: rk3576_pdm_capture
 *
 * Description:
 *   Run one DMA-backed capture of len bytes into the bounce buffer and wait
 *   for it to complete.  The caller must hold priv->lock and must have
 *   sized the bounce buffer already.
 *
 * Input Parameters:
 *   priv - Controller instance
 *   len  - Capture length in bytes, a multiple of the FIFO word size
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 ****************************************************************************/

static int rk3576_pdm_capture(struct rk3576_pdm_s *priv, size_t len)
{
  struct dma_config_s cfg;
  uint32_t timeout_ms;
  int ret;

  /* The DMA writes the bounce buffer behind the D-cache's back; drop any
   * stale lines covering it before the transfer starts.
   */

  up_invalidate_dcache((uintptr_t)priv->dmabuf, (uintptr_t)priv->dmabuf + len);

  memset(&cfg, 0, sizeof(cfg));
  cfg.direction = DMA_DEV_TO_MEM;
  cfg.src_drq = RK3576_PDM_DMA_RX_DRQ;
  cfg.src_width = RK3576_PDM_FIFO_WIDTH;
  cfg.dst_width = RK3576_PDM_FIFO_WIDTH;

  ret = DMA_CONFIG(priv->dmach, &cfg);
  if (ret < 0)
    {
      auderr("ERROR: DMA_CONFIG failed: %d\n", ret);
      return ret;
    }

  priv->result = OK;

  ret = DMA_START(priv->dmach, rk3576_pdm_dma_callback, priv,
                  (uintptr_t)priv->dmabuf,
                  priv->desc->base + RK3576_PDM_RXFIFO, len);
  if (ret < 0)
    {
      auderr("ERROR: DMA_START failed: %d\n", ret);
      return ret;
    }

  rk3576_pdm_hw_start(priv);

  /* Nominal duration of the buffer, with generous slack. */

  timeout_ms = (uint32_t)(((uint64_t)len * 1000 * RK3576_PDM_TIMEOUT_FACTOR) /
                          ((uint64_t)priv->samplerate * priv->nchannels *
                           (priv->datawidth / 8))) +
               1;

  ret = nxsem_tickwait_uninterruptible(&priv->done, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      auderr("ERROR: capture timed out after %" PRIu32 " ms\n", timeout_ms);
    }
  else
    {
      ret = priv->result;
    }

  rk3576_pdm_hw_stop(priv);
  DMA_STOP(priv->dmach);

  if (ret >= 0)
    {
      /* The buffer was filled by the DMA engine, not by the CPU. */

      up_invalidate_dcache((uintptr_t)priv->dmabuf,
                           (uintptr_t)priv->dmabuf + len);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_pdm_rxchannels
 *
 * Description:
 *   I2S method: set the number of microphones to capture.  The block pairs
 *   two microphones per data line, so only even counts up to the number of
 *   data lines times two are accepted.
 *
 * Returned Value:
 *   The channel count in effect, or zero if the request was rejected.
 ****************************************************************************/

static uint32_t rk3576_pdm_rxchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  struct rk3576_pdm_s *priv = (struct rk3576_pdm_s *)dev;

  if (channels == 0 || channels > RK3576_PDM_MAX_CHANNELS ||
      (channels % RK3576_PDM_MICS_PER_PATH) != 0)
    {
      auderr("ERROR: unsupported channel count %u\n", channels);
      return 0;
    }

  nxmutex_lock(&priv->lock);
  priv->nchannels = channels;
  priv->configured = false;
  nxmutex_unlock(&priv->lock);

  return channels;
}

/****************************************************************************
 * Name: rk3576_pdm_rxsamplerate
 *
 * Description:
 *   I2S method: set the PCM sample rate.  The rate is validated against the
 *   clock tree when the format is applied at the next capture.
 *
 * Returned Value:
 *   The sample rate in effect, or zero if the request was rejected.
 ****************************************************************************/

static uint32_t rk3576_pdm_rxsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  struct rk3576_pdm_s *priv = (struct rk3576_pdm_s *)dev;

  if (rate == 0)
    {
      return 0;
    }

  nxmutex_lock(&priv->lock);
  priv->samplerate = rate;
  priv->configured = false;
  nxmutex_unlock(&priv->lock);

  return rate;
}

/****************************************************************************
 * Name: rk3576_pdm_rxdatawidth
 *
 * Description:
 *   I2S method: set the PCM sample width in bits.  The decimator emits
 *   right-aligned samples in 32-bit FIFO words; 16, 24 and 32 bits are
 *   supported.
 *
 * Returned Value:
 *   The bits-per-sample in effect, or zero if the request was rejected.
 ****************************************************************************/

static uint32_t rk3576_pdm_rxdatawidth(struct i2s_dev_s *dev, int bits)
{
  struct rk3576_pdm_s *priv = (struct rk3576_pdm_s *)dev;

  if (bits != 16 && bits != 24 && bits != 32)
    {
      auderr("ERROR: unsupported sample width %d\n", bits);
      return 0;
    }

  nxmutex_lock(&priv->lock);
  priv->datawidth = (uint8_t)bits;
  priv->configured = false;
  nxmutex_unlock(&priv->lock);

  return (uint32_t)bits;
}

/****************************************************************************
 * Name: rk3576_pdm_receive
 *
 * Description:
 *   I2S method: capture one audio pipeline buffer.  The transfer is run to
 *   completion before this function returns and the callback, if any, is
 *   invoked with the result; the caller therefore never has to wait for an
 *   out-of-band completion.  This keeps the driver free of a worker thread,
 *   which the PL330 back end needs because it has no cyclic-transfer
 *   support yet.
 *
 * Input Parameters:
 *   dev      - PDM I2S instance
 *   apb      - Buffer to fill; apb->nmaxbytes bounds the capture length
 *   callback - Optional completion callback
 *   arg      - Opaque argument for the callback
 *   timeout  - Unused; the capture is bounded by its own nominal duration
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 ****************************************************************************/

static int rk3576_pdm_receive(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                              i2s_callback_t callback, void *arg,
                              uint32_t timeout)
{
  struct rk3576_pdm_s *priv = (struct rk3576_pdm_s *)dev;
  size_t len;
  int ret;

  UNUSED(timeout);

  if (apb == NULL || apb->samp == NULL)
    {
      return -EINVAL;
    }

  /* The FIFO is drained one 32-bit word at a time, so the capture length
   * has to be a whole number of words.
   */

  len = apb->nmaxbytes & ~(size_t)(RK3576_PDM_FIFO_WIDTH - 1);
  if (len == 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  if (!priv->configured)
    {
      ret = rk3576_pdm_configure(priv);
      if (ret < 0)
        {
          goto errout;
        }
    }

  ret = rk3576_pdm_bounce_buffer(priv, len);
  if (ret < 0)
    {
      goto errout;
    }

  ret = rk3576_pdm_capture(priv, len);
  if (ret < 0)
    {
      goto errout;
    }

  memcpy(apb->samp, priv->dmabuf, len);
  apb->nbytes = (apb_samp_t)len;
  apb->curbyte = 0;

errout:
  nxmutex_unlock(&priv->lock);

  if (callback != NULL)
    {
      callback(dev, apb, arg, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_pdm_txchannels
 *
 * Description:
 *   I2S method: not supported, the PDM block has no transmit path.
 ****************************************************************************/

static uint32_t rk3576_pdm_txchannels(struct i2s_dev_s *dev, uint8_t channels)
{
  UNUSED(dev);
  UNUSED(channels);
  return 0;
}

/****************************************************************************
 * Name: rk3576_pdm_txsamplerate
 *
 * Description:
 *   I2S method: not supported, the PDM block has no transmit path.
 ****************************************************************************/

static uint32_t rk3576_pdm_txsamplerate(struct i2s_dev_s *dev, uint32_t rate)
{
  UNUSED(dev);
  UNUSED(rate);
  return 0;
}

/****************************************************************************
 * Name: rk3576_pdm_txdatawidth
 *
 * Description:
 *   I2S method: not supported, the PDM block has no transmit path.
 ****************************************************************************/

static uint32_t rk3576_pdm_txdatawidth(struct i2s_dev_s *dev, int bits)
{
  UNUSED(dev);
  UNUSED(bits);
  return 0;
}

/****************************************************************************
 * Name: rk3576_pdm_send
 *
 * Description:
 *   I2S method: not supported, the PDM block has no transmit path.
 ****************************************************************************/

static int rk3576_pdm_send(struct i2s_dev_s *dev, struct ap_buffer_s *apb,
                           i2s_callback_t callback, void *arg,
                           uint32_t timeout)
{
  UNUSED(dev);
  UNUSED(apb);
  UNUSED(callback);
  UNUSED(arg);
  UNUSED(timeout);
  return -ENOTSUP;
}

/****************************************************************************
 * Name: rk3576_pdm_ioctl
 *
 * Description:
 *   I2S method: no controller-specific commands are implemented.
 ****************************************************************************/

static int rk3576_pdm_ioctl(struct i2s_dev_s *dev, int cmd, unsigned long arg)
{
  UNUSED(dev);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pdm_initialize
 *
 * Description:
 *   Create the I2S (receive-only) interface for one PDM controller.  See
 *   rk3576_pdm.h for the full description.
 ****************************************************************************/

struct i2s_dev_s *rk3576_pdm_initialize(int controller)
{
  struct rk3576_pdm_s *priv;
  int ret;

  if (controller < 0 || controller >= RK3576_PDM_NCTRL)
    {
      auderr("ERROR: invalid PDM controller %d\n", controller);
      return NULL;
    }

  priv = &g_rk3576_pdm[controller];
  if (priv->dev.ops != NULL)
    {
      /* Already initialized; hand back the same singleton. */

      return &priv->dev;
    }

  priv->desc = &g_rk3576_pdm_desc[controller];
  priv->samplerate = RK3576_PDM_DEFAULT_RATE;
  priv->datawidth = RK3576_PDM_DEFAULT_WIDTH;
  priv->nchannels = RK3576_PDM_DEFAULT_CHANNELS;
  priv->configured = false;
  priv->started = false;

  nxmutex_init(&priv->lock);
  nxsem_init(&priv->done, 0, 0);

  /* Bring the clocks up now so that a bad clock tree is reported at
   * registration time rather than on the first capture.
   */

  ret = rk3576_pdm_clk_init(priv, priv->samplerate);
  if (ret < 0)
    {
      goto errout_with_sem;
    }

  ret = rk3576_pdm_reset(priv);
  if (ret < 0)
    {
      goto errout_with_sem;
    }

  rk3576_pdm_putreg(priv, RK3576_PDM_INT_EN, 0);
  rk3576_pdm_putreg(priv, RK3576_PDM_INT_CLR, PDM_INT_ALL);

  priv->dmadev = rk3576_dma_initialize();
  priv->dmach = DMA_GET_CHAN(priv->dmadev, RK3576_PDM_DMA_RX_DRQ);
  if (priv->dmach == NULL)
    {
      auderr("ERROR: no free DMA channel for PDM%d\n", controller);
      ret = -EBUSY;
      goto errout_with_sem;
    }

  ret = irq_attach(priv->desc->irq, rk3576_pdm_interrupt, priv);
  if (ret < 0)
    {
      auderr("ERROR: failed to attach IRQ %d: %d\n", priv->desc->irq, ret);
      goto errout_with_chan;
    }

  up_enable_irq(priv->desc->irq);

  priv->dev.ops = &g_rk3576_pdm_ops;
  audinfo("PDM%d at 0x%" PRIxPTR " ready\n", controller, priv->desc->base);
  return &priv->dev;

errout_with_chan:
  DMA_PUT_CHAN(priv->dmadev, priv->dmach);
  priv->dmach = NULL;

errout_with_sem:
  nxsem_destroy(&priv->done);
  nxmutex_destroy(&priv->lock);
  return NULL;
}

#endif /* CONFIG_RK3576_PDM */
