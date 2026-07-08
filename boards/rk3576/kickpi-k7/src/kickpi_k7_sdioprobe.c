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

#include "rk3576_gpio.h"
#include "rk3576_sdmmc.h"
#include "rk3576_cru.h"
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

/* Minimal raw RK3576 I2C master-write of one register on i2c@2ac50000 (the
 * bus the hym8563 RTC sits on).  Sequence per TRM + u-boot rk_i2c.c.
 * Returns 0 on ACK, -1 on NAK, -2 on timeout.
 */

#define I2C5_BASE      0x2ac50000
#define I2C_CON        0x0000
#define I2C_CLKDIV     0x0004
#define I2C_MTXCNT     0x0010
#define I2C_IPD        0x001c
#define I2C_TXDATA0    0x0100

static int i2c5_write_reg(uint8_t slave, uint8_t reg, uint8_t val)
{
  int t;

  mmio_wr(I2C5_BASE + I2C_CLKDIV, 0x003e003d);   /* ~100kHz @ PCLK 100MHz */

  /* START */

  mmio_wr(I2C5_BASE + I2C_IPD, 0x7f);
  mmio_wr(I2C5_BASE + I2C_CON, 0x9);             /* EN | START */
  for (t = 0; !(mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 4)); t++)
    {
      if (t > 100000)
        {
          return -2;
        }
    }

  mmio_wr(I2C5_BASE + I2C_IPD, 1 << 4);

  /* Transmit: [slave<<1|W][reg][val] */

  mmio_wr(I2C5_BASE + I2C_TXDATA0,
          ((uint32_t)slave << 1) | ((uint32_t)reg << 8) |
          ((uint32_t)val << 16));
  mmio_wr(I2C5_BASE + I2C_CON, 0x1);             /* EN, TX mode */
  mmio_wr(I2C5_BASE + I2C_MTXCNT, 3);            /* triggers the transfer */

  for (t = 0; ; t++)
    {
      uint32_t ipd = mmio_rd(I2C5_BASE + I2C_IPD);
      if (ipd & (1 << 6))                        /* NAK */
        {
          mmio_wr(I2C5_BASE + I2C_IPD, 0x7f);
          mmio_wr(I2C5_BASE + I2C_CON, 0);
          return -1;
        }

      if (ipd & (1 << 2))                        /* MBTF: tx done */
        {
          break;
        }

      if (t > 100000)
        {
          mmio_wr(I2C5_BASE + I2C_CON, 0);
          return -2;
        }
    }

  /* STOP */

  mmio_wr(I2C5_BASE + I2C_IPD, 0x7f);
  mmio_wr(I2C5_BASE + I2C_CON, 0x11);            /* EN | STOP */
  for (t = 0; !(mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 5)); t++)
    {
      if (t > 100000)
        {
          break;
        }
    }

  mmio_wr(I2C5_BASE + I2C_IPD, 1 << 5);
  mmio_wr(I2C5_BASE + I2C_CON, 0);
  return 0;
}

