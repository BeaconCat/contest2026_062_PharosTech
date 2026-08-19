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

#include <debug.h>
#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>

#include "arm64_internal.h"

#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_sv6621_transport.h"
#include "sv6621.h"

#ifdef CONFIG_KICKPI_K7_WIFI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIFI_SDIO_PIN(pin) \
  (GPIO_PORT1 | (pin) | GPIO_ALT | GPIO_AF2 | GPIO_PULLUP)

#define WIFI_WL_REG_ON (GPIO_PORT1 | GPIO_PIN_C6 | GPIO_OUTPUT)
#define WIFI_BT_RST    (GPIO_PORT1 | GPIO_PIN_C7 | GPIO_OUTPUT)

/* IOC drive-strength registers for the SDIO bus pins (max drive). */

#define WIFI_IOC_DRV0 0x26046210
#define WIFI_IOC_DRV1 0x26046214
#define WIFI_IOC_DRV2 0x26046218
#define WIFI_IOC_DRV3 0x2604621c

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* CP firmware images, linked into .rodata from the extracted SeekWave
 * blobs (redistributed with the product; copyright SeekWave, loaded only).
 */

__asm__("  .section .rodata, \"a\"\n"
        "  .align 4\n"
        "  .global g_sv6621_iram_start\n"
        "g_sv6621_iram_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_IRAM "\"\n"
        "  .global g_sv6621_iram_end\n"
        "g_sv6621_iram_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_dram_start\n"
        "g_sv6621_dram_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_DRAM "\"\n"
        "  .global g_sv6621_dram_end\n"
        "g_sv6621_dram_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_nv_start\n"
        "g_sv6621_nv_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_NV "\"\n"
        "  .global g_sv6621_nv_end\n"
        "g_sv6621_nv_end:\n"
        "  .align 4\n"
        "  .global g_sv6621_calib_start\n"
        "g_sv6621_calib_start:\n"
        "  .incbin \"" CONFIG_KICKPI_K7_WIFI_CALIB "\"\n"
        "  .global g_sv6621_calib_end\n"
        "g_sv6621_calib_end:\n"
        "  .previous\n");

extern const uint8_t g_sv6621_iram_start[];
extern const uint8_t g_sv6621_iram_end[];
extern const uint8_t g_sv6621_dram_start[];
extern const uint8_t g_sv6621_dram_end[];
extern const uint8_t g_sv6621_nv_start[];
extern const uint8_t g_sv6621_nv_end[];
extern const uint8_t g_sv6621_calib_start[];
extern const uint8_t g_sv6621_calib_end[];

static const gpio_pinset_t g_wifi_sdio_pins[] = {
  WIFI_SDIO_PIN(GPIO_PIN_B4), /* D0 */
  WIFI_SDIO_PIN(GPIO_PIN_B5), /* D1 */
  WIFI_SDIO_PIN(GPIO_PIN_B6), /* D2 */
  WIFI_SDIO_PIN(GPIO_PIN_B7), /* D3 */
  WIFI_SDIO_PIN(GPIO_PIN_C0), /* CMD */
  WIFI_SDIO_PIN(GPIO_PIN_C1), /* CLK */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int kickpi_k7_wifi_enable_32k(void);
static int kickpi_k7_wifi_power_on(FAR void *arg);
static void kickpi_k7_wifi_power_off(FAR void *arg);
static int kickpi_k7_wifi_load_address(
    FAR void *arg, uint8_t address[SV6621_MAC_LENGTH]);
static int kickpi_k7_wifi_store_address(
    FAR void *arg, FAR const uint8_t address[SV6621_MAC_LENGTH]);

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
  uint8_t reg = 0x0d;
  uint8_t rback = 0;
  int attempt;

  struct i2c_msg_s wmsg = {
    .frequency = 400000, .addr = 0x51, .flags = 0, .buffer = wbuf, .length = 2
  };
  struct i2c_msg_s pmsg = {
    .frequency = 400000, .addr = 0x51, .flags = 0, .buffer = &reg, .length = 1
  };
  struct i2c_msg_s dmsg = { .frequency = 400000,
                            .addr = 0x51,
                            .flags = I2C_M_READ,
                            .buffer = &rback,
                            .length = 1 };

  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_B7 | GPIO_ALT | GPIO_AF9 |
                     GPIO_PULLUP); /* I2C2 SCL */
  rk3576_config_gpio(GPIO_PORT0 | GPIO_PIN_C0 | GPIO_ALT | GPIO_AF9 |
                     GPIO_PULLUP); /* I2C2 SDA */

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
      if (wr >= 0 && pr >= 0 && rd >= 0 &&
          (rback & 0x83) == 0x80)
        {
          up_mdelay(150);
          return OK;
        }

      up_mdelay(5);
    }

  wlwarn("WARNING: hym8563 CLKOUT setup failed, readback=0x%02x\n", rback);
  return -EIO;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_power_on
 *
 * Description:
 *   Reset the combo through WL_REG_ON and wait for the boot ROM to settle.
 ****************************************************************************/

