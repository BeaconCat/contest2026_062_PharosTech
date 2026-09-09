/****************************************************************************
 * chips/rk3576/rk3576_mailbox.c
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
 *   - TX data available (openvela -> Linux): write B2A_CMD/DATA of mailbox0.
 *     Linux enabled its B2A receive interrupt on mailbox0 and reads it as rx.
 *   - RX buffer consumed (openvela -> Linux): write B2A_CMD/DATA of mailbox3.
 *     Newer Linux masters use this to reclaim their transmit buffers.
 *   - RX (Linux -> openvela): Linux writes A2B_CMD/DATA of mailbox3;
 *     openvela enables its A2B interrupt output (MAILBOX_BB3) and reads it.
 *
 * A message is a {cmd,data} pair.  RK3576 mailbox v2 defaults to data-write
 * triggering: write CMD first and DATA last.  The rpmsg layer puts the
 * link-id in cmd and the magic in data; this driver remains payload-agnostic.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_mailbox.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_mailbox.h"

#ifdef CONFIG_RK3576_MAILBOX

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_mailbox_getreg(uintptr_t base,
                                             unsigned int offset);
static inline void rk3576_mailbox_putreg(uint32_t value, uintptr_t base,
                                         unsigned int offset);
static uintptr_t rk3576_mailbox_base(unsigned int instance);
static int rk3576_mailbox_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static rk3576_mailbox_callback_t g_mbox_callback;
static void *g_mbox_arg;
static uintptr_t g_mbox_rx_base;
static int g_mbox_rx_irq = -1;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_mailbox_getreg(uintptr_t base,
                                             unsigned int offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_mailbox_putreg(uint32_t value, uintptr_t base,
                                         unsigned int offset)
{
  putreg32(value, base + offset);
}

static uintptr_t rk3576_mailbox_base(unsigned int instance)
{
  if (instance >= RK3576_MAILBOX_COUNT)
    {
      return 0;
    }

  return RK3576_MAILBOX_BASE + instance * RK3576_MAILBOX_STRIDE;
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
  uint32_t status =
      rk3576_mailbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_STATUS);
  uint32_t cmd;
  uint32_t data;

  (void)irq;
  (void)context;
  (void)arg;

  if ((status & RK3576_MBOX_INT_MASK) != 0)
    {
      cmd = rk3576_mailbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_CMD);
      data = rk3576_mailbox_getreg(g_mbox_rx_base, RK3576_MBOX_A2B_DATA);

      /* Acknowledge (write 1 to clear the pending status). */

      rk3576_mailbox_putreg(RK3576_MBOX_INT_MASK, g_mbox_rx_base,
                            RK3576_MBOX_A2B_STATUS);

      if (g_mbox_callback != NULL)
        {
          g_mbox_callback(g_mbox_arg, cmd, data);
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
 *   Kick Linux through the selected mailbox instance's B2A doorbell.
 *   Writing DATA last raises the destination interrupt.
 *
 ****************************************************************************/

int rk3576_mailbox_send(unsigned int instance, uint32_t cmd, uint32_t data)
{
  uintptr_t base = rk3576_mailbox_base(instance);

  if (base == 0)
    {
      return -EINVAL;
    }

  /* Select data-write trigger without changing the peer's RX enable bit. */

  rk3576_mailbox_putreg(RK3576_MBOX_TRIGGER_UPDATE | RK3576_MBOX_TRIGGER_MASK,
                        base, RK3576_MBOX_B2A_INTEN);

  if ((rk3576_mailbox_getreg(base, RK3576_MBOX_B2A_STATUS) &
       RK3576_MBOX_INT_MASK) != 0)
    {
      return -EBUSY;
    }

  rk3576_mailbox_putreg(cmd, base, RK3576_MBOX_B2A_CMD);
  UP_DMB();
  rk3576_mailbox_putreg(data, base, RK3576_MBOX_B2A_DATA);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mailbox_register_callback
 *
 * Description:
 *   Register (or clear, with NULL) the handler invoked when Linux kicks us,
 *   and enable/disable the RX (mailbox3 A2B) doorbell interrupt accordingly.
 ****************************************************************************/

void rk3576_mailbox_register_callback(rk3576_mailbox_callback_t callback,
                                      void *arg)
{
  g_mbox_callback = callback;
  g_mbox_arg = arg;

  if (callback != NULL)
    {
      UP_DMB();
      rk3576_mailbox_putreg(RK3576_MBOX_INT_UPDATE | RK3576_MBOX_INT_MASK,
                            g_mbox_rx_base, RK3576_MBOX_A2B_INTEN);
      up_enable_irq(g_mbox_rx_irq);
    }
  else
    {
      up_disable_irq(g_mbox_rx_irq);
      rk3576_mailbox_putreg(RK3576_MBOX_INT_UPDATE, g_mbox_rx_base,
                            RK3576_MBOX_A2B_INTEN);
    }
}

/****************************************************************************
 * Name: rk3576_mailbox_initialize
 *
 * Description:
 *   Enable the mailbox clock and attach its RX interrupt (mailbox3), while
 *   preserving any startup notification already sent by the Linux peer.
 ****************************************************************************/

int rk3576_mailbox_initialize(unsigned int rx_instance)
{
  uintptr_t base;
  int ret;

  base = rk3576_mailbox_base(rx_instance);
  if (base == 0)
    {
      return -EINVAL;
    }

  g_mbox_rx_base = base;
  g_mbox_rx_irq = RK3576_IRQ_MAILBOX_BB0 + rx_instance;

  /* The remote may start before Linux probes its clock controller.  Keep the
   * mailbox usable by enabling PCLK_MAILBOX0 directly (gate high means off).
   */

  putreg32(1u << (RK3576_CRU_MAILBOX_GATE_BIT + 16),
           RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(RK3576_CRU_MAILBOX_GATE_CON));

  rk3576_mailbox_putreg(RK3576_MBOX_INT_UPDATE, base, RK3576_MBOX_A2B_INTEN);

  /* N-Boot clears stale status before starting either OS.  Preserve a
   * pending Linux startup notification until the callback is installed.
   */

  ret = irq_attach(g_mbox_rx_irq, rk3576_mailbox_interrupt, NULL);
  if (ret < 0)
    {
      mcerr("ERROR: mailbox irq_attach failed: %d\n", ret);
      return ret;
    }

  return OK;
}

#endif /* CONFIG_RK3576_MAILBOX */
