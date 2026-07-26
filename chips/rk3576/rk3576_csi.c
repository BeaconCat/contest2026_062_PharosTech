/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_csi.c
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
 * RK3576 MIPI CSI-2 host controller driver (Synopsys DesignWare MIPI CSI-2
 * Host).  Five instances terminate the CSI-2 link protocol coming out of the
 * two RX D-PHYs and forward pixel data to VICAP/ISP over an internal bus -
 * the controller itself has no DMA engine and no frame buffers, so no DMA
 * memory is allocated here.
 *
 * Each instance owns two GIC lines ("csi-intr1"/"csi-intr2" in the vendor
 * DTS); both are attached to the same handler, which reads the top-level
 * CSI2_INT_ST_MAIN register and then drains the per-group status registers.
 * All of the status registers are read-to-clear.
 *
 * Interrupt-safe: the counters are only touched from the handler and read
 * with interrupts disabled in rk3576_csi_get_stats().
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
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_csi.h"
#include "rk3576_csi.h"
#include "rk3576_dphy.h"

#ifdef CONFIG_RK3576_CSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Number of GIC lines per host controller. */

#define RK3576_CSI_NIRQS 2

/* Interrupt groups we care about.  The individual bits inside each group
 * are lane/VC specific and only logged, so every source is unmasked.
 *
 * The Synopsys mask registers use "1 = masked", therefore unmasking is a
 * write of zero and masking a write of all-ones.
 */

#define RK3576_CSI_INT_UNMASK_ALL 0x00000000
#define RK3576_CSI_INT_MASK_ALL   0xffffffff

/* Reset pulse width.  The controller only needs the APB clock to be running
 * for a few cycles; 10us is far beyond that and keeps the code readable.
 */

#define RK3576_CSI_RESET_DELAY_US 10

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static per-controller description. */

struct rk3576_csi_desc_s
{
  uintptr_t base;            /* Register window base            */
  int irq[RK3576_CSI_NIRQS]; /* GIC lines                       */
  const char *pclk_name;     /* APB clock gate name             */
  const char *iclk_name;     /* Interface clock gate, or NULL   */
};

/* Runtime state of one controller. */

