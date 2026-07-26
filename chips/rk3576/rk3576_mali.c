/****************************************************************************
 * chips/rk3576/rk3576_mali.c
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
 * RK3576 ARM Mali-G52 MC3 (Bifrost) Job Manager driver.
 *
 * This is the "openvela drives the GPU directly" path: a minimal command
 * submitter, with no shader compiler and no graphics state tracker.  Job
 * chains are either built by hand (the NULL job self test) or produced
 * offline on a host PC by Mesa/Panfrost and replayed here.
 *
 * Bring-up sequence implemented by rk3576_mali_initialize():
 *
 *   1. Enable the GPU clock tree (one function, rk3576_mali_clk_init()).
 *      The bus clocks have to run before the power domain handshake, since
 *      the PMU waits for the domain BIUs to go idle over those clocks.
 *   2. Power the VD_GPU voltage domain up through the PMU.
 *   3. Soft reset the GPU and wait for GPU_IRQ_RAWSTAT.RESET_COMPLETED.
 *   4. Read the identity and feature registers.  A Mali-G52 reports
 *      product id 0x7002 in GPU_ID[31:16]; anything else means the block
 *      is not actually powered and clocked.
 *   5. Power the L2 slices, then the core stacks, then the shader cores,
 *      then the tiler, polling the matching *_READY register after each.
 *   6. Configure address space 0 as a 1:1 identity mapping.
 *   7. Attach and unmask the GPU, MMU and JOB interrupts.
 *
 * Memory model: the address space is an identity mapping, so a GPU
 * virtual address is a physical address.  Every buffer the GPU touches
 * must therefore be physically contiguous and below the 4GB line, which
 * is exactly what rk3576_dma_alloc() guarantees.  There is no page table
 * and no per-process address space; a full LPAE page table walker would
 * be the next step if isolation is ever needed.
 *
 * Register documentation: the RK3576 TRM defers the GPU register map to
 * the ARM technical reference manual, so hardware/rk3576_mali.h is built
 * from the publicly published Panfrost/Mesa hardware documentation.
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
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_mali.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_mali.h"
#include "rk3576_pd.h"

#ifdef CONFIG_RK3576_MALI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Address space the driver programs and every submission runs through. */

#define RK3576_MALI_AS 0

/* Default job completion timeout.  A recorded frame at 360x360 finishes in
 * well under a millisecond, so a second is a very generous ceiling that
 * still catches a wedged Job Manager quickly.
 */

#define RK3576_MALI_JOB_TIMEOUT_MS 1000

/* Polling budgets for the reset, power and address space handshakes.  All
 * three complete in a few microseconds on working hardware.
 */

#define RK3576_MALI_RESET_RETRIES 10000
#define RK3576_MALI_PWR_RETRIES   20000
#define RK3576_MALI_AS_RETRIES    10000
#define RK3576_MALI_POLL_DELAY_US 1

/* Job chains must start on a cache line so that the clean/invalidate the
 * Job Manager performs on our behalf cannot straddle unrelated data.
 */

#define RK3576_MALI_JOB_ALIGN 64

/* Scratch buffer for the NULL job self test: one job header, rounded up to
 * the DMA allocator granule.
 */

#define RK3576_MALI_NULL_JOB_BYTES 64

/* Job index of the single job in a hand built one-entry chain.  Index 0 is
 * reserved by the hardware as "no job".
 */

#define RK3576_MALI_FIRST_JOB_INDEX 1

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* State of one job slot. */

struct rk3576_mali_slot_s
{
  sem_t donesem;   /* Posted by the job interrupt handler       */
  uint32_t status; /* JS_STATUS latched by the handler          */
  bool failed;     /* Set when the "job failed" bit came up     */
  bool busy;       /* A chain is outstanding on this slot       */
};

/* Driver state.  There is exactly one Mali block on the RK3576. */

struct rk3576_mali_dev_s
{
  uintptr_t base;                 /* Register window base address     */
  struct rk3576_mali_info_s info; /* Identity / feature snapshot      */
  mutex_t lock;                   /* Serialises submissions           */
  struct rk3576_mali_slot_s slot[RK3576_MALI_NSLOTS];
  uint32_t mmu_faultstatus; /* Last MMU fault, for diagnostics  */
  uint64_t mmu_faultaddress;
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_mali_getreg(unsigned int offset);
static void rk3576_mali_putreg(unsigned int offset, uint32_t value);
static uint64_t rk3576_mali_getreg64(unsigned int offset);
static void rk3576_mali_putreg64(unsigned int offset, uint64_t value);

static int rk3576_mali_clk_init(void);
static int rk3576_mali_soft_reset(void);
static void rk3576_mali_read_features(void);
static int rk3576_mali_wait_ready(unsigned int readyreg, uint64_t mask,
                                  const char *what);
static int rk3576_mali_power_on(void);
static int rk3576_mali_as_identity(void);

static int rk3576_mali_job_interrupt(int irq, void *context, void *arg);
static int rk3576_mali_mmu_interrupt(int irq, void *context, void *arg);
static int rk3576_mali_gpu_interrupt(int irq, void *context, void *arg);

static const char *rk3576_mali_status_name(uint32_t status);
static void rk3576_mali_relocate(uint64_t *words, size_t nwords, uint64_t from,
                                 uint64_t size, uint64_t to);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_mali_dev_s g_rk3576_mali = {
  .base = RK3576_MALI_ADDR,
};

/* Clock gates the GPU needs, in the order they are enabled.  aclk_s / aclk_m0
 * are the two AXI ports of the GPU BIU, pclk_gpu_root / _biu / _grf feed the
 * APB side and clk_gpu is the shader core clock.
 */

static const char *g_rk3576_mali_clocks[] = {
  "pclk_gpu_root_en",  "pclk_gpu_biu_en",    "pclk_gpu_grf_en",
  "aclk_s_gpu_biu_en", "aclk_m0_gpu_biu_en", "clk_gpu_src_pre_en",
  "clk_gpu_inner_en",
};

/* The functional clock whose rate is reported through rk3576_mali_info(). */

static const char *g_rk3576_mali_coreclk = "clk_gpu_en";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mali_getreg / rk3576_mali_putreg
 *
 * Description:
 *   Access a 32-bit register in the Mali register window.
 *
 ****************************************************************************/

static uint32_t rk3576_mali_getreg(unsigned int offset)
{
  return getreg32(g_rk3576_mali.base + offset);
}

static void rk3576_mali_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, g_rk3576_mali.base + offset);
}

