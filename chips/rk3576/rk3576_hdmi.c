/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_hdmi.c
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
 * RK3576 HDMI TX encoder driver.
 *
 * Drives the Synopsys DesignWare HDMI TX "QP" controller at 0x27DA0000
 * together with the Rockchip/Samsung HDPTX PHY at 0x2B000000.  The VOP is
 * the pixel source; this module only turns the incoming pixel stream into
 * TMDS, so the API is a plain chip-level one (there is no NuttX HDMI
 * upper half to bind to) — see rk3576_hdmi.h.
 *
 * Power-up order implemented by rk3576_hdmi_set_mode():
 *
 *   1. PHY PLL off, output disabled
 *   2. compute ROPLL dividers for the requested TMDS bit rate
 *   3. PHY bias/bandgap on, PLL on, wait for lock
 *   4. controller: video mapping, sync polarity, AVI InfoFrame
 *   5. SCDC 1/40 clock ratio + scrambling if the TMDS rate needs it
 *   6. enable the lane serializers
 *
 * The pixel clock is not restricted to a fixed mode table: the ROPLL
 * dividers are computed for any pixel clock the PHY can reach.  That is a
 * requirement for this board, whose display path is
 *
 *   VOP -> HDMI -> Toshiba TC358870XBG -> 2x MIPI DSI panel
 *
 * and therefore uses non-CEA timings (e.g. 1440x720@60 split by the
 * bridge into two 720x720 panels) as well as the standard 1920x1080@60
 * and 1280x720@60 modes.
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
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_hdmi.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_hdmi.h"
#include "rk3576_vop.h"

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Reference clock of the HDPTX PHY: the 24 MHz crystal. */

#define RK3576_HDMI_PHY_REFCLK 24000000u

/* ROPLL VCO range.  The divider search keeps the VCO inside this window.
 *
 * TODO: verify the exact VCO limits against the RK3576 TRM; the values
 * below are the conservative Samsung HDPTX figures used on RK3588.
 */

#define RK3576_HDMI_VCO_MIN 1600000000ull
#define RK3576_HDMI_VCO_MAX 3200000000ull

/* ROPLL field limits */

#define RK3576_HDMI_ROPLL_MDIV_MIN   32u
#define RK3576_HDMI_ROPLL_MDIV_MAX   255u
#define RK3576_HDMI_ROPLL_REFDIV_MAX 4u
#define RK3576_HDMI_ROPLL_SDM_DENO   0x40u

/* Sigma-delta fractional resolution: numerator is expressed in units of
 * 1/RK3576_HDMI_ROPLL_SDM_DENO of one MDIV step.
 */

#define RK3576_HDMI_ROPLL_SDIV_MAX 16u

/* TMDS: ten bits are serialised per pixel clock per lane at 8 bpc. */

#define RK3576_HDMI_TMDS_BITS_PER_PIXEL 10u

/* Poll limits (microseconds) */

#define RK3576_HDMI_PLL_LOCK_TIMEOUT_US 20000
#define RK3576_HDMI_I2CM_TIMEOUT_US     100000
#define RK3576_HDMI_POLL_STEP_US        100

/* DDC transfers are done four bytes at a time (one RDDATA word). */

#define RK3576_HDMI_I2CM_CHUNK 4

/* AVI InfoFrame, CTA-861: type 0x82, version 2, length 13. */

#define RK3576_HDMI_AVI_TYPE    0x82
#define RK3576_HDMI_AVI_VERSION 0x02
#define RK3576_HDMI_AVI_LENGTH  13

/* Audio InfoFrame, CTA-861: type 0x84, version 1, length 10. */

#define RK3576_HDMI_AUDI_TYPE    0x84
#define RK3576_HDMI_AUDI_VERSION 0x01
#define RK3576_HDMI_AUDI_LENGTH  10

/* AVI byte 1: RGB output, active format information valid, no bar data */

#define RK3576_HDMI_AVI_Y_RGB    (0x0 << 5)
#define RK3576_HDMI_AVI_A_ACTIVE (0x1 << 4)
#define RK3576_HDMI_AVI_S_NONE   (0x0 << 0)

/* AVI byte 2: colorimetry + picture aspect ratio + active format */

#define RK3576_HDMI_AVI_C_NODATA (0x0 << 6)
#define RK3576_HDMI_AVI_M_NODATA (0x0 << 4)
#define RK3576_HDMI_AVI_M_4_3    (0x1 << 4)
#define RK3576_HDMI_AVI_M_16_9   (0x2 << 4)
#define RK3576_HDMI_AVI_R_SAME   (0x8 << 0)

/* AVI byte 4: video identification code (VIC) of the transmitted mode */

#define RK3576_HDMI_VIC_NONE    0
#define RK3576_HDMI_VIC_720P60  4
#define RK3576_HDMI_VIC_1080P60 16

/* Audio clock regeneration: the "N" values recommended by the HDMI
 * specification for TMDS rates that are integer multiples of 27 MHz.
 */

#define RK3576_HDMI_ACR_N_32K  4096
#define RK3576_HDMI_ACR_N_44K1 6272
#define RK3576_HDMI_ACR_N_48K  6144

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Computed HDPTX ROPLL configuration for one TMDS bit rate. */

struct rk3576_hdmi_ropll_s
{
  uint32_t refdiv;   /* Reference pre-divider, 1..4                       */
  uint32_t mdiv;     /* Feedback (integer part)                           */
  uint32_t sdiv;     /* Post divider, power of two 1..16                  */
  uint32_t sdm_num;  /* Fractional numerator, 0..(sdm_deno - 1)           */
  uint32_t sdm_deno; /* Fractional denominator                            */
  bool sdm_en;       /* Fractional mode required                          */
  uint64_t fout;     /* Resulting bit clock in Hz                         */
};

