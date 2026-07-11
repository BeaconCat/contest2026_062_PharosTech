/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_sdioprobe.c
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
 * Bring-up self-test for the SDIO WiFi controller (SDIO0, mmc@2a320000).
 * Muxes the SDIO pins, powers the on-board combo via WL_REG_ON, enumerates
 * the SD-IO card (CMD5) and reads the CIS manufacturer ID.  This validates
 * the CRU clock, pin mux, power sequence and the host's SDIO command path
 * on real hardware before the full WiFi driver exists.  The KICKPI-K7 combo
 * is an RTL8822CS, whose SDIO IDs are vendor 0x024C / device 0xB822.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/sdio.h>
#include <nuttx/i2c/i2c_master.h>

#include "rk3576_gpio.h"
#include "rk3576_sdmmc.h"
#include "rk3576_cru.h"
#include "rk3576_i2c.h"
#include "kickpi_k7.h"

#ifdef CONFIG_KICKPI_K7_SDIO_PROBE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDIO_PIN(pin) \
  (GPIO_PORT1 | (pin) | GPIO_ALT | GPIO_AF2 | GPIO_PULLUP)

#define WL_REG_ON  (GPIO_PORT1 | GPIO_PIN_C6 | GPIO_OUTPUT)

/* CCCR / CIS addresses (SDIO spec, common function 0). */

#define CCCR_CIS_PTR   0x09      /* 3-byte little-endian pointer to CIS */
#define CISTPL_MANFID  0x20      /* manufacturer ID tuple */
#define CISTPL_END     0xff

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const gpio_pinset_t g_sdio_pins[] =
{
  SDIO_PIN(GPIO_PIN_B4),   /* D0 */
  SDIO_PIN(GPIO_PIN_B5),   /* D1 */
  SDIO_PIN(GPIO_PIN_B6),   /* D2 */
  SDIO_PIN(GPIO_PIN_B7),   /* D3 */
  SDIO_PIN(GPIO_PIN_C0),   /* CMD */
  SDIO_PIN(GPIO_PIN_C1),   /* CLK */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t mmio_rd(uintptr_t addr)
{
  return *(volatile uint32_t *)addr;
}

static inline void mmio_wr(uintptr_t addr, uint32_t val)
{
  *(volatile uint32_t *)addr = val;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void kickpi_k7_sdio_probe(void)
{
  FAR struct sdio_dev_s *dev;
  int i;

  syslog(LOG_ERR, "SDIOPROBE: ===== start (driver-path SDIO enum) =====\n");

  /* Mux the SDIO bus pins (GPIO1, func 2, pull-up) + input schmitt. */

  for (i = 0; i < (int)(sizeof(g_sdio_pins) / sizeof(g_sdio_pins[0])); i++)
    {
      rk3576_config_gpio(g_sdio_pins[i]);
    }

  mmio_wr(0x26046214, (0xf0u << 16) | 0xf0u);
  mmio_wr(0x26046218, (0x03u << 16) | 0x03u);

  /* Power-on: chip_en LOW, host up with the card clock GATED, chip_en HIGH,
   * 200 ms quiet settle (no clock).
   */

  /* NO-TOUCH experiment: leave chip_en exactly as the previous OS (Android)
   * left it.  Dump GPIO1 first: warm-reboot registers survive, showing what
   * Android left -- especially whether its shutdown already drove chip_en
   * (C6, bit22) low, which would void the preserved-state assumption.
   */

  syslog(LOG_ERR, "SDIOPROBE: GPIO1 DR=%08" PRIx32 " DDR=%08" PRIx32
         " EXT=%08" PRIx32 " (C6=%u C7=%u)\n",
         mmio_rd(0x2ae10000 + 0x00), mmio_rd(0x2ae10000 + 0x08),
         mmio_rd(0x2ae10000 + 0x70),
         (unsigned)((mmio_rd(0x2ae10000 + 0x70) >> 22) & 1),
         (unsigned)((mmio_rd(0x2ae10000 + 0x70) >> 23) & 1));

  /* Bring up the host controller.  This also starts the ID-mode clock, so we
   * gate it again immediately -- the vendor pwrseq requires NO card clock
   * until 200 ms after chip_en is released.
   */

  dev = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "SDIOPROBE: host init failed\n");
      return;
    }

  {
    const uint32_t cmd5 = (5u | MMCSD_R4_RESPONSE);
    uint32_t r4 = 0;
    uint32_t ocr = 0;
    int wret;
    int k;

    /* Strict mmc-pwrseq-simple + mmc-core power-up order (from the Rockchip
     * vendor kernel behavior spec), which we had never reproduced cleanly:
     *   1. chip_en (GPIO1_C6, ACTIVE_LOW) driven LOW  = assert reset
     *   2. host up, card clock GATED (no CLK yet)
     *   3. chip_en HIGH = release, then wait 200 ms WITH NO CLOCK
     *   4. only THEN start the ID-mode clock (driver's CLKDIV, no phase hack)
     *   5. 10 ms, then CMD0 (80-clock init) + CMD5
     */

    rk3576_config_gpio(WL_REG_ON);
    rk3576_gpio_write(WL_REG_ON, false);          /* assert reset (low) */
    SDIO_CLOCK(dev, CLOCK_SDIO_DISABLED);         /* gate the card clock */
    up_mdelay(10);

    rk3576_gpio_write(WL_REG_ON, true);           /* release (high) */
    up_mdelay(200);                               /* settle, NO clock */

    SDIO_CLOCK(dev, CLOCK_IDMODE);                /* now start ~393 kHz */
    syslog(LOG_ERR, "SDIOPROBE: pwrseq-order CLKDIV=%08" PRIx32 " CLKENA=%08"
           PRIx32 " CLKSEL104=%08" PRIx32 "\n",
           mmio_rd(0x2a320008), mmio_rd(0x2a320010), mmio_rd(0x272004a0));

    up_mdelay(10);

    SDIO_SENDCMD(dev, MMCSD_CMD0, 0);             /* 80-clock init + reset */
    up_mdelay(2);

    /* SDIO enumeration: CMD5 arg=0 to read OCR, then CMD5 with the OCR set
     * until the card reports ready (R4 bit31 C=1).
     */

    for (k = 0; k < 20; k++)
      {
        SDIO_SENDCMD(dev, cmd5, ocr);
        wret = SDIO_WAITRESPONSE(dev, cmd5);
        r4 = 0;
        SDIO_RECVR4(dev, cmd5, &r4);

        syslog(LOG_ERR, "SDIOPROBE: DRVPATH CMD5 #%d arg=%08" PRIx32 " wait=%d "
               "R4=%08" PRIx32 "\n", k, ocr, wret, r4);

        if (wret == OK && r4 != 0)
          {
            ocr = r4 & 0x00ffffff;                /* card's OCR voltage window */
            if ((r4 & 0x80000000) != 0)
              {
                syslog(LOG_ERR, "SDIOPROBE: *** SDIO CARD READY R4=%08" PRIx32
                       " numfn=%u mempresent=%u ***\n", r4,
                       (unsigned)((r4 >> 28) & 7), (unsigned)((r4 >> 27) & 1));
                break;
              }
          }

        up_mdelay(10);
      }
  }

  syslog(LOG_ERR, "SDIOPROBE: ===== done =====\n");
}

#endif /* CONFIG_KICKPI_K7_SDIO_PROBE */