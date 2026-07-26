/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_mali.h
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
 * ARM Mali-G52 MC3 (Bifrost, "Gondul" r1p0) register definitions for the
 * RK3576.  The GPU sits at 0x27800000 with a 128KB register window and
 * three GIC interrupts:
 *
 *   GPU_JOB  - GIC INTID 379 (device tree SPI 347)
 *   GPU_MMU  - GIC INTID 380 (device tree SPI 348)
 *   GPU      - GIC INTID 381 (device tree SPI 349)
 *
 * The RK3576 TRM chapter 4 ("GPU") does not document the register file; it
 * defers to the ARM technical reference manual.  All offsets below come
 * from the publicly documented Mali programming model as reverse
 * engineered and published by the Panfrost project (Mesa,
 * drivers/gpu/drm/panfrost/panfrost_regs.h and the Mesa GenXML job
 * descriptor definitions).  That documentation describes the hardware, not
 * ARM source code, so it may be re-implemented freely.
 *
 * The register window is split into three blocks:
 *
 *   0x0000  GPU_CONTROL  - identity, feature registers, power management
 *   0x1000  JOB_CONTROL  - job interrupt block and the three job slots
 *   0x2000  MMU_CONTROL  - MMU interrupt block and the address spaces
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MALI_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MALI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Geometry of the register window ******************************************/

#define RK3576_MALI_REGS_SIZE 0x00020000 /* 128KB, per device tree "reg" */

#define RK3576_MALI_NSLOTS    3 /* JS0, JS1, JS2                        */
#define RK3576_MALI_NAS       8 /* AS0..AS7 (see AS_PRESENT at runtime) */

/* GPU_CONTROL block ********************************************************/

#define RK3576_MALI_GPU_ID              0x0000 /* GPU identity          */
#define RK3576_MALI_L2_FEATURES         0x0004 /* L2 cache geometry     */
#define RK3576_MALI_TILER_FEATURES      0x000c /* Tiler geometry        */
#define RK3576_MALI_MEM_FEATURES        0x0010 /* Memory system         */
#define RK3576_MALI_MMU_FEATURES        0x0014 /* VA/PA width, page bits*/
#define RK3576_MALI_AS_PRESENT          0x0018 /* Address space bitmask */
#define RK3576_MALI_JS_PRESENT          0x001c /* Job slot bitmask      */
#define RK3576_MALI_GPU_IRQ_RAWSTAT     0x0020 /* Raw interrupt state   */
#define RK3576_MALI_GPU_IRQ_CLEAR       0x0024 /* Write-1-to-clear      */
#define RK3576_MALI_GPU_IRQ_MASK        0x0028 /* Interrupt enable      */
#define RK3576_MALI_GPU_IRQ_STATUS      0x002c /* RAWSTAT & MASK        */
#define RK3576_MALI_GPU_COMMAND         0x0030 /* Global command        */
#define RK3576_MALI_GPU_STATUS          0x0034 /* Global status         */
#define RK3576_MALI_GPU_FAULTSTATUS     0x003c /* Last global fault     */
#define RK3576_MALI_GPU_FAULTADDRESS_LO 0x0040 /* Faulting address low  */
#define RK3576_MALI_GPU_FAULTADDRESS_HI 0x0044 /* Faulting address high */
#define RK3576_MALI_L2_CONFIG           0x0048 /* L2 tuning (Bifrost)   */
#define RK3576_MALI_PWR_KEY             0x0050 /* Unlock PWR_OVERRIDEn  */
#define RK3576_MALI_PWR_OVERRIDE0       0x0054 /* Power quirks          */
#define RK3576_MALI_PWR_OVERRIDE1       0x0058 /* Power quirks          */
#define RK3576_MALI_CYCLE_COUNT_LO      0x0090 /* Free running counter  */
#define RK3576_MALI_CYCLE_COUNT_HI      0x0094
#define RK3576_MALI_TIMESTAMP_LO        0x0098 /* Free running timer    */
#define RK3576_MALI_TIMESTAMP_HI        0x009c
#define RK3576_MALI_THREAD_MAX_THREADS  0x00a0 /* Shader core limits    */
#define RK3576_MALI_THREAD_FEATURES     0x00ac
#define RK3576_MALI_TEXTURE_FEATURES(n) (0x00b0 + ((n)*4))
#define RK3576_MALI_JS_FEATURES(n)      (0x00c0 + ((n)*4))

