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
          return -3;                           /* transfer-phase timeout */
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

/* Raw register-read on the same RK I2C: hardware TRX mode does the whole
 * write-reg-then-read transaction (u-boot rk_i2c read path).
 *   MRXADDR(0x08)  = (slave<<1|1) | addr-valid bit24
 *   MRXRADDR(0x0c) = reg | reg-valid bit24
 *   CON mode=TRX(2), MRXCNT(0x14)=count -> wait IPD MBRF (bit3)
 */

static int i2c5_read_reg(uint8_t slave, uint8_t reg, uint8_t *val)
{
  int t;

  /* Phase 1: TX the register pointer [slave|W][reg] (like the write path
   * but only two bytes and no payload), then a repeated START in RX mode.
   */

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
  mmio_wr(I2C5_BASE + I2C_TXDATA0,
          ((uint32_t)slave << 1) | ((uint32_t)reg << 8));
  mmio_wr(I2C5_BASE + I2C_CON, 0x1);             /* EN, TX mode */
  mmio_wr(I2C5_BASE + I2C_MTXCNT, 2);
  for (t = 0; !(mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 2)); t++)
    {
      if (mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 6))
        {
          mmio_wr(I2C5_BASE + I2C_CON, 0);
          return -1;                             /* NAK on pointer write */
        }

      if (t > 100000)
        {
          mmio_wr(I2C5_BASE + I2C_CON, 0);
          return -2;
        }
    }

  /* Phase 2: repeated START, RX-only mode (mode 1): controller sends
   * MRXADDR (read address) itself, then clocks in MRXCNT bytes.
   */

  mmio_wr(I2C5_BASE + 0x08, ((uint32_t)(slave << 1) | 1) | (1u << 24));
  mmio_wr(I2C5_BASE + I2C_IPD, 0x7f);
  mmio_wr(I2C5_BASE + I2C_CON, 0x9 | (1 << 1));  /* EN | MOD_RX | START */
  mmio_wr(I2C5_BASE + 0x14, 1);                  /* MRXCNT = 1 */
  for (t = 0; !(mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 3)); t++)
    {
      if (t > 100000)
        {
          mmio_wr(I2C5_BASE + I2C_CON, 0);
          return -3;                             /* RX timeout */
        }
    }

  *val = (uint8_t)(mmio_rd(I2C5_BASE + 0x200) & 0xff);

  mmio_wr(I2C5_BASE + I2C_IPD, 0x7f);
  mmio_wr(I2C5_BASE + I2C_CON, 0x11);            /* EN | STOP */
  for (t = 0; !(mmio_rd(I2C5_BASE + I2C_IPD) & (1 << 5)); t++)
    {
      if (t > 100000)
        {
          break;
        }
    }

  mmio_wr(I2C5_BASE + I2C_CON, 0);
  return 0;
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

  /* Diagnostic: CRU gate for i2c2 (GATE_CON12 bit1=pclk bit13=clk, 0=enabled)
   * and the i2c2 controller CON/CLKDIV (readable => pclk present).  If the
   * i2c write below still times out at START with the gate bits clear, the
   * functional clk_i2c2 mux parent is not running.
   */

  syslog(LOG_ERR, "SDIOPROBE: GATE12=%08lx i2c2.CON=%08lx i2c2.CLKDIV=%08lx\n",
         (unsigned long)mmio_rd(0x27200830),
         (unsigned long)mmio_rd(I2C5_BASE + I2C_CON),
         (unsigned long)mmio_rd(I2C5_BASE + I2C_CLKDIV));

  /* Mux the I2C2_M0 pins (the loader leaves them un-muxed, so i2c START never
   * completes): SCL=GPIO0_B7 func9, SDA=GPIO0_C0 func9, via PMU1_IOC.  The
   * PMU IO-controller pclk (pclk_pmu0) is CLK_IS_CRITICAL, so the domain is
   * up and this write is safe (an earlier hang here was really the
   * .note.gnu.build-id entry bug, since fixed in dramboot.ld).
   */

  mmio_wr(0x26042000 + 0x00, (0xfu << 28) | (0x9u << 12));  /* GPIO0_B7 = I2C2_SCL_M0 */
  mmio_wr(0x26042000 + 0x04, (0xfu << 16) | (0x9u << 0));   /* GPIO0_C0 = I2C2_SDA_M0 */
  syslog(LOG_ERR, "SDIOPROBE: PMU1_IOC B_H=%08lx C_L=%08lx\n",
         (unsigned long)mmio_rd(0x26042000 + 0x00),
         (unsigned long)mmio_rd(0x26042000 + 0x04));
  up_udelay(10);

  /* Ensure the hym8563 drives its 32.768kHz CLKOUT (reg 0x0D = 0x80, FE=1
   * freq=32768): the RTL8822CS needs this LPO to come out of reset.
   */

  ret = i2c5_write_reg(0x51, 0x0d, 0x80);
  syslog(LOG_ERR, "SDIOPROBE: hym8563 CLKOUT enable i2c ret=%d "
         "(0=ack -1=nak -2=timeout)\n", ret);

  /* Read back CLKOUT (expect 0x80) plus control/seconds registers: if every
   * register reads 0xff the read path is broken (SDA stuck high); if 0x00
   * and the ticking 0x02 read sane values then 0x0D really is wrong and the
   * chip is emitting 1Hz (FD=11) instead of 32768Hz -- the LPO root cause.
   */

  {
    uint8_t r00 = 0xa5;
    uint8_t r02 = 0xa5;
    uint8_t r0d = 0xa5;
    int r;

    for (r = 0; r < 3 && i2c5_read_reg(0x51, 0x00, &r00) < 0; r++)
      {
        up_mdelay(2);
      }

    up_mdelay(2);
    for (r = 0; r < 3 && i2c5_read_reg(0x51, 0x02, &r02) < 0; r++)
      {
        up_mdelay(2);
      }

    up_mdelay(2);
    for (r = 0; r < 3; r++)
      {
        ret = i2c5_read_reg(0x51, 0x0d, &r0d);
        if (ret >= 0)
          {
            break;
          }

        up_mdelay(2);
      }

    syslog(LOG_ERR, "SDIOPROBE: hym8563 rb ret=%d r00=0x%02x r02=0x%02x "
           "r0D=0x%02x\n", ret, r00, r02, r0d);
  }

