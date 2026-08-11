/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_wifi.c
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
 * Board glue for the on-board SeekWave SV6621 (SWT6621-S) WiFi/BT combo.
 * Owns the module pin environment (SDIO bus mux + drive strength, the
 * companion BT-side pins the combo boot ROM samples, and the hym8563 RTC
 * 32.768 kHz sleep clock), supplies the CP firmware images, and drives
 * WL_REG_ON for the core driver's power callback.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>

#include "arm64_internal.h"

#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_skw.h"
#include "rk3576_skw_internal.h"
#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_WIFI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_SDIO_PIN(pin) \
  (GPIO_PORT1 | (pin) | GPIO_ALT | GPIO_AF2 | GPIO_PULLUP)

#define WIFI_WL_REG_ON  (GPIO_PORT1 | GPIO_PIN_C6 | GPIO_OUTPUT)
#define WIFI_BT_RST     (GPIO_PORT1 | GPIO_PIN_C7 | GPIO_OUTPUT)

/* IOC drive-strength registers for the SDIO bus pins (max drive). */

#define WIFI_IOC_DRV0   0x26046210
#define WIFI_IOC_DRV1   0x26046214
#define WIFI_IOC_DRV2   0x26046218
#define WIFI_IOC_DRV3   0x2604621c

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CP firmware images, linked into .rodata from the extracted SeekWave
 * blobs (redistributed with the product; copyright SeekWave, loaded only).
 */

__asm__(
  "  .section .rodata, \"a\"\n"
  "  .align 4\n"
  "  .global g_skw_iram_start\n"
  "g_skw_iram_start:\n"
  "  .incbin \"" CONFIG_KICKPI_K7_WIFI_IRAM "\"\n"
  "  .global g_skw_iram_end\n"
  "g_skw_iram_end:\n"
  "  .align 4\n"
  "  .global g_skw_dram_start\n"
  "g_skw_dram_start:\n"
  "  .incbin \"" CONFIG_KICKPI_K7_WIFI_DRAM "\"\n"
  "  .global g_skw_dram_end\n"
  "g_skw_dram_end:\n"
  "  .align 4\n"
  "  .global g_skw_nv_start\n"
  "g_skw_nv_start:\n"
  "  .incbin \"" CONFIG_KICKPI_K7_WIFI_NV "\"\n"
  "  .global g_skw_nv_end\n"
  "g_skw_nv_end:\n"
  "  .align 4\n"
  "  .global g_skw_calib_start\n"
  "g_skw_calib_start:\n"
  "  .incbin \"" CONFIG_KICKPI_K7_WIFI_CALIB "\"\n"
  "  .global g_skw_calib_end\n"
  "g_skw_calib_end:\n"
  "  .previous\n");

extern const uint8_t g_skw_iram_start[];
extern const uint8_t g_skw_iram_end[];
extern const uint8_t g_skw_dram_start[];
extern const uint8_t g_skw_dram_end[];
extern const uint8_t g_skw_nv_start[];
extern const uint8_t g_skw_nv_end[];
extern const uint8_t g_skw_calib_start[];
extern const uint8_t g_skw_calib_end[];

static const gpio_pinset_t g_wifi_sdio_pins[] =
{
  WIFI_SDIO_PIN(GPIO_PIN_B4),   /* D0 */
  WIFI_SDIO_PIN(GPIO_PIN_B5),   /* D1 */
  WIFI_SDIO_PIN(GPIO_PIN_B6),   /* D2 */
  WIFI_SDIO_PIN(GPIO_PIN_B7),   /* D3 */
  WIFI_SDIO_PIN(GPIO_PIN_C0),   /* CMD */
  WIFI_SDIO_PIN(GPIO_PIN_C1),   /* CLK */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int kickpi_k7_wifi_enable_32k(void);
static void kickpi_k7_wifi_power(bool on);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_wifi_enable_32k
 *
 * Description:
 *   Enable the hym8563 RTC 32.768 kHz CLKOUT (register 0x0D = 0xC4), the
 *   combo's low-power sleep clock.  The RTC is on I2C2 at 7-bit 0x51; the
 *   reg-pointer write and read are issued as separate transfers so the
 *   driver's merged-transaction path (needed by other devices) does not
 *   corrupt the hym8563 read.
 ****************************************************************************/

static int kickpi_k7_wifi_enable_32k(void)
{
  struct i2c_master_s *i2c;
  uint8_t wbuf[2] = { 0x0d, 0xc4 };
  uint8_t reg     = 0x0d;
  uint8_t rback   = 0;
  int attempt;

  struct i2c_msg_s wmsg =
  {
    .frequency = 400000, .addr = 0x51, .flags = 0,
    .buffer = wbuf, .length = 2
  };
  struct i2c_msg_s pmsg =
  {
    .frequency = 400000, .addr = 0x51, .flags = 0,
    .buffer = &reg, .length = 1
  };
  struct i2c_msg_s dmsg =
  {
    .frequency = 400000, .addr = 0x51, .flags = I2C_M_READ,
    .buffer = &rback, .length = 1
  };

  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_B7 | GPIO_ALT | GPIO_AF9 |
                     GPIO_PULLUP);              /* I2C2 SCL */
  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_C0 | GPIO_ALT | GPIO_AF9 |
                     GPIO_PULLUP);              /* I2C2 SDA */

  /* rk3576_i2c_initialize ungates the controller clock via the CRU
   * driver, so no explicit gate call is needed here.
   */

  i2c = rk3576_i2c_initialize(2);
  if (i2c == NULL)
    {
      wlwarn("WARNING: i2c2 init failed, 32k not enabled\n");
      return -ENODEV;
    }

  for (attempt = 0; attempt < 6; attempt++)
    {
      int wr = I2C_TRANSFER(i2c, &wmsg, 1);
      int pr;
      int rd;

      up_mdelay(3);
      pr = I2C_TRANSFER(i2c, &pmsg, 1);
      rd = I2C_TRANSFER(i2c, &dmsg, 1);
      if (wr == OK && pr == OK && rd == OK && rback == 0xc4)
        {
          up_mdelay(150);
          return OK;
        }

      up_mdelay(5);
    }

  wlwarn("WARNING: hym8563 CLKOUT setup failed, readback=0x%02x\n",
         rback);
  return -EIO;
}


#endif /* CONFIG_KICKPI_K7_WIFI */
