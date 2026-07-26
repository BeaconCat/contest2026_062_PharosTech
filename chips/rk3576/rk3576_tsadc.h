/****************************************************************************
 * chips/rk3576/rk3576_tsadc.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "hardware/rk3576_tsadc.h"

#ifdef CONFIG_RK3576_TSADC

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Over-temperature notification, invoked from interrupt context.
 *
 * chanmask       - Bit n set means channel n crossed its high-temperature
 *                  interrupt threshold (see RK3576_TSADC_CH_*).
 * shutmask       - Bit n set means channel n crossed its thermal-shutdown
 *                  threshold.  If shutdown routing to the CRU is enabled
 *                  the SoC is reset by hardware shortly afterwards.
 * arg            - Opaque value supplied at registration time.
 */

typedef void (*rk3576_tsadc_alarm_cb_t)(uint32_t chanmask, uint32_t shutmask,
                                        void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_tsadc_initialize
 *
 * Description:
 *   Bring up the TS-ADC controller: program the conversion period, the
 *   per-channel high-temperature interrupt and thermal-shutdown thresholds,
 *   attach the interrupt handler and start auto (loop) conversion mode.
 *
 *   Calling this more than once is harmless; subsequent calls return OK
 *   without touching the hardware.
 *
 * Input Parameters:
 *   chanmask - Bit mask of channels to convert, or 0 for all six channels.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_tsadc_initialize(uint32_t chanmask);

/****************************************************************************
 * Name: rk3576_tsadc_get_temp
 *
 * Description:
 *   Read the most recent conversion result of one channel and convert it
 *   to a temperature.  Intended for thermal throttling / DVFS callers that
 *   do not want to go through the sensor character device.
 *
 * Input Parameters:
 *   ch            - Channel index, one of RK3576_TSADC_CH_*.
 *   millicelsius  - [out] Receives the temperature in milli-degrees
 *                   Celsius (e.g. 45500 for 45.5 C).
 *
 * Returned Value:
 *   OK on success; -EINVAL for a bad channel or NULL pointer; -ENODEV if
 *   the controller has not been initialized; -EAGAIN if the channel has
 *   not produced a conversion result yet.
 *
 ****************************************************************************/

int rk3576_tsadc_get_temp(int ch, int *millicelsius);

/****************************************************************************
 * Name: rk3576_tsadc_set_alarm_cb
 *
 * Description:
 *   Install (or, with cb == NULL, remove) the over-temperature callback.
 *   The callback runs in interrupt context and must not block.
 *
 * Input Parameters:
 *   cb  - Callback to invoke, or NULL to disable notification.
 *   arg - Opaque value passed back to the callback.
 *
 * Returned Value:
 *   OK.
 *
 ****************************************************************************/

int rk3576_tsadc_set_alarm_cb(rk3576_tsadc_alarm_cb_t cb, void *arg);

#ifdef CONFIG_SENSORS
/****************************************************************************
 * Name: rk3576_tsadc_register
 *
 * Description:
 *   Register one TS-ADC channel with the NuttX sensor framework as a
 *   SENSOR_TYPE_AMBIENT_TEMPERATURE device (/dev/uorb/sensor_tempN).
 *   rk3576_tsadc_initialize() must have been called first.
 *
 * Input Parameters:
 *   ch    - Channel index, one of RK3576_TSADC_CH_*.
 *   devno - Sensor device minor number.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_tsadc_register(int ch, int devno);
#endif /* CONFIG_SENSORS */

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_TSADC */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_TSADC_H */
