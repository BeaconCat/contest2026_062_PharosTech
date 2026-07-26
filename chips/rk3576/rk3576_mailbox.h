/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_mailbox.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_MAILBOX_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_MAILBOX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_MAILBOX

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mailbox_callback_t
 *
 * Description:
 *   Receive callback, invoked from interrupt context once per incoming
 *   B2A message.  It must not block.
 *
 * Input Parameters:
 *   cmd  - Command word of the message (B2A_CMD).
 *   data - Data word of the message (B2A_DAT).
 *   arg  - Opaque argument given at registration time.
 *
 ****************************************************************************/

typedef void (*rk3576_mailbox_callback_t)(uint32_t cmd, uint32_t data,
                                          void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mailbox_initialize
 *
 * Description:
 *   Bring up one mailbox instance: mask and clear all channels, attach the
 *   AP-side (B2A) interrupt and enable it at the GIC.  Receive interrupts
 *   of an individual channel stay masked until a callback is registered
 *   for it.  Calling this twice on the same instance is harmless and
 *   returns OK without disturbing registered callbacks.
 *
 * Input Parameters:
 *   instance - Mailbox instance index, 0 .. RK3576_MAILBOX_NINSTANCES - 1.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mailbox_initialize(int instance);

/****************************************************************************
 * Name: rk3576_mailbox_send
 *
 * Description:
 *   Post one message to the remote core over an A2B channel.  The data
 *   word is written first, then the command word, which is what raises the
 *   remote interrupt.  The call does not wait for the peer to consume the
 *   message; the caller must not reuse the channel until the peer has
 *   acknowledged it (the hardware provides no full/empty flag on A2B).
 *
 * Input Parameters:
 *   instance - Mailbox instance index.
 *   chan     - Channel index, 0 .. RK3576_MAILBOX_NCHANNELS - 1.
 *   cmd      - Command word.
 *   data     - Data word.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mailbox_send(int instance, int chan, uint32_t cmd, uint32_t data);

/****************************************************************************
 * Name: rk3576_mailbox_register_callback
 *
 * Description:
 *   Attach (or, with cb == NULL, detach) the receive callback of one B2A
 *   channel.  Attaching unmasks the channel interrupt, detaching masks it.
 *
 * Input Parameters:
 *   instance - Mailbox instance index.
 *   chan     - Channel index, 0 .. RK3576_MAILBOX_NCHANNELS - 1.
 *   cb       - Callback invoked in interrupt context, or NULL to detach.
 *   arg      - Opaque argument passed back to the callback.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mailbox_register_callback(int instance, int chan,
                                     rk3576_mailbox_callback_t cb, void *arg);

/****************************************************************************
 * Name: rk3576_mailbox_trylock / rk3576_mailbox_unlock
 *
 * Description:
 *   Take / release one of the four hardware atomic locks of an instance.
 *   These are provided by the IP so that the two cores can arbitrate
 *   access to a shared resource (typically a shared-memory ring) without
 *   needing coherent exclusive monitors across the AMP boundary.
 *
 * Input Parameters:
 *   instance - Mailbox instance index.
 *   lock     - Lock index, 0 .. RK3576_MAILBOX_NCHANNELS - 1.
 *
 * Returned Value:
 *   rk3576_mailbox_trylock() returns true when the lock was taken.
 *
 ****************************************************************************/

bool rk3576_mailbox_trylock(int instance, int lock);
void rk3576_mailbox_unlock(int instance, int lock);

#endif /* CONFIG_RK3576_MAILBOX */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_MAILBOX_H */
