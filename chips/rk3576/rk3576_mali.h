/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_mali.h
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
 * Public interface of the RK3576 Mali-G52 Job Manager driver.
 *
 * NuttX has no GPU / DRM subsystem to plug into (drivers/video only hosts
 * the frame buffer and V4L2-style capture interfaces, neither of which can
 * express a command submission queue), so this module exports an explicit
 * in-kernel API instead of implementing a standard lower half.
 *
 * The driver deliberately implements only the command submission path of
 * the GPU: bring the block out of reset, power the shader cores up,
 * configure one address space and push pre-built job chains at the job
 * slots.  It contains no shader compiler and no graphics state tracker.
 * Command streams and shader binaries are produced offline on a host PC by
 * Mesa/Panfrost and replayed here (see rk3576_mali_replay()).
 *
 * Address translation is configured as a 1:1 identity mapping, so every
 * GPU virtual address is a physical address.  All memory handed to the GPU
 * must therefore be physically contiguous and below the 4GB line, i.e. it
 * must come from rk3576_dma_alloc().
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_MALI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_MALI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

#include "hardware/rk3576_mali.h"

#ifdef CONFIG_RK3576_MALI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Job slot assignment used by every Mali driver:
 *
 *   slot 0 - fragment jobs
 *   slot 1 - vertex, geometry, tiler and compute jobs
 *   slot 2 - secondary compute slot, unused here
 */

#define RK3576_MALI_SLOT_FRAGMENT 0
#define RK3576_MALI_SLOT_VERTEX   1
#define RK3576_MALI_SLOT_COMPUTE  2

/* Default JS_CONFIG_NEXT value for a submission through address space 0:
 * clean and invalidate the caches on both ends of the job so that CPU
 * writes are visible to the GPU and GPU writes are visible to the CPU,
 * enable MMU translation and run at the middle thread priority.
 */

#define RK3576_MALI_CONFIG_DEFAULT                                         \
  (RK3576_MALI_JS_CONFIG_AS(0) | RK3576_MALI_JS_CONFIG_START_FLUSH_INV |   \
   RK3576_MALI_JS_CONFIG_END_FLUSH_INV | RK3576_MALI_JS_CONFIG_START_MMU | \
   RK3576_MALI_JS_CONFIG_THREAD_PRI(8))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Bifrost job header, the 32-byte structure every job chain entry starts
 * with.  The layout is the one published by Mesa (GenXML "Job Header",
 * eight 32-bit words):
 *
 *   0x00 exception_status       written back by the GPU on completion
 *   0x04 first_incomplete_task  written back by the GPU on failure
 *   0x08 fault_pointer          written back by the GPU on failure
 *   0x10 type_and_size          bit 0 = 64-bit descriptor, bits 7:1 type
 *   0x11 flags                  barrier / cache / dependency relaxation
 *   0x12 index                  identifier of this job inside the chain
 *   0x14 dependency1            index of a job that must complete first
 *   0x16 dependency2            index of a job that must complete first
 *   0x18 next                   address of the next header, 0 to end
 *
 * The job-type specific payload follows immediately after the header.
 */

begin_packed_struct struct rk3576_mali_job_header_s
{
  uint32_t exception_status;
  uint32_t first_incomplete_task;
  uint64_t fault_pointer;
  uint8_t type_and_size;
  uint8_t flags;
  uint16_t index;
  uint16_t dependency1;
  uint16_t dependency2;
  uint64_t next;
} end_packed_struct;

/* Read-only snapshot of what the hardware reported about itself during
 * bring-up.  Returned by rk3576_mali_info().
 */

