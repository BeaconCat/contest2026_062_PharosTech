/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_rknpu.h
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
 * Public interface of the RK3576 RKNPU (6 TOPS NPU) core driver.
 *
 * NuttX has no NPU / accelerator subsystem to plug into (drivers/ has no
 * equivalent of the video, audio or sensor frameworks for compute
 * offload), so the driver exposes a character device, /dev/rknpu, plus a
 * set of direct in-kernel entry points.  The ioctl set deliberately
 * mirrors the semantics of the Linux "rknpu" driver - ACTION, SUBMIT,
 * MEM_CREATE, MEM_MAP, MEM_DESTROY - so that a matmul layer written
 * against the public rknn_matmul_api programming model can be ported with
 * a change of ioctl numbers and structure names only.
 *
 * Memory model.  Everything the NPU touches - the task descriptor array,
 * the register command streams and the tensor buffers - has to be
 * physically contiguous and below the 4GB line, because the core's DMA
 * address registers are 32 bits wide.  The driver therefore allocates
 * exclusively from rk3576_dma_alloc() and, for now, leaves the per-core
 * IOMMU in bypass (paging disabled), so a DMA address is simply the
 * physical address of the buffer.  Cache maintenance is explicit: the
 * caller syncs a buffer to the device before submitting and from the
 * device before reading results back.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_RKNPU_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_RKNPU_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#ifdef CONFIG_RK3576_RKNPU

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Character device path. */

#define RK3576_RKNPU_DEVPATH "/dev/rknpu"

/* Driver ABI version reported by RK3576_RKNPU_ACT_GET_DRV_VERSION.  Bump
 * the minor on additive changes, the major whenever a structure below
 * changes shape.
 */

#define RK3576_RKNPU_DRV_VERSION_MAJOR 0
#define RK3576_RKNPU_DRV_VERSION_MINOR 1
#define RK3576_RKNPU_DRV_VERSION \
  ((RK3576_RKNPU_DRV_VERSION_MAJOR << 16) | RK3576_RKNPU_DRV_VERSION_MINOR)

/* Number of cores as seen by the ABI.  Kept separate from the register
 * level RK3576_RKNPU_NCORES so that this header stays self-contained and
 * usable from application code; rk3576_rknpu.c asserts that the two agree.
 */

#define RK3576_RKNPU_NCORES_ABI 2

/* Core selection bits for the .core_mask field of a submission. */

#define RK3576_RKNPU_CORE0     (1 << 0)
#define RK3576_RKNPU_CORE1     (1 << 1)
#define RK3576_RKNPU_CORE_AUTO 0 /* Driver picks core 0 */

/* Cache maintenance directions for RK3576_RKNPUIOC_MEM_SYNC and for
 * rk3576_rknpu_mem_sync().  They may be combined.
 */

#define RK3576_RKNPU_SYNC_TO_DEVICE   (1 << 0) /* Clean  D-cache  */
#define RK3576_RKNPU_SYNC_FROM_DEVICE (1 << 1) /* Invalidate D-cache */

/* Default job timeout, in milliseconds, used when a submission leaves
 * .timeout_ms at zero.
 */

#define RK3576_RKNPU_DEFAULT_TIMEOUT_MS 6000

/* Sub-commands of RK3576_RKNPUIOC_ACTION.  The numbering follows the
 * Linux rknpu driver so that ported user code keeps working.
 */

#define RK3576_RKNPU_ACT_GET_HW_VERSION   0  /* Core VERSION register    */
#define RK3576_RKNPU_ACT_GET_DRV_VERSION  1  /* RK3576_RKNPU_DRV_VERSION */
#define RK3576_RKNPU_ACT_GET_FREQ         2  /* Core clock, in Hz        */
#define RK3576_RKNPU_ACT_SET_FREQ         3  /* Core clock, in Hz        */
#define RK3576_RKNPU_ACT_RESET            6  /* Soft-reset both cores    */
#define RK3576_RKNPU_ACT_GET_IOMMU_EN     16 /* 0: bypass, 1: paging     */
#define RK3576_RKNPU_ACT_GET_TOTAL_RW     15 /* Bytes moved by last job  */

/* ioctl commands.  0x1e00 is the private RKNPU range; the RGA driver owns
 * 0x1f00.  Every argument is a pointer to the structure named in the
 * comment; the structures are read and written in place.
 */

