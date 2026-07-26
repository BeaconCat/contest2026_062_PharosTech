/****************************************************************************
 * chips/rk3576/hardware/rk3576_wdt.h
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
 * RK3576 Watchdog Timer register definitions (TRM chapter 15).
 *
 * The block is a Synopsys DesignWare Watchdog ("snps,dw-wdt").  The SoC
 * instantiates six of them; this header targets the non-secure system
 * watchdog WDT_NS at 0x2ACE0000, which is the one exposed to the
 * application processor by the vendor device tree
 * (watchdog@2ace0000, interrupts = <GIC_SPI 40>).
 *
 * TRM base addresses of all six instances:
 *
 *   PMU_WDT  0x27340000   NPU_WDT  0x27780000   DDR_WDT  0x2A040000
 *   WDT_S    0x2A4C0000   WDT_NS   0x2ACE0000   BUS_WDT  0x2AEB0000
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_WDT_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.
 *
 * TODO: move to hardware/rk3576_memorymap.h once the chip-wide memory map
 * is updated; kept local so this driver is self-contained.
 */

#ifndef RK3576_WDT_NS_ADDR
#  define RK3576_WDT_NS_ADDR 0x2ACE0000 /* WDT_NS, 0x100 register window */
#endif

/* SYS_GRF, holds the per-watchdog "may reset the system" enables.
 *
 * TODO: move to hardware/rk3576_memorymap.h together with the other GRF
 * bases when a GRF driver is introduced.
 */


/* GIC interrupt number.
 *
 * The vendor DTS declares interrupts = <0 0x28 4>, i.e. SPI 40.  The GIC
 * INTID of an SPI is (SPI number + 32) = 72, which is already provided as
 * RK3576_IRQ_WDT_NS by chips/rk3576/include/irq.h.
 */

#define RK3576_WDT_NS_SPI 40

/* Counting-clock (tclk) frequency.
 *
 * The TRM allows the watchdog counter to be clocked either from the 24 MHz
 * crystal or from the 32 kHz slow clock.  The reset/boot selection for
 * WDT_NS on this SoC is the 24 MHz crystal (xin_osc0), which is also what
 * the vendor kernel reports for the "tclk" input.
 *
 * TODO: read the selection back from the CRU once a WDT clock API exists,
 * instead of assuming the reset value.
 */

#define RK3576_WDT_CLK_HZ RK3576_OSC_FREQ

/* Register offsets *********************************************************/

#define RK3576_WDT_CR   0x0000 /* Control register                        */
#define RK3576_WDT_TORR 0x0004 /* Timeout range register                  */
#define RK3576_WDT_CCVR 0x0008 /* Current counter value register (RO)     */
#define RK3576_WDT_CRR  0x000c /* Counter restart register (WO)           */
#define RK3576_WDT_STAT 0x0010 /* Interrupt status register (RO)          */
#define RK3576_WDT_EOI  0x0014 /* Interrupt clear register (RO, read-clr) */

/* WDT_CR (0x00) ************************************************************/

#define WDT_CR_EN                 (1 << 0) /* Watchdog enable (sticky)     */
#define WDT_CR_RMOD               (1 << 1) /* 0: reset, 1: IRQ then reset  */
#define WDT_CR_RMOD_RESET         (0 << 1)
#define WDT_CR_RMOD_IRQ           (1 << 1)
#define WDT_CR_RPL_SHIFT          2        /* Bits 4:2 reset pulse length  */
#define WDT_CR_RPL_MASK           (7 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_2PCLK          (0 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_4PCLK          (1 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_8PCLK          (2 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_16PCLK         (3 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_32PCLK         (4 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_64PCLK         (5 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_128PCLK        (6 << WDT_CR_RPL_SHIFT)
#define WDT_CR_RPL_256PCLK        (7 << WDT_CR_RPL_SHIFT)

/* WDT_TORR (0x04) **********************************************************/

#define WDT_TORR_PERIOD_SHIFT     0        /* Bits 3:0 timeout period      */
#define WDT_TORR_PERIOD_MASK      (0xf << WDT_TORR_PERIOD_SHIFT)
#define WDT_TORR_PERIOD_NR        16       /* Number of selectable ranges  */

/* Counter reload value for timeout index i (0..15): 2^(16 + i) - 1. */

#define WDT_TORR_TICKS(i)         ((uint32_t)(((uint64_t)1 << (16 + (i))) - 1))

/* WDT_CRR (0x0c) ***********************************************************/

#define WDT_CRR_KICK              0x76     /* Magic value that kicks the dog */

/* WDT_STAT (0x10) **********************************************************/

#define WDT_STAT_IRQ              (1 << 0) /* Interrupt pending            */

/* SYS_GRF_SOC_CON4 (SYS_GRF + 0x0010), hiword-masked *********************/

#define RK3576_SYS_GRF_SOC_CON4         0x0010

#define SYS_GRF_SOC_CON4_BUSWDT_PAUSE   (1 << 5)  /* BUS_WDT pause enable  */
#define SYS_GRF_SOC_CON4_WDTNS_PAUSE    (1 << 6)  /* WDT_NS pause enable   */
#define SYS_GRF_SOC_CON4_PMUWDT_GLBRST  (1 << 7)  /* PMU_WDT resets system */
#define SYS_GRF_SOC_CON4_WDTNS_GLBRST   (1 << 8)  /* WDT_NS resets system  */

/* Hiword-mask write helper: the upper 16 bits are a per-bit write enable
 * for the lower 16 bits.  WDT_HIWORD() sets the given bits, WDT_HIWORD_CLR()
 * clears them; the two may be OR-ed together when the bit sets are disjoint.
 */

#define WDT_HIWORD(bits)     (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define WDT_HIWORD_CLR(bits) ((uint32_t)(bits) << 16)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_WDT_H */