struct rk3576_mali_info_s
{
  uint32_t gpu_id;         /* Raw GPU_ID register                       */
  uint32_t product_id;     /* GPU_ID[31:16], 0x7002 for Mali-G52        */
  uint32_t l2_features;    /* Raw L2_FEATURES                           */
  uint32_t tiler_features; /* Raw TILER_FEATURES                        */
  uint32_t mem_features;   /* Raw MEM_FEATURES                          */
  uint32_t mmu_features;   /* Raw MMU_FEATURES                          */
  uint32_t as_present;     /* Bitmask of implemented address spaces     */
  uint32_t js_present;     /* Bitmask of implemented job slots          */
  uint64_t shader_present; /* Bitmask of implemented shader cores       */
  uint64_t tiler_present;  /* Bitmask of implemented tiler units        */
  uint64_t l2_present;     /* Bitmask of implemented L2 slices          */
  uint64_t stack_present;  /* Bitmask of implemented core stacks        */
  uint32_t coreclk;        /* Core clock rate reported by the CLK tree  */
};

/* Description of one recorded command stream to replay.
 *
 * cmdstream is a byte-exact copy of the GPU-visible memory block that a
 * host side capture (PANDECODE / pandecode-standalone) dumped, starting at
 * capture_cva.  shader_binary is the matching block of pre-compiled
 * Bifrost instructions, captured at capture_sva.  Both are copied into
 * freshly allocated DMA memory before submission and then relocated: any
 * naturally aligned 64-bit word inside the command stream that points into
 * one of the two captured ranges is rewritten to point at the copy.
 *
 * shader_binary may be NULL when the recorded stream embeds its shaders
 * inside cmdstream, in which case only the command stream range is
 * relocated.
 */