struct rk3576_hdmi_dev_s
{
  bool inited;                       /* rk3576_hdmi_initialize() done      */
  bool enabled;                      /* TMDS output running                */
  bool hpd;                          /* Latest hot-plug detect level       */
  uint32_t pixclk;                   /* Pixel clock of the active mode     */
  struct rk3576_vop_timing_s timing; /* Active mode                       */
  struct clk_s *pclk;                /* Controller APB clock               */
  struct clk_s *hclk;                /* VO1 AHB clock                      */
  struct clk_s *refclk;              /* hdmitx_ref                         */
  struct clk_s *hpdclk;              /* Hot-plug detect clock              */
  struct clk_s *earcclk;             /* eARC clock (kept on for the core)  */
  struct clk_s *audclk;              /* Audio sampler clock                */
  struct clk_s *phy_pclk;            /* HDPTX PHY APB clock                */
  struct clk_s *phy_refclk;          /* HDPTX PHY reference clock          */
  uint32_t pclk_hz;                  /* Measured APB rate, for the timers  */
  mutex_t lock;                      /* Serialises DDC and mode changes    */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_hdmi_getreg(uint32_t off);
static inline void rk3576_hdmi_putreg(uint32_t off, uint32_t val);
static inline void rk3576_hdmi_modreg(uint32_t off, uint32_t clrbits,
                                      uint32_t setbits);
static inline uint32_t rk3576_hdmi_phy_getreg(uint32_t off);
static inline void rk3576_hdmi_phy_putreg(uint32_t off, uint32_t val);
static inline void rk3576_hdmi_phy_modreg(uint32_t off, uint32_t clrbits,
                                          uint32_t setbits);
static inline void rk3576_hdmi_grf_putreg(uintptr_t base, uint32_t off,
                                          uint32_t mask, uint32_t val);
static inline uint32_t rk3576_hdmi_grf_getreg(uintptr_t base, uint32_t off);

static int rk3576_hdmi_clk_init(void);

static int rk3576_hdmi_ropll_calc(uint64_t bitrate,
                                  struct rk3576_hdmi_ropll_s *cfg);
static int rk3576_hdmi_phy_power_on(uint32_t pixclk);
static void rk3576_hdmi_phy_power_off(void);
static void rk3576_hdmi_phy_lane_enable(bool enable);

static void rk3576_hdmi_ctrl_reset(void);
static void rk3576_hdmi_set_video_mode(const struct rk3576_vop_timing_s *t);
static void rk3576_hdmi_write_infoframe(uint32_t base, const uint8_t *frame,
                                        size_t len);
static void rk3576_hdmi_avi_infoframe(const struct rk3576_vop_timing_s *t);
static uint8_t rk3576_hdmi_lookup_vic(const struct rk3576_vop_timing_s *t);
static int rk3576_hdmi_scdc_write(uint8_t offset, uint8_t value);
static int rk3576_hdmi_scdc_setup(uint32_t pixclk);

static int rk3576_hdmi_i2cm_wait(void);
static int rk3576_hdmi_ddc_read(uint8_t slave, uint8_t segment, uint8_t offset,
                                uint8_t *buf, size_t len);

static int rk3576_hdmi_interrupt(int irq, void *context, void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_hdmi_dev_s g_rk3576_hdmi;

/* HDPTX PHY TMDS mode common initialisation.
 *
 * TODO: this is the minimum protocol/bus-width/lane-mux programming that
 * is common to every TMDS rate.  The complete vendor initialisation
 * sequence (bias trims, equaliser and slew-rate settings) still has to be
 * transcribed from the RK3576 TRM "HDPTX PHY" chapter; without it the
 * PHY runs on its reset defaults, which is sufficient for short cables
 * but is not the characterised operating point.
 */

struct rk3576_hdmi_phy_init_s
{
  uint32_t off; /* Byte offset inside the PHY register file */
  uint32_t val; /* Value to write                           */
};

static const struct rk3576_hdmi_phy_init_s g_rk3576_hdmi_phy_tmds_init[] = {
  /* Select the TMDS protocol and a 40-bit parallel data bus (10 bits per
   * lane per pixel clock, four pixel clocks per parallel word).
   */

  { RK3576_HDPTX_LNTOP_REG0200, RK3576_HDPTX_LNTOP_PROT_TMDS },
  { RK3576_HDPTX_LNTOP_REG0206, RK3576_HDPTX_LNTOP_WIDTH_40BIT },
  { RK3576_HDPTX_LNTOP_REG0207, RK3576_HDPTX_LNTOP_WIDTH_40BIT },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_getreg / rk3576_hdmi_putreg / rk3576_hdmi_modreg
 *
 * Description:
 *   32-bit accessors for the HDMI TX controller register file.
 ****************************************************************************/

static inline uint32_t rk3576_hdmi_getreg(uint32_t off)
{
  return getreg32(RK3576_HDMITX0_ADDR + off);
}

static inline void rk3576_hdmi_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, RK3576_HDMITX0_ADDR + off);
}

static inline void rk3576_hdmi_modreg(uint32_t off, uint32_t clrbits,
                                      uint32_t setbits)
{
  uint32_t regval = rk3576_hdmi_getreg(off);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_hdmi_putreg(off, regval);
}

/****************************************************************************
 * Name: rk3576_hdmi_phy_getreg / _putreg / _modreg
 *
 * Description:
 *   32-bit accessors for the HDPTX PHY register file.
 ****************************************************************************/

static inline uint32_t rk3576_hdmi_phy_getreg(uint32_t off)
{
  return getreg32(RK3576_HDPTXPHY_ADDR + off);
}

static inline void rk3576_hdmi_phy_putreg(uint32_t off, uint32_t val)
{
  putreg32(val, RK3576_HDPTXPHY_ADDR + off);
}

static inline void rk3576_hdmi_phy_modreg(uint32_t off, uint32_t clrbits,
                                          uint32_t setbits)
{
  uint32_t regval = rk3576_hdmi_phy_getreg(off);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_hdmi_phy_putreg(off, regval);
}

/****************************************************************************
 * Name: rk3576_hdmi_grf_putreg / rk3576_hdmi_grf_getreg
 *
 * Description:
 *   GRF registers use the Rockchip hiword-mask scheme: the upper 16 bits
 *   are the write-enable mask for the lower 16.
 ****************************************************************************/

static inline void rk3576_hdmi_grf_putreg(uintptr_t base, uint32_t off,
                                          uint32_t mask, uint32_t val)
{
  putreg32(RK3576_HDMI_HIWORD(mask, val), base + off);
}

static inline uint32_t rk3576_hdmi_grf_getreg(uintptr_t base, uint32_t off)
{
  return getreg32(base + off);
}

/****************************************************************************
 * Name: rk3576_hdmi_clk_init
 *
 * Description:
 *   Acquire and enable every clock the HDMI TX controller and the HDPTX
 *   PHY need.  All clock handling of this driver lives in this one
 *   function so that a change in the clock tree only touches one place.
 *
 *   Clock names follow the vendor DTS clock-names of hdmi@27da0000
 *   ("pclk", "hpd", "earc", "hdmitx_ref", "aud", "hclk_vo1") and of
 *   hdmiphy@2b000000 ("apb", "ref").
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_hdmi_clk_init(void)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;
  static const char *const names[] = {
    "pclk_hdmitx0_en",     /* controller APB                              */
    "hclk_vo1_en",         /* VO1 AHB, parent bus of the HDMI TX          */
    "clk_hdmitx0_ref_en",  /* hdmitx_ref, core reference                  */
    "clk_hdmitx0_hpd_en",  /* hot-plug detect sampling clock              */
    "clk_hdmitx0_earc_en", /* eARC, also clocks the packet scheduler      */
    "clk_hdmitx0_aud_en",  /* audio sampler                               */
    "pclk_hdptx0_en",      /* PHY APB                                     */
    "clk_hdptx0_ref_en",   /* PHY reference (24 MHz crystal)              */
  };

  struct clk_s **slots[] = {
    &priv->pclk,    &priv->hclk,   &priv->refclk,   &priv->hpdclk,
    &priv->earcclk, &priv->audclk, &priv->phy_pclk, &priv->phy_refclk,
  };

  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
    {
      struct clk_s *clk = clk_get(names[i]);

      if (clk == NULL)
        {
          gerr("ERROR: HDMI: clk_get(%s) failed\n", names[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          gerr("ERROR: HDMI: clk_enable(%s) failed: %d\n", names[i], ret);
          return ret;
        }

      *slots[i] = clk;
    }

  /* The controller derives its internal microsecond timers from the APB
   * clock, so the real rate has to be read back rather than assumed.
   */

  priv->pclk_hz = clk_get_rate(priv->pclk);
  if (priv->pclk_hz == 0)
    {
      gerr("ERROR: HDMI: pclk rate reads back as 0\n");
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_ropll_calc
 *
 * Description:
 *   Find HDPTX ROPLL dividers for the requested serial bit rate.
 *
 *     fvco = fref / refdiv * (mdiv + sdm_num / sdm_deno)
 *     fout = fvco / sdiv
 *
 *   The search walks refdiv and sdiv (a power of two) and keeps the
 *   solution whose |fout - bitrate| is smallest, preferring integer-N
 *   configurations because they have lower jitter.
 *
 * Input Parameters:
 *   bitrate - Requested serial bit rate per lane, in Hz
 *   cfg     - Receives the divider set
 *
 * Returned Value:
 *   OK if a configuration within 0.5 per mille was found, -EINVAL if not.
 *
 ****************************************************************************/

static int rk3576_hdmi_ropll_calc(uint64_t bitrate,
                                  struct rk3576_hdmi_ropll_s *cfg)
{
  uint64_t best_err = UINT64_MAX;
  uint32_t refdiv;
  uint32_t sdiv;
  bool found = false;

  if (bitrate == 0)
    {
      return -EINVAL;
    }

  for (refdiv = 1; refdiv <= RK3576_HDMI_ROPLL_REFDIV_MAX; refdiv++)
    {
      uint64_t fref = RK3576_HDMI_PHY_REFCLK / refdiv;

      if (fref == 0)
        {
          continue;
        }

      for (sdiv = 1; sdiv <= RK3576_HDMI_ROPLL_SDIV_MAX; sdiv <<= 1)
        {
          uint64_t fvco = bitrate * sdiv;
          uint64_t scaled;
          uint64_t mdiv;
          uint64_t num;
          uint64_t fout;
          uint64_t err;

          if (fvco < RK3576_HDMI_VCO_MIN || fvco > RK3576_HDMI_VCO_MAX)
            {
              continue;
            }

          /* Feedback ratio in units of 1/sdm_deno. */

          scaled = (fvco * RK3576_HDMI_ROPLL_SDM_DENO) / fref;
          mdiv = scaled / RK3576_HDMI_ROPLL_SDM_DENO;
          num = scaled % RK3576_HDMI_ROPLL_SDM_DENO;

          if (mdiv < RK3576_HDMI_ROPLL_MDIV_MIN ||
              mdiv > RK3576_HDMI_ROPLL_MDIV_MAX)
            {
              continue;
            }

          fout = (fref * (mdiv * RK3576_HDMI_ROPLL_SDM_DENO + num)) /
                 (RK3576_HDMI_ROPLL_SDM_DENO * sdiv);

          err = (fout > bitrate) ? (fout - bitrate) : (bitrate - fout);

          /* Prefer an exact integer-N hit over a marginally closer
           * fractional one: the sigma-delta modulator adds jitter.
           */

          if (num == 0 && err == 0)
            {
              best_err = 0;
            }
          else if (err >= best_err)
            {
              continue;
            }
          else
            {
              best_err = err;
            }

          cfg->refdiv = refdiv;
          cfg->mdiv = (uint32_t)mdiv;
          cfg->sdiv = sdiv;
          cfg->sdm_num = (uint32_t)num;
          cfg->sdm_deno = RK3576_HDMI_ROPLL_SDM_DENO;
          cfg->sdm_en = (num != 0);
          cfg->fout = fout;
          found = true;

          if (best_err == 0)
            {
              return OK;
            }
        }
    }

  if (!found)
    {
      return -EINVAL;
    }

  /* Reject anything worse than 0.05%: HDMI sinks tolerate roughly 0.5%
   * on the TMDS clock, keep an order of magnitude of margin.
   */

  if (best_err > bitrate / 2000)
    {
      gerr("ERROR: HDMI: no ROPLL setting for %llu Hz (best error %llu)\n",
           (unsigned long long)bitrate, (unsigned long long)best_err);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_phy_power_off
 *
 * Description:
 *   Disable the lane serializers and drop the PLL/bias enables in the
 *   HDPTX PHY GRF.
 ****************************************************************************/

static void rk3576_hdmi_phy_power_off(void)
{
  rk3576_hdmi_phy_lane_enable(false);

  rk3576_hdmi_phy_modreg(RK3576_HDPTX_CMN_REG0008, RK3576_HDPTX_CMN_ROPLL_EN,
                         0);

  rk3576_hdmi_grf_putreg(RK3576_HDPTXPHY_GRF_ADDR, RK3576_HDPTXPHY_GRF_CON0,
                         RK3576_HDPTXPHY_I_PLL_EN | RK3576_HDPTXPHY_I_BIAS_EN |
                             RK3576_HDPTXPHY_I_BGR_EN,
                         0);
}

/****************************************************************************
 * Name: rk3576_hdmi_phy_lane_enable
 *
 * Description:
 *   Enable or disable the four TMDS lane serializers (three data lanes
 *   plus the clock lane).
 ****************************************************************************/

static void rk3576_hdmi_phy_lane_enable(bool enable)
{
  unsigned int lane;

  for (lane = 0; lane < RK3576_HDPTX_LANE_NUM; lane++)
    {
      uint32_t off = RK3576_HDPTX_LANE_REG(lane, RK3576_HDPTX_LANE_SER_EN);

      if (enable)
        {
          rk3576_hdmi_phy_modreg(off, 0, RK3576_HDPTX_LANE_SER_EN_BIT);
        }
      else
        {
          rk3576_hdmi_phy_modreg(off, RK3576_HDPTX_LANE_SER_EN_BIT, 0);
        }
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_phy_power_on
 *
 * Description:
 *   Program the ROPLL for the TMDS bit rate implied by pixclk, power the
 *   PHY up and wait for PLL lock.  The lane serializers stay off; they
 *   are enabled once the controller is configured.
 *
 * Input Parameters:
 *   pixclk - Pixel clock of the mode, in Hz
 *
 * Returned Value:
 *   OK on success; -EINVAL if the rate is unreachable; -ETIMEDOUT if the
 *   PLL does not lock.
 *
 ****************************************************************************/

static int rk3576_hdmi_phy_power_on(uint32_t pixclk)
{
  struct rk3576_hdmi_ropll_s cfg;
  uint64_t bitrate;
  unsigned int i;
  int elapsed;
  int ret;

  bitrate = (uint64_t)pixclk * RK3576_HDMI_TMDS_BITS_PER_PIXEL;

  ret = rk3576_hdmi_ropll_calc(bitrate, &cfg);
  if (ret < 0)
    {
      return ret;
    }

  ginfo("HDMI: PHY %" PRIu32 " Hz pixclk -> refdiv %" PRIu32 " mdiv %" PRIu32
        " sdiv %" PRIu32 " num %" PRIu32 "\n",
        pixclk, cfg.refdiv, cfg.mdiv, cfg.sdiv, cfg.sdm_num);

  /* Start from a known-off state. */

  rk3576_hdmi_phy_power_off();

  /* Protocol / bus width common setup. */

  for (i = 0; i < sizeof(g_rk3576_hdmi_phy_tmds_init) /
                      sizeof(g_rk3576_hdmi_phy_tmds_init[0]);
       i++)
    {
      rk3576_hdmi_phy_putreg(g_rk3576_hdmi_phy_tmds_init[i].off,
                             g_rk3576_hdmi_phy_tmds_init[i].val);
    }

  /* ROPLL dividers.  MDIV_AFC tracks MDIV (the automatic frequency
   * calibration loop uses the same target).
   */

  rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_PMS_MDIV, cfg.mdiv);
  rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_PMS_MDIV_AFC, cfg.mdiv);
  rk3576_hdmi_phy_putreg(
      RK3576_HDPTX_CMN_ROPLL_PMS_PDIV,
      (1u << RK3576_HDPTX_CMN_ROPLL_PDIV_SHIFT) |
          (cfg.refdiv & RK3576_HDPTX_CMN_ROPLL_REFDIV_MASK));

  /* SDIV is encoded as the power-of-two exponent. */

  {
    uint32_t sdiv_exp = 0;
    uint32_t tmp = cfg.sdiv;

    while (tmp > 1)
      {
        tmp >>= 1;
        sdiv_exp++;
      }

    rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_PMS_SDIV,
                           sdiv_exp << RK3576_HDPTX_CMN_ROPLL_SDIV_SHIFT);
  }

  if (cfg.sdm_en)
    {
      rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_SDM_DENO, cfg.sdm_deno);
      rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_SDM_NUM_SIGN, 0);
      rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_SDM_NUM, cfg.sdm_num);
      rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_SDM_EN,
                             RK3576_HDPTX_CMN_ROPLL_SDM_EN_BIT |
                                 RK3576_HDPTX_CMN_ROPLL_SDM_RSTN_BIT);
    }
  else
    {
      rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_SDM_EN,
                             RK3576_HDPTX_CMN_ROPLL_SDM_RSTN_BIT);
    }

  /* Release the I/Q divider reset and enable the ROPLL core. */

  rk3576_hdmi_phy_putreg(RK3576_HDPTX_CMN_ROPLL_IQDIV_RSTN,
                         RK3576_HDPTX_CMN_ROPLL_IQDIV_RSTN_B);
  rk3576_hdmi_phy_modreg(RK3576_HDPTX_CMN_REG0008, 0,
                         RK3576_HDPTX_CMN_ROPLL_EN);

  /* Bandgap, bias, then PLL, via the PHY GRF. */

  rk3576_hdmi_grf_putreg(RK3576_HDPTXPHY_GRF_ADDR, RK3576_HDPTXPHY_GRF_CON0,
                         RK3576_HDPTXPHY_I_BGR_EN | RK3576_HDPTXPHY_I_BIAS_EN,
                         RK3576_HDPTXPHY_I_BGR_EN | RK3576_HDPTXPHY_I_BIAS_EN);
  up_udelay(10);
  rk3576_hdmi_grf_putreg(RK3576_HDPTXPHY_GRF_ADDR, RK3576_HDPTXPHY_GRF_CON0,
                         RK3576_HDPTXPHY_I_PLL_EN, RK3576_HDPTXPHY_I_PLL_EN);

  for (elapsed = 0; elapsed < RK3576_HDMI_PLL_LOCK_TIMEOUT_US;
       elapsed += RK3576_HDMI_POLL_STEP_US)
    {
      uint32_t status = rk3576_hdmi_grf_getreg(RK3576_HDPTXPHY_GRF_ADDR,
                                               RK3576_HDPTXPHY_GRF_STATUS0);

      if ((status & RK3576_HDPTXPHY_O_PLL_LOCK_DONE) != 0 &&
          (status & RK3576_HDPTXPHY_O_PHY_CLK_RDY) != 0)
        {
          ginfo("HDMI: PHY PLL locked after %d us\n", elapsed);
          return OK;
        }

      up_udelay(RK3576_HDMI_POLL_STEP_US);
    }

  gerr("ERROR: HDMI: PHY PLL failed to lock (status 0x%08" PRIx32 ")\n",
       rk3576_hdmi_grf_getreg(RK3576_HDPTXPHY_GRF_ADDR,
                              RK3576_HDPTXPHY_GRF_STATUS0));
  rk3576_hdmi_phy_power_off();
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_hdmi_ctrl_reset
 *
 * Description:
 *   Pulse the controller's internal audio/video datapath reset and clear
 *   any latched interrupt status.  The CRU-level "ref" and "hdp" resets
 *   are left alone: the IP-internal reset is enough to get the datapath
 *   into a defined state and it does not require guessing CRU softrst bit
 *   positions.
 ****************************************************************************/

