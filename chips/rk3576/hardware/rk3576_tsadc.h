/****************************************************************************
 * chips/rk3576/hardware/rk3576_tsadc.h
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
 * RK3576 Temperature-Sensor ADC (TS-ADC) register definitions.
 *
 * Source: Rockchip RK3576 TRM Part1 V1.2, chapter 19 "Temperature-Sensor
 * ADC (TS-ADC)".  Single controller at 0x2AE70000, 0x400 register window,
 * six conversion channels (TRM Table 19-1):
 *
 *   0 near chip center   1 big core   2 little core
 *   3 GPU                4 NPU        5 DDR
 *
 * The controller converts every enabled channel in a loop ("auto mode")
 * and compares each result against a per-channel high-temperature
 * interrupt threshold (COMPn_INT) and a per-channel thermal-shutdown
 * threshold (COMPn_SHUT).  A shutdown event can be routed to the CRU (chip
 * reset) and/or to the TSADC_SHUT pad (PMIC).
 *
 * Registers whose upper half is a per-bit write-enable mask are marked
 * "WE" below and must be written through RK3576_TSADC_WE()/_WE_CLR().
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_TSADC_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_TSADC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.
 *
 * TODO: move to hardware/rk3576_memorymap.h once the integration commit
 * lands; kept local so this driver builds standalone.
 */

#ifndef RK3576_TSADC_ADDR
#  define RK3576_TSADC_ADDR 0x2ae70000
#endif

#define RK3576_TSADC_SIZE 0x400

/* Number of conversion channels (TRM Table 19-1). */

#define RK3576_TSADC_NCHAN 6

/* Channel index -> monitored block. */

#define RK3576_TSADC_CH_CENTER   0 /* Near chip center */
#define RK3576_TSADC_CH_BIGCORE  1 /* Cortex-A72 cluster */
#define RK3576_TSADC_CH_LITCORE  2 /* Cortex-A53 cluster */
#define RK3576_TSADC_CH_GPU      3 /* Mali GPU */
#define RK3576_TSADC_CH_NPU      4 /* NPU */
#define RK3576_TSADC_CH_DDR      5 /* DDR controller */

/* clk_tsadc frequency.  CRU_CLKSEL_CON59[7:0] (clk_tsadc_div) resets to
 * 0x0b, i.e. the 24 MHz oscillator divided by 12.  TRM section 19.5.2
 * requires this clock to be 2 MHz.
 */

#define RK3576_TSADC_CLK_HZ 2000000

/* Register offsets *********************************************************/

#define RK3576_TSADC_USER_CON       0x0000 /* User control (WE)            */
#define RK3576_TSADC_AUTO_CON       0x0004 /* Auto control (WE)            */
#define RK3576_TSADC_AUTO_STATUS    0x0008 /* Auto-mode status             */
#define RK3576_TSADC_AUTO_SRC       0x000c /* Channel enable mask (WE)     */
#define RK3576_TSADC_LT_EN          0x0010 /* Low-temp check enable (WE)   */
#define RK3576_TSADC_HT_INT_EN      0x0014 /* High-temp int enable (WE)    */
#define RK3576_TSADC_GPIO_EN        0x0018 /* Shut-to-GPIO enable (WE)     */
#define RK3576_TSADC_CRU_EN         0x001c /* Shut-to-CRU enable (WE)      */
#define RK3576_TSADC_LT_INT_EN      0x0020 /* Low-temp int enable (WE)     */
#define RK3576_TSADC_HLT_INT_PD     0x0024 /* High/low int status (W1C)    */
#define RK3576_TSADC_EOC_HSHUT_PD   0x0028 /* EOC / shut status (W1C)      */

#define RK3576_TSADC_DATA(n)        (0x002c + ((n) * 4)) /* Channel data   */
#define RK3576_TSADC_COMP_INT(n)    (0x006c + ((n) * 4)) /* HT int thresh  */
#define RK3576_TSADC_COMP_SHUT(n)   (0x010c + ((n) * 4)) /* Shut threshold */

#define RK3576_TSADC_HIGH_INT_DEBOUNCE   0x014c /* HT interrupt debounce  */
#define RK3576_TSADC_HIGH_TSHUT_DEBOUNCE 0x0150 /* Shutdown debounce      */
#define RK3576_TSADC_AUTO_PERIOD         0x0154 /* Conversion interleave  */
#define RK3576_TSADC_AUTO_PERIOD_HT      0x0158 /* Interleave when hot    */

#define RK3576_TSADC_COMP_LOW_INT(n) (0x015c + ((n) * 4)) /* LT threshold */

