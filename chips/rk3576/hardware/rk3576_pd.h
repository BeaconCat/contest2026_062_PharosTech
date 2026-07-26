/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_pd.h
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
 * RK3576 PMU power-domain register definitions.
 *
 * The PMU register file lives at 0x27380000 (the TRM spells this as
 * "operational base 0x27360000 + offset 0x20xxx"); the device tree node is
 * power-management@27380000 with a 0x800 byte window.  Only the registers
 * used by the power-domain driver are described here.
 *
 * Every *_CON / *_SFTCON register is HIWORD write-masked: the upper 16 bits
 * are a per-bit write enable for the lower 16 bits.  Status registers are
 * plain read-only 32-bit words.
 *
 * Bit numbering conventions used throughout:
 *
 *   - PWR_GATE_CON0 / PWR_GATE_SFTCON0 / PWR_GATE_CON1 / PWR_GATE_SFTCON1
 *     hold the per-domain "power down" enables.  1 = powered down.
 *   - PWR_GATE_STS is the flattened status of both: SFTCON0 bit n maps to
 *     PWR_GATE_STS bit n, SFTCON1 bit n maps to PWR_GATE_STS bit (16 + n).
 *     1 = domain is powered down.
 *   - BIU_IDLE_SFTCON0/1 hold the per-BIU idle requests.  BIU_IDLE_ACK_STS
 *     and BIU_IDLE_STS are flattened the same way: CON0 bit n -> bit n,
 *     CON1 bit n -> bit (16 + n).
 *
 * Source: RK3576 TRM Part1 V1.2, chapter 6 (PMU), sections 6.3.2, 6.5.5,
 * 6.5.6 and the PMU register descriptions.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PD_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register offsets from RK3576_PMU_ADDR *********************************/

#define RK3576_PMU_BIU_IDLE_CON0        0x0100 /* BIU idle req (hardware) */
#define RK3576_PMU_BIU_IDLE_CON1        0x0104
#define RK3576_PMU_BIU_IDLE_SFTCON0     0x0110 /* BIU idle req (software) */
#define RK3576_PMU_BIU_IDLE_SFTCON1     0x0114
#define RK3576_PMU_BIU_IDLE_ACK_STS     0x0120 /* BIU idle acknowledge    */
#define RK3576_PMU_BIU_IDLE_STS         0x0128 /* BIU idle state          */
#define RK3576_PMU_PWR_GATE_CON0        0x0200 /* PD power down (hardware) */
#define RK3576_PMU_PWR_GATE_CON1        0x0204
#define RK3576_PMU_PWR_GATE_SFTCON0     0x0210 /* PD power down (software) */
#define RK3576_PMU_PWR_GATE_SFTCON1     0x0214
#define RK3576_PMU_VOL_GATE_CON0        0x0220 /* VD power off (hardware)  */
#define RK3576_PMU_VOL_GATE_CON1        0x0224
#define RK3576_PMU_PWR_GATE_STS         0x0230 /* PD power state           */
#define RK3576_PMU_PWR_GATE_POWER_STS   0x0238
#define RK3576_PMU_PWR_MEM_STS0         0x0250 /* PD memory power state    */
#define RK3576_PMU_PWR_MEM_STS1         0x0254
#define RK3576_PMU_MEM_PWR_GATE_SFTCON0 0x0300 /* PD memory retention (sw) */
#define RK3576_PMU_MEM_PWR_GATE_SFTCON1 0x0304
#define RK3576_PMU_BISR_PWR_REPAIR_ST0  0x0570 /* Memory BISR repair state */
#define RK3576_PMU_BISR_PWR_REPAIR_ST1  0x0574

/* Absolute register addresses *********************************************/

#define RK3576_PMU_REG(o) (RK3576_PMU_ADDR + (o))

/* HIWORD write mask helper: write bits described by "mask" with "value" and
 * leave every other bit of the register untouched.
 */

#define RK3576_PMU_HIWORD(mask, value) (((mask) << 16) | ((value) & (mask)))

/* PMU_PWR_GATE_CON0 / PMU_PWR_GATE_SFTCON0 bit assignment ******************
 * (1 = power the domain down)
 */

