/****************************************************************************
 * arch/arm64/src/rk3576/hardware/rk3576_mailbox.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MAILBOX_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MAILBOX_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 mailbox register offsets (TRM Part1 V1.2).  Each mailbox instance
 * carries one A2B and one B2A channel, a command + data doorbell each; the
 * INTEN bit gates the destination-side interrupt.  A2B = local -> remote,
 * B2A = remote -> local (from this core's point of view).
 */

#define RK3576_MBOX_A2B_INTEN     0x0000
#define RK3576_MBOX_A2B_STATUS    0x0004
#define RK3576_MBOX_A2B_CMD       0x0008
#define RK3576_MBOX_A2B_DATA      0x000c
#define RK3576_MBOX_B2A_INTEN     0x0010
#define RK3576_MBOX_B2A_STATUS    0x0014
#define RK3576_MBOX_B2A_CMD       0x0018
#define RK3576_MBOX_B2A_DATA      0x001c

/* Single-channel enable bit (INTEN reset value is 0x100 = channel 0 en). */

#define RK3576_MBOX_CH0_EN        (1 << 0)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MAILBOX_H */
