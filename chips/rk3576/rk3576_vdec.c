/****************************************************************************
 * chips/rk3576/rk3576_vdec.c
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
 * RK3576 RKVDEC (v383) video decoder driver.
 *
 * NuttX has no video-decoder subsystem, so the decoder is exposed as the
 * character device /dev/vdec0.  One ioctl submits one picture: the driver
 * writes the decoder register image (bitstream address, output frame,
 * reference frame array, picture parameters), sets the DEC_E start bit and
 * sleeps until the completion interrupt (GIC INTID 308) fires.
 *
 * H.264 is fully programmed.  H.265 and VP9 share the same submission
 * sequence but need extra register groups; see the TODOs below.
 *
 * The decoder IOMMU is left in bypass (paging disabled), so every address
 * handed to the hardware must be a physically-contiguous 32-bit address:
 * allocate all buffers with rk3576_dma_alloc().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/compiler.h>
#include <nuttx/fs/fs.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <arch/irq.h>

#include "arm64_internal.h"
#include "hardware/rk3576_vdec.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_vdec.h"

#ifdef CONFIG_RK3576_VDEC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GIC INTID of the decoder core.  The vendor DTS carries the raw SPI number
 * (interrupts = <0 0x134 4> -> SPI 308); the GIC INTID is SPI + 32 = 340,
 * which include/irq.h already provides as RK3576_IRQ_RKVDEC_DEC.
 */

#define RK3576_IRQ_RKVDEC RK3576_IRQ_RKVDEC_DEC

/* Power domain index of the VPU (vendor DTS: power-domains = <&power 0xe>).
 * The power-domain driver is not merged yet, hence the weak binding below.
 */

#ifndef RK3576_PD_VPU
#define RK3576_PD_VPU 14
#endif

/* Maximum time a single picture may take before the driver gives up. */

#define RK3576_VDEC_TIMEOUT_MS 500

/* Soft-reset poll budget, in microseconds. */

#define RK3576_VDEC_RESET_TIMEOUT_US 10000
#define RK3576_VDEC_RESET_STEP_US    10

/* Macroblock geometry. */

#define RK3576_VDEC_MB_SIZE 16
#define RK3576_VDEC_MAX_MBS 512 /* 9-bit width/height fields */

/* Hardware addresses are 32-bit. */