/* Core availability / readiness / power control.  Every one of these is a
 * 64-bit core bitmask exposed as a LO/HI register pair.
 */

#define RK3576_MALI_SHADER_PRESENT_LO  0x0100
#define RK3576_MALI_SHADER_PRESENT_HI  0x0104
#define RK3576_MALI_TILER_PRESENT_LO   0x0110
#define RK3576_MALI_TILER_PRESENT_HI   0x0114
#define RK3576_MALI_L2_PRESENT_LO      0x0120
#define RK3576_MALI_L2_PRESENT_HI      0x0124
#define RK3576_MALI_SHADER_READY_LO    0x0140
#define RK3576_MALI_SHADER_READY_HI    0x0144
#define RK3576_MALI_TILER_READY_LO     0x0150
#define RK3576_MALI_TILER_READY_HI     0x0154
#define RK3576_MALI_L2_READY_LO        0x0160
#define RK3576_MALI_L2_READY_HI        0x0164
#define RK3576_MALI_SHADER_PWRON_LO    0x0180
#define RK3576_MALI_SHADER_PWRON_HI    0x0184
#define RK3576_MALI_TILER_PWRON_LO     0x0190
#define RK3576_MALI_TILER_PWRON_HI     0x0194
#define RK3576_MALI_L2_PWRON_LO        0x01a0
#define RK3576_MALI_L2_PWRON_HI        0x01a4
#define RK3576_MALI_SHADER_PWROFF_LO   0x01c0
#define RK3576_MALI_SHADER_PWROFF_HI   0x01c4
#define RK3576_MALI_TILER_PWROFF_LO    0x01d0
#define RK3576_MALI_TILER_PWROFF_HI    0x01d4
#define RK3576_MALI_L2_PWROFF_LO       0x01e0
#define RK3576_MALI_L2_PWROFF_HI       0x01e4
#define RK3576_MALI_SHADER_PWRTRANS_LO 0x0200
#define RK3576_MALI_TILER_PWRTRANS_LO  0x0210
#define RK3576_MALI_L2_PWRTRANS_LO     0x0220
#define RK3576_MALI_COHERENCY_FEATURES 0x0300
#define RK3576_MALI_COHERENCY_ENABLE   0x0304

/* Bifrost adds "core stacks": a shader core can only be powered up once
 * the stack it belongs to is up.
 */

#define RK3576_MALI_STACK_PRESENT_LO  0x0e00
#define RK3576_MALI_STACK_PRESENT_HI  0x0e04
#define RK3576_MALI_STACK_READY_LO    0x0e40
#define RK3576_MALI_STACK_READY_HI    0x0e44
#define RK3576_MALI_STACK_PWRON_LO    0x0e80
#define RK3576_MALI_STACK_PWRON_HI    0x0e84
#define RK3576_MALI_STACK_PWROFF_LO   0x0ec0
#define RK3576_MALI_STACK_PWROFF_HI   0x0ec4
#define RK3576_MALI_STACK_PWRTRANS_LO 0x0f00

/* GPU_ID field extraction.  On Bifrost and later the upper half word is
 * the "arch/product" identifier and the lower half word the revision:
 *
 *   [31:28] arch major   [27:24] arch minor   [23:20] arch revision
 *   [19:16] product major
 *   [15:12] version major [11:8] version minor [7:0] version status
 */