/* TSADC_USER_CON (0x0000) — write-enable protected ************************/

#define TSADC_USER_CON_SRC_SEL_MASK  0x000f /* [3:0] user-mode channel    */
#define TSADC_USER_CON_SRC_SEL_SHIFT 0
#define TSADC_USER_CON_POWER_UP      (1 << 4) /* 0=power down 1=power up  */
#define TSADC_USER_CON_START_MODE_SW (1 << 5) /* 1=start bit controlled   */
#define TSADC_USER_CON_EOC_INTEN     (1 << 6) /* End-of-conversion int    */
#define TSADC_USER_CON_START         (1 << 7) /* W1, self-clearing        */
#define TSADC_USER_CON_ADC_STATUS    (1 << 8) /* RO, 1=conversion running */

/* TSADC_AUTO_CON (0x0004) — write-enable protected ************************/

#define TSADC_AUTO_CON_AUTO_EN       (1 << 0) /* 1=auto (loop) mode       */
#define TSADC_AUTO_CON_Q_SEL         (1 << 1) /* 1=(q_max - q), negative
                                               * temperature coefficient  */
#define TSADC_AUTO_CON_ROUND_INT_EN  (1 << 2) /* Int after a full round   */
#define TSADC_AUTO_CON_TSHUT_POL_HIGH (1 << 8) /* 0=low active pad output */

/* TSADC_AUTO_STATUS (0x0008) — plain register ****************************/

#define TSADC_AUTO_STATUS_LAST_TSHUT_GPIO (1 << 0) /* W1C                 */
#define TSADC_AUTO_STATUS_LAST_TSHUT_CRU  (1 << 1) /* W1C                 */
#define TSADC_AUTO_STATUS_AUTO_RUNNING    (1 << 2) /* RO                  */
#define TSADC_AUTO_STATUS_HT_WARM         (1 << 3) /* RO, above COMP_SHUT */

/* Per-channel bit masks shared by AUTO_SRC / LT_EN / HT_INT_EN / GPIO_EN /
 * CRU_EN / LT_INT_EN (all write-enable protected, one bit per channel).
 */

#define TSADC_CHAN_BIT(n)  (1 << (n))
#define TSADC_CHAN_MASK    ((1 << RK3576_TSADC_NCHAN) - 1)

/* TSADC_HLT_INT_PD (0x0024) — write 1 to clear ***************************/

#define TSADC_HLT_INT_PD_HT(n) (1 << (n))        /* [15:0]  high-temp int */
#define TSADC_HLT_INT_PD_LT(n) (1 << (16 + (n))) /* [31:16] low-temp int  */
#define TSADC_HLT_INT_PD_HT_ALL TSADC_CHAN_MASK
#define TSADC_HLT_INT_PD_LT_ALL (TSADC_CHAN_MASK << 16)

/* TSADC_EOC_HSHUT_PD (0x0028) — write 1 to clear *************************/

#define TSADC_EOC_HSHUT_PD_SHUT(n) (1 << (n)) /* [15:0] shut threshold hit */
#define TSADC_EOC_HSHUT_PD_SHUT_ALL TSADC_CHAN_MASK
#define TSADC_EOC_HSHUT_PD_USR_EOC  (1 << 16) /* User-mode conversion done */
#define TSADC_EOC_HSHUT_PD_ROUND    (1 << 17) /* Auto-mode round complete  */

/* TSADC_DATA / TSADC_COMP_* payload **************************************/

#define TSADC_DATA_MASK 0x03ff /* [9:0] 10-bit conversion result */

/* Debounce counters, TSADC_HIGH_{INT,TSHUT}_DEBOUNCE [7:0] ***************/

#define TSADC_DEBOUNCE_MASK 0x00ff

/* Write-enable helpers.
 *
 *   RK3576_TSADC_WE(bits)     - set the given bits, leave the rest alone
 *   RK3576_TSADC_WE_CLR(bits) - clear the given bits
 *   RK3576_TSADC_WE_VAL(v, m) - write value v under field mask m
 *
 * Only bits whose mask bit (upper half) is 1 are modified, so a field that
 * must be programmed to zero has to go through RK3576_TSADC_WE_VAL() or
 * RK3576_TSADC_WE_CLR() — RK3576_TSADC_WE() alone can never clear a bit.
 */

#define RK3576_TSADC_WE(bits) \
  (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define RK3576_TSADC_WE_CLR(bits) ((uint32_t)(bits) << 16)
#define RK3576_TSADC_WE_VAL(v, m) \
  (((uint32_t)(m) << 16) | ((uint32_t)(v) & (uint32_t)(m)))

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_TSADC_H */