#define RK3576_PMU_PWR0_NPU         (1 << 0)  /* VD_NPU              */
#define RK3576_PMU_PWR0_BUS         (1 << 1)  /* PD_BUS   (always on) */
#define RK3576_PMU_PWR0_SECURE      (1 << 2)  /* PD_SECURE(always on) */
#define RK3576_PMU_PWR0_CENTER      (1 << 3)  /* PD_CENTER           */
#define RK3576_PMU_PWR0_DDR         (1 << 4)  /* VD_DDR              */
#define RK3576_PMU_PWR0_CCI         (1 << 5)  /* PD_CCI   (always on) */
#define RK3576_PMU_PWR0_NVM         (1 << 6)  /* PD_NVM              */
#define RK3576_PMU_PWR0_SDGMAC      (1 << 7)  /* PD_SDGMAC           */
#define RK3576_PMU_PWR0_AUDIO       (1 << 8)  /* PD_AUDIO            */
#define RK3576_PMU_PWR0_PHP         (1 << 9)  /* PD_PHP              */
#define RK3576_PMU_PWR0_SUBPHP      (1 << 10) /* PD_SUBPHP           */
#define RK3576_PMU_PWR0_VOP         (1 << 11) /* PD_VOP              */
#define RK3576_PMU_PWR0_VOP_ESMART  (1 << 12) /* PD_VOP_ESMART       */
#define RK3576_PMU_PWR0_VOP_CLUSTER (1 << 13) /* PD_VOP_CLUSTER      */
#define RK3576_PMU_PWR0_VO1         (1 << 14) /* PD_VO1              */
#define RK3576_PMU_PWR0_VO0         (1 << 15) /* PD_VO0              */

/* PMU_PWR_GATE_CON1 / PMU_PWR_GATE_SFTCON1 bit assignment */

#define RK3576_PMU_PWR1_USB    (1 << 0) /* PD_USB              */
#define RK3576_PMU_PWR1_VI     (1 << 1) /* PD_VI               */
#define RK3576_PMU_PWR1_VEPU0  (1 << 2) /* PD_VEPU0            */
#define RK3576_PMU_PWR1_VEPU1  (1 << 3) /* PD_VEPU1            */
#define RK3576_PMU_PWR1_VDEC   (1 << 4) /* PD_VDEC             */
#define RK3576_PMU_PWR1_VPU    (1 << 5) /* PD_VPU              */
#define RK3576_PMU_PWR1_NPUTOP (1 << 6) /* PD_NPUTOP           */
#define RK3576_PMU_PWR1_NPU0   (1 << 7) /* PD_NPU0             */
#define RK3576_PMU_PWR1_NPU1   (1 << 8) /* PD_NPU1             */
#define RK3576_PMU_PWR1_GPU    (1 << 9) /* VD_GPU              */

/* PMU_PWR_GATE_STS: SFTCON1 bits are shifted up by this amount */

#define RK3576_PMU_PWR_STS_CON1_SHIFT 16

/* PMU_VOL_GATE_CON0 / CON1 bit assignment (voltage domain off request) */

#define RK3576_PMU_VOL0_NPU (1 << 0) /* VD_NPU              */
#define RK3576_PMU_VOL0_DDR (1 << 4) /* VD_DDR              */
#define RK3576_PMU_VOL0_CCI (1 << 5) /* VD_LITCORE          */
#define RK3576_PMU_VOL1_GPU (1 << 9) /* VD_GPU              */

/* PMU_BIU_IDLE_CON0 / PMU_BIU_IDLE_SFTCON0 bit assignment ******************
 * (1 = request the BIU to go idle)
 */

#define RK3576_PMU_IDLE0_GPU        (1 << 0)  /* BIU_GPU             */
#define RK3576_PMU_IDLE0_NPU0       (1 << 1)  /* BIU_NPU0            */
#define RK3576_PMU_IDLE0_NPU1       (1 << 2)  /* BIU_NPU1            */
#define RK3576_PMU_IDLE0_NPUTOP     (1 << 3)  /* BIU_NPUTOP          */
#define RK3576_PMU_IDLE0_NPUSYS     (1 << 4)  /* BIU_NPUSYS          */
#define RK3576_PMU_IDLE0_VPU        (1 << 5)  /* BIU_VPU             */
#define RK3576_PMU_IDLE0_VDEC       (1 << 6)  /* BIU_VDEC            */
#define RK3576_PMU_IDLE0_VEPU0      (1 << 7)  /* BIU_VEPU0           */
#define RK3576_PMU_IDLE0_VEPU1      (1 << 8)  /* BIU_VEPU1           */
#define RK3576_PMU_IDLE0_VI         (1 << 9)  /* BIU_VI              */
#define RK3576_PMU_IDLE0_USB        (1 << 10) /* BIU_USB             */
#define RK3576_PMU_IDLE0_VO0        (1 << 11) /* BIU_VO0             */
#define RK3576_PMU_IDLE0_VO1        (1 << 12) /* BIU_VO1             */
#define RK3576_PMU_IDLE0_VOP        (1 << 13) /* BIU_VOP             */
#define RK3576_PMU_IDLE0_VOP_DDRSCH (1 << 14) /* BIU_VOP_DDRSCH      */
#define RK3576_PMU_IDLE0_PHP        (1 << 15) /* BIU_PHP             */