struct rk3576_csi_s
{
  const struct rk3576_csi_desc_s *desc;
  struct rk3576_csi_config_s cfg;  /* Copy of the caller's config     */
  struct rk3576_csi_stats_s stats; /* Error counters                  */
  spinlock_t lock;                 /* Guards stats and state          */
  bool clocked;                    /* Clocks acquired and enabled     */
  bool running;                    /* Between start() and stop()      */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_csi_getreg(struct rk3576_csi_s *priv, unsigned int off);
static void rk3576_csi_putreg(struct rk3576_csi_s *priv, unsigned int off,
                              uint32_t val);
static int rk3576_csi_clk_init(struct rk3576_csi_s *priv);
static void rk3576_csi_set_reset(struct rk3576_csi_s *priv, bool assert_rst);
static void rk3576_csi_program_ids(struct rk3576_csi_s *priv);
static void rk3576_csi_mask_all(struct rk3576_csi_s *priv);
static void rk3576_csi_unmask_all(struct rk3576_csi_s *priv);
static int rk3576_csi_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Base addresses and interrupts taken from the vendor DTS nodes
 * mipiN-csi2-hw@27c[8-c]0000.  Only CSI0 has a separate interface clock
 * ("iclk_csi2host"); the others only list "pclk_csi2host".
 */

static const struct rk3576_csi_desc_s
    g_rk3576_csi_desc[RK3576_CSI2HOST_NHOST] = {
      {
          .base = RK3576_CSI2HOST0_ADDR,
          .irq = { RK3576_IRQ_CSIHOST0_1, RK3576_IRQ_CSIHOST0_2 },
          .pclk_name = "pclk_csi2host0_en",
          .iclk_name = "iclk_csi2host0_en",
      },
      {
          .base = RK3576_CSI2HOST1_ADDR,
          .irq = { RK3576_IRQ_CSIHOST1_1, RK3576_IRQ_CSIHOST1_2 },
          .pclk_name = "pclk_csi2host1_en",
          .iclk_name = NULL,
      },
      {
          .base = RK3576_CSI2HOST2_ADDR,
          .irq = { RK3576_IRQ_CSIHOST2_1, RK3576_IRQ_CSIHOST2_2 },
          .pclk_name = "pclk_csi2host2_en",
          .iclk_name = NULL,
      },
      {
          .base = RK3576_CSI2HOST3_ADDR,
          .irq = { RK3576_IRQ_CSIHOST3_1, RK3576_IRQ_CSIHOST3_2 },
          .pclk_name = "pclk_csi2host3_en",
          .iclk_name = NULL,
      },
      {
          .base = RK3576_CSI2HOST4_ADDR,
          .irq = { RK3576_IRQ_CSIHOST4_1, RK3576_IRQ_CSIHOST4_2 },
          .pclk_name = "pclk_csi2host4_en",
          .iclk_name = NULL,
      },
    };

static struct rk3576_csi_s g_rk3576_csi[RK3576_CSI2HOST_NHOST] = {
  {
      .desc = &g_rk3576_csi_desc[0],
      .lock = SP_UNLOCKED,
  },
  {
      .desc = &g_rk3576_csi_desc[1],
      .lock = SP_UNLOCKED,
  },
  {
      .desc = &g_rk3576_csi_desc[2],
      .lock = SP_UNLOCKED,
  },
  {
      .desc = &g_rk3576_csi_desc[3],
      .lock = SP_UNLOCKED,
  },
  {
      .desc = &g_rk3576_csi_desc[4],
      .lock = SP_UNLOCKED,
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_getreg / rk3576_csi_putreg
 *
 * Description:
 *   Access one controller register.
 *
 ****************************************************************************/

static uint32_t rk3576_csi_getreg(struct rk3576_csi_s *priv, unsigned int off)
{
  return getreg32(priv->desc->base + off);
}

static void rk3576_csi_putreg(struct rk3576_csi_s *priv, unsigned int off,
                              uint32_t val)
{
  putreg32(val, priv->desc->base + off);
}

/****************************************************************************
 * Name: rk3576_csi_clk_init
 *
 * Description:
 *   Single point of contact with the CLK framework for this driver: get and
 *   enable the APB clock and, where the SoC provides one, the interface
 *   clock of this controller.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_csi_clk_init(struct rk3576_csi_s *priv)
{
  struct clk_s *clk;
  int ret;

  clk = clk_get(priv->desc->pclk_name);
  if (clk == NULL)
    {
      verr("ERROR: failed to get %s\n", priv->desc->pclk_name);
      return -ENODEV;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      verr("ERROR: failed to enable %s: %d\n", priv->desc->pclk_name, ret);
      return ret;
    }

  if (priv->desc->iclk_name != NULL)
    {
      clk = clk_get(priv->desc->iclk_name);
      if (clk == NULL)
        {
          verr("ERROR: failed to get %s\n", priv->desc->iclk_name);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          verr("ERROR: failed to enable %s: %d\n", priv->desc->iclk_name, ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_csi_set_reset
 *
 * Description:
 *   Assert or release the controller's active-low soft reset.
 *
 ****************************************************************************/

static void rk3576_csi_set_reset(struct rk3576_csi_s *priv, bool assert_rst)
{
  rk3576_csi_putreg(priv, RK3576_CSI2_RESETN_OFFSET,
                    assert_rst ? 0 : RK3576_CSI2_RESETN_ENABLE);
  up_udelay(RK3576_CSI_RESET_DELAY_US);
}

/****************************************************************************
 * Name: rk3576_csi_program_ids
 *
 * Description:
 *   Fill CSI2_DATA_IDS_1/2 with the VC/DT pairs the link should accept.
 *   Unused slots are left at zero, which the controller treats as "no
 *   match" for anything but VC0/DT0 (a reserved short packet type).
 *
 ****************************************************************************/

static void rk3576_csi_program_ids(struct rk3576_csi_s *priv)
{
  uint32_t ids[2];
  int i;

  ids[0] = 0;
  ids[1] = 0;

  for (i = 0; i < priv->cfg.nstreams; i++)
    {
      int reg = i / RK3576_CSI2_DATA_IDS_PER_REG;

      ids[reg] |= (uint32_t)RK3576_CSI2_DATA_ID(priv->cfg.streams[i].vc,
                                                priv->cfg.streams[i].dt)
                  << RK3576_CSI2_DATA_ID_SHIFT(i);
    }

  rk3576_csi_putreg(priv, RK3576_CSI2_DATA_IDS_1_OFFSET, ids[0]);
  rk3576_csi_putreg(priv, RK3576_CSI2_DATA_IDS_2_OFFSET, ids[1]);
}

/****************************************************************************
 * Name: rk3576_csi_mask_all / rk3576_csi_unmask_all
 *
 * Description:
 *   Mask or unmask every error source.  1 = masked in this IP.
 *
 ****************************************************************************/

static void rk3576_csi_mask_all(struct rk3576_csi_s *priv)
{
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PHY_FATAL_OFF,
                    RK3576_CSI_INT_MASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PKT_FATAL_OFF,
                    RK3576_CSI_INT_MASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_FRAME_FATAL_O,
                    RK3576_CSI_INT_MASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PHY_OFFSET,
                    RK3576_CSI_INT_MASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PKT_OFFSET,
                    RK3576_CSI_INT_MASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_LINE_OFFSET,
                    RK3576_CSI_INT_MASK_ALL);
}

static void rk3576_csi_unmask_all(struct rk3576_csi_s *priv)
{
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PHY_FATAL_OFF,
                    RK3576_CSI_INT_UNMASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PKT_FATAL_OFF,
                    RK3576_CSI_INT_UNMASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_FRAME_FATAL_O,
                    RK3576_CSI_INT_UNMASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PHY_OFFSET,
                    RK3576_CSI_INT_UNMASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_PKT_OFFSET,
                    RK3576_CSI_INT_UNMASK_ALL);
  rk3576_csi_putreg(priv, RK3576_CSI2_INT_MSK_LINE_OFFSET,
                    RK3576_CSI_INT_UNMASK_ALL);
}

/****************************************************************************
 * Name: rk3576_csi_interrupt
 *
 * Description:
 *   Common handler for both GIC lines of one controller.  Reads the main
 *   status register and drains (read-to-clear) each signalled group,
 *   accumulating per-group counters.
 *
 * Returned Value:
 *   Always OK.
 *
 ****************************************************************************/

static int rk3576_csi_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_csi_s *priv = (struct rk3576_csi_s *)arg;
  uint32_t main_st;
  uint32_t st;