#ifdef CONFIG_RK3576_I2C
  /* Cross-check the bare i2c5_read_reg() readback against the real
   * rk3576_i2c driver: bring up I2C2 and read hym8563 reg 0x0D through the
   * standard i2c_master transfer path.  The driver value must match r0D
   * above (0xc4 observed) -- this validates the driver before we push it.
   */

  {
    struct i2c_master_s *i2c = rk3576_i2c_initialize(2);
    uint8_t wbuf3[2] = { 0x0d, 0x80 };            /* CLKOUT enable (3B write) */
    uint8_t regptr = 0x02;                        /* seconds register */
    uint8_t drvval = 0xa5;
    struct i2c_msg_s msgs[2];
    int dret = -ENODEV;

    if (i2c != NULL)
      {
        int w3;
        int w2;

        /* (1) 3-byte write [addr|W][0x0D][0x80] -- mirrors the bare CLKOUT
         * enable exactly.  (2) 2-byte pointer write [addr|W][0x02].  (3)
         * combined pointer-write + repeated-START read of reg 0x02.
         */

        msgs[0].frequency = 100000;
        msgs[0].addr      = 0x51;
        msgs[0].flags     = 0;
        msgs[0].buffer    = wbuf3;
        msgs[0].length    = 2;
        w3 = I2C_TRANSFER(i2c, msgs, 1);

        msgs[0].buffer    = &regptr;
        msgs[0].length    = 1;
        w2 = I2C_TRANSFER(i2c, msgs, 1);

        msgs[0].buffer    = &regptr;
        msgs[0].length    = 1;
        msgs[1].frequency = 100000;
        msgs[1].addr      = 0x51;
        msgs[1].flags     = I2C_M_READ;
        msgs[1].buffer    = &drvval;
        msgs[1].length    = 1;
        dret = I2C_TRANSFER(i2c, msgs, 2);

        syslog(LOG_ERR, "SDIOPROBE: hym8563 DRIVER w3=%d w2=%d\n", w3, w2);
      }

    syslog(LOG_ERR, "SDIOPROBE: hym8563 DRIVER reg02 ret=%d val=0x%02x "
           "(bare r02 above)\n", dret, drvval);
  }