#define RK3576_MALI_GPU_ID_PRODUCT(id)    (((id) >> 16) & 0xffff)
#define RK3576_MALI_GPU_ID_VER_MAJOR(id)  (((id) >> 12) & 0xf)
#define RK3576_MALI_GPU_ID_VER_MINOR(id)  (((id) >> 8) & 0xf)
#define RK3576_MALI_GPU_ID_VER_STATUS(id) ((id)&0xff)

/* Product identifier of the Mali-G52 (Bifrost arch v7, product major 2).
 * The RK3576 integrates the MC3 configuration, so SHADER_PRESENT reads
 * back three cores.
 */

#define RK3576_MALI_PRODUCT_G52 0x7002

/* GPU_COMMAND opcodes */

#define RK3576_MALI_CMD_NOP                0
#define RK3576_MALI_CMD_SOFT_RESET         1
#define RK3576_MALI_CMD_HARD_RESET         2
#define RK3576_MALI_CMD_PRFCNT_CLEAR       3
#define RK3576_MALI_CMD_PRFCNT_SAMPLE      4
#define RK3576_MALI_CMD_CYCLE_COUNT_START  5
#define RK3576_MALI_CMD_CYCLE_COUNT_STOP   6
#define RK3576_MALI_CMD_CLEAN_CACHES       7
#define RK3576_MALI_CMD_CLEAN_INV_CACHES   8
#define RK3576_MALI_CMD_SET_PROTECTED_MODE 9

/* GPU_IRQ_RAWSTAT / _CLEAR / _MASK / _STATUS bits */

#define RK3576_MALI_GPU_IRQ_FAULT             (1 << 0)
#define RK3576_MALI_GPU_IRQ_PROTM_FAULT       (1 << 1)
#define RK3576_MALI_GPU_IRQ_MULTIPLE_FAULT    (1 << 7)
#define RK3576_MALI_GPU_IRQ_RESET_COMPLETED   (1 << 8)
#define RK3576_MALI_GPU_IRQ_POWER_CHANGED     (1 << 9)
#define RK3576_MALI_GPU_IRQ_POWER_CHANGED_ALL (1 << 10)
#define RK3576_MALI_GPU_IRQ_PRFCNT_SAMPLE     (1 << 16)
#define RK3576_MALI_GPU_IRQ_CLEAN_CACHES      (1 << 17)

#define RK3576_MALI_GPU_IRQ_ALL               0xffffffff

/* The power-changed interrupts fire on every core transition and are of no
 * use to a polled power-up sequence, so they are kept masked.
 */

#define RK3576_MALI_GPU_IRQ_ENABLED                              \
  (RK3576_MALI_GPU_IRQ_FAULT | RK3576_MALI_GPU_IRQ_PROTM_FAULT | \
   RK3576_MALI_GPU_IRQ_MULTIPLE_FAULT | RK3576_MALI_GPU_IRQ_RESET_COMPLETED)

/* GPU_STATUS bits */

#define RK3576_MALI_GPU_STATUS_ACTIVE        (1 << 0)
#define RK3576_MALI_GPU_STATUS_PRFCNT_ACTIVE (1 << 2)
#define RK3576_MALI_GPU_STATUS_PROTM_ACTIVE  (1 << 7)

/* PWR_KEY unlock value guarding PWR_OVERRIDE0/1 */

#define RK3576_MALI_PWR_KEY_UNLOCK 0x2968a819

/* JOB_CONTROL block ********************************************************/

#define RK3576_MALI_JOB_IRQ_RAWSTAT  0x1000 /* Raw job interrupt state   */
#define RK3576_MALI_JOB_IRQ_CLEAR    0x1004 /* Write-1-to-clear          */
#define RK3576_MALI_JOB_IRQ_MASK     0x1008 /* Interrupt enable          */
#define RK3576_MALI_JOB_IRQ_STATUS   0x100c /* RAWSTAT & MASK            */
#define RK3576_MALI_JOB_IRQ_JS_STATE 0x1010 /* Per-slot busy state      */
#define RK3576_MALI_JOB_IRQ_THROTTLE 0x1014 /* Interrupt coalescing     */