struct rk3576_mali_replay_s
{
  const void *cmdstream;     /* Recorded job chain plus descriptors     */
  size_t cmdsize;            /* Size of cmdstream, in bytes             */
  const void *shader_binary; /* Pre-compiled Bifrost ISA, may be NULL   */
  size_t shader_size;        /* Size of shader_binary, in bytes         */
  uint64_t capture_cva;      /* GPU address cmdstream was captured at   */
  uint64_t capture_sva;      /* GPU address shader_binary was captured  */
  uint32_t job_offset;       /* Offset of the first job header in cmd   */
  unsigned int slot;         /* Job slot to submit to                   */
  uint32_t config;           /* JS_CONFIG_NEXT value, 0 = default       */
  unsigned int timeout_ms;   /* Completion timeout, 0 = default         */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_mali_initialize
 *
 * Description:
 *   Bring the Mali-G52 up: enable its clocks, power the VD_GPU domain,
 *   soft reset the block, read and log the identity registers, power the
 *   L2 slices, core stacks, shader cores and tiler up, install a 1:1
 *   identity address space and attach the three interrupt handlers.
 *
 *   Must be called after rk3576_clk_tree_initialize(), i.e. from
 *   board_late_initialize().  Calling it twice is a no-op.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.  -ENODEV is returned
 *   when the GPU_ID register does not identify a Mali-G52, which is the
 *   symptom of the power domain or the clocks not being up.
 *
 ****************************************************************************/

int rk3576_mali_initialize(void);

/****************************************************************************
 * Name: rk3576_mali_info
 *
 * Description:
 *   Return the identity and feature snapshot collected during bring-up.
 *
 * Returned Value:
 *   A pointer to the snapshot, or NULL if the driver is not initialized.
 *
 ****************************************************************************/

const struct rk3576_mali_info_s *rk3576_mali_info(void);

/****************************************************************************
 * Name: rk3576_mali_submit_job
 *
 * Description:
 *   Publish a job chain on a job slot and start it.  The call returns as
 *   soon as the hardware has accepted the chain; use
 *   rk3576_mali_wait_done() to block until it retires.
 *
 *   The caller owns the job chain memory and must keep it alive, and must
 *   have cleaned the D-cache over every descriptor the GPU will read.
 *
 * Input Parameters:
 *   slot            - Job slot index, 0..2.  See RK3576_MALI_SLOT_*.
 *   job_chain_phys  - Physical (== GPU) address of the first job header.
 *                     Must be 64-byte aligned.
 *   config          - JS_CONFIG_NEXT value; 0 selects
 *                     RK3576_MALI_CONFIG_DEFAULT.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.  -EBUSY is returned
 *   when a job is already outstanding on that slot.
 *
 ****************************************************************************/

int rk3576_mali_submit_job(unsigned int slot, uintptr_t job_chain_phys,
                           uint32_t config);

/****************************************************************************
 * Name: rk3576_mali_wait_done
 *
 * Description:
 *   Block until the job outstanding on a slot retires.  On a timeout the
 *   slot is hard stopped so the hardware is left in a usable state.
 *
 * Input Parameters:
 *   slot       - Job slot index, 0..2.
 *   timeout_ms - Maximum time to wait, in milliseconds.  0 selects the
 *                built-in default.
 *
 * Returned Value:
 *   OK when the job completed successfully; -ETIMEDOUT if it did not
 *   retire in time; -EIO when the hardware reported a job fault (the
 *   JS_STATUS code is logged); -EINVAL / -ESRCH for a bad slot or an idle
 *   slot.
 *
 ****************************************************************************/

int rk3576_mali_wait_done(unsigned int slot, unsigned int timeout_ms);

/****************************************************************************
 * Name: rk3576_mali_run_job
 *
 * Description:
 *   Convenience wrapper: submit a job chain and wait for it to retire.
 *
 * Input Parameters:
 *   slot           - Job slot index, 0..2.
 *   job_chain_phys - Physical address of the first job header.
 *   config         - JS_CONFIG_NEXT value; 0 selects the default.
 *   timeout_ms     - Completion timeout; 0 selects the default.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_mali_run_job(unsigned int slot, uintptr_t job_chain_phys,
                        uint32_t config, unsigned int timeout_ms);

/****************************************************************************
 * Name: rk3576_mali_job_header_init
 *
 * Description:
 *   Fill a job header with the fields the hardware requires: the 64-bit
 *   descriptor flag, the job type, the chain index and the link to the
 *   next header.  Every other field is zeroed.
 *
 * Input Parameters:
 *   hdr   - Header to initialise, in DMA-safe memory.
 *   type  - One of the RK3576_MALI_JOB_TYPE_* values.
 *   index - Identifier of this job inside the chain.  Must be non-zero and
 *           strictly increasing along the chain.
 *   next  - Physical address of the next header, 0 to terminate.
 *
 ****************************************************************************/

void rk3576_mali_job_header_init(struct rk3576_mali_job_header_s *hdr,
                                 uint8_t type, uint16_t index, uint64_t next);

/****************************************************************************
 * Name: rk3576_mali_null_job
 *
 * Description:
 *   Submit a single NULL job and wait for it.  A NULL job touches no
 *   memory and runs no shader, so it exercises exactly the Job Manager
 *   path: descriptor fetch through the address space, slot start, and the
 *   completion interrupt.  This is the bring-up self test for the command
 *   submission path.
 *
 * Returned Value:
 *   OK when the job retired with a clean status; a negated errno value
 *   otherwise.
 *
 ****************************************************************************/

int rk3576_mali_null_job(void);

/****************************************************************************
 * Name: rk3576_mali_replay
 *
 * Description:
 *   Replay a command stream recorded on a host PC.  The command stream and
 *   the shader binary are copied into DMA-safe memory, pointers inside the
 *   command stream that referred to the capture-time addresses are rebased
 *   onto the copies, the D-cache is cleaned over both blocks and the chain
 *   is submitted and waited for.  The scratch memory is released before
 *   returning.
 *
 * Input Parameters:
 *   replay - Description of the recording; see the structure comment.
 *
 * Returned Value:
 *   OK when the recorded chain retired cleanly; a negated errno value
 *   otherwise.
 *
 ****************************************************************************/

int rk3576_mali_replay(const struct rk3576_mali_replay_s *replay);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_MALI */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_MALI_H */
