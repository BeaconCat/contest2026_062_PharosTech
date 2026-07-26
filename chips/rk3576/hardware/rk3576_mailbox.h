/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_mailbox.h
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
 * The RK3576 mailbox is the classic Rockchip mailbox IP: a pair of
 * unidirectional 4-channel message queues between two CPUs.  Each channel
 * carries a 32-bit command word and a 32-bit data word.
 *
 *   A2B - written by "side A" (this AP), raises the interrupt of side B.
 *   B2A - written by "side B" (the remote core), raises the AP interrupt.
 *
 * Writing the CMD register of a channel is what actually posts the message
 * and raises the peer interrupt, so the DAT register must always be written
 * first.  The receiver clears the pending flag by writing 1 to the matching
 * bit of its STATUS register (write-1-to-clear).
 *
 * The SoC instantiates 14 identical mailboxes at 0x2ae50000, one every
 * 0x1000 (0x2ae50000 .. 0x2ae5d000).  Instance n raises AP interrupt
 * GIC SPI (125 + n), i.e. GIC INTID (157 + n) since INTID = SPI + 32.
 * The DTS ("rockchip,rk3576-mailbox") only maps the first 0x20 bytes of
 * each window because Linux does not use the atomic-lock registers; the
 * full register file extends to 0x110.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MAILBOX_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MAILBOX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller geometry ******************************************************/

/* Base address of mailbox instance 0.  Not (yet) present in
 * rk3576_memorymap.h; defined here so the driver builds standalone.
 */

#ifndef RK3576_MAILBOX0_ADDR
#  define RK3576_MAILBOX0_ADDR 0x2ae50000
#endif

#define RK3576_MAILBOX_STRIDE     0x1000 /* Distance between instances    */
#define RK3576_MAILBOX_NINSTANCES 14     /* 0x2ae50000 .. 0x2ae5d000      */
#define RK3576_MAILBOX_NCHANNELS  4      /* Channels per direction        */

/* Register block base of instance n (n = 0 .. 13). */

#define RK3576_MAILBOX_BASE(n) \
  (RK3576_MAILBOX0_ADDR + ((n) * RK3576_MAILBOX_STRIDE))

/* Register offsets *********************************************************/

#define RK3576_MAILBOX_A2B_INTEN  0x0000 /* A2B interrupt enable          */
#define RK3576_MAILBOX_A2B_STATUS 0x0004 /* A2B interrupt status (W1C)    */
#define RK3576_MAILBOX_A2B_CMD(i) (0x0008 + ((i) * 8)) /* A2B command  */
#define RK3576_MAILBOX_A2B_DAT(i) (0x000c + ((i) * 8)) /* A2B data     */

#define RK3576_MAILBOX_B2A_INTEN  0x0028 /* B2A interrupt enable          */
#define RK3576_MAILBOX_B2A_STATUS 0x002c /* B2A interrupt status (W1C)    */
#define RK3576_MAILBOX_B2A_CMD(i) (0x0030 + ((i) * 8)) /* B2A command  */
#define RK3576_MAILBOX_B2A_DAT(i) (0x0034 + ((i) * 8)) /* B2A data     */

#define RK3576_MAILBOX_ATOMIC_LOCK(i) (0x0100 + ((i) * 4))

/* Bit definitions **********************************************************/

/* A2B_INTEN / A2B_STATUS / B2A_INTEN / B2A_STATUS: one bit per channel. */

#define RK3576_MAILBOX_CHAN_BIT(i) (1u << (i))
#define RK3576_MAILBOX_CHAN_MASK   0x0000000fu

/* ATOMIC_LOCK: bit 0 is the lock flag.  A read returns 0 and takes the
 * lock, or returns 1 if the lock was already held.  Writing 0 releases it.
 */

#define RK3576_MAILBOX_ATOMIC_LOCK_BIT (1u << 0)

/* Interrupt numbers ********************************************************/

/* GIC INTID of the AP-side (B2A) interrupt of instance n.
 * DTS: mailbox@2ae50000 interrupts = <0 0x7d 4> -> SPI 125 -> INTID 157.
 * chips/rk3576/include/irq.h already provides RK3576_IRQ_MAILBOX_AP0..13.
 */

#define RK3576_MAILBOX_IRQ(n) (RK3576_IRQ_MAILBOX_AP0 + (n))

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MAILBOX_H */