/* JOB_IRQ_* bit n = "job done" on slot n, bit 16+n = "job failed". */

#define RK3576_MALI_JOB_IRQ_DONE(slot)   (1u << (slot))
#define RK3576_MALI_JOB_IRQ_FAILED(slot) (1u << (16 + (slot)))
#define RK3576_MALI_JOB_IRQ_ALL          0xffffffff

/* Job slot register file.  Each slot occupies 0x80 bytes. */

#define RK3576_MALI_JS_BASE(slot)          (0x1800 + ((slot)*0x80))

#define RK3576_MALI_JS_HEAD_LO(s)          (RK3576_MALI_JS_BASE(s) + 0x00)
#define RK3576_MALI_JS_HEAD_HI(s)          (RK3576_MALI_JS_BASE(s) + 0x04)
#define RK3576_MALI_JS_TAIL_LO(s)          (RK3576_MALI_JS_BASE(s) + 0x08)
#define RK3576_MALI_JS_TAIL_HI(s)          (RK3576_MALI_JS_BASE(s) + 0x0c)
#define RK3576_MALI_JS_AFFINITY_LO(s)      (RK3576_MALI_JS_BASE(s) + 0x10)
#define RK3576_MALI_JS_AFFINITY_HI(s)      (RK3576_MALI_JS_BASE(s) + 0x14)
#define RK3576_MALI_JS_CONFIG(s)           (RK3576_MALI_JS_BASE(s) + 0x18)
#define RK3576_MALI_JS_XAFFINITY(s)        (RK3576_MALI_JS_BASE(s) + 0x1c)
#define RK3576_MALI_JS_COMMAND(s)          (RK3576_MALI_JS_BASE(s) + 0x20)
#define RK3576_MALI_JS_STATUS(s)           (RK3576_MALI_JS_BASE(s) + 0x24)
#define RK3576_MALI_JS_HEAD_NEXT_LO(s)     (RK3576_MALI_JS_BASE(s) + 0x40)
#define RK3576_MALI_JS_HEAD_NEXT_HI(s)     (RK3576_MALI_JS_BASE(s) + 0x44)
#define RK3576_MALI_JS_AFFINITY_NEXT_LO(s) (RK3576_MALI_JS_BASE(s) + 0x48)
#define RK3576_MALI_JS_AFFINITY_NEXT_HI(s) (RK3576_MALI_JS_BASE(s) + 0x4c)
#define RK3576_MALI_JS_CONFIG_NEXT(s)      (RK3576_MALI_JS_BASE(s) + 0x50)
#define RK3576_MALI_JS_XAFFINITY_NEXT(s)   (RK3576_MALI_JS_BASE(s) + 0x54)
#define RK3576_MALI_JS_COMMAND_NEXT(s)     (RK3576_MALI_JS_BASE(s) + 0x60)
#define RK3576_MALI_JS_FLUSH_ID_NEXT(s)    (RK3576_MALI_JS_BASE(s) + 0x70)

/* JS_COMMAND / JS_COMMAND_NEXT opcodes */

#define RK3576_MALI_JS_CMD_NOP         0
#define RK3576_MALI_JS_CMD_START       1
#define RK3576_MALI_JS_CMD_SOFT_STOP   2
#define RK3576_MALI_JS_CMD_HARD_STOP   3
#define RK3576_MALI_JS_CMD_SOFT_STOP_0 4
#define RK3576_MALI_JS_CMD_HARD_STOP_0 5
#define RK3576_MALI_JS_CMD_SOFT_STOP_1 6
#define RK3576_MALI_JS_CMD_HARD_STOP_1 7

/* JS_CONFIG / JS_CONFIG_NEXT fields */

