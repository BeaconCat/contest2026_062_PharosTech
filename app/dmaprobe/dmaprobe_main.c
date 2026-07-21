/****************************************************************************
 * app/dmaprobe/dmaprobe_main.c
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
 * Bring-up self-test for the RK3576 ARM PL330 (DMA-330) controller.
 *
 * Exercises the generic NuttX DMA framework path end-to-end on real
 * hardware:
 *
 *   DMA_GET_CHAN   -- reserve a memory-to-memory channel (DRQ_NONE)
 *   DMA_CONFIG     -- direction=MEM_TO_MEM, width=4 bytes
 *   DMA_START      -- copy a 512-byte seeded buffer; callback posts a sem
 *   DMA_PUT_CHAN   -- release channel
 *
 * The destination is cache-invalidated before comparison so the CPU reads
 * what the PL330 actually stored.  Run "dmaprobe" from the NSH command
 * line and observe the result on the console / syslog.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/cache.h>
#include <nuttx/dma/dma.h>
#include <nuttx/semaphore.h>

/* Chip-level initializer declared here rather than via the arch include
 * path so the app directory stays decoupled from the BSP tree.  Only the
 * generic struct dma_dev_s * handle is consumed; the private micro-code
 * API is fully hidden inside rk3576_dma.c.
 */

struct dma_dev_s *rk3576_dma_initialize(void);

/* M2M sentinel: no peripheral request line bound to this channel.
 * Matches RK3576_DMA_DRQ_NONE defined in chips/rk3576/rk3576_dma.h.
 */

#define DMAPROBE_DRQ_NONE 0xff

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DMAPROBE_NBYTES  512
#define DMAPROBE_WIDTH   4 /* beat width in bytes */
#define DMAPROBE_TIMEOUT 2 /* seconds to wait for completion */

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Cache-line aligned so the dcache maintenance around the DMA hits whole
 * lines only (no false sharing with neighbours).
 */

static uint8_t g_src[DMAPROBE_NBYTES] aligned_data(64);
static uint8_t g_dst[DMAPROBE_NBYTES] aligned_data(64);

static sem_t g_done;
static ssize_t g_result;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dmaprobe_callback(FAR struct dma_chan_s *chan, FAR void *arg,
                              ssize_t len)
{
  UNUSED(chan);
  UNUSED(arg);

  g_result = len;
  nxsem_post(&g_done);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR struct dma_dev_s *dma;
  FAR struct dma_chan_s *chan;
  struct dma_config_s cfg;
  unsigned int i;
  int ret;

  UNUSED(argc);
  UNUSED(argv);

  syslog(LOG_INFO, "DMAPROBE: RK3576 PL330 generic-framework M2M self-test\n");

  nxsem_init(&g_done, 0, 0);

  /* Seed source, clear destination. */

  for (i = 0; i < DMAPROBE_NBYTES; i++)
    {
      g_src[i] = (uint8_t)(i ^ 0xa5);
    }

  memset(g_dst, 0, DMAPROBE_NBYTES);

  /* Clean both buffers: PL330 is not cache-coherent with the CPU. */

  up_clean_dcache((uintptr_t)g_src, (uintptr_t)g_src + DMAPROBE_NBYTES);
  up_clean_dcache((uintptr_t)g_dst, (uintptr_t)g_dst + DMAPROBE_NBYTES);

  /* Obtain the DMA controller and a free M2M channel. */

  dma = rk3576_dma_initialize();
  chan = DMA_GET_CHAN(dma, DMAPROBE_DRQ_NONE);
  if (chan == NULL)
    {
      syslog(LOG_ERR, "DMAPROBE: DMA_GET_CHAN failed -- no free channel\n");
      ret = -ENODEV;
      goto out_sem;
    }

  /* Configure: memory-to-memory, 4-byte beats. */

  memset(&cfg, 0, sizeof(cfg));
  cfg.direction = DMA_MEM_TO_MEM;
  cfg.src_width = DMAPROBE_WIDTH;
  cfg.dst_width = DMAPROBE_WIDTH;

  ret = DMA_CONFIG(chan, &cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: DMA_CONFIG failed: %d\n", ret);
      goto out_chan;
    }

  /* Start the copy; callback wakes g_done on completion. */

  ret = DMA_START(chan, dmaprobe_callback, NULL, (uintptr_t)g_dst,
                  (uintptr_t)g_src, DMAPROBE_NBYTES);
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: DMA_START failed: %d\n", ret);
      goto out_chan;
    }

  /* Wait for completion (bounded so a dead controller cannot hang). */

  ret = nxsem_tickwait_uninterruptible(&g_done, SEC2TICK(DMAPROBE_TIMEOUT));
  if (ret < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: timeout waiting for completion\n");
      DMA_STOP(chan);
      goto out_chan;
    }

  if (g_result < 0)
    {
      syslog(LOG_ERR, "DMAPROBE: transfer faulted (%" PRIdPTR ")\n",
             (intptr_t)g_result);
      ret = (int)g_result;
      goto out_chan;
    }

  /* Invalidate destination: read what DMA actually wrote, not CPU cache. */

  up_invalidate_dcache((uintptr_t)g_dst, (uintptr_t)g_dst + DMAPROBE_NBYTES);

  if (memcmp(g_src, g_dst, DMAPROBE_NBYTES) == 0)
    {
      syslog(LOG_INFO, "DMAPROBE: PASS -- %u bytes copied and verified\n",
             DMAPROBE_NBYTES);
      ret = OK;
    }
  else
    {
      syslog(LOG_ERR, "DMAPROBE: FAIL -- destination mismatch\n");
      ret = -EIO;
    }

out_chan:
  DMA_PUT_CHAN(dma, chan);
out_sem:
  nxsem_destroy(&g_done);
  return ret;
}
