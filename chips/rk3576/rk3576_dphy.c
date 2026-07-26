/****************************************************************************
 * chips/rk3576/rk3576_dphy.c
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
 * RK3576 MIPI RX D-PHY (Innosilicon) driver.
 *
 * Two RX D-PHYs feed the five MIPI CSI-2 host controllers.  This driver
 * brings a PHY up for a given sensor lane rate: it gates the APB clock on,
 * switches the PHY to CSI mode through its GRF companion, enables the clock
 * lane plus the requested data lanes and programs the HS-SETTLE counters.
 *
 * ---------------------------------------------------------------------
 * Lane rate
 * ---------------------------------------------------------------------
 * The per-lane HS bit rate carried on the link is
 *
 *     lane_rate[bps] = pixel_rate[pixel/s] * bits_per_pixel / num_lanes
 *
 * with pixel_rate = width * height * fps.  Example, the on-board IMX415 in
 * 3864x2192 RAW10 at 30fps over 4 lanes:
 *
 *     pixel_rate = 3864 * 2192 * 30      = 254.1 Mpixel/s
 *     lane_rate  = 254.1e6 * 10 / 4      = 635 Mbps  (active pixels only)
 *
 * Real sensors also transmit blanking, so the datasheet link frequency is
 * authoritative: the IMX415 CSI-2 4-lane mode runs at 720MHz DDR, i.e.
 * 1440Mbps per lane.  Always prefer the datasheet number and only fall back
 * to rk3576_dphy_lane_rate_mbps() when it is unavailable.
 *
 * ---------------------------------------------------------------------
 * HS-SETTLE
 * ---------------------------------------------------------------------
 * T_HS-SETTLE is the time the receiver ignores the data lane after the
 * HS-Rqst (LP-00) to HS-0 transition, so that it samples only after the
 * transmitter's own T_HS-SETTLE window has closed.  The MIPI D-PHY spec
 * constrains the receiver to
 *
 *     85ns + 6*UI  <=  T_HS-SETTLE  <=  145ns + 10*UI
 *
 * where UI (unit interval) = 1 / lane_rate.  We aim at the midpoint,
 *
 *     T_HS-SETTLE = 115ns + 8*UI
 *
 * which leaves the widest margin at both ends.  For the clock lane the spec
 * only sets T_CLK-SETTLE in [95ns, 300ns]; 150ns is used.
 *
 * The register counts periods of the PHY reference clock (the APB clock
 * feeding the PHY digital core, read back with clk_get_rate()):
 *
 *     count = ceil(T_SETTLE[ns] * f_ref[Hz] / 1e9) - 1
 *
 * All of the arithmetic below is integer only; UI is kept in picoseconds
 * (UI_ps = 1000000 / lane_rate_mbps) to avoid losing resolution.
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
#include "hardware/rk3576_csi.h"
#include "rk3576_dphy.h"

#ifdef CONFIG_RK3576_CSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MIPI D-PHY specification constants, see the block comment above. */

#define RK3576_DPHY_SETTLE_BASE_NS   115 /* midpoint of 85..145ns       */
#define RK3576_DPHY_SETTLE_UI_FACTOR 8   /* midpoint of 6..10 UI        */
#define RK3576_DPHY_CLK_SETTLE_NS    150 /* within 95..300ns            */

/* One microsecond expressed in picoseconds, used for the UI computation. */

#define RK3576_DPHY_PS_PER_US 1000000u

/* Nanoseconds per second, used for the reference-clock conversion. */

#define RK3576_DPHY_NS_PER_SEC 1000000000ull

/* Skew calibration is mandatory above this lane rate (MIPI D-PHY v1.2). */

#define RK3576_DPHY_CALIB_THRESHOLD_MBPS 1500

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static per-PHY description: register windows and clock name. */

struct rk3576_dphy_desc_s
{
  uintptr_t base;        /* PHY register window base            */
  uintptr_t grf;         /* Associated MIPI D-PHY GRF base      */
  const char *pclk_name; /* APB clock gate name in the CLK tree */
};

/* Runtime state of one PHY. */

