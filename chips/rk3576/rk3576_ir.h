/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_ir.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_IR_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_IR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_IR

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Layout of one decoded NEC event, as returned by read() on the character
 * device and as passed to a registered callback:
 *
 *   bit 31      RK3576_IR_KEY_REPEAT  - frame is an auto-repeat of the
 *                                       previous key (button held down)
 *   bits 23..16 address
 *   bits  7.. 0 command
 *
 * Extended-NEC frames (16-bit address, no address/~address complement)
 * are reported with the full 16-bit address in bits 31..16 minus the
 * repeat flag; use RK3576_IR_ADDR()/RK3576_IR_CMD() to unpack.
 */

#define RK3576_IR_KEY_REPEAT (1u << 31)
#define RK3576_IR_ADDR_SHIFT 16
#define RK3576_IR_ADDR_MASK  0x7fff0000u
#define RK3576_IR_CMD_MASK   0x000000ffu

#define RK3576_IR_ADDR(key) \
  (((uint32_t)(key)&RK3576_IR_ADDR_MASK) >> RK3576_IR_ADDR_SHIFT)
#define RK3576_IR_CMD(key)       ((uint32_t)(key)&RK3576_IR_CMD_MASK)
#define RK3576_IR_IS_REPEAT(key) (((uint32_t)(key)&RK3576_IR_KEY_REPEAT) != 0)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Key-event callback.  Invoked from interrupt context, so it must not
 * block and must not call any API that can sleep.
 *
 * Input Parameters:
 *   key - Decoded event, see the RK3576_IR_* accessors above.
 *   arg - Opaque value supplied at registration time.
 */

typedef void (*rk3576_ir_callback_t)(uint32_t key, void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_ir_initialize
 *
 * Description:
 *   Bring up the infrared receiver (PWM0 channel 0 in power-key capture
 *   mode) and register a character device that returns decoded NEC key
 *   events as a stream of uint32_t values.  The board must mux the
 *   PWM0_CH0 pin to its IR-input function before calling this.
 *
 * Input Parameters:
 *   devpath - Character device path to create, e.g. "/dev/ir0".
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_ir_initialize(const char *devpath);

/****************************************************************************
 * Name: rk3576_ir_register_callback
 *
 * Description:
 *   Install (or, with a NULL callback, remove) a function called on every
 *   decoded key event.  Only one callback is supported; registering a new
 *   one replaces the previous.  The callback runs in interrupt context.
 *
 *   Events are still queued for the character device while a callback is
 *   installed, so both interfaces can be used at the same time.
 *
 * Input Parameters:
 *   callback - Function to call, or NULL to detach.
 *   arg      - Opaque value passed back to the callback.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value if the driver has not been
 *   initialized yet.
 *
 ****************************************************************************/

int rk3576_ir_register_callback(rk3576_ir_callback_t callback, void *arg);

#endif /* CONFIG_RK3576_IR */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_IR_H */
