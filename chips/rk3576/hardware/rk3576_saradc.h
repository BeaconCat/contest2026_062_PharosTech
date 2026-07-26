/****************************************************************************
 * chips/rk3576/hardware/rk3576_saradc.h
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
 * Rockchip SAR-ADC v2 register definitions (the block RK3576 shares with
 * the RK3588, device-tree compatible
 * "rockchip,rk3576-saradc", "rockchip,rk3588-saradc").
 *
 * Register layout and reset values are taken from the Rockchip RK3576 TRM
 * Part1 V1.2-20240624, chapter 18 "SARADC", register summary section
 * 18.4.2.  Note that the register map is NOT densely packed: the threshold
 * comparator arrays are followed by holes, so every offset below is
 * spelled out rather than derived.
 *
 * Registers that carry a "write_enable" field in bits [31:16] are
 * write-masked (HIWORD): a bit in the low half is only committed when the
 * matching bit in the high half is set.  Use RK3576_SARADC_HIWORD() /
 * RK3576_SARADC_HIWORD_CLR() for those.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_SARADC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_SARADC_H

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
 * Vendor DTS: adc@2ae00000 { reg = <0x0 0x2AE00000 0x0 0x10000>; }
 *
 * TODO: move this definition to hardware/rk3576_memorymap.h once the
 * chip-wide peripheral base list is extended; it is kept local so that this
 * driver can be built without touching the shared header.
 */

#define RK3576_SARADC_ADDR 0x2AE00000

/* Interrupt.
 *
 * Vendor DTS: interrupts = <GIC_SPI 0x7C IRQ_TYPE_LEVEL_HIGH>, i.e. SPI 124,
 * so the GIC INTID is 124 + 32 = 156.  That INTID is already exported as
 * RK3576_IRQ_SARADC by chips/rk3576/include/irq.h, which this driver uses;
 * the value is repeated here only for documentation.
 */

#define RK3576_SARADC_SPI   124
#define RK3576_SARADC_INTID (RK3576_SARADC_SPI + 32)

/* Hardware capabilities.
 *
 * The CONV_CON.channel_sel field is four bits wide (16 channels are
 * addressable by the FSM) but RK3576 only routes eight analog inputs and
 * only provides DATA0..DATA7 / HIGH_COMP0..7 / LOW_COMP0..7.
 */

#define RK3576_SARADC_NCHANNELS 8
#define RK3576_SARADC_RESOLUTION 12
#define RK3576_SARADC_DATA_MASK 0x0fff

/* Reference functional clock for the converter (Hz).  The Rockchip driver
 * for this IP runs clk_saradc at 1 MHz on RK3588-class parts.
 */

#define RK3576_SARADC_CLK_HZ 1000000

/* Register offsets (TRM Part1 table in section 18.4.2) *********************/

#define RK3576_SARADC_CONV_CON     0x0000 /* Conversion control (HIWORD)   */
#define RK3576_SARADC_T_PD_SOC     0x0004 /* Power-up to SOC timing        */
#define RK3576_SARADC_T_AS_SOC     0x0008 /* Assert SOC timing             */
#define RK3576_SARADC_T_DAS_SOC    0x000c /* De-assert SOC to channel sel  */
#define RK3576_SARADC_T_SEL_SOC    0x0010 /* Channel load to SOC assert    */
#define RK3576_SARADC_HIGH_COMP(n) (0x0014 + ((n) * 4)) /* n = 0..7        */
#define RK3576_SARADC_LOW_COMP(n)  (0x0054 + ((n) * 4)) /* n = 0..7        */
#define RK3576_SARADC_DEBOUNCE     0x0094 /* Threshold debounce count      */
#define RK3576_SARADC_HT_INT_EN    0x0098 /* High-threshold IE (HIWORD)    */
#define RK3576_SARADC_LT_INT_EN    0x009c /* Low-threshold IE (HIWORD)     */
#define RK3576_SARADC_MT_INT_EN    0x0100 /* Middle-threshold IE (HIWORD)  */
#define RK3576_SARADC_END_INT_EN   0x0104 /* End-of-conversion IE (HIWORD) */
#define RK3576_SARADC_ST_CON       0x0108 /* Analog static control         */
#define RK3576_SARADC_STATUS       0x010c /* Converter status (RO)         */
#define RK3576_SARADC_END_INT_ST   0x0110 /* End-of-conversion IS (W1C)    */
#define RK3576_SARADC_HT_INT_ST    0x0114 /* High-threshold IS (W1C)       */
#define RK3576_SARADC_LT_INT_ST    0x0118 /* Low-threshold IS (W1C)        */
#define RK3576_SARADC_MT_INT_ST    0x011c /* Middle-threshold IS (W1C)     */
#define RK3576_SARADC_DATA(n)      (0x0120 + ((n) * 4)) /* n = 0..7        */
#define RK3576_SARADC_AUTO_CH_EN   0x0160 /* Auto-scan channel mask        */

/* SARADC_CONV_CON (0x0000, HIWORD) ****************************************/

