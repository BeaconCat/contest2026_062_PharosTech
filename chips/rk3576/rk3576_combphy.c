/****************************************************************************
 * chips/rk3576/rk3576_combphy.c
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
 * RK3576 Naneng COMBPHY driver -- PCIe personality.
 *
 * Each combo PHY owns three pieces of state:
 *
 *   - A small analog register file at its own base address.  Registers are
 *     addressed as (index << 2), matching the vendor numbering PHYREG<n>.
 *   - A slice of the "pipe PHY GRF" syscon that selects the protocol
 *     personality (PCIe/USB3/SATA/SGMII) and the pipe-side signal sources.
 *     All GRF writes are hiword-masked.
 *   - Three clocks (refclk / apbclk / pipe_clk) and two resets
 *     (combphy-apb / combphy), taken from the vendor device tree.
 *
 * Bring-up order, mirroring the vendor sequence:
 *   clocks on -> apb reset deasserted -> personality + pipe muxing in GRF
 *   -> analog trim for the current reference clock -> PHY reset deasserted
 *   -> poll pipe PHY GRF status for "phy ready".
 *
 * The reference clock is driven at 100 MHz (the DTS assigns 0x5F5E100 to
 * the refclk), which is the PCIe external reference rate.
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

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_pcie.h"
#include "rk3576_combphy.h"

#ifdef CONFIG_RK3576_COMBPHY

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Analog register file addressing: PHYREG<n> lives at base + (n << 2). */

#define RK3576_COMBPHY_REG(n) ((n) << 2)

/* Analog registers touched during PCIe bring-up (vendor numbering). */

#define RK3576_COMBPHY_PHYREG7       RK3576_COMBPHY_REG(0x07) /* SSC enable */
#define RK3576_COMBPHY_PHYREG27      RK3576_COMBPHY_REG(0x27) /* PLL LPF trim */
#define RK3576_COMBPHY_PHYREG29      RK3576_COMBPHY_REG(0x29) /* SU trim */
#define RK3576_COMBPHY_PHYREG31      RK3576_COMBPHY_REG(0x1f) /* SSC spread */
#define RK3576_COMBPHY_PHYREG33      RK3576_COMBPHY_REG(0x33) /* PLL KVCO */
#define RK3576_COMBPHY_PHYREG44      RK3576_COMBPHY_REG(0x44) /* jitter ctrl */

#define RK3576_COMBPHY_SSC_EN        (1 << 4)
#define RK3576_COMBPHY_SSC_DOWN_MASK (3 << 4)
#define RK3576_COMBPHY_SSC_DOWNWARD  (1 << 4)
#define RK3576_COMBPHY_KVCO_MASK     (7 << 10)
#define RK3576_COMBPHY_KVCO_VALUE    (2 << 10)
#define RK3576_COMBPHY_JITTER_RANDOM 0x04
#define RK3576_COMBPHY_PLL_LPF_TRIM  0x04
#define RK3576_COMBPHY_SU_TRIM_100M  0x90

/* Pipe PHY GRF CON values selecting the PCIe personality.  The four CON
 * words are written wholesale (16-bit hiword mask 0xffff).
 *
 * TODO: these are the RK3568/RK3588 naneng values reused for RK3576.  They
 * match the pipe-phy-grf layout in the RK3576 TRM, but the exact CON2/CON3
 * lane-swap and rate fields should be re-checked against the TRM once the
 * PCIe chapter is available.
 */

#define RK3576_COMBPHY_CON0_PCIE 0x1000
#define RK3576_COMBPHY_CON1_PCIE 0x0000
#define RK3576_COMBPHY_CON2_PCIE 0x0101
#define RK3576_COMBPHY_CON3_PCIE 0x0200

/* pipe_clk_100m: PIPE_PHY_GRF CON1[14:13] = 0b10 selects a 100 MHz pipe
 * reference; 0b01 would select 25 MHz.
 */

