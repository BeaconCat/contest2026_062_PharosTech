/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_mailbox.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_MAILBOX_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_MAILBOX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Invoked in interrupt context when the remote core kicks us; cmd is the
 * command word from the doorbell (used as the rptun notify id).
 */

typedef void (*rk3576_mbox_callback_t)(void *arg, uint32_t cmd);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Kick the remote core with a command/data doorbell. */

void rk3576_mailbox_send(uint32_t cmd, uint32_t data);

/* Register/clear the remote-kick callback and enable/disable the interrupt. */

void rk3576_mailbox_register_callback(rk3576_mbox_callback_t callback,
                                      void *arg);

/* Attach the mailbox interrupt (clock must already be enabled). */

int rk3576_mailbox_initialize(void);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_MAILBOX_H */