/* PMU_BIU_IDLE_CON1 / PMU_BIU_IDLE_SFTCON1 bit assignment */

#define RK3576_PMU_IDLE1_AUDIO          (1 << 0)  /* BIU_AUDIO           */
#define RK3576_PMU_IDLE1_GMAC           (1 << 1)  /* BIU_SDGMAC          */
#define RK3576_PMU_IDLE1_NVM            (1 << 2)  /* BIU_NVM             */
#define RK3576_PMU_IDLE1_CENTER_DDRSCH  (1 << 3)  /* BIU_CENTER_DDRSCH   */
#define RK3576_PMU_IDLE1_CENTER_MAIN    (1 << 4)  /* BIU_CENTER_MAIN     */
#define RK3576_PMU_IDLE1_DDR            (1 << 5)  /* BIU_DDR             */
#define RK3576_PMU_IDLE1_DDRSCH0        (1 << 6)  /* BIU_DDRSCH0         */
#define RK3576_PMU_IDLE1_DDRSCH1        (1 << 7)  /* BIU_DDRSCH1         */
#define RK3576_PMU_IDLE1_BUS            (1 << 8)  /* BIU_BUS             */
#define RK3576_PMU_IDLE1_SECURE         (1 << 9)  /* BIU_SECURE          */
#define RK3576_PMU_IDLE1_TOP            (1 << 10) /* BIU_TOP             */
#define RK3576_PMU_IDLE1_VO0VOP_CHANNEL (1 << 11) /* BIU_VO0VOP_CHANNEL  */
#define RK3576_PMU_IDLE1_CCI            (1 << 12) /* BIU_CCI             */
#define RK3576_PMU_IDLE1_CCI_DDRSCH     (1 << 13) /* BIU_CCI_DDRSCH      */

/* PMU_BIU_IDLE_ACK_STS / PMU_BIU_IDLE_STS: CON1 bits are shifted up by
 * this amount.
 */

#define RK3576_PMU_IDLE_STS_CON1_SHIFT 16

/* PMU_MEM_PWR_GATE_SFTCON0 bit assignment (1 = power the SRAM down) */

#define RK3576_PMU_MEM0_CENTER      (1 << 3)  /* PD_CENTER memory    */
#define RK3576_PMU_MEM0_NVM         (1 << 6)  /* PD_NVM memory       */
#define RK3576_PMU_MEM0_SDGMAC      (1 << 7)  /* PD_SDGMAC memory    */
#define RK3576_PMU_MEM0_AUDIO       (1 << 8)  /* PD_AUDIO memory     */
#define RK3576_PMU_MEM0_PHP         (1 << 9)  /* PD_PHP memory       */
#define RK3576_PMU_MEM0_SUBPHP      (1 << 10) /* PD_SUBPHP memory    */
#define RK3576_PMU_MEM0_VOP         (1 << 11) /* PD_VOP memory       */
#define RK3576_PMU_MEM0_VOP_ESMART  (1 << 12) /* PD_VOP_ESMART mem   */
#define RK3576_PMU_MEM0_VOP_CLUSTER (1 << 13) /* PD_VOP_CLUSTER mem  */
#define RK3576_PMU_MEM0_VO1         (1 << 14) /* PD_VO1 memory       */
#define RK3576_PMU_MEM0_VO0         (1 << 15) /* PD_VO0 memory       */

/* PMU_MEM_PWR_GATE_SFTCON1 bit assignment */

#define RK3576_PMU_MEM1_USB    (1 << 0) /* PD_USB memory       */
#define RK3576_PMU_MEM1_VI     (1 << 1) /* PD_VI memory        */
#define RK3576_PMU_MEM1_VEPU0  (1 << 2) /* PD_VEPU0 memory     */
#define RK3576_PMU_MEM1_VEPU1  (1 << 3) /* PD_VEPU1 memory     */
#define RK3576_PMU_MEM1_VDEC   (1 << 4) /* PD_VDEC memory      */
#define RK3576_PMU_MEM1_VPU    (1 << 5) /* PD_VPU memory       */
#define RK3576_PMU_MEM1_NPUTOP (1 << 6) /* PD_NPUTOP memory    */
#define RK3576_PMU_MEM1_NPU0   (1 << 7) /* PD_NPU0 memory      */
#define RK3576_PMU_MEM1_NPU1   (1 << 8) /* PD_NPU1 memory      */

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PD_H */
