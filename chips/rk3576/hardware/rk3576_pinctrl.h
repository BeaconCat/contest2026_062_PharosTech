/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_pinctrl.h
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
 * RK3576 IOC (IO Controller) register map used by the pinctrl framework.
 *
 * The RK3576 pad control registers are physically split into several IOC
 * sub-domains (PMU1_IOC, PMU2_IOC, TOP_IOC, VCCIO_IOC, ...), but the SoC
 * maps all of them into one contiguous 48 KiB window that starts at
 * RK3576_IOC_ADDR (0x26040000, "rockchip,rk3576-ioc-grf", reg length
 * 0xc000 in the vendor device tree).  Every offset below is therefore
 * relative to that single base and the sub-domain split shows up only as
 * discontinuities in the offset tables:
 *
 *   0x0000..0x0fff  PMU1_IOC   GPIO0_A .. GPIO0_B[0:3]  (always-on domain)
 *   0x2000..0x2fff  PMU2_IOC   GPIO0_B[4:7] .. GPIO0_D
 *   0x4000..0x7fff  TOP_IOC    GPIO1, GPIO2, GPIO3, GPIO4_A/B
 *   0xa000..0xafff  VCCIO6_IOC GPIO4_C
 *   0xb000..0xbfff  VCCIO7_IOC GPIO4_D
 *
 * GPIO0 needs the "special" treatment mentioned in the TRM only because
 * its B group straddles the PMU1/PMU2 boundary at pin 12: pins 0..11 are
 * in PMU1_IOC and pins 12..31 are in PMU2_IOC.  That boundary is encoded
 * in g_rk3576_pinctrl_routes[] below, so callers never see it.
 *
 * All IOC registers are HIWORD write-masked: bits [31:16] are a per-bit
 * write enable for bits [15:0].  A plain read-modify-write is both
 * unnecessary and wrong here; use RK3576_WRITE_MASK()/RK3576_WRITE_BIT()
 * from hardware/rk3576_gpio.h.
 *
 * Register geometry (identical to the single-pin path in rk3576_gpio.c):
 *
 *   IOMUX   4 bits/pin, 4 pins/register (per 8-pin group: 2 registers)
 *   PULL    2 bits/pin, 8 pins/register
 *   DRIVE   4 bits/pin, 4 pins/register
 *   SCHMITT 1 bit /pin, 8 pins/register
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PINCTRL_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PINCTRL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_gpio.h"
#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Size of the IOC window (vendor DTS: reg = <0x0 0x26040000 0x0 0xc000>). */

#define RK3576_IOC_SIZE 0x0000c000

/* Number of pad-control regions in the routing table. */

#define RK3576_PINCTRL_NROUTES 8

/* IOMUX geometry.  Shared with rk3576_gpio.c, which walks the same
 * g_iomux_groups[] table declared in hardware/rk3576_gpio.h.
 */

#define RK3576_MUX_BITS_PER_PIN 4
#define RK3576_MUX_PINS_PER_REG 4
#define RK3576_MUX_FUNC_MAX     15

/* GPIO0_B[4:7] (pins 12..15) live in PMU2_IOC while the rest of the
 * GPIO0_B IOMUX group lives in PMU1_IOC.  Their IOMUX register is the
 * PMU1_IOC group offset plus this fixup.
 */

#define RK3576_MUX_GPIO0BH_FIXUP 0x00001ff4

/* Pin range that needs the fixup above. */

#define RK3576_GPIO0_PMU2_FIRST_PIN 12

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One pad-control region: a contiguous run of pins inside one bank whose
 * pull / drive / schmitt registers all live in the same IOC sub-domain.
 *
 * The *_base fields are "bank origin" offsets in exactly the sense used by
 * rk3576_gpio.c: the register for a pin is
 *
 *   base + (pin / pins_per_reg) * 4
 *
 * with the *absolute* pin number inside the bank (not the offset from
 * first_pin).  Keeping that convention makes the table directly
 * comparable with the single-pin code path.
 */

struct rk3576_pinctrl_route_s
{
  uint8_t bank;       /* GPIO bank, 0..4                                  */
  uint8_t first_pin;  /* First pin covered by this region, 0..31          */
  uint8_t last_pin;   /* Last pin covered by this region, 0..31           */
  uint32_t pull_base; /* Bank-origin offset of the pull registers         */
  uint32_t drv_base;  /* Bank-origin offset of the drive registers        */
  uint32_t smt_base;  /* Bank-origin offset of the schmitt registers      */
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/* Complete pad-control routing table for GPIO0..GPIO4, 32 pins each. */

extern const struct rk3576_pinctrl_route_s
    g_rk3576_pinctrl_routes[RK3576_PINCTRL_NROUTES];

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PINCTRL_H */
