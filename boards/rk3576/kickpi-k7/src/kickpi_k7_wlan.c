/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_wlan.c
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
 * The on-board WiFi/BT combo is an AP6256 (Broadcom BCM43456) on the SDIO
 * controller (SDIO0, mmc@2a320000).  This file wires it to the NuttX bcmf
 * (bcm43xxx) stack:
 *   - Mux the SDIO pins (sdmmc1 m0 group, all GPIO1, IOMUX func 2).
 *   - Drive the WL_REG_ON power line (GPIO1_C6) via the bcmf power callback.
 *   - Bring up the SDIO host (slot 1) and hand it to bcmf_sdio_initialize().
 * Pin/func values come from the reverse-engineered vendor DTS (sdmmc1m0,
 * sdio-pwrseq, wlan-platdata).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <errno.h>
#include <debug.h>
#include <netinet/ether.h>

#include <nuttx/arch.h>
#include <nuttx/sdio.h>
#include <nuttx/wireless/ieee80211/bcmf_sdio.h>
#include <nuttx/wireless/ieee80211/bcmf_board.h>

#include "rk3576_gpio.h"
#include "rk3576_sdmmc.h"
#include "kickpi_k7.h"

#ifdef CONFIG_IEEE80211_BROADCOM_FULLMAC_SDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SDIO bus pins: sdmmc1 m0 group, all on GPIO1, IOMUX alternate function 2,
 * internal pull-up (DTS pcfg-pull-up-drv-level-2).
 */

#define WLAN_SDIO_PIN(pin) \
  (GPIO_PORT1 | (pin) | GPIO_ALT | GPIO_AF2 | GPIO_PULLUP)

static const gpio_pinset_t g_wlan_sdio_pins[] =
{
  WLAN_SDIO_PIN(GPIO_PIN_B4),   /* SDIO_D0 */
  WLAN_SDIO_PIN(GPIO_PIN_B5),   /* SDIO_D1 */
  WLAN_SDIO_PIN(GPIO_PIN_B6),   /* SDIO_D2 */
  WLAN_SDIO_PIN(GPIO_PIN_B7),   /* SDIO_D3 */
  WLAN_SDIO_PIN(GPIO_PIN_C0),   /* SDIO_CMD */
  WLAN_SDIO_PIN(GPIO_PIN_C1),   /* SDIO_CLK */
};

/* WL_REG_ON power-enable line (active high), from sdio-pwrseq reset-gpios. */

#define WLAN_REG_ON  (GPIO_PORT1 | GPIO_PIN_C6 | GPIO_OUTPUT)

/* WL_HOST_WAKE out-of-band interrupt line, from WIFI,host_wake_irq. */

#define WLAN_HOST_WAKE (GPIO_PORT1 | GPIO_PIN_D5 | GPIO_INPUT)

/****************************************************************************
 * Public Functions (bcmf board callbacks)
 ****************************************************************************/

/****************************************************************************
 * Name: bcmf_board_initialize
 *
 * Description:
 *   One-time GPIO setup for the WiFi combo: mux the SDIO bus pins and drive
 *   WL_REG_ON low (powered off) until bcmf_board_power() turns it on.
 ****************************************************************************/

void bcmf_board_initialize(int minor)
{
  int i;

  for (i = 0; i < (int)(sizeof(g_wlan_sdio_pins) /
                        sizeof(g_wlan_sdio_pins[0])); i++)
    {
      rk3576_config_gpio(g_wlan_sdio_pins[i]);
    }

  rk3576_config_gpio(WLAN_REG_ON);
  rk3576_gpio_write(WLAN_REG_ON, false);

  rk3576_config_gpio(WLAN_HOST_WAKE);
}

/****************************************************************************
 * Name: bcmf_board_power
 *
 * Description:
 *   Drive the WL_REG_ON line to power the AP6256 on or off.  A power-on
 *   needs the post-power-on settle time from the DTS pwrseq (200ms).
 ****************************************************************************/

void bcmf_board_power(int minor, bool power)
{
  rk3576_gpio_write(WLAN_REG_ON, power);
  if (power)
    {
      up_mdelay(200);
    }
}

/****************************************************************************
 * Name: bcmf_board_reset
 *
 * Description:
 *   The AP6256 has no reset line separate from WL_REG_ON, so a reset is a
 *   power cycle handled by bcmf_board_power(); nothing to do here.
 ****************************************************************************/

void bcmf_board_reset(int minor, bool reset)
{
}

/****************************************************************************
 * Name: bcmf_board_setup_oob_irq
 *
 * Description:
 *   Attach the WL_HOST_WAKE out-of-band interrupt so the chip can wake the
 *   bcmf daemon.
 *   TODO: bridge GPIO1_D5 edge interrupt to func().  Until then bcmf falls
 *   back to polling (CONFIG_IEEE80211_BROADCOM_BCMFMAC_NO_OOB).
 ****************************************************************************/

void bcmf_board_setup_oob_irq(int minor, CODE int (*func)(FAR void *),
                              FAR void *arg)
{
}

/****************************************************************************
 * Name: bcmf_board_etheraddr
 *
 * Description:
 *   Return false so bcmf uses the MAC address programmed in the chip.
 ****************************************************************************/

bool bcmf_board_etheraddr(FAR struct ether_addr *ethaddr)
{
  return false;
}

/****************************************************************************
 * Name: kickpi_k7_wlan_initialize
 *
 * Description:
 *   Bring up the SDIO host (slot 1) and register the AP6256 with the bcmf
 *   stack.  Called from board_late_initialize().
 ****************************************************************************/

int kickpi_k7_wlan_initialize(void)
{
  FAR struct sdio_dev_s *sdio;
  int ret;

  sdio = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (sdio == NULL)
    {
      wlerr("ERROR: SDIO host (slot %d) init failed\n", RK3576_SDIO_SLOT);
      return -ENODEV;
    }

  ret = bcmf_sdio_initialize(0, sdio);
  if (ret < 0)
    {
      wlerr("ERROR: bcmf_sdio_initialize failed: %d\n", ret);
    }

  return ret;
}

#endif /* CONFIG_IEEE80211_BROADCOM_FULLMAC_SDIO */
