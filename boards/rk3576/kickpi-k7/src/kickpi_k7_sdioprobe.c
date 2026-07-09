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

/* Raw DW-MSHC command (bypasses the mmcsd layer) to replicate Linux's exact
 * pre-enumeration sequence: CMD52 I/O-reset -> CMD0 -> CMD8 -> CMD5.
 * Returns RINTSTS; *resp gets RESP0 when a response was expected.
 */

static uint32_t sdio_raw_cmd(uint8_t idx, uint32_t arg, bool resp,
                             uint32_t *rsp)
{
  uint32_t rint;
  int t;

  mmio_wr(0x2a320044, 0xffffffff);               /* clear RINTSTS */
  mmio_wr(0x2a320028, arg);                      /* CMDARG */
  mmio_wr(0x2a32002c, (1u << 31) | (1u << 29) | (1u << 13) |
          (resp ? (1u << 6) : 0) | idx);         /* START|HOLD|WAITPRV */
  for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
  for (t = 0; !(mmio_rd(0x2a320044) & (1u << 2)) && t < 100000; t++);

  rint = mmio_rd(0x2a320044);
  if (rsp != NULL)
    {
      *rsp = mmio_rd(0x2a320030);                /* RESP0 */
    }

  return rint;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void kickpi_k7_sdio_probe(void)
{
  FAR struct sdio_dev_s *dev;
  uint32_t upd = (1u << 31) | (1u << 21) | (1u << 13);
  uint32_t rsp;
  uint32_t st;
  int i;
  int t;

  syslog(LOG_ERR, "SDIOPROBE: ===== start (clean, correct-phase-first) =====\n");

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

  rk3576_config_gpio(WL_REG_ON);
  rk3576_gpio_write(WL_REG_ON, false);
  up_mdelay(50);

  dev = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "SDIOPROBE: host init failed\n");
      return;
    }

  mmio_wr(0x2a320010, 0);                          /* CLKENA = 0 */
  mmio_wr(0x2a32002c, upd);
  for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);

  rk3576_gpio_write(WL_REG_ON, true);
  up_mdelay(200);

  /* CORRECT PHASE FIRST -- before any bus command.  Android clock path
   * (CLKDIV=0 so the internal phase is effective; CRU cclk_src = xin24m/30 =
   * 800 kHz, fixed /2 clkgen -> 400 kHz card clock).  MISC_CON MEM_CLK_
   * AUTOGATE and the phase regs are HIWORD-MASK (upper 16 = write-enable).
   * drive=180 deg puts CMD on the CLK-low/falling edge so the SV6621 samples
   * stable CMD at the rising edge (verified: CLK rises while CMD is steady).
   */

  mmio_wr(0x272004a0, (0xffu << 16) | (2u << 6) | 29u);  /* CLKSEL104 */
  mmio_wr(0x2a320010, 0);
  mmio_wr(0x2a32002c, upd);
  for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
  mmio_wr(0x2a320008, 0);                          /* CLKDIV = 0 */
  mmio_wr(0x2a32002c, upd);
  for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);

  mmio_wr(0x2a320138, 0x00200020);                /* MISC_CON MEM_CLK_AUTOGATE */
  mmio_wr(0x2a320130, 0x0ffe0004);                /* drive 180 deg */
  mmio_wr(0x2a320134, 0x0ffe0000);                /* sample 0 deg */

  mmio_wr(0x2a320010, 1);                          /* CLKENA = 1 */
  mmio_wr(0x2a32002c, upd);
  for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
  up_mdelay(10);

  syslog(LOG_ERR, "SDIOPROBE: 400kHz drive=180 CLKDIV=%08" PRIx32 " misc=%08"
         PRIx32 " con0=%08" PRIx32 " -- CMD5 is the FIRST command\n",
         mmio_rd(0x2a320008), mmio_rd(0x2a320138), mmio_rd(0x2a320130));

  /* Response-window pad sampling: fire CMD5 then immediately watch the CMD
   * pad (GPIO1 EXT_PORT bit16) for the chip pulling it LOW (its response start
   * bit + bits).  The teammate's analyzer saw 1-sample glitches after some
   * CMD5s -- if the chip really drives a response but our host keeps driving
   * CMD (bus contention), we should see CMD-low samples here even though the
   * controller reports RTO.  cmd_low > 0 = chip drove a response the host
   * failed to capture; 0 = chip truly silent.
   */

  {
    uint32_t e;
    int c5 = 0;
    int c0 = 0;
    int k;

    /* Control: CMD0 expects NO response -- CMD stays high afterwards. */

    mmio_wr(0x2a320044, 0xffffffff);
    mmio_wr(0x2a320028, 0);
    mmio_wr(0x2a32002c, (1u << 31) | (1u << 29) | (1u << 13) | 0);
    for (k = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && k < 100000; k++);
    for (k = 0; k < 80000; k++)
      {
        e = mmio_rd(0x2ae10000 + 0x70);
        if ((e & (1u << 16)) == 0)
          {
            c0++;
          }
      }

    /* CMD5 expects R4 -- if the chip responds, CMD is pulled low here. */

    mmio_wr(0x2a320044, 0xffffffff);
    mmio_wr(0x2a320028, 0);
    mmio_wr(0x2a32002c, (1u << 31) | (1u << 29) | (1u << 13) | (1u << 6) | 5);
    for (k = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && k < 100000; k++);
    for (k = 0; k < 80000; k++)
      {
        e = mmio_rd(0x2ae10000 + 0x70);
        if ((e & (1u << 16)) == 0)
          {
            c5++;
          }
      }

    syslog(LOG_ERR, "SDIOPROBE: RESPWIN CMD-low CMD0(noresp)=%d CMD5=%d "
           "/ 80000 (CMD5>>CMD0 => real chip response)\n", c0, c5);
  }

  /* CMD5 as the very first command on the bus, correct phase, repeated. */

  for (i = 0; i < 200; i++)
    {
      st = sdio_raw_cmd(5, 0, true, &rsp);
      if ((i % 20) == 0)
        {
          syslog(LOG_ERR, "SDIOPROBE: CMD5 #%d RINTSTS=%08" PRIx32
                 " RESP=%08" PRIx32 "\n", i, st, rsp);
        }

      if ((st & (1u << 8)) == 0 && (st & (1u << 2)) != 0)
        {
          syslog(LOG_ERR, "SDIOPROBE: *** CMD5 RESPONDED #%d RINTSTS=%08"
                 PRIx32 " RESP=%08" PRIx32 " ***\n", i, st, rsp);
          break;
        }

      up_mdelay(20);
    }

  syslog(LOG_ERR, "SDIOPROBE: ===== done =====\n");
}

#endif /* CONFIG_KICKPI_K7_SDIO_PROBE */
