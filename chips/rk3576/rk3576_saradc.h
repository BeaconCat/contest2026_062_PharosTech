/****************************************************************************
 * chips/rk3576/rk3576_saradc.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SARADC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SARADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include <nuttx/analog/adc.h>

#include "hardware/rk3576_saradc.h"

#ifdef CONFIG_RK3576_SARADC

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_saradc_initialize
 *
 * Description:
 *   Bring up the SAR-ADC (clocks, analog trim, converter timing) and return
 *   the NuttX upper-half device structure so the board can hand it to
 *   adc_register().  Calling this more than once is harmless: the hardware
 *   is only initialised on the first call and the same singleton device is
 *   returned afterwards.
 *
 *   The analog inputs are dedicated SARADC_IN pins, so no pin muxing is
 *   required from the board.
 *
 * Input Parameters:
 *   chanmask - Bit mask of the channels the character device should sample
 *              on an ANIOC_TRIGGER, bit n selecting SARADC channel n
 *              (0 .. RK3576_SARADC_NCHANNELS - 1).  Must be non-zero.
 *
 * Returned Value:
 *   A pointer to the adc_dev_s on success; NULL on an invalid mask or a
 *   hardware bring-up failure.
 *
 ****************************************************************************/

struct adc_dev_s *rk3576_saradc_initialize(uint32_t chanmask);

/****************************************************************************
 * Name: rk3576_saradc_register
 *
 * Description:
 *   Convenience wrapper that runs rk3576_saradc_initialize() and registers
 *   the resulting device at 'devpath' (typically "/dev/adc0").
 *
 * Input Parameters:
 *   devpath  - Character device path to create.
 *   chanmask - Channel bit mask, see rk3576_saradc_initialize().
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_saradc_register(const char *devpath, uint32_t chanmask);

/****************************************************************************
 * Name: rk3576_saradc_read_channel
 *
 * Description:
 *   Perform one blocking single-shot conversion on 'ch' and return the raw
 *   12-bit code.  The conversion is polled rather than interrupt driven, so
 *   this may be used by board logic (for example head-set key detection on
 *   channel 3) independently of, and even before, the character device is
 *   registered.  The call is serialised against the character-device path
 *   by the driver's bus mutex, and therefore must not be issued from an
 *   interrupt handler.
 *
 * Input Parameters:
 *   ch  - SARADC channel, 0 .. RK3576_SARADC_NCHANNELS - 1.
 *   val - [out] Raw conversion result, 0 .. 4095.
 *
 * Returned Value:
 *   OK on success; -EINVAL on a bad channel or NULL pointer; -ETIMEDOUT if
 *   the converter did not report end-of-conversion in time.
 *
 ****************************************************************************/

int rk3576_saradc_read_channel(int ch, uint16_t *val);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_SARADC */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SARADC_H */