static void rk3576_hdmi_ctrl_reset(void)
{
  /* Hold the AVP datapath disabled while it is reset. */

  rk3576_hdmi_putreg(RK3576_HDMI_GLOBAL_SWDISABLE,
                     RK3576_HDMI_SWDIS_AVP_DATAPATH);
  rk3576_hdmi_putreg(RK3576_HDMI_GLOBAL_SWRESET_REQUEST,
                     RK3576_HDMI_SWRST_AVP | RK3576_HDMI_SWRST_I2CM);
  up_udelay(10);
  rk3576_hdmi_putreg(RK3576_HDMI_GLOBAL_SWRESET_REQUEST, 0);
  rk3576_hdmi_putreg(RK3576_HDMI_GLOBAL_SWDISABLE, 0);

  /* Clear all latched interrupt sources. */

  rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_0_INT_CLEAR, UINT32_MAX);
  rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR, UINT32_MAX);
  rk3576_hdmi_putreg(RK3576_HDMI_AVP_0_INT_CLEAR, UINT32_MAX);
  rk3576_hdmi_putreg(RK3576_HDMI_AVP_1_INT_CLEAR, UINT32_MAX);
}

/****************************************************************************
 * Name: rk3576_hdmi_set_video_mode
 *
 * Description:
 *   Program the video input interface: colour mapping, sync polarity and
 *   the active/blanking geometry used by the packet scheduler.
 ****************************************************************************/