#define RK3576_MALI_JS_CONFIG_AS_MASK             0x0f
#define RK3576_MALI_JS_CONFIG_AS(n)               ((n)&0x0f)
#define RK3576_MALI_JS_CONFIG_START_FLUSH_CLEAN   (1 << 8)
#define RK3576_MALI_JS_CONFIG_START_FLUSH_INV     (3 << 8)
#define RK3576_MALI_JS_CONFIG_START_MMU           (1 << 10)
#define RK3576_MALI_JS_CONFIG_JOB_CHAIN_FLAG      (1 << 11)
#define RK3576_MALI_JS_CONFIG_END_FLUSH_CLEAN     (1 << 12)
#define RK3576_MALI_JS_CONFIG_END_FLUSH_INV       (3 << 12)
#define RK3576_MALI_JS_CONFIG_ENABLE_FLUSH_REDUCE (1 << 14)
#define RK3576_MALI_JS_CONFIG_DISABLE_DESC_WR_BK  (1 << 15)
#define RK3576_MALI_JS_CONFIG_THREAD_PRI(n)       ((n) << 16)

/* JS_STATUS values.  Anything >= _FAULT_START is a failure. */

#define RK3576_MALI_JS_STATUS_DONE                0x01
#define RK3576_MALI_JS_STATUS_INTERRUPTED         0x02
#define RK3576_MALI_JS_STATUS_STOPPED             0x03
#define RK3576_MALI_JS_STATUS_TERMINATED          0x04
#define RK3576_MALI_JS_STATUS_ACTIVE              0x08
#define RK3576_MALI_JS_STATUS_FAULT_START         0x40
#define RK3576_MALI_JS_STATUS_CONFIG_FAULT        0x40
#define RK3576_MALI_JS_STATUS_POWER_FAULT         0x41
#define RK3576_MALI_JS_STATUS_READ_FAULT          0x42
#define RK3576_MALI_JS_STATUS_WRITE_FAULT         0x43
#define RK3576_MALI_JS_STATUS_AFFINITY_FAULT      0x44
#define RK3576_MALI_JS_STATUS_BUS_FAULT           0x48
#define RK3576_MALI_JS_STATUS_INSTR_INVALID_PC    0x50
#define RK3576_MALI_JS_STATUS_INSTR_INVALID_ENC   0x51
#define RK3576_MALI_JS_STATUS_INSTR_BARRIER_FAULT 0x55
#define RK3576_MALI_JS_STATUS_DATA_INVALID_FAULT  0x58
#define RK3576_MALI_JS_STATUS_TILE_RANGE_FAULT    0x59
#define RK3576_MALI_JS_STATUS_OUT_OF_MEMORY       0x60
#define RK3576_MALI_JS_STATUS_UNKNOWN             0x7f
#define RK3576_MALI_JS_STATUS_TRANSLATION_FAULT   0xc0
#define RK3576_MALI_JS_STATUS_PERMISSION_FAULT    0xc8

/* MMU_CONTROL block ********************************************************/

#define RK3576_MALI_MMU_IRQ_RAWSTAT 0x2000
#define RK3576_MALI_MMU_IRQ_CLEAR   0x2004
#define RK3576_MALI_MMU_IRQ_MASK    0x2008
#define RK3576_MALI_MMU_IRQ_STATUS  0x200c

/* MMU_IRQ_* bit n = page fault on AS n, bit 16+n = bus error on AS n. */

#define RK3576_MALI_MMU_IRQ_PAGE_FAULT(as) (1u << (as))
#define RK3576_MALI_MMU_IRQ_BUS_ERROR(as)  (1u << (16 + (as)))
#define RK3576_MALI_MMU_IRQ_ALL            0xffffffff

/* Address space register file.  Each AS occupies 0x40 bytes. */

#define RK3576_MALI_AS_BASE(as)           (0x2400 + ((as)*0x40))

