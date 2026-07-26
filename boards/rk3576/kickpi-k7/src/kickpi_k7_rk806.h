/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_rk806.h
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

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_RK806_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_RK806_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C address and bus of the RK806 companion PMIC on KICKPI-K7.
 * Source: reverse engineered vendor device tree, node
 * /i2c@2ac40000/pmic@23, compatible "rockchip,rk806".
 */

#define KICKPI_K7_RK806_I2C_BUS  1
#define KICKPI_K7_RK806_I2C_ADDR 0x23
#define KICKPI_K7_RK806_I2C_FREQ 400000

/* Regulator identifiers.  The ordering is the RK806 hardware ordering
 * (BUCK1..10, NLDO1..5, PLDO1..6) and is used as the regulator_desc_s "id".
 */

enum kickpi_k7_rk806_id_e
{
  RK806_ID_DCDC1 = 0, /* vdd_cpu_big_s0                                 */
  RK806_ID_DCDC2,     /* vdd_npu_s0                                     */
  RK806_ID_DCDC3,     /* vdd_cpu_lit_s0                                 */
  RK806_ID_DCDC4,     /* vcc_3v3_s3                                     */
  RK806_ID_DCDC5,     /* vdd_gpu_s0                                     */
  RK806_ID_DCDC6,     /* vddq_ddr_s0                                    */
  RK806_ID_DCDC7,     /* vdd_logic_s0                                   */
  RK806_ID_DCDC8,     /* vcc_1v8_s3                                     */
  RK806_ID_DCDC9,     /* vdd2_ddr_s3                                    */
  RK806_ID_DCDC10,    /* vdd_ddr_s0                                     */
  RK806_ID_NLDO1,     /* vdd_0v75_s3                                    */
  RK806_ID_NLDO2,     /* vdda_ddr_pll_s0                                */
  RK806_ID_NLDO3,     /* vdda0v75_hdmi_s0                               */
  RK806_ID_NLDO4,     /* vdda_0v85_s0                                   */
  RK806_ID_NLDO5,     /* vdda_0v75_s0                                   */
  RK806_ID_PLDO1,     /* vcca_1v8_s0                                    */
  RK806_ID_PLDO2,     /* vcca1v8_pldo2_s0                               */
  RK806_ID_PLDO3,     /* vdda_1v2_s0                                    */
  RK806_ID_PLDO4,     /* vcca_3v3_s0                                    */
  RK806_ID_PLDO5,     /* vccio_sd_s0 - SD/SDIO IO rail, 1.8 V / 3.3 V   */
  RK806_ID_PLDO6,     /* vcca1v8_pldo6_s3                               */
  RK806_ID_NREGULATORS
};

/* Convenience voltages for the SD/SDIO IO rail (PLDO5, vccio_sd_s0).
 * SD spec signalling levels; used by the CMD11 voltage switch sequence.
 */

#define KICKPI_K7_VCCIO_SD_3V3 3300000
#define KICKPI_K7_VCCIO_SD_1V8 1800000

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: kickpi_k7_rk806_initialize
 *
 * Description:
 *   Probe the RK806 PMIC on the given I2C master and register every board
 *   power rail with the NuttX regulator framework.  Rail names match the
 *   vendor device tree ("vccio_sd_s0", "vdd_gpu_s0", ...) so that consumers
 *   can look them up with regulator_get().
 *
 * Input Parameters:
 *   i2c - An initialized I2C master bound to I2C1 (0x2ac40000).
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_initialize(struct i2c_master_s *i2c);

/****************************************************************************
 * Name: kickpi_k7_rk806_set_voltage_uv
 *
 * Description:
 *   Set the output voltage of one rail, bypassing the regulator consumer
 *   reference counting.  Intended for low level board code (for example the
 *   SD signal voltage switch) that owns the rail exclusively.
 *
 * Input Parameters:
 *   id - One of enum kickpi_k7_rk806_id_e.
 *   uv - Requested voltage in microvolts.  Rounded up to the next
 *        programmable step and rejected if outside the rail constraints.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_set_voltage_uv(int id, int uv);

/****************************************************************************
 * Name: kickpi_k7_rk806_get_voltage_uv
 *
 * Description:
 *   Read back the programmed output voltage of one rail.
 *
 * Input Parameters:
 *   id - One of enum kickpi_k7_rk806_id_e.
 *
 * Returned Value:
 *   The voltage in microvolts (> 0) on success; a negated errno value on
 *   failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_get_voltage_uv(int id);

/****************************************************************************
 * Name: kickpi_k7_rk806_enable
 *
 * Description:
 *   Switch one rail on, bypassing consumer reference counting.
 *
 * Input Parameters:
 *   id - One of enum kickpi_k7_rk806_id_e.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_enable(int id);

/****************************************************************************
 * Name: kickpi_k7_rk806_disable
 *
 * Description:
 *   Switch one rail off, bypassing consumer reference counting.
 *
 * Input Parameters:
 *   id - One of enum kickpi_k7_rk806_id_e.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_disable(int id);

/****************************************************************************
 * Name: kickpi_k7_rk806_sys_config
 *
 * Description:
 *   Apply the board system level PMIC policy taken from the vendor device
 *   tree: reset function, hot die warning temperature and thermal shutdown
 *   temperature.  This is intentionally NOT called from
 *   kickpi_k7_rk806_initialize(); the loader already programs these fields
 *   and re-programming them is only useful when NuttX owns the whole boot.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_rk806_sys_config(void);

#ifdef __cplusplus
}
#endif

#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_RK806_H */
