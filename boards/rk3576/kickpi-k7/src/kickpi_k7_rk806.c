/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_rk806.c
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
 * Rockchip RK806 companion PMIC driver for the KICKPI-K7 board.
 *
 * The RK806 sits on I2C1 (0x2ac40000) at slave address 0x23 and supplies
 * every core and IO rail of the RK3576: ten synchronous bucks, five NLDOs
 * and six PLDOs.  Each rail is exported through the standard NuttX
 * regulator framework (include/nuttx/power/regulator.h) using the vendor
 * device tree rail names, so consumers can do:
 *
 *   struct regulator_s *r = regulator_get("vccio_sd_s0");
 *   regulator_set_voltage(r, 1800000, 1800000);
 *
 * The rail-to-name mapping is board specific (it describes what the board
 * wired each output to), which is why this driver lives in the board layer
 * rather than in chips/rk3576.
 *
 * The most important rail for bring-up is PLDO5 / vccio_sd_s0: it feeds the
 * SD and SDIO IO pads, and the SD "CMD11" signal voltage switch requires
 * moving it from 3.3 V to 1.8 V while the card clock is stopped.
 *
 * No RK3576 clock or DMA resource is consumed here - the PMIC is reached
 * only through the I2C master handed in by the board, and that master owns
 * its own clocks.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/power/regulator.h>

#include "kickpi_k7_rk806.h"

#if defined(CONFIG_REGULATOR) && defined(CONFIG_KICKPI_K7_RK806)

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK806 register map (I2C, 8-bit register address, 8-bit data). */

#define RK806_POWER_EN0             0x00  /* BUCK1..BUCK4 enables         */
#define RK806_POWER_EN1             0x01  /* BUCK5..BUCK8 enables         */
#define RK806_POWER_EN2             0x02  /* BUCK9, BUCK10 enables        */
#define RK806_POWER_EN3             0x03  /* NLDO1..NLDO4 enables         */
#define RK806_POWER_EN4             0x04  /* PLDO6, PLDO1..PLDO3 enables  */
#define RK806_POWER_EN5             0x05  /* PLDO4, PLDO5, NLDO5 enables  */

#define RK806_BUCK_ON_VSEL(n)       (0x1a + (n))  /* n = 0..9  (BUCK1..10) */
#define RK806_NLDO_ON_VSEL(n)       (0x43 + (n))  /* n = 0..4  (NLDO1..5)  */
#define RK806_PLDO_ON_VSEL(n)       (0x4e + (n))  /* n = 0..5  (PLDO1..6)  */

#define RK806_CHIP_NAME             0x5a
#define RK806_CHIP_VER              0x5b
#define RK806_SYS_CFG1              0x5d  /* Thermal thresholds           */
#define RK806_SYS_CFG3              0x72  /* Reset function select        */

/* The POWER_ENx registers are write-mask protected: the low nibble carries
 * the enable bits for four rails, the matching high nibble bit must be set
 * for the write to take effect.
 */

#define RK806_EN_WRITE_MASK_SHIFT   4

/* Voltage selector fields are the full 8 bits of the ON_VSEL registers. */

#define RK806_VSEL_MASK             0xff

/* BUCK selector ranges (RK806 datasheet, two linear segments):
 *   sel   0..160 : 0.5000 V + sel * 6.25 mV  -> 0.500 V .. 1.500 V
 *   sel 161..237 : 1.5000 V + (sel-161) * 25 mV -> 1.500 V .. 3.400 V
 */

#define RK806_BUCK_R0_MIN_UV        500000
#define RK806_BUCK_R0_STEP_UV       6250
#define RK806_BUCK_R0_MAX_SEL       160
#define RK806_BUCK_R1_MIN_UV        1500000
#define RK806_BUCK_R1_STEP_UV       25000
#define RK806_BUCK_R1_MIN_SEL       161
#define RK806_BUCK_R1_MAX_SEL       237
#define RK806_BUCK_MAX_UV           3400000
#define RK806_BUCK_NVOLTAGES        (RK806_BUCK_R1_MAX_SEL + 1)

/* LDO selector range (single linear segment):
 *   sel 0..232 : 0.5 V + sel * 12.5 mV -> 0.500 V .. 3.400 V
 */

#define RK806_LDO_MIN_UV            500000
#define RK806_LDO_STEP_UV           12500
#define RK806_LDO_MAX_SEL           232
#define RK806_LDO_MAX_UV            3400000
#define RK806_LDO_NVOLTAGES         (RK806_LDO_MAX_SEL + 1)

