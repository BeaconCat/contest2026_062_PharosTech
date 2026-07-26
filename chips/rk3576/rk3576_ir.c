/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_ir.c
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
 * RK3576 infrared remote-control receiver.
 *
 * The SoC has no dedicated IR block.  Reception re-uses PWM0 channel 0 in
 * capture mode together with the "power key match" hardware, which is a
 * fixed-function NEC decoder: it measures the 9 ms/4.5 ms leader, then the
 * 32 bit cells, assembles the frame and raises PWM_INTSTS.pwr_intsts with
 * the result in PWM_PWRCAPTURE_VALUE (TRM Part1 chapter 34, flow 34.6.2).
 * With PWM_PWRMATCH_CTRL.pwrkey_int_ctrl = 1 and .pwrkey_capture_ctrl = 1
 * the interrupt fires for *every* frame, not only for the 16 preprogrammed
 * PWM_PWRMATCH_VALUEn power-key codes, which turns the block into a
 * general-purpose NEC receiver.  No software pulse timing is therefore
 * needed and the driver never sees per-edge interrupts.
 *
 * The counting clock is programmed to exactly 1 MHz (required by the TRM
 * for power-key mode), so all PWM_PWRMATCH_* thresholds are expressed in
 * microseconds.
 *
 * Decoded events are delivered two ways: queued to a character device
 * (/dev/ir0, read() returns uint32_t events) and, optionally, handed to a
 * callback registered with rk3576_ir_register_callback().
 *
 * Pin muxing of the PWM0_CH0 pad to its IR input function is the board's
 * responsibility and must be done before rk3576_ir_initialize().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "hardware/rk3576_ir.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_pwm.h"
#include "rk3576_ir.h"

#ifdef CONFIG_RK3576_IR

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Clock gate names produced by the RK3576 clock tree for PWM0.  The IR
 * receiver only ever uses controller 0 (see RK3576_IR_PWM_CTRL_ID), so the
 * names can be string literals.
 */

#define RK3576_IR_PCLK_NAME "pclk_pwm0_en"
#define RK3576_IR_FCLK_NAME "clk_pwm0_osc_en"

/* Power-key mode mandates a 1 MHz counting clock (TRM 34.6.2 step 2).
 * One tick therefore equals one microsecond.
 */

#define RK3576_IR_CAP_CLK_HZ 1000000

/* PWM_CLK_CTRL divider limits: f = f_in / (2^prescale * 2 * scale). */

#define RK3576_IR_PRESCALE_MAX 7
#define RK3576_IR_SCALE_MIN    1
#define RK3576_IR_SCALE_MAX    256

/* NEC protocol timing, in counting-clock ticks (== microseconds).  A +/-
 * tolerance window is applied to every nominal value; the numbers match
 * the hardware reset values of the PWM_PWRMATCH_* registers, which are
 * already NEC-tuned for a 1 MHz clock, but are programmed explicitly so
 * the driver does not depend on the reset state of a warm-booted block.
 */

#define RK3576_IR_NEC_LEADER_LOW_US  9000 /* 9 ms burst   (input low)  */
#define RK3576_IR_NEC_LEADER_HIGH_US 4500 /* 4.5 ms space (input high) */
#define RK3576_IR_NEC_BIT_LOW_US     560  /* 560 us burst, every bit   */
#define RK3576_IR_NEC_ZERO_HIGH_US   560  /* 560 us space  -> logic 0  */
#define RK3576_IR_NEC_ONE_HIGH_US    1690 /* 1.69 ms space -> logic 1  */

#define RK3576_IR_TOL_NUM            1 /* Tolerance = value * 1 / 10   */
#define RK3576_IR_TOL_DEN            10

#define RK3576_IR_MIN_US(us) \
  ((us) - ((us)*RK3576_IR_TOL_NUM) / RK3576_IR_TOL_DEN)
#define RK3576_IR_MAX_US(us) \
  ((us) + ((us)*RK3576_IR_TOL_NUM) / RK3576_IR_TOL_DEN)

#define RK3576_IR_THRESHOLD(us) \
  PWM_PWRMATCH_CNT(RK3576_IR_MIN_US(us), RK3576_IR_MAX_US(us))

/* A NEC transmitter repeats the last frame every 108 ms while the key is
 * held.  Two identical frames closer together than this window are
 * reported as an auto-repeat event.
 */

#define RK3576_IR_REPEAT_WINDOW_MS 130