static int sdio_rd(FAR struct sdio_dev_s *dev, uint32_t addr, uint8_t *val)
{
  return sdio_io_rw_direct(dev, false, 0, addr, 0, val);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void kickpi_k7_sdio_probe(void)
{
  FAR struct sdio_dev_s *dev;
  uint32_t cis;
  uint8_t b0;
  uint8_t b1;
  uint8_t b2;
  int ret;
  int i;

  syslog(LOG_ERR, "SDIOPROBE: ===== start =====\n");

  /* The hym8563 RTC 32.768kHz CLKOUT is the RTL8822CS sleep clock (schematic
   * K7_V2.1: HYM8563 CLKOUT -> 32KOUT_RTC -> WIFIBT_32KIN).  It sits on I2C2,
   * which the loader leaves gated, so ungate the I2C2 clocks before touching
   * the RTC.  The I2C2_M0 pins (GPIO0_B7/C0) live in the PMU1_IOC domain;
   * the loader already muxes them to reach the RTC at boot, so we do not
   * touch PMU1_IOC here (its pclk is not guaranteed on and a blind write
   * hangs the bus).
   */

  rk3576_cru_i2c2_enable();
  up_udelay(10);

  /* Ensure the hym8563 drives its 32.768kHz CLKOUT (reg 0x0D = 0x80, FE=1
   * freq=32768): the RTL8822CS needs this LPO to come out of reset.
   */

  ret = i2c5_write_reg(0x51, 0x0d, 0x80);
  syslog(LOG_ERR, "SDIOPROBE: hym8563 CLKOUT enable i2c ret=%d "
         "(0=ack -1=nak -2=timeout)\n", ret);
  up_mdelay(10);

  /* Mux the SDIO bus pins (GPIO1, IOMUX func 2, pull-up). */

  for (i = 0; i < (int)(sizeof(g_sdio_pins) / sizeof(g_sdio_pins[0])); i++)
    {
      rk3576_config_gpio(g_sdio_pins[i]);
    }

  /* Power sequence (mmc-pwrseq-simple): assert WL_REG_ON low (reset), then
   * deassert high, then the post-power-on settle time.
   */

  rk3576_config_gpio(WL_REG_ON);
  rk3576_gpio_write(WL_REG_ON, false);
  up_mdelay(20);
  rk3576_gpio_write(WL_REG_ON, true);
  up_mdelay(200);

  syslog(LOG_ERR, "SDIOPROBE: WL_REG_ON readback=%d\n",
         rk3576_gpio_read(GPIO_PORT1 | GPIO_PIN_C6 | GPIO_INPUT));

  /* Bring up the SDIO host (slot 1) and enumerate the SD-IO card. */

  dev = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "SDIOPROBE: host init failed\n");
      return;
    }

  /* Diagnostics: CRU regs (SDIO clock/reset) and a controller register to
   * confirm the SDIO0 block is clocked (a dead clock reads back as 0).
   */

  syslog(LOG_ERR, "SDIOPROBE: CRU CLKSEL104=%08" PRIx32 " GATE42=%08" PRIx32
         " SOFTRST42=%08" PRIx32 "\n",
         mmio_rd(0x272004a0), mmio_rd(0x272008a8), mmio_rd(0x27200aa8));
  syslog(LOG_ERR, "SDIOPROBE: SDIO0 CTRL=%08" PRIx32 " VERID=%08" PRIx32 "\n",
         mmio_rd(0x2a320000 + 0x00), mmio_rd(0x2a320000 + 0x6c));

  /* Pin mux readback (should be nibble 2 = SDIO func for each pin) and the
   * controller card-clock registers (CLKDIV/CLKENA/CLKSRC).
   */

  syslog(LOG_ERR, "SDIOPROBE: IOMUX B4-7=%08" PRIx32 " C0-1=%08" PRIx32 "\n",
         mmio_rd(0x2604402c), mmio_rd(0x26044030));
  syslog(LOG_ERR, "SDIOPROBE: CLKDIV=%08" PRIx32 " CLKENA=%08" PRIx32
         " CLKSRC=%08" PRIx32 "\n",
         mmio_rd(0x2a320008), mmio_rd(0x2a320010), mmio_rd(0x2a32000c));

  ret = sdio_probe(dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SDIOPROBE: sdio_probe (CMD5) failed: %d "
             "(no SD-IO card responded)\n", ret);
      return;
    }

  syslog(LOG_ERR, "SDIOPROBE: sdio_probe OK -- SD-IO card present\n");

  /* Read the common CIS pointer from CCCR, then walk the CIS tuples for the
   * manufacturer-ID tuple to report vendor:device.
   */

  if (sdio_rd(dev, CCCR_CIS_PTR + 0, &b0) < 0 ||
      sdio_rd(dev, CCCR_CIS_PTR + 1, &b1) < 0 ||
      sdio_rd(dev, CCCR_CIS_PTR + 2, &b2) < 0)
    {
      syslog(LOG_ERR, "SDIOPROBE: CCCR read failed\n");
      return;
    }

  cis = (uint32_t)b0 | ((uint32_t)b1 << 8) | ((uint32_t)b2 << 16);
  syslog(LOG_ERR, "SDIOPROBE: CIS ptr = 0x%06" PRIx32 "\n", cis);

  for (i = 0; i < 256; i++)
    {
      uint8_t code;
      uint8_t link;

      if (sdio_rd(dev, cis, &code) < 0)
        {
          break;
        }

      if (code == CISTPL_END)
        {
          break;
        }

      if (sdio_rd(dev, cis + 1, &link) < 0)
        {
          break;
        }

      if (code == CISTPL_MANFID && link >= 4)
        {
          uint8_t m0;
          uint8_t m1;
          uint8_t c0;
          uint8_t c1;

          sdio_rd(dev, cis + 2, &m0);
          sdio_rd(dev, cis + 3, &m1);
          sdio_rd(dev, cis + 4, &c0);
          sdio_rd(dev, cis + 5, &c1);

          syslog(LOG_ERR, "SDIOPROBE: MANFID vendor=0x%02x%02x "
                 "device=0x%02x%02x  %s\n", m1, m0, c1, c0,
                 (m1 == 0x02 && m0 == 0x4c) ? "(RTL8822CS)" : "");
          break;
        }

      cis += 2 + link;
    }

  syslog(LOG_ERR, "SDIOPROBE: ===== done =====\n");
}

#endif /* CONFIG_KICKPI_K7_SDIO_PROBE */