  UNUSED(irq);
  UNUSED(context);

  main_st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_MAIN_OFFSET);
  if (main_st == 0)
    {
      return OK;
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_PHY_FATAL) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PHY_FATAL_OFF);
      priv->stats.phy_fatal++;
      verr("CSI: PHY fatal 0x%08" PRIx32 "\n", st);
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_PKT_FATAL) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PKT_FATAL_OFF);
      priv->stats.pkt_fatal++;
      verr("CSI: packet fatal 0x%08" PRIx32 "\n", st);
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_FRAME_FATAL) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_FRAME_FATAL_OF);
      priv->stats.frame_fatal++;
      verr("CSI: frame fatal 0x%08" PRIx32 "\n", st);
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_PHY) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PHY_OFFSET);
      priv->stats.phy_err++;
      vwarn("CSI: PHY error 0x%08" PRIx32 "\n", st);
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_PKT) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PKT_OFFSET);
      priv->stats.pkt_err++;
      vwarn("CSI: packet error 0x%08" PRIx32 "\n", st);
    }

  if ((main_st & RK3576_CSI2_INT_MAIN_LINE) != 0)
    {
      st = rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_LINE_OFFSET);
      priv->stats.line_err++;
      vwarn("CSI: line error 0x%08" PRIx32 "\n", st);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_initialize
 *
 * Description:
 *   See rk3576_csi.h.
 *
 ****************************************************************************/