/* SYS_CFG1: hot die warning and thermal shutdown thresholds.
 * TODO: bit positions taken from the Rockchip vendor PMIC driver, not from
 * a public RK806 datasheet.  Verify against the datasheet before relying on
 * kickpi_k7_rk806_sys_config().
 */

#define RK806_SYS_CFG1_HOTDIE_SHIFT     6
#define RK806_SYS_CFG1_HOTDIE_MASK      (3 << RK806_SYS_CFG1_HOTDIE_SHIFT)
#define RK806_SYS_CFG1_HOTDIE_115C      0
#define RK806_SYS_CFG1_TSD_SHIFT        4
#define RK806_SYS_CFG1_TSD_MASK         (1 << RK806_SYS_CFG1_TSD_SHIFT)
#define RK806_SYS_CFG1_TSD_160C         1

/* SYS_CFG3: RST_FUN selects what the reset pin / watchdog does.
 * Value 1 matches the device tree property "pmic-reset-func = <1>":
 * restart the PMIC power sequence instead of shutting down.
 */

#define RK806_SYS_CFG3_RSTFUN_SHIFT     6
#define RK806_SYS_CFG3_RSTFUN_MASK      (3 << RK806_SYS_CFG3_RSTFUN_SHIFT)
#define KICKPI_K7_RK806_RESET_FUNC      1

/* Enable ramp delay of the DVS capable bucks, from the device tree
 * "regulator-enable-ramp-delay" property (microseconds).
 */

#define RK806_DVS_ENABLE_TIME_US        400

/* Voltage slew rate of the DVS capable bucks, from the device tree
 * "regulator-ramp-delay" property (microvolts per microsecond).
 */

#define RK806_DVS_RAMP_DELAY            12500

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static hardware description of one RK806 output. */