static void rk3576_hdmi_set_video_mode(const struct rk3576_vop_timing_s *t)
{
  uint32_t hblank = (uint32_t)t->hfp + t->hsync + t->hbp;
  uint32_t vblank = (uint32_t)t->vfp + t->vsync + t->vbp;
  uint32_t cfg0;

  /* RGB 8 bpc, no colour space conversion.  Active-low sync is signalled
   * to the encoder by the POL_LOW bits.
   */

  cfg0 = RK3576_HDMI_VID_MAP_RGB_8BIT << RK3576_HDMI_VID_MAP_CFG_SHIFT;

  if (!t->hsync_active_high)
    {
      cfg0 |= RK3576_HDMI_VID_HSYNC_POL_LOW;
    }

  if (!t->vsync_active_high)
    {
      cfg0 |= RK3576_HDMI_VID_VSYNC_POL_LOW;
    }

  rk3576_hdmi_putreg(RK3576_HDMI_VIDEO_INTERFACE_CONFIG0, cfg0);

  rk3576_hdmi_putreg(RK3576_HDMI_VIDEO_INTERFACE_CONFIG1,
                     ((uint32_t)t->hact << RK3576_HDMI_VID_HACTIVE_SHIFT) |
                         ((uint32_t)t->vact << RK3576_HDMI_VID_VACTIVE_SHIFT));

  rk3576_hdmi_putreg(RK3576_HDMI_VIDEO_INTERFACE_CONFIG2,
                     (hblank << RK3576_HDMI_VID_HBLANK_SHIFT) |
                         (vblank << RK3576_HDMI_VID_VBLANK_SHIFT));

  /* HDMI mode (not DVI): required for InfoFrames and audio. */

  rk3576_hdmi_modreg(RK3576_HDMI_LINK_CONFIG0, RK3576_HDMI_LINK_OPMODE_DVI, 0);
}

