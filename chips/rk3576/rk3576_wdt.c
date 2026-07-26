/****************************************************************************
 * chips/rk3576/rk3576_wdt.c
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
 * RK3576 non-secure watchdog (WDT_NS, "snps,dw-wdt") lower-half driver.
 *
 * The DesignWare watchdog is a down counter clocked by tclk.  The reload
 * value is not freely programmable: WDT_TORR selects one of sixteen fixed
 * ranges, 2^(16 + i) - 1 ticks for i = 0..15.  settimeout() therefore picks
 * the range whose resulting period is closest to the value requested by the
 * application and reports that rounded value back through getstatus().
 *
 * Two response modes are supported (TRM 15.3.1):
 *
 *   RMOD = 0  the SoC is reset directly on the first timeout.  This is the
 *             default configured here.
 *   RMOD = 1  the first timeout raises irq_wdt_ns; a second timeout without
 *             the interrupt being cleared resets the SoC.  This mode is
 *             selected automatically when the upper half installs a capture
 *             handler.
 *
 * Note that WDT_CR.EN is sticky: once the watchdog has been enabled it can
 * only be cleared by a system reset.  stop() reflects this by returning
 * -ENOSYS when the hardware refuses to disable.
 *
 * The two input clocks (pclk for the register interface, tclk for the down
 * counter) are taken from the NuttX CLK framework; the tclk rate is read
 * back with clk_get_rate() and is what every tick/millisecond conversion in
 * this file is based on.
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
#include <stdio.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/timers/watchdog.h>

#include "arm64_internal.h"
#include "hardware/rk3576_wdt.h"
#include "rk3576_wdt.h"

#ifdef CONFIG_RK3576_WDT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Reset pulse width applied to the SoC reset line on timeout. */

#define RK3576_WDT_RPL WDT_CR_RPL_256PCLK

/* CLK framework node names of the two input clocks. */

#define RK3576_WDT_PCLK_NAME "pclk_wdt_ns_en"
#define RK3576_WDT_TCLK_NAME "tclk_wdt_ns_en"

/* Default timeout applied at initialisation time (milliseconds). */

