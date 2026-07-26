/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_hym8563.h
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

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HYM8563_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HYM8563_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/i2c/i2c_master.h>

#ifdef CONFIG_KICKPI_K7_HYM8563

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C slave address of the HYM8563 on the KICKPI-K7 (i2c2, hym8563@51) */

#define KICKPI_K7_HYM8563_I2C_ADDR 0x51

/* I2C bus the RTC is wired to (i2c@2AC50000) */

#define KICKPI_K7_HYM8563_I2C_BUS 2

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: kickpi_k7_hym8563_initialize
 *
 * Description:
 *   Bind the on-board HYM8563 RTC to the given I2C master and register it
 *   with the NuttX RTC upper half as /dev/rtc0.
 *
 *   The 32.768 kHz CLKOUT of this part feeds the on-board SV6621 WiFi/BT
 *   companion chip.  This function only verifies that CLKOUT is enabled and
 *   re-enables it if it is not; it never disables it.
 *
 * Input Parameters:
 *   i2c - An initialized I2C master interface for the bus the RTC sits on
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_hym8563_initialize(struct i2c_master_s *i2c);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_KICKPI_K7_HYM8563 */
#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HYM8563_H */
