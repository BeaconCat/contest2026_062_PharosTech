/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_mailbox.c
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
 * The RK3576 mailbox is a one-command/one-data doorbell used for inter-core
 * notification in an AMP setup (openvela on one cluster, Linux on another).
 * It carries the rptun/rpmsg "kick": the local core writes A2B_CMD/DATA and
 * raises the destination interrupt; the remote core does the same on B2A and
 * this core takes an IRQ.  The payload (command) identifies the virtqueue to
 * process; the actual data lives in the shared-memory vrings.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_mailbox.h"
#include "rk3576_mailbox.h"

#ifdef CONFIG_RK3576_MAILBOX

/****************************************************************************
 * Private Data
 ****************************************************************************/

static rk3576_mbox_callback_t g_mbox_callback;
static void                  *g_mbox_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t mbox_getreg(unsigned int offset)
{
  return getreg32(RK3576_MAILBOX_ADDR + offset);
}

static inline void mbox_putreg(uint32_t value, unsigned int offset)
{
  putreg32(value, RK3576_MAILBOX_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_mailbox_interrupt
 *
 * Description:
 *   Remote-to-local (B2A) doorbell handler.  Reads the command word,
 *   acknowledges the interrupt (write-1-clear status) and forwards the
 *   command to the registered rptun callback.
 ****************************************************************************/

static int rk3576_mailbox_interrupt(int irq, void *context, void *arg)
{
  uint32_t status = mbox_getreg(RK3576_MBOX_B2A_STATUS);
  uint32_t cmd;

  if ((status & RK3576_MBOX_CH0_EN) != 0)
    {
      cmd = mbox_getreg(RK3576_MBOX_B2A_CMD);

      /* Acknowledge (write 1 to clear the pending status). */

      mbox_putreg(RK3576_MBOX_CH0_EN, RK3576_MBOX_B2A_STATUS);

      if (g_mbox_callback != NULL)
        {
          g_mbox_callback(g_mbox_arg, cmd);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mailbox_send
 *
 * Description:
 *   Kick the remote core: write the command/data doorbell and raise the
 *   A2B interrupt on the destination.
 ****************************************************************************/

void rk3576_mailbox_send(uint32_t cmd, uint32_t data)
{
  mbox_putreg(data, RK3576_MBOX_A2B_DATA);
  mbox_putreg(cmd, RK3576_MBOX_A2B_CMD);
  mbox_putreg(RK3576_MBOX_CH0_EN, RK3576_MBOX_A2B_INTEN);
}

/****************************************************************************
 * Name: rk3576_mailbox_register_callback
 *
 * Description:
 *   Register (or clear, with NULL) the handler invoked when the remote core
 *   kicks us, and enable/disable the B2A doorbell interrupt accordingly.
 ****************************************************************************/

void rk3576_mailbox_register_callback(rk3576_mbox_callback_t callback,
                                      void *arg)
{
  g_mbox_callback = callback;
  g_mbox_arg      = arg;

  if (callback != NULL)
    {
      mbox_putreg(RK3576_MBOX_CH0_EN, RK3576_MBOX_B2A_INTEN);
      up_enable_irq(RK3576_IRQ_MAILBOX_AP0);
    }
  else
    {
      up_disable_irq(RK3576_IRQ_MAILBOX_AP0);
      mbox_putreg(0, RK3576_MBOX_B2A_INTEN);
    }
}

/****************************************************************************
 * Name: rk3576_mailbox_initialize
 *
 * Description:
 *   Attach the mailbox interrupt.  The pclk_mailbox gate must already be on
 *   (enabled by the loader or the CRU bring-up); this routine does not touch
 *   the clock.
 *
 * TODO: enable pclk_mailbox via the CRU if the loader leaves it gated (the
 * vendor DTS marks the node disabled).
 ****************************************************************************/

int rk3576_mailbox_initialize(void)
{
  int ret;

  ret = irq_attach(RK3576_IRQ_MAILBOX_AP0, rk3576_mailbox_interrupt, NULL);
  if (ret < 0)
    {
      mcerr("ERROR: mailbox irq_attach failed: %d\n", ret);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_RK3576_MAILBOX */