#define RK3576_VDEC_ADDR_LIMIT 0x100000000ull

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_vdec_dev_s
{
  mutex_t lock;     /* Serialises task submission             */
  sem_t done;       /* Posted by the completion interrupt     */
  uint32_t status;  /* RKVDEC_REG_INTERRUPT snapshot from ISR */
  uint32_t aclk_hz; /* Real AXI clock rate, for diagnostics   */
  uint32_t core_hz; /* Real decoder core clock rate           */
  void *cabac_tab;  /* Fallback CABAC table buffer, or NULL   */
  bool initialized; /* /dev/vdec0 registered                  */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_vdec_getreg(unsigned int offset);
static void rk3576_vdec_putreg(unsigned int offset, uint32_t value);
static int rk3576_vdec_clk_init(void);
static void rk3576_vdec_pd_init(void);
static void rk3576_vdec_mmu_bypass(void);
static int rk3576_vdec_interrupt(int irq, void *context, void *arg);
static int rk3576_vdec_check_task(const struct rk3576_vdec_task_s *task);
static void rk3576_vdec_write_task(const struct rk3576_vdec_task_s *task);
static void rk3576_vdec_sync_task(const struct rk3576_vdec_task_s *task);

static int rk3576_vdec_ioctl(struct file *filep, int cmd, unsigned long arg);

/* Provided by the (not yet merged) RK3576 power-domain driver.  Until it
 * lands the VPU domain is left in whatever state the boot loader chose,
 * which on this board is powered on.
 */

int rk3576_pd_on(int domain) weak_function;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_vdec_dev_s g_rk3576_vdec = {
  .lock = NXMUTEX_INITIALIZER,
  .done = SEM_INITIALIZER(0),
  .initialized = false,
};

static const struct file_operations g_rk3576_vdec_fops = {
  .ioctl = rk3576_vdec_ioctl,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vdec_getreg
 *
 * Description:
 *   Read one decoder core register.
 *
 ****************************************************************************/

static uint32_t rk3576_vdec_getreg(unsigned int offset)
{
  return getreg32(RK3576_VDEC_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_vdec_putreg
 *
 * Description:
 *   Write one decoder core register.
 *
 ****************************************************************************/

static void rk3576_vdec_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, RK3576_VDEC_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_vdec_clk_init
 *
 * Description:
 *   Acquire and enable every clock the decoder needs.  This is the only
 *   place in the driver that talks to the CLK framework.
 *
 *   The vendor DTS lists five clocks for rkvdec@27b00000:
 *     aclk_vcodec, hclk_vcodec, clk_core, clk_cabac, clk_hevc_cabac
 *   which map onto the RK3576 clock-tree names below.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_vdec_clk_init(void)
{
  static const char *g_vdec_clocks[] = {
    "aclk_vdec_en",      "hclk_vdec_en",           "clk_vdec_core_en",
    "clk_vdec_cabac_en", "clk_vdec_hevc_cabac_en",
  };

  struct clk_s *clk;
  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(g_vdec_clocks) / sizeof(g_vdec_clocks[0]); i++)
    {
      clk = clk_get(g_vdec_clocks[i]);
      if (clk == NULL)
        {
          verr("ERROR: failed to get %s\n", g_vdec_clocks[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          verr("ERROR: failed to enable %s: %d\n", g_vdec_clocks[i], ret);
          return ret;
        }

      /* Keep the rates of the two clocks that matter for diagnostics. */

      if (i == 0)
        {
          g_rk3576_vdec.aclk_hz = clk_get_rate(clk);
        }
      else if (i == 2)
        {
          g_rk3576_vdec.core_hz = clk_get_rate(clk);
        }
    }

  vinfo("aclk %" PRIu32 " Hz, core %" PRIu32 " Hz\n", g_rk3576_vdec.aclk_hz,
        g_rk3576_vdec.core_hz);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vdec_pd_init
 *
 * Description:
 *   Power up the VPU domain if the power-domain driver is present.
 *
 ****************************************************************************/

static void rk3576_vdec_pd_init(void)
{
  if (rk3576_pd_on != NULL)
    {
      int ret = rk3576_pd_on(RK3576_PD_VPU);
      if (ret < 0)
        {
          verr("ERROR: failed to power up the VPU domain: %d\n", ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_vdec_mmu_bypass
 *
 * Description:
 *   Put the decoder IOMMU into bypass so that the register addresses are
 *   consumed as physical addresses.  All decoder buffers come from
 *   rk3576_dma_alloc() and are therefore physically contiguous below 4GB,
 *   which makes address translation unnecessary.
 *
 ****************************************************************************/

static void rk3576_vdec_mmu_bypass(void)
{
  /* Mask the MMU fault interrupt: with paging disabled it can only be
   * raised by a stale configuration.
   */

  putreg32(0, RK3576_VDEC_MMU_ADDR + RK3576_VDEC_MMU_INT_MASK);

  putreg32(RK3576_VDEC_MMU_CMD_DISABLE_PAGING,
           RK3576_VDEC_MMU_ADDR + RK3576_VDEC_MMU_COMMAND);

  /* Acknowledge anything the boot loader left behind. */

  putreg32(0xffffffff, RK3576_VDEC_MMU_ADDR + RK3576_VDEC_MMU_INT_STATUS);
}

/****************************************************************************
 * Name: rk3576_vdec_interrupt
 *
 * Description:
 *   Decoder completion interrupt handler.  Latches the status word, clears
 *   the interrupt and wakes the submitting thread.
 *
 ****************************************************************************/

static int rk3576_vdec_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_vdec_dev_s *priv = arg;
  uint32_t status;

  status = rk3576_vdec_getreg(RKVDEC_REG_INTERRUPT);

  /* Stop the core and clear every latched status bit.  Writing the status
   * bits back acknowledges them; DEC_E is cleared at the same time so the
   * hardware does not restart.
   */

  rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, status & RKVDEC_INT_STA_MASK);

  priv->status = status;
  nxsem_post(&priv->done);

  UNUSED(irq);
  UNUSED(context);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vdec_check_task
 *
 * Description:
 *   Validate a task descriptor before any register is touched.
 *
 * Returned Value:
 *   OK if the task can be submitted, a negated errno value otherwise.
 *
 ****************************************************************************/

static int rk3576_vdec_check_task(const struct rk3576_vdec_task_s *task)
{
  unsigned int mbw;
  unsigned int mbh;
  unsigned int i;

  if (task == NULL)
    {
      return -EINVAL;
    }

  if (task->codec != RK3576_VDEC_CODEC_H264)
    {
      /* TODO: H.265 needs the SCALING_LIST / tile registers and VP9 needs
       * the probability-table and segment registers to be programmed in
       * rk3576_vdec_write_task() before they can be enabled here.
       */

      verr("ERROR: codec %u not implemented\n", task->codec);
      return -ENOTSUP;
    }

  if (task->stream == 0 || task->stream_len == 0 || task->output == 0)
    {
      return -EINVAL;
    }

  if (task->pps == 0 || task->rps == 0)
    {
      return -EINVAL;
    }

  if (task->width == 0 || task->height == 0)
    {
      return -EINVAL;
    }

  mbw = (task->width + RK3576_VDEC_MB_SIZE - 1) / RK3576_VDEC_MB_SIZE;
  mbh = (task->height + RK3576_VDEC_MB_SIZE - 1) / RK3576_VDEC_MB_SIZE;

  if (mbw > RK3576_VDEC_MAX_MBS || mbh > RK3576_VDEC_MAX_MBS)
    {
      return -EINVAL;
    }

  if (task->nrefs > RKVDEC_MAX_REFS)
    {
      return -EINVAL;
    }

  /* Alignment and 32-bit reach of every buffer the hardware will touch. */

  if ((task->stream % RKVDEC_STREAM_ALIGN) != 0 ||
      (task->output % RKVDEC_FRAME_ALIGN) != 0)
    {
      verr("ERROR: misaligned stream/output buffer\n");
      return -EINVAL;
    }

  if ((uint64_t)task->stream + task->stream_len >= RK3576_VDEC_ADDR_LIMIT ||
      (uint64_t)task->output + task->frame_size >= RK3576_VDEC_ADDR_LIMIT)
    {
      verr("ERROR: buffer above the 4GB decoder addressing limit\n");
      return -EFAULT;
    }

  for (i = 0; i < task->nrefs; i++)
    {
      if (task->refs[i].frame >= RK3576_VDEC_ADDR_LIMIT ||
          task->refs[i].colmv >= RK3576_VDEC_ADDR_LIMIT)
        {
          return -EFAULT;
        }
    }

  if ((task->luma_stride % RKVDEC_VIRSTRIDE_UNIT) != 0)
    {
      verr("ERROR: luma stride must be a multiple of %d bytes\n",
           RKVDEC_VIRSTRIDE_UNIT);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_vdec_sync_task
 *
 * Description:
 *   Push every buffer the decoder reads out of the D-cache.  The build is
 *   flat (virtual == physical), so the task addresses double as valid
 *   kernel virtual addresses.
 *
 ****************************************************************************/

static void rk3576_vdec_sync_task(const struct rk3576_vdec_task_s *task)
{
  up_clean_dcache(task->stream, task->stream + task->stream_len);

  /* The parameter buffers have no explicit length in the task descriptor;
   * one cache line covers the packed SPS/PPS and RPS structures used by
   * the H.264 path.
   */

  up_clean_dcache(task->pps, task->pps + RKVDEC_FRAME_ALIGN);
  up_clean_dcache(task->rps, task->rps + RKVDEC_FRAME_ALIGN);

  if (task->cabac_table != 0)
    {
      up_clean_dcache(task->cabac_table,
                      task->cabac_table + RKVDEC_H264_CABAC_TAB_SIZE);
    }

  /* The output frame is written by the hardware: drop any dirty lines now
   * so they cannot be evicted on top of the decoded picture.
   */

  if (task->frame_size != 0)
    {
      up_flush_dcache(task->output, task->output + task->frame_size);
    }
}

/****************************************************************************
 * Name: rk3576_vdec_write_task
 *
 * Description:
 *   Translate a task descriptor into the decoder register image.  The core
 *   must be idle when this runs.
 *
 ****************************************************************************/

static void rk3576_vdec_write_task(const struct rk3576_vdec_task_s *task)
{
  uint32_t sysctrl;
  uint32_t picpar;
  uint32_t mbw;
  uint32_t mbh;
  unsigned int i;

  /* System control: little-endian, plain bitstream mode, H.264. */

  sysctrl = ((uint32_t)RKVDEC_SYSCTRL_MODE_H264 << RKVDEC_SYSCTRL_MODE_SHIFT) &
            RKVDEC_SYSCTRL_MODE_MASK;

  sysctrl |=
      ((uint32_t)task->strm_start_bit << RKVDEC_SYSCTRL_STRM_START_BIT_SHIFT) &
      RKVDEC_SYSCTRL_STRM_START_BIT_MASK;

  sysctrl |= ((uint32_t)task->yuv_format << RKVDEC_SYSCTRL_YUV_MODE_SHIFT) &
             RKVDEC_SYSCTRL_YUV_MODE_MASK;

  if (task->bitdepth > 8)
    {
      sysctrl |=
          (1u << RKVDEC_SYSCTRL_BITDEPTH_SHIFT) & RKVDEC_SYSCTRL_BITDEPTH_MASK;
    }

  rk3576_vdec_putreg(RKVDEC_REG_SYSCTRL, sysctrl);

  /* Picture geometry, in macroblocks minus one. */

  mbw = (task->width + RK3576_VDEC_MB_SIZE - 1) / RK3576_VDEC_MB_SIZE;
  mbh = (task->height + RK3576_VDEC_MB_SIZE - 1) / RK3576_VDEC_MB_SIZE;

  picpar = ((mbw - 1) << RKVDEC_PICPAR_WIDTH_SHIFT) & RKVDEC_PICPAR_WIDTH_MASK;
  picpar |=
      ((mbh - 1) << RKVDEC_PICPAR_HEIGHT_SHIFT) & RKVDEC_PICPAR_HEIGHT_MASK;

  if (task->field)
    {
      picpar |= RKVDEC_PICPAR_FIELD_FLAG;
      if (task->topfield)
        {
          picpar |= RKVDEC_PICPAR_TOPFIELD;
        }
    }

  rk3576_vdec_putreg(RKVDEC_REG_PICPAR, picpar);

  /* Input bitstream. */

  rk3576_vdec_putreg(RKVDEC_REG_STRM_RLC_BASE, (uint32_t)task->stream);
  rk3576_vdec_putreg(RKVDEC_REG_STRM_LEN, (uint32_t)task->stream_len);

  /* CABAC initialisation table.  A zero-filled fallback keeps the core
   * from fetching from address 0 when the caller has none; CAVLC streams
   * never read it.
   */

  rk3576_vdec_putreg(RKVDEC_REG_CABACTBL_PROB_BASE,
                     task->cabac_table != 0
                         ? (uint32_t)task->cabac_table
                         : (uint32_t)(uintptr_t)g_rk3576_vdec.cabac_tab);

  /* Output frame and its strides (programmed in 16-byte units). */

  rk3576_vdec_putreg(RKVDEC_REG_DECOUT_BASE, (uint32_t)task->output);
  rk3576_vdec_putreg(RKVDEC_REG_Y_VIRSTRIDE,
                     task->luma_stride / RKVDEC_VIRSTRIDE_UNIT);
  rk3576_vdec_putreg(RKVDEC_REG_YUV_VIRSTRIDE,
                     task->frame_size / RKVDEC_VIRSTRIDE_UNIT);

  /* Parameter sets and motion-vector storage. */

  rk3576_vdec_putreg(RKVDEC_REG_PPS_BASE, (uint32_t)task->pps);
  rk3576_vdec_putreg(RKVDEC_REG_RPS_BASE, (uint32_t)task->rps);
  rk3576_vdec_putreg(RKVDEC_REG_DIRMV_BASE, (uint32_t)task->colmv);
  rk3576_vdec_putreg(RKVDEC_REG_ERRINFO_BASE, (uint32_t)task->errinfo);

  rk3576_vdec_putreg(RKVDEC_REG_ERR_CTRL,
                     task->conceal ? RKVDEC_ERR_CTRL_CONCEAL_E : 0);

  rk3576_vdec_putreg(RKVDEC_REG_CACHE_CTRL,
                     RKVDEC_CACHE_CTRL_RD_EN | RKVDEC_CACHE_CTRL_WR_EN);

  /* Reference picture array.  Unused slots are cleared so that a stale
   * address from the previous task can never be fetched.
   */

  for (i = 0; i < RKVDEC_MAX_REFS; i++)
    {
      uint32_t poc = 0;
      uint32_t frame = 0;
      uint32_t colmv = 0;

      if (i < task->nrefs && task->refs[i].frame != 0)
        {
          frame = (uint32_t)task->refs[i].frame;
          colmv = (uint32_t)task->refs[i].colmv;
          poc = ((uint32_t)task->refs[i].poc & RKVDEC_REFER_POC_MASK) |
                RKVDEC_REFER_VALID;

          if (task->refs[i].field)
            {
              poc |= RKVDEC_REFER_FIELD_FLAG;
            }
        }

      rk3576_vdec_putreg(RKVDEC_REG_REFER_BASE(i), frame);
      rk3576_vdec_putreg(RKVDEC_REG_REFER_COLMV_BASE(i), colmv);
      rk3576_vdec_putreg(RKVDEC_REG_REFER_POC(i), poc);
    }
}

/****************************************************************************
 * Name: rk3576_vdec_ioctl
 *
 * Description:
 *   /dev/vdec0 ioctl handler.
 *
 ****************************************************************************/

static int rk3576_vdec_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  int ret;

  UNUSED(filep);

  switch (cmd)
    {
      case RK3576_VDEC_IOC_DECODE:
        {
          const struct rk3576_vdec_task_s *task =
              (const struct rk3576_vdec_task_s *)((uintptr_t)arg);

          ret = rk3576_vdec_decode_frame(task);
        }
        break;

      case RK3576_VDEC_IOC_RESET:
        ret = rk3576_vdec_reset();
        break;

      case RK3576_VDEC_IOC_GETVER:
        {
          uint32_t *version = (uint32_t *)((uintptr_t)arg);

          if (version == NULL)
            {
              ret = -EINVAL;
            }
          else
            {
              *version = rk3576_vdec_getreg(RKVDEC_REG_ID);
              ret = OK;
            }
        }
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vdec_reset
 *
 * Description:
 *   Soft reset the decoder core.
 *
 ****************************************************************************/

int rk3576_vdec_reset(void)
{
  unsigned int elapsed;

  rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, RKVDEC_INT_SOFTRESET_E);

  for (elapsed = 0; elapsed < RK3576_VDEC_RESET_TIMEOUT_US;
       elapsed += RK3576_VDEC_RESET_STEP_US)
    {
      if ((rk3576_vdec_getreg(RKVDEC_REG_INTERRUPT) &
           RKVDEC_INT_SOFTRESET_RDY) != 0)
        {
          rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, RKVDEC_INT_STA_MASK);
          return OK;
        }

      up_udelay(RK3576_VDEC_RESET_STEP_US);
    }

  verr("ERROR: soft reset timed out\n");

  /* Leave the core in a defined state even when the ready bit never came:
   * clearing DEC_E and the status bits is the best that can be done.
   */

  rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, RKVDEC_INT_STA_MASK);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_vdec_decode_frame
 *
 * Description:
 *   Submit one picture and wait for the decoder to finish it.
 *
 ****************************************************************************/

int rk3576_vdec_decode_frame(const struct rk3576_vdec_task_s *task)
{
  struct rk3576_vdec_dev_s *priv = &g_rk3576_vdec;
  uint32_t status;
  int ret;

  if (!priv->initialized)
    {
      return -ENODEV;
    }

  ret = rk3576_vdec_check_task(task);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Drain a stale post left by a task that timed out earlier. */

  while (nxsem_trywait(&priv->done) >= 0)
    {
    }

  priv->status = 0;

  rk3576_vdec_sync_task(task);

  /* Clear any pending status, program the task, then start. */

  rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, RKVDEC_INT_STA_MASK);
  rk3576_vdec_write_task(task);

  rk3576_vdec_putreg(RKVDEC_REG_INTERRUPT, RKVDEC_INT_DEC_E |
                                               RKVDEC_INT_DEC_TIMEOUT_E |
                                               RKVDEC_INT_BUF_EMPTY_E);

  ret = nxsem_tickwait_uninterruptible(&priv->done,
                                       MSEC2TICK(RK3576_VDEC_TIMEOUT_MS));
  if (ret < 0)
    {
      verr("ERROR: decode timed out, status 0x%08" PRIx32 "\n",
           rk3576_vdec_getreg(RKVDEC_REG_INTERRUPT));
      rk3576_vdec_reset();
      nxmutex_unlock(&priv->lock);
      return -ETIMEDOUT;
    }

  status = priv->status;

  if ((status & RKVDEC_INT_ERR_MASK) != 0)
    {
      verr("ERROR: decode failed, status 0x%08" PRIx32 "\n", status);
      rk3576_vdec_reset();
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  if ((status & RKVDEC_INT_DEC_RDY_STA) == 0)
    {
      verr("ERROR: decoder finished without ready, status 0x%08" PRIx32 "\n",
           status);
      nxmutex_unlock(&priv->lock);
      return -EIO;
    }

  /* The picture was written by the hardware behind the D-cache. */

  if (task->frame_size != 0)
    {
      up_invalidate_dcache(task->output, task->output + task->frame_size);
    }

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_vdec_initialize
 *
 * Description:
 *   Bring the decoder up and register /dev/vdec0.
 *
 ****************************************************************************/

int rk3576_vdec_initialize(void)
{
  struct rk3576_vdec_dev_s *priv = &g_rk3576_vdec;
  int ret;

  if (priv->initialized)
    {
      return OK;
    }

  rk3576_vdec_pd_init();

  ret = rk3576_vdec_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  /* Register mode only: make sure the boot loader did not leave the link
   * (command chain) engine enabled.
   */

  putreg32(0, RK3576_VDEC_LINK_ADDR + RKVDEC_LINK_REG_EN);

  rk3576_vdec_mmu_bypass();

  ret = rk3576_vdec_reset();
  if (ret < 0)
    {
      return ret;
    }

  /* Fallback CABAC table: zero filled, used only when a caller submits an
   * H.264 task without one.  Allocated from the DMA heap so the decoder can
   * reach it.
   */

  priv->cabac_tab = rk3576_dma_alloc(RKVDEC_H264_CABAC_TAB_SIZE);
  if (priv->cabac_tab == NULL)
    {
      verr("ERROR: failed to allocate the CABAC table buffer\n");
      return -ENOMEM;
    }

  memset(priv->cabac_tab, 0, RKVDEC_H264_CABAC_TAB_SIZE);
  up_clean_dcache((uintptr_t)priv->cabac_tab,
                  (uintptr_t)priv->cabac_tab + RKVDEC_H264_CABAC_TAB_SIZE);

  ret = irq_attach(RK3576_IRQ_RKVDEC, rk3576_vdec_interrupt, priv);
  if (ret < 0)
    {
      verr("ERROR: irq_attach failed: %d\n", ret);
      goto err_free;
    }

  up_enable_irq(RK3576_IRQ_RKVDEC);

  ret = register_driver(RK3576_VDEC_DEVPATH, &g_rk3576_vdec_fops, 0666, priv);
  if (ret < 0)
    {
      verr("ERROR: failed to register %s: %d\n", RK3576_VDEC_DEVPATH, ret);
      goto err_irq;
    }

  priv->initialized = true;

  vinfo("RKVDEC ready, hardware id 0x%08" PRIx32 "\n",
        rk3576_vdec_getreg(RKVDEC_REG_ID));
  return OK;

err_irq:
  up_disable_irq(RK3576_IRQ_RKVDEC);
  irq_detach(RK3576_IRQ_RKVDEC);

err_free:
  rk3576_dma_free(priv->cabac_tab, RKVDEC_H264_CABAC_TAB_SIZE);
  priv->cabac_tab = NULL;
  return ret;
}

#endif /* CONFIG_RK3576_VDEC */