/* Event queue.  Power sizes only (the index arithmetic uses a mask). */

#define RK3576_IR_QUEUE_SIZE   16
#define RK3576_IR_QUEUE_MASK   (RK3576_IR_QUEUE_SIZE - 1)

#define RK3576_IR_NPOLLWAITERS 2

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_ir_dev_s
{
  uintptr_t base;     /* PWM channel register window        */
  struct clk_s *pclk; /* APB interface clock                */
  struct clk_s *fclk; /* Counting-clock source (osc)        */
  uint32_t fclk_hz;   /* Real rate of fclk, from the tree   */
  bool initialized;   /* Hardware brought up                */

  /* Decoded-event queue, filled from interrupt context */

  uint32_t queue[RK3576_IR_QUEUE_SIZE];
  uint8_t head;    /* Next slot to write (producer)      */
  uint8_t tail;    /* Next slot to read  (consumer)      */
  spinlock_t lock; /* Guards queue/head/tail             */

  sem_t waitsem;    /* Signalled when an event is queued  */
  mutex_t readlock; /* Serialises concurrent readers      */
  bool waiting;     /* A reader is blocked on waitsem     */

  /* Auto-repeat tracking */

  uint32_t last_key; /* Previous decoded addr/cmd          */
  clock_t last_tick; /* Timestamp of the previous frame    */
  bool have_last;    /* last_key/last_tick are valid       */

  /* Optional user callback, invoked from interrupt context */

  rk3576_ir_callback_t callback;
  void *callback_arg;

  struct pollfd *fds[RK3576_IR_NPOLLWAITERS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_ir_getreg(struct rk3576_ir_dev_s *priv,
                                 unsigned int off);
static void rk3576_ir_putreg(struct rk3576_ir_dev_s *priv, unsigned int off,
                             uint32_t val);

static int rk3576_ir_clk_init(struct rk3576_ir_dev_s *priv);
static int rk3576_ir_calc_divider(uint32_t in_hz, uint32_t out_hz,
                                  uint32_t *prescale, uint32_t *scale);
static bool rk3576_ir_decode(uint32_t raw, uint32_t *key);
static void rk3576_ir_post(struct rk3576_ir_dev_s *priv, uint32_t key);
static int rk3576_ir_interrupt(int irq, void *context, void *arg);
static int rk3576_ir_hw_setup(struct rk3576_ir_dev_s *priv);

static ssize_t rk3576_ir_read(struct file *filep, char *buffer, size_t buflen);
static int rk3576_ir_poll(struct file *filep, struct pollfd *fds, bool setup);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_rk3576_ir_fops = {
  .read = rk3576_ir_read,
  .poll = rk3576_ir_poll,
};

static struct rk3576_ir_dev_s g_rk3576_ir = {
  .lock = SP_UNLOCKED,
  .waitsem = SEM_INITIALIZER(0),
  .readlock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_ir_getreg(struct rk3576_ir_dev_s *priv,
                                 unsigned int off)
{
  return getreg32(priv->base + off);
}

static void rk3576_ir_putreg(struct rk3576_ir_dev_s *priv, unsigned int off,
                             uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_ir_clk_init
 *
 * Description:
 *   Single point of contact with the clock framework: acquire and enable
 *   the PWM0 APB clock and the oscillator-derived counting clock, and
 *   record the real input rate.  Called once from rk3576_ir_initialize().
 *
 ****************************************************************************/

static int rk3576_ir_clk_init(struct rk3576_ir_dev_s *priv)
{
  int ret;

  /* APB bus clock — needed before any register access */

  priv->pclk = clk_get(RK3576_IR_PCLK_NAME);
  if (priv->pclk == NULL)
    {
      _err("ERROR: failed to get %s\n", RK3576_IR_PCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      _err("ERROR: failed to enable %s: %d\n", RK3576_IR_PCLK_NAME, ret);
      return ret;
    }

  /* Counting clock.  The oscillator branch is used because it is a fixed,
   * PLL-independent rate that divides cleanly down to 1 MHz.
   */

  priv->fclk = clk_get(RK3576_IR_FCLK_NAME);
  if (priv->fclk == NULL)
    {
      _err("ERROR: failed to get %s\n", RK3576_IR_FCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(priv->fclk);
  if (ret < 0)
    {
      _err("ERROR: failed to enable %s: %d\n", RK3576_IR_FCLK_NAME, ret);
      return ret;
    }

  priv->fclk_hz = clk_get_rate(priv->fclk);
  if (priv->fclk_hz == 0)
    {
      _err("ERROR: %s reports a zero rate\n", RK3576_IR_FCLK_NAME);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_ir_calc_divider
 *
 * Description:
 *   Solve in_hz / (2^prescale * 2 * scale) == out_hz exactly.  The PWM
 *   power-key decoder compares against absolute tick counts, so an
 *   approximate clock is not acceptable; only exact solutions are taken.
 *
 * Returned Value:
 *   OK and the divider fields on success, -ERANGE if the input rate cannot
 *   produce the requested counting clock.
 *
 ****************************************************************************/

static int rk3576_ir_calc_divider(uint32_t in_hz, uint32_t out_hz,
                                  uint32_t *prescale, uint32_t *scale)
{
  uint32_t pre;
  uint32_t sc;

  if (out_hz == 0 || in_hz < out_hz)
    {
      return -ERANGE;
    }

  for (pre = 0; pre <= RK3576_IR_PRESCALE_MAX; pre++)
    {
      uint32_t step = (1u << pre) * 2u;

      for (sc = RK3576_IR_SCALE_MIN; sc <= RK3576_IR_SCALE_MAX; sc++)
        {
          uint32_t div = step * sc;

          if (in_hz % div != 0)
            {
              continue;
            }

          if (in_hz / div == out_hz)
            {
              *prescale = pre;

              /* The register field holds scale modulo 256: a programmed
               * value of 0 selects the maximum divider of 256.
               */

              *scale = sc & (RK3576_IR_SCALE_MAX - 1);
              return OK;
            }
        }
    }

  return -ERANGE;
}

/****************************************************************************
 * Name: rk3576_ir_decode
 *
 * Description:
 *   Validate a raw 32-bit frame delivered by the power-key decoder and
 *   convert it to the driver's address/command representation.
 *
 *   A NEC frame carries address, ~address, command, ~command, each byte
 *   sent LSB first.  The hardware assembles the bits in reception order,
 *   which places the address in the least significant byte.  Some frames
 *   observed on Rockchip parts arrive byte-swapped instead, so both
 *   orderings are tried and the one whose ~command byte checks out wins.
 *
 *   Extended NEC uses a 16-bit address and therefore fails the ~address
 *   check; such frames are accepted with the full 16-bit address.
 *
 * Returned Value:
 *   true if the frame is a valid NEC frame, false if it must be dropped.
 *
 ****************************************************************************/

static bool rk3576_ir_decode(uint32_t raw, uint32_t *key)
{
  uint32_t candidates[2];
  unsigned int i;

  candidates[0] = raw;
  candidates[1] = ((raw & 0x000000ffu) << 24) | ((raw & 0x0000ff00u) << 8) |
                  ((raw & 0x00ff0000u) >> 8) | ((raw & 0xff000000u) >> 24);

  for (i = 0; i < 2; i++)
    {
      uint32_t frame = candidates[i];
      uint32_t addr = frame & 0xffu;
      uint32_t naddr = (frame >> 8) & 0xffu;
      uint32_t cmd = (frame >> 16) & 0xffu;
      uint32_t ncmd = (frame >> 24) & 0xffu;

      if (((cmd ^ ncmd) & 0xffu) != 0xffu)
        {
          continue;
        }

      /* Standard NEC: address and its complement agree.  Extended NEC:
       * the two bytes form a 16-bit address instead.
       */

      if (((addr ^ naddr) & 0xffu) != 0xffu)
        {
          addr |= naddr << 8;
        }

      *key = ((addr << RK3576_IR_ADDR_SHIFT) & RK3576_IR_ADDR_MASK) |
             (cmd & RK3576_IR_CMD_MASK);
      return true;
    }

  return false;
}

/****************************************************************************
 * Name: rk3576_ir_post
 *
 * Description:
 *   Queue one decoded event, wake a blocked reader and any pollers, and
 *   run the registered callback.  Runs in interrupt context.
 *
 ****************************************************************************/

static void rk3576_ir_post(struct rk3576_ir_dev_s *priv, uint32_t key)
{
  irqstate_t flags;
  uint8_t next;
  bool wake;

  flags = spin_lock_irqsave(&priv->lock);

  next = (priv->head + 1) & RK3576_IR_QUEUE_MASK;
  if (next == priv->tail)
    {
      /* Queue full: drop the oldest event so the newest key press is the
       * one the application eventually sees.
       */

      priv->tail = (priv->tail + 1) & RK3576_IR_QUEUE_MASK;
    }

  priv->queue[priv->head] = key;
  priv->head = next;

  wake = priv->waiting;
  priv->waiting = false;

  spin_unlock_irqrestore(&priv->lock, flags);

  if (wake)
    {
      nxsem_post(&priv->waitsem);
    }

  poll_notify(priv->fds, RK3576_IR_NPOLLWAITERS, POLLIN);

  if (priv->callback != NULL)
    {
      priv->callback(key, priv->callback_arg);
    }
}

/****************************************************************************
 * Name: rk3576_ir_interrupt
 *
 * Description:
 *   Power-key match interrupt handler.  One interrupt equals one fully
 *   decoded 32-bit NEC frame in PWM_PWRCAPTURE_VALUE.
 *
 ****************************************************************************/

static int rk3576_ir_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_ir_dev_s *priv = (struct rk3576_ir_dev_s *)arg;
  uint32_t status;
  uint32_t raw;
  uint32_t key;
  clock_t now;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_ir_getreg(priv, RK3576_PWM_INTSTS);
  if ((status & PWM_INT_PWR) == 0)
    {
      /* Not ours (or a stale status bit from another feature): clear
       * whatever is pending so the line does not stay asserted.
       */

      rk3576_ir_putreg(priv, RK3576_PWM_INTSTS, status);
      return OK;
    }

  raw = rk3576_ir_getreg(priv, RK3576_PWM_PWRCAPTURE_VALUE);

  /* Acknowledge before processing so a frame arriving during decode is
   * not lost.
   */

  rk3576_ir_putreg(priv, RK3576_PWM_INTSTS, PWM_INT_PWR);

  if (!rk3576_ir_decode(raw, &key))
    {
      _warn("WARNING: dropping malformed IR frame %08" PRIx32 "\n", raw);
      return OK;
    }

  now = clock_systime_ticks();

  if (priv->have_last && priv->last_key == key &&
      TICK2MSEC(now - priv->last_tick) <= RK3576_IR_REPEAT_WINDOW_MS)
    {
      key |= RK3576_IR_KEY_REPEAT;
    }

  priv->last_key = key & ~RK3576_IR_KEY_REPEAT;
  priv->last_tick = now;
  priv->have_last = true;

  rk3576_ir_post(priv, key);
  return OK;
}

/****************************************************************************
 * Name: rk3576_ir_hw_setup
 *
 * Description:
 *   Bring the PWM channel up in power-key capture mode, following the
 *   sequence of TRM Part1 section 34.6.2.
 *
 ****************************************************************************/

static int rk3576_ir_hw_setup(struct rk3576_ir_dev_s *priv)
{
  uint32_t prescale;
  uint32_t scale;
  uint32_t regval;
  unsigned int i;
  int ret;

  /* Step 1: keep the channel disabled while it is reconfigured. */

  rk3576_ir_putreg(priv, RK3576_PWM_ENABLE,
                   PWM_HIWORD_CLR(PWM_ENABLE_EN | PWM_ENABLE_CLK_EN));

  /* Step 2: 1 MHz counting clock out of the 24 MHz oscillator branch. */

  ret = rk3576_ir_calc_divider(priv->fclk_hz, RK3576_IR_CAP_CLK_HZ, &prescale,
                               &scale);
  if (ret < 0)
    {
      _err("ERROR: cannot derive %d Hz from %" PRIu32 " Hz\n",
           RK3576_IR_CAP_CLK_HZ, priv->fclk_hz);
      return ret;
    }

  regval = ((prescale << PWM_CLK_PRESCALE_SHIFT) & PWM_CLK_PRESCALE_MASK) |
           ((scale << PWM_CLK_SCALE_SHIFT) & PWM_CLK_SCALE_MASK) |
           PWM_CLK_SRC_SEL_CLK_OSC;
  regval |= (PWM_CLK_PRESCALE_MASK | PWM_CLK_SCALE_MASK | PWM_CLK_SRC_SEL_MASK)
            << 16;

  rk3576_ir_putreg(priv, RK3576_PWM_CLK_CTRL, regval);

  /* Step 3: capture mode. */

  rk3576_ir_putreg(priv, RK3576_PWM_CTRL,
                   PWM_HIWORD(PWM_CTRL_MODE_CAPTURE) |
                       PWM_HIWORD_CLR(PWM_CTRL_MODE_MASK));

  /* Step 4: grant the power-key resource to this channel.  The arbiter is
   * shared by all channels of the controller and is a plain (non
   * write-masked) register.
   */

  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_ARBITER,
                   (1u << RK3576_IR_PWM_CHANNEL) << PWM_PWRMATCH_GRANT_SHIFT);

  /* Step 5: enable the power-key interrupt, mask everything else. */

  rk3576_ir_putreg(priv, RK3576_PWM_INTSTS, PWM_INT_ALL);
  rk3576_ir_putreg(priv, RK3576_PWM_INT_EN,
                   PWM_HIWORD(PWM_INT_PWR) |
                       PWM_HIWORD_CLR(PWM_INT_ALL & ~PWM_INT_PWR));
  rk3576_ir_putreg(priv, RK3576_PWM_INT_MASK, PWM_HIWORD_CLR(PWM_INT_ALL));

  /* Step 6: PWM_PWRMATCH_VALUE0..15 select the codes that assert the
   * hardware power-key match.  This driver reports every frame instead
   * (see pwrkey_int_ctrl below), so the match table is cleared.
   */

  for (i = 0; i < RK3576_PWM_PWRMATCH_NVALUES; i++)
    {
      rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_VALUE(i), 0);
    }

  /* Step 7: NEC bit-cell windows, in 1 MHz ticks. */

  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_LPRE,
                   RK3576_IR_THRESHOLD(RK3576_IR_NEC_LEADER_LOW_US));
  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_HPRE,
                   RK3576_IR_THRESHOLD(RK3576_IR_NEC_LEADER_HIGH_US));
  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_LD,
                   RK3576_IR_THRESHOLD(RK3576_IR_NEC_BIT_LOW_US));
  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_HD_ZERO,
                   RK3576_IR_THRESHOLD(RK3576_IR_NEC_ZERO_HIGH_US));
  rk3576_ir_putreg(priv, RK3576_PWM_PWRMATCH_HD_ONE,
                   RK3576_IR_THRESHOLD(RK3576_IR_NEC_ONE_HIGH_US));

  /* Steps 8/9: an IR photodiode receiver output idles high and pulls low
   * during a burst, so the input polarity is negative.  Capture directly
   * and interrupt on every frame rather than only on a match.
   */

  rk3576_ir_putreg(
      priv, RK3576_PWM_PWRMATCH_CTRL,
      PWM_HIWORD(PWM_PWRMATCH_POLARITY_NEG | PWM_PWRMATCH_CAPTURE_DIRECT |
                 PWM_PWRMATCH_INT_NO_MATCH | PWM_PWRMATCH_ENABLE));

  /* Step 10: the optional glitch filter is left disabled; the external
   * receiver module already demodulates and shapes the 38 kHz carrier.
   */

  /* Step 11: run. */

  rk3576_ir_putreg(priv, RK3576_PWM_ENABLE,
                   PWM_HIWORD(PWM_ENABLE_CLK_EN | PWM_ENABLE_EN));

  _info("IR: capture running, fclk=%" PRIu32 " Hz prescale=%" PRIu32
        " scale=%" PRIu32 "\n",
        priv->fclk_hz, prescale, scale);
  return OK;
}

/****************************************************************************
 * Name: rk3576_ir_read
 *
 * Description:
 *   Return queued key events.  The transfer size must be a multiple of
 *   sizeof(uint32_t); a blocking read waits for at least one event.
 *
 ****************************************************************************/

static ssize_t rk3576_ir_read(struct file *filep, char *buffer, size_t buflen)
{
  struct inode *inode = filep->f_inode;
  struct rk3576_ir_dev_s *priv = inode->i_private;
  irqstate_t flags;
  size_t nread = 0;
  int ret;

  if (buflen < sizeof(uint32_t))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->readlock);
  if (ret < 0)
    {
      return ret;
    }

  for (;;)
    {
      flags = spin_lock_irqsave(&priv->lock);

      while (priv->tail != priv->head && nread + sizeof(uint32_t) <= buflen)
        {
          uint32_t key = priv->queue[priv->tail];

          priv->tail = (priv->tail + 1) & RK3576_IR_QUEUE_MASK;
          spin_unlock_irqrestore(&priv->lock, flags);

          memcpy(buffer + nread, &key, sizeof(uint32_t));
          nread += sizeof(uint32_t);

          flags = spin_lock_irqsave(&priv->lock);
        }

      if (nread > 0)
        {
          spin_unlock_irqrestore(&priv->lock, flags);
          break;
        }

      if ((filep->f_oflags & O_NONBLOCK) != 0)
        {
          spin_unlock_irqrestore(&priv->lock, flags);
          ret = -EAGAIN;
          goto errout;
        }

      priv->waiting = true;
      spin_unlock_irqrestore(&priv->lock, flags);

      ret = nxsem_wait(&priv->waitsem);
      if (ret < 0)
        {
          goto errout;
        }
    }

  nxmutex_unlock(&priv->readlock);
  return (ssize_t)nread;

errout:
  nxmutex_unlock(&priv->readlock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_ir_poll
 ****************************************************************************/

static int rk3576_ir_poll(struct file *filep, struct pollfd *fds, bool setup)
{
  struct inode *inode = filep->f_inode;
  struct rk3576_ir_dev_s *priv = inode->i_private;
  irqstate_t flags;
  pollevent_t eventset = 0;
  int ret = OK;
  int i;

  flags = spin_lock_irqsave(&priv->lock);

  if (!setup)
    {
      struct pollfd **slot = (struct pollfd **)fds->priv;

      if (slot != NULL)
        {
          *slot = NULL;
          fds->priv = NULL;
        }

      goto out;
    }

  for (i = 0; i < RK3576_IR_NPOLLWAITERS; i++)
    {
      if (priv->fds[i] == NULL)
        {
          priv->fds[i] = fds;
          fds->priv = &priv->fds[i];
          break;
        }
    }

  if (i >= RK3576_IR_NPOLLWAITERS)
    {
      fds->priv = NULL;
      ret = -EBUSY;
      goto out;
    }

  if (priv->head != priv->tail)
    {
      eventset = POLLIN;
    }

out:
  spin_unlock_irqrestore(&priv->lock, flags);

  if (eventset != 0)
    {
      poll_notify(&fds, 1, eventset);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_ir_initialize
 *
 * Description:
 *   See rk3576_ir.h.
 *
 ****************************************************************************/

int rk3576_ir_initialize(const char *devpath)
{
  struct rk3576_ir_dev_s *priv = &g_rk3576_ir;
  int ret;

  if (devpath == NULL)
    {
      return -EINVAL;
    }

  if (priv->initialized)
    {
      return -EBUSY;
    }

  priv->base = RK3576_PWM0_ADDR + RK3576_IR_PWM_CHANNEL * RK3576_PWM_CH_STRIDE;
  priv->head = 0;
  priv->tail = 0;
  priv->have_last = false;

  ret = rk3576_ir_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_ir_hw_setup(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = irq_attach(RK3576_IRQ_PWM0_2CH_0, rk3576_ir_interrupt, priv);
  if (ret < 0)
    {
      _err("ERROR: failed to attach IR IRQ %d: %d\n", RK3576_IRQ_PWM0_2CH_0,
           ret);
      goto errout_disable;
    }

  up_enable_irq(RK3576_IRQ_PWM0_2CH_0);

  ret = register_driver(devpath, &g_rk3576_ir_fops, 0444, priv);
  if (ret < 0)
    {
      _err("ERROR: failed to register %s: %d\n", devpath, ret);
      up_disable_irq(RK3576_IRQ_PWM0_2CH_0);
      irq_detach(RK3576_IRQ_PWM0_2CH_0);
      goto errout_disable;
    }

  priv->initialized = true;
  _info("IR: %s ready\n", devpath);
  return OK;

errout_disable:
  rk3576_ir_putreg(priv, RK3576_PWM_ENABLE,
                   PWM_HIWORD_CLR(PWM_ENABLE_EN | PWM_ENABLE_CLK_EN));
  return ret;
}

/****************************************************************************
 * Name: rk3576_ir_register_callback
 *
 * Description:
 *   See rk3576_ir.h.
 *
 ****************************************************************************/

int rk3576_ir_register_callback(rk3576_ir_callback_t callback, void *arg)
{
  struct rk3576_ir_dev_s *priv = &g_rk3576_ir;
  irqstate_t flags;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  flags = spin_lock_irqsave(&priv->lock);
  priv->callback_arg = arg;
  priv->callback = callback;
  spin_unlock_irqrestore(&priv->lock, flags);

  return OK;
}

#endif /* CONFIG_RK3576_IR */
