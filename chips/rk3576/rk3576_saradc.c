/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_saradc.c
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
 * RK3576 SAR-ADC (Rockchip SARADC v2) lower-half driver.
 *
 * Implements struct adc_ops_s from include/nuttx/analog/adc.h so the NuttX
 * ADC upper half can expose a /dev/adcN character device.  The converter
 * only supports one channel per start-of-conversion, therefore an
 * ANIOC_TRIGGER walks the configured channel mask one channel at a time:
 * the end-of-conversion interrupt pushes the sample into the upper-half
 * FIFO and kicks the next channel of the mask.
 *
 * In addition, rk3576_saradc_read_channel() offers a polled, blocking
 * single conversion for board level users that do not want to go through
 * the character device (head-set key detection on channel 3, for example).
 * Both paths are serialised by the same mutex.
 *
 * The analog inputs are dedicated SARADC_INn balls, so no pin muxing is
 * required.  Clocks are taken from the NuttX CLK framework, which means
 * this driver must be brought up after rk3576_clk_tree_initialize().
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

#include <nuttx/analog/adc.h>
#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_saradc.h"
#include "rk3576_saradc.h"

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clock names as registered by the RK3576 clock tree. */

#define RK3576_SARADC_PCLK_NAME "pclk_saradc_en"
#define RK3576_SARADC_CLK_NAME  "clk_saradc_en"

/* Polled conversion timeout.  A single conversion needs a few tens of
 * clk_saradc cycles (some tens of microseconds at 1 MHz); one millisecond
 * of polling in 10 us steps is two orders of magnitude of head room.
 */

#define RK3576_SARADC_POLL_STEP_US 10
#define RK3576_SARADC_POLL_STEPS   100

/* Mask of all implemented channels. */

#define RK3576_SARADC_CHAN_ALL ((1u << RK3576_SARADC_NCHANNELS) - 1)

/* Nanoseconds per second, used when converting a timing requirement
 * expressed in nanoseconds into clk_saradc cycles.
 */