/****************************************************************************
 * Name: rk3576_hdmi_write_infoframe
 *
 * Description:
 *   Copy a complete InfoFrame (3 header bytes followed by the body) into
 *   one of the packet windows.  Word 0 holds the header, the remaining
 *   words hold four body bytes each, little endian.
 *
 * Input Parameters:
 *   base  - Packet window base offset (RK3576_HDMI_PKT_*_CONTENTS0)
 *   frame - Header + body
 *   len   - Total length of frame in bytes (3 + body length)
 *
 ****************************************************************************/

static void rk3576_hdmi_write_infoframe(uint32_t base, const uint8_t *frame,
                                        size_t len)
{
  uint32_t words[RK3576_HDMI_PKT_CONTENTS_WORDS];
  size_t body_len = len - 3;
  size_t i;

  memset(words, 0, sizeof(words));

  words[0] = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
             ((uint32_t)frame[2] << 16);

  for (i = 0; i < body_len; i++)
    {
      size_t word = 1 + (i / 4);

      if (word >= RK3576_HDMI_PKT_CONTENTS_WORDS)
        {
          break;
        }

      words[word] |= (uint32_t)frame[3 + i] << ((i % 4) * 8);
    }

  for (i = 0; i < RK3576_HDMI_PKT_CONTENTS_WORDS; i++)
    {
      rk3576_hdmi_putreg(base + (uint32_t)(i * 4), words[i]);
    }
}

/****************************************************************************
 * Name: rk3576_hdmi_lookup_vic
 *
 * Description:
 *   Map a timing to its CTA-861 video identification code.  Only the two
 *   standard modes this board uses have a VIC; the custom dual-panel
 *   timings driving the TC358870 bridge are not CTA modes and correctly
 *   report VIC 0.
 ****************************************************************************/

static uint8_t rk3576_hdmi_lookup_vic(const struct rk3576_vop_timing_s *t)
{
  if (t->hact == 1920 && t->vact == 1080 && t->pixclk == 148500000)
    {
      return RK3576_HDMI_VIC_1080P60;
    }

  if (t->hact == 1280 && t->vact == 720 && t->pixclk == 74250000)
    {
      return RK3576_HDMI_VIC_720P60;
    }

  return RK3576_HDMI_VIC_NONE;
}

/****************************************************************************
 * Name: rk3576_hdmi_avi_infoframe
 *
 * Description:
 *   Build and arm the AVI InfoFrame for the current mode.  RGB full-range
 *   output, no colorimetry override; the aspect ratio is derived from the
 *   active geometry.
 ****************************************************************************/

static void rk3576_hdmi_avi_infoframe(const struct rk3576_vop_timing_s *t)
{
  uint8_t frame[3 + RK3576_HDMI_AVI_LENGTH];
  uint32_t sum = 0;
  unsigned int i;

  memset(frame, 0, sizeof(frame));

  frame[0] = RK3576_HDMI_AVI_TYPE;
  frame[1] = RK3576_HDMI_AVI_VERSION;
  frame[2] = RK3576_HDMI_AVI_LENGTH;

  /* frame[3] is the checksum, filled in below. */

  frame[4] = RK3576_HDMI_AVI_Y_RGB | RK3576_HDMI_AVI_A_ACTIVE |
             RK3576_HDMI_AVI_S_NONE;

  frame[5] = RK3576_HDMI_AVI_C_NODATA | RK3576_HDMI_AVI_R_SAME;
  if (t->hact * 9 == t->vact * 16)
    {
      frame[5] |= RK3576_HDMI_AVI_M_16_9;
    }
  else if (t->hact * 3 == t->vact * 4)
    {
      frame[5] |= RK3576_HDMI_AVI_M_4_3;
    }
  else
    {
      frame[5] |= RK3576_HDMI_AVI_M_NODATA;
    }

  /* frame[6] (byte 3) leaves scaling / extended colorimetry at zero. */

  frame[7] = rk3576_hdmi_lookup_vic(t);

  /* frame[8] (byte 5) pixel repetition: none. */

  for (i = 0; i < sizeof(frame); i++)
    {
      sum += frame[i];
    }

  frame[3] = (uint8_t)(0x100u - (sum & 0xff));

  rk3576_hdmi_write_infoframe(RK3576_HDMI_PKT_AVI_CONTENTS0, frame,
                              sizeof(frame));

  /* Send it once per frame, automatically. */

  rk3576_hdmi_modreg(RK3576_HDMI_PKTSCHED_PKT_SEND_AUTO, 0,
                     RK3576_HDMI_PKTSCHED_AVI_TX_EN);
  rk3576_hdmi_modreg(RK3576_HDMI_PKTSCHED_PKT_EN, 0,
                     RK3576_HDMI_PKTSCHED_AVI_TX_EN);
}

