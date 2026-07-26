/****************************************************************************
 * chips/rk3576/hardware/rk3576_ir.h
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
 * Infrared remote-control receiver register definitions.
 *
 * The RK3576 has no dedicated IR block.  Infrared reception re-uses a
 * Rockchip PWM v4 channel running in capture mode together with the
 * "power key match" hardware, which contains a fixed-function NEC protocol
 * decoder (leader detection, bit-cell width discrimination, 32-bit frame
 * assembly).  The vendor device tree describes it as:
 *
 *   pwm@27330000 { compatible = "rockchip,remotectl-pwm-v4";
 *                  reg = <0x0 0x27330000 0x0 0x1000>;
 *                  interrupts = <0x0 0x64 0x4>; };
 *
 * i.e. PWM0 channel 0 (RK3576_PWM0_ADDR) and GIC SPI 100, whose INTID is
 * 100 + 32 = 132 == RK3576_IRQ_PWM0_2CH_0.
 *
 * This header only adds the register offsets that the waveform-generator
 * driver (hardware/rk3576_pwm.h) does not need.  Everything shared — base
 * addresses, the channel stride, PWM_ENABLE / PWM_CLK_CTRL / PWM_CTRL and
 * the HIWORD write helpers — is taken from that header.
 *
 * Source: Rockchip RK3576 TRM Part1 V1.2-20240624, chapter 34 "PWM",
 * sections 34.4.2 (register summary) and 34.6.2 (power-key capture flow).
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_IR_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_IR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdint.h>

#include "rk3576_memorymap.h"

#include "rk3576_pwm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller / channel wired to the IR receiver on this SoC.  The PWM
 * register window is selected with RK3576_PWM0_ADDR + channel * stride.
 */

#define RK3576_IR_PWM_CTRL_ID RK3576_PWM0
#define RK3576_IR_PWM_CHANNEL 0

/* GIC INTID of the IR channel.  Identical to RK3576_IRQ_PWM0_2CH_0 in
 * chips/rk3576/include/irq.h (GIC INTID = device-tree SPI 100 + 32); it is
 * repeated here only as documentation of the derivation.
 */

#define RK3576_IR_GIC_SPI   100
#define RK3576_IR_GIC_INTID (RK3576_IR_GIC_SPI + 32)

/* Register offsets not covered by hardware/rk3576_pwm.h *******************/

#define RK3576_PWM_FILTER_CTRL       0x0020 /* Input filter (HIWORD)     */
#define RK3576_PWM_HPC               0x002c /* Captured high cycles (RO) */
#define RK3576_PWM_LPC               0x0030 /* Captured low cycles  (RO) */
#define RK3576_PWM_INTSTS            0x0070 /* Interrupt status (W1C)    */
#define RK3576_PWM_INT_EN            0x0074 /* Interrupt enable (HIWORD) */
#define RK3576_PWM_INT_MASK          0x0078 /* Interrupt mask   (HIWORD) */

#define RK3576_PWM_PWRMATCH_ARBITER  0x0100 /* Power-key arbiter         */
#define RK3576_PWM_PWRMATCH_CTRL     0x0104 /* Power-key ctrl (HIWORD)   */
#define RK3576_PWM_PWRMATCH_LPRE     0x0108 /* Leader low  min/max       */
#define RK3576_PWM_PWRMATCH_HPRE     0x010c /* Leader high min/max       */
#define RK3576_PWM_PWRMATCH_LD       0x0110 /* Bit-cell low  min/max     */
#define RK3576_PWM_PWRMATCH_HD_ZERO  0x0114 /* Logic-0 high min/max      */
#define RK3576_PWM_PWRMATCH_HD_ONE   0x0118 /* Logic-1 high min/max      */
#define RK3576_PWM_PWRMATCH_VALUE(n) (0x011c + ((n) << 2)) /* n = 0..15  */
#define RK3576_PWM_PWRCAPTURE_VALUE  0x015c /* Decoded frame (RO)        */

#define RK3576_PWM_PWRMATCH_NVALUES  16 /* PWRMATCH_VALUE0..15       */

/* PWM_FILTER_CTRL (0x20) *************************************************/

#define PWM_FILTER_ENABLE       (1 << 0) /* Glitch filter enable   */
#define PWM_FILTER_NUMBER_MASK  0x03f0   /* [9:4] filter windows   */
#define PWM_FILTER_NUMBER_SHIFT 4

/* PWM_INTSTS (0x70) / PWM_INT_EN (0x74) / PWM_INT_MASK (0x78)
 *
 * The three registers share the same bit layout.  PWM_INTSTS is
 * write-1-to-clear and takes a plain 32-bit write (no HIWORD mask);
 * PWM_INT_EN and PWM_INT_MASK are HIWORD write-masked.
 */

#define PWM_INT_CAP_LPC          (1 << 0) /* Capture low  cycles    */
#define PWM_INT_CAP_HPC          (1 << 1) /* Capture high cycles    */
#define PWM_INT_ONESHOT_END      (1 << 2) /* One-shot waveform end  */
#define PWM_INT_RELOAD           (1 << 3) /* Reload                 */
#define PWM_INT_FREQ             (1 << 4) /* Frequency meter        */
#define PWM_INT_PWR              (1 << 5) /* Power-key match/capture*/
#define PWM_INT_IR_TRANS_END     (1 << 6) /* IR transmission end    */
#define PWM_INT_WAVE_MAX         (1 << 7) /* Waveform max address   */
#define PWM_INT_WAVE_MIDDLE      (1 << 8) /* Waveform mid address   */
#define PWM_INT_BIPHASIC_COUNTER (1 << 9) /* Biphasic counter       */
#define PWM_INT_ALL              0x03ff   /* All status bits        */

/* PWM_PWRMATCH_ARBITER (0x100) ******************************************/

#define PWM_PWRMATCH_GRANT_MASK   0x000000ff /* [7:0]  per channel   */
#define PWM_PWRMATCH_GRANT_SHIFT  0
#define PWM_PWRMATCH_RDLOCK_MASK  0x00ff0000 /* [23:16] per channel  */
#define PWM_PWRMATCH_RDLOCK_SHIFT 16

/* PWM_PWRMATCH_CTRL (0x104), HIWORD write-masked *************************/

#define PWM_PWRMATCH_ENABLE         (1 << 0) /* Power-key mode enable  */
#define PWM_PWRMATCH_POLARITY_NEG   (1 << 1) /* 0 = positive input     */
#define PWM_PWRMATCH_CAPTURE_DIRECT (1 << 2) /* 1 = capture directly   */
#define PWM_PWRMATCH_INT_NO_MATCH   (1 << 3) /* 1 = IRQ without match  */

/* min/max threshold registers (LPRE/HPRE/LD/HD_ZERO/HD_ONE) **************/

#define PWM_PWRMATCH_CNT_MIN_SHIFT 0
#define PWM_PWRMATCH_CNT_MAX_SHIFT 16

#define PWM_PWRMATCH_CNT(min, max)                            \
  ((((uint32_t)(min)&0xffff) << PWM_PWRMATCH_CNT_MIN_SHIFT) | \
   (((uint32_t)(max)&0xffff) << PWM_PWRMATCH_CNT_MAX_SHIFT))

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_IR_H */
