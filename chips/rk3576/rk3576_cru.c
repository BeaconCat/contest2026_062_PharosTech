/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_cru.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <debug.h>

#include <nuttx/arch.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "rk3576_cru.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: cru_write_mask
 *
 * Description:
 *   Write a Rockchip CRU 16-bit write-mask register.  The upper 16 bits are
 *   the write enable: only the low-16 data bits whose mask bit is set are
 *   actually written, the rest are left untouched.  This lets independent
 *   fields (gating / divider / reset) be updated safely without a
 *   read-modify-write and without disturbing each other.
 *
 * Input Parameters:
 *   addr - CRU register physical address
 *   mask - bits to modify (within the low 16 bits)
 *   val  - data to write (only the bits covered by mask take effect)
 *
 ****************************************************************************/

static inline void cru_write_mask(uintptr_t addr, uint32_t mask,
                                  uint32_t val)
{
  putreg32((mask << 16) | (val & mask), addr);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_cru_sdio_enable
 *
 * Description:
 *   Enable the clock and reset of the SDIO controller (mmc@2a320000, the
 *   WiFi AP6256).  SDIO is not the boot device, so the MiniLoader leaves its
 *   clock domain uninitialized; this routine explicitly:
 *     1. Sets cclk_src_sdio source=gpll and divider=/24 -> 49.5 MHz (the
 *        reset default gpll/3 = 396 MHz exceeds the DW-MSHC 8-bit CLKDIV
 *        limit and cannot be divided down to the 400 kHz card-id clock).
 *     2. Enables the hclk_sdio (bus clock) and cclk_src_sdio (card clock
 *        source) gates.
 *     3. Pulses the hresetn_sdio soft reset to bring the controller to a
 *        known initial state.
 *   All register/bit values are cross-verified between TRM Part1 and the
 *   Linux clk-rk3576.c driver.
 *
 * Returned Value:
 *   The actual cclk_src_sdio frequency (Hz) for the host driver's CLKIN.
 *
 ****************************************************************************/

uint32_t rk3576_cru_sdio_enable(void)
{
  /* 1. cclk_src_sdio: sel=gpll(0), div_con=23 -> gpll(1188MHz)/24 = 49.5MHz */

  cru_write_mask(RK3576_CRU_CLKSEL_CON(RK3576_CRU_SDIO_CLKSEL),
                 RK3576_CRU_SDIO_SEL_MASK | RK3576_CRU_SDIO_DIV_MASK,
                 (RK3576_CRU_SDIO_SEL_GPLL << RK3576_CRU_SDIO_SEL_SHIFT) |
                 (RK3576_CRU_SDIO_DIV_CON << RK3576_CRU_SDIO_DIV_SHIFT));

  /* 2. Enable gates: a gate bit high = disabled, so writing 0 = enabled */

  cru_write_mask(RK3576_CRU_GATE_CON(RK3576_CRU_SDIO_GATE),
                 RK3576_CRU_SDIO_HCLK_BIT | RK3576_CRU_SDIO_CCLK_BIT,
                 0);

  /* 3. Soft-reset pulse: assert -> short delay -> deassert */

  cru_write_mask(RK3576_CRU_SOFTRST_CON(RK3576_CRU_SDIO_SOFTRST),
                 RK3576_CRU_SDIO_RST_BIT, RK3576_CRU_SDIO_RST_BIT);
  up_udelay(20);

  cru_write_mask(RK3576_CRU_SOFTRST_CON(RK3576_CRU_SDIO_SOFTRST),
                 RK3576_CRU_SDIO_RST_BIT, 0);
  up_udelay(20);

  mcinfo("SDIO CRU enabled: cclk_src_sdio = %u Hz\n",
         RK3576_CRU_SDIO_CCLK_FREQ);

  return RK3576_CRU_SDIO_CCLK_FREQ;
}

/****************************************************************************
 * Name: rk3576_cru_i2c2_enable
 *
 * Description:
 *   Bring up the I2C2 controller clocks (mmc@... hym8563 RTC bus).  The
 *   loader leaves i2c2 gated, so a raw access to the controller hangs; this
 *   selects clk_i2c2 = 100 MHz and ungates pclk_i2c2 + clk_i2c2.
 *
 ****************************************************************************/

void rk3576_cru_i2c2_enable(void)
{
  /* Select clk_i2c2 = xin24m (sel=3): always-on, unlike the 100/50 MHz PLL
   * children whose parents may be gated off.
   */

  cru_write_mask(RK3576_CRU_CLKSEL_CON(RK3576_CRU_I2C2_CLKSEL),
                 RK3576_CRU_I2C2_SEL_MASK,
                 RK3576_CRU_I2C2_SEL_24M << RK3576_CRU_I2C2_SEL_SHIFT);

  /* Ungate pclk_i2c2 + clk_i2c2 (a gate bit high = disabled, so write 0). */

  cru_write_mask(RK3576_CRU_GATE_CON(RK3576_CRU_I2C2_GATE),
                 RK3576_CRU_I2C2_PCLK_BIT | RK3576_CRU_I2C2_CCLK_BIT, 0);

  mcinfo("I2C2 CRU enabled (clk_i2c2 = 100 MHz)\n");
}
