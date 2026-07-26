/****************************************************************************
 * chips/rk3576/hardware/rk3576_rknpu.h
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
 * Rockchip RKNPU (Neural Processing Unit) register definitions for the
 * RK3576.  The SoC integrates two identical RKNN cores rated at 3 TOPS
 * each (6 TOPS combined, INT8):
 *
 *   RKNN core0  0x27700000, 32KB window, GIC INTID 279 (SPI 247)
 *   RKNN core1  0x27708000, 32KB window, GIC INTID 280 (SPI 248)
 *
 * Each core embeds a Rockchip "iommu-v2" instance with two register banks
 * inside that same window, at core_base + 0x2000 and core_base + 0x2100.
 * The device tree spells them out as separate reg entries:
 *
 *   0x27702000 / 0x27702100   core0 read and write MMU banks
 *   0x2770a000 / 0x2770a100   core1 read and write MMU banks
 *
 * Programming model of one core (the "PC", program counter, engine):
 *
 *   1. Build an array of struct rk3576_rknpu_hw_task_s in physically
 *      contiguous memory below 4GB.  Each entry points at a register
 *      command stream (also physically contiguous) that the PC engine
 *      replays into the core's internal register file.
 *   2. Publish the register command stream address in PC_DATA_ADDR and
 *      its length in PC_DATA_AMOUNT.
 *   3. Publish the task array address in PC_DMA_BASE_ADDR and the number
 *      of tasks in PC_TASK_CONTROL.
 *   4. Pulse PC_OPERATION_ENABLE (1 then 0) to start the engine.
 *   5. Wait for the completion interrupt, read INT_STATUS, acknowledge
 *      through INT_CLEAR and read PC_TASK_STATUS for the retired task
 *      counter.
 *
 * References: the RK3576 TRM does not publish the RKNPU register map, so
 * the offsets below come from the publicly available Rockchip "rknpu"
 * kernel driver register header, which is the same IP block on
 * RK3562/RK3568/RK3576/RK3588.  Everything that could not be
 * cross-checked against a second source is flagged TODO and has to be
 * confirmed on hardware - see the VERSION read in
 * rk3576_rknpu_initialize(), which is the cheapest first check.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RKNPU_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RKNPU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller instances *****************************************************/

#define RK3576_RKNPU_NCORES        2   /* Two identical RKNN cores        */
#define RK3576_RKNPU_CORE_MASK_ALL 0x3 /* Bit per core, both present     */

/* Size of one core register window (device tree "reg" length). */

#define RK3576_RKNPU_CORE_WINSIZE 0x8000

/* Core register offsets, relative to the core base *************************/

#define RK3576_RKNPU_VERSION          0x0000 /* IP version, read-only    */
#define RK3576_RKNPU_VERSION_NUM      0x0004 /* IP revision, read-only   */
#define RK3576_RKNPU_PC_OP_EN         0x0008 /* PC engine start pulse    */
#define RK3576_RKNPU_PC_DATA_ADDR     0x0010 /* Register command stream  */
#define RK3576_RKNPU_PC_DATA_AMOUNT   0x0014 /* Command stream length    */
#define RK3576_RKNPU_INT_MASK         0x0020 /* Interrupt enable mask    */
#define RK3576_RKNPU_INT_CLEAR        0x0024 /* Write 1 to acknowledge   */
#define RK3576_RKNPU_INT_STATUS       0x0028 /* Masked interrupt status  */
#define RK3576_RKNPU_INT_RAW_STATUS   0x002c /* Unmasked status          */
#define RK3576_RKNPU_PC_TASK_CONTROL  0x0030 /* Task count + run mode    */
#define RK3576_RKNPU_PC_DMA_BASE_ADDR 0x0034 /* Task descriptor array    */
#define RK3576_RKNPU_PC_TASK_STATUS   0x003c /* Retired task counter     */
#define RK3576_RKNPU_ENABLE_MASK      0x0f08 /* Sub-engine enable mask   */

/* Bandwidth / traffic counters.  Reset by writing 1 to CLR_ALL_RW_AMOUNT,
 * then read as 32-bit byte counters.  Used by the ACTION ioctl to report
 * how much traffic a job generated.
 */

#define RK3576_RKNPU_CLR_ALL_RW_AMOUNT 0x8010 /* Clear the counters      */
#define RK3576_RKNPU_DT_WR_AMOUNT      0x8034 /* Feature map writes      */
#define RK3576_RKNPU_DT_RD_AMOUNT      0x8038 /* Feature map reads       */
#define RK3576_RKNPU_WT_RD_AMOUNT      0x803c /* Weight reads            */

/* PC_OP_EN ****************************************************************/

#define RK3576_RKNPU_PC_OP_EN_START (1 << 0) /* Pulsed 1 -> 0 to start   */

/* PC_TASK_CONTROL **********************************************************
 *
 * [11:0]  run-mode selector, 0x6 selects "PC fetches the task array"
 * [23:12] number of tasks the PC engine has to execute
 *
 * TODO: the 0x6 run-mode encoding and the 12-bit task count field width
 * are taken from the Rockchip driver's RK3576 configuration table
 * (pc_task_number_bits = 12).  Confirm both against a real job that the
 * hardware retires (PC_TASK_STATUS must equal the task count).
 */

#define RK3576_RKNPU_PC_TASK_NUMBER_SHIFT 12
#define RK3576_RKNPU_PC_TASK_NUMBER_MASK  0xfff
#define RK3576_RKNPU_PC_TASK_MODE         0x6
#define RK3576_RKNPU_PC_TASK_MAX          RK3576_RKNPU_PC_TASK_NUMBER_MASK