#define RK3576_COMBPHY_PIPE_CLK_SHIFT 13
#define RK3576_COMBPHY_PIPE_CLK_MASK  0x3
#define RK3576_COMBPHY_PIPE_CLK_25M   0x1
#define RK3576_COMBPHY_PIPE_CLK_100M  0x2

/* Reference clock rates the analog trim table knows about. */

#define RK3576_COMBPHY_REF_25MHZ  25000000u
#define RK3576_COMBPHY_REF_100MHZ 100000000u

/* PCIe wants a 100 MHz reference. */

#define RK3576_COMBPHY_PCIE_REF_RATE RK3576_COMBPHY_REF_100MHZ

/* PHY-ready polling: the vendor driver allows ~1 ms. */

#define RK3576_COMBPHY_READY_US     2000
#define RK3576_COMBPHY_POLL_STEP_US 10

/* Reset pulse widths (vendor sequence uses "a few microseconds"). */

#define RK3576_COMBPHY_RESET_US 20

/* Software reset descriptors.
 *
 * The vendor DTS encodes combo PHY resets as <&cru 0x2xxxx>, where the
 * 0x20000 flag selects the PHP/PPLL CRU instance and the low 16 bits are a
 * flat bit index: register = SOFTRST_CON(index / 16), bit = index % 16.
 *
 *   combphy0: apb = 0x20005 -> PPLL SOFTRST_CON0 bit 5
 *             phy = 0x20015 -> PPLL SOFTRST_CON1 bit 5
 *   combphy1: apb = 0x20007 -> PPLL SOFTRST_CON0 bit 7
 *             phy = 0x20018 -> PPLL SOFTRST_CON1 bit 8
 *
 * TODO: confirm the PPLL CRU SOFTRST_CON base against the RK3576 TRM CRU
 * chapter; the mapping above is derived from the vendor device tree.
 */

#define RK3576_COMBPHY_RST_BANK(id) (((id)&0xffff) / 16)
#define RK3576_COMBPHY_RST_BIT(id)  (((id)&0xffff) % 16)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-instance immutable description. */

struct rk3576_combphy_config_s
{
  uintptr_t base;           /* Analog register file base */
  uintptr_t phy_grf;        /* pipe-phy-grf syscon base */
  uint32_t apb_rst_id;      /* combphy-apb reset id (vendor encoding) */
  uint32_t phy_rst_id;      /* combphy reset id (vendor encoding) */
  const char *refclk_name;  /* CLK framework name of the reference clock */
  const char *apbclk_name;  /* CLK framework name of the APB clock */
  const char *pipeclk_name; /* CLK framework name of the pipe clock */
};

/* Per-instance mutable state. */