#define RK3576_MALI_AS_TRANSTAB_LO(a)     (RK3576_MALI_AS_BASE(a) + 0x00)
#define RK3576_MALI_AS_TRANSTAB_HI(a)     (RK3576_MALI_AS_BASE(a) + 0x04)
#define RK3576_MALI_AS_MEMATTR_LO(a)      (RK3576_MALI_AS_BASE(a) + 0x08)
#define RK3576_MALI_AS_MEMATTR_HI(a)      (RK3576_MALI_AS_BASE(a) + 0x0c)
#define RK3576_MALI_AS_LOCKADDR_LO(a)     (RK3576_MALI_AS_BASE(a) + 0x10)
#define RK3576_MALI_AS_LOCKADDR_HI(a)     (RK3576_MALI_AS_BASE(a) + 0x14)
#define RK3576_MALI_AS_COMMAND(a)         (RK3576_MALI_AS_BASE(a) + 0x18)
#define RK3576_MALI_AS_FAULTSTATUS(a)     (RK3576_MALI_AS_BASE(a) + 0x1c)
#define RK3576_MALI_AS_FAULTADDRESS_LO(a) (RK3576_MALI_AS_BASE(a) + 0x20)
#define RK3576_MALI_AS_FAULTADDRESS_HI(a) (RK3576_MALI_AS_BASE(a) + 0x24)
#define RK3576_MALI_AS_STATUS(a)          (RK3576_MALI_AS_BASE(a) + 0x28)
#define RK3576_MALI_AS_TRANSCFG_LO(a)     (RK3576_MALI_AS_BASE(a) + 0x30)
#define RK3576_MALI_AS_TRANSCFG_HI(a)     (RK3576_MALI_AS_BASE(a) + 0x34)
#define RK3576_MALI_AS_FAULTEXTRA_LO(a)   (RK3576_MALI_AS_BASE(a) + 0x38)
#define RK3576_MALI_AS_FAULTEXTRA_HI(a)   (RK3576_MALI_AS_BASE(a) + 0x3c)

/* AS_COMMAND opcodes */

#define RK3576_MALI_AS_CMD_NOP       0
#define RK3576_MALI_AS_CMD_UPDATE    1
#define RK3576_MALI_AS_CMD_LOCK      2
#define RK3576_MALI_AS_CMD_UNLOCK    3
#define RK3576_MALI_AS_CMD_FLUSH_PT  4
#define RK3576_MALI_AS_CMD_FLUSH_MEM 5

/* AS_STATUS bits */

#define RK3576_MALI_AS_STATUS_ACTIVE (1 << 0)

/* AS_TRANSTAB_LO addressing mode, used by the legacy (pre-AArch64) MMU
 * interface.  The low two bits select the mode, the rest of the register
 * is the page table base address.
 */

#define RK3576_MALI_AS_TRANSTAB_ADRMODE_MASK     0x03
#define RK3576_MALI_AS_TRANSTAB_ADRMODE_UNMAPPED 0x00
#define RK3576_MALI_AS_TRANSTAB_ADRMODE_IDENTITY 0x02
#define RK3576_MALI_AS_TRANSTAB_ADRMODE_TABLE    0x03
#define RK3576_MALI_AS_TRANSTAB_READ_INNER       (1 << 2)
#define RK3576_MALI_AS_TRANSTAB_SHARE_OUTER      (1 << 4)

/* AS_TRANSCFG_LO addressing mode, used by the AArch64 MMU interface that
 * Bifrost implements.  When an addressing mode other than LEGACY is
 * selected here, AS_TRANSTAB is ignored.
 */

#define RK3576_MALI_AS_TRANSCFG_ADRMODE_MASK        0x0f
#define RK3576_MALI_AS_TRANSCFG_ADRMODE_LEGACY      0x00
#define RK3576_MALI_AS_TRANSCFG_ADRMODE_UNMAPPED    0x01
#define RK3576_MALI_AS_TRANSCFG_ADRMODE_IDENTITY    0x02
#define RK3576_MALI_AS_TRANSCFG_ADRMODE_AARCH64_4K  0x06
#define RK3576_MALI_AS_TRANSCFG_ADRMODE_AARCH64_64K 0x08