/* PC_DATA_AMOUNT ***********************************************************
 *
 * The engine wants ((regcfg_amount + EXTRA) / SCALE) - 1, i.e. the number
 * of fetch bursts minus one rather than a raw word count.  SCALE is 2 on
 * RK3576 (the PC engine fetches two register words per beat).
 *
 * TODO: SCALE and EXTRA come from the Rockchip driver's per-SoC table.
 * Confirm on hardware; a wrong value shows up as a job that never
 * completes (timeout) or as a bus error interrupt.
 */

#define RK3576_RKNPU_PC_DATA_EXTRA_AMOUNT 4
#define RK3576_RKNPU_PC_DATA_AMOUNT_SCALE 2

/* Interrupt bits ***********************************************************
 *
 * The core raises one bit per sub-engine completion plus error bits.  The
 * exact per-bit assignment is not published; the driver treats the whole
 * low 17-bit field as "something happened", hands the raw value back to
 * the caller (which knows the int_mask it asked for) and acknowledges
 * everything.
 *
 * TODO: split RK3576_RKNPU_INT_ALL into named completion / error bits
 * once a real job has been observed on hardware.
 */

#define RK3576_RKNPU_INT_ALL 0x0001ffff

/* Rockchip IOMMU v2 ********************************************************
 *
 * Two banks per core, at the offsets below inside the core window.  The
 * register layout is the standard Rockchip IOMMU one (identical to the
 * block Linux drives from drivers/iommu/rockchip-iommu.c).
 */

#define RK3576_RKNPU_MMU_NBANKS          2
#define RK3576_RKNPU_MMU0_OFFSET         0x2000
#define RK3576_RKNPU_MMU1_OFFSET         0x2100

#define RK3576_RKNPU_MMU_DTE_ADDR        0x00 /* Directory table base    */
#define RK3576_RKNPU_MMU_STATUS          0x04 /* Status, read-only       */
#define RK3576_RKNPU_MMU_COMMAND         0x08 /* Command, write-only     */
#define RK3576_RKNPU_MMU_PAGE_FAULT_ADDR 0x0c /* Faulting address        */
#define RK3576_RKNPU_MMU_ZAP_ONE_LINE    0x10 /* Invalidate one line     */
#define RK3576_RKNPU_MMU_INT_RAWSTAT     0x14 /* Unmasked IRQ status     */
#define RK3576_RKNPU_MMU_INT_CLEAR       0x18 /* Write 1 to acknowledge  */
#define RK3576_RKNPU_MMU_INT_MASK        0x1c /* Interrupt enable mask   */
#define RK3576_RKNPU_MMU_INT_STATUS      0x20 /* Masked IRQ status       */
#define RK3576_RKNPU_MMU_AUTO_GATING     0x24 /* Clock auto gating       */

/* MMU_STATUS bits */

#define RK3576_RKNPU_MMU_ST_PAGING_ENABLED      (1 << 0)
#define RK3576_RKNPU_MMU_ST_PAGE_FAULT_ACTIVE   (1 << 1)
#define RK3576_RKNPU_MMU_ST_STALL_ACTIVE        (1 << 2)
#define RK3576_RKNPU_MMU_ST_IDLE                (1 << 3)
#define RK3576_RKNPU_MMU_ST_REPLAY_BUF_EMPTY    (1 << 4)
#define RK3576_RKNPU_MMU_ST_PAGE_FAULT_IS_WRITE (1 << 5)
#define RK3576_RKNPU_MMU_ST_STALL_NOT_ACTIVE    (1u << 31)

/* MMU_COMMAND values (the register takes a command code, not a bitmask) */

#define RK3576_RKNPU_MMU_CMD_ENABLE_PAGING   0
#define RK3576_RKNPU_MMU_CMD_DISABLE_PAGING  1
#define RK3576_RKNPU_MMU_CMD_ENABLE_STALL    2
#define RK3576_RKNPU_MMU_CMD_DISABLE_STALL   3
#define RK3576_RKNPU_MMU_CMD_ZAP_CACHE       4
#define RK3576_RKNPU_MMU_CMD_PAGE_FAULT_DONE 5
#define RK3576_RKNPU_MMU_CMD_FORCE_RESET     6

/* MMU interrupt bits, shared by RAWSTAT / CLEAR / MASK / STATUS */

#define RK3576_RKNPU_MMU_IRQ_PAGE_FAULT (1 << 0)
#define RK3576_RKNPU_MMU_IRQ_BUS_ERROR  (1 << 1)
#define RK3576_RKNPU_MMU_IRQ_ALL        0x3

/* CRU soft resets **********************************************************
 *
 * The four resets the device tree lists for npu@27700000, expressed as
 * Rockchip flat reset identifiers.  register = SOFTRST_CON(id / 16),
 * bit = id % 16, hiword-mask write:
 *
 *   srst_a0      0x1c9 -> SOFTRST_CON28 bit 9
 *   srst_a1      0x1d0 -> SOFTRST_CON29 bit 0
 *   srst_a_cbuf  0x200 -> SOFTRST_CON32 bit 0
 *   srst_h_cbuf  0x20c -> SOFTRST_CON32 bit 12
 *
 * The bit positions agree with the gate bits of aclk_rknn0 / aclk_rknn1 /
 * aclk_rknn_cbuf / hclk_rknn_cbuf in CLKGATE_CON28/29/32, which is the
 * usual Rockchip layout and a useful cross-check.
 */

#define RK3576_RKNPU_RST_A0       0x1c9
#define RK3576_RKNPU_RST_A1       0x1d0
#define RK3576_RKNPU_RST_A_CBUF   0x200
#define RK3576_RKNPU_RST_H_CBUF   0x20c

#define RK3576_RKNPU_RST_BANK(id) ((id) >> 4)
#define RK3576_RKNPU_RST_BIT(id)  ((id)&0xf)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RKNPU_H */
