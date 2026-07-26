/****************************************************************************
 * chips/rk3576/rk3576_rknpu.c
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
 * RK3576 RKNPU (Neural Processing Unit) core driver.
 *
 * The SoC carries two RKNN cores, 3 TOPS INT8 each.  Both live in the
 * VD_NPU voltage domain and have to be powered and clocked before a
 * single register read returns anything but zero, which is why bring-up
 * runs power domains first, then the clock tree, then the CRU soft
 * resets.
 *
 * A job is a list of hardware task descriptors (struct
 * rk3576_rknpu_hw_task_s) in DMA-safe memory.  Each descriptor points at
 * a register command stream that the core's PC engine replays into its
 * internal register file; the engine then runs the convolution / matmul
 * the stream describes and raises a completion interrupt.  The driver
 * only owns the plumbing - it does not synthesise command streams.  That
 * is the job of the matmul layer above it, which follows the publicly
 * documented rknn_matmul_api programming model.
 *
 * Address translation.  Both per-core IOMMUs are left in bypass (paging
 * disabled), so every address the NPU is handed is a physical address.
 * All buffers therefore come from rk3576_dma_alloc(), which guarantees
 * physical contiguity, 64-byte alignment and an address below the 4GB
 * line - the core's DMA address registers are only 32 bits wide.  Full
 * page-table management is a TODO; the register definitions for it are
 * already in hardware/rk3576_rknpu.h.
 *
 * NuttX has no accelerator subsystem, so the user-space face is a
 * character device with private ioctls whose semantics mirror the Linux
 * "rknpu" driver (see rk3576_rknpu.h).
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_rknpu.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_pd.h"
#include "rk3576_rknpu.h"

#ifdef CONFIG_RK3576_RKNPU

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of memory objects the handle table can hold at once. */

#ifndef CONFIG_RK3576_RKNPU_MAX_MEMS
#define CONFIG_RK3576_RKNPU_MAX_MEMS 64
#endif

/* Soft reset pulse width and settle time, in microseconds. */

#define RK3576_RKNPU_RESET_DELAY_US 20

/* Polling budget while waiting for an IOMMU command to retire. */

#define RK3576_RKNPU_MMU_RETRIES  1000
#define RK3576_RKNPU_MMU_DELAY_US 1

/* Size of one hardware task descriptor, in bytes, and the alignment the
 * PC engine needs for the array holding them.  40 is not a power of two,
 * so the two are unrelated: the array only has to be 8-byte aligned
 * (rk3576_dma_alloc() hands out 64-byte aligned memory anyway).
 */

#define RK3576_RKNPU_HW_TASK_BYTES sizeof(struct rk3576_rknpu_hw_task_s)
#define RK3576_RKNPU_TASK_ALIGN    8

/* Size the hardware expects a descriptor to have: eight 32-bit fields
 * followed by one 64-bit address.  Checked against the compiler's idea of
 * the structure at init time.
 */

#define RK3576_RKNPU_HW_TASK_ABI_BYTES (8 * 4 + 8)

/* Highest address the NPU can generate: the DMA address registers are
 * 32 bits wide, so every buffer has to live below this line.
 */

#define RK3576_RKNPU_DMA_LIMIT UINT64_C(0x100000000)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Immutable per-core description. */

struct rk3576_rknpu_coredesc_s
{
  uintptr_t base; /* Register window base address        */
  int irq;        /* GIC INTID, shared with the core MMU */
  int pd;         /* Power domain feeding this core      */
};

/* Per-core run-time state. */

struct rk3576_rknpu_core_s
{
  const struct rk3576_rknpu_coredesc_s *desc;
  sem_t donesem;      /* Posted by the ISR when the job ends    */
  uint32_t intstatus; /* Raw INT_RAW_STATUS latched by the ISR  */
  bool mmufault;      /* An IOMMU fault was seen for this job   */
  bool done;          /* Guards against a double semaphore post */
};

/* One entry of the memory handle table. */

struct rk3576_rknpu_memobj_s
{
  void *va;    /* Allocation address, NULL when the slot is free */
  size_t size; /* Size the allocation was made with              */
};

/* Driver state.  There is a single instance: the two cores are two
 * halves of one accelerator, not two independent devices.
 */