#endif

  up_mdelay(10);

  /* Mux the SDIO bus pins (GPIO1, IOMUX func 2, pull-up). */

  for (i = 0; i < (int)(sizeof(g_sdio_pins) / sizeof(g_sdio_pins[0])); i++)
    {
      rk3576_config_gpio(g_sdio_pins[i]);
    }

  /* Enable the input schmitt trigger on the six SDIO pins -- Android's
   * pinctrl does (debugfs pinconf: "input schmitt enabled") and our GPIO
   * driver does not touch it.  GPIO1 SMT: VCCIO_IOC + 0x6210 + (pin/8)*4,
   * one bit per pin, Rockchip 16-bit write-mask.
   *   B4-B7 = pins 12-15 -> 0x26046214 bits 4-7
   *   C0-C1 = pins 16-17 -> 0x26046218 bits 0-1
   */

  mmio_wr(0x26046214, (0xf0u << 16) | 0xf0u);
  mmio_wr(0x26046218, (0x03u << 16) | 0x03u);
  syslog(LOG_ERR, "SDIOPROBE: SMT 6214=%08" PRIx32 " 6218=%08" PRIx32 "\n",
         mmio_rd(0x26046214), mmio_rd(0x26046218));

  /* Power sequence, mmc-pwrseq-simple semantics (order matters!):
   *   1. assert WL_REG_ON low (chip held in reset)
   *   2. bring up the SDIO host so the 400kHz card clock free-runs
   *   3. release WL_REG_ON high WHILE the clock is running
   *   4. post-power-on settle, then CMD5
   * The SV6621 boot ROM samples its host interface (SDIO/USB/UART tri-mode)
   * when it comes out of reset -- releasing reset before the clock runs made
   * it miss SDIO and never answer CMD5.
   */

  rk3576_config_gpio(WL_REG_ON);
  rk3576_gpio_write(WL_REG_ON, false);
  up_mdelay(20);

  /* Bring up the SDIO host (slot 1) with the chip still in reset. */

  dev = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (dev == NULL)
    {
      syslog(LOG_ERR, "SDIOPROBE: host init failed\n");
      return;
    }

  /* Release reset with the card clock running, then settle. */

  rk3576_gpio_write(WL_REG_ON, true);
  up_mdelay(200);

  syslog(LOG_ERR, "SDIOPROBE: WL_REG_ON readback=%d\n",
         rk3576_gpio_read(GPIO_PORT1 | GPIO_PIN_C6 | GPIO_INPUT));

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

  /* Let the free-running 400kHz card clock feed the chip before the first
   * command: the SV6621 boot ROM samples bus activity to detect its host
   * interface (SDIO/USB/UART tri-mode).
   */

  up_mdelay(100);

  /* Retry CMD5 enumeration: the SV6621 may need time to reach SDIO-ready
   * after power-on.  Log RINTSTS each try (0x100 = RTO, no response).
   */

  /* Linux mmc_rescan pre-enumeration sequence, verbatim: CMD52 write 0x08 to
   * CCCR reg 6 (I/O abort / RES, the SDIO-specific reset -- works before
   * enumeration and unsticks a wedged I/O state machine), CMD0, CMD8, then
   * CMD5.  Log the raw RINTSTS/response of each step: ANY response at all
   * proves the chip is alive on the bus.
   */

  {
    uint32_t rsp = 0;
    uint32_t st;

    st = sdio_raw_cmd(52, 0x80000c08, true, &rsp);
    syslog(LOG_ERR, "SDIOPROBE: raw CMD52(reset) RINTSTS=%08" PRIx32
           " RESP=%08" PRIx32 "\n", st, rsp);
    st = sdio_raw_cmd(0, 0, false, NULL);
    syslog(LOG_ERR, "SDIOPROBE: raw CMD0 RINTSTS=%08" PRIx32 "\n", st);
    up_mdelay(2);
    st = sdio_raw_cmd(8, 0x1aa, true, &rsp);
    syslog(LOG_ERR, "SDIOPROBE: raw CMD8 RINTSTS=%08" PRIx32
           " RESP=%08" PRIx32 "\n", st, rsp);
    st = sdio_raw_cmd(5, 0, true, &rsp);
    syslog(LOG_ERR, "SDIOPROBE: raw CMD5 RINTSTS=%08" PRIx32
           " RESP=%08" PRIx32 "\n", st, rsp);
  }

  for (i = 0; i < 8; i++)
    {
      if (i == 2)
        {
          /* Long power-down variant: hold WL_REG_ON low 500ms in case the
           * short 20ms pulse does not fully reset the interface-detect
           * logic, then release and settle.
           */

          rk3576_gpio_write(WL_REG_ON, false);
          up_mdelay(500);
          rk3576_gpio_write(WL_REG_ON, true);
          up_mdelay(300);
          syslog(LOG_ERR, "SDIOPROBE: long-PDN retry\n");
        }

      if (i == 4)
        {
          /* Second half of the tries at ~97kHz (CLKDIV=0xff): raw slow-clock
           * sweep in case the chip needs a sub-400kHz identification clock.
           * Sequence per DW-MSHC: CLKENA=0 -> update -> CLKDIV -> update ->
           * CLKENA=1 -> update, all via CMD_UPD_CLK|START.
           */

          uint32_t upd = (1u << 31) | (1u << 21) | (1u << 13);
          int t;

          mmio_wr(0x2a320010, 0);
          mmio_wr(0x2a32002c, upd);
          for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
          mmio_wr(0x2a320008, 0xff);
          mmio_wr(0x2a32002c, upd);
          for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
          mmio_wr(0x2a320010, 1);
          mmio_wr(0x2a32002c, upd);
          for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
          syslog(LOG_ERR, "SDIOPROBE: slow sweep CLKDIV=%08" PRIx32 "\n",
                 mmio_rd(0x2a320008));
          up_mdelay(100);
        }

      ret = sdio_probe(dev);
      syslog(LOG_ERR, "SDIOPROBE: CMD5 try%d ret=%d RINTSTS=%08" PRIx32 "\n",
             i, ret, mmio_rd(0x2a320000 + 0x44));
      if (ret >= 0)
        {
          break;
        }

      up_mdelay(200);
    }

  if (ret < 0)
    {
      /* Android-clocking replication: the vendor kernel NEVER uses the
       * DW-MSHC internal CLKDIV (dmesg shows "div = 0" at both 400kHz and
       * 198MHz); it re-rates the CRU source instead, and clk_summary
       * (cclk_src_sdio=396MHz vs bus 198MHz) proves a fixed /2 clkgen sits
       * between the CRU and the card clock.  If the internal divider is not
       * effective for the command path on this integration, all previous
       * tries actually ran CLK at cclk_src speed -- far too fast for
       * identification.  Replicate Android exactly: CRU source = xin24m/60
       * = 400kHz, internal CLKDIV = 0 (card clock 400kHz, or 200kHz if the
       * /2 clkgen applies -- both legal for ID), then raw CMD0/CMD5.
       */

      uint32_t upd = (1u << 31) | (1u << 21) | (1u << 13);
      uint32_t st;
      uint32_t rsp;
      int t;

      mmio_wr(0x272004a0, (0xffu << 16) | (2u << 6) | 59u); /* CLKSEL104 */
      syslog(LOG_ERR, "SDIOPROBE: ANDROID-CLK CLKSEL104=%08" PRIx32 "\n",
             mmio_rd(0x272004a0));

      mmio_wr(0x2a320010, 0);                    /* CLKENA=0 */
      mmio_wr(0x2a32002c, upd);
      for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
      mmio_wr(0x2a320008, 0);                    /* CLKDIV=0 */
      mmio_wr(0x2a32002c, upd);
      for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);
      mmio_wr(0x2a320010, 1);                    /* CLKENA=1 */
      mmio_wr(0x2a32002c, upd);
      for (t = 0; (mmio_rd(0x2a32002c) & (1u << 31)) && t < 100000; t++);

      /* Power-cycle the chip so it identifies on the new clock. */

      rk3576_gpio_write(WL_REG_ON, false);
      up_mdelay(100);
      rk3576_gpio_write(WL_REG_ON, true);
      up_mdelay(300);

      for (i = 0; i < 4; i++)
        {
          st = sdio_raw_cmd(0, 0, false, NULL);
          syslog(LOG_ERR, "SDIOPROBE: ANDROID-CLK CMD0 RINTSTS=%08" PRIx32
                 "\n", st);
          st = sdio_raw_cmd(5, 0, true, &rsp);
          syslog(LOG_ERR, "SDIOPROBE: ANDROID-CLK CMD5 try%d RINTSTS=%08"
                 PRIx32 " RESP=%08" PRIx32 "\n", i, st, rsp);
          if ((st & (1u << 8)) == 0 && (st & (1u << 2)) != 0)
            {
              break;                             /* no RTO + cmd done */
            }

          up_mdelay(100);
        }

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