struct rk3576_combphy_dev_s
{
  const struct rk3576_combphy_config_s *cfg;
  uint32_t ref_rate; /* Reference clock rate actually achieved (Hz) */
  bool initialized;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_combphy_grf_write(uintptr_t grf, uint32_t off,
                                     uint32_t shift, uint32_t mask,
                                     uint32_t value);
static void rk3576_combphy_reset_assert(uint32_t id);
static void rk3576_combphy_reset_deassert(uint32_t id);
static int rk3576_combphy_clk_init(struct rk3576_combphy_dev_s *dev);
static void rk3576_combphy_pcie_trim(struct rk3576_combphy_dev_s *dev);
static int rk3576_combphy_wait_ready(struct rk3576_combphy_dev_s *dev);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rk3576_combphy_config_s
    g_rk3576_combphy_config[RK3576_COMBPHY_NUM] = {
      {
          .base = RK3576_COMBPHY0_ADDR,
          .phy_grf = RK3576_PIPE_PHY_GRF0_ADDR,
          .apb_rst_id = 0x20005,
          .phy_rst_id = 0x20015,
          .refclk_name = "clk_combphy0_ref_en",
          .apbclk_name = "pclk_combphy0_en",
          .pipeclk_name = "clk_combphy0_pipe_en",
      },
      {
          .base = RK3576_COMBPHY1_ADDR,
          .phy_grf = RK3576_PIPE_PHY_GRF1_ADDR,
          .apb_rst_id = 0x20007,
          .phy_rst_id = 0x20018,
          .refclk_name = "clk_combphy1_ref_en",
          .apbclk_name = "pclk_combphy1_en",
          .pipeclk_name = "clk_combphy1_pipe_en",
      },
    };

static struct rk3576_combphy_dev_s g_rk3576_combphy_dev[RK3576_COMBPHY_NUM] = {
  {
      .cfg = &g_rk3576_combphy_config[RK3576_COMBPHY0],
  },
  {
      .cfg = &g_rk3576_combphy_config[RK3576_COMBPHY1],
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_combphy_grf_write
 *
 * Description:
 *   Update a bit field of a hiword-masked pipe PHY GRF register.
 *
 * Input Parameters:
 *   grf   - GRF syscon base address
 *   off   - Register offset within the syscon
 *   shift - Bit offset of the field
 *   mask  - Field mask, already shifted down to bit 0
 *   value - New field value, already shifted down to bit 0
 *
 ****************************************************************************/

static void rk3576_combphy_grf_write(uintptr_t grf, uint32_t off,
                                     uint32_t shift, uint32_t mask,
                                     uint32_t value)
{
  putreg32(RK3576_PCIE_HIWORD(mask << shift, (value & mask) << shift),
           grf + off);
}

/****************************************************************************
 * Name: rk3576_combphy_reset_assert
 *
 * Description:
 *   Assert one CRU software reset described by a vendor reset id.
 *
 ****************************************************************************/

static void rk3576_combphy_reset_assert(uint32_t id)
{
  uintptr_t reg = RK3576_PPLL_CRU_ADDR +
                  RK3576_CRU_SOFTRST_CON(RK3576_COMBPHY_RST_BANK(id));
  uint32_t bit = 1u << RK3576_COMBPHY_RST_BIT(id);

  putreg32(RK3576_PCIE_HIWORD(bit, bit), reg);
}

/****************************************************************************
 * Name: rk3576_combphy_reset_deassert
 *
 * Description:
 *   Release one CRU software reset described by a vendor reset id.
 *
 ****************************************************************************/

static void rk3576_combphy_reset_deassert(uint32_t id)
{
  uintptr_t reg = RK3576_PPLL_CRU_ADDR +
                  RK3576_CRU_SOFTRST_CON(RK3576_COMBPHY_RST_BANK(id));
  uint32_t bit = 1u << RK3576_COMBPHY_RST_BIT(id);

  putreg32(RK3576_PCIE_HIWORD(bit, 0), reg);
}

/****************************************************************************
 * Name: rk3576_combphy_clk_init
 *
 * Description:
 *   Single point of contact with the NuttX CLK framework for this driver:
 *   fetch, rate-set and enable every clock the combo PHY needs, then record
 *   the real reference clock rate for the analog trim step.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_combphy_clk_init(struct rk3576_combphy_dev_s *dev)
{
  const struct rk3576_combphy_config_s *cfg = dev->cfg;
  struct clk_s *clk;
  int ret;

  /* APB register-access clock */

  clk = clk_get(cfg->apbclk_name);
  if (clk == NULL)
    {
      pcierr("ERROR: failed to get %s\n", cfg->apbclk_name);
      return -ENODEV;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      pcierr("ERROR: failed to enable %s: %d\n", cfg->apbclk_name, ret);
      return ret;
    }

  /* PIPE interface clock towards the controller */

  clk = clk_get(cfg->pipeclk_name);
  if (clk == NULL)
    {
      pcierr("ERROR: failed to get %s\n", cfg->pipeclk_name);
      return -ENODEV;
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      pcierr("ERROR: failed to enable %s: %d\n", cfg->pipeclk_name, ret);
      return ret;
    }

  /* Reference clock: PCIe needs 100 MHz.  Ask for it, then read back what
   * the tree actually produced instead of assuming the request succeeded.
   */

  clk = clk_get(cfg->refclk_name);
  if (clk == NULL)
    {
      pcierr("ERROR: failed to get %s\n", cfg->refclk_name);
      return -ENODEV;
    }

  ret = clk_set_rate(clk, RK3576_COMBPHY_PCIE_REF_RATE);
  if (ret < 0)
    {
      pciwarn("WARNING: could not set %s to %u Hz: %d\n", cfg->refclk_name,
              RK3576_COMBPHY_PCIE_REF_RATE, ret);
    }

  ret = clk_enable(clk);
  if (ret < 0)
    {
      pcierr("ERROR: failed to enable %s: %d\n", cfg->refclk_name, ret);
      return ret;
    }

  dev->ref_rate = clk_get_rate(clk);
  pciinfo("combphy ref clock %u Hz\n", dev->ref_rate);

  return OK;
}

/****************************************************************************
 * Name: rk3576_combphy_pcie_trim
 *
 * Description:
 *   Apply the analog trim for the PCIe personality.  The values depend on
 *   the reference clock rate that rk3576_combphy_clk_init() measured.
 *
 ****************************************************************************/

static void rk3576_combphy_pcie_trim(struct rk3576_combphy_dev_s *dev)
{
  uintptr_t base = dev->cfg->base;
  uintptr_t grf = dev->cfg->phy_grf;
  uint32_t val;

  /* Downward spread spectrum, as required for a PCIe reference. */

  val = getreg32(base + RK3576_COMBPHY_PHYREG31);
  val &= ~RK3576_COMBPHY_SSC_DOWN_MASK;
  val |= RK3576_COMBPHY_SSC_DOWNWARD;
  putreg32(val, base + RK3576_COMBPHY_PHYREG31);

  /* Select the PCIe personality and the pipe signal sources. */

  putreg32(RK3576_PCIE_HIWORD(0xffff, RK3576_COMBPHY_CON0_PCIE),
           grf + RK3576_PIPE_PHY_GRF_CON0);
  putreg32(RK3576_PCIE_HIWORD(0xffff, RK3576_COMBPHY_CON1_PCIE),
           grf + RK3576_PIPE_PHY_GRF_CON1);
  putreg32(RK3576_PCIE_HIWORD(0xffff, RK3576_COMBPHY_CON2_PCIE),
           grf + RK3576_PIPE_PHY_GRF_CON2);
  putreg32(RK3576_PCIE_HIWORD(0xffff, RK3576_COMBPHY_CON3_PCIE),
           grf + RK3576_PIPE_PHY_GRF_CON3);

  if (dev->ref_rate == RK3576_COMBPHY_REF_100MHZ)
    {
      rk3576_combphy_grf_write(
          grf, RK3576_PIPE_PHY_GRF_CON1, RK3576_COMBPHY_PIPE_CLK_SHIFT,
          RK3576_COMBPHY_PIPE_CLK_MASK, RK3576_COMBPHY_PIPE_CLK_100M);

      /* PLL KVCO tuning for a 100 MHz reference. */

      val = getreg32(base + RK3576_COMBPHY_PHYREG33);
      val &= ~RK3576_COMBPHY_KVCO_MASK;
      val |= RK3576_COMBPHY_KVCO_VALUE;
      putreg32(val, base + RK3576_COMBPHY_PHYREG33);

      /* Enable random-jitter control, set the PLL loop filter trim and the
       * start-up trim to the values the vendor uses at 100 MHz.
       */

      putreg32(RK3576_COMBPHY_JITTER_RANDOM, base + RK3576_COMBPHY_PHYREG44);
      putreg32(RK3576_COMBPHY_PLL_LPF_TRIM, base + RK3576_COMBPHY_PHYREG27);
      putreg32(RK3576_COMBPHY_SU_TRIM_100M, base + RK3576_COMBPHY_PHYREG29);
    }
  else if (dev->ref_rate == RK3576_COMBPHY_REF_25MHZ)
    {
      rk3576_combphy_grf_write(
          grf, RK3576_PIPE_PHY_GRF_CON1, RK3576_COMBPHY_PIPE_CLK_SHIFT,
          RK3576_COMBPHY_PIPE_CLK_MASK, RK3576_COMBPHY_PIPE_CLK_25M);
    }
  else
    {
      /* 24 MHz (or anything else) keeps the PHY reset defaults.  PCIe is
       * only specified for a 100 MHz reference, so warn loudly.
       */

      pciwarn("WARNING: combphy reference %u Hz is not 100 MHz\n",
              dev->ref_rate);
    }

  /* Finally enable spread spectrum on the transmit PLL. */

  val = getreg32(base + RK3576_COMBPHY_PHYREG7);
  val |= RK3576_COMBPHY_SSC_EN;
  putreg32(val, base + RK3576_COMBPHY_PHYREG7);
}

/****************************************************************************
 * Name: rk3576_combphy_wait_ready
 *
 * Description:
 *   Poll the pipe PHY GRF status word until the PHY reports ready.
 *
 * Returned Value:
 *   OK if the PHY came up, -ETIMEDOUT otherwise.
 *
 ****************************************************************************/

static int rk3576_combphy_wait_ready(struct rk3576_combphy_dev_s *dev)
{
  uint32_t elapsed;

  for (elapsed = 0; elapsed < RK3576_COMBPHY_READY_US;
       elapsed += RK3576_COMBPHY_POLL_STEP_US)
    {
      if ((getreg32(dev->cfg->phy_grf + RK3576_PIPE_PHY_GRF_STATUS0) &
           RK3576_PIPE_PHY_STATUS0_PHY_READY) == 0)
        {
          /* The status bit is active low: 0 means ready. */

          return OK;
        }

      up_udelay(RK3576_COMBPHY_POLL_STEP_US);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_combphy_pcie_init
 *
 * Description:
 *   Power up and configure one combo PHY for the PCIe personality.
 *   See rk3576_combphy.h for the full contract.
 *
 ****************************************************************************/

int rk3576_combphy_pcie_init(int phyid)
{
  struct rk3576_combphy_dev_s *dev;
  int ret;

  if (phyid < 0 || phyid >= RK3576_COMBPHY_NUM)
    {
      return -EINVAL;
    }

  dev = &g_rk3576_combphy_dev[phyid];
  if (dev->initialized)
    {
      return OK;
    }

  /* Hold the PHY in reset while its clocks are brought up. */

  rk3576_combphy_reset_assert(dev->cfg->apb_rst_id);
  rk3576_combphy_reset_assert(dev->cfg->phy_rst_id);

  ret = rk3576_combphy_clk_init(dev);
  if (ret < 0)
    {
      return ret;
    }

  up_udelay(RK3576_COMBPHY_RESET_US);

  /* Registers become accessible once the APB reset is released. */

  rk3576_combphy_reset_deassert(dev->cfg->apb_rst_id);
  up_udelay(RK3576_COMBPHY_RESET_US);

  rk3576_combphy_pcie_trim(dev);

  /* Release the PHY itself and wait for the PLL to lock. */

  rk3576_combphy_reset_deassert(dev->cfg->phy_rst_id);

  ret = rk3576_combphy_wait_ready(dev);
  if (ret < 0)
    {
      pcierr("ERROR: combphy%d not ready, grf status 0x%08" PRIx32 "\n", phyid,
             getreg32(dev->cfg->phy_grf + RK3576_PIPE_PHY_GRF_STATUS0));
      rk3576_combphy_reset_assert(dev->cfg->phy_rst_id);
      return ret;
    }

  dev->initialized = true;
  pciinfo("combphy%d ready (PCIe mode)\n", phyid);
  return OK;
}

/****************************************************************************
 * Name: rk3576_combphy_uninit
 *
 * Description:
 *   Assert the PHY reset again and mark the instance as uninitialised.
 *
 ****************************************************************************/

void rk3576_combphy_uninit(int phyid)
{
  struct rk3576_combphy_dev_s *dev;

  if (phyid < 0 || phyid >= RK3576_COMBPHY_NUM)
    {
      return;
    }

  dev = &g_rk3576_combphy_dev[phyid];
  rk3576_combphy_reset_assert(dev->cfg->phy_rst_id);
  dev->initialized = false;
}

#endif /* CONFIG_RK3576_COMBPHY */
