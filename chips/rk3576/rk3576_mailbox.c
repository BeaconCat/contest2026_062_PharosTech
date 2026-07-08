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
 * RK3576 mailbox doorbell for AMP rptun/rpmsg (openvela <-> Linux).
 *
 * openvela runs as the rpmsg remote; Linux is the master.  Rockchip's
 * rpmsg-over-mailbox link (arch/arm64/boot/dts/rockchip/rk3576-amp.dtsi:
 * mboxes = <&mailbox0 0 &mailbox3 0>, names "rpmsg-rx","rpmsg-tx") uses two
 * separate mailbox instances, one per direction.  The Linux mailbox driver
 * always transmits on A2B and receives on B2A, so Linux is the "A" endpoint
 * and openvela is the mirror "B" endpoint:
 *
 *   - TX (openvela -> Linux): write B2A_CMD/DATA of mailbox0.  Linux enabled
 *     its B2A receive interrupt on mailbox0 and reads it as an rx.
 *   - RX (Linux -> openvela): Linux writes A2B_CMD/DATA of mailbox3; openvela
 *     enables the A2B interrupt on mailbox3 (IRQ MAILBOX_AP3) and reads it.
 *
 * A message is a {cmd,data} pair; writing CMD raises the destination
 * interrupt.  The rpmsg layer above puts the link-id in cmd and the magic in
 * data (see rk3576_rptun.c); this driver is payload-agnostic.
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
 * Pre-processor Definitions
 ****************************************************************************/

/* openvela TX instance (mailbox0, B2A) and RX instance (mailbox3, A2B). */

#define RK3576_MBOX_TX_ADDR    RK3576_MAILBOX_ADDR   /* mailbox0 */
#define RK3576_MBOX_RX_ADDR    RK3576_MAILBOX3_ADDR  /* mailbox3 */
#define RK3576_MBOX_RX_IRQ     RK3576_IRQ_MAILBOX_AP3

/****************************************************************************
 * Private Data
 ****************************************************************************/

static rk3576_mbox_callback_t g_mbox_callback;
static void                  *g_mbox_arg;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t mbox_getreg(uintptr_t base, unsigned int offset)
{
  return getreg32(base + offset);
}

static inline void mbox_putreg(uint32_t value, uintptr_t base,
                               unsigned int offset)
{
  putreg32(value, base + offset);
}

/****************************************************************************
 * Name: rk3576_mailbox_interrupt
 *
 * Description:
 *   RX doorbell handler (Linux -> openvela, A2B of mailbox3).  Reads the
 *   command word, acknowledges (write-1-clear status) and forwards it to the
 *   registered rpmsg callback.
 ****************************************************************************/

static int rk3576_mailbox_interrupt(int irq, void *context, void *arg)
{
  uint32_t status = mbox_getreg(RK3576_MBOX_RX_ADDR, RK3576_MBOX_A2B_STATUS);
  uint32_t cmd;

  if ((status & RK3576_MBOX_CH0_EN) != 0)
    {
      cmd = mbox_getreg(RK3576_MBOX_RX_ADDR, RK3576_MBOX_A2B_CMD);

      /* Acknowledge (write 1 to clear the pending status). */

      mbox_putreg(RK3576_MBOX_CH0_EN, RK3576_MBOX_RX_ADDR,
                  RK3576_MBOX_A2B_STATUS);

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
 *   Kick Linux: write the command/data doorbell on the TX instance
 *   (mailbox0, B2A).  Writing CMD last raises the destination interrupt.
 ****************************************************************************/

void rk3576_mailbox_send(uint32_t cmd, uint32_t data)
{
  mbox_putreg(data, RK3576_MBOX_TX_ADDR, RK3576_MBOX_B2A_DATA);
  mbox_putreg(cmd, RK3576_MBOX_TX_ADDR, RK3576_MBOX_B2A_CMD);
}

/****************************************************************************
 * Name: rk3576_mailbox_register_callback
 *
 * Description:
 *   Register (or clear, with NULL) the handler invoked when Linux kicks us,
 *   and enable/disable the RX (mailbox3 A2B) doorbell interrupt accordingly.
 ****************************************************************************/

void rk3576_mailbox_register_callback(rk3576_mbox_callback_t callback,
                                      void *arg)
{
  g_mbox_callback = callback;
  g_mbox_arg      = arg;

  if (callback != NULL)
    {
      mbox_putreg(RK3576_MBOX_CH0_EN, RK3576_MBOX_RX_ADDR,
                  RK3576_MBOX_A2B_INTEN);
      up_enable_irq(RK3576_MBOX_RX_IRQ);
    }
  else
    {
      up_disable_irq(RK3576_MBOX_RX_IRQ);
      mbox_putreg(0, RK3576_MBOX_RX_ADDR, RK3576_MBOX_A2B_INTEN);
    }
}

/****************************************************************************
 * Name: rk3576_mailbox_initialize
 *
 * Description:
 *   Attach the RX mailbox interrupt (mailbox3).  The pclk_mailbox gate must
 *   already be on (enabled by the loader or the Linux AMP node); this routine
 *   does not touch the clock.
 ****************************************************************************/

int rk3576_mailbox_initialize(void)
{
  int ret;

  ret = irq_attach(RK3576_MBOX_RX_IRQ, rk3576_mailbox_interrupt, NULL);
  if (ret < 0)
    {
      mcerr("ERROR: mailbox irq_attach failed: %d\n", ret);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_RK3576_MAILBOX */
