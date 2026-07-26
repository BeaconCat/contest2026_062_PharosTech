/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_saradc.h
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
 * Rockchip SAR-ADC v2 register definitions.  RK3576 instantiates the same
 * block as the RK3588; the vendor device tree node is
 *
 *   adc@2ae00000 {
 *     compatible = "rockchip,rk3576-saradc", "rockchip,rk3588-saradc";
 *     reg = <0x0 0x2AE00000 0x0 0x10000>;
 *     interrupts = <GIC_SPI 0x7C IRQ_TYPE_LEVEL_HIGH>;
 *     clock-names = "saradc", "apb_pclk";
 *   };
 *
 * The v2 register map is sparse: the threshold comparator arrays, the
 * interrupt block and the data array each sit at their own 0x100 aligned
 * island, so every offset is spelled out rather than derived.
 *
 * Control registers marked HIWORD are write-masked: a bit in the low half
 * is only committed when the matching bit of the high half is set.  Use
 * RK3576_SARADC_HIWORD() / RK3576_SARADC_HIWORD_CLR() for those.
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

/* Controller base address.  Defined here only until the chip-wide
 * peripheral base list in hardware/rk3576_memorymap.h grows the entry; the
 * guard keeps this header working either way.
 */

#ifndef RK3576_SARADC_ADDR
#define RK3576_SARADC_ADDR 0x2ae00000
#endif

/* Hardware capabilities.  CONV_CON.channel_sel is four bits wide, but
 * RK3576 only routes eight analog inputs and only implements DATA0..DATA7.
 */

#define RK3576_SARADC_NCHANNELS  8
#define RK3576_SARADC_RESOLUTION 12
#define RK3576_SARADC_DATA_MASK  0x0fff

/* Register offsets ********************************************************/

#define RK3576_SARADC_CONV_CON     0x0000 /* Conversion control (HIWORD)   */
#define RK3576_SARADC_T_PD_SOC     0x0004 /* Power-down to SOC timing      */
#define RK3576_SARADC_T_AS_SOC     0x000c /* Analog settling timing        */
#define RK3576_SARADC_T_DAS_SOC    0x0010 /* Data available after SOC      */
#define RK3576_SARADC_T_SEL_SOC    0x0018 /* Channel select to SOC timing  */
#define RK3576_SARADC_HIGH_COMP(n) (0x0100 + ((n) * 4)) /* n = 0..7        */
#define RK3576_SARADC_LOW_COMP(n)  (0x0180 + ((n) * 4)) /* n = 0..7        */
#define RK3576_SARADC_DEBOUNCE     0x0200 /* Threshold debounce count      */
#define RK3576_SARADC_HT_INT_EN    0x0204 /* High-threshold IE (HIWORD)    */
#define RK3576_SARADC_LT_INT_EN    0x0208 /* Low-threshold IE (HIWORD)     */
#define RK3576_SARADC_MT_INT_EN    0x020c /* Middle-threshold IE (HIWORD)  */
#define RK3576_SARADC_END_INT_EN   0x0210 /* End-of-conversion IE (HIWORD) */
#define RK3576_SARADC_ST_CON       0x0214 /* Analog static control         */
#define RK3576_SARADC_STATUS       0x0218 /* Converter status (read only)  */
#define RK3576_SARADC_END_INT_ST   0x021c /* End-of-conversion IS (W1C)    */
#define RK3576_SARADC_HT_INT_ST    0x0220 /* High-threshold IS (W1C)       */
#define RK3576_SARADC_LT_INT_ST    0x0224 /* Low-threshold IS (W1C)        */
#define RK3576_SARADC_MT_INT_ST    0x0228 /* Middle-threshold IS (W1C)     */
#define RK3576_SARADC_DATA(n)      (0x0300 + ((n) * 4)) /* n = 0..7        */

/* SARADC_CONV_CON (0x0000, HIWORD) ****************************************/

#define SARADC_CONV_CON_CHANNEL_SEL_MASK  0x000f   /* [3:0] channel select */
#define SARADC_CONV_CON_CHANNEL_SEL_SHIFT 0
#define SARADC_CONV_CON_START             (1 << 4) /* Start conversion     */
#define SARADC_CONV_CON_SINGLE_MODE       (1 << 5) /* One-shot conversion  */

/* SARADC_END_INT_EN (0x0210, HIWORD) **************************************/

#define SARADC_END_INT_EN_EN (1 << 0) /* End-of-conversion interrupt enable */

/* SARADC_ST_CON (0x0214) **************************************************/

#define SARADC_ST_CON_CCTRL_MASK  0x0007 /* [2:0] capacitor DAC trim       */
#define SARADC_ST_CON_CCTRL_SHIFT 0
#define SARADC_ST_CON_ICTRL_MASK  0x0038 /* [5:3] preamplifier bias trim   */
#define SARADC_ST_CON_ICTRL_SHIFT 3

/* Power-on default of ST_CON: ictrl = 3, cctrl = 4.  The driver restores
 * it explicitly so a warm restart cannot inherit a mis-trimmed front end.
 */

#define SARADC_ST_CON_DEFAULT 0x0000001c

/* SARADC_STATUS (0x0218, read only) ***************************************/

#define SARADC_STATUS_CONV_ST (1 << 0) /* 1: conversion FSM busy           */

/* SARADC_END_INT_ST (0x021c) - write 1 to clear ***************************/

#define SARADC_END_INT_ST_ST (1 << 0) /* End-of-conversion interrupt state */

/* Analog timing requirements of the converter, in nanoseconds.  They are
 * programmed into T_PD_SOC / T_AS_SOC / T_DAS_SOC / T_SEL_SOC as a number
 * of clk_saradc cycles, so the driver has to scale them with the real
 * functional clock rate reported by the CLK framework.
 *
 * TODO: the numbers below reproduce the reset values of the timing
 * registers at the 1 MHz clk_saradc the vendor kernel programs (0x14 /
 * 0x02 / 0x02 / 0x01).  Cross-check against the RK3576 TRM SARADC chapter
 * once the analog timing table is available.
 */

#define SARADC_T_PD_SOC_NS  20000 /* Power-down settling                   */
#define SARADC_T_AS_SOC_NS  2000  /* Analog input settling                 */
#define SARADC_T_DAS_SOC_NS 2000  /* Data available after start-of-convert */
#define SARADC_T_SEL_SOC_NS 1000  /* Channel select setup                  */

/* Nominal functional clock this driver asks the CLK framework for.  The
 * Rockchip reference driver runs clk_saradc at 1 MHz on RK3588 class SoCs.
 */

#define RK3576_SARADC_CLK_HZ 1000000

/* HIWORD write helpers.
 *
 * RK3576_SARADC_HIWORD(bits)     - set every bit in 'bits'.
 * RK3576_SARADC_HIWORD_CLR(bits) - clear every bit in 'bits'.
 * RK3576_SARADC_HIWORD_ALL(val)  - write the whole low half-word.
 *
 * A multi-bit field that may hold zeroes (channel_sel) has to be cleared
 * and set within a single write:
 *
 *   RK3576_SARADC_HIWORD(value) | RK3576_SARADC_HIWORD_CLR(field_mask)
 */

#define RK3576_SARADC_HIWORD(bits) \
  (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define RK3576_SARADC_HIWORD_CLR(bits) ((uint32_t)(bits) << 16)
#define RK3576_SARADC_HIWORD_ALL(val) \
  (0xffff0000u | ((uint32_t)(val) & 0xffffu))

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_SARADC_H */