/****************************************************************************
 * Name: rk3576_mali_getreg64 / rk3576_mali_putreg64
 *
 * Description:
 *   Access one of the LO/HI register pairs.  The Mali register interface
 *   is 32 bits wide, so a 64-bit quantity is always split over two
 *   consecutive registers with the low half first.
 *
 ****************************************************************************/

static uint64_t rk3576_mali_getreg64(unsigned int offset)
{
  uint64_t lo = rk3576_mali_getreg(offset);
  uint64_t hi = rk3576_mali_getreg(offset + 4);

  return lo | (hi << 32);
}

static void rk3576_mali_putreg64(unsigned int offset, uint64_t value)
{
  rk3576_mali_putreg(offset, (uint32_t)value);
  rk3576_mali_putreg(offset + 4, (uint32_t)(value >> 32));
}

/****************************************************************************
 * Name: rk3576_mali_clk_init
 *
 * Description:
 *   Enable every clock the GPU needs.  All clock handling for this driver
 *   lives in this one function, so a change to the CLK API only has to be
 *   applied here.  The core clock rate is read back rather than assumed.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_mali_clk_init(void)
{
  struct clk_s *clk;
  unsigned int i;
  int ret;

  for (i = 0; i < nitems(g_rk3576_mali_clocks); i++)
    {
      clk = clk_get(g_rk3576_mali_clocks[i]);
      if (clk == NULL)
        {
          gerr("ERROR: failed to get %s\n", g_rk3576_mali_clocks[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          gerr("ERROR: failed to enable %s: %d\n", g_rk3576_mali_clocks[i],
               ret);
          return ret;
        }
    }

  clk = clk_get(g_rk3576_mali_coreclk);
  if (clk == NULL)
    {
      gerr("ERROR: failed to get %s\n", g_rk3576_mali_coreclk);
      return -ENODEV;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      gerr("ERROR: failed to enable %s: %d\n", g_rk3576_mali_coreclk, ret);
      return ret;
    }

  g_rk3576_mali.info.coreclk = clk_get_rate(clk);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_soft_reset
 *
 * Description:
 *   Issue GPU_COMMAND.SOFT_RESET and poll GPU_IRQ_RAWSTAT until the
 *   hardware reports the reset as completed.  Interrupts are kept masked
 *   throughout so that the reset can be observed by polling even before
 *   the handlers are attached.
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if the reset never retires.
 *
 ****************************************************************************/

static int rk3576_mali_soft_reset(void)
{
  unsigned int i;

  rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_MASK, 0);
  rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_CLEAR,
                     RK3576_MALI_GPU_IRQ_RESET_COMPLETED);
  rk3576_mali_putreg(RK3576_MALI_GPU_COMMAND, RK3576_MALI_CMD_SOFT_RESET);

  for (i = 0; i < RK3576_MALI_RESET_RETRIES; i++)
    {
      if ((rk3576_mali_getreg(RK3576_MALI_GPU_IRQ_RAWSTAT) &
           RK3576_MALI_GPU_IRQ_RESET_COMPLETED) != 0)
        {
          rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_CLEAR,
                             RK3576_MALI_GPU_IRQ_ALL);
          return OK;
        }

      up_udelay(RK3576_MALI_POLL_DELAY_US);
    }

  gerr("ERROR: GPU soft reset timed out, GPU_ID=%08" PRIx32 "\n",
       rk3576_mali_getreg(RK3576_MALI_GPU_ID));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_mali_read_features
 *
 * Description:
 *   Snapshot the identity and feature registers into the info structure.
 *
 ****************************************************************************/

