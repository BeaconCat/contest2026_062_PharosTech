/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_mailbox.c
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
 * RK3576 mailbox driver.  The mailbox is the inter-processor doorbell used
 * by the AMP configuration of this board (this AP running openvela plus a
 * remote core), and is the transport underneath rptun/rpmsg: the payload
 * itself lives in shared memory, the mailbox only carries the 32-bit
 * command/data doorbell pair and raises the peer interrupt.
 *
 * Direction naming follows the IP: A2B is written here and interrupts the
 * remote core, B2A is written by the remote core and interrupts us.  Every
 * instance owns four independent channels per direction.
 *
 * There is no NuttX upper-half framework for a raw doorbell mailbox, so
 * this driver exposes a small in-kernel API (see rk3576_mailbox.h) that
 * rptun and other in-kernel users bind to directly; no /dev node is
 * created.
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
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_mailbox.h"
#include "rk3576_mailbox.h"

#ifdef CONFIG_RK3576_MAILBOX

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Receive binding of a single B2A channel. */

struct rk3576_mailbox_chan_s
{
  rk3576_mailbox_callback_t cb; /* Receive callback, NULL when unbound  */
  void *arg;                    /* Opaque callback argument             */
};

/* One mailbox instance. */

struct rk3576_mailbox_dev_s
{
  uintptr_t base;    /* Register block base address                  */
  int irq;           /* AP-side (B2A) GIC INTID                      */
  bool initialized;  /* rk3576_mailbox_initialize() has run          */
  spinlock_t lock;   /* Guards the register file and the bindings    */
  struct rk3576_mailbox_chan_s chan[RK3576_MAILBOX_NCHANNELS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_mailbox_getreg(struct rk3576_mailbox_dev_s *priv,
                                      unsigned int off);
static void rk3576_mailbox_putreg(struct rk3576_mailbox_dev_s *priv,
                                  unsigned int off, uint32_t val);
static struct rk3576_mailbox_dev_s *rk3576_mailbox_lookup(int instance);
static int rk3576_mailbox_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_mailbox_dev_s
  g_rk3576_mailbox[RK3576_MAILBOX_NINSTANCES];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mailbox_getreg
 ****************************************************************************/

static uint32_t rk3576_mailbox_getreg(struct rk3576_mailbox_dev_s *priv,
                                      unsigned int off)
{
  return getreg32(priv->base + off);
}

/****************************************************************************
 * Name: rk3576_mailbox_putreg
 ****************************************************************************/

static void rk3576_mailbox_putreg(struct rk3576_mailbox_dev_s *priv,
                                  unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_mailbox_lookup
 *
 * Description:
 *   Validate an instance index and return its device structure, filling in
 *   the static hardware description on first use.
 *
 ****************************************************************************/

static struct rk3576_mailbox_dev_s *rk3576_mailbox_lookup(int instance)
{
  struct rk3576_mailbox_dev_s *priv;

  if (instance < 0 || instance >= RK3576_MAILBOX_NINSTANCES)
    {
      return NULL;
    }

  priv = &g_rk3576_mailbox[instance];

  if (priv->base == 0)
    {
      priv->base = RK3576_MAILBOX_BASE(instance);
      priv->irq  = RK3576_MAILBOX_IRQ(instance);
    }

  return priv;
}

/****************************************************************************
 * Name: rk3576_mailbox_interrupt
 *
 * Description:
 *   AP-side interrupt handler.  Reads the B2A status word, and for every
 *   pending channel latches the command/data pair, clears the pending flag
 *   (write 1 to clear) and then runs the channel callback.  The flag is
 *   cleared before the callback runs so that a message posted by the peer
 *   while the callback executes is not lost.
 *
 ****************************************************************************/

static int rk3576_mailbox_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_mailbox_dev_s *priv =
    (struct rk3576_mailbox_dev_s *)arg;
  rk3576_mailbox_callback_t cb;
  irqstate_t flags;
  uint32_t status;
  uint32_t cmd;
  uint32_t data;
  void *cbarg;
  int chan;

  DEBUGASSERT(priv != NULL);
  UNUSED(irq);
  UNUSED(context);

  flags  = spin_lock_irqsave(&priv->lock);
  status = rk3576_mailbox_getreg(priv, RK3576_MAILBOX_B2A_STATUS) &
           RK3576_MAILBOX_CHAN_MASK;

  for (chan = 0; chan < RK3576_MAILBOX_NCHANNELS; chan++)
    {
      if ((status & RK3576_MAILBOX_CHAN_BIT(chan)) == 0)
        {
          continue;
        }

      cmd  = rk3576_mailbox_getreg(priv, RK3576_MAILBOX_B2A_CMD(chan));
      data = rk3576_mailbox_getreg(priv, RK3576_MAILBOX_B2A_DAT(chan));

      /* Acknowledge this channel (write 1 to clear). */

      rk3576_mailbox_putreg(priv, RK3576_MAILBOX_B2A_STATUS,
                            RK3576_MAILBOX_CHAN_BIT(chan));

      cb    = priv->chan[chan].cb;
      cbarg = priv->chan[chan].arg;

      spin_unlock_irqrestore(&priv->lock, flags);

      if (cb != NULL)
        {
          cb(cmd, data, cbarg);
        }
      else
        {
          _warn("mailbox: dropped message on unbound channel %d\n", chan);
        }

      flags = spin_lock_irqsave(&priv->lock);
    }

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mailbox_initialize
 *
 * Description:
 *   See rk3576_mailbox.h.
 *
 ****************************************************************************/

int rk3576_mailbox_initialize(int instance)
{
  struct rk3576_mailbox_dev_s *priv;
  irqstate_t flags;
  int ret;

  priv = rk3576_mailbox_lookup(instance);
  if (priv == NULL)
    {
      return -EINVAL;
    }

  if (priv->initialized)
    {
      return OK;
    }

  spin_lock_init(&priv->lock);
  memset(priv->chan, 0, sizeof(priv->chan));

  /* Mask every receive channel and drop any message left over from a
   * previous boot, then make sure we do not interrupt the remote core
   * either.  A2B_INTEN belongs to the remote side, so only the stale A2B
   * status is cleared here.
   */

  flags = spin_lock_irqsave(&priv->lock);

  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_B2A_INTEN, 0);
  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_B2A_STATUS,
                        RK3576_MAILBOX_CHAN_MASK);
  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_A2B_STATUS,
                        RK3576_MAILBOX_CHAN_MASK);