struct rk3576_rknpu_dev_s
{
  struct rk3576_rknpu_core_s core[RK3576_RKNPU_NCORES];
  struct clk_s *coreclk; /* clk_rknn_dsu0, the DVFS handle        */
  uint32_t version;      /* VERSION register, read once at init   */
  uint32_t versionnum;   /* VERSION_NUM register                  */
  mutex_t lock;          /* Serialises submissions                */
  mutex_t memlock;       /* Protects the handle table             */
  struct rk3576_rknpu_memobj_s mem[CONFIG_RK3576_RKNPU_MAX_MEMS];
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_rknpu_getreg(struct rk3576_rknpu_core_s *core,
                                    unsigned int offset);
static void rk3576_rknpu_putreg(struct rk3576_rknpu_core_s *core,
                                unsigned int offset, uint32_t value);
static uintptr_t rk3576_rknpu_mmu_base(struct rk3576_rknpu_core_s *core,
                                       int bank);

static int rk3576_rknpu_clk_init(void);
static int rk3576_rknpu_power_on(void);
static void rk3576_rknpu_rst_set(uint32_t id, bool on);
static int rk3576_rknpu_mmu_bypass(struct rk3576_rknpu_core_s *core);
static int rk3576_rknpu_interrupt(int irq, void *context, void *arg);

static const struct rk3576_rknpu_hw_task_s *
rk3576_rknpu_first_task(const struct rk3576_rknpu_task_s *task);
static int rk3576_rknpu_check(const struct rk3576_rknpu_task_s *task);
static void rk3576_rknpu_kick(struct rk3576_rknpu_core_s *core,
                              const struct rk3576_rknpu_task_s *task,
                              const struct rk3576_rknpu_hw_task_s *first);
static int rk3576_rknpu_wait(struct rk3576_rknpu_core_s *core,
                             uint32_t timeout_ms);

static struct rk3576_rknpu_memobj_s *rk3576_rknpu_mem_lookup(uint32_t handle);
static int rk3576_rknpu_mem_create(struct rk3576_rknpu_mem_s *arg);
static int rk3576_rknpu_mem_map(struct rk3576_rknpu_mem_s *arg);
static int rk3576_rknpu_mem_destroy(struct rk3576_rknpu_mem_s *arg);
static int rk3576_rknpu_mem_sync_ioctl(struct rk3576_rknpu_sync_s *arg);
static int rk3576_rknpu_action(struct rk3576_rknpu_action_s *arg);

static int rk3576_rknpu_fops_open(struct file *filep);
static int rk3576_rknpu_fops_close(struct file *filep);
static int rk3576_rknpu_fops_ioctl(struct file *filep, int cmd,
                                   unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rk3576_rknpu_coredesc_s
    g_rknpu_desc[RK3576_RKNPU_NCORES] = {
      {
          .base = RK3576_RKNPU0_ADDR,
          .irq = RK3576_IRQ_RKNN_CORE0,
          .pd = RK3576_PD_NPU0,
      },
      {
          .base = RK3576_RKNPU1_ADDR,
          .irq = RK3576_IRQ_RKNN_CORE1,
          .pd = RK3576_PD_NPU1,
      },
    };

/* Clocks the NPU needs, in dependency order: the DSU clock first because
 * it is the parent of every aclk below it.  Index 0 is kept as the DVFS
 * handle.
 */

static const char *const g_rknpu_clocks[] = {
  "clk_rknn_dsu0_en",   /* Core / DSU clock, parent of the aclks   */
  "hclk_rknn_root_en",  /* AHB register interface root             */
  "aclk_rknn0_en",      /* Core0 AXI                               */
  "aclk_rknn1_en",      /* Core1 AXI                               */
  "aclk_rknn_cbuf_en",  /* Shared convolution buffer, AXI          */
  "hclk_rknn_cbuf_en",  /* Shared convolution buffer, AHB          */
  "pclk_nputop_root_en" /* NPU top APB (timers, watchdog, GRF)     */
};

/* The four CRU soft resets the device tree lists for npu@27700000. */

static const uint32_t g_rknpu_resets[] = { RK3576_RKNPU_RST_A0,
                                           RK3576_RKNPU_RST_A1,
                                           RK3576_RKNPU_RST_A_CBUF,
                                           RK3576_RKNPU_RST_H_CBUF };

static struct rk3576_rknpu_dev_s g_rknpu;

static const struct file_operations g_rk3576_rknpu_fops = {
  .open = rk3576_rknpu_fops_open,
  .close = rk3576_rknpu_fops_close,
  .ioctl = rk3576_rknpu_fops_ioctl,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rknpu_getreg / rk3576_rknpu_putreg
 *
 * Description:
 *   Access one register of a core's window.
 *
 ****************************************************************************/

static uint32_t rk3576_rknpu_getreg(struct rk3576_rknpu_core_s *core,
                                    unsigned int offset)
{
  return getreg32(core->desc->base + offset);
}

static void rk3576_rknpu_putreg(struct rk3576_rknpu_core_s *core,
                                unsigned int offset, uint32_t value)
{
  putreg32(value, core->desc->base + offset);
}

/****************************************************************************
 * Name: rk3576_rknpu_mmu_base
 *
 * Description:
 *   Base address of one of the two IOMMU register banks of a core.
 *
 ****************************************************************************/

static uintptr_t rk3576_rknpu_mmu_base(struct rk3576_rknpu_core_s *core,
                                       int bank)
{
  return core->desc->base +
         (bank == 0 ? RK3576_RKNPU_MMU0_OFFSET : RK3576_RKNPU_MMU1_OFFSET);
}

/****************************************************************************
 * Name: rk3576_rknpu_clk_init
 *
 * Description:
 *   Enable every clock the NPU needs.  All clock handling for this driver
 *   lives in this one function, so a change to the CLK API only has to be
 *   applied here.  g_rknpu.coreclk is left pointing at the DSU clock so
 *   that the FREQ actions can query and retune it.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_rknpu_clk_init(void)
{
  struct clk_s *clk;
  unsigned int i;
  int ret;

  for (i = 0; i < nitems(g_rknpu_clocks); i++)
    {
      clk = clk_get(g_rknpu_clocks[i]);
      if (clk == NULL)
        {
          _err("ERROR: RKNPU: failed to get %s\n", g_rknpu_clocks[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          _err("ERROR: RKNPU: failed to enable %s: %d\n", g_rknpu_clocks[i],
               ret);
          return ret;
        }

      if (i == 0)
        {
          g_rknpu.coreclk = clk;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_power_on
 *
 * Description:
 *   Bring up the NPU power domains.  rk3576_pd_on() walks the parent
 *   chain (NPU0/NPU1 -> NPUTOP -> NPU), but the whole chain is requested
 *   explicitly so that a failure names the domain that refused.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_rknpu_power_on(void)
{
  static const int g_rootpd[] = {
    RK3576_PD_NPU,   /* VD_NPU voltage domain                 */
    RK3576_PD_NPUTOP /* RKNN_TOP, shared buffer, BIU, MCU     */
  };

  unsigned int i;
  int ret;

  for (i = 0; i < nitems(g_rootpd); i++)
    {
      ret = rk3576_pd_on(g_rootpd[i]);
      if (ret < 0)
        {
          _err("ERROR: RKNPU: power domain %s failed: %d\n",
               rk3576_pd_name(g_rootpd[i]), ret);
          return ret;
        }
    }

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      ret = rk3576_pd_on(g_rknpu_desc[i].pd);
      if (ret < 0)
        {
          _err("ERROR: RKNPU: power domain %s failed: %d\n",
               rk3576_pd_name(g_rknpu_desc[i].pd), ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_rst_set
 *
 * Description:
 *   Assert or release one CRU soft reset.  The SOFTRST_CON registers are
 *   hiword-mask: bits [31:16] select which of bits [15:0] the write
 *   applies to, so neighbouring resets are never disturbed.
 *
 * Input Parameters:
 *   id - Rockchip flat reset identifier, RK3576_RKNPU_RST_*
 *   on - true to hold the block in reset, false to release it
 *
 ****************************************************************************/

static void rk3576_rknpu_rst_set(uint32_t id, bool on)
{
  uintptr_t reg =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_RKNPU_RST_BANK(id));
  uint32_t bit = RK3576_RKNPU_RST_BIT(id);

  putreg32((UINT32_C(1) << (bit + 16)) | (on ? (UINT32_C(1) << bit) : 0), reg);
}

/****************************************************************************
 * Name: rk3576_rknpu_mmu_bypass
 *
 * Description:
 *   Put both IOMMU banks of a core into bypass: paging off, directory
 *   table pointer cleared, fault reporting left enabled so that a stray
 *   access is loud instead of silent.
 *
 *   TODO: implement real page-table management (a two-level Rockchip
 *   IOMMU directory) so that scattered user buffers can be used instead
 *   of physically contiguous ones.  Until then every buffer handed to the
 *   NPU has to come from rk3576_dma_alloc().
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if a bank refuses to leave paging mode.
 *
 ****************************************************************************/

static int rk3576_rknpu_mmu_bypass(struct rk3576_rknpu_core_s *core)
{
  uintptr_t mmu;
  unsigned int i;
  int bank;

  for (bank = 0; bank < RK3576_RKNPU_MMU_NBANKS; bank++)
    {
      mmu = rk3576_rknpu_mmu_base(core, bank);

      /* Acknowledge whatever the boot loader left pending, then ask the
       * bank to stop translating.
       */

      putreg32(RK3576_RKNPU_MMU_IRQ_ALL, mmu + RK3576_RKNPU_MMU_INT_CLEAR);
      putreg32(RK3576_RKNPU_MMU_CMD_DISABLE_PAGING,
               mmu + RK3576_RKNPU_MMU_COMMAND);

      for (i = 0; i < RK3576_RKNPU_MMU_RETRIES; i++)
        {
          if ((getreg32(mmu + RK3576_RKNPU_MMU_STATUS) &
               RK3576_RKNPU_MMU_ST_PAGING_ENABLED) == 0)
            {
              break;
            }

          up_udelay(RK3576_RKNPU_MMU_DELAY_US);
        }

      if (i == RK3576_RKNPU_MMU_RETRIES)
        {
          _err("ERROR: RKNPU: MMU bank %d at %08lx stuck in paging mode\n",
               bank, (unsigned long)mmu);
          return -ETIMEDOUT;
        }

      /* No directory table while in bypass, but keep page-fault and bus
       * error reporting on: in bypass they must never fire, so if one
       * does it is a genuine bug worth an interrupt.
       */

      putreg32(0, mmu + RK3576_RKNPU_MMU_DTE_ADDR);
      putreg32(RK3576_RKNPU_MMU_IRQ_ALL, mmu + RK3576_RKNPU_MMU_INT_MASK);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_interrupt
 *
 * Description:
 *   Completion / error handler for one core.  The core and its IOMMU
 *   share a GIC INTID, so both status registers are examined.  The raw
 *   status is latched for the submitter, everything pending is
 *   acknowledged and the waiter is released exactly once.
 *
 ****************************************************************************/

static int rk3576_rknpu_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_rknpu_core_s *core = (struct rk3576_rknpu_core_s *)arg;
  uint32_t status;
  uint32_t mmustatus;
  uintptr_t mmu;
  bool fault = false;
  int bank;

  UNUSED(irq);
  UNUSED(context);

  /* IOMMU first: a page fault stalls the core, so it has to be
   * acknowledged before the core status is meaningful.
   */

  for (bank = 0; bank < RK3576_RKNPU_MMU_NBANKS; bank++)
    {
      mmu = rk3576_rknpu_mmu_base(core, bank);
      mmustatus = getreg32(mmu + RK3576_RKNPU_MMU_INT_STATUS);
      if (mmustatus == 0)
        {
          continue;
        }

      _err("ERROR: RKNPU: MMU bank %d fault %08" PRIx32 " at %08" PRIx32 "\n",
           bank, mmustatus, getreg32(mmu + RK3576_RKNPU_MMU_PAGE_FAULT_ADDR));

      putreg32(mmustatus, mmu + RK3576_RKNPU_MMU_INT_CLEAR);
      putreg32(RK3576_RKNPU_MMU_CMD_PAGE_FAULT_DONE,
               mmu + RK3576_RKNPU_MMU_COMMAND);
      fault = true;
    }

  status = rk3576_rknpu_getreg(core, RK3576_RKNPU_INT_RAW_STATUS);
  if (status != 0)
    {
      rk3576_rknpu_putreg(core, RK3576_RKNPU_INT_CLEAR, status);
      core->intstatus |= status;
    }

  if (fault)
    {
      core->mmufault = true;
    }

  if ((status != 0 || fault) && !core->done)
    {
      core->done = true;
      nxsem_post(&core->donesem);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_first_task
 *
 * Description:
 *   Resolve the address of the first hardware task descriptor of a
 *   submission so that the driver can read the enable / interrupt masks
 *   and the command stream pointer out of it.
 *
 *   The NPU is fed physical addresses, and NuttX runs flat here (virtual
 *   equals physical for the DMA heap, see rk3576_dma_alloc.h), so the DMA
 *   address doubles as a CPU pointer.
 *
 *   TODO: revisit when an MMU-backed address environment is introduced -
 *   the submission ABI would then have to carry the CPU address as well.
 *
 ****************************************************************************/

static const struct rk3576_rknpu_hw_task_s *
rk3576_rknpu_first_task(const struct rk3576_rknpu_task_s *task)
{
  uintptr_t base = (uintptr_t)task->task_addr;

  return (
      const struct rk3576_rknpu_hw_task_s *)(base +
                                             task->task_start *
                                                 RK3576_RKNPU_HW_TASK_BYTES);
}

/****************************************************************************
 * Name: rk3576_rknpu_check
 *
 * Description:
 *   Validate a submission descriptor before any register is touched.
 *
 * Returned Value:
 *   OK when the request is sane; -EINVAL otherwise.
 *
 ****************************************************************************/

static int rk3576_rknpu_check(const struct rk3576_rknpu_task_s *task)
{
  uint64_t last;

  if ((task->core_mask & ~(uint32_t)RK3576_RKNPU_CORE_MASK_ALL) != 0)
    {
      _err("ERROR: RKNPU: bad core mask %" PRIx32 "\n", task->core_mask);
      return -EINVAL;
    }

  if (task->task_number == 0 || task->task_number > RK3576_RKNPU_PC_TASK_MAX)
    {
      _err("ERROR: RKNPU: bad task count %" PRIu32 "\n", task->task_number);
      return -EINVAL;
    }

  if (task->task_addr == 0 ||
      (task->task_addr & (RK3576_RKNPU_TASK_ALIGN - 1)) != 0)
    {
      _err("ERROR: RKNPU: task array at %" PRIx64 " is not usable\n",
           task->task_addr);
      return -EINVAL;
    }

  /* The whole descriptor array has to sit below the 4GB line, because
   * PC_DMA_BASE_ADDR is a 32-bit register.
   */

  last = task->task_addr + ((uint64_t)task->task_start + task->task_number) *
                               RK3576_RKNPU_HW_TASK_BYTES;

  if (last > RK3576_RKNPU_DMA_LIMIT)
    {
      _err("ERROR: RKNPU: task array crosses the 4GB line\n");
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_kick
 *
 * Description:
 *   Program one core with a task list and start its PC engine.  The
 *   sequence is: acknowledge stale interrupts, arm the sources the task
 *   asks for, enable the sub-engines it needs, publish the register
 *   command stream and the descriptor array, then pulse PC_OP_EN.
 *
 ****************************************************************************/

static void rk3576_rknpu_kick(struct rk3576_rknpu_core_s *core,
                              const struct rk3576_rknpu_task_s *task,
                              const struct rk3576_rknpu_hw_task_s *first)
{
  uint32_t amount;
  uint32_t dmabase;

  core->intstatus = 0;
  core->mmufault = false;
  core->done = false;

  /* Drain a stale post left by a previous, timed-out submission. */

  while (nxsem_trywait(&core->donesem) == OK)
    {
    }

  rk3576_rknpu_putreg(core, RK3576_RKNPU_INT_CLEAR, RK3576_RKNPU_INT_ALL);
  rk3576_rknpu_putreg(core, RK3576_RKNPU_INT_MASK, first->int_mask);
  rk3576_rknpu_putreg(core, RK3576_RKNPU_ENABLE_MASK, first->enable_mask);

  /* Zero the traffic counters so RK3576_RKNPU_ACT_GET_TOTAL_RW reports
   * this job and not the sum of everything since boot.
   */

  rk3576_rknpu_putreg(core, RK3576_RKNPU_CLR_ALL_RW_AMOUNT, 1);

  /* Register command stream of the first task.  PC_DATA_AMOUNT wants the
   * number of fetch bursts minus one, not a word count.
   */

  rk3576_rknpu_putreg(core, RK3576_RKNPU_PC_DATA_ADDR,
                      (uint32_t)first->regcmd_addr);

  amount = (first->regcfg_amount + RK3576_RKNPU_PC_DATA_EXTRA_AMOUNT +
            RK3576_RKNPU_PC_DATA_AMOUNT_SCALE - 1) /
           RK3576_RKNPU_PC_DATA_AMOUNT_SCALE;

  rk3576_rknpu_putreg(core, RK3576_RKNPU_PC_DATA_AMOUNT, amount - 1);

  /* How many descriptors the engine has to walk, and where they are. */

  rk3576_rknpu_putreg(
      core, RK3576_RKNPU_PC_TASK_CONTROL,
      RK3576_RKNPU_PC_TASK_MODE |
          (task->task_number << RK3576_RKNPU_PC_TASK_NUMBER_SHIFT));

  dmabase = (uint32_t)(task->task_addr + (uint64_t)task->task_start *
                                             RK3576_RKNPU_HW_TASK_BYTES);

  rk3576_rknpu_putreg(core, RK3576_RKNPU_PC_DMA_BASE_ADDR, dmabase);

  /* Go.  The start bit is a pulse, not a level. */

  rk3576_rknpu_putreg(core, RK3576_RKNPU_PC_OP_EN,
                      RK3576_RKNPU_PC_OP_EN_START);
  rk3576_rknpu_putreg(core, RK3576_RKNPU_PC_OP_EN, 0);
}

/****************************************************************************
 * Name: rk3576_rknpu_wait
 *
 * Description:
 *   Block until the core signals completion or the deadline expires.
 *
 * Returned Value:
 *   OK on a clean completion, -ETIMEDOUT when the core went silent,
 *   -EIO when the IOMMU faulted during the job.
 *
 ****************************************************************************/

static int rk3576_rknpu_wait(struct rk3576_rknpu_core_s *core,
                             uint32_t timeout_ms)
{
  int ret;

  ret = nxsem_tickwait_uninterruptible(&core->donesem, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      _err("ERROR: RKNPU: core at %08lx timed out, pc status %08" PRIx32
           " raw int %08" PRIx32 "\n",
           (unsigned long)core->desc->base,
           rk3576_rknpu_getreg(core, RK3576_RKNPU_PC_TASK_STATUS),
           rk3576_rknpu_getreg(core, RK3576_RKNPU_INT_RAW_STATUS));
      return -ETIMEDOUT;
    }

  if (core->mmufault)
    {
      _err("ERROR: RKNPU: core at %08lx hit an IOMMU fault\n",
           (unsigned long)core->desc->base);
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_lookup
 *
 * Description:
 *   Translate a handle into its table entry.  Must be called with
 *   g_rknpu.memlock held.
 *
 * Returned Value:
 *   The entry, or NULL when the handle is out of range or free.
 *
 ****************************************************************************/

static struct rk3576_rknpu_memobj_s *rk3576_rknpu_mem_lookup(uint32_t handle)
{
  struct rk3576_rknpu_memobj_s *obj;

  if (handle == 0 || handle > (uint32_t)CONFIG_RK3576_RKNPU_MAX_MEMS)
    {
      return NULL;
    }

  obj = &g_rknpu.mem[handle - 1];
  return obj->va != NULL ? obj : NULL;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_create
 *
 * Description:
 *   Back RK3576_RKNPUIOC_MEM_CREATE: allocate a DMA-safe buffer and
 *   publish it under a fresh handle.
 *
 ****************************************************************************/

static int rk3576_rknpu_mem_create(struct rk3576_rknpu_mem_s *arg)
{
  uintptr_t pa;
  void *va;
  int i;
  int ret;

  if (arg->flags != 0 || arg->size == 0 || arg->size >= RK3576_RKNPU_DMA_LIMIT)
    {
      return -EINVAL;
    }

  va = rk3576_dma_alloc((size_t)arg->size);
  if (va == NULL)
    {
      _err("ERROR: RKNPU: DMA heap exhausted for %" PRIu64 " bytes\n",
           arg->size);
      return -ENOMEM;
    }

  pa = up_addrenv_va_to_pa(va);
  if ((uint64_t)pa + arg->size > RK3576_RKNPU_DMA_LIMIT)
    {
      _err("ERROR: RKNPU: allocation at %08lx is out of DMA reach\n",
           (unsigned long)pa);
      rk3576_dma_free(va, (size_t)arg->size);
      return -EFAULT;
    }

  ret = nxmutex_lock(&g_rknpu.memlock);
  if (ret < 0)
    {
      rk3576_dma_free(va, (size_t)arg->size);
      return ret;
    }

  for (i = 0; i < CONFIG_RK3576_RKNPU_MAX_MEMS; i++)
    {
      if (g_rknpu.mem[i].va == NULL)
        {
          break;
        }
    }

  if (i == CONFIG_RK3576_RKNPU_MAX_MEMS)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      rk3576_dma_free(va, (size_t)arg->size);
      _err("ERROR: RKNPU: memory handle table full\n");
      return -EMFILE;
    }

  g_rknpu.mem[i].va = va;
  g_rknpu.mem[i].size = (size_t)arg->size;
  nxmutex_unlock(&g_rknpu.memlock);

  arg->handle = (uint32_t)i + 1;
  arg->va = (uint64_t)(uintptr_t)va;
  arg->dma_addr = (uint64_t)pa;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_map
 *
 * Description:
 *   Back RK3576_RKNPUIOC_MEM_MAP.  NuttX runs a flat address space here,
 *   so there is nothing to map: the call resolves a handle back to the
 *   addresses MEM_CREATE already handed out.  It exists so that code
 *   ported from the Linux rknpu ABI, which has to mmap() its buffers,
 *   keeps the same call sequence.
 *
 ****************************************************************************/

static int rk3576_rknpu_mem_map(struct rk3576_rknpu_mem_s *arg)
{
  struct rk3576_rknpu_memobj_s *obj;
  int ret;

  ret = nxmutex_lock(&g_rknpu.memlock);
  if (ret < 0)
    {
      return ret;
    }

  obj = rk3576_rknpu_mem_lookup(arg->handle);
  if (obj == NULL)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      return -ENOENT;
    }

  arg->size = obj->size;
  arg->va = (uint64_t)(uintptr_t)obj->va;
  arg->dma_addr = (uint64_t)up_addrenv_va_to_pa(obj->va);
  nxmutex_unlock(&g_rknpu.memlock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_destroy
 *
 * Description:
 *   Back RK3576_RKNPUIOC_MEM_DESTROY: release a handle and its buffer.
 *
 ****************************************************************************/

static int rk3576_rknpu_mem_destroy(struct rk3576_rknpu_mem_s *arg)
{
  struct rk3576_rknpu_memobj_s *obj;
  size_t size;
  void *va;
  int ret;

  ret = nxmutex_lock(&g_rknpu.memlock);
  if (ret < 0)
    {
      return ret;
    }

  obj = rk3576_rknpu_mem_lookup(arg->handle);
  if (obj == NULL)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      return -ENOENT;
    }

  va = obj->va;
  size = obj->size;
  obj->va = NULL;
  obj->size = 0;
  nxmutex_unlock(&g_rknpu.memlock);

  rk3576_dma_free(va, size);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_sync_ioctl
 *
 * Description:
 *   Back RK3576_RKNPUIOC_MEM_SYNC: cache maintenance on a sub-range of a
 *   handle.
 *
 ****************************************************************************/

static int rk3576_rknpu_mem_sync_ioctl(struct rk3576_rknpu_sync_s *arg)
{
  struct rk3576_rknpu_memobj_s *obj;
  uintptr_t start;
  size_t size;
  int ret;

  if ((arg->flags & ~(uint32_t)(RK3576_RKNPU_SYNC_TO_DEVICE |
                                RK3576_RKNPU_SYNC_FROM_DEVICE)) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_rknpu.memlock);
  if (ret < 0)
    {
      return ret;
    }

  obj = rk3576_rknpu_mem_lookup(arg->handle);
  if (obj == NULL)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      return -ENOENT;
    }

  if (arg->offset > obj->size)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      return -EINVAL;
    }

  size = arg->size != 0 ? (size_t)arg->size : obj->size - arg->offset;
  if (size > obj->size - arg->offset)
    {
      nxmutex_unlock(&g_rknpu.memlock);
      return -EINVAL;
    }

  start = (uintptr_t)obj->va + (uintptr_t)arg->offset;
  nxmutex_unlock(&g_rknpu.memlock);

  rk3576_rknpu_mem_sync((void *)start, size, arg->flags);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_action
 *
 * Description:
 *   Back RK3576_RKNPUIOC_ACTION, the query / control door of the driver.
 *
 ****************************************************************************/

static int rk3576_rknpu_action(struct rk3576_rknpu_action_s *arg)
{
  struct rk3576_rknpu_core_s *core = &g_rknpu.core[0];
  int ret = OK;

  switch (arg->flags)
    {
      case RK3576_RKNPU_ACT_GET_HW_VERSION:
        arg->value = g_rknpu.version;
        break;

      case RK3576_RKNPU_ACT_GET_DRV_VERSION:
        arg->value = RK3576_RKNPU_DRV_VERSION;
        break;

      case RK3576_RKNPU_ACT_GET_FREQ:
        arg->value = clk_get_rate(g_rknpu.coreclk);
        break;

      case RK3576_RKNPU_ACT_SET_FREQ:
        if (arg->value == 0)
          {
            ret = -EINVAL;
            break;
          }

        ret = clk_set_rate(g_rknpu.coreclk, arg->value);
        if (ret >= 0)
          {
            arg->value = clk_get_rate(g_rknpu.coreclk);
            ret = OK;
          }
        break;

      case RK3576_RKNPU_ACT_RESET:
        ret = rk3576_rknpu_reset();
        break;

      case RK3576_RKNPU_ACT_GET_IOMMU_EN:

        /* Always zero for now: both IOMMUs run in bypass. */

        arg->value = 0;
        break;

      case RK3576_RKNPU_ACT_GET_TOTAL_RW:
        arg->value = rk3576_rknpu_getreg(core, RK3576_RKNPU_DT_WR_AMOUNT) +
                     rk3576_rknpu_getreg(core, RK3576_RKNPU_DT_RD_AMOUNT) +
                     rk3576_rknpu_getreg(core, RK3576_RKNPU_WT_RD_AMOUNT);
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_rknpu_fops_open / rk3576_rknpu_fops_close
 ****************************************************************************/

static int rk3576_rknpu_fops_open(struct file *filep)
{
  UNUSED(filep);
  return OK;
}

static int rk3576_rknpu_fops_close(struct file *filep)
{
  UNUSED(filep);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_fops_ioctl
 *
 * Description:
 *   Character-device entry point for the RK3576_RKNPUIOC_* commands.
 *
 ****************************************************************************/

static int rk3576_rknpu_fops_ioctl(struct file *filep, int cmd,
                                   unsigned long arg)
{
  UNUSED(filep);

  if (arg == 0)
    {
      return -EINVAL;
    }

  switch (cmd)
    {
      case RK3576_RKNPUIOC_ACTION:
        return rk3576_rknpu_action((struct rk3576_rknpu_action_s *)arg);

      case RK3576_RKNPUIOC_SUBMIT:
        return rk3576_rknpu_submit((struct rk3576_rknpu_task_s *)arg);

      case RK3576_RKNPUIOC_MEM_CREATE:
        return rk3576_rknpu_mem_create((struct rk3576_rknpu_mem_s *)arg);

      case RK3576_RKNPUIOC_MEM_MAP:
        return rk3576_rknpu_mem_map((struct rk3576_rknpu_mem_s *)arg);

      case RK3576_RKNPUIOC_MEM_DESTROY:
        return rk3576_rknpu_mem_destroy((struct rk3576_rknpu_mem_s *)arg);

      case RK3576_RKNPUIOC_MEM_SYNC:
        return rk3576_rknpu_mem_sync_ioctl((struct rk3576_rknpu_sync_s *)arg);

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rknpu_initialize
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

int rk3576_rknpu_initialize(void)
{
  struct rk3576_rknpu_core_s *core;
  int attached = 0;
  int ret;
  int i;

  if (g_rknpu.initialized)
    {
      return OK;
    }

  /* The descriptor array layout is an ABI with the hardware; a compiler
   * that pads it differently would corrupt every job.
   */

  DEBUGASSERT(RK3576_RKNPU_HW_TASK_BYTES == RK3576_RKNPU_HW_TASK_ABI_BYTES);
  DEBUGASSERT(RK3576_RKNPU_NCORES_ABI == RK3576_RKNPU_NCORES);

  /* Power first: a gated domain reads back as zeroes and swallows
   * writes, so nothing below would have any effect.
   */

  ret = rk3576_rknpu_power_on();
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_rknpu_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      core = &g_rknpu.core[i];
      core->desc = &g_rknpu_desc[i];
      nxsem_init(&core->donesem, 0, 0);
    }

  nxmutex_init(&g_rknpu.lock);
  nxmutex_init(&g_rknpu.memlock);

  /* Soft reset both cores and configure the IOMMUs for bypass. */

  ret = rk3576_rknpu_reset();
  if (ret < 0)
    {
      goto err_destroy;
    }

  g_rknpu.version =
      rk3576_rknpu_getreg(&g_rknpu.core[0], RK3576_RKNPU_VERSION);
  g_rknpu.versionnum =
      rk3576_rknpu_getreg(&g_rknpu.core[0], RK3576_RKNPU_VERSION_NUM);

  if (g_rknpu.version == 0 || g_rknpu.version == UINT32_MAX)
    {
      /* All zeroes means the domain is still gated, all ones means the
       * window is not decoded.  Either way there is no point continuing.
       */

      _err("ERROR: RKNPU: implausible VERSION %08" PRIx32 ", aborting\n",
           g_rknpu.version);
      ret = -ENODEV;
      goto err_destroy;
    }

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      core = &g_rknpu.core[i];

      ret = irq_attach(core->desc->irq, rk3576_rknpu_interrupt, core);
      if (ret < 0)
        {
          _err("ERROR: RKNPU: irq_attach(%d) failed: %d\n", core->desc->irq,
               ret);
          goto err_detach;
        }

      up_enable_irq(core->desc->irq);
      attached++;
    }

  ret = register_driver(RK3576_RKNPU_DEVPATH, &g_rk3576_rknpu_fops, 0666,
                        &g_rknpu);
  if (ret < 0)
    {
      _err("ERROR: RKNPU: register_driver(%s) failed: %d\n",
           RK3576_RKNPU_DEVPATH, ret);
      goto err_detach;
    }

  g_rknpu.initialized = true;

  _info("RKNPU: %d cores, version %08" PRIx32 ".%08" PRIx32
        ", core clock %" PRIu32 " Hz, IOMMU bypassed\n",
        RK3576_RKNPU_NCORES, g_rknpu.version, g_rknpu.versionnum,
        clk_get_rate(g_rknpu.coreclk));

  return OK;

err_detach:
  for (i = 0; i < attached; i++)
    {
      up_disable_irq(g_rknpu.core[i].desc->irq);
      irq_detach(g_rknpu.core[i].desc->irq);
    }

err_destroy:
  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      nxsem_destroy(&g_rknpu.core[i].donesem);
    }

  nxmutex_destroy(&g_rknpu.memlock);
  nxmutex_destroy(&g_rknpu.lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_rknpu_reset
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

int rk3576_rknpu_reset(void)
{
  unsigned int i;
  int ret;

  for (i = 0; i < nitems(g_rknpu_resets); i++)
    {
      rk3576_rknpu_rst_set(g_rknpu_resets[i], true);
    }

  up_udelay(RK3576_RKNPU_RESET_DELAY_US);

  for (i = 0; i < nitems(g_rknpu_resets); i++)
    {
      rk3576_rknpu_rst_set(g_rknpu_resets[i], false);
    }

  up_udelay(RK3576_RKNPU_RESET_DELAY_US);

  /* A reset clears the IOMMU configuration, so re-apply it, and make
   * sure no interrupt survived the pulse.
   */

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      ret = rk3576_rknpu_mmu_bypass(&g_rknpu.core[i]);
      if (ret < 0)
        {
          return ret;
        }

      rk3576_rknpu_putreg(&g_rknpu.core[i], RK3576_RKNPU_INT_MASK, 0);
      rk3576_rknpu_putreg(&g_rknpu.core[i], RK3576_RKNPU_INT_CLEAR,
                          RK3576_RKNPU_INT_ALL);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknpu_submit
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

int rk3576_rknpu_submit(struct rk3576_rknpu_task_s *task)
{
  const struct rk3576_rknpu_hw_task_s *first;
  struct rk3576_rknpu_core_s *core;
  uint32_t core_mask;
  uint32_t timeout;
  uintptr_t taskva;
  size_t tasklen;
  int result = OK;
  int ret;
  int i;

  if (task == NULL)
    {
      return -EINVAL;
    }

  if (!g_rknpu.initialized)
    {
      return -ENODEV;
    }

  ret = rk3576_rknpu_check(task);
  if (ret < 0)
    {
      return ret;
    }

  core_mask = task->core_mask;
  if (core_mask == RK3576_RKNPU_CORE_AUTO)
    {
      core_mask = RK3576_RKNPU_CORE0;
    }

  timeout = task->timeout_ms != 0 ? task->timeout_ms
                                  : RK3576_RKNPU_DEFAULT_TIMEOUT_MS;

  first = rk3576_rknpu_first_task(task);
  taskva = (uintptr_t)task->task_addr +
           task->task_start * RK3576_RKNPU_HW_TASK_BYTES;
  tasklen = task->task_number * RK3576_RKNPU_HW_TASK_BYTES;

  memset(task->int_status, 0, sizeof(task->int_status));
  memset(task->task_counter, 0, sizeof(task->task_counter));

  ret = nxmutex_lock(&g_rknpu.lock);
  if (ret < 0)
    {
      return ret;
    }

  /* The PC engine fetches the descriptors itself, so they have to be out
   * of the D-cache before the engine is started.  The register command
   * streams they point at belong to the caller, which is expected to
   * have synced them already.
   */

  up_clean_dcache(taskva, taskva + tasklen);

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      if ((core_mask & (UINT32_C(1) << i)) != 0)
        {
          rk3576_rknpu_kick(&g_rknpu.core[i], task, first);
        }
    }

  for (i = 0; i < RK3576_RKNPU_NCORES; i++)
    {
      if ((core_mask & (UINT32_C(1) << i)) == 0)
        {
          continue;
        }

      core = &g_rknpu.core[i];

      ret = rk3576_rknpu_wait(core, timeout);
      if (ret < 0 && result == OK)
        {
          result = ret;
        }

      task->int_status[i] = core->intstatus;
      task->task_counter[i] =
          rk3576_rknpu_getreg(core, RK3576_RKNPU_PC_TASK_STATUS);

      /* Stop the core from raising anything else until the next job. */

      rk3576_rknpu_putreg(core, RK3576_RKNPU_INT_MASK, 0);
    }

  if (result < 0)
    {
      /* A wedged core only comes back through the CRU. */

      if (rk3576_rknpu_reset() < 0)
        {
          _err("ERROR: RKNPU: recovery reset failed\n");
        }
    }

  nxmutex_unlock(&g_rknpu.lock);
  return result;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_alloc
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

void *rk3576_rknpu_mem_alloc(size_t size, uintptr_t *dma_addr)
{
  uintptr_t pa;
  void *va;

  if (size == 0)
    {
      return NULL;
    }

  va = rk3576_dma_alloc(size);
  if (va == NULL)
    {
      return NULL;
    }

  pa = up_addrenv_va_to_pa(va);
  if ((uint64_t)pa + size > RK3576_RKNPU_DMA_LIMIT)
    {
      _err("ERROR: RKNPU: allocation at %08lx is out of DMA reach\n",
           (unsigned long)pa);
      rk3576_dma_free(va, size);
      return NULL;
    }

  if (dma_addr != NULL)
    {
      *dma_addr = pa;
    }

  return va;
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_free
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

void rk3576_rknpu_mem_free(void *va, size_t size)
{
  if (va != NULL && size != 0)
    {
      rk3576_dma_free(va, size);
    }
}

/****************************************************************************
 * Name: rk3576_rknpu_mem_sync
 *
 * Description:
 *   See rk3576_rknpu.h.
 *
 ****************************************************************************/

void rk3576_rknpu_mem_sync(void *va, size_t size, uint32_t flags)
{
  uintptr_t start = (uintptr_t)va;
  uintptr_t end = start + size;

  if (va == NULL || size == 0)
    {
      return;
    }

  if ((flags & RK3576_RKNPU_SYNC_TO_DEVICE) != 0)
    {
      up_clean_dcache(start, end);
    }

  if ((flags & RK3576_RKNPU_SYNC_FROM_DEVICE) != 0)
    {
      up_invalidate_dcache(start, end);
    }
}

#endif /* CONFIG_RK3576_RKNPU */