struct kickpi_k7_rk806_rail_s
{
  uint8_t vsel_reg;   /* ON_VSEL register of this rail                    */
  uint8_t en_reg;     /* POWER_ENx register holding the enable bit        */
  uint8_t en_bit;     /* Enable bit position inside the low nibble (0..3) */
  bool    is_buck;    /* true: buck selector table, false: LDO table      */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk806_read_reg(uint8_t reg, uint8_t *val);
static int rk806_write_reg(uint8_t reg, uint8_t val);
static int rk806_update_bits(uint8_t reg, uint8_t mask, uint8_t val);

static int rk806_sel_to_uv(const struct kickpi_k7_rk806_rail_s *rail,
                           uint8_t sel);
static int rk806_uv_to_sel(const struct kickpi_k7_rk806_rail_s *rail,
                           int uv);

static const struct kickpi_k7_rk806_rail_s *
  rk806_rail_of(struct regulator_dev_s *rdev);

static int rk806_do_enable(const struct kickpi_k7_rk806_rail_s *rail,
                           bool enable);
static int rk806_do_set_voltage(const struct kickpi_k7_rk806_rail_s *rail,
                                int min_uv, int max_uv, unsigned int *sel);
static int rk806_do_get_voltage(const struct kickpi_k7_rk806_rail_s *rail);

static int rk806_enable(struct regulator_dev_s *rdev);
static int rk806_disable(struct regulator_dev_s *rdev);
static int rk806_is_enabled(struct regulator_dev_s *rdev);
static int rk806_set_voltage(struct regulator_dev_s *rdev, int min_uv,
                             int max_uv, unsigned int *selector);
static int rk806_get_voltage(struct regulator_dev_s *rdev);
static int rk806_list_voltage(struct regulator_dev_s *rdev,
                              unsigned int selector);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* I2C master the PMIC is attached to, captured at initialize() time. */

static struct i2c_master_s *g_rk806_i2c;

static const struct regulator_ops_s g_rk806_ops =
{
  .list_voltage = rk806_list_voltage,
  .set_voltage  = rk806_set_voltage,
  .get_voltage  = rk806_get_voltage,
  .enable       = rk806_enable,
  .disable      = rk806_disable,
  .is_enabled   = rk806_is_enabled,
};

/* Hardware description of the 21 RK806 outputs, indexed by
 * enum kickpi_k7_rk806_id_e.
 *
 * TODO: the POWER_ENx bit assignment of the LDOs follows the Rockchip
 * vendor driver (NLDO1..4 in EN3, PLDO6/PLDO1..3 in EN4, PLDO4/PLDO5/NLDO5
 * in EN5).  Re-check against the RK806 datasheet when it becomes available.
 */

static const struct kickpi_k7_rk806_rail_s
  g_rk806_rails[RK806_ID_NREGULATORS] =
{
  [RK806_ID_DCDC1]  =
    {
      RK806_BUCK_ON_VSEL(0), RK806_POWER_EN0, 0, true
    },
  [RK806_ID_DCDC2]  =
    {
      RK806_BUCK_ON_VSEL(1), RK806_POWER_EN0, 1, true
    },
  [RK806_ID_DCDC3]  =
    {
      RK806_BUCK_ON_VSEL(2), RK806_POWER_EN0, 2, true
    },
  [RK806_ID_DCDC4]  =
    {
      RK806_BUCK_ON_VSEL(3), RK806_POWER_EN0, 3, true
    },
  [RK806_ID_DCDC5]  =
    {
      RK806_BUCK_ON_VSEL(4), RK806_POWER_EN1, 0, true
    },
  [RK806_ID_DCDC6]  =
    {
      RK806_BUCK_ON_VSEL(5), RK806_POWER_EN1, 1, true
    },
  [RK806_ID_DCDC7]  =
    {
      RK806_BUCK_ON_VSEL(6), RK806_POWER_EN1, 2, true
    },
  [RK806_ID_DCDC8]  =
    {
      RK806_BUCK_ON_VSEL(7), RK806_POWER_EN1, 3, true
    },
  [RK806_ID_DCDC9]  =
    {
      RK806_BUCK_ON_VSEL(8), RK806_POWER_EN2, 0, true
    },
  [RK806_ID_DCDC10] =
    {
      RK806_BUCK_ON_VSEL(9), RK806_POWER_EN2, 1, true
    },
  [RK806_ID_NLDO1]  =
    {
      RK806_NLDO_ON_VSEL(0), RK806_POWER_EN3, 0, false
    },
  [RK806_ID_NLDO2]  =
    {
      RK806_NLDO_ON_VSEL(1), RK806_POWER_EN3, 1, false
    },
  [RK806_ID_NLDO3]  =
    {
      RK806_NLDO_ON_VSEL(2), RK806_POWER_EN3, 2, false
    },
  [RK806_ID_NLDO4]  =
    {
      RK806_NLDO_ON_VSEL(3), RK806_POWER_EN3, 3, false
    },
  [RK806_ID_NLDO5]  =
    {
      RK806_NLDO_ON_VSEL(4), RK806_POWER_EN5, 2, false
    },
  [RK806_ID_PLDO1]  =
    {
      RK806_PLDO_ON_VSEL(0), RK806_POWER_EN4, 1, false
    },
  [RK806_ID_PLDO2]  =
    {
      RK806_PLDO_ON_VSEL(1), RK806_POWER_EN4, 2, false
    },
  [RK806_ID_PLDO3]  =
    {
      RK806_PLDO_ON_VSEL(2), RK806_POWER_EN4, 3, false
    },
  [RK806_ID_PLDO4]  =
    {
      RK806_PLDO_ON_VSEL(3), RK806_POWER_EN5, 0, false
    },
  [RK806_ID_PLDO5]  =
    {
      RK806_PLDO_ON_VSEL(4), RK806_POWER_EN5, 1, false
    },
  [RK806_ID_PLDO6]  =
    {
      RK806_PLDO_ON_VSEL(5), RK806_POWER_EN4, 0, false
    },
};

/* Board rail constraints.  Names, limits, always-on / boot-on state and
 * ramp delays are transcribed from the KICKPI-K7 vendor device tree node
 * /i2c@2ac40000/pmic@23/regulators.
 */

static const struct regulator_desc_s
  g_rk806_desc[RK806_ID_NREGULATORS] =
{
  [RK806_ID_DCDC1] =
    {
      .name        = "vdd_cpu_big_s0",
      .id          = RK806_ID_DCDC1,
      .n_voltages  = RK806_BUCK_NVOLTAGES,
      .min_uv      = 550000,
      .max_uv      = 950000,
      .uv_step     = RK806_BUCK_R0_STEP_UV,
      .ramp_delay  = RK806_DVS_RAMP_DELAY,
      .enable_time = RK806_DVS_ENABLE_TIME_US,
      .boot_on     = 1,
      .always_on   = 1,
    },
  [RK806_ID_DCDC2] =
    {
      .name        = "vdd_npu_s0",
      .id          = RK806_ID_DCDC2,
      .n_voltages  = RK806_BUCK_NVOLTAGES,
      .min_uv      = 550000,
      .max_uv      = 950000,
      .uv_step     = RK806_BUCK_R0_STEP_UV,
      .ramp_delay  = RK806_DVS_RAMP_DELAY,
      .enable_time = RK806_DVS_ENABLE_TIME_US,
      .boot_on     = 1,
    },
  [RK806_ID_DCDC3] =
    {
      .name        = "vdd_cpu_lit_s0",
      .id          = RK806_ID_DCDC3,
      .n_voltages  = RK806_BUCK_NVOLTAGES,
      .min_uv      = 550000,
      .max_uv      = 950000,
      .uv_step     = RK806_BUCK_R0_STEP_UV,
      .ramp_delay  = RK806_DVS_RAMP_DELAY,
      .boot_on     = 1,
      .always_on   = 1,
    },
  [RK806_ID_DCDC4] =
    {
      .name       = "vcc_3v3_s3",
      .id         = RK806_ID_DCDC4,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = 3300000,
      .max_uv     = 3300000,
      .uv_step    = RK806_BUCK_R1_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_DCDC5] =
    {
      .name        = "vdd_gpu_s0",
      .id          = RK806_ID_DCDC5,
      .n_voltages  = RK806_BUCK_NVOLTAGES,
      .min_uv      = 550000,
      .max_uv      = 900000,
      .uv_step     = RK806_BUCK_R0_STEP_UV,
      .ramp_delay  = RK806_DVS_RAMP_DELAY,
      .enable_time = RK806_DVS_ENABLE_TIME_US,
      .boot_on     = 1,
    },
  [RK806_ID_DCDC6] =
    {
      .name       = "vddq_ddr_s0",
      .id         = RK806_ID_DCDC6,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = RK806_BUCK_R0_MIN_UV,
      .max_uv     = RK806_BUCK_MAX_UV,
      .uv_step    = RK806_BUCK_R0_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_DCDC7] =
    {
      .name       = "vdd_logic_s0",
      .id         = RK806_ID_DCDC7,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = 550000,
      .max_uv     = 800000,
      .uv_step    = RK806_BUCK_R0_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_DCDC8] =
    {
      .name       = "vcc_1v8_s3",
      .id         = RK806_ID_DCDC8,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = 1800000,
      .max_uv     = 1800000,
      .uv_step    = RK806_BUCK_R1_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_DCDC9] =
    {
      .name       = "vdd2_ddr_s3",
      .id         = RK806_ID_DCDC9,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = RK806_BUCK_R0_MIN_UV,
      .max_uv     = RK806_BUCK_MAX_UV,
      .uv_step    = RK806_BUCK_R0_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_DCDC10] =
    {
      .name       = "vdd_ddr_s0",
      .id         = RK806_ID_DCDC10,
      .n_voltages = RK806_BUCK_NVOLTAGES,
      .min_uv     = 550000,
      .max_uv     = 1200000,
      .uv_step    = RK806_BUCK_R0_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_NLDO1] =
    {
      .name       = "vdd_0v75_s3",
      .id         = RK806_ID_NLDO1,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 750000,
      .max_uv     = 750000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_NLDO2] =
    {
      .name       = "vdda_ddr_pll_s0",
      .id         = RK806_ID_NLDO2,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 850000,
      .max_uv     = 850000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_NLDO3] =
    {
      .name       = "vdda0v75_hdmi_s0",
      .id         = RK806_ID_NLDO3,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 837500,
      .max_uv     = 837500,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_NLDO4] =
    {
      .name       = "vdda_0v85_s0",
      .id         = RK806_ID_NLDO4,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 850000,
      .max_uv     = 850000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_NLDO5] =
    {
      .name       = "vdda_0v75_s0",
      .id         = RK806_ID_NLDO5,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 750000,
      .max_uv     = 750000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_PLDO1] =
    {
      .name       = "vcca_1v8_s0",
      .id         = RK806_ID_PLDO1,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 1800000,
      .max_uv     = 1800000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_PLDO2] =
    {
      .name       = "vcca1v8_pldo2_s0",
      .id         = RK806_ID_PLDO2,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 1800000,
      .max_uv     = 1800000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_PLDO3] =
    {
      .name       = "vdda_1v2_s0",
      .id         = RK806_ID_PLDO3,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 1200000,
      .max_uv     = 1200000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_PLDO4] =
    {
      .name       = "vcca_3v3_s0",
      .id         = RK806_ID_PLDO4,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 3300000,
      .max_uv     = 3300000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },

  /* PLDO5 feeds the SD / SDIO IO pads.  It is the only rail whose voltage
   * is switched at runtime: 3.3 V for legacy SD signalling, 1.8 V after a
   * successful CMD11 voltage switch (UHS-I, and the SDIO WiFi controller).
   */

  [RK806_ID_PLDO5] =
    {
      .name       = "vccio_sd_s0",
      .id         = RK806_ID_PLDO5,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = KICKPI_K7_VCCIO_SD_1V8,
      .max_uv     = KICKPI_K7_VCCIO_SD_3V3,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
  [RK806_ID_PLDO6] =
    {
      .name       = "vcca1v8_pldo6_s3",
      .id         = RK806_ID_PLDO6,
      .n_voltages = RK806_LDO_NVOLTAGES,
      .min_uv     = 1800000,
      .max_uv     = 1800000,
      .uv_step    = RK806_LDO_STEP_UV,
      .boot_on    = 1,
      .always_on  = 1,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk806_read_reg
 *
 * Description:
 *   Read one 8-bit RK806 register over I2C (write register address, then
 *   repeated start and read one byte).
 *
 ****************************************************************************/

static int rk806_read_reg(uint8_t reg, uint8_t *val)
{
  struct i2c_msg_s msgs[2];

  if (g_rk806_i2c == NULL)
    {
      return -ENODEV;
    }

  msgs[0].frequency = KICKPI_K7_RK806_I2C_FREQ;
  msgs[0].addr      = KICKPI_K7_RK806_I2C_ADDR;
  msgs[0].flags     = I2C_M_NOSTOP;
  msgs[0].buffer    = &reg;
  msgs[0].length    = 1;

  msgs[1].frequency = KICKPI_K7_RK806_I2C_FREQ;
  msgs[1].addr      = KICKPI_K7_RK806_I2C_ADDR;
  msgs[1].flags     = I2C_M_READ;
  msgs[1].buffer    = val;
  msgs[1].length    = 1;

  return I2C_TRANSFER(g_rk806_i2c, msgs, 2);
}

/****************************************************************************
 * Name: rk806_write_reg
 *
 * Description:
 *   Write one 8-bit RK806 register over I2C.
 *
 ****************************************************************************/

static int rk806_write_reg(uint8_t reg, uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t txbuf[2];

  if (g_rk806_i2c == NULL)
    {
      return -ENODEV;
    }

  txbuf[0] = reg;
  txbuf[1] = val;

  msg.frequency = KICKPI_K7_RK806_I2C_FREQ;
  msg.addr      = KICKPI_K7_RK806_I2C_ADDR;
  msg.flags     = 0;
  msg.buffer    = txbuf;
  msg.length    = sizeof(txbuf);

  return I2C_TRANSFER(g_rk806_i2c, &msg, 1);
}

/****************************************************************************
 * Name: rk806_update_bits
 *
 * Description:
 *   Read-modify-write the bits selected by mask in one RK806 register.
 *
 ****************************************************************************/

static int rk806_update_bits(uint8_t reg, uint8_t mask, uint8_t val)
{
  uint8_t oldval;
  uint8_t newval;
  int ret;

  ret = rk806_read_reg(reg, &oldval);
  if (ret < 0)
    {
      return ret;
    }

  newval = (oldval & ~mask) | (val & mask);
  if (newval == oldval)
    {
      return OK;
    }

  return rk806_write_reg(reg, newval);
}

/****************************************************************************
 * Name: rk806_sel_to_uv
 *
 * Description:
 *   Convert a raw RK806 voltage selector to microvolts.
 *
 ****************************************************************************/

static int rk806_sel_to_uv(const struct kickpi_k7_rk806_rail_s *rail,
                           uint8_t sel)
{
  if (!rail->is_buck)
    {
      if (sel > RK806_LDO_MAX_SEL)
        {
          return RK806_LDO_MAX_UV;
        }

      return RK806_LDO_MIN_UV + (int)sel * RK806_LDO_STEP_UV;
    }

  if (sel <= RK806_BUCK_R0_MAX_SEL)
    {
      return RK806_BUCK_R0_MIN_UV + (int)sel * RK806_BUCK_R0_STEP_UV;
    }

  if (sel <= RK806_BUCK_R1_MAX_SEL)
    {
      return RK806_BUCK_R1_MIN_UV +
             ((int)sel - RK806_BUCK_R1_MIN_SEL) * RK806_BUCK_R1_STEP_UV;
    }

  return RK806_BUCK_MAX_UV;
}

/****************************************************************************
 * Name: rk806_uv_to_sel
 *
 * Description:
 *   Convert microvolts to the smallest raw RK806 selector that produces at
 *   least that voltage.
 *
 * Returned Value:
 *   The selector (0..255) on success; -EINVAL if the request is above the
 *   highest supported output.
 *
 ****************************************************************************/

static int rk806_uv_to_sel(const struct kickpi_k7_rk806_rail_s *rail,
                           int uv)
{
  int sel;

  if (!rail->is_buck)
    {
      if (uv > RK806_LDO_MAX_UV)
        {
          return -EINVAL;
        }

      if (uv <= RK806_LDO_MIN_UV)
        {
          return 0;
        }

      sel = (uv - RK806_LDO_MIN_UV + RK806_LDO_STEP_UV - 1) /
            RK806_LDO_STEP_UV;
      return sel;
    }

  if (uv > RK806_BUCK_MAX_UV)
    {
      return -EINVAL;
    }

  if (uv <= RK806_BUCK_R0_MIN_UV)
    {
      return 0;
    }

  if (uv <= RK806_BUCK_R1_MIN_UV)
    {
      return (uv - RK806_BUCK_R0_MIN_UV + RK806_BUCK_R0_STEP_UV - 1) /
             RK806_BUCK_R0_STEP_UV;
    }

  sel = RK806_BUCK_R1_MIN_SEL +
        (uv - RK806_BUCK_R1_MIN_UV + RK806_BUCK_R1_STEP_UV - 1) /
        RK806_BUCK_R1_STEP_UV;
  return sel;
}

/****************************************************************************
 * Name: rk806_rail_of
 *
 * Description:
 *   Recover the hardware description attached to a regulator device.
 *
 ****************************************************************************/

static const struct kickpi_k7_rk806_rail_s *
  rk806_rail_of(struct regulator_dev_s *rdev)
{
  return (const struct kickpi_k7_rk806_rail_s *)rdev->priv;
}

/****************************************************************************
 * Name: rk806_do_enable
 *
 * Description:
 *   Set or clear the POWER_ENx bit of one rail.  The high nibble of the
 *   register is the per-bit write enable mask and must carry the matching
 *   bit for the write to be accepted.
 *
 ****************************************************************************/

static int rk806_do_enable(const struct kickpi_k7_rk806_rail_s *rail,
                           bool enable)
{
  uint8_t val;

  val = (uint8_t)(1u << (rail->en_bit + RK806_EN_WRITE_MASK_SHIFT));
  if (enable)
    {
      val |= (uint8_t)(1u << rail->en_bit);
    }

  return rk806_write_reg(rail->en_reg, val);
}

/****************************************************************************
 * Name: rk806_do_set_voltage
 *
 * Description:
 *   Program the ON_VSEL register of one rail with the lowest selector that
 *   satisfies min_uv, rejecting the request if that selector exceeds
 *   max_uv.
 *
 ****************************************************************************/

static int rk806_do_set_voltage(const struct kickpi_k7_rk806_rail_s *rail,
                                int min_uv, int max_uv, unsigned int *sel)
{
  int selector;
  int uv;
  int ret;

  selector = rk806_uv_to_sel(rail, min_uv);
  if (selector < 0)
    {
      return selector;
    }

  uv = rk806_sel_to_uv(rail, (uint8_t)selector);
  if (uv > max_uv)
    {
      pwrerr("ERROR: RK806 vsel reg 0x%02x: %d uV out of [%d, %d]\n",
             rail->vsel_reg, uv, min_uv, max_uv);
      return -EINVAL;
    }

  ret = rk806_update_bits(rail->vsel_reg, RK806_VSEL_MASK,
                          (uint8_t)selector);
  if (ret < 0)
    {
      pwrerr("ERROR: RK806 vsel reg 0x%02x write failed: %d\n",
             rail->vsel_reg, ret);
      return ret;
    }

  if (sel != NULL)
    {
      *sel = (unsigned int)selector;
    }

  return OK;
}

/****************************************************************************
 * Name: rk806_do_get_voltage
 *
 * Description:
 *   Read back the programmed output voltage of one rail in microvolts.
 *
 ****************************************************************************/

static int rk806_do_get_voltage(const struct kickpi_k7_rk806_rail_s *rail)
{
  uint8_t sel;
  int ret;

  ret = rk806_read_reg(rail->vsel_reg, &sel);
  if (ret < 0)
    {
      return ret;
    }

  return rk806_sel_to_uv(rail, (uint8_t)(sel & RK806_VSEL_MASK));
}

/****************************************************************************
 * Name: rk806_enable
 ****************************************************************************/

static int rk806_enable(struct regulator_dev_s *rdev)
{
  return rk806_do_enable(rk806_rail_of(rdev), true);
}

/****************************************************************************
 * Name: rk806_disable
 ****************************************************************************/

static int rk806_disable(struct regulator_dev_s *rdev)
{
  return rk806_do_enable(rk806_rail_of(rdev), false);
}

/****************************************************************************
 * Name: rk806_is_enabled
 ****************************************************************************/

static int rk806_is_enabled(struct regulator_dev_s *rdev)
{
  const struct kickpi_k7_rk806_rail_s *rail = rk806_rail_of(rdev);
  uint8_t val;
  int ret;

  ret = rk806_read_reg(rail->en_reg, &val);
  if (ret < 0)
    {
      return ret;
    }

  return (val & (1u << rail->en_bit)) != 0;
}

/****************************************************************************
 * Name: rk806_set_voltage
 ****************************************************************************/

static int rk806_set_voltage(struct regulator_dev_s *rdev, int min_uv,
                             int max_uv, unsigned int *selector)
{
  return rk806_do_set_voltage(rk806_rail_of(rdev), min_uv, max_uv,
                              selector);
}

/****************************************************************************
 * Name: rk806_get_voltage
 ****************************************************************************/

static int rk806_get_voltage(struct regulator_dev_s *rdev)
{
  return rk806_do_get_voltage(rk806_rail_of(rdev));
}

/****************************************************************************
 * Name: rk806_list_voltage
 *
 * Description:
 *   Return the voltage produced by a given raw selector, without touching
 *   the hardware.
 *
 ****************************************************************************/

static int rk806_list_voltage(struct regulator_dev_s *rdev,
                              unsigned int selector)
{
  const struct kickpi_k7_rk806_rail_s *rail = rk806_rail_of(rdev);

  if (selector >= rdev->desc->n_voltages)
    {
      return -EINVAL;
    }

  return rk806_sel_to_uv(rail, (uint8_t)selector);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_rk806_initialize
 *
 * Description:
 *   Probe the RK806 PMIC and register every board power rail with the NuttX
 *   regulator framework.
 *
 ****************************************************************************/

int kickpi_k7_rk806_initialize(struct i2c_master_s *i2c)
{
  struct regulator_dev_s *rdev;
  uint8_t chip_name;
  uint8_t chip_ver;
  int registered = 0;
  int ret;
  int i;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  if (g_rk806_i2c != NULL)
    {
      /* Already initialized; keep the existing registrations. */

      return OK;
    }

  g_rk806_i2c = i2c;

  /* Probe: the chip identification registers must be readable. */

  ret = rk806_read_reg(RK806_CHIP_NAME, &chip_name);
  if (ret < 0)
    {
      pwrerr("ERROR: RK806 not responding at 0x%02x: %d\n",
             KICKPI_K7_RK806_I2C_ADDR, ret);
      g_rk806_i2c = NULL;
      return ret;
    }

  ret = rk806_read_reg(RK806_CHIP_VER, &chip_ver);
  if (ret < 0)
    {
      pwrerr("ERROR: RK806 CHIP_VER read failed: %d\n", ret);
      g_rk806_i2c = NULL;
      return ret;
    }

  pwrinfo("RK806 PMIC detected: chip name 0x%02x version 0x%02x\n",
          chip_name, chip_ver);

  /* Register every rail with the regulator framework.  A single failing
   * rail must not abort the others: the SoC is already running off these
   * supplies, so the useful outcome is to expose as much as possible.
   */

  for (i = 0; i < RK806_ID_NREGULATORS; i++)
    {
      rdev = regulator_register(&g_rk806_desc[i], &g_rk806_ops,
                                (void *)&g_rk806_rails[i]);
      if (rdev == NULL)
        {
          pwrerr("ERROR: failed to register regulator %s\n",
                 g_rk806_desc[i].name);
          continue;
        }

      registered++;
    }

  pwrinfo("RK806: %d/%d regulators registered\n",
          registered, RK806_ID_NREGULATORS);

  return registered > 0 ? OK : -ENODEV;
}

/****************************************************************************
 * Name: kickpi_k7_rk806_set_voltage_uv
 *
 * Description:
 *   Set the output voltage of one rail, bypassing consumer reference
 *   counting.
 *
 ****************************************************************************/

int kickpi_k7_rk806_set_voltage_uv(int id, int uv)
{
  if (id < 0 || id >= RK806_ID_NREGULATORS)
    {
      return -EINVAL;
    }

  if (uv < (int)g_rk806_desc[id].min_uv ||
      uv > (int)g_rk806_desc[id].max_uv)
    {
      pwrerr("ERROR: %s: %d uV outside board limits [%u, %u]\n",
             g_rk806_desc[id].name, uv, g_rk806_desc[id].min_uv,
             g_rk806_desc[id].max_uv);
      return -EINVAL;
    }

  return rk806_do_set_voltage(&g_rk806_rails[id], uv,
                              (int)g_rk806_desc[id].max_uv, NULL);
}

/****************************************************************************
 * Name: kickpi_k7_rk806_get_voltage_uv
 *
 * Description:
 *   Read back the programmed output voltage of one rail.
 *
 ****************************************************************************/

int kickpi_k7_rk806_get_voltage_uv(int id)
{
  if (id < 0 || id >= RK806_ID_NREGULATORS)
    {
      return -EINVAL;
    }

  return rk806_do_get_voltage(&g_rk806_rails[id]);
}

/****************************************************************************
 * Name: kickpi_k7_rk806_enable
 *
 * Description:
 *   Switch one rail on, bypassing consumer reference counting.
 *
 ****************************************************************************/

int kickpi_k7_rk806_enable(int id)
{
  if (id < 0 || id >= RK806_ID_NREGULATORS)
    {
      return -EINVAL;
    }

  return rk806_do_enable(&g_rk806_rails[id], true);
}

/****************************************************************************
 * Name: kickpi_k7_rk806_disable
 *
 * Description:
 *   Switch one rail off, bypassing consumer reference counting.
 *
 ****************************************************************************/

int kickpi_k7_rk806_disable(int id)
{
  if (id < 0 || id >= RK806_ID_NREGULATORS)
    {
      return -EINVAL;
    }

  return rk806_do_enable(&g_rk806_rails[id], false);
}

/****************************************************************************
 * Name: kickpi_k7_rk806_sys_config
 *
 * Description:
 *   Apply the board PMIC system policy: reset function, hot die warning and
 *   thermal shutdown thresholds.
 *
 ****************************************************************************/

int kickpi_k7_rk806_sys_config(void)
{
  int ret;

  ret = rk806_update_bits(RK806_SYS_CFG3, RK806_SYS_CFG3_RSTFUN_MASK,
                          KICKPI_K7_RK806_RESET_FUNC <<
                          RK806_SYS_CFG3_RSTFUN_SHIFT);
  if (ret < 0)
    {
      pwrerr("ERROR: RK806 SYS_CFG3 write failed: %d\n", ret);
      return ret;
    }

  /* Hot die warning at 115 C, thermal shutdown at 160 C, matching the
   * vendor device tree properties hotdie_temperture_threshold = <115> and
   * shutdown_temperture_threshold = <160>.
   */

  ret = rk806_update_bits(RK806_SYS_CFG1,
                          RK806_SYS_CFG1_HOTDIE_MASK |
                          RK806_SYS_CFG1_TSD_MASK,
                          (RK806_SYS_CFG1_HOTDIE_115C <<
                           RK806_SYS_CFG1_HOTDIE_SHIFT) |
                          (RK806_SYS_CFG1_TSD_160C <<
                           RK806_SYS_CFG1_TSD_SHIFT));
  if (ret < 0)
    {
      pwrerr("ERROR: RK806 SYS_CFG1 write failed: %d\n", ret);
      return ret;
    }

  /* TODO: the device tree also carries low_voltage_threshold = <3000> mV
   * and shutdown_voltage_threshold = <2700> mV.  The RK806 register and bit
   * layout for the VSYS under-voltage comparators is not documented in any
   * public datasheet we have, so those two thresholds are left at their
   * loader-programmed values rather than guessed at.
   */

  return OK;
}

#endif /* CONFIG_REGULATOR && CONFIG_KICKPI_K7_RK806 */