struct rk3576_dphy_s
{
  const struct rk3576_dphy_desc_s *desc;
  uint32_t pclk_hz; /* Reference clock rate, read via clk framework */
  int num_lanes;    /* Data lanes currently enabled                 */
  bool initialized; /* Clocks acquired and PHY configured           */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_dphy_putreg(struct rk3576_dphy_s *priv, unsigned int off,
                               uint32_t val);
static uint32_t rk3576_dphy_getreg(struct rk3576_dphy_s *priv,
                                   unsigned int off);
static int rk3576_dphy_clk_init(struct rk3576_dphy_s *priv);
static uint32_t rk3576_dphy_settle_count(uint32_t settle_ns, uint32_t ref_hz);
static void rk3576_dphy_program_settle(struct rk3576_dphy_s *priv,
                                       uint32_t lane_rate_mbps, int num_lanes);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rk3576_dphy_desc_s
    g_rk3576_dphy_desc[RK3576_CSI2DPHY_NPHY] = {
      {
          .base = RK3576_CSI2DPHY0_ADDR,
          .grf = RK3576_CSI2DPHY0_GRF_ADDR,
          .pclk_name = "pclk_csidphy0_en",
      },
      {
          .base = RK3576_CSI2DPHY1_ADDR,
          .grf = RK3576_CSI2DPHY1_GRF_ADDR,
          .pclk_name = "pclk_csidphy1_en",
      },
    };

static struct rk3576_dphy_s g_rk3576_dphy[RK3576_CSI2DPHY_NPHY] = {
  {
      .desc = &g_rk3576_dphy_desc[0],
  },
  {
      .desc = &g_rk3576_dphy_desc[1],
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dphy_putreg / rk3576_dphy_getreg
 *
 * Description:
 *   Access one PHY register.  Only the low 8 bits of each 32-bit word are
 *   implemented by the Innosilicon PHY.
 *
 ****************************************************************************/

static void rk3576_dphy_putreg(struct rk3576_dphy_s *priv, unsigned int off,
                               uint32_t val)
{
  putreg32(val, priv->desc->base + off);
}

static uint32_t rk3576_dphy_getreg(struct rk3576_dphy_s *priv,
                                   unsigned int off)
{
  return getreg32(priv->desc->base + off);
}

/****************************************************************************
 * Name: rk3576_dphy_clk_init
 *
 * Description:
 *   Single point of contact with the CLK framework for this driver: acquire
 *   and enable the PHY APB clock and cache its real rate, which doubles as
 *   the HS-SETTLE counter reference.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_dphy_clk_init(struct rk3576_dphy_s *priv)
{
  struct clk_s *pclk;
  int ret;

  pclk = clk_get(priv->desc->pclk_name);
  if (pclk == NULL)
    {
      verr("ERROR: failed to get %s\n", priv->desc->pclk_name);
      return -ENODEV;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      verr("ERROR: failed to enable %s: %d\n", priv->desc->pclk_name, ret);
      return ret;
    }

  priv->pclk_hz = clk_get_rate(pclk);
  if (priv->pclk_hz == 0)
    {
      verr("ERROR: %s reports a zero rate\n", priv->desc->pclk_name);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_dphy_settle_count
 *
 * Description:
 *   Convert a settle time in nanoseconds into a reference-clock period
 *   count, rounding up and subtracting the implicit first period:
 *
 *     count = ceil(settle_ns * ref_hz / 1e9) - 1
 *
 * Returned Value:
 *   The register value, saturated to the field width.
 *
 ****************************************************************************/

static uint32_t rk3576_dphy_settle_count(uint32_t settle_ns, uint32_t ref_hz)
{
  uint64_t cycles;

  cycles =
      ((uint64_t)settle_ns * (uint64_t)ref_hz + RK3576_DPHY_NS_PER_SEC - 1) /
      RK3576_DPHY_NS_PER_SEC;

  if (cycles == 0)
    {
      return 0;
    }

  cycles--;

  if (cycles > RK3576_DPHY_THS_SETTLE_MASK)
    {
      cycles = RK3576_DPHY_THS_SETTLE_MASK;
    }

  return (uint32_t)cycles;
}

/****************************************************************************
 * Name: rk3576_dphy_program_settle
 *
 * Description:
 *   Compute and write the clock-lane and data-lane HS-SETTLE counters, and
 *   enable per-lane skew calibration when the link runs above 1.5Gbps.
 *
 ****************************************************************************/

static void rk3576_dphy_program_settle(struct rk3576_dphy_s *priv,
                                       uint32_t lane_rate_mbps, int num_lanes)
{
  uint32_t ui_ps;
  uint32_t settle_ns;
  uint32_t data_count;
  uint32_t clk_count;
  uint32_t calib;
  int lane;

  /* UI in picoseconds: 1 / lane_rate.  1 Mbps -> 1000000ps per bit. */

  ui_ps = RK3576_DPHY_PS_PER_US / lane_rate_mbps;

  /* T_HS-SETTLE = 115ns + 8*UI, rounded up to whole nanoseconds. */

  settle_ns = RK3576_DPHY_SETTLE_BASE_NS +
              (RK3576_DPHY_SETTLE_UI_FACTOR * ui_ps + 999) / 1000;

  data_count = rk3576_dphy_settle_count(settle_ns, priv->pclk_hz);
  clk_count =
      rk3576_dphy_settle_count(RK3576_DPHY_CLK_SETTLE_NS, priv->pclk_hz);

  vinfo("DPHY%d rate=%" PRIu32 "Mbps UI=%" PRIu32 "ps settle=%" PRIu32
        "ns ref=%" PRIu32 "Hz clkcnt=%" PRIu32 " datacnt=%" PRIu32 "\n",
        (int)(priv - g_rk3576_dphy), lane_rate_mbps, ui_ps, settle_ns,
        priv->pclk_hz, clk_count, data_count);

  calib = lane_rate_mbps > RK3576_DPHY_CALIB_THRESHOLD_MBPS
              ? RK3576_DPHY_CALIB_ENABLE
              : RK3576_DPHY_CALIB_DISABLE;

  rk3576_dphy_putreg(priv, RK3576_DPHY_CLK_WR_THS_SETTLE_OFFSET, clk_count);
  rk3576_dphy_putreg(priv, RK3576_DPHY_CLK_CALIB_EN_OFFSET, calib);

  for (lane = 0; lane < num_lanes; lane++)
    {
      rk3576_dphy_putreg(priv,
                         RK3576_DPHY_LANE0_WR_THS_SETTLE_OFFSET +
                             lane * RK3576_DPHY_LANE_WR_THS_SETTLE_STRIDE,
                         data_count);

      rk3576_dphy_putreg(priv,
                         RK3576_DPHY_LANE0_CALIB_EN_OFFSET +
                             lane * RK3576_DPHY_LANE_CALIB_EN_STRIDE,
                         calib);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dphy_lane_rate_mbps
 *
 * Description:
 *   See rk3576_dphy.h.
 *
 ****************************************************************************/

uint32_t rk3576_dphy_lane_rate_mbps(uint64_t pixel_rate, uint32_t bpp,
                                    int num_lanes)
{
  uint64_t bits;

  if (pixel_rate == 0 || bpp == 0 || num_lanes < 1 ||
      num_lanes > RK3576_DPHY_MAX_LANES)
    {
      return 0;
    }

  bits = pixel_rate * bpp / (uint64_t)num_lanes;

  /* Convert bit/s to Mbps, rounding up so we never under-drive the PHY. */

  return (uint32_t)((bits + 999999ull) / 1000000ull);
}

/****************************************************************************
 * Name: rk3576_dphy_initialize
 *
 * Description:
 *   See rk3576_dphy.h.
 *
 ****************************************************************************/

int rk3576_dphy_initialize(int phy_id, uint32_t lane_rate_mbps, int num_lanes)
{
  struct rk3576_dphy_s *priv;
  uint32_t lane_mask;
  uint32_t val;
  int ret;

  if (phy_id < 0 || phy_id >= RK3576_CSI2DPHY_NPHY)
    {
      verr("ERROR: invalid D-PHY id %d\n", phy_id);
      return -EINVAL;
    }

  if (num_lanes < 1 || num_lanes > RK3576_DPHY_MAX_LANES)
    {
      verr("ERROR: invalid lane count %d\n", num_lanes);
      return -EINVAL;
    }

  if (lane_rate_mbps < RK3576_DPHY_MIN_LANE_RATE_MBPS ||
      lane_rate_mbps > RK3576_DPHY_MAX_LANE_RATE_MBPS)
    {
      verr("ERROR: lane rate %" PRIu32 "Mbps out of range\n", lane_rate_mbps);
      return -ERANGE;
    }

  priv = &g_rk3576_dphy[phy_id];

  if (!priv->initialized)
    {
      ret = rk3576_dphy_clk_init(priv);
      if (ret < 0)
        {
          return ret;
        }
    }

  lane_mask = (1u << num_lanes) - 1u;

  /* Select CSI mode and enable the lanes in the GRF companion.  The GRF
   * register is hiword-masked.
   */

  putreg32(RK3576_DPHY_GRF_HIWORD(RK3576_DPHY_GRF_CON0_LANE_MASK |
                                      RK3576_DPHY_GRF_CON0_CLK_LANE_EN |
                                      RK3576_DPHY_GRF_CON0_PHY_MODE_CSI,
                                  lane_mask |
                                      RK3576_DPHY_GRF_CON0_CLK_LANE_EN |
                                      RK3576_DPHY_GRF_CON0_PHY_MODE_CSI),
           priv->desc->grf + RK3576_DPHY_GRF_CON0_OFFSET);

  /* Hold the digital core in reset while the lanes are configured. */

  rk3576_dphy_putreg(priv, RK3576_DPHY_CTRL_DIG_RST_OFFSET,
                     RK3576_DPHY_CTRL_DIG_RST_RESET);

  rk3576_dphy_program_settle(priv, lane_rate_mbps, num_lanes);

  /* Enable the clock lane and the requested data lanes. */

  val = rk3576_dphy_getreg(priv, RK3576_DPHY_CTRL_LANE_ENABLE_OFFSET);
  val &=
      ~(RK3576_DPHY_CTRL_LANE_ENABLE_MASK | RK3576_DPHY_CTRL_LANE_ENABLE_CK);
  val |= (lane_mask << RK3576_DPHY_CTRL_LANE_ENABLE_SHIFT) &
         RK3576_DPHY_CTRL_LANE_ENABLE_MASK;
  val |= RK3576_DPHY_CTRL_LANE_ENABLE_CK | RK3576_DPHY_CTRL_ENABLE;
  rk3576_dphy_putreg(priv, RK3576_DPHY_CTRL_LANE_ENABLE_OFFSET, val);

  /* Release the digital core.  The PHY needs a short settling time before
   * the link may be driven; 1us is well above the PPI requirement.
   */

  rk3576_dphy_putreg(priv, RK3576_DPHY_CTRL_DIG_RST_OFFSET,
                     RK3576_DPHY_CTRL_DIG_RST_RUN);
  up_udelay(1);

  priv->num_lanes = num_lanes;
  priv->initialized = true;

  vinfo("DPHY%d up: %d lanes @ %" PRIu32 "Mbps\n", phy_id, num_lanes,
        lane_rate_mbps);
  return OK;
}

/****************************************************************************
 * Name: rk3576_dphy_uninitialize
 *
 * Description:
 *   See rk3576_dphy.h.
 *
 ****************************************************************************/

int rk3576_dphy_uninitialize(int phy_id)
{
  struct rk3576_dphy_s *priv;

  if (phy_id < 0 || phy_id >= RK3576_CSI2DPHY_NPHY)
    {
      return -EINVAL;
    }

  priv = &g_rk3576_dphy[phy_id];
  if (!priv->initialized)
    {
      return OK;
    }

  rk3576_dphy_putreg(priv, RK3576_DPHY_CTRL_LANE_ENABLE_OFFSET, 0);
  rk3576_dphy_putreg(priv, RK3576_DPHY_CTRL_DIG_RST_OFFSET,
                     RK3576_DPHY_CTRL_DIG_RST_RESET);

  putreg32(RK3576_DPHY_GRF_HIWORD(RK3576_DPHY_GRF_CON0_LANE_MASK |
                                      RK3576_DPHY_GRF_CON0_CLK_LANE_EN,
                                  0),
           priv->desc->grf + RK3576_DPHY_GRF_CON0_OFFSET);

  priv->num_lanes = 0;
  return OK;
}

#endif /* CONFIG_RK3576_CSI */