#define RK3576_RKNPUIOC_ACTION      _IOC(0x1e00, 1) /* rknpu_action_s * */
#define RK3576_RKNPUIOC_SUBMIT      _IOC(0x1e00, 2) /* rknpu_task_s *   */
#define RK3576_RKNPUIOC_MEM_CREATE  _IOC(0x1e00, 3) /* rknpu_mem_s *    */
#define RK3576_RKNPUIOC_MEM_MAP     _IOC(0x1e00, 4) /* rknpu_mem_s *    */
#define RK3576_RKNPUIOC_MEM_DESTROY _IOC(0x1e00, 5) /* rknpu_mem_s *    */
#define RK3576_RKNPUIOC_MEM_SYNC    _IOC(0x1e00, 6) /* rknpu_sync_s *   */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Hardware task descriptor.
 *
 * This is *not* a driver invention: it is the layout the NPU's PC engine
 * fetches from PC_DMA_BASE_ADDR, one entry per task.  The producer of the
 * register command streams (the matmul layer) fills an array of these in
 * DMA-safe memory and hands its DMA address to the driver.
 *
 *   flags         - per task control bits, 0 for a plain compute task
 *   op_idx        - operator index, echoed back in the status registers
 *   enable_mask   - sub-engine enable value written to ENABLE_MASK
 *   int_mask      - interrupt sources this task wants, written to INT_MASK
 *   int_clear     - acknowledge value the engine uses for this task
 *   int_status    - written by the hardware with the raised sources
 *   regcfg_amount - number of 32-bit words in the register command stream
 *   regcfg_offset - byte offset of the stream inside its buffer
 *   regcmd_addr   - DMA address of the register command stream
 *
 * The natural alignment of regcmd_addr places it at offset 32 and makes
 * the structure exactly 40 bytes without any padding; rk3576_rknpu.c
 * checks that at init time rather than relying on a packed attribute.
 */

struct rk3576_rknpu_hw_task_s
{
  uint32_t flags;
  uint32_t op_idx;
  uint32_t enable_mask;
  uint32_t int_mask;
  uint32_t int_clear;
  uint32_t int_status;
  uint32_t regcfg_amount;
  uint32_t regcfg_offset;
  uint64_t regcmd_addr;
};

/* Software submission descriptor - what a caller hands to
 * rk3576_rknpu_submit() or to RK3576_RKNPUIOC_SUBMIT.
 *
 * task_addr points at an array of struct rk3576_rknpu_hw_task_s in
 * DMA-safe memory.  task_start is the index of the first entry to run and
 * task_number how many entries follow it.  When core_mask selects both
 * cores the very same range is programmed into both, which is what the
 * Rockchip runtime does for a "big" job that the compiler already split.
 *
 * On return int_status[] holds the raw interrupt status each selected
 * core raised, and task_counter the value of its PC_TASK_STATUS register,
 * which is how many tasks the engine actually retired.
 */

struct rk3576_rknpu_task_s
{
  uint32_t core_mask;                            /* RK3576_RKNPU_CORExx  */
  uint32_t task_start;                           /* First task index     */
  uint32_t task_number;                          /* Tasks to execute     */
  uint32_t timeout_ms;                           /* 0: driver default    */
  uint64_t task_addr;                            /* DMA addr of array    */
  uint32_t int_status[RK3576_RKNPU_NCORES_ABI];  /* Out: raw INT status  */
  uint32_t task_counter[RK3576_RKNPU_NCORES_ABI];/* Out: retired tasks   */
};

/* Memory object descriptor, shared by MEM_CREATE / MEM_MAP / MEM_DESTROY.
 *
 * MEM_CREATE fills in handle, va and dma_addr for a fresh allocation of
 * 'size' bytes.  MEM_MAP resolves an existing handle back to its
 * addresses (in the NuttX flat address space there is nothing to map, the
 * allocation address is already usable, so this is a lookup).
 * MEM_DESTROY releases the handle and its buffer; only .handle is read.
 */

struct rk3576_rknpu_mem_s
{
  uint32_t handle;   /* Object handle, non-zero, assigned by MEM_CREATE  */
  uint32_t flags;    /* Reserved, must be zero                           */
  uint64_t size;     /* Allocation size, in bytes                        */
  uint64_t va;       /* CPU-visible address of the allocation            */
  uint64_t dma_addr; /* Address the NPU has to be given, 32-bit safe     */
};

/* Cache maintenance request for RK3576_RKNPUIOC_MEM_SYNC. */