/****************************************************************************
 * Name: rk3576_hdmi_scdc_write
 *
 * Description:
 *   Write one SCDC register in the sink over the DDC channel.
 ****************************************************************************/

static int rk3576_hdmi_scdc_write(uint8_t offset, uint8_t value)
{
  int ret;

  rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                     RK3576_HDMI_I2CM_IRQ_ALL);

  rk3576_hdmi_putreg(
      RK3576_HDMI_I2CM_INTERFACE_CONTROL1,
      ((uint32_t)RK3576_HDMI_SCDC_ADDR << RK3576_HDMI_I2CM_SLVADDR_SHIFT) |
          ((uint32_t)offset << RK3576_HDMI_I2CM_ADDR_SHIFT));

  rk3576_hdmi_putreg(RK3576_HDMI_I2CM_INTERFACE_WRDATA0, value);

  rk3576_hdmi_putreg(RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
                     RK3576_HDMI_I2CM_WR_WRITE |
                         (0u << RK3576_HDMI_I2CM_NBYTES_SHIFT));

  rk3576_hdmi_putreg(RK3576_HDMI_I2CM_CONTROL0, RK3576_HDMI_I2CM_EXECUTE);

  ret = rk3576_hdmi_i2cm_wait();
  return ret;
}

/****************************************************************************
 * Name: rk3576_hdmi_scdc_setup
 *
 * Description:
 *   Above 340 MHz TMDS character rate the link must run at a 1/40 clock
 *   ratio with scrambling enabled on both ends.  Below that threshold
 *   both are switched off again.
 *
 *   SCDC offset 0x20 is TMDS_Config: bit0 = scrambling enable,
 *   bit1 = TMDS bit clock ratio 1/40.
 ****************************************************************************/