static void rk3576_mali_read_features(void)
{
  struct rk3576_mali_info_s *info = &g_rk3576_mali.info;

  info->gpu_id = rk3576_mali_getreg(RK3576_MALI_GPU_ID);
  info->product_id = RK3576_MALI_GPU_ID_PRODUCT(info->gpu_id);
  info->l2_features = rk3576_mali_getreg(RK3576_MALI_L2_FEATURES);
  info->tiler_features = rk3576_mali_getreg(RK3576_MALI_TILER_FEATURES);
  info->mem_features = rk3576_mali_getreg(RK3576_MALI_MEM_FEATURES);
  info->mmu_features = rk3576_mali_getreg(RK3576_MALI_MMU_FEATURES);
  info->as_present = rk3576_mali_getreg(RK3576_MALI_AS_PRESENT);
  info->js_present = rk3576_mali_getreg(RK3576_MALI_JS_PRESENT);
  info->shader_present = rk3576_mali_getreg64(RK3576_MALI_SHADER_PRESENT_LO);
  info->tiler_present = rk3576_mali_getreg64(RK3576_MALI_TILER_PRESENT_LO);
  info->l2_present = rk3576_mali_getreg64(RK3576_MALI_L2_PRESENT_LO);
  info->stack_present = rk3576_mali_getreg64(RK3576_MALI_STACK_PRESENT_LO);
}

/****************************************************************************
 * Name: rk3576_mali_wait_ready
 *
 * Description:
 *   Poll one of the *_READY register pairs until every core in mask
 *   reports itself powered.
 *
 * Input Parameters:
 *   readyreg - Offset of the READY_LO register.
 *   mask     - Core bitmask that must come up.
 *   what     - Name of the core group, for the error message.
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if the cores never came up.
 *
 ****************************************************************************/