struct rk3576_rknpu_sync_s
{
  uint32_t handle; /* Object to sync, from MEM_CREATE                */
  uint32_t flags;  /* RK3576_RKNPU_SYNC_TO_DEVICE / _FROM_DEVICE     */
  uint64_t offset; /* Byte offset inside the object                  */
  uint64_t size;   /* Number of bytes to sync, 0 means "all of it"   */
};

/* Argument of RK3576_RKNPUIOC_ACTION: 'flags' selects the sub-command,
 * 'value' carries the argument in and the result out.
 */

struct rk3576_rknpu_action_s
{
  uint32_t flags; /* RK3576_RKNPU_ACT_*                              */
  uint32_t value; /* In and/or out, depending on the sub-command     */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_rknpu_initialize
 *
 * Description:
 *   Bring the NPU up: power the VD_NPU voltage domain and the NPUTOP /
 *   NPU0 / NPU1 power domains, enable the clock tree, release the four
 *   CRU soft resets, put both per-core IOMMUs into bypass, attach the two
 *   completion interrupts and register /dev/rknpu.
 *
 *   Must be called from board_late_initialize(), i.e. after
 *   rk3576_clk_tree_initialize() has registered the clock tree and after
 *   rk3576_dma_alloc_init() has created the DMA heap.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.  Calling it twice is
 *   harmless and returns OK.
 *
 ****************************************************************************/

int rk3576_rknpu_initialize(void);

/****************************************************************************
 * Name: rk3576_rknpu_submit
 *
 * Description:
 *   Run a task list on one or both cores and block until the hardware
 *   signals completion.  The caller owns the task array and the register
 *   command streams it references, and is responsible for having synced
 *   both to the device (see rk3576_rknpu_mem_sync()) before the call.
 *
 *   On return the .int_status[] and .task_counter[] fields of 'task' hold
 *   the per core completion information.
 *
 * Input Parameters:
 *   task - Submission descriptor; the output fields are written in place,
 *          so it may not point into read-only memory.
 *
 * Returned Value:
 *   OK on success; -EINVAL for a malformed request, -ENODEV if the driver
 *   was never initialized, -ETIMEDOUT if a core did not raise its
 *   completion interrupt in time (both cores are reset in that case).
 *
 ****************************************************************************/

int rk3576_rknpu_submit(struct rk3576_rknpu_task_s *task);

/****************************************************************************
 * Name: rk3576_rknpu_reset
 *
 * Description:
 *   Pulse the four CRU soft resets of the NPU and re-apply the IOMMU
 *   bypass configuration.  Used on the error path of a submission and
 *   exported for the RK3576_RKNPU_ACT_RESET action.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rknpu_reset(void);

/****************************************************************************
 * Name: rk3576_rknpu_mem_alloc
 *
 * Description:
 *   Allocate a DMA-safe buffer the NPU can reach: physically contiguous,
 *   64-byte aligned and below the 4GB line.  Thin wrapper around
 *   rk3576_dma_alloc() that also resolves the physical address for the
 *   caller.
 *
 * Input Parameters:
 *   size     - Number of bytes to allocate
 *   dma_addr - Receives the address to program into the NPU; may be NULL
 *
 * Returned Value:
 *   CPU-visible pointer to the buffer, or NULL when the DMA heap is
 *   exhausted.
 *
 ****************************************************************************/

void *rk3576_rknpu_mem_alloc(size_t size, uintptr_t *dma_addr);

/****************************************************************************
 * Name: rk3576_rknpu_mem_free
 *
 * Description:
 *   Release a buffer obtained from rk3576_rknpu_mem_alloc().  'size' must
 *   be the size passed to the allocating call.
 *
 ****************************************************************************/

void rk3576_rknpu_mem_free(void *va, size_t size);

/****************************************************************************
 * Name: rk3576_rknpu_mem_sync
 *
 * Description:
 *   Perform the cache maintenance a buffer needs when ownership moves
 *   between the CPU and the NPU.  RK3576_RKNPU_SYNC_TO_DEVICE cleans the
 *   D-cache so the accelerator sees what the CPU wrote;
 *   RK3576_RKNPU_SYNC_FROM_DEVICE invalidates it so the CPU sees what the
 *   accelerator wrote.  Both may be set.
 *
 * Input Parameters:
 *   va    - Start of the region, must be 64-byte aligned in practice
 *   size  - Length of the region, in bytes
 *   flags - RK3576_RKNPU_SYNC_TO_DEVICE / RK3576_RKNPU_SYNC_FROM_DEVICE
 *
 ****************************************************************************/

void rk3576_rknpu_mem_sync(void *va, size_t size, uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_RKNPU */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_RKNPU_H */