#define SARADC_CONV_CON_CHANNEL_SEL_MASK  0x000f /* [3:0] channel select   */
#define SARADC_CONV_CON_CHANNEL_SEL_SHIFT 0
#define SARADC_CONV_CON_START             (1 << 4) /* Start conversion     */
#define SARADC_CONV_CON_SINGLE_PD_MODE    (1 << 5) /* One-shot, then PD    */
#define SARADC_CONV_CON_AUTO_CHANNEL_MODE (1 << 6) /* Scan AUTO_CH_EN mask */
#define SARADC_CONV_CON_END_CONV          (1 << 7) /* Terminate scan (W1C) */
#define SARADC_CONV_CON_AS_PD_MODE        (1 << 8) /* PD between samples   */
#define SARADC_CONV_CON_INT_LOCK          (1 << 9) /* Latch data on int    */

/* SARADC_END_INT_EN (0x0104, HIWORD) **************************************/

#define SARADC_END_INT_EN_EN (1 << 0) /* End-of-conversion interrupt enable */

/* SARADC_ST_CON (0x0108) **************************************************/

#define SARADC_ST_CON_CCTRL_MASK  0x0007 /* [2:0] capacitor DAC trim       */
#define SARADC_ST_CON_CCTRL_SHIFT 0
#define SARADC_ST_CON_ICTRL_MASK  0x0038 /* [5:3] preamplifier bias trim   */
#define SARADC_ST_CON_ICTRL_SHIFT 3

/* Power-on default of ST_CON per the TRM ("Reset Value 0x0000001C"):
 * ictrl = 3, cctrl = 4.  The driver restores this value explicitly so a
 * warm restart does not inherit a mis-trimmed analog front end.
 */

#define SARADC_ST_CON_DEFAULT 0x0000001c

/* SARADC_STATUS (0x010c, read-only) ***************************************/

#define SARADC_STATUS_CONV_ST     (1 << 0) /* 1: FSM busy                  */
#define SARADC_STATUS_PD          (1 << 1) /* 1: analog block powered down */
#define SARADC_STATUS_SEL_MASK    0x003c   /* [5:2] channel being sampled  */
#define SARADC_STATUS_SEL_SHIFT   2

/* SARADC_END_INT_ST (0x0110) - write 1 to clear *************************/

#define SARADC_END_INT_ST_ST (1 << 0) /* End-of-conversion interrupt state */

/* Reset values of the analog timing registers (TRM section 18.4.2).  These
 * are expressed in clk_saradc cycles and are valid for the 1 MHz functional
 * clock this driver programs.
 */

#define SARADC_T_PD_SOC_DEFAULT  0x13
#define SARADC_T_AS_SOC_DEFAULT  0x05
#define SARADC_T_DAS_SOC_DEFAULT 0x07
#define SARADC_T_SEL_SOC_DEFAULT 0x03

/* HIWORD write helpers.
 *
 * RK3576_SARADC_HIWORD(bits)     - set every bit in 'bits'.
 * RK3576_SARADC_HIWORD_CLR(bits) - clear every bit in 'bits'.
 *
 * A multi-bit field that may hold zeroes (channel_sel) has to be cleared
 * and set in the same write:
 *
 *   RK3576_SARADC_HIWORD(value) | RK3576_SARADC_HIWORD_CLR(field_mask)
 */

#define RK3576_SARADC_HIWORD(bits) \
  (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define RK3576_SARADC_HIWORD_CLR(bits) ((uint32_t)(bits) << 16)

/* CRU resources owned by the SARADC.
 *
 * CRU_GATE_CON13 (CRU base + 0x0834) holds both SARADC gates.  Like every
 * Rockchip gate bit these are active-low enables: writing 1 *disables* the
 * clock.
 *
 * CRU_CLKSEL_CON58 (CRU base + 0x03E8) holds the converter clock mux and
 * divider: clk_saradc = source / (div_con + 1).
 *
 * TODO: these belong behind a rk3576_cru_set_saradc_clock_gate() /
 * rk3576_cru_set_saradc_clock_selection() pair in chips/rk3576/rk3576_cru.c
 * once that driver grows SARADC support; they are defined here so the ADC
 * driver is self-contained in the meantime.
 */

#define RK3576_SARADC_CRU_GATE_CON_IDX  13
#define SARADC_CRU_GATE_CLK_SARADC      (1 << 7)
#define SARADC_CRU_GATE_PCLK_SARADC     (1 << 6)

#define RK3576_SARADC_CRU_CLKSEL_CON_IDX 58
#define SARADC_CRU_CLKSEL_DIV_MASK       0x0ff0 /* [11:4] div_con          */
#define SARADC_CRU_CLKSEL_DIV_SHIFT      4
#define SARADC_CRU_CLKSEL_SEL_MASK       0x1000 /* [12] source mux         */
#define SARADC_CRU_CLKSEL_SEL_GPLL       (0 << 12)
#define SARADC_CRU_CLKSEL_SEL_XIN_OSC0   (1 << 12)

/* The mux source this driver picks: the 24 MHz crystal, which is always
 * running and needs no PLL bring-up.
 */

#define RK3576_SARADC_CLK_SRC_HZ RK3576_OSC_FREQ

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_SARADC_H */