#define RK3576_SARADC_NSEC_PER_SEC 1000000000ull

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_saradc_priv_s
{
  const struct adc_callback_s *cb; /* Upper-half callback, set by bind    */
  mutex_t lock;                    /* Serialises access to the converter  */
  uint32_t chanmask;               /* Channels sampled by ANIOC_TRIGGER   */
  uint32_t pending;                /* Channels left in the running scan   */
  uint32_t clk_hz;                 /* Real clk_saradc rate                */
  uint8_t current;                 /* Channel currently converting        */
  bool hw_ready;                   /* Clocks up, timing programmed        */
  bool irq_attached;               /* Interrupt handler installed         */
  bool rxenabled;                  /* Interrupt driven mode requested     */
  bool scanning;                   /* An interrupt driven scan is running */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_saradc_getreg(unsigned int off);
static void rk3576_saradc_putreg(unsigned int off, uint32_t val);

static int rk3576_saradc_clk_init(struct rk3576_saradc_priv_s *priv);
static uint32_t rk3576_saradc_ns2cycles(uint32_t clk_hz, uint32_t ns);
static void rk3576_saradc_timing(struct rk3576_saradc_priv_s *priv);
static int rk3576_saradc_hwinit(struct rk3576_saradc_priv_s *priv);

static void rk3576_saradc_start(uint8_t ch);
static void rk3576_saradc_stop(void);
static int rk3576_saradc_interrupt(int irq, void *context, void *arg);

static int rk3576_saradc_bind(struct adc_dev_s *dev,
                              const struct adc_callback_s *callback);
static void rk3576_saradc_reset(struct adc_dev_s *dev);
static int rk3576_saradc_setup(struct adc_dev_s *dev);
static void rk3576_saradc_shutdown(struct adc_dev_s *dev);
static void rk3576_saradc_rxint(struct adc_dev_s *dev, bool enable);
static int rk3576_saradc_ioctl(struct adc_dev_s *dev, int cmd,
                               unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct adc_ops_s g_rk3576_saradc_ops =
{
  .ao_bind     = rk3576_saradc_bind,
  .ao_reset    = rk3576_saradc_reset,
  .ao_setup    = rk3576_saradc_setup,
  .ao_shutdown = rk3576_saradc_shutdown,
  .ao_rxint    = rk3576_saradc_rxint,
  .ao_ioctl    = rk3576_saradc_ioctl,
};

/* There is exactly one SARADC instance on RK3576. */

static struct rk3576_saradc_priv_s g_rk3576_saradc_priv =
{
  .lock = NXMUTEX_INITIALIZER,
};

static struct adc_dev_s g_rk3576_saradc_dev =
{
  .ad_ops  = &g_rk3576_saradc_ops,
  .ad_priv = &g_rk3576_saradc_priv,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_saradc_getreg / rk3576_saradc_putreg
 *
 * Description:
 *   Register accessors for the single SARADC instance.
 *
 ****************************************************************************/

static uint32_t rk3576_saradc_getreg(unsigned int off)
{
  return getreg32(RK3576_SARADC_ADDR + off);
}

static void rk3576_saradc_putreg(unsigned int off, uint32_t val)
{
  putreg32(val, RK3576_SARADC_ADDR + off);
}

/****************************************************************************
 * Name: rk3576_saradc_clk_init
 *
 * Description:
 *   Single point of contact with the NuttX CLK framework: enables the APB
 *   bus clock and the converter clock and records the real converter rate.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_saradc_clk_init(struct rk3576_saradc_priv_s *priv)
{
  struct clk_s *pclk;
  struct clk_s *fclk;
  int ret;

  /* APB bus clock */

  pclk = clk_get(RK3576_SARADC_PCLK_NAME);
  if (pclk == NULL)
    {
      aerr("ERROR: failed to get %s\n", RK3576_SARADC_PCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable %s: %d\n", RK3576_SARADC_PCLK_NAME, ret);
      return ret;
    }

  /* Converter clock.  Ask for the nominal rate but keep working with
   * whatever the clock tree can actually deliver.
   */

  fclk = clk_get(RK3576_SARADC_CLK_NAME);
  if (fclk == NULL)
    {
      aerr("ERROR: failed to get %s\n", RK3576_SARADC_CLK_NAME);
      clk_disable(pclk);
      return -ENODEV;
    }

  ret = clk_set_rate(fclk, RK3576_SARADC_CLK_HZ);
  if (ret < 0)
    {
      awarn("WARNING: cannot set %s to %d Hz: %d\n", RK3576_SARADC_CLK_NAME,
            RK3576_SARADC_CLK_HZ, ret);
    }

  ret = clk_enable(fclk);
  if (ret < 0)
    {
      aerr("ERROR: failed to enable %s: %d\n", RK3576_SARADC_CLK_NAME, ret);
      clk_disable(pclk);
      return ret;
    }

  priv->clk_hz = clk_get_rate(fclk);
  if (priv->clk_hz == 0)
    {
      aerr("ERROR: %s reports a zero rate\n", RK3576_SARADC_CLK_NAME);
      clk_disable(fclk);
      clk_disable(pclk);
      return -EINVAL;
    }

  ainfo("clk_saradc running at %" PRIu32 " Hz\n", priv->clk_hz);
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_ns2cycles
 *
 * Description:
 *   Convert a timing requirement given in nanoseconds into a number of
 *   clk_saradc cycles, rounded up and clamped to at least one cycle.
 *
 ****************************************************************************/

static uint32_t rk3576_saradc_ns2cycles(uint32_t clk_hz, uint32_t ns)
{
  uint64_t cycles;

  cycles = ((uint64_t)clk_hz * (uint64_t)ns +
            RK3576_SARADC_NSEC_PER_SEC - 1) /
           RK3576_SARADC_NSEC_PER_SEC;

  if (cycles == 0)
    {
      cycles = 1;
    }

  return (uint32_t)cycles;
}

/****************************************************************************
 * Name: rk3576_saradc_timing
 *
 * Description:
 *   Program the analog timing registers from the real converter clock.
 *
 ****************************************************************************/

static void rk3576_saradc_timing(struct rk3576_saradc_priv_s *priv)
{
  uint32_t hz = priv->clk_hz;

  rk3576_saradc_putreg(RK3576_SARADC_T_PD_SOC,
                       rk3576_saradc_ns2cycles(hz, SARADC_T_PD_SOC_NS));
  rk3576_saradc_putreg(RK3576_SARADC_T_AS_SOC,
                       rk3576_saradc_ns2cycles(hz, SARADC_T_AS_SOC_NS));
  rk3576_saradc_putreg(RK3576_SARADC_T_DAS_SOC,
                       rk3576_saradc_ns2cycles(hz, SARADC_T_DAS_SOC_NS));
  rk3576_saradc_putreg(RK3576_SARADC_T_SEL_SOC,
                       rk3576_saradc_ns2cycles(hz, SARADC_T_SEL_SOC_NS));
}

/****************************************************************************
 * Name: rk3576_saradc_hwinit
 *
 * Description:
 *   Bring the converter up once: clocks, analog trim, timing, and a clean
 *   interrupt state.  Idempotent.
 *
 ****************************************************************************/

static int rk3576_saradc_hwinit(struct rk3576_saradc_priv_s *priv)
{
  int ret;

  if (priv->hw_ready)
    {
      return OK;
    }

  ret = rk3576_saradc_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* Restore the documented analog trim and the converter timing. */

  rk3576_saradc_putreg(RK3576_SARADC_ST_CON, SARADC_ST_CON_DEFAULT);
  rk3576_saradc_timing(priv);

  /* Make sure no conversion and no interrupt survives a warm restart. */

  rk3576_saradc_stop();

  priv->hw_ready = true;
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_start
 *
 * Description:
 *   Kick a single-shot conversion on channel 'ch'.  Any pending
 *   end-of-conversion status is cleared first so the interrupt that
 *   follows belongs to this conversion.
 *
 ****************************************************************************/

static void rk3576_saradc_start(uint8_t ch)
{
  uint32_t con;

  rk3576_saradc_putreg(RK3576_SARADC_END_INT_ST, SARADC_END_INT_ST_ST);

  con = ((uint32_t)ch << SARADC_CONV_CON_CHANNEL_SEL_SHIFT) &
        SARADC_CONV_CON_CHANNEL_SEL_MASK;
  con |= SARADC_CONV_CON_START | SARADC_CONV_CON_SINGLE_MODE;

  rk3576_saradc_putreg(RK3576_SARADC_CONV_CON,
                       RK3576_SARADC_HIWORD_ALL(con));
}

/****************************************************************************
 * Name: rk3576_saradc_stop
 *
 * Description:
 *   Drop the start bit, disable the end-of-conversion interrupt and clear
 *   its status.
 *
 ****************************************************************************/

static void rk3576_saradc_stop(void)
{
  rk3576_saradc_putreg(RK3576_SARADC_END_INT_EN,
                       RK3576_SARADC_HIWORD_CLR(SARADC_END_INT_EN_EN));
  rk3576_saradc_putreg(RK3576_SARADC_CONV_CON,
                       RK3576_SARADC_HIWORD_CLR(
                         SARADC_CONV_CON_START |
                         SARADC_CONV_CON_SINGLE_MODE));
  rk3576_saradc_putreg(RK3576_SARADC_END_INT_ST, SARADC_END_INT_ST_ST);
}

/****************************************************************************
 * Name: rk3576_saradc_interrupt
 *
 * Description:
 *   End-of-conversion interrupt handler.  Publishes the sample to the
 *   upper half and advances to the next channel of the running scan.
 *
 ****************************************************************************/

static int rk3576_saradc_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_saradc_priv_s *priv =
    (struct rk3576_saradc_priv_s *)arg;
  uint32_t sample;
  uint8_t ch;

  UNUSED(irq);
  UNUSED(context);

  if ((rk3576_saradc_getreg(RK3576_SARADC_END_INT_ST) &
       SARADC_END_INT_ST_ST) == 0)
    {
      return OK;
    }

  /* Acknowledge before reading on, the data register is latched. */

  rk3576_saradc_putreg(RK3576_SARADC_END_INT_ST, SARADC_END_INT_ST_ST);

  if (!priv->scanning)
    {
      /* Stray interrupt, for example the tail of a polled conversion. */

      return OK;
    }

  ch = priv->current;
  sample = rk3576_saradc_getreg(RK3576_SARADC_DATA(ch)) &
           RK3576_SARADC_DATA_MASK;

  if (priv->cb != NULL && priv->cb->au_receive != NULL)
    {
      priv->cb->au_receive(&g_rk3576_saradc_dev, ch, (int32_t)sample);
    }

  /* Advance to the next channel of this scan, if any. */

  priv->pending &= ~(1u << ch);
  if (priv->pending != 0)
    {
      for (ch = 0; ch < RK3576_SARADC_NCHANNELS; ch++)
        {
          if ((priv->pending & (1u << ch)) != 0)
            {
              priv->current = ch;
              rk3576_saradc_start(ch);
              return OK;
            }
        }
    }

  /* Scan complete. */

  priv->scanning = false;
  rk3576_saradc_putreg(RK3576_SARADC_END_INT_EN,
                       RK3576_SARADC_HIWORD_CLR(SARADC_END_INT_EN_EN));
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_bind
 *
 * Description:
 *   Bind the upper-half callbacks to this lower half.
 *
 ****************************************************************************/

static int rk3576_saradc_bind(struct adc_dev_s *dev,
                              const struct adc_callback_s *callback)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;

  DEBUGASSERT(priv != NULL);
  priv->cb = callback;
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_reset
 *
 * Description:
 *   Return the converter to its idle state: no conversion running, no
 *   interrupt enabled, documented analog trim and timing.
 *
 ****************************************************************************/

static void rk3576_saradc_reset(struct adc_dev_s *dev)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;
  irqstate_t flags;

  if (!priv->hw_ready)
    {
      return;
    }

  flags = enter_critical_section();

  rk3576_saradc_stop();
  rk3576_saradc_putreg(RK3576_SARADC_ST_CON, SARADC_ST_CON_DEFAULT);
  rk3576_saradc_timing(priv);

  priv->scanning = false;
  priv->pending = 0;

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rk3576_saradc_setup
 *
 * Description:
 *   Called when the character device is first opened: make sure the
 *   hardware is up and attach the end-of-conversion interrupt.
 *
 ****************************************************************************/

static int rk3576_saradc_setup(struct adc_dev_s *dev)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_saradc_hwinit(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  if (!priv->irq_attached)
    {
      ret = irq_attach(RK3576_IRQ_SARADC, rk3576_saradc_interrupt, priv);
      if (ret < 0)
        {
          aerr("ERROR: failed to attach IRQ %d: %d\n", RK3576_IRQ_SARADC,
               ret);
          nxmutex_unlock(&priv->lock);
          return ret;
        }

      priv->irq_attached = true;
    }

  up_enable_irq(RK3576_IRQ_SARADC);
  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_saradc_shutdown
 *
 * Description:
 *   Called when the last reference to the character device is closed.
 *
 ****************************************************************************/

static void rk3576_saradc_shutdown(struct adc_dev_s *dev)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;

  nxmutex_lock(&priv->lock);

  up_disable_irq(RK3576_IRQ_SARADC);
  rk3576_saradc_stop();

  priv->rxenabled = false;
  priv->scanning = false;
  priv->pending = 0;

  nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: rk3576_saradc_rxint
 *
 * Description:
 *   Enable or disable the delivery of conversion results to the upper
 *   half.  The end-of-conversion interrupt itself is only unmasked while a
 *   scan is actually running.
 *
 ****************************************************************************/

static void rk3576_saradc_rxint(struct adc_dev_s *dev, bool enable)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;
  irqstate_t flags;

  flags = enter_critical_section();

  priv->rxenabled = enable;
  if (!enable && priv->scanning)
    {
      priv->scanning = false;
      priv->pending = 0;
      rk3576_saradc_stop();
    }

  leave_critical_section(flags);
}

/****************************************************************************
 * Name: rk3576_saradc_ioctl
 *
 * Description:
 *   Lower-half ioctl.  ANIOC_TRIGGER launches one interrupt driven sweep
 *   of the configured channel mask; ANIOC_GET_NCHANNELS reports the number
 *   of channels the sweep produces.
 *
 ****************************************************************************/

static int rk3576_saradc_ioctl(struct adc_dev_s *dev, int cmd,
                               unsigned long arg)
{
  struct rk3576_saradc_priv_s *priv = dev->ad_priv;
  irqstate_t flags;
  uint8_t ch;
  int ret = OK;

  switch (cmd)
    {
      case ANIOC_TRIGGER:
        {
          if (!priv->hw_ready)
            {
              return -EAGAIN;
            }

          flags = enter_critical_section();

          if (priv->scanning)
            {
              ret = -EBUSY;
            }
          else
            {
              priv->pending = priv->chanmask;
              priv->scanning = true;

              for (ch = 0; ch < RK3576_SARADC_NCHANNELS; ch++)
                {
                  if ((priv->pending & (1u << ch)) != 0)
                    {
                      break;
                    }
                }

              priv->current = ch;
              rk3576_saradc_putreg(RK3576_SARADC_END_INT_EN,
                                   RK3576_SARADC_HIWORD(
                                     SARADC_END_INT_EN_EN));
              rk3576_saradc_start(ch);
            }

          leave_critical_section(flags);
        }
        break;

      case ANIOC_GET_NCHANNELS:
        {
          ret = 0;
          for (ch = 0; ch < RK3576_SARADC_NCHANNELS; ch++)
            {
              if ((priv->chanmask & (1u << ch)) != 0)
                {
                  ret++;
                }
            }
        }
        break;

      default:
        {
          UNUSED(arg);
          ret = -ENOTTY;
        }
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_saradc_initialize
 *
 * Description:
 *   Bring the SAR-ADC up and return the upper-half device structure.
 *   See rk3576_saradc.h for the full description.
 *
 ****************************************************************************/

struct adc_dev_s *rk3576_saradc_initialize(uint32_t chanmask)
{
  struct rk3576_saradc_priv_s *priv = &g_rk3576_saradc_priv;
  int ret;

  if (chanmask == 0 || (chanmask & ~RK3576_SARADC_CHAN_ALL) != 0)
    {
      aerr("ERROR: invalid channel mask 0x%08" PRIx32 "\n", chanmask);
      return NULL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return NULL;
    }

  ret = rk3576_saradc_hwinit(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return NULL;
    }

  priv->chanmask = chanmask;
  nxmutex_unlock(&priv->lock);

  return &g_rk3576_saradc_dev;
}

/****************************************************************************
 * Name: rk3576_saradc_register
 *
 * Description:
 *   Initialise the converter and register it at 'devpath'.
 *
 ****************************************************************************/

int rk3576_saradc_register(const char *devpath, uint32_t chanmask)
{
  struct adc_dev_s *dev;
  int ret;

  DEBUGASSERT(devpath != NULL);

  dev = rk3576_saradc_initialize(chanmask);
  if (dev == NULL)
    {
      return -ENODEV;
    }

  ret = adc_register(devpath, dev);
  if (ret < 0)
    {
      aerr("ERROR: adc_register(%s) failed: %d\n", devpath, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_saradc_read_channel
 *
 * Description:
 *   Blocking, polled single conversion.  See rk3576_saradc.h.
 *
 ****************************************************************************/

int rk3576_saradc_read_channel(int ch, uint16_t *val)
{
  struct rk3576_saradc_priv_s *priv = &g_rk3576_saradc_priv;
  irqstate_t flags;
  unsigned int i;
  uint32_t status;
  int ret;

  if (val == NULL || ch < 0 || ch >= RK3576_SARADC_NCHANNELS)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_saradc_hwinit(priv);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  /* Keep the interrupt handler out of this conversion: the mutex excludes
   * a concurrent ANIOC_TRIGGER, the critical section closes the window
   * against an interrupt left over from a previous scan.
   */

  flags = enter_critical_section();
  rk3576_saradc_putreg(RK3576_SARADC_END_INT_EN,
                       RK3576_SARADC_HIWORD_CLR(SARADC_END_INT_EN_EN));
  rk3576_saradc_start((uint8_t)ch);
  leave_critical_section(flags);

  ret = -ETIMEDOUT;
  for (i = 0; i < RK3576_SARADC_POLL_STEPS; i++)
    {
      status = rk3576_saradc_getreg(RK3576_SARADC_END_INT_ST);
      if ((status & SARADC_END_INT_ST_ST) != 0)
        {
          *val = (uint16_t)(rk3576_saradc_getreg(RK3576_SARADC_DATA(ch)) &
                            RK3576_SARADC_DATA_MASK);
          ret = OK;
          break;
        }

      up_udelay(RK3576_SARADC_POLL_STEP_US);
    }

  rk3576_saradc_stop();

  if (ret < 0)
    {
      aerr("ERROR: channel %d conversion timed out\n", ch);
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

#endif /* CONFIG_RK3576_SARADC */