#define RK3576_MALI_AS_TRANSCFG_PTW_MEMATTR_NC      (1 << 24)
#define RK3576_MALI_AS_TRANSCFG_PTW_MEMATTR_WB      (2 << 24)
#define RK3576_MALI_AS_TRANSCFG_PTW_SH_NS           (0 << 28)
#define RK3576_MALI_AS_TRANSCFG_PTW_SH_OS           (2 << 28)
#define RK3576_MALI_AS_TRANSCFG_PTW_SH_IS           (3 << 28)
#define RK3576_MALI_AS_TRANSCFG_PTW_RA              (1 << 30)

/* Bits 32..36 of AS_TRANSCFG, i.e. bits 0..4 of AS_TRANSCFG_HI. */

#define RK3576_MALI_AS_TRANSCFG_HI_DISABLE_HIER_AP  (1 << 1)
#define RK3576_MALI_AS_TRANSCFG_HI_DISABLE_AF_FAULT (1 << 2)
#define RK3576_MALI_AS_TRANSCFG_HI_WXN              (1 << 3)
#define RK3576_MALI_AS_TRANSCFG_HI_XREADABLE        (1 << 4)

/* AS_MEMATTR is an eight-slot attribute table with the same shape as an
 * ARMv8 MAIR.  The value below is the one the io-pgtable ARM_MALI_LPAE
 * format publishes:
 *
 *   slot 0 (non cacheable)      = 0x88 implementation defined
 *   slot 1 (cached)             = 0x8d write-back, write-allocate
 *   slot 2 (device)             = 0x88 implementation defined
 */

#define RK3576_MALI_MEMATTR_IMP_DEF     0x88
#define RK3576_MALI_MEMATTR_WRITE_ALLOC 0x8d
#define RK3576_MALI_MEMATTR_DEFAULT_LO                                    \
  (RK3576_MALI_MEMATTR_IMP_DEF | (RK3576_MALI_MEMATTR_WRITE_ALLOC << 8) | \
   (RK3576_MALI_MEMATTR_IMP_DEF << 16))
#define RK3576_MALI_MEMATTR_DEFAULT_HI 0x00000000

/* Job descriptor ***********************************************************/

/* Size of the 64-bit form of the job header, in bytes. */

#define RK3576_MALI_JOB_HEADER_SIZE 32

/* Byte 0x10 of the header: descriptor size flag plus job type. */

#define RK3576_MALI_JOB_SIZE_64BIT (1 << 0)
#define RK3576_MALI_JOB_TYPE_SHIFT 1
#define RK3576_MALI_JOB_TYPE_MASK  0x7f

/* Byte 0x11 of the header: dependency and cache behaviour flags. */

#define RK3576_MALI_JOB_FLAG_BARRIER           (1 << 0)
#define RK3576_MALI_JOB_FLAG_INVALIDATE_CACHE  (1 << 1)
#define RK3576_MALI_JOB_FLAG_SUPPRESS_PREFETCH (1 << 3)
#define RK3576_MALI_JOB_FLAG_ENABLE_TEXMAP     (1 << 4)
#define RK3576_MALI_JOB_FLAG_RELAX_DEP1        (1 << 6)
#define RK3576_MALI_JOB_FLAG_RELAX_DEP2        (1 << 7)

/* Job types, as published in the Mesa GenXML "Job Type" enumeration. */

#define RK3576_MALI_JOB_TYPE_NOT_STARTED 0
#define RK3576_MALI_JOB_TYPE_NULL        1
#define RK3576_MALI_JOB_TYPE_WRITE_VALUE 2
#define RK3576_MALI_JOB_TYPE_CACHE_FLUSH 3
#define RK3576_MALI_JOB_TYPE_COMPUTE     4
#define RK3576_MALI_JOB_TYPE_VERTEX      5
#define RK3576_MALI_JOB_TYPE_GEOMETRY    6
#define RK3576_MALI_JOB_TYPE_TILER       7
#define RK3576_MALI_JOB_TYPE_FUSED       8
#define RK3576_MALI_JOB_TYPE_FRAGMENT    9

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_MALI_H */