  spin_unlock_irqrestore(&priv->lock, flags);

  ret = irq_attach(priv->irq, rk3576_mailbox_interrupt, priv);
  if (ret < 0)
    {
      _err("mailbox%d: irq_attach(%d) failed: %d\n", instance, priv->irq,
            ret);
      return ret;
    }

  up_enable_irq(priv->irq);
  priv->initialized = true;

  _info("mailbox%d: base 0x%" PRIxPTR " irq %d ready\n", instance,
         priv->base, priv->irq);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mailbox_send
 *
 * Description:
 *   See rk3576_mailbox.h.
 *
 ****************************************************************************/

int rk3576_mailbox_send(int instance, int chan, uint32_t cmd,
                        uint32_t data)
{
  struct rk3576_mailbox_dev_s *priv;
  irqstate_t flags;

  priv = rk3576_mailbox_lookup(instance);
  if (priv == NULL || chan < 0 || chan >= RK3576_MAILBOX_NCHANNELS)
    {
      return -EINVAL;
    }

  if (!priv->initialized)
    {
      return -ENXIO;
    }

  flags = spin_lock_irqsave(&priv->lock);

  /* The data word must be in place before the command word is written:
   * writing A2B_CMD is what posts the message and raises the remote
   * interrupt.
   */

  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_A2B_DAT(chan), data);
  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_A2B_CMD(chan), cmd);

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mailbox_register_callback
 *
 * Description:
 *   See rk3576_mailbox.h.
 *
 ****************************************************************************/

int rk3576_mailbox_register_callback(int instance, int chan,
                                     rk3576_mailbox_callback_t cb,
                                     void *arg)
{
  struct rk3576_mailbox_dev_s *priv;
  irqstate_t flags;
  uint32_t inten;

  priv = rk3576_mailbox_lookup(instance);
  if (priv == NULL || chan < 0 || chan >= RK3576_MAILBOX_NCHANNELS)
    {
      return -EINVAL;
    }

  if (!priv->initialized)
    {
      return -ENXIO;
    }

  flags = spin_lock_irqsave(&priv->lock);

  priv->chan[chan].cb  = cb;
  priv->chan[chan].arg = arg;

  inten = rk3576_mailbox_getreg(priv, RK3576_MAILBOX_B2A_INTEN);

  if (cb != NULL)
    {
      /* Discard anything already pending on this channel so that a fresh
       * binding does not immediately receive a stale message.
       */

      rk3576_mailbox_putreg(priv, RK3576_MAILBOX_B2A_STATUS,
                            RK3576_MAILBOX_CHAN_BIT(chan));
      inten |= RK3576_MAILBOX_CHAN_BIT(chan);
    }
  else
    {
      inten &= ~RK3576_MAILBOX_CHAN_BIT(chan);
    }

  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_B2A_INTEN, inten);

  spin_unlock_irqrestore(&priv->lock, flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mailbox_trylock
 *
 * Description:
 *   See rk3576_mailbox.h.
 *
 ****************************************************************************/

bool rk3576_mailbox_trylock(int instance, int lock)
{
  struct rk3576_mailbox_dev_s *priv;

  priv = rk3576_mailbox_lookup(instance);
  if (priv == NULL || lock < 0 || lock >= RK3576_MAILBOX_NCHANNELS)
    {
      return false;
    }

  /* Reading the register both reports and takes the lock: a read of 0
   * means the lock was free and is now owned by this core.
   */

  return (rk3576_mailbox_getreg(priv, RK3576_MAILBOX_ATOMIC_LOCK(lock)) &
          RK3576_MAILBOX_ATOMIC_LOCK_BIT) == 0;
}

/****************************************************************************
 * Name: rk3576_mailbox_unlock
 *
 * Description:
 *   See rk3576_mailbox.h.
 *
 ****************************************************************************/

void rk3576_mailbox_unlock(int instance, int lock)
{
  struct rk3576_mailbox_dev_s *priv;

  priv = rk3576_mailbox_lookup(instance);
  if (priv == NULL || lock < 0 || lock >= RK3576_MAILBOX_NCHANNELS)
    {
      return;
    }

  rk3576_mailbox_putreg(priv, RK3576_MAILBOX_ATOMIC_LOCK(lock), 0);
}

#endif /* CONFIG_RK3576_MAILBOX */