static int rk3576_hdmi_scdc_setup(uint32_t pixclk)
{
  bool high_tmds = pixclk > RK3576_HDMI_TMDS_SCDC_THRESHOLD;
  int ret;

  ret = rk3576_hdmi_scdc_write(0x20, high_tmds ? 0x03 : 0x00);
  if (ret < 0)
    {
      /* A DVI sink or an adapter without SCDC will NAK.  That is only
       * fatal when the link actually needs the high-rate configuration.
       */

      if (high_tmds)
        {
          gerr("ERROR: HDMI: sink rejected SCDC TMDS config: %d\n", ret);
          return ret;
        }

      gwarn("HDMI: sink has no SCDC (%d), continuing at <=340 MHz\n", ret);
      ret = OK;
    }

  if (high_tmds)
    {
      rk3576_hdmi_modreg(RK3576_HDMI_SCRAMB_CONFIG0, 0, RK3576_HDMI_SCRAMB_EN);
    }
  else
    {
      rk3576_hdmi_modreg(RK3576_HDMI_SCRAMB_CONFIG0, RK3576_HDMI_SCRAMB_EN, 0);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_hdmi_i2cm_wait
 *
 * Description:
 *   Poll the MAINUNIT_1 interrupt status for completion of the current
 *   I2C master operation.  Polling rather than waiting on the IRQ keeps
 *   EDID reads usable from board bring-up context.
 *
 * Returned Value:
 *   OK on completion, -ENXIO on NAK, -EIO on arbitration loss,
 *   -ETIMEDOUT if the core never reports a result.
 *
 ****************************************************************************/

static int rk3576_hdmi_i2cm_wait(void)
{
  int elapsed;

  for (elapsed = 0; elapsed < RK3576_HDMI_I2CM_TIMEOUT_US;
       elapsed += RK3576_HDMI_POLL_STEP_US)
    {
      uint32_t status = rk3576_hdmi_getreg(RK3576_HDMI_MAINUNIT_1_INT_STATUS);

      if ((status & RK3576_HDMI_I2CM_NACK_RCVD_IRQ) != 0)
        {
          rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                             RK3576_HDMI_I2CM_IRQ_ALL);
          return -ENXIO;
        }

      if ((status & RK3576_HDMI_I2CM_ARB_LOST_IRQ) != 0)
        {
          rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                             RK3576_HDMI_I2CM_IRQ_ALL);
          return -EIO;
        }

      if ((status & (RK3576_HDMI_I2CM_OP_DONE_IRQ |
                     RK3576_HDMI_I2CM_READ_REQ_IRQ)) != 0)
        {
          rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                             RK3576_HDMI_I2CM_IRQ_ALL);
          return OK;
        }

      up_udelay(RK3576_HDMI_POLL_STEP_US);
    }

  rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                     RK3576_HDMI_I2CM_IRQ_ALL);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_hdmi_ddc_read
 *
 * Description:
 *   Read len bytes from a DDC slave using the controller's I2C master.
 *   Transfers are issued four bytes at a time because that is the width
 *   of one RDDATA window; the E-DDC segment pointer selects the 256-byte
 *   block for EDID reads beyond block 1.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_hdmi_ddc_read(uint8_t slave, uint8_t segment, uint8_t offset,
                                uint8_t *buf, size_t len)
{
  size_t done = 0;

  while (done < len)
    {
      size_t chunk = len - done;
      uint32_t control1;
      uint32_t rddata;
      size_t i;
      int ret;

      if (chunk > RK3576_HDMI_I2CM_CHUNK)
        {
          chunk = RK3576_HDMI_I2CM_CHUNK;
        }

      rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_1_INT_CLEAR,
                         RK3576_HDMI_I2CM_IRQ_ALL);

      control1 = ((uint32_t)slave << RK3576_HDMI_I2CM_SLVADDR_SHIFT) |
                 ((uint32_t)(offset + done) << RK3576_HDMI_I2CM_ADDR_SHIFT) |
                 ((uint32_t)RK3576_HDMI_DDC_SEGADDR
                  << RK3576_HDMI_I2CM_SEGADDR_SHIFT) |
                 ((uint32_t)segment << RK3576_HDMI_I2CM_SEGPTR_SHIFT);

      rk3576_hdmi_putreg(RK3576_HDMI_I2CM_INTERFACE_CONTROL1, control1);

      /* NBYTES is encoded as "bytes - 1". */

      rk3576_hdmi_putreg(
          RK3576_HDMI_I2CM_INTERFACE_CONTROL0,
          (segment != 0 ? RK3576_HDMI_I2CM_WR_EXT_READ
                        : RK3576_HDMI_I2CM_WR_READ) |
              ((uint32_t)(chunk - 1) << RK3576_HDMI_I2CM_NBYTES_SHIFT));

      rk3576_hdmi_putreg(RK3576_HDMI_I2CM_CONTROL0, RK3576_HDMI_I2CM_EXECUTE);

      ret = rk3576_hdmi_i2cm_wait();
      if (ret < 0)
        {
          return ret;
        }

      rddata = rk3576_hdmi_getreg(RK3576_HDMI_I2CM_INTERFACE_RDDATA0);

      for (i = 0; i < chunk; i++)
        {
          buf[done + i] = (uint8_t)((rddata >> (i * 8)) & 0xff);
        }

      done += chunk;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_interrupt
 *
 * Description:
 *   Hot-plug detect interrupt handler.  Latches the new HPD level and
 *   clears the source; mode setting is left to the caller so that this
 *   handler stays short.
 ****************************************************************************/

static int rk3576_hdmi_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_hdmi_dev_s *priv = (struct rk3576_hdmi_dev_s *)arg;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status =
      rk3576_hdmi_grf_getreg(RK3576_VO1_GRF_ADDR, RK3576_VO1_GRF_SOC_STATUS0);
  priv->hpd = (status & RK3576_VO1_GRF_HDMI_HPD_LEVEL) != 0;

  rk3576_hdmi_putreg(RK3576_HDMI_MAINUNIT_0_INT_CLEAR, UINT32_MAX);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_initialize
 ****************************************************************************/

int rk3576_hdmi_initialize(void)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;
  uint32_t coreid;
  int ret;

  if (priv->inited)
    {
      return OK;
    }

  nxmutex_init(&priv->lock);

  ret = rk3576_hdmi_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  rk3576_hdmi_ctrl_reset();

  coreid = rk3576_hdmi_getreg(RK3576_HDMI_CORE_ID);
  ginfo("HDMI: core id 0x%08" PRIx32 " version 0x%08" PRIx32 "\n", coreid,
        rk3576_hdmi_getreg(RK3576_HDMI_VER_NUMBER));

  if (coreid == 0 || coreid == UINT32_MAX)
    {
      gerr("ERROR: HDMI: controller not responding (core id 0x%08" PRIx32
           ")\n",
           coreid);
      return -ENODEV;
    }

  /* The core's internal timers are expressed in APB clock periods; the
   * timer base register wants the APB rate in MHz.
   */

  rk3576_hdmi_putreg(RK3576_HDMI_TIMER_BASE_CONFIG0,
                     (priv->pclk_hz / 1000000u) & RK3576_HDMI_TIMER_BASE_MASK);

  /* DDC at fast mode (400 kHz): EDID reads of two blocks then take a few
   * milliseconds instead of tens.
   */

  rk3576_hdmi_putreg(RK3576_HDMI_I2CM_CONFIG0, 0);
  rk3576_hdmi_modreg(RK3576_HDMI_I2CM_INTERFACE_CONTROL0, 0,
                     RK3576_HDMI_I2CM_FM_EN);
  rk3576_hdmi_modreg(RK3576_HDMI_SCDC_CONFIG0, 0, RK3576_HDMI_SCDC_I2C_FM_EN);

  /* Sample the initial hot-plug state, then unmask the HPD interrupt. */

  priv->hpd = (rk3576_hdmi_grf_getreg(RK3576_VO1_GRF_ADDR,
                                      RK3576_VO1_GRF_SOC_STATUS0) &
               RK3576_VO1_GRF_HDMI_HPD_LEVEL) != 0;

  ret = irq_attach(RK3576_IRQ_HDMITX_HPD, rk3576_hdmi_interrupt, priv);
  if (ret < 0)
    {
      gerr("ERROR: HDMI: irq_attach failed: %d\n", ret);
      return ret;
    }

  up_enable_irq(RK3576_IRQ_HDMITX_HPD);

  priv->inited = true;

  ginfo("HDMI: initialised, sink %s\n", priv->hpd ? "present" : "absent");
  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_set_mode
 ****************************************************************************/

int rk3576_hdmi_set_mode(const struct rk3576_vop_timing_s *timing)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;
  int ret;

  if (timing == NULL)
    {
      return -EINVAL;
    }

  if (!priv->inited)
    {
      return -EPERM;
    }

  if (timing->pixclk < RK3576_HDMI_PIXCLK_MIN ||
      timing->pixclk > RK3576_HDMI_PIXCLK_MAX)
    {
      gerr("ERROR: HDMI: pixel clock %" PRIu32 " Hz out of range\n",
           timing->pixclk);
      return -EINVAL;
    }

  if (timing->hact == 0 || timing->vact == 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  /* Mute the output while the PHY re-locks. */

  rk3576_hdmi_phy_lane_enable(false);
  rk3576_hdmi_putreg(RK3576_HDMI_PKTSCHED_PKT_EN, 0);
  priv->enabled = false;

  ret = rk3576_hdmi_phy_power_on(timing->pixclk);
  if (ret < 0)
    {
      nxmutex_unlock(&priv->lock);
      return ret;
    }

  rk3576_hdmi_set_video_mode(timing);

  /* SCDC needs the DDC channel, which needs a sink; skip it when nothing
   * is plugged in so that a headless boot still configures the encoder.
   */

  if (priv->hpd)
    {
      ret = rk3576_hdmi_scdc_setup(timing->pixclk);
      if (ret < 0)
        {
          rk3576_hdmi_phy_power_off();
          nxmutex_unlock(&priv->lock);
          return ret;
        }
    }
  else if (timing->pixclk > RK3576_HDMI_TMDS_SCDC_THRESHOLD)
    {
      /* Configure the transmitter side anyway; the sink side will be
       * programmed by the next set_mode after hot plug.
       */

      rk3576_hdmi_modreg(RK3576_HDMI_SCRAMB_CONFIG0, 0, RK3576_HDMI_SCRAMB_EN);
    }

  rk3576_hdmi_avi_infoframe(timing);

  rk3576_hdmi_phy_lane_enable(true);

  priv->timing = *timing;
  priv->pixclk = timing->pixclk;
  priv->enabled = true;

  nxmutex_unlock(&priv->lock);

  ginfo("HDMI: %ux%u @ %" PRIu32 " Hz pixel clock enabled\n", timing->hact,
        timing->vact, timing->pixclk);
  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_disable
 ****************************************************************************/

int rk3576_hdmi_disable(void)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;

  if (!priv->inited)
    {
      return -EPERM;
    }

  nxmutex_lock(&priv->lock);

  rk3576_hdmi_putreg(RK3576_HDMI_PKTSCHED_PKT_EN, 0);
  rk3576_hdmi_phy_power_off();
  priv->enabled = false;
  priv->pixclk = 0;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_hdmi_read_edid
 ****************************************************************************/

ssize_t rk3576_hdmi_read_edid(uint8_t *buf, size_t len)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;
  size_t done = 0;
  int ret = OK;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  if (!priv->inited)
    {
      return -EPERM;
    }

  if (!priv->hpd)
    {
      return -ENODEV;
    }

  if (len > RK3576_HDMI_EDID_MAX_BYTES)
    {
      len = RK3576_HDMI_EDID_MAX_BYTES;
    }

  nxmutex_lock(&priv->lock);

  while (done < len)
    {
      /* The E-DDC segment selects a 256-byte window; the offset pointer
       * addresses inside it.
       */

      size_t abs = done;
      uint8_t segment = (uint8_t)(abs / 256);
      uint8_t offset = (uint8_t)(abs % 256);
      size_t chunk = len - done;

      if (chunk > (size_t)(256 - offset))
        {
          chunk = (size_t)(256 - offset);
        }

      ret = rk3576_hdmi_ddc_read(RK3576_HDMI_DDC_ADDR, segment, offset,
                                 &buf[done], chunk);
      if (ret < 0)
        {
          break;
        }

      done += chunk;
    }

  nxmutex_unlock(&priv->lock);

  if (done == 0 && ret < 0)
    {
      return ret;
    }

  return (ssize_t)done;
}

/****************************************************************************
 * Name: rk3576_hdmi_hpd_status
 ****************************************************************************/

bool rk3576_hdmi_hpd_status(void)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;

  if (!priv->inited)
    {
      return false;
    }

  /* Re-sample rather than trusting the last interrupt: the level is
   * cheap to read and this makes polling callers correct even if the
   * interrupt has not fired yet.
   */

  priv->hpd = (rk3576_hdmi_grf_getreg(RK3576_VO1_GRF_ADDR,
                                      RK3576_VO1_GRF_SOC_STATUS0) &
               RK3576_VO1_GRF_HDMI_HPD_LEVEL) != 0;

  return priv->hpd;
}