static int rk3576_mali_wait_ready(unsigned int readyreg, uint64_t mask,
                                  const char *what)
{
  unsigned int i;

  for (i = 0; i < RK3576_MALI_PWR_RETRIES; i++)
    {
      if ((rk3576_mali_getreg64(readyreg) & mask) == mask)
        {
          return OK;
        }

      up_udelay(RK3576_MALI_POLL_DELAY_US);
    }

  gerr("ERROR: %s did not power up, ready=%" PRIx64 " want=%" PRIx64 "\n",
       what, rk3576_mali_getreg64(readyreg), mask);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_mali_power_on
 *
 * Description:
 *   Power the GPU core groups up in the order the hardware requires: L2
 *   slices first, then the Bifrost core stacks, then the shader cores that
 *   sit on those stacks, and finally the tiler.  Each step waits for the
 *   matching READY bitmask before the next one is started.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_mali_power_on(void)
{
  const struct rk3576_mali_info_s *info = &g_rk3576_mali.info;
  int ret;

  rk3576_mali_putreg64(RK3576_MALI_L2_PWRON_LO, info->l2_present);
  ret =
      rk3576_mali_wait_ready(RK3576_MALI_L2_READY_LO, info->l2_present, "L2");
  if (ret < 0)
    {
      return ret;
    }

  /* Bifrost groups the shader cores into "core stacks".  A stack has to be
   * powered before any core on it can be, and STACK_PRESENT reads back
   * zero on the Midgard style layouts that do not have them.
   */

  if (info->stack_present != 0)
    {
      rk3576_mali_putreg64(RK3576_MALI_STACK_PWRON_LO, info->stack_present);
      ret = rk3576_mali_wait_ready(RK3576_MALI_STACK_READY_LO,
                                   info->stack_present, "core stacks");
      if (ret < 0)
        {
          return ret;
        }
    }

  rk3576_mali_putreg64(RK3576_MALI_SHADER_PWRON_LO, info->shader_present);
  ret = rk3576_mali_wait_ready(RK3576_MALI_SHADER_READY_LO,
                               info->shader_present, "shader cores");
  if (ret < 0)
    {
      return ret;
    }

  rk3576_mali_putreg64(RK3576_MALI_TILER_PWRON_LO, info->tiler_present);
  return rk3576_mali_wait_ready(RK3576_MALI_TILER_READY_LO,
                                info->tiler_present, "tiler");
}

/****************************************************************************
 * Name: rk3576_mali_as_identity
 *
 * Description:
 *   Configure address space RK3576_MALI_AS as a 1:1 identity mapping, so
 *   that a GPU virtual address is a physical address and no page table is
 *   needed.
 *
 *   Bifrost implements the AArch64 style MMU interface, where the
 *   addressing mode lives in AS_TRANSCFG and AS_TRANSTAB is ignored.  The
 *   older interface keeps the addressing mode in the low two bits of
 *   AS_TRANSTAB instead.  Both encodings are written: whichever one the
 *   block implements selects identity mapping and the other is a
 *   don't-care, which keeps the sequence correct without having to probe
 *   the MMU generation first.
 *
 *   AS_MEMATTR is programmed with the standard three-slot attribute table
 *   (implementation defined / write-back write-allocate / implementation
 *   defined) that the io-pgtable ARM_MALI_LPAE format publishes.  Slot 0
 *   is the one an identity mapping uses.
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if AS_STATUS never goes idle.
 *
 ****************************************************************************/

static int rk3576_mali_as_identity(void)
{
  unsigned int i;

  /* Wait for any in-flight address space operation to retire first. */

  for (i = 0; i < RK3576_MALI_AS_RETRIES; i++)
    {
      if ((rk3576_mali_getreg(RK3576_MALI_AS_STATUS(RK3576_MALI_AS)) &
           RK3576_MALI_AS_STATUS_ACTIVE) == 0)
        {
          break;
        }

      up_udelay(RK3576_MALI_POLL_DELAY_US);
    }

  if (i == RK3576_MALI_AS_RETRIES)
    {
      gerr("ERROR: AS%d busy before configuration\n", RK3576_MALI_AS);
      return -ETIMEDOUT;
    }

  /* AArch64 MMU interface: identity mode, outer shareable write-back page
   * table walks (unused in identity mode but harmless and matches what the
   * hardware expects to see in the field).
   */

  rk3576_mali_putreg(RK3576_MALI_AS_TRANSCFG_LO(RK3576_MALI_AS),
                     RK3576_MALI_AS_TRANSCFG_ADRMODE_IDENTITY |
                         RK3576_MALI_AS_TRANSCFG_PTW_MEMATTR_WB |
                         RK3576_MALI_AS_TRANSCFG_PTW_SH_OS |
                         RK3576_MALI_AS_TRANSCFG_PTW_RA);
  rk3576_mali_putreg(RK3576_MALI_AS_TRANSCFG_HI(RK3576_MALI_AS), 0);

  /* Legacy MMU interface: the addressing mode lives in TRANSTAB[1:0] and
   * there is no page table base to publish for an identity mapping.
   */

  rk3576_mali_putreg(RK3576_MALI_AS_TRANSTAB_LO(RK3576_MALI_AS),
                     RK3576_MALI_AS_TRANSTAB_ADRMODE_IDENTITY |
                         RK3576_MALI_AS_TRANSTAB_READ_INNER |
                         RK3576_MALI_AS_TRANSTAB_SHARE_OUTER);
  rk3576_mali_putreg(RK3576_MALI_AS_TRANSTAB_HI(RK3576_MALI_AS), 0);

  rk3576_mali_putreg(RK3576_MALI_AS_MEMATTR_LO(RK3576_MALI_AS),
                     RK3576_MALI_MEMATTR_DEFAULT_LO);
  rk3576_mali_putreg(RK3576_MALI_AS_MEMATTR_HI(RK3576_MALI_AS),
                     RK3576_MALI_MEMATTR_DEFAULT_HI);

  /* Commit the new configuration. */

  rk3576_mali_putreg(RK3576_MALI_AS_COMMAND(RK3576_MALI_AS),
                     RK3576_MALI_AS_CMD_UPDATE);

  for (i = 0; i < RK3576_MALI_AS_RETRIES; i++)
    {
      if ((rk3576_mali_getreg(RK3576_MALI_AS_STATUS(RK3576_MALI_AS)) &
           RK3576_MALI_AS_STATUS_ACTIVE) == 0)
        {
          return OK;
        }

      up_udelay(RK3576_MALI_POLL_DELAY_US);
    }

  gerr("ERROR: AS%d update timed out\n", RK3576_MALI_AS);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_mali_status_name
 *
 * Description:
 *   Human readable name of a JS_STATUS exception code, for logging.
 *
 ****************************************************************************/

static const char *rk3576_mali_status_name(uint32_t status)
{
  switch (status)
    {
      case RK3576_MALI_JS_STATUS_DONE:
        return "done";

      case RK3576_MALI_JS_STATUS_INTERRUPTED:
        return "interrupted";

      case RK3576_MALI_JS_STATUS_STOPPED:
        return "stopped";

      case RK3576_MALI_JS_STATUS_TERMINATED:
        return "terminated";

      case RK3576_MALI_JS_STATUS_ACTIVE:
        return "active";

      case RK3576_MALI_JS_STATUS_CONFIG_FAULT:
        return "job config fault";

      case RK3576_MALI_JS_STATUS_POWER_FAULT:
        return "job power fault";

      case RK3576_MALI_JS_STATUS_READ_FAULT:
        return "job read fault";

      case RK3576_MALI_JS_STATUS_WRITE_FAULT:
        return "job write fault";

      case RK3576_MALI_JS_STATUS_AFFINITY_FAULT:
        return "job affinity fault";

      case RK3576_MALI_JS_STATUS_BUS_FAULT:
        return "job bus fault";

      case RK3576_MALI_JS_STATUS_INSTR_INVALID_PC:
        return "instruction invalid pc";

      case RK3576_MALI_JS_STATUS_INSTR_INVALID_ENC:
        return "instruction invalid encoding";

      case RK3576_MALI_JS_STATUS_INSTR_BARRIER_FAULT:
        return "instruction barrier fault";

      case RK3576_MALI_JS_STATUS_DATA_INVALID_FAULT:
        return "data invalid fault";

      case RK3576_MALI_JS_STATUS_TILE_RANGE_FAULT:
        return "tile range fault";

      case RK3576_MALI_JS_STATUS_OUT_OF_MEMORY:
        return "out of memory";

      default:
        break;
    }

  if (status >= RK3576_MALI_JS_STATUS_TRANSLATION_FAULT)
    {
      return "MMU translation/permission fault";
    }

  return "unknown";
}

/****************************************************************************
 * Name: rk3576_mali_job_interrupt
 *
 * Description:
 *   Job Manager completion interrupt.  Bit n of JOB_IRQ_STATUS means slot
 *   n retired a chain, bit 16+n means it failed.  The slot's JS_STATUS is
 *   latched for the submitter before the interrupt is acknowledged.
 *
 ****************************************************************************/

static int rk3576_mali_job_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_mali_dev_s *priv = (struct rk3576_mali_dev_s *)arg;
  uint32_t status;
  unsigned int slot;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_mali_getreg(RK3576_MALI_JOB_IRQ_STATUS);
  if (status == 0)
    {
      return OK;
    }

  for (slot = 0; slot < RK3576_MALI_NSLOTS; slot++)
    {
      bool done = (status & RK3576_MALI_JOB_IRQ_DONE(slot)) != 0;
      bool failed = (status & RK3576_MALI_JOB_IRQ_FAILED(slot)) != 0;

      if (!done && !failed)
        {
          continue;
        }

      priv->slot[slot].status =
          rk3576_mali_getreg(RK3576_MALI_JS_STATUS(slot));
      priv->slot[slot].failed = failed;

      if (priv->slot[slot].busy)
        {
          nxsem_post(&priv->slot[slot].donesem);
        }
    }

  /* Acknowledge exactly the bits that were serviced. */

  rk3576_mali_putreg(RK3576_MALI_JOB_IRQ_CLEAR, status);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_mmu_interrupt
 *
 * Description:
 *   MMU page fault / bus error interrupt.  With an identity mapping there
 *   is nothing to fix up, so the fault is recorded and logged and the
 *   address space is unlocked so the block does not stay wedged.
 *
 ****************************************************************************/

static int rk3576_mali_mmu_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_mali_dev_s *priv = (struct rk3576_mali_dev_s *)arg;
  uint32_t status;
  unsigned int as;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_mali_getreg(RK3576_MALI_MMU_IRQ_STATUS);
  if (status == 0)
    {
      return OK;
    }

  for (as = 0; as < RK3576_MALI_NAS; as++)
    {
      if ((status & (RK3576_MALI_MMU_IRQ_PAGE_FAULT(as) |
                     RK3576_MALI_MMU_IRQ_BUS_ERROR(as))) == 0)
        {
          continue;
        }

      priv->mmu_faultstatus =
          rk3576_mali_getreg(RK3576_MALI_AS_FAULTSTATUS(as));
      priv->mmu_faultaddress =
          rk3576_mali_getreg64(RK3576_MALI_AS_FAULTADDRESS_LO(as));

      gerr("ERROR: GPU MMU fault on AS%u: status=%08" PRIx32
           " address=%" PRIx64 "\n",
           as, priv->mmu_faultstatus, priv->mmu_faultaddress);

      /* Unlock the address space so the GPU can retire the faulting job
       * and report it through JS_STATUS instead of stalling forever.
       */

      rk3576_mali_putreg(RK3576_MALI_AS_COMMAND(as),
                         RK3576_MALI_AS_CMD_UNLOCK);
    }

  rk3576_mali_putreg(RK3576_MALI_MMU_IRQ_CLEAR, status);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_gpu_interrupt
 *
 * Description:
 *   Global GPU interrupt: faults reported by the block itself outside of a
 *   job, and the reset completion notification.
 *
 ****************************************************************************/

static int rk3576_mali_gpu_interrupt(int irq, void *context, void *arg)
{
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);
  UNUSED(arg);

  status = rk3576_mali_getreg(RK3576_MALI_GPU_IRQ_STATUS);
  if (status == 0)
    {
      return OK;
    }

  if ((status & (RK3576_MALI_GPU_IRQ_FAULT | RK3576_MALI_GPU_IRQ_PROTM_FAULT |
                 RK3576_MALI_GPU_IRQ_MULTIPLE_FAULT)) != 0)
    {
      gerr("ERROR: GPU fault: status=%08" PRIx32 " faultstatus=%08" PRIx32
           " address=%" PRIx64 "\n",
           status, rk3576_mali_getreg(RK3576_MALI_GPU_FAULTSTATUS),
           rk3576_mali_getreg64(RK3576_MALI_GPU_FAULTADDRESS_LO));
    }

  rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_CLEAR, status);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_relocate
 *
 * Description:
 *   Rebase every naturally aligned 64-bit pointer in a recorded command
 *   stream that falls inside the capture-time range [from, from + size)
 *   onto the replay-time base address.
 *
 *   This is the whole relocation model of the record/replay path.  It is a
 *   heuristic: it cannot tell a pointer from a payload word that happens
 *   to hold the same value.  Captures should therefore be taken with the
 *   two ranges placed far away from any plausible constant, which is what
 *   a Panfrost capture on a 64-bit GPU address space naturally gives.
 *
 * Input Parameters:
 *   words  - Command stream copy, viewed as 64-bit words.
 *   nwords - Number of 64-bit words in the copy.
 *   from   - Capture-time base address of the range being relocated.
 *   size   - Size of that range, in bytes.
 *   to     - Replay-time base address of the same range.
 *
 ****************************************************************************/

static void rk3576_mali_relocate(uint64_t *words, size_t nwords, uint64_t from,
                                 uint64_t size, uint64_t to)
{
  size_t i;

  if (size == 0 || from == to)
    {
      return;
    }

  for (i = 0; i < nwords; i++)
    {
      if (words[i] >= from && words[i] < from + size)
        {
          words[i] = to + (words[i] - from);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_mali_initialize
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_initialize(void)
{
  struct rk3576_mali_dev_s *priv = &g_rk3576_mali;
  unsigned int slot;
  int ret;

  if (priv->initialized)
    {
      return OK;
    }

  /* The job header layout is dictated by the hardware; a padding surprise
   * from the compiler would corrupt every submission.
   */

  DEBUGASSERT(sizeof(struct rk3576_mali_job_header_s) ==
              RK3576_MALI_JOB_HEADER_SIZE);

  /* Clocks first: the PMU power-up handshake waits for the GPU bus
   * interface units to acknowledge, and they only do so while clocked.
   */

  ret = rk3576_mali_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_pd_on(RK3576_PD_GPU);
  if (ret < 0)
    {
      gerr("ERROR: failed to power up %s: %d\n", rk3576_pd_name(RK3576_PD_GPU),
           ret);
      return ret;
    }

  ret = rk3576_mali_soft_reset();
  if (ret < 0)
    {
      return ret;
    }

  rk3576_mali_read_features();

  if (priv->info.product_id != RK3576_MALI_PRODUCT_G52)
    {
      gerr("ERROR: unexpected GPU_ID %08" PRIx32 " (product %04" PRIx32
           ", expected %04x)\n",
           priv->info.gpu_id, priv->info.product_id, RK3576_MALI_PRODUCT_G52);
      return -ENODEV;
    }

  ret = rk3576_mali_power_on();
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_mali_as_identity();
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_init(&priv->lock);
  for (slot = 0; slot < RK3576_MALI_NSLOTS; slot++)
    {
      nxsem_init(&priv->slot[slot].donesem, 0, 0);
    }

  /* Clear anything stale, then attach and unmask the three interrupts. */

  rk3576_mali_putreg(RK3576_MALI_JOB_IRQ_CLEAR, RK3576_MALI_JOB_IRQ_ALL);
  rk3576_mali_putreg(RK3576_MALI_MMU_IRQ_CLEAR, RK3576_MALI_MMU_IRQ_ALL);
  rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_CLEAR, RK3576_MALI_GPU_IRQ_ALL);

  ret = irq_attach(RK3576_IRQ_GPU_JOB, rk3576_mali_job_interrupt, priv);
  if (ret < 0)
    {
      gerr("ERROR: irq_attach(%d) failed: %d\n", RK3576_IRQ_GPU_JOB, ret);
      goto err_sem;
    }

  ret = irq_attach(RK3576_IRQ_GPU_MMU, rk3576_mali_mmu_interrupt, priv);
  if (ret < 0)
    {
      gerr("ERROR: irq_attach(%d) failed: %d\n", RK3576_IRQ_GPU_MMU, ret);
      goto err_job;
    }

  ret = irq_attach(RK3576_IRQ_GPU, rk3576_mali_gpu_interrupt, priv);
  if (ret < 0)
    {
      gerr("ERROR: irq_attach(%d) failed: %d\n", RK3576_IRQ_GPU, ret);
      goto err_mmu;
    }

  rk3576_mali_putreg(RK3576_MALI_JOB_IRQ_MASK, RK3576_MALI_JOB_IRQ_ALL);
  rk3576_mali_putreg(RK3576_MALI_MMU_IRQ_MASK, RK3576_MALI_MMU_IRQ_ALL);
  rk3576_mali_putreg(RK3576_MALI_GPU_IRQ_MASK, RK3576_MALI_GPU_IRQ_ENABLED);

  up_enable_irq(RK3576_IRQ_GPU_JOB);
  up_enable_irq(RK3576_IRQ_GPU_MMU);
  up_enable_irq(RK3576_IRQ_GPU);

  priv->initialized = true;

  ginfo("Mali-G52 at %08lx: GPU_ID=%08" PRIx32 " (r%up%u), core clock %" PRIu32
        " Hz\n",
        (unsigned long)priv->base, priv->info.gpu_id,
        RK3576_MALI_GPU_ID_VER_MAJOR(priv->info.gpu_id),
        RK3576_MALI_GPU_ID_VER_MINOR(priv->info.gpu_id), priv->info.coreclk);
  ginfo("  shader=%" PRIx64 " tiler=%" PRIx64 " l2=%" PRIx64 " stack=%" PRIx64
        "\n",
        priv->info.shader_present, priv->info.tiler_present,
        priv->info.l2_present, priv->info.stack_present);
  ginfo("  as_present=%02" PRIx32 " js_present=%02" PRIx32
        " mmu_features=%08" PRIx32 "\n",
        priv->info.as_present, priv->info.js_present, priv->info.mmu_features);

  return OK;

err_mmu:
  irq_detach(RK3576_IRQ_GPU_MMU);
err_job:
  irq_detach(RK3576_IRQ_GPU_JOB);
err_sem:
  for (slot = 0; slot < RK3576_MALI_NSLOTS; slot++)
    {
      nxsem_destroy(&priv->slot[slot].donesem);
    }

  nxmutex_destroy(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_mali_info
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

const struct rk3576_mali_info_s *rk3576_mali_info(void)
{
  if (!g_rk3576_mali.initialized)
    {
      return NULL;
    }

  return &g_rk3576_mali.info;
}

/****************************************************************************
 * Name: rk3576_mali_submit_job
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_submit_job(unsigned int slot, uintptr_t job_chain_phys,
                           uint32_t config)
{
  struct rk3576_mali_dev_s *priv = &g_rk3576_mali;
  int ret;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  if (slot >= RK3576_MALI_NSLOTS ||
      (priv->info.js_present & (1u << slot)) == 0)
    {
      gerr("ERROR: invalid job slot %u\n", slot);
      return -EINVAL;
    }

  if (job_chain_phys == 0 ||
      (job_chain_phys & (RK3576_MALI_JOB_ALIGN - 1)) != 0)
    {
      gerr("ERROR: job chain %lx is not %d-byte aligned\n",
           (unsigned long)job_chain_phys, RK3576_MALI_JOB_ALIGN);
      return -EINVAL;
    }

  if (config == 0)
    {
      config = RK3576_MALI_CONFIG_DEFAULT;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->slot[slot].busy)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  /* Drop any completion left over from a timed out submission. */

  while (nxsem_trywait(&priv->slot[slot].donesem) == OK)
    {
    }

  priv->slot[slot].status = 0;
  priv->slot[slot].failed = false;
  priv->slot[slot].busy = true;

  /* Publish the chain, then the core affinity, then the configuration, and
   * only then start the slot: JS_COMMAND_NEXT latches the whole *_NEXT
   * register set at once.
   */

  rk3576_mali_putreg64(RK3576_MALI_JS_HEAD_NEXT_LO(slot),
                       (uint64_t)job_chain_phys);
  rk3576_mali_putreg64(RK3576_MALI_JS_AFFINITY_NEXT_LO(slot),
                       priv->info.shader_present);
  rk3576_mali_putreg(RK3576_MALI_JS_CONFIG_NEXT(slot), config);
  rk3576_mali_putreg(RK3576_MALI_JS_COMMAND_NEXT(slot),
                     RK3576_MALI_JS_CMD_START);

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_wait_done
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_wait_done(unsigned int slot, unsigned int timeout_ms)
{
  struct rk3576_mali_dev_s *priv = &g_rk3576_mali;
  uint32_t status;
  bool failed;
  int ret;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  if (slot >= RK3576_MALI_NSLOTS)
    {
      return -EINVAL;
    }

  if (!priv->slot[slot].busy)
    {
      return -ESRCH;
    }

  if (timeout_ms == 0)
    {
      timeout_ms = RK3576_MALI_JOB_TIMEOUT_MS;
    }

  ret = nxsem_tickwait_uninterruptible(&priv->slot[slot].donesem,
                                       MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      gerr("ERROR: job on slot %u timed out, JS_STATE=%08" PRIx32
           " JS_STATUS=%08" PRIx32 "\n",
           slot, rk3576_mali_getreg(RK3576_MALI_JOB_IRQ_JS_STATE),
           rk3576_mali_getreg(RK3576_MALI_JS_STATUS(slot)));

      /* Take the slot back so the next submission has a clean start. */

      rk3576_mali_putreg(RK3576_MALI_JS_COMMAND(slot),
                         RK3576_MALI_JS_CMD_HARD_STOP);
      priv->slot[slot].busy = false;
      return -ETIMEDOUT;
    }

  status = priv->slot[slot].status;
  failed = priv->slot[slot].failed;
  priv->slot[slot].busy = false;

  if (failed || status >= RK3576_MALI_JS_STATUS_FAULT_START)
    {
      gerr("ERROR: job on slot %u failed: JS_STATUS=%02" PRIx32 " (%s)\n",
           slot, status, rk3576_mali_status_name(status));
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_mali_run_job
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_run_job(unsigned int slot, uintptr_t job_chain_phys,
                        uint32_t config, unsigned int timeout_ms)
{
  int ret;

  ret = rk3576_mali_submit_job(slot, job_chain_phys, config);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_mali_wait_done(slot, timeout_ms);
}

/****************************************************************************
 * Name: rk3576_mali_job_header_init
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

void rk3576_mali_job_header_init(struct rk3576_mali_job_header_s *hdr,
                                 uint8_t type, uint16_t index, uint64_t next)
{
  DEBUGASSERT(hdr != NULL);

  memset(hdr, 0, sizeof(*hdr));

  hdr->type_and_size =
      RK3576_MALI_JOB_SIZE_64BIT |
      ((type & RK3576_MALI_JOB_TYPE_MASK) << RK3576_MALI_JOB_TYPE_SHIFT);
  hdr->index = index;
  hdr->next = next;
}

/****************************************************************************
 * Name: rk3576_mali_null_job
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_null_job(void)
{
  struct rk3576_mali_job_header_s *hdr;
  uintptr_t pa;
  int ret;

  if (!g_rk3576_mali.initialized)
    {
      return -ENODEV;
    }

  hdr = rk3576_dma_alloc(RK3576_MALI_NULL_JOB_BYTES);
  if (hdr == NULL)
    {
      gerr("ERROR: out of DMA memory for the NULL job\n");
      return -ENOMEM;
    }

  memset(hdr, 0, RK3576_MALI_NULL_JOB_BYTES);
  rk3576_mali_job_header_init(hdr, RK3576_MALI_JOB_TYPE_NULL,
                              RK3576_MALI_FIRST_JOB_INDEX, 0);

  pa = up_addrenv_va_to_pa(hdr);
  up_clean_dcache((uintptr_t)hdr, (uintptr_t)hdr + RK3576_MALI_NULL_JOB_BYTES);

  ret = rk3576_mali_run_job(RK3576_MALI_SLOT_VERTEX, pa, 0, 0);

  /* The GPU writes the exception status back into the header. */

  up_invalidate_dcache((uintptr_t)hdr,
                       (uintptr_t)hdr + RK3576_MALI_NULL_JOB_BYTES);

  if (ret == OK)
    {
      ginfo("NULL job retired, exception_status=%08" PRIx32 "\n",
            hdr->exception_status);
    }

  rk3576_dma_free(hdr, RK3576_MALI_NULL_JOB_BYTES);
  return ret;
}

/****************************************************************************
 * Name: rk3576_mali_replay
 *
 * Description:
 *   See rk3576_mali.h.
 *
 ****************************************************************************/

int rk3576_mali_replay(const struct rk3576_mali_replay_s *replay)
{
  struct rk3576_mali_dev_s *priv = &g_rk3576_mali;
  void *cmd = NULL;
  void *shader = NULL;
  uintptr_t cmdpa;
  uintptr_t shaderpa = 0;
  int ret;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  if (replay == NULL || replay->cmdstream == NULL || replay->cmdsize == 0 ||
      (size_t)replay->job_offset + RK3576_MALI_JOB_HEADER_SIZE >
          replay->cmdsize ||
      (replay->job_offset & (RK3576_MALI_JOB_ALIGN - 1)) != 0 ||
      replay->slot >= RK3576_MALI_NSLOTS)
    {
      return -EINVAL;
    }

  if (replay->shader_binary != NULL && replay->shader_size == 0)
    {
      return -EINVAL;
    }

  /* The relocation pass walks the command stream as 64-bit words, so the
   * recording has to be a whole number of them.
   */

  if ((replay->cmdsize & 7) != 0)
    {
      gerr("ERROR: command stream size %zu is not a multiple of 8\n",
           replay->cmdsize);
      return -EINVAL;
    }

  cmd = rk3576_dma_alloc(replay->cmdsize);
  if (cmd == NULL)
    {
      gerr("ERROR: out of DMA memory for a %zu byte command stream\n",
           replay->cmdsize);
      return -ENOMEM;
    }

  if (replay->shader_binary != NULL)
    {
      shader = rk3576_dma_alloc(replay->shader_size);
      if (shader == NULL)
        {
          gerr("ERROR: out of DMA memory for a %zu byte shader\n",
               replay->shader_size);
          ret = -ENOMEM;
          goto err_free_cmd;
        }

      memcpy(shader, replay->shader_binary, replay->shader_size);
      shaderpa = up_addrenv_va_to_pa(shader);
    }

  memcpy(cmd, replay->cmdstream, replay->cmdsize);
  cmdpa = up_addrenv_va_to_pa(cmd);

  if ((cmdpa & (RK3576_MALI_JOB_ALIGN - 1)) != 0)
    {
      gerr("ERROR: DMA allocator returned unaligned command stream %lx\n",
           (unsigned long)cmdpa);
      ret = -EFAULT;
      goto err_free_shader;
    }

  /* Rebase the recorded pointers onto the copies.  The address space is an
   * identity mapping, so the replay-time GPU address of a block is simply
   * its physical address.
   */

  rk3576_mali_relocate(cmd, replay->cmdsize / sizeof(uint64_t),
                       replay->capture_cva, replay->cmdsize, (uint64_t)cmdpa);

  if (shader != NULL)
    {
      rk3576_mali_relocate(cmd, replay->cmdsize / sizeof(uint64_t),
                           replay->capture_sva, replay->shader_size,
                           (uint64_t)shaderpa);

      up_clean_dcache((uintptr_t)shader,
                      (uintptr_t)shader + replay->shader_size);
    }

  up_clean_dcache((uintptr_t)cmd, (uintptr_t)cmd + replay->cmdsize);

  ret = rk3576_mali_run_job(replay->slot, cmdpa + replay->job_offset,
                            replay->config, replay->timeout_ms);

  /* The GPU writes exception status back into the job headers. */

  up_invalidate_dcache((uintptr_t)cmd, (uintptr_t)cmd + replay->cmdsize);

  if (ret < 0)
    {
      const struct rk3576_mali_job_header_s *hdr =
          (const struct rk3576_mali_job_header_s *)((uintptr_t)cmd +
                                                    replay->job_offset);

      gerr("ERROR: replay failed: exception_status=%08" PRIx32
           " fault_pointer=%" PRIx64 "\n",
           hdr->exception_status, hdr->fault_pointer);
    }

err_free_shader:
  if (shader != NULL)
    {
      rk3576_dma_free(shader, replay->shader_size);
    }

err_free_cmd:
  rk3576_dma_free(cmd, replay->cmdsize);
  return ret;
}

#endif /* CONFIG_RK3576_MALI */