int rk3576_csi_initialize(int csi_id, const struct rk3576_csi_config_s *cfg)
{
  struct rk3576_csi_s *priv;
  uint32_t version;
  int ret;
  int i;

  if (csi_id < 0 || csi_id >= RK3576_CSI2HOST_NHOST || cfg == NULL)
    {
      verr("ERROR: invalid CSI host id %d\n", csi_id);
      return -EINVAL;
    }

  if (cfg->num_lanes < 1 || cfg->num_lanes > RK3576_CSI_MAX_LANES)
    {
      verr("ERROR: invalid lane count %d\n", cfg->num_lanes);
      return -EINVAL;
    }

  if (cfg->nstreams < 1 || cfg->nstreams > RK3576_CSI_MAX_STREAMS)
    {
      verr("ERROR: invalid stream count %d\n", cfg->nstreams);
      return -EINVAL;
    }

  priv = &g_rk3576_csi[csi_id];

  if (!priv->clocked)
    {
      ret = rk3576_csi_clk_init(priv);
      if (ret < 0)
        {
          return ret;
        }

      priv->clocked = true;
    }

  /* Hold the controller in reset while it is configured. */

  rk3576_csi_set_reset(priv, true);
  rk3576_csi_mask_all(priv);

  memcpy(&priv->cfg, cfg, sizeof(priv->cfg));
  memset(&priv->stats, 0, sizeof(priv->stats));

  rk3576_csi_putreg(priv, RK3576_CSI2_N_LANES_OFFSET,
                    (uint32_t)(cfg->num_lanes - 1) & RK3576_CSI2_N_LANES_MASK);

  rk3576_csi_program_ids(priv);

  version = rk3576_csi_getreg(priv, RK3576_CSI2_VERSION_OFFSET);
  vinfo("CSI%d: version 0x%08" PRIx32 ", %d lanes, %d stream(s), phy%d\n",
        csi_id, version, cfg->num_lanes, cfg->nstreams, cfg->phy_id);

  /* Attach both interrupt lines; they share one handler. */

  for (i = 0; i < RK3576_CSI_NIRQS; i++)
    {
      ret = irq_attach(priv->desc->irq[i], rk3576_csi_interrupt, priv);
      if (ret < 0)
        {
          verr("ERROR: failed to attach IRQ %d: %d\n", priv->desc->irq[i],
               ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_csi_start
 *
 * Description:
 *   See rk3576_csi.h.
 *
 ****************************************************************************/

int rk3576_csi_start(int csi_id)
{
  struct rk3576_csi_s *priv;
  irqstate_t flags;
  int i;

  if (csi_id < 0 || csi_id >= RK3576_CSI2HOST_NHOST)
    {
      return -EINVAL;
    }

  priv = &g_rk3576_csi[csi_id];
  if (!priv->clocked)
    {
      verr("ERROR: CSI%d not initialized\n", csi_id);
      return -EPERM;
    }

  flags = spin_lock_irqsave(&priv->lock);

  if (priv->running)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return OK;
    }

  priv->running = true;
  spin_unlock_irqrestore(&priv->lock, flags);

  /* Release the reset first so that the status registers are valid, then
   * clear any error latched during the PHY bring-up and unmask.
   */

  rk3576_csi_set_reset(priv, false);

  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PHY_FATAL_OFF);
  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PKT_FATAL_OFF);
  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_FRAME_FATAL_OF);
  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PHY_OFFSET);
  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_PKT_OFFSET);
  rk3576_csi_getreg(priv, RK3576_CSI2_INT_ST_LINE_OFFSET);

  rk3576_csi_unmask_all(priv);

  for (i = 0; i < RK3576_CSI_NIRQS; i++)
    {
      up_enable_irq(priv->desc->irq[i]);
    }

  vinfo("CSI%d: started\n", csi_id);
  return OK;
}

/****************************************************************************
 * Name: rk3576_csi_stop
 *
 * Description:
 *   See rk3576_csi.h.
 *
 ****************************************************************************/

int rk3576_csi_stop(int csi_id)
{
  struct rk3576_csi_s *priv;
  irqstate_t flags;
  int i;

  if (csi_id < 0 || csi_id >= RK3576_CSI2HOST_NHOST)
    {
      return -EINVAL;
    }

  priv = &g_rk3576_csi[csi_id];
  if (!priv->clocked)
    {
      return -EPERM;
    }

  flags = spin_lock_irqsave(&priv->lock);

  if (!priv->running)
    {
      spin_unlock_irqrestore(&priv->lock, flags);
      return OK;
    }

  priv->running = false;
  spin_unlock_irqrestore(&priv->lock, flags);

  for (i = 0; i < RK3576_CSI_NIRQS; i++)
    {
      up_disable_irq(priv->desc->irq[i]);
    }

  rk3576_csi_mask_all(priv);
  rk3576_csi_set_reset(priv, true);

  vinfo("CSI%d: stopped\n", csi_id);
  return OK;
}

/****************************************************************************
 * Name: rk3576_csi_get_stats
 *
 * Description:
 *   See rk3576_csi.h.
 *
 ****************************************************************************/

int rk3576_csi_get_stats(int csi_id, struct rk3576_csi_stats_s *stats)
{
  struct rk3576_csi_s *priv;
  irqstate_t flags;

  if (csi_id < 0 || csi_id >= RK3576_CSI2HOST_NHOST || stats == NULL)
    {
      return -EINVAL;
    }

  priv = &g_rk3576_csi[csi_id];
  flags = spin_lock_irqsave(&priv->lock);
  memcpy(stats, &priv->stats, sizeof(*stats));
  spin_unlock_irqrestore(&priv->lock, flags);

  return OK;
}

#endif /* CONFIG_RK3576_CSI */