/****************************************************************************
 * Name: rk3576_hdmi_audio_config
 ****************************************************************************/

int rk3576_hdmi_audio_config(uint32_t samplerate, uint8_t channels,
                             uint8_t bits)
{
  struct rk3576_hdmi_dev_s *priv = &g_rk3576_hdmi;
  uint8_t frame[3 + RK3576_HDMI_AUDI_LENGTH];
  uint32_t sum = 0;
  uint32_t n;
  uint64_t cts;
  unsigned int i;

  if (!priv->inited)
    {
      return -EPERM;
    }

  if (!priv->enabled || priv->pixclk == 0)
    {
      /* CTS is derived from the pixel clock, so a mode must be active. */

      return -EPERM;
    }

  if (channels < 2 || channels > 8)
    {
      return -EINVAL;
    }

  if (bits != 16 && bits != 20 && bits != 24)
    {
      return -EINVAL;
    }

  /* N from the HDMI specification, scaled for the 2x / 4x rates. */

  switch (samplerate)
    {
      case 32000:
        n = RK3576_HDMI_ACR_N_32K;
        break;
      case 44100:
        n = RK3576_HDMI_ACR_N_44K1;
        break;
      case 48000:
        n = RK3576_HDMI_ACR_N_48K;
        break;
      case 88200:
        n = RK3576_HDMI_ACR_N_44K1 * 2;
        break;
      case 96000:
        n = RK3576_HDMI_ACR_N_48K * 2;
        break;
      case 176400:
        n = RK3576_HDMI_ACR_N_44K1 * 4;
        break;
      case 192000:
        n = RK3576_HDMI_ACR_N_48K * 4;
        break;
      default:
        return -EINVAL;
    }

  /* CTS = (TMDS clock * N) / (128 * fs) */

  cts = ((uint64_t)priv->pixclk * n) / (128ull * samplerate);

  nxmutex_lock(&priv->lock);

  /* I2S input, standard format, from SAI6. */

  rk3576_hdmi_putreg(
      RK3576_HDMI_AUDIO_INTERFACE_CONFIG0,
      RK3576_HDMI_AUD_IFACE_I2S | RK3576_HDMI_AUD_I2S_MODE_STD |
          ((uint32_t)bits << RK3576_HDMI_AUD_WIDTH_SHIFT) |
          ((uint32_t)(channels - 1) << RK3576_HDMI_AUD_CHANNELS_SHIFT));

  rk3576_hdmi_putreg(RK3576_HDMI_AUDPKT_ACR_CONFIG0,
                     (n << RK3576_HDMI_AUDPKT_ACR_N_SHIFT) &
                         RK3576_HDMI_AUDPKT_ACR_N_MASK);
  rk3576_hdmi_putreg(RK3576_HDMI_AUDPKT_ACR_CONFIG1,
                     ((uint32_t)cts << RK3576_HDMI_AUDPKT_ACR_CTS_SHIFT) &
                         RK3576_HDMI_AUDPKT_ACR_CTS_MASK);
  rk3576_hdmi_putreg(RK3576_HDMI_AUDPKT_ACR_CONTROL0,
                     RK3576_HDMI_AUDPKT_ACR_CTS_SW_SEL);

  /* Audio InfoFrame: channel count, everything else "refer to stream". */

  memset(frame, 0, sizeof(frame));
  frame[0] = RK3576_HDMI_AUDI_TYPE;
  frame[1] = RK3576_HDMI_AUDI_VERSION;
  frame[2] = RK3576_HDMI_AUDI_LENGTH;
  frame[4] = (uint8_t)(channels - 1);

  for (i = 0; i < sizeof(frame); i++)
    {
      sum += frame[i];
    }

  frame[3] = (uint8_t)(0x100u - (sum & 0xff));

  rk3576_hdmi_write_infoframe(RK3576_HDMI_PKT_AUDI_CONTENTS0, frame,
                              sizeof(frame));

  rk3576_hdmi_modreg(RK3576_HDMI_PKTSCHED_PKT_SEND_AUTO, 0,
                     RK3576_HDMI_PKTSCHED_AUDI_TX_EN);
  rk3576_hdmi_modreg(RK3576_HDMI_PKTSCHED_PKT_EN, 0,
                     RK3576_HDMI_PKTSCHED_AUDI_TX_EN |
                         RK3576_HDMI_PKTSCHED_ACR_TX_EN |
                         RK3576_HDMI_PKTSCHED_AUDS_TX_EN);

  nxmutex_unlock(&priv->lock);

  ginfo("HDMI: audio %" PRIu32 " Hz, %u ch, %u bit, N %" PRIu32 ", CTS %llu\n",
        samplerate, channels, bits, n, (unsigned long long)cts);
  return OK;
}

#endif /* CONFIG_RK3576_HDMI */