static int kickpi_k7_wifi_power_on(FAR void *arg)
{
  (void)arg;
  rk3576_gpio_write(WIFI_WL_REG_ON, false);
  up_mdelay(1000);
  rk3576_gpio_write(WIFI_WL_REG_ON, true);
  up_mdelay(200);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_power_off
 ****************************************************************************/

static void kickpi_k7_wifi_power_off(FAR void *arg)
{
  (void)arg;
  rk3576_gpio_write(WIFI_WL_REG_ON, false);
}

/****************************************************************************
 * Name: kickpi_k7_wifi_load_address
 ****************************************************************************/

static int kickpi_k7_wifi_load_address(
    FAR void *arg, uint8_t address[SV6621_MAC_LENGTH])
{
  FAR uint8_t *saved = arg;

  if (saved[0] == 0)
    {
      return -ENOENT;
    }

  memcpy(address, saved, SV6621_MAC_LENGTH);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_wifi_store_address
 ****************************************************************************/

static int kickpi_k7_wifi_store_address(
    FAR void *arg, FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  memcpy(arg, address, SV6621_MAC_LENGTH);
  return 0;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint8_t g_kickpi_k7_wifi_address[SV6621_MAC_LENGTH];
static FAR struct sv6621_dev_s *g_kickpi_k7_wifi_dev;

static const struct sv6621_board_ops_s g_kickpi_k7_wifi_board_ops = {
  .power_on = kickpi_k7_wifi_power_on,
  .power_off = kickpi_k7_wifi_power_off,
  .load_address = kickpi_k7_wifi_load_address,
  .store_address = kickpi_k7_wifi_store_address,
};

static const struct sv6621_regulatory_domain_s
    g_kickpi_k7_wifi_regulatory_domains[] = {
  {
    .country = { 'C', 'N' },
    .rule_count = 4,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 149, 17, 33, 0, 0 },
    },
  },
  {
    .country = { '0', '0' },
    .rule_count = 7,
    .rules = {
      { 1, 11, 20, 0, 0 },
      { 12, 2, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                        SV6621_REGULATORY_FLAG_AUTO_BW },
      { 14, 1, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                        SV6621_REGULATORY_FLAG_NO_OFDM },
      { 36, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                         SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                         SV6621_REGULATORY_FLAG_DFS |
                         SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 20, 0, SV6621_REGULATORY_FLAG_NO_IR |
                          SV6621_REGULATORY_FLAG_DFS },
      { 149, 17, 20, 0, SV6621_REGULATORY_FLAG_NO_IR },
    },
  },
  {
    .country = { 'U', 'S' },
    .rule_count = 5,
    .rules = {
      { 1, 11, 30, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 24, 0, SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 24, 0, SV6621_REGULATORY_FLAG_DFS },
      { 149, 17, 30, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
    },
  },
  {
    .country = { 'D', 'E' },
    .rule_count = 5,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 36, 13, 23, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_NO_OUTDOOR |
                          SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 41, 27, 0, SV6621_REGULATORY_FLAG_DFS },
      { 149, 25, 14, 0, 0 },
    },
  },
  {
    .country = { 'J', 'P' },
    .rule_count = 5,
    .rules = {
      { 1, 13, 20, 0, 0 },
      { 14, 1, 20, 0, SV6621_REGULATORY_FLAG_NO_OFDM },
      { 36, 13, 20, 0, SV6621_REGULATORY_FLAG_AUTO_BW },
      { 52, 13, 20, 0, SV6621_REGULATORY_FLAG_DFS |
                          SV6621_REGULATORY_FLAG_AUTO_BW },
      { 100, 45, 23, 0, SV6621_REGULATORY_FLAG_DFS },
    },
  },
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_wifi_initialize
 *
 * Description:
 *   Set up the SV6621 pin environment and hand off to the SeekWave core
 *   driver.  Muxes the SDIO bus at full drive, parks the BT-side companion
 *   pins the combo boot ROM samples (BT_RST low, BT_WAKE high, UART4 lines
 *   idle-high), enables the 32 kHz sleep clock, and brings up WiFi.
 ****************************************************************************/

int kickpi_k7_wifi_initialize(void)
{
  static bool initialized;
  static int init_result;
  struct sv6621_config_s config;
  int ret;
  int i;

  /* The pinmux / 32 kHz / I2C bring-up must run once: re-running it on a
   * live system races the running driver and hangs the I2C poll.
   */

  if (initialized)
    {
      return init_result;
    }

  /* SDIO bus mux (GPIO1, func 2, pull-up) + max drive strength. */

  for (i = 0;
       i < (int)(sizeof(g_wifi_sdio_pins) / sizeof(g_wifi_sdio_pins[0])); i++)
    {
      rk3576_config_gpio(g_wifi_sdio_pins[i]);
    }

  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV0);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV1);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV2);
  putreg32((0xffu << 16) | 0xffu, WIFI_IOC_DRV3);

  /* Companion pin environment the SV6160lite boot ROM samples.  Keep the
   * Bluetooth side in reset while Wi-Fi boots, matching the last known-good
   * networking build; BT wake and the UART4 lines remain idle-high.
   */

  rk3576_config_gpio(WIFI_BT_RST);
  rk3576_gpio_write(WIFI_BT_RST, false);
  rk3576_config_gpio(GPIO_PORT1 | GPIO_PIN_D4 | GPIO_OUTPUT);
  rk3576_gpio_write(GPIO_PORT1 | GPIO_PIN_D4 | GPIO_OUTPUT, true);
  rk3576_config_gpio(GPIO_PORT1 | GPIO_PIN_C2 | GPIO_OUTPUT);
  rk3576_gpio_write(GPIO_PORT1 | GPIO_PIN_C2 | GPIO_OUTPUT, true);
  rk3576_config_gpio(GPIO_PORT1 | GPIO_PIN_C3 | GPIO_OUTPUT);
  rk3576_gpio_write(GPIO_PORT1 | GPIO_PIN_C3 | GPIO_OUTPUT, true);
  rk3576_config_gpio(GPIO_PORT1 | GPIO_PIN_C4 | GPIO_OUTPUT);
  rk3576_gpio_write(GPIO_PORT1 | GPIO_PIN_C4 | GPIO_OUTPUT, true);
  rk3576_config_gpio(GPIO_PORT1 | GPIO_PIN_C5 | GPIO_OUTPUT);
  rk3576_gpio_write(GPIO_PORT1 | GPIO_PIN_C5 | GPIO_OUTPUT, true);
  rk3576_config_gpio(WIFI_WL_REG_ON);

  ret = kickpi_k7_wifi_enable_32k();
  if (ret < 0)
    {
      wlwarn("WARNING: WiFi sleep clock setup failed: %d; continuing\n", ret);
    }

  memset(&config, 0, sizeof(config));
  config.transport = rk3576_sv6621_transport();
  config.board_ops = &g_kickpi_k7_wifi_board_ops;
  config.board_arg = g_kickpi_k7_wifi_address;
  config.iram.data = g_sv6621_iram_start;
  config.iram.length = g_sv6621_iram_end - g_sv6621_iram_start;
  config.dram.data = g_sv6621_dram_start;
  config.dram.length = g_sv6621_dram_end - g_sv6621_dram_start;
  config.nvram.data = g_sv6621_nv_start;
  config.nvram.length = g_sv6621_nv_end - g_sv6621_nv_start;
  config.calibration.data = g_sv6621_calib_start;
  config.calibration.length =
      g_sv6621_calib_end - g_sv6621_calib_start;
  config.regulatory = &g_kickpi_k7_wifi_regulatory_domains[0];
  config.regulatory_domains = g_kickpi_k7_wifi_regulatory_domains;
  config.regulatory_domain_count =
      sizeof(g_kickpi_k7_wifi_regulatory_domains) /
      sizeof(g_kickpi_k7_wifi_regulatory_domains[0]);
#ifdef CONFIG_SV6621_PM
  config.system_suspend.wake_enabled = true;
  config.system_suspend.wake_flags =
      SV6621_WAKE_DISCONNECT | SV6621_WAKE_MAGIC_PACKET |
      SV6621_WAKE_GTK_REKEY_FAILURE;
#endif

  ret = sv6621_create(&config, &g_kickpi_k7_wifi_dev);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_start(g_kickpi_k7_wifi_dev);
  if (ret < 0)
    {
      sv6621_destroy(g_kickpi_k7_wifi_dev);
      g_kickpi_k7_wifi_dev = NULL;
      return ret;
    }

  init_result = ret;
  initialized = true;

  return ret;
}

#endif /* CONFIG_KICKPI_K7_WIFI */