#define RK3576_WDT_DEFAULT_TIMEOUT_MS 10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_wdt_lowerhalf_s
{
  const struct watchdog_ops_s *ops; /* Lower-half operations (must be 1st) */
  uintptr_t base;                   /* Register base address               */
  int irq;                          /* GIC interrupt number                */
  xcpt_t handler;                   /* Capture handler, NULL if unused     */
  uint32_t timeout;                 /* Requested timeout, milliseconds     */
  uint32_t period;                  /* Rounded timeout, milliseconds       */
  uint32_t tclk_hz;                 /* Counter clock rate from the CLK fw  */
  uint8_t torr;                     /* WDT_TORR timeout period index       */
  bool started;                     /* Watchdog currently running          */
  spinlock_t lock;                  /* Protects read-modify-write of CR    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_wdt_getreg(struct rk3576_wdt_lowerhalf_s *priv,
                                  unsigned int off);
static void rk3576_wdt_putreg(struct rk3576_wdt_lowerhalf_s *priv,
                              unsigned int off, uint32_t val);
static int rk3576_wdt_clk_init(void);
static uint32_t rk3576_wdt_ticks_to_ms(struct rk3576_wdt_lowerhalf_s *priv,
                                       uint32_t ticks);
static uint8_t rk3576_wdt_select_torr(struct rk3576_wdt_lowerhalf_s *priv,
                                      uint32_t timeout_ms,
                                      uint32_t *actual_ms);
static void rk3576_wdt_kick(struct rk3576_wdt_lowerhalf_s *priv);
static int rk3576_wdt_interrupt(int irq, void *context, void *arg);

static int rk3576_wdt_start(struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_stop(struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_keepalive(struct watchdog_lowerhalf_s *lower);
static int rk3576_wdt_getstatus(struct watchdog_lowerhalf_s *lower,
                                struct watchdog_status_s *status);
static int rk3576_wdt_settimeout(struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout);
static xcpt_t rk3576_wdt_capture(struct watchdog_lowerhalf_s *lower,
                                 xcpt_t handler);
static int rk3576_wdt_ioctl(struct watchdog_lowerhalf_s *lower, int cmd,
                            unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct watchdog_ops_s g_rk3576_wdt_ops =
{
  .start      = rk3576_wdt_start,
  .stop       = rk3576_wdt_stop,
  .keepalive  = rk3576_wdt_keepalive,
  .getstatus  = rk3576_wdt_getstatus,
  .settimeout = rk3576_wdt_settimeout,
  .capture    = rk3576_wdt_capture,
  .ioctl      = rk3576_wdt_ioctl,
};

static struct rk3576_wdt_lowerhalf_s g_rk3576_wdt =
{
  .ops     = &g_rk3576_wdt_ops,
  .base    = RK3576_WDT_NS_ADDR,
  .irq     = RK3576_IRQ_WDT_NS,
  .handler = NULL,
  .started = false,
  .lock    = SP_UNLOCKED,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_wdt_getreg(struct rk3576_wdt_lowerhalf_s *priv,
                                  unsigned int off)
{
  return getreg32(priv->base + off);
}

static void rk3576_wdt_putreg(struct rk3576_wdt_lowerhalf_s *priv,
                              unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_wdt_clk_init
 *
 * Description:
 *   Ungate the two WDT_NS input clocks through the NuttX CLK framework and
 *   latch the counter clock rate.  All clock handling of this driver lives
 *   here.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_wdt_clk_init(void)
{
  struct rk3576_wdt_lowerhalf_s *priv = &g_rk3576_wdt;
  struct clk_s *pclk;
  struct clk_s *tclk;
  int ret;

  /* APB register interface clock. */

  pclk = clk_get(RK3576_WDT_PCLK_NAME);
  if (pclk == NULL)
    {
      wderr("ERROR: failed to get %s\n", RK3576_WDT_PCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      wderr("ERROR: failed to enable %s: %d\n", RK3576_WDT_PCLK_NAME, ret);
      return ret;
    }

  /* Down-counter clock.  Its rate defines every timeout period the block
   * can produce, so it is read back rather than assumed.
   */

  tclk = clk_get(RK3576_WDT_TCLK_NAME);
  if (tclk == NULL)
    {
      wderr("ERROR: failed to get %s\n", RK3576_WDT_TCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(tclk);
  if (ret < 0)
    {
      wderr("ERROR: failed to enable %s: %d\n", RK3576_WDT_TCLK_NAME, ret);
      return ret;
    }

  priv->tclk_hz = clk_get_rate(tclk);
  if (priv->tclk_hz == 0)
    {
      wderr("ERROR: %s reports a zero rate\n", RK3576_WDT_TCLK_NAME);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_ticks_to_ms
 *
 * Description:
 *   Convert a watchdog counter tick count into milliseconds.  The 64-bit
 *   intermediate avoids overflow: the largest tick count is 0x7fffffff.
 ****************************************************************************/

static uint32_t rk3576_wdt_ticks_to_ms(struct rk3576_wdt_lowerhalf_s *priv,
                                       uint32_t ticks)
{
  return (uint32_t)(((uint64_t)ticks * 1000ull) / priv->tclk_hz);
}

/****************************************************************************
 * Name: rk3576_wdt_select_torr
 *
 * Description:
 *   Find the WDT_TORR timeout period index whose resulting period is
 *   closest to the requested timeout.
 *
 * Input Parameters:
 *   priv       - Device state, holding the counter clock rate.
 *   timeout_ms - Requested timeout in milliseconds.
 *   actual_ms  - [out] Period actually obtained, in milliseconds.
 *
 * Returned Value:
 *   The WDT_TORR timeout period index (0..15).
 *
 ****************************************************************************/

static uint8_t rk3576_wdt_select_torr(struct rk3576_wdt_lowerhalf_s *priv,
                                      uint32_t timeout_ms,
                                      uint32_t *actual_ms)
{
  uint32_t best_ms = 0;
  uint64_t best_diff = UINT64_MAX;
  uint8_t best = 0;
  uint8_t i;

  for (i = 0; i < WDT_TORR_PERIOD_NR; i++)
    {
      uint32_t ms = rk3576_wdt_ticks_to_ms(priv, WDT_TORR_TICKS(i));
      uint64_t diff = (ms > timeout_ms) ? (uint64_t)(ms - timeout_ms)
                                        : (uint64_t)(timeout_ms - ms);

      if (diff < best_diff)
        {
          best_diff = diff;
          best_ms   = ms;
          best      = i;
        }
    }

  *actual_ms = best_ms;
  return best;
}

/****************************************************************************
 * Name: rk3576_wdt_kick
 *
 * Description:
 *   Restart the counter from the currently selected timeout period.  This
 *   also latches a freshly written WDT_TORR value and clears any pending
 *   watchdog interrupt.
 ****************************************************************************/

static void rk3576_wdt_kick(struct rk3576_wdt_lowerhalf_s *priv)
{
  rk3576_wdt_putreg(priv, RK3576_WDT_CRR, WDT_CRR_KICK);
}

/****************************************************************************
 * Name: rk3576_wdt_interrupt
 *
 * Description:
 *   WDT_NS interrupt handler, only reachable while RMOD = 1.  The pending
 *   interrupt is acknowledged by reading WDT_EOI before the registered
 *   capture handler runs; the SoC is reset by the hardware if a second
 *   timeout elapses without the counter being kicked.
 ****************************************************************************/

static int rk3576_wdt_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)arg;

  /* Reading WDT_EOI clears the interrupt without restarting the counter. */

  rk3576_wdt_getreg(priv, RK3576_WDT_EOI);

  wdinfo("WDT_NS timeout, capture handler %p\n", priv->handler);

  if (priv->handler != NULL)
    {
      return priv->handler(irq, context, arg);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_start
 ****************************************************************************/

static int rk3576_wdt_start(struct watchdog_lowerhalf_s *lower)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;
  irqstate_t flags;
  uint32_t cr;

  flags = spin_lock_irqsave(&priv->lock);

  /* Load the timeout range and restart the counter before enabling, so the
   * first period is the configured one rather than the reset default.
   */

  rk3576_wdt_putreg(priv, RK3576_WDT_TORR,
                    (uint32_t)priv->torr << WDT_TORR_PERIOD_SHIFT);
  rk3576_wdt_kick(priv);

  cr = rk3576_wdt_getreg(priv, RK3576_WDT_CR);
  cr &= ~(WDT_CR_RPL_MASK | WDT_CR_RMOD);
  cr |= RK3576_WDT_RPL;
  cr |= (priv->handler != NULL) ? WDT_CR_RMOD_IRQ : WDT_CR_RMOD_RESET;
  cr |= WDT_CR_EN;
  rk3576_wdt_putreg(priv, RK3576_WDT_CR, cr);

  priv->started = true;

  spin_unlock_irqrestore(&priv->lock, flags);

  wdinfo("started: timeout=%" PRIu32 "ms period=%" PRIu32 "ms torr=%u\n",
         priv->timeout, priv->period, priv->torr);
  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_stop
 *
 * Description:
 *   Attempt to disable the watchdog.  WDT_CR.EN is sticky on this IP: the
 *   TRM states that once set it can only be cleared by a system reset.  The
 *   write is still issued and read back so that silicon which does allow it
 *   is handled correctly.
 ****************************************************************************/

static int rk3576_wdt_stop(struct watchdog_lowerhalf_s *lower)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;
  irqstate_t flags;
  uint32_t cr;
  int ret = OK;

  flags = spin_lock_irqsave(&priv->lock);

  cr = rk3576_wdt_getreg(priv, RK3576_WDT_CR);
  rk3576_wdt_putreg(priv, RK3576_WDT_CR, cr & ~WDT_CR_EN);

  if ((rk3576_wdt_getreg(priv, RK3576_WDT_CR) & WDT_CR_EN) != 0)
    {
      /* The hardware refused: keep the counter alive so the caller is not
       * reset by a watchdog it believes to be stopped.
       */

      rk3576_wdt_kick(priv);
      ret = -ENOSYS;
    }
  else
    {
      priv->started = false;
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  if (ret < 0)
    {
      wdwarn("WARNING: WDT_CR.EN is sticky, watchdog cannot be stopped\n");
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_wdt_keepalive
 ****************************************************************************/

static int rk3576_wdt_keepalive(struct watchdog_lowerhalf_s *lower)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;

  rk3576_wdt_kick(priv);
  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_getstatus
 ****************************************************************************/

static int rk3576_wdt_getstatus(struct watchdog_lowerhalf_s *lower,
                                struct watchdog_status_s *status)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;

  status->flags = 0;
  if (priv->started)
    {
      status->flags |= WDFLAGS_ACTIVE;
    }

  if (priv->handler != NULL)
    {
      status->flags |= WDFLAGS_CAPTURE;
    }

  /* Report the rounded period the hardware really implements. */

  status->timeout  = priv->period;
  status->timeleft =
    rk3576_wdt_ticks_to_ms(priv, rk3576_wdt_getreg(priv, RK3576_WDT_CCVR));

  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_settimeout
 *
 * Description:
 *   Select the timeout range closest to the requested value.  A change of
 *   WDT_TORR only takes effect on the next counter restart, so the counter
 *   is kicked here when the watchdog is already running.
 ****************************************************************************/

static int rk3576_wdt_settimeout(struct watchdog_lowerhalf_s *lower,
                                 uint32_t timeout)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;
  uint32_t max_ms = rk3576_wdt_ticks_to_ms(
    priv, WDT_TORR_TICKS(WDT_TORR_PERIOD_NR - 1));
  uint32_t min_ms = rk3576_wdt_ticks_to_ms(priv, WDT_TORR_TICKS(0));
  irqstate_t flags;

  if (timeout < min_ms || timeout > max_ms)
    {
      wderr("ERROR: timeout %" PRIu32 "ms out of range [%" PRIu32
            ", %" PRIu32 "]\n", timeout, min_ms, max_ms);
      return -ERANGE;
    }

  flags = spin_lock_irqsave(&priv->lock);

  priv->timeout = timeout;
  priv->torr    = rk3576_wdt_select_torr(priv, timeout, &priv->period);

  rk3576_wdt_putreg(priv, RK3576_WDT_TORR,
                    (uint32_t)priv->torr << WDT_TORR_PERIOD_SHIFT);

  if (priv->started)
    {
      rk3576_wdt_kick(priv);
    }

  spin_unlock_irqrestore(&priv->lock, flags);

  wdinfo("timeout=%" PRIu32 "ms -> period=%" PRIu32 "ms (torr=%u)\n",
         priv->timeout, priv->period, priv->torr);
  return OK;
}

/****************************************************************************
 * Name: rk3576_wdt_capture
 *
 * Description:
 *   Install (or remove) a handler called on the first watchdog timeout.
 *   Installing a handler switches the block to RMOD = 1 so that the first
 *   timeout raises irq_wdt_ns instead of resetting the SoC immediately; a
 *   second timeout still resets the SoC.  Passing NULL restores RMOD = 0.
 *
 * Returned Value:
 *   The previously installed handler, or NULL if there was none.
 *
 ****************************************************************************/

static xcpt_t rk3576_wdt_capture(struct watchdog_lowerhalf_s *lower,
                                 xcpt_t handler)
{
  struct rk3576_wdt_lowerhalf_s *priv =
    (struct rk3576_wdt_lowerhalf_s *)lower;
  irqstate_t flags;
  xcpt_t oldhandler;
  uint32_t cr;

  flags = spin_lock_irqsave(&priv->lock);

  oldhandler    = priv->handler;
  priv->handler = handler;

  cr = rk3576_wdt_getreg(priv, RK3576_WDT_CR);

  if (handler != NULL)
    {
      cr |= WDT_CR_RMOD_IRQ;
      rk3576_wdt_putreg(priv, RK3576_WDT_CR, cr);
      up_enable_irq(priv->irq);
    }
  else
    {
      up_disable_irq(priv->irq);
      cr &= ~WDT_CR_RMOD;
      rk3576_wdt_putreg(priv, RK3576_WDT_CR, cr);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return oldhandler;
}

/****************************************************************************
 * Name: rk3576_wdt_ioctl
 ****************************************************************************/

static int rk3576_wdt_ioctl(struct watchdog_lowerhalf_s *lower, int cmd,
                            unsigned long arg)
{
  UNUSED(lower);
  UNUSED(cmd);
  UNUSED(arg);
  return -ENOTTY;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_wdt_initialize
 *
 * Description:
 *   Initialise WDT_NS and register it as /dev/watchdogN.  See
 *   rk3576_wdt.h for the full description.
 ****************************************************************************/

int rk3576_wdt_initialize(int minor)
{
  struct rk3576_wdt_lowerhalf_s *priv = &g_rk3576_wdt;
  char devpath[16];
  int ret;

  /* Ungate pclk and tclk and learn the counter clock rate before touching
   * any register of the block.
   */

  ret = rk3576_wdt_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  /* Allow WDT_NS to drive the global SoC reset.  The bit lives in the
   * hiword-masked SYS_GRF_SOC_CON4 register.  The pause bit is left alone.
   */

  putreg32(WDT_HIWORD(SYS_GRF_SOC_CON4_WDTNS_GLBRST),
           RK3576_SYS_GRF_ADDR + RK3576_SYS_GRF_SOC_CON4);

  /* Make sure the counter is not running and no interrupt is pending. */

  rk3576_wdt_putreg(priv, RK3576_WDT_CR, RK3576_WDT_RPL);
  rk3576_wdt_getreg(priv, RK3576_WDT_EOI);

  priv->handler = NULL;
  priv->started = false;
  priv->timeout = RK3576_WDT_DEFAULT_TIMEOUT_MS;
  priv->torr    = rk3576_wdt_select_torr(priv, priv->timeout,
                                         &priv->period);

  rk3576_wdt_putreg(priv, RK3576_WDT_TORR,
                    (uint32_t)priv->torr << WDT_TORR_PERIOD_SHIFT);

  /* Attach the timeout interrupt.  It stays disabled at the GIC until a
   * capture handler is installed, because in the default RMOD = 0 mode the
   * hardware resets the SoC without ever raising it.
   */

  ret = irq_attach(priv->irq, rk3576_wdt_interrupt, priv);
  if (ret < 0)
    {
      wderr("ERROR: irq_attach(%d) failed: %d\n", priv->irq, ret);
      return ret;
    }

  up_disable_irq(priv->irq);

  snprintf(devpath, sizeof(devpath), "/dev/watchdog%d", minor);
  if (watchdog_register(devpath, (struct watchdog_lowerhalf_s *)priv) ==
      NULL)
    {
      wderr("ERROR: watchdog_register(%s) failed\n", devpath);
      irq_detach(priv->irq);
      return -EEXIST;
    }

  wdinfo("%s registered: base=%08" PRIxPTR " tclk=%" PRIu32 "Hz period=%"
         PRIu32 "ms\n", devpath, priv->base, priv->tclk_hz, priv->period);
  return OK;
}

#endif /* CONFIG_RK3576_WDT */
