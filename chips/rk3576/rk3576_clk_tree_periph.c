/****************************************************************************
 * chips/rk3576/rk3576_clk_tree_periph.c
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
 * RK3576 Clock Tree — peripheral extension.
 *
 * rk3576_clk_tree.c registers the roots of the clock tree (xin_osc0, the
 * GPLL / CPLL / AUPLL fractional PLLs and their fixed-factor dividers) plus
 * the I2C, PWM and audio branches.  This file adds the branches of every
 * other peripheral block that has a driver in chips/rk3576/, so that the
 * two files can be maintained independently and later merged without
 * touching a single line of the other.
 *
 * The registration style, the hiword-mask flags and the error handling are
 * deliberately identical to rk3576_clk_tree.c:  Rockchip CRU registers put
 * the write-enable mask in bits [31:16] and the value in bits [15:0], and
 * gates are active-low ("write 1 to disable"), which the NuttX CLK
 * framework expresses with CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE.
 *
 * IMPORTANT — accuracy of the register data
 *
 *   Only a minority of the register/bit assignments below were read out of
 *   the RK3576 TRM.  The rest were derived from the Rockchip clock-ID
 *   ordering of the vendor device tree (4-HardwareData/k7_debian_vendor.dts)
 *   and are marked with a "TODO: verify" comment at the point of use.  A
 *   wrong gate bit does not fail loudly:  the block simply stays unclocked
 *   and every register read comes back as zero.  Do not treat an unverified
 *   node as a hardware fact.
 *
 *   The nodes whose register data comes from the TRM (and therefore carry
 *   no TODO) are: WDT, SPI0..4, mailbox, OTP, RNG and the whole Mali GPU
 *   sub-tree.
 *
 * Reference: Rockchip RK3576 TRM, Chapter 5 "Clock and Reset Unit".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <debug.h>
#include <nuttx/config.h>
#include <stdint.h>
#include <sys/param.h>

#include <nuttx/clk/clk.h>
#include <nuttx/clk/clk_provider.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_clk_tree_periph.h"

#ifdef CONFIG_CLK

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Root oscillator frequency — 24 MHz, the reference for all PLLs.  Kept in
 * sync with rk3576_clk_tree.c; the definition is repeated (instead of being
 * exported) so that merging the two files leaves no dangling reference.
 */

#ifndef CONFIG_RK3576_OSC_FREQ
#define CONFIG_RK3576_OSC_FREQ 24000000UL
#endif

/* SECURECRU gate register array.  hardware/rk3576_cru.h only describes the
 * main CRU and the PMU1CRU today; the OTP controller and the TRNG live in
 * the non-secure half of the SECURECRU at 0x27210000.
 *
 * TODO: move this macro into hardware/rk3576_cru.h next to
 * RK3576_CRU_GATE_CON() / RK3576_PMU1CRU_GATE_CON().
 */

#define RK3576_SECURECRU_GATE_CON(n) (0x0800 + ((n) * 4))

/* Off-SoC camera master clock (XVCLK).  Board oscillator described by the
 * "external-camera-37m-clock" fixed-clock node of the vendor device tree.
 */

#define RK3576_EXT_CAM_37M_RATE 37125000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Private data of a fractional PLL, identical in layout and semantics to
 * struct rk3576_fracpll_s in rk3576_clk_tree.c.  That type has file scope
 * there, so the peripheral PLLs registered here need their own copy.
 *
 * TODO: when this file is merged into rk3576_clk_tree.c, delete this type
 * and rk3576_periph_fracpll_recalc_rate() and reuse the originals.
 *
 * Register layout for FRACPLL:
 *   CON0[9:0]  = m (FBDIV, 10-bit main divider, 64 <= m <= 1023)
 *   CON1[5:0]  = p (REFDIV, 6-bit pre-divider, 1 <= p <= 63)
 *   CON1[8:6]  = s (POSTDIV2 exponent, 3-bit scaler, 0 <= s <= 6)
 *   CON2[15:0] = k (FRAC, 16-bit two's complement DSM value)
 */

struct rk3576_periph_fracpll_s
{
  uintptr_t con_base; /* CON0 register address (CON1/2 are +4/+8) */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t
rk3576_periph_fracpll_recalc_rate(struct clk_s *clk, uint32_t parent_rate);

static void rk3576_clk_register_periph_plls(void);

#ifdef CONFIG_RK3576_WDT
static void rk3576_clk_register_wdt(void);
#endif
#ifdef CONFIG_RK3576_SPI
static void rk3576_clk_register_spi(void);
#endif
#ifdef CONFIG_RK3576_MAILBOX
static void rk3576_clk_register_mailbox(void);
#endif
#ifdef CONFIG_RK3576_I3C
static void rk3576_clk_register_i3c(void);
#endif
#ifdef CONFIG_RK3576_SARADC
static void rk3576_clk_register_saradc(void);
#endif
#ifdef CONFIG_RK3576_TSADC
static void rk3576_clk_register_tsadc(void);
#endif
#ifdef CONFIG_RK3576_CRYPTO
static void rk3576_clk_register_crypto(void);
#endif
#ifdef CONFIG_RK3576_OTP
static void rk3576_clk_register_otp(void);
#endif
#ifdef CONFIG_RK3576_RNG
static void rk3576_clk_register_rng(void);
#endif
#ifdef CONFIG_RK3576_VOP
static void rk3576_clk_register_vop(void);
#endif
#ifdef CONFIG_RK3576_HDMI
static void rk3576_clk_register_hdmi(void);
#endif
#ifdef CONFIG_RK3576_RGA
static void rk3576_clk_register_rga(void);
#endif
#ifdef CONFIG_RK3576_VDEC
static void rk3576_clk_register_vdec(void);
#endif
#ifdef CONFIG_RK3576_CSI
static void rk3576_clk_register_csi(void);
#endif
#if defined(CONFIG_RK3576_VICAP) || defined(CONFIG_RK3576_ISP)
static void rk3576_clk_register_vi(void);
#endif
#ifdef CONFIG_RK3576_RKNPU
static void rk3576_clk_register_rknpu(void);
#endif
#ifdef CONFIG_RK3576_MALI
static void rk3576_clk_register_mali(void);
#endif
#ifdef CONFIG_RK3576_GMAC
static void rk3576_clk_register_gmac(void);
#endif
#if defined(CONFIG_RK3576_PCIE) || defined(CONFIG_RK3576_COMBPHY)
static void rk3576_clk_register_pcie(void);
#endif
#ifdef CONFIG_RK3576_PDM
static void rk3576_clk_register_pdm(void);
#endif
#if defined(CONFIG_RK3576_IR) && !defined(CONFIG_RK3576_PWM)
static void rk3576_clk_register_ir(void);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct clk_ops_s g_rk3576_periph_fracpll_ops = {
  .recalc_rate = rk3576_periph_fracpll_recalc_rate,
};

/* Every PLL registered here hangs off the 24 MHz crystal. */

static const char *g_periph_pll_parents[] = {
  "xin_osc0",
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_periph_fracpll_recalc_rate
 *
 * Description:
 *   Recalculate FRACPLL output frequency from CON0..CON2 registers.
 *   FOUT = ((m + k/65536) * FIN) / (p * 2^s)
 *   Where m=CON0[9:0], p=CON1[5:0], s=CON1[8:6], k=CON2[15:0].
 *
 *   parent_rate = FIN (xin_osc0 = 24 MHz).
 *
 *   k is a 16-bit two's complement integer, so we treat it as int16_t.
 *   To avoid floating-point, compute:
 *     FOUT = ((m * 65536 + k) * FIN) / (p * 65536 * (1 << s))
 *   using 64-bit arithmetic to prevent overflow.
 *
 *   This is a verbatim copy of rk3576_fracpll_recalc_rate() in
 *   rk3576_clk_tree.c, which has file scope there.
 *
 ****************************************************************************/

static uint32_t
rk3576_periph_fracpll_recalc_rate(struct clk_s *clk, uint32_t parent_rate)
{
  struct rk3576_periph_fracpll_s *pll = clk->private_data;
  uint32_t con0;
  uint32_t con1;
  uint32_t con2;
  uint32_t m;
  uint32_t p;
  uint32_t s;
  int16_t k;
  uint64_t numerator;
  uint64_t denominator;

  DEBUGASSERT(pll);
  DEBUGASSERT(parent_rate == CONFIG_RK3576_OSC_FREQ);

  con0 = getreg32(pll->con_base);     /* CON0 */
  con1 = getreg32(pll->con_base + 4); /* CON1 */
  con2 = getreg32(pll->con_base + 8); /* CON2 */

  m = con0 & 0x3ff;             /* CON0[9:0]   */
  p = con1 & 0x3f;              /* CON1[5:0]   */
  s = (con1 >> 6) & 0x7;        /* CON1[8:6]   */
  k = (int16_t)(con2 & 0xffff); /* CON2[15:0], two's complement */

  /* Guard against invalid register values. */

  if (p == 0 || m < 64 || m > 1023 || s > 6)
    {
      return 0;
    }

  numerator = (uint64_t)parent_rate * ((uint64_t)m * 65536 + (int64_t)k);
  denominator = (uint64_t)p * 65536 * (1ULL << s);

  return (uint32_t)(numerator / denominator);
}

/****************************************************************************
 * Name: rk3576_clk_register_periph_plls
 *
 * Description:
 *   Register the PLLs and the fixed-factor dividers that the peripheral
 *   branches need but rk3576_clk_tree.c does not provide.
 *
 *   VPLL (video PLL) is the pixel-clock source of the VOP display pipeline
 *   and of the HDMI TX PHY.  It is a FRACPLL like GPLL and CPLL, so the
 *   same recalc_rate implementation applies; its CON block lives at
 *   RK3576_CRU_VPLL_CON(0) (offset 0x0160).
 *
 *   The /5 dividers are used by the HDMI reference clock mux and by the RGA
 *   core mux; rk3576_clk_tree.c only registers /2 /3 /4 /6 /8 off GPLL and
 *   /2 /4 /10 /20 off CPLL.
 *
 *   NOTE: RK3576 has no "NPLL".  The PLL list of the CRU is BPLL, LPLL,
 *   VPLL, AUPLL, CPLL, GPLL (main CRU) plus PPLL (PMU domain) — see the
 *   PLL_CON macros in hardware/rk3576_cru.h.  The NPU therefore does not
 *   own a dedicated PLL: its clk_rknn_dsu0 mux selects between GPLL, CPLL,
 *   AUPLL and SPLL, and the boot loader leaves it on GPLL.
 *
 *   TODO: SPLL and LPLL are referenced by the NPU and GPU source muxes but
 *   are not modelled yet (SPLL is a fixed ~702 MHz PLL outside the main CRU
 *   CON block, LPLL is the little-core PLL in LITCORE_CRU).  Their mux
 *   slots fall back to a registered PLL so that clk_get_rate() never
 *   dereferences a missing parent; register them properly before enabling
 *   NPU or GPU DVFS.
 *
 ****************************************************************************/

static void rk3576_clk_register_periph_plls(void)
{
  static struct rk3576_periph_fracpll_s vpll_priv;
  struct clk_s *vpll;

  /* VPLL (FRACPLL) — rate derived from VPLL_CON(0..2) at runtime. */

  vpll_priv.con_base = RK3576_CRU_ADDR + RK3576_CRU_VPLL_CON(0);

  vpll = clk_register("clk_vpll", g_periph_pll_parents,
                      nitems(g_periph_pll_parents),
                      CLK_NAME_IS_STATIC | CLK_PARENT_NAME_IS_STATIC,
                      &g_rk3576_periph_fracpll_ops, &vpll_priv,
                      sizeof(vpll_priv));
  if (!vpll)
    {
      _err("CLK: failed to register clk_vpll\n");
      return;
    }

  clk_register_fixed_factor("clk_vpll_div2", "clk_vpll", CLK_NAME_IS_STATIC,
                            1, 2);
  clk_register_fixed_factor("clk_vpll_div4", "clk_vpll", CLK_NAME_IS_STATIC,
                            1, 4);

  /* Extra GPLL/CPLL post-dividers used by the media branches. */

  clk_register_fixed_factor("clk_gpll_div5", "clk_gpll", CLK_NAME_IS_STATIC,
                            1, 5);
  clk_register_fixed_factor("clk_cpll_div5", "clk_cpll", CLK_NAME_IS_STATIC,
                            1, 5);
}

/****************************************************************************
 * Name: rk3576_clk_register_wdt
 *
 * Description:
 *   Register the clocks of the non-secure watchdog (watchdog@2ace0000).
 *
 *   TRM CRU_GATE_CON16 (0x27200000 + 0x0840):
 *     bit 7  pclk_wdtpmu_en  -> DTS clock id 0xa7, "pclk"
 *     bit 8  tclk_wdtpmu_en  -> DTS clock id 0xa8, "tclk"
 *
 *   The TRM signal name is "wdtpmu"; the block it clocks is WDT_NS, which
 *   is how the driver and the node names below refer to it.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_WDT
static void rk3576_clk_register_wdt(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  clk_register_gate("pclk_wdt_ns_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(16), 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* No CLKSEL mux exists for tclk_wdtpmu, so the counter clock is wired
   * straight to the 24 MHz crystal.
   *
   * TODO: confirm the parent on silicon — read back the period the block
   * actually produces and compare it with clk_get_rate("tclk_wdt_ns_en").
   */

  clk_register_gate("tclk_wdt_ns_en", "xin_osc0", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(16), 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_WDT */

/**
 * Macro: RK3576_CLK_REGISTER_SPI_ONE
 *
 * Register one SPI controller clock tree (mux + pclk gate + fclk gate).
 * Uses #ctrl stringification so all clock names are compile-time constants
 * — no snprintf required.
 *
 * Parameters:
 *   ctrl      - controller index (0..4), used as name suffix
 *   sel_reg   - CLKSEL register address
 *   sel_shift - MUX select field bit offset
 *   pclk_reg  - pclk GATE register address
 *   pclk_bit  - pclk GATE bit
 *   clk_reg   - functional clock GATE register address
 *   clk_bit   - functional clock GATE bit
 */

#define RK3576_CLK_REGISTER_SPI_ONE(ctrl, sel_reg, sel_shift, pclk_reg,    \
                                    pclk_bit, clk_reg, clk_bit)            \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      _mux = clk_register_mux("clk_spi" #ctrl "_sel", g_spi_sel_parents,   \
                              nitems(g_spi_sel_parents),                   \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_spi" #ctrl "_sel\n");          \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("pclk_spi" #ctrl "_en", NULL, CLK_NAME_IS_STATIC,  \
                        pclk_reg, pclk_bit,                                \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      clk_register_gate("clk_spi" #ctrl "_en", "clk_spi" #ctrl "_sel",     \
                        CLK_NAME_IS_STATIC, clk_reg, clk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_spi
 *
 * Description:
 *   Register the clock trees of SPI0..SPI4 (spi4 = spi@2ad30000).
 *
 *   TRM CRU_CLKSEL_CON70 (0x0418): bits 14:13 clk_spi0_sel
 *   TRM CRU_CLKSEL_CON71 (0x041c): bits 1:0 / 3:2 / 5:4 / 7:6
 *                                  clk_spi1_sel .. clk_spi4_sel
 *   TRM CRU_GATE_CON15   (0x083c): bits 13/14/15 pclk_spi0/1/2_en
 *   TRM CRU_GATE_CON16   (0x0840): bits 0/1      pclk_spi3/4_en
 *                                  bits 2..6     clk_spi0..4_en
 *
 *   The DTS node spi@2ad30000 uses ids 0xa1 ("apb_pclk") and 0xa6
 *   ("spiclk"), which line up with GATE_CON16 bit 1 and bit 6.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_SPI

/* Order matches the hardware 2-bit select encoding (TRM CLKSEL_CON70/71):
 * 00 = clk_gpll_div6_src, 01 = clk_gpll_div8_src,
 * 10 = clk_cpll_div10_src, 11 = xin_osc0_func
 */

static const char *g_spi_sel_parents[] = {
  "clk_gpll_div6",  /* 0b00 */
  "clk_gpll_div8",  /* 0b01 */
  "clk_cpll_div10", /* 0b10 */
  "xin_osc0",       /* 0b11 */
};

static void rk3576_clk_register_spi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  RK3576_CLK_REGISTER_SPI_ONE(0, cru + RK3576_CRU_CLKSEL_CON(70), 13,
                              cru + RK3576_CRU_GATE_CON(15), 13,
                              cru + RK3576_CRU_GATE_CON(16), 2);

  RK3576_CLK_REGISTER_SPI_ONE(1, cru + RK3576_CRU_CLKSEL_CON(71), 0,
                              cru + RK3576_CRU_GATE_CON(15), 14,
                              cru + RK3576_CRU_GATE_CON(16), 3);

  RK3576_CLK_REGISTER_SPI_ONE(2, cru + RK3576_CRU_CLKSEL_CON(71), 2,
                              cru + RK3576_CRU_GATE_CON(15), 15,
                              cru + RK3576_CRU_GATE_CON(16), 4);

  RK3576_CLK_REGISTER_SPI_ONE(3, cru + RK3576_CRU_CLKSEL_CON(71), 4,
                              cru + RK3576_CRU_GATE_CON(16), 0,
                              cru + RK3576_CRU_GATE_CON(16), 5);

  RK3576_CLK_REGISTER_SPI_ONE(4, cru + RK3576_CRU_CLKSEL_CON(71), 6,
                              cru + RK3576_CRU_GATE_CON(16), 1,
                              cru + RK3576_CRU_GATE_CON(16), 6);
}
#endif /* CONFIG_RK3576_SPI */

#undef RK3576_CLK_REGISTER_SPI_ONE

/****************************************************************************
 * Name: rk3576_clk_register_mailbox
 *
 * Description:
 *   Register the APB gate of the inter-core mailbox block
 *   (mailbox@2ae5x000).
 *
 *   TRM CRU_GATE_CON17 (0x0844) bit 13 = pclk_mailbox0_en.  One gate serves
 *   every mailbox instance — the vendor device tree gives all eight AP
 *   mailbox nodes the same clock (id 0xb6).
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_MAILBOX
static void rk3576_clk_register_mailbox(void)
{
  clk_register_gate("pclk_mailbox0_en", NULL, CLK_NAME_IS_STATIC,
                    RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(17), 13,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_MAILBOX */

/**
 * Macro: RK3576_CLK_REGISTER_I3C_ONE
 *
 * Register one I3C controller clock tree (mux + hclk gate + core gate).
 *
 * Parameters:
 *   ctrl      - controller index (0 or 1), used as name suffix
 *   sel_reg   - CLKSEL register address
 *   sel_shift - MUX select field bit offset
 *   hclk_reg  - hclk GATE register address
 *   hclk_bit  - hclk GATE bit
 *   clk_reg   - core clock GATE register address
 *   clk_bit   - core clock GATE bit
 */

#define RK3576_CLK_REGISTER_I3C_ONE(ctrl, sel_reg, sel_shift, hclk_reg,    \
                                    hclk_bit, clk_reg, clk_bit)            \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      _mux = clk_register_mux("clk_i3c" #ctrl "_sel", g_i3c_sel_parents,   \
                              nitems(g_i3c_sel_parents),                   \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_i3c" #ctrl "_sel\n");          \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("hclk_i3c" #ctrl "_en", NULL, CLK_NAME_IS_STATIC,  \
                        hclk_reg, hclk_bit,                                \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      clk_register_gate("clk_i3c" #ctrl "_en", "clk_i3c" #ctrl "_sel",     \
                        CLK_NAME_IS_STATIC, clk_reg, clk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_i3c
 *
 * Description:
 *   Register the clock trees of the two DesignWare MIPI I3C masters
 *   (i3c-master@2abe0000 and its sibling).
 *
 *   Vendor DTS clock ids: HCLK_I3C0 = 0xcd, CLK_I3C0 = 0xdb,
 *   HCLK_I3C1 = 0xce, CLK_I3C1 = 0xdc.
 *
 *   TODO: verify the CLKSEL/GATE register indices and bit positions against
 *   the RK3576 TRM CRU chapter.  The clock ids come from the DTS, but the
 *   register placement is inferred from the neighbouring I2C/UART entries
 *   (HCLK_I3Cn with the other BUS-domain AHB gates, CLK_I3Cn with the
 *   BUS-domain functional gates).
 *
 *   TODO: the I3C1 mux field (CLKSEL_CON(58) bits 5:4) collides with the
 *   inferred SARADC divider field (CLKSEL_CON(58) bits 11:4).  At most one
 *   of the two can be right; resolve against the TRM before bring-up.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_I3C
static const char *g_i3c_sel_parents[] = {
  "clk_gpll_div6",  /* 0b00 */
  "clk_cpll_div10", /* 0b01 */
  "clk_cpll_div20", /* 0b10 */
  "xin_osc0",       /* 0b11 */
};

static void rk3576_clk_register_i3c(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* I3C0 — HCLK_I3C0 = 0xcd, CLK_I3C0 = 0xdb.  TODO: verify. */

  RK3576_CLK_REGISTER_I3C_ONE(0, cru + RK3576_CRU_CLKSEL_CON(58), 2,
                              cru + RK3576_CRU_GATE_CON(13), 9,
                              cru + RK3576_CRU_GATE_CON(13), 11);

  /* I3C1 — HCLK_I3C1 = 0xce, CLK_I3C1 = 0xdc.  TODO: verify. */

  RK3576_CLK_REGISTER_I3C_ONE(1, cru + RK3576_CRU_CLKSEL_CON(58), 4,
                              cru + RK3576_CRU_GATE_CON(13), 10,
                              cru + RK3576_CRU_GATE_CON(13), 12);
}
#endif /* CONFIG_RK3576_I3C */

#undef RK3576_CLK_REGISTER_I3C_ONE

/****************************************************************************
 * Name: rk3576_clk_register_saradc
 *
 * Description:
 *   Register the SAR-ADC clock tree (mux + divider + two gates).
 *
 *   TODO: verify GATE_CON(13) bits 6/7 and CLKSEL_CON(58) shift 12 (1-bit
 *   mux, 0 = GPLL, 1 = xin_osc0) / shift 4 width 8 (divider) against the
 *   RK3576 TRM CRU chapter; they are derived from the Rockchip clock-ID
 *   table (CLK_SARADC = 0x84, PCLK_SARADC = 0x83 in the vendor DTS).
 *
 *   TODO: see the collision note in rk3576_clk_register_i3c() — the
 *   inferred divider field overlaps the inferred I3C1 mux field.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_SARADC
static const char *g_saradc_sel_parents[] = {
  "clk_gpll",  /* 0b0 */
  "xin_osc0",  /* 0b1 */
};

static void rk3576_clk_register_saradc(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  struct clk_s *mux;

  mux = clk_register_mux("clk_saradc_sel", g_saradc_sel_parents,
                         nitems(g_saradc_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(58), 12, 1,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_saradc_sel\n");
      return;
    }

  clk_register_divider("clk_saradc_div", "clk_saradc_sel",
                       CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                       cru + RK3576_CRU_CLKSEL_CON(58), 4, 8,
                       CLK_DIVIDER_HIWORD_MASK);

  clk_register_gate("pclk_saradc_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(13), 6,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_saradc_en", "clk_saradc_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(13), 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_SARADC */

/****************************************************************************
 * Name: rk3576_clk_register_tsadc
 *
 * Description:
 *   Register the on-die temperature sensor clocks: one divider off the
 *   24 MHz oscillator plus the APB and conversion gates.  The vendor DTS
 *   pins clk_tsadc to 2 MHz (assigned-clock-rates = <0x1e8480>, i.e.
 *   24 MHz / 12) and names the clocks "tsadc" (id 0x86) and "apb_pclk"
 *   (id 0x85).
 *
 *   TODO: the CLKSEL/GATE register indices and bit positions are inferred
 *   from the RK3576 clock-ID ordering and have NOT been verified against
 *   the TRM CRU chapter.  Confirm that CLKSEL_CON(59)[7:0] is the tsadc
 *   divider and that GATE_CON(20) bits 11/12 are the two gates.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_TSADC
static void rk3576_clk_register_tsadc(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* 8-bit divider off xin_osc0; resets to /12 -> 2 MHz.  TODO: verify. */

  clk_register_divider("clk_tsadc_div", "xin_osc0",
                       CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                       cru + RK3576_CRU_CLKSEL_CON(59), 0, 8,
                       CLK_DIVIDER_HIWORD_MASK);

  clk_register_gate("pclk_tsadc_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(20), 11,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_tsadc_en", "clk_tsadc_div", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(20), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_TSADC */

/****************************************************************************
 * Name: rk3576_clk_register_crypto
 *
 * Description:
 *   Register the clocks of the hardware crypto engine (crypto@2a400000).
 *   Vendor DTS clocks: "aclk" = id 0x227 (AXI data path, 300 MHz),
 *   "hclk" = id 0x222 (AHB register interface), "pka" = id 0x228 (public
 *   key accelerator core, 300 MHz).
 *
 *   TODO: the GATE_CON index and bit positions are inferred from the
 *   Rockchip clock-ID ordering, not read from the TRM.  Verify on hardware
 *   by reading GATE_CON(26) before and after Linux probes the crypto node.
 *
 *   TODO: the parents "aclk_bus_root" / "hclk_bus_root" are the correct
 *   hardware sources but are not modelled by the clock tree yet, so
 *   clk_get_rate() on these gates reports 0.  The crypto driver only polls
 *   status bits, so it never asks for the rate.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_CRYPTO
static void rk3576_clk_register_crypto(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  clk_register_gate("aclk_crypto_ns_en", "aclk_bus_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(26), 6,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("hclk_crypto_ns_en", "hclk_bus_root", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(26), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_pka_crypto_ns_en", "aclk_crypto_ns_en",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(26), 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_CRYPTO */

/****************************************************************************
 * Name: rk3576_clk_register_otp
 *
 * Description:
 *   Register the OTP controller clocks (otp@2a580000).  The block lives in
 *   the non-secure half of the SECURECRU, not in the main CRU.
 *
 *   TRM SECURECRU_GATE_CON00 (0x27210000 + 0x0800):
 *     bit 8  pclk_otpc_ns_en -> DTS otp id 0x221, "apb"
 *     bit 9  clk_otpc_ns_en  -> DTS otp id 0x224, "otpc"
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_OTP
static void rk3576_clk_register_otp(void)
{
  const unsigned long secure = RK3576_SECURE_CRU_ADDR;

  clk_register_gate("pclk_otpc_ns_en", NULL, CLK_NAME_IS_STATIC,
                    secure + RK3576_SECURECRU_GATE_CON(0), 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* TODO: parent unknown.  Neither the CRU nor the SECURECRU CLKSEL tables
   * carry a clk_otpc_ns mux or divider, so the fuse-array user clock is fed
   * from a fixed source (xin_osc0 is the likely one).  The OTP driver only
   * uses absolute up_udelay() timings, so it never asks for the rate;
   * attach the real parent once it is confirmed.
   */

  clk_register_gate("clk_otpc_ns_en", NULL, CLK_NAME_IS_STATIC,
                    secure + RK3576_SECURECRU_GATE_CON(0), 9,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_OTP */

/****************************************************************************
 * Name: rk3576_clk_register_rng
 *
 * Description:
 *   Register the AHB gate of the true random number generator
 *   (rng@2a410000).
 *
 *   TRM SECURECRU_GATE_CON00 bit 4 = hclk_trng_ns_en -> DTS rng id 0x223,
 *   "hclk_trng".
 *
 *   The hardware parent is hclk_secure_ns_root, which the tree does not
 *   model yet; the RNG driver never asks for the rate.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_RNG
static void rk3576_clk_register_rng(void)
{
  clk_register_gate("hclk_trng_ns_en", NULL, CLK_NAME_IS_STATIC,
                    RK3576_SECURE_CRU_ADDR + RK3576_SECURECRU_GATE_CON(0), 4,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_RNG */

/****************************************************************************
 * Name: rk3576_clk_register_vop
 *
 * Description:
 *   Register the VOP2 display controller clocks (vop@27d00000): the AHB and
 *   AXI bus gates plus the pixel-clock chain of video port 0
 *   (mux -> divider -> gate).
 *
 *   Vendor DTS clock ids: hclk_vop = 0x189, aclk_vop = 0x18a,
 *   dclk_src_vp0..2 = 0x18b..0x18d, dclk_vp0..2 = 0x18e / 0x190 / 0x191.
 *
 *   VP1 and VP2 follow the same pattern and are added when the second
 *   display is brought up.
 *
 *   TODO: every GATE_CON / CLKSEL_CON index and bit below is inferred from
 *   the VOP2 clock ordering and must be re-checked against the RK3576 TRM
 *   CRU chapter before first bring-up.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_VOP
static const char *g_vop_dclk_sel_parents[] = {
  "clk_gpll_div2", /* 0b00 */
  "clk_cpll_div2", /* 0b01 */
  "clk_vpll",      /* 0b10 — video PLL, HDMI pixel rates */
  "xin_osc0",      /* 0b11 */
};

static void rk3576_clk_register_vop(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  struct clk_s *mux;

  /* Bus clocks of the VOP block.  TODO: verify. */

  clk_register_gate("hclk_vop_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(45), 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("aclk_vop_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(45), 9,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* dclk_vp0: source mux -> divider -> gate.  TODO: verify. */

  mux = clk_register_mux("clk_dclk_vp0_sel", g_vop_dclk_sel_parents,
                         nitems(g_vop_dclk_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(147), 0, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_dclk_vp0_sel\n");
      return;
    }

  clk_register_divider("clk_dclk_vp0_div", "clk_dclk_vp0_sel",
                       CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                       cru + RK3576_CRU_CLKSEL_CON(147), 2, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  clk_register_gate("dclk_vp0_en", "clk_dclk_vp0_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(45), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_VOP */

/****************************************************************************
 * Name: rk3576_clk_register_hdmi
 *
 * Description:
 *   Register the HDMI TX0 controller and HDPTX PHY clocks.  The node names
 *   follow the vendor DTS clock-names of hdmi@27da0000 ("pclk", "hclk_vo1",
 *   "hdmitx_ref", "hpd", "earc", "aud") and hdmiphy@2b000000 ("apb",
 *   "ref").
 *
 *   TODO: the GATE_CON word index and bit position of every gate below are
 *   inferred from the RK3576 clock-ID ordering in the vendor DTS
 *   (pclk_hdmitx0 = 413, earc = 414, hdmitx_ref = 415, aud = 423,
 *   hpd = 478, hclk_vo1 = 404, hdptx apb/ref = 500/523).  They must be
 *   cross-checked against the TRM CRU GATE_CON tables.
 *
 *   TODO: the inferred hclk_vo1_en bit (GATE_CON(46) bit 1) collides with
 *   the inferred aclk_rga2_1_en bit; at most one of the two is right.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_HDMI
static const char *g_hdmitx_ref_parents[] = {
  "clk_gpll_div5", /* 0b00 */
  "clk_cpll_div5", /* 0b01 */
  "xin_osc0",      /* 0b10 */
};

static void rk3576_clk_register_hdmi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  struct clk_s *mux;

  /* Reference clock source select of the HDMI TX core.  TODO: verify. */

  mux = clk_register_mux("clk_hdmitx0_ref_sel", g_hdmitx_ref_parents,
                         nitems(g_hdmitx_ref_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(148), 0, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_hdmitx0_ref_sel\n");
      return;
    }

  /* VO1 bus clock — parent bus of the HDMI TX controller.  TODO: verify. */

  clk_register_gate("hclk_vo1_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(46), 1,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* HDMI TX0 controller gates.  TODO: verify. */

  clk_register_gate("pclk_hdmitx0_en", "hclk_vo1_en", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(47), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_hdmitx0_earc_en", "hclk_vo1_en", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(47), 6,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_hdmitx0_ref_en", "clk_hdmitx0_ref_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(47), 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_hdmitx0_aud_en", "hclk_vo1_en", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(48), 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* Hot-plug detect sampling clock: fixed 24 MHz / 2 in the vendor tree. */

  clk_register_fixed_factor("clk_hdmitx0_hpd_div", "xin_osc0",
                            CLK_NAME_IS_STATIC, 1, 2);

  clk_register_gate("clk_hdmitx0_hpd_en", "clk_hdmitx0_hpd_div",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(54), 14,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* HDPTX PHY.  TODO: verify. */

  clk_register_gate("pclk_hdptx0_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(57), 6,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_hdptx0_ref_en", "xin_osc0", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(59), 11,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_HDMI */

/**
 * Macro: RK3576_CLK_REGISTER_RGA2_ONE
 *
 * Register one RGA2 core clock group (aclk gate + hclk gate + core mux and
 * gate).
 *
 * Parameters:
 *   core      - RGA2 core index (0 or 1), used as name suffix
 *   aclk_reg  - aclk GATE register address
 *   aclk_bit  - aclk GATE bit
 *   hclk_reg  - hclk GATE register address
 *   hclk_bit  - hclk GATE bit
 *   sel_reg   - CLKSEL register address of the core clock mux
 *   sel_shift - MUX select field bit offset
 *   clk_reg   - core clock GATE register address
 *   clk_bit   - core clock GATE bit
 */

#define RK3576_CLK_REGISTER_RGA2_ONE(core, aclk_reg, aclk_bit, hclk_reg,   \
                                     hclk_bit, sel_reg, sel_shift,         \
                                     clk_reg, clk_bit)                     \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      clk_register_gate("aclk_rga2_" #core "_en", NULL,                    \
                        CLK_NAME_IS_STATIC, aclk_reg, aclk_bit,            \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      clk_register_gate("hclk_rga2_" #core "_en", NULL,                    \
                        CLK_NAME_IS_STATIC, hclk_reg, hclk_bit,            \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      _mux = clk_register_mux("clk_rga2_" #core "_sel",                    \
                              g_rga2_sel_parents,                          \
                              nitems(g_rga2_sel_parents),                  \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_rga2_" #core "_sel\n");        \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("clk_rga2_" #core "_en", "clk_rga2_" #core "_sel", \
                        CLK_NAME_IS_STATIC, clk_reg, clk_bit,              \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_rga
 *
 * Description:
 *   Register the clocks of the two RGA2 2D acceleration cores
 *   (rga@27920000 and rga@27930000).  Vendor DTS clock ids: core0
 *   aclk 0x157 / hclk 0x156 / clk 0x158, core1 aclk 0x15f / hclk 0x15e /
 *   clk 0x160.
 *
 *   TODO: the GATE_CON indices and bit positions are inferred from the
 *   RK3576 clock-ID ordering (the VPU/RGA block sits in GATE_CON(45) and
 *   GATE_CON(46)); re-check every bit against the TRM.  Note in particular
 *   the collision of aclk_rga2_1_en with the inferred hclk_vo1_en bit.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_RGA

/* Core clock source candidates.  TODO: confirm the mux parent order. */

static const char *g_rga2_sel_parents[] = {
  "clk_gpll_div3", /* 0b00 */
  "clk_cpll_div4", /* 0b01 */
  "clk_gpll_div5", /* 0b10 */
  "xin_osc0",      /* 0b11 */
};

static void rk3576_clk_register_rga(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* RGA2 core0.  TODO: verify. */

  RK3576_CLK_REGISTER_RGA2_ONE(0, cru + RK3576_CRU_GATE_CON(45), 6,
                               cru + RK3576_CRU_GATE_CON(45), 5,
                               cru + RK3576_CRU_CLKSEL_CON(72), 0,
                               cru + RK3576_CRU_GATE_CON(45), 7);

  /* RGA2 core1.  TODO: verify. */

  RK3576_CLK_REGISTER_RGA2_ONE(1, cru + RK3576_CRU_GATE_CON(46), 1,
                               cru + RK3576_CRU_GATE_CON(46), 0,
                               cru + RK3576_CRU_CLKSEL_CON(72), 2,
                               cru + RK3576_CRU_GATE_CON(46), 2);
}
#endif /* CONFIG_RK3576_RGA */

#undef RK3576_CLK_REGISTER_RGA2_ONE

/****************************************************************************
 * Name: rk3576_clk_register_vdec
 *
 * Description:
 *   Register the RKVDEC video decoder clocks (rkvdec@27b00000).  The vendor
 *   DTS uses aclk_vcodec / hclk_vcodec / clk_core / clk_cabac /
 *   clk_hevc_cabac (clock ids 324, 325, 327, 553, 326).
 *
 *   TODO: the CLKSEL/GATE indices and bit positions are inferred from the
 *   clock-ID ordering of the VIDEO domain and must be re-checked against
 *   the TRM CRU chapter on silicon.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_VDEC

/* TODO: verify the 2-bit mux encoding order. */

static const char *g_vdec_sel_parents[] = {
  "clk_gpll_div2", /* 0b00 */
  "clk_cpll_div2", /* 0b01 */
  "clk_gpll_div4", /* 0b10 */
  "xin_osc0",      /* 0b11 */
};

static void rk3576_clk_register_vdec(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  struct clk_s *mux;

  /* Decoder core clock mux (clk_core, 600 MHz in the vendor DTS). */

  mux = clk_register_mux("clk_vdec_core_sel", g_vdec_sel_parents,
                         nitems(g_vdec_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(65), 0, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_vdec_core_sel\n");
      return;
    }

  /* CABAC clock mux (clk_cabac, 500 MHz). */

  mux = clk_register_mux("clk_vdec_cabac_sel", g_vdec_sel_parents,
                         nitems(g_vdec_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(65), 2, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_vdec_cabac_sel\n");
      return;
    }

  /* HEVC CABAC clock mux (clk_hevc_cabac, 1000 MHz). */

  mux = clk_register_mux("clk_vdec_hevc_cabac_sel", g_vdec_sel_parents,
                         nitems(g_vdec_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(65), 4, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_vdec_hevc_cabac_sel\n");
      return;
    }

  /* Bus gates.  TODO: verify. */

  clk_register_gate("aclk_vdec_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(35), 0,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("hclk_vdec_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(35), 1,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* Functional gates.  TODO: verify. */

  clk_register_gate("clk_vdec_core_en", "clk_vdec_core_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(35), 3,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_vdec_cabac_en", "clk_vdec_cabac_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(35), 4,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_vdec_hevc_cabac_en", "clk_vdec_hevc_cabac_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(35), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_VDEC */

/**
 * Macro: RK3576_CLK_REGISTER_CSI2HOST_ONE
 *
 * Register the APB gate of one MIPI CSI-2 host instance.
 *
 * Parameters:
 *   n        - CSI-2 host index (0..4), used as name suffix
 *   pclk_reg - pclk GATE register address
 *   pclk_bit - pclk GATE bit
 */

#define RK3576_CLK_REGISTER_CSI2HOST_ONE(n, pclk_reg, pclk_bit)          \
  do                                                                     \
    {                                                                    \
      clk_register_gate("pclk_csi2host" #n "_en", NULL,                  \
                        CLK_NAME_IS_STATIC, pclk_reg, pclk_bit,          \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE); \
    }                                                                    \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_csi
 *
 * Description:
 *   Register the MIPI CSI-2 host controller and RX D-PHY clocks.
 *
 *   Vendor DTS clock ids: pclk_csi2host0..4 = 0x178..0x17c,
 *   iclk_csi2host0 = 0x17e, pclk_csidphy0 = 0x1da, pclk_csidphy1 = 0x0dd.
 *
 *   The D-PHY APB clock also feeds the PHY digital core and is the
 *   HS-SETTLE counter reference, so clk_get_rate() has to return a real
 *   rate once the parent chain is modelled.
 *
 *   TODO: the GATE_CON register indices and bit positions are inferred from
 *   the VI-domain gate ordering and must be cross-checked against the TRM
 *   CRU_GATE_CON tables.  Names, flags and parents follow the convention.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_CSI
static void rk3576_clk_register_csi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* CSI-2 host APB gates — VI domain, GATE_CON(43).  TODO: verify. */

  RK3576_CLK_REGISTER_CSI2HOST_ONE(0, cru + RK3576_CRU_GATE_CON(43), 8);
  RK3576_CLK_REGISTER_CSI2HOST_ONE(1, cru + RK3576_CRU_GATE_CON(43), 9);
  RK3576_CLK_REGISTER_CSI2HOST_ONE(2, cru + RK3576_CRU_GATE_CON(43), 10);
  RK3576_CLK_REGISTER_CSI2HOST_ONE(3, cru + RK3576_CRU_GATE_CON(43), 11);
  RK3576_CLK_REGISTER_CSI2HOST_ONE(4, cru + RK3576_CRU_GATE_CON(43), 12);

  /* Interface clock — only instance 0 has one.  TODO: verify. */

  clk_register_gate("iclk_csi2host0_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(43), 14,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* RX D-PHY APB gates.  TODO: verify register and bit. */

  clk_register_gate("pclk_csidphy0_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(55), 2,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("pclk_csidphy1_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(29), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_CSI */

#undef RK3576_CLK_REGISTER_CSI2HOST_ONE

/**
 * Macro: RK3576_CLK_REGISTER_VI_GATE
 *
 * Register one VI-domain gate and report a failure by name.  The VI gates
 * are plain on/off nodes with no mux or divider of their own, so a helper
 * that only takes a name/parent/register/bit keeps the call sites short.
 *
 * Parameters:
 *   name   - clock node name
 *   parent - parent clock name, or NULL when the source is not modelled
 *   reg    - GATE register address
 *   bit    - GATE bit
 */

#define RK3576_CLK_REGISTER_VI_GATE(name, parent, reg, bit)      \
  do                                                             \
    {                                                            \
      if (clk_register_gate((name), (parent), CLK_NAME_IS_STATIC,\
                            (reg), (bit),                        \
                            CLK_GATE_HIWORD_MASK |               \
                            CLK_GATE_SET_TO_DISABLE) == NULL)    \
        {                                                        \
          _err("CLK: failed to register %s\n", (name));          \
        }                                                        \
    }                                                            \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_vi
 *
 * Description:
 *   Register the VI-domain gates shared by the VICAP (rkcif) capture front
 *   end and the ISP.
 *
 *   Consumers: rk3576_vicap.c takes aclk_cif_en, hclk_cif_en, dclk_cif_en
 *   and clk_cif_i0_en..clk_cif_i4_en; rk3576_isp.c takes aclk_isp_en,
 *   hclk_isp_en, clk_isp_core_en, clk_isp_core_marvin_en and
 *   clk_isp_core_vicap_en.
 *
 *   Vendor DTS clock ids: dclk_cif 0x16d, aclk_cif 0x16e, hclk_cif 0x16f,
 *   clk_isp_core 0x170, marvin 0x171, vicap 0x172, aclk_isp 0x173,
 *   hclk_isp 0x174, i0clk_cif 0x181 .. i4clk_cif 0x185.
 *
 *   TODO: every GATE_CON index and bit below is inferred and must be
 *   cross-checked against the TRM CRU gate tables.  GATE_CON(31) and
 *   GATE_CON(32) are also the inferred home of the NPU, PCIe, COMBPHY and
 *   GMAC gates, and several bits collide — see the report accompanying this
 *   file; at most one claimant per bit can be correct.
 *
 *   TODO: the mux parents of dclk_cif and clk_isp_core are unknown, so both
 *   are left parented to the gate's default source.  The vendor DT assigns
 *   dclk_cif = 600 MHz.
 *
 ****************************************************************************/

#if defined(CONFIG_RK3576_VICAP) || defined(CONFIG_RK3576_ISP)
static void rk3576_clk_register_vi(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* VICAP (rkcif) — bus, pixel and per-input sampling clocks. */

  RK3576_CLK_REGISTER_VI_GATE("dclk_cif_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 0);
  RK3576_CLK_REGISTER_VI_GATE("aclk_cif_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 1);
  RK3576_CLK_REGISTER_VI_GATE("hclk_cif_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 2);

  RK3576_CLK_REGISTER_VI_GATE("clk_cif_i0_en", "dclk_cif_en",
                              cru + RK3576_CRU_GATE_CON(32), 0);
  RK3576_CLK_REGISTER_VI_GATE("clk_cif_i1_en", "dclk_cif_en",
                              cru + RK3576_CRU_GATE_CON(32), 1);
  RK3576_CLK_REGISTER_VI_GATE("clk_cif_i2_en", "dclk_cif_en",
                              cru + RK3576_CRU_GATE_CON(32), 2);
  RK3576_CLK_REGISTER_VI_GATE("clk_cif_i3_en", "dclk_cif_en",
                              cru + RK3576_CRU_GATE_CON(32), 3);
  RK3576_CLK_REGISTER_VI_GATE("clk_cif_i4_en", "dclk_cif_en",
                              cru + RK3576_CRU_GATE_CON(32), 4);

  /* ISP (rkisp) — bus clocks plus the three core clock branches. */

  RK3576_CLK_REGISTER_VI_GATE("clk_isp_core_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 3);
  RK3576_CLK_REGISTER_VI_GATE("clk_isp_core_marvin_en", "clk_isp_core_en",
                              cru + RK3576_CRU_GATE_CON(31), 4);
  RK3576_CLK_REGISTER_VI_GATE("clk_isp_core_vicap_en", "clk_isp_core_en",
                              cru + RK3576_CRU_GATE_CON(31), 5);
  RK3576_CLK_REGISTER_VI_GATE("aclk_isp_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 6);
  RK3576_CLK_REGISTER_VI_GATE("hclk_isp_en", NULL,
                              cru + RK3576_CRU_GATE_CON(31), 7);

  /* Off-SoC camera master clock (XVCLK) shared by the three IMX415
   * modules.  The board describes it as a fixed-clock node
   * ("external-camera-37m-clock", 37.125 MHz), so there is no CRU gate or
   * mux behind it; a fixed-rate node is all clk_get_rate() needs.
   */

  clk_register_fixed_rate("ext_cam_37m_clk", NULL, CLK_NAME_IS_STATIC,
                          RK3576_EXT_CAM_37M_RATE);
}
#endif /* CONFIG_RK3576_VICAP || CONFIG_RK3576_ISP */

#undef RK3576_CLK_REGISTER_VI_GATE

/****************************************************************************
 * Name: rk3576_clk_register_rknpu
 *
 * Description:
 *   Register the RKNPU (6 TOPS NPU) clock tree of npu@27700000:
 *
 *   - clk_rknn_dsu0_{sel,div,en} : core / DSU clock, parent of every aclk
 *                                  below; the DVFS knob (the OPP table
 *                                  drives it to ~950 MHz, boot value is
 *                                  198 MHz)
 *   - hclk_rknn_root_{sel,en}    : AHB register-interface root
 *   - aclk_rknn0_en              : core0 AXI
 *   - aclk_rknn1_en              : core1 AXI
 *   - aclk_rknn_cbuf_en          : shared convolution buffer, AXI
 *   - hclk_rknn_cbuf_en          : shared convolution buffer, AHB
 *   - pclk_nputop_root_{sel,en}  : NPU top APB (timer / watchdog / GRF)
 *
 *   CLKSEL_CON(86) is shared by three fields; the hiword-mask write scheme
 *   keeps the three registrations independent.
 *
 *   TODO: the register / bit assignments are cross-checked against the DTS
 *   reset identifiers (srst_a0 = 0x1c9 -> CON28 bit 9, srst_a1 = 0x1d0 ->
 *   CON29 bit 0, srst_a_cbuf = 0x200 -> CON32 bit 0, srst_h_cbuf = 0x20c ->
 *   CON32 bit 12), but not against the TRM CRU chapter, and GATE_CON(31) /
 *   GATE_CON(32) collide with the inferred VI, PCIe and GMAC bits.  Confirm
 *   on hardware: clk_get_rate("clk_rknn_dsu0_en") must come out at
 *   198000000 with the stock boot loader.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_RKNPU

/* clk_rknn_dsu0 selects between four PLLs.  AUPLL and SPLL are not
 * registered by the clock tree yet, so their slots fall back to GPLL.
 * Nothing selects them today (the boot loader leaves the mux on GPLL or
 * CPLL), but a wrong parent name would make clk_get_rate() lie.
 * TODO: replace the two fallbacks once AUPLL / SPLL are registered.
 */

static const char *g_rknn_dsu_parents[] = {
  "clk_gpll", /* 0b00 */
  "clk_cpll", /* 0b01 */
  "clk_gpll", /* 0b10 — AUPLL, not registered yet (TODO) */
  "clk_gpll", /* 0b11 — SPLL, not registered yet (TODO) */
};

/* mux_200m_100m_50m_24m: the standard Rockchip "bus root" parent set. */

static const char *g_rknn_hclk_parents[] = {
  "clk_gpll_div6",  /* 0b00 — 200 MHz */
  "clk_cpll_div10", /* 0b01 — 100 MHz */
  "clk_cpll_div20", /* 0b10 —  50 MHz */
  "xin_osc0",       /* 0b11 —  24 MHz */
};

/* mux_100m_50m_24m: 2-bit field whose 0b11 slot is undefined. */

static const char *g_nputop_pclk_parents[] = {
  "clk_cpll_div10", /* 0b00 — 100 MHz */
  "clk_cpll_div20", /* 0b01 —  50 MHz */
  "xin_osc0",       /* 0b10 —  24 MHz */
  "xin_osc0",       /* 0b11 — undefined, fallback */
};

static void rk3576_clk_register_rknpu(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  struct clk_s *mux;

  /* clk_rknn_dsu0 — mux[8:7] + divider[6:2] in CLKSEL_CON(86), gate
   * GATE_CON(31) bit 5.
   */

  mux = clk_register_mux("clk_rknn_dsu0_sel", g_rknn_dsu_parents,
                         nitems(g_rknn_dsu_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(86), 7, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_rknn_dsu0_sel\n");
      return;
    }

  clk_register_divider("clk_rknn_dsu0_div", "clk_rknn_dsu0_sel",
                       CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                       cru + RK3576_CRU_CLKSEL_CON(86), 2, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  clk_register_gate("clk_rknn_dsu0_en", "clk_rknn_dsu0_div",
                    CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(31), 5,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* hclk_rknn_root — mux[1:0] in CLKSEL_CON(86), GATE_CON(31) bit 4. */

  mux = clk_register_mux("hclk_rknn_root_sel", g_rknn_hclk_parents,
                         nitems(g_rknn_hclk_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(86), 0, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register hclk_rknn_root_sel\n");
      return;
    }

  clk_register_gate("hclk_rknn_root_en", "hclk_rknn_root_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(31), 4,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* Per-core AXI gates, both fed by the DSU clock. */

  clk_register_gate("aclk_rknn0_en", "clk_rknn_dsu0_en", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(28), 9,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("aclk_rknn1_en", "clk_rknn_dsu0_en", CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(29), 0,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* Shared convolution buffer: AXI from the DSU clock, AHB from the RKNN
   * AHB root.
   */

  clk_register_gate("aclk_rknn_cbuf_en", "clk_rknn_dsu0_en",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(32), 0,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("hclk_rknn_cbuf_en", "hclk_rknn_root_en",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(32), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* NPU top APB — mux[1:0] in CLKSEL_CON(87), GATE_CON(31) bit 8. */

  mux = clk_register_mux("pclk_nputop_root_sel", g_nputop_pclk_parents,
                         nitems(g_nputop_pclk_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         cru + RK3576_CRU_CLKSEL_CON(87), 0, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register pclk_nputop_root_sel\n");
      return;
    }

  clk_register_gate("pclk_nputop_root_en", "pclk_nputop_root_sel",
                    CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(31), 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_RKNPU */

/****************************************************************************
 * Name: rk3576_clk_register_mali
 *
 * Description:
 *   Register the Mali-G52 GPU clock tree: the shader core clock chain
 *   (PLL mux -> divider -> gate -> PVTPLL mux -> gate), the two AXI ports
 *   of the GPU BIU and the APB root with its three gates.
 *
 *   The register data below was read out of the RK3576 TRM, not inferred:
 *     CRU_CLKSEL_CON165 (0x0594): [9] clk_gpu_pvtpll_src_sel
 *                                 [8] clk_gpu_inner_sel
 *                                 [7:5] clk_gpu_src_pre_sel
 *                                 [4:0] clk_gpu_src_pre_div
 *     CRU_CLKSEL_CON166 (0x0598): [11:10] pclk_gpu_root_sel
 *                                 [9:5] aclk_m0_gpu_biu_div
 *                                 [4:0] aclk_s_gpu_biu_div
 *     CRU_GATE_CON69    (0x0914): b1 clk_gpu_src_pre_en
 *                                 b2 clk_gpu_inner_en
 *                                 b3 clk_gpu_en
 *                                 b4 clk_gpu_pvtpll_src_en
 *                                 b6 aclk_s_gpu_biu_en
 *                                 b7 aclk_m0_gpu_biu_en
 *                                 b8 pclk_gpu_root_en
 *                                 b9 pclk_gpu_biu_en
 *                                 b13 pclk_gpu_grf_en
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_MALI

/* CLKSEL_CON165[7:5].  AUPLL / SPLL / LPLL are not registered by the clock
 * tree yet; the boot loader leaves the GPU on GPLL (0b000) and the DTS
 * assigns 198 MHz = GPLL 1188 MHz / 6, so the missing parents only matter
 * once GPU DVFS is enabled.
 * TODO: register clk_aupll / clk_spll / clk_lpll and drop the fallbacks.
 */

static const char *g_gpu_src_parents[] = {
  "clk_gpll", /* 0b000 */
  "clk_cpll", /* 0b001 */
  "clk_gpll", /* 0b010 — AUPLL, not registered yet (TODO) */
  "clk_gpll", /* 0b011 — SPLL, not registered yet (TODO) */
  "clk_gpll", /* 0b100 — LPLL, not registered yet (TODO) */
};

static const char *g_gpu_inner_parents[] = {
  "clk_gpu_src_pre_en",   /* 0b0 */
  "clk_gpu_pvtpll_src_en" /* 0b1 */
};

static const char *g_gpu_pclk_root_parents[] = {
  "clk_cpll_div10", /* 0b00 */
  "clk_cpll_div20", /* 0b01 */
  "xin_osc0",       /* 0b10 */
  "xin_osc0",       /* 0b11 — undefined, fallback */
};

static void rk3576_clk_register_mali(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long sel165 = cru + RK3576_CRU_CLKSEL_CON(165);
  const unsigned long sel166 = cru + RK3576_CRU_CLKSEL_CON(166);
  const unsigned long gate69 = cru + RK3576_CRU_GATE_CON(69);
  struct clk_s *mux;

  /* Shader core clock chain. */

  mux = clk_register_mux("clk_gpu_src_sel", g_gpu_src_parents,
                         nitems(g_gpu_src_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         sel165, 5, 3, CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_gpu_src_sel\n");
      return;
    }

  clk_register_divider("clk_gpu_src_div", "clk_gpu_src_sel",
                       CLK_NAME_IS_STATIC, sel165, 0, 5,
                       CLK_DIVIDER_HIWORD_MASK);

  clk_register_gate("clk_gpu_src_pre_en", "clk_gpu_src_div",
                    CLK_NAME_IS_STATIC, gate69, 1,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_gpu_pvtpll_src_en", NULL, CLK_NAME_IS_STATIC,
                    gate69, 4,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  mux = clk_register_mux("clk_gpu_inner_sel", g_gpu_inner_parents,
                         nitems(g_gpu_inner_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         sel165, 8, 1, CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_gpu_inner_sel\n");
      return;
    }

  clk_register_gate("clk_gpu_inner_en", "clk_gpu_inner_sel",
                    CLK_NAME_IS_STATIC, gate69, 2,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_gpu_en", "clk_gpu_inner_en", CLK_NAME_IS_STATIC,
                    gate69, 3,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* AXI ports of the GPU BIU.
   *
   * TODO: the dividers in CLKSEL_CON166[9:5] (aclk_m0) and [4:0] (aclk_s)
   * hang off an interconnect matrix clock the tree does not model yet, so
   * only the gates are registered here.
   */

  clk_register_gate("aclk_s_gpu_biu_en", NULL, CLK_NAME_IS_STATIC,
                    gate69, 6,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("aclk_m0_gpu_biu_en", NULL, CLK_NAME_IS_STATIC,
                    gate69, 7,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* APB side: root mux plus the three gates. */

  mux = clk_register_mux("pclk_gpu_root_sel", g_gpu_pclk_root_parents,
                         nitems(g_gpu_pclk_root_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         sel166, 10, 2, CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register pclk_gpu_root_sel\n");
      return;
    }

  clk_register_gate("pclk_gpu_root_en", "pclk_gpu_root_sel",
                    CLK_NAME_IS_STATIC, gate69, 8,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("pclk_gpu_biu_en", "pclk_gpu_root_en",
                    CLK_NAME_IS_STATIC, gate69, 9,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("pclk_gpu_grf_en", "pclk_gpu_root_en",
                    CLK_NAME_IS_STATIC, gate69, 13,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_MALI */

/**
 * Macro: RK3576_CLK_REGISTER_GMAC_ONE
 *
 * Register one GMAC clock sub-tree: aclk gate, pclk gate and the functional
 * clock mux + divider + gate.  The driver consumes aclk_gmac<n>_en,
 * pclk_gmac<n>_en and clk_gmac<n>_en, whose rate it sets to 125 / 25 /
 * 2.5 MHz as the link speed changes.
 *
 * Parameters:
 *   n         - controller index (0 or 1)
 *   sel_reg   - CLKSEL register address of the mux
 *   sel_shift - MUX select field bit offset
 *   div_shift - divider field bit offset in the same CLKSEL register
 *   gate_reg  - GATE register address
 *   aclk_bit  - aclk GATE bit
 *   pclk_bit  - pclk GATE bit
 *   clk_bit   - functional clock GATE bit
 */

#define RK3576_CLK_REGISTER_GMAC_ONE(n, sel_reg, sel_shift, div_shift,      \
                                     gate_reg, aclk_bit, pclk_bit,          \
                                     clk_bit)                               \
  do                                                                        \
    {                                                                       \
      struct clk_s *_mux;                                                   \
                                                                            \
      _mux = clk_register_mux("clk_gmac" #n "_sel", g_gmac_sel_parents,     \
                              nitems(g_gmac_sel_parents),                   \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,     \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK);  \
      if (!_mux)                                                            \
        {                                                                   \
          _err("CLK: failed to register clk_gmac" #n "_sel\n");             \
          break;                                                            \
        }                                                                   \
                                                                            \
      clk_register_divider("clk_gmac" #n "_div", "clk_gmac" #n "_sel",      \
                           CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,        \
                           sel_reg, div_shift, 5,                           \
                           CLK_DIVIDER_HIWORD_MASK);                        \
                                                                            \
      clk_register_gate("aclk_gmac" #n "_en", NULL, CLK_NAME_IS_STATIC,     \
                        gate_reg, aclk_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("pclk_gmac" #n "_en", NULL, CLK_NAME_IS_STATIC,     \
                        gate_reg, pclk_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
                                                                            \
      clk_register_gate("clk_gmac" #n "_en", "clk_gmac" #n "_div",          \
                        CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,           \
                        gate_reg, clk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);    \
    }                                                                       \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_gmac
 *
 * Description:
 *   Register the clock sub-trees of both gigabit Ethernet controllers.
 *
 *   TODO: the CLKSEL/GATE register indices and bit positions are inferred
 *   from the vendor DTS clock ids (aclk_mac = 293, pclk_mac = 295,
 *   stmmaceth = 42/43, clk_mac_ref = 52/53) and from the CRU layout of the
 *   neighbouring PHP/GMAC blocks.  They must be checked against the TRM
 *   before the driver is trusted on hardware; a wrong bit silently leaves
 *   the block unclocked and every register read comes back as zero.  Note
 *   also that GATE_CON(32) bits 2..7 are claimed by the inferred VI,
 *   COMBPHY and NPU nodes as well.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_GMAC

/* Parents of the GMAC functional (RGMII TX) clock mux.
 * TODO: verify the 2-bit encoding order against the TRM CRU chapter.
 */

static const char *g_gmac_sel_parents[] = {
  "clk_gpll_div4", /* 0b00 */
  "clk_cpll_div4", /* 0b01 */
  "clk_gpll_div6", /* 0b10 */
  "xin_osc0",      /* 0b11 */
};

static void rk3576_clk_register_gmac(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  RK3576_CLK_REGISTER_GMAC_ONE(0, cru + RK3576_CRU_CLKSEL_CON(104), 0, 2,
                               cru + RK3576_CRU_GATE_CON(32),
                               2,  /* aclk_gmac0 */
                               4,  /* pclk_gmac0 */
                               6); /* clk_gmac0  */

  RK3576_CLK_REGISTER_GMAC_ONE(1, cru + RK3576_CRU_CLKSEL_CON(104), 7, 9,
                               cru + RK3576_CRU_GATE_CON(32),
                               3,  /* aclk_gmac1 */
                               5,  /* pclk_gmac1 */
                               7); /* clk_gmac1  */
}
#endif /* CONFIG_RK3576_GMAC */

#undef RK3576_CLK_REGISTER_GMAC_ONE

/**
 * Macro: RK3576_CLK_REGISTER_PCIE_ONE
 *
 * Register the five gates of one DesignWare PCIe root complex port.
 *
 * Parameters:
 *   port     - PCIe port index (0 or 1)
 *   gate_reg - GATE register address
 *   mst_bit  - master AXI GATE bit
 *   slv_bit  - slave AXI GATE bit
 *   dbi_bit  - DBI AXI GATE bit
 *   pclk_bit - APB GATE bit
 *   aux_bit  - auxiliary clock GATE bit
 */

#define RK3576_CLK_REGISTER_PCIE_ONE(port, gate_reg, mst_bit, slv_bit,     \
                                     dbi_bit, pclk_bit, aux_bit)           \
  do                                                                       \
    {                                                                      \
      clk_register_gate("aclk_pcie" #port "_mst_en", "aclk_php_root_en",   \
                        CLK_NAME_IS_STATIC, gate_reg, mst_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("aclk_pcie" #port "_slv_en", "aclk_php_root_en",   \
                        CLK_NAME_IS_STATIC, gate_reg, slv_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("aclk_pcie" #port "_dbi_en", "aclk_php_root_en",   \
                        CLK_NAME_IS_STATIC, gate_reg, dbi_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("pclk_pcie" #port "_en", NULL, CLK_NAME_IS_STATIC, \
                        gate_reg, pclk_bit,                                \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("clk_pcie" #port "_aux_en", "xin_osc0",            \
                        CLK_NAME_IS_STATIC, gate_reg, aux_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/**
 * Macro: RK3576_CLK_REGISTER_COMBPHY_ONE
 *
 * Register the reference clock chain (mux + gate), the APB gate and the
 * pipe clock gate of one Naneng combo PHY.
 *
 * Parameters:
 *   phy       - combo PHY index (0 or 1)
 *   sel_reg   - CLKSEL register address of the reference clock mux
 *   sel_shift - MUX select field bit offset (1-bit field)
 *   gate_reg  - GATE register address
 *   ref_bit   - reference clock GATE bit
 *   apb_bit   - APB GATE bit
 *   pipe_bit  - pipe clock GATE bit
 */

#define RK3576_CLK_REGISTER_COMBPHY_ONE(phy, sel_reg, sel_shift, gate_reg, \
                                        ref_bit, apb_bit, pipe_bit)        \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      _mux = clk_register_mux("clk_combphy" #phy "_ref_sel",               \
                              g_combphy_ref_parents,                       \
                              nitems(g_combphy_ref_parents),               \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 1, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_combphy" #phy "_ref_sel\n");   \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("clk_combphy" #phy "_ref_en",                      \
                        "clk_combphy" #phy "_ref_sel", CLK_NAME_IS_STATIC, \
                        gate_reg, ref_bit,                                 \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("pclk_combphy" #phy "_en", NULL,                   \
                        CLK_NAME_IS_STATIC, gate_reg, apb_bit,             \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
      clk_register_gate("clk_combphy" #phy "_pipe_en", NULL,               \
                        CLK_NAME_IS_STATIC, gate_reg, pipe_bit,            \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_pcie
 *
 * Description:
 *   Register the clocks of both PCIe root complexes and of the two Naneng
 *   combo PHYs that serve them.
 *
 *   Vendor DTS references: pcie@2a200000 clocks 0x108/0x109/0x10a/0x106/
 *   0x107 (aclk_mst, aclk_slv, aclk_dbi, pclk, aux), pcie@2a210000 clocks
 *   0x115/0x116/0x117/0x113/0x114, phy@2b050000 clocks 0x140/0x13c/0x106
 *   (refclk, apbclk, pipe_clk) and phy@2b060000 clocks 0x141/0x13d/0x113.
 *
 *   TODO: the GATE_CON register index and bit of every node below are
 *   PLACEHOLDERS derived from the PHP-domain gate block ordering, NOT read
 *   from the TRM.  They must be corrected against TRM CRU_GATE_CON30..33
 *   before this is trusted on silicon, and the 100 MHz reference clock
 *   mux/divider is likewise unverified.  GATE_CON(31) and GATE_CON(32) are
 *   additionally claimed by the inferred VI, NPU and GMAC nodes.
 *
 ****************************************************************************/

#if defined(CONFIG_RK3576_PCIE) || defined(CONFIG_RK3576_COMBPHY)

/* Parents of the combo PHY reference clock mux. */

static const char *g_combphy_ref_parents[] = {
  "clk_cpll_div10", /* 0b0 — 100 MHz when CPLL runs at 1 GHz */
  "xin_osc0",       /* 0b1 */
};

static void rk3576_clk_register_pcie(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;

  /* Shared PHP (peripheral high-speed) AXI root — parent of both ports.
   * TODO: verify GATE_CON(30) bit 0.
   */

  clk_register_gate("aclk_php_root_en", NULL, CLK_NAME_IS_STATIC,
                    cru + RK3576_CRU_GATE_CON(30), 0,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  /* TODO: placeholder gate bits, see the banner above. */

  RK3576_CLK_REGISTER_PCIE_ONE(0, cru + RK3576_CRU_GATE_CON(30),
                               8, 9, 10, 6, 7);
  RK3576_CLK_REGISTER_PCIE_ONE(1, cru + RK3576_CRU_GATE_CON(31),
                               5, 6, 7, 3, 4);

  RK3576_CLK_REGISTER_COMBPHY_ONE(0, cru + RK3576_CRU_CLKSEL_CON(153), 0,
                                  cru + RK3576_CRU_GATE_CON(32),
                                  0, 12, 6);
  RK3576_CLK_REGISTER_COMBPHY_ONE(1, cru + RK3576_CRU_CLKSEL_CON(153), 1,
                                  cru + RK3576_CRU_GATE_CON(32),
                                  1, 13, 3);
}
#endif /* CONFIG_RK3576_PCIE || CONFIG_RK3576_COMBPHY */

#undef RK3576_CLK_REGISTER_PCIE_ONE
#undef RK3576_CLK_REGISTER_COMBPHY_ONE

/**
 * Macro: RK3576_CLK_REGISTER_PDM_ONE
 *
 * Register one PDM controller clock group: the AHB gate, the functional
 * clock gate and the bit-clock output root (mux + gate).
 *
 * Parameters:
 *   ctrl      - controller index (0 or 1), used as name suffix
 *   sel_reg   - CLKSEL register address of the clk_out mux
 *   sel_shift - clk_out MUX select field bit offset
 *   hclk_reg  - hclk GATE register address
 *   hclk_bit  - hclk GATE bit
 *   clk_reg   - functional clock GATE register address
 *   clk_bit   - functional clock GATE bit
 *   out_reg   - clk_out GATE register address
 *   out_bit   - clk_out GATE bit
 */

#define RK3576_CLK_REGISTER_PDM_ONE(ctrl, sel_reg, sel_shift, hclk_reg,    \
                                    hclk_bit, clk_reg, clk_bit, out_reg,   \
                                    out_bit)                               \
  do                                                                       \
    {                                                                      \
      struct clk_s *_mux;                                                  \
                                                                           \
      clk_register_gate("hclk_pdm" #ctrl "_en", NULL, CLK_NAME_IS_STATIC,  \
                        hclk_reg, hclk_bit,                                \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      clk_register_gate("clk_pdm" #ctrl "_en", NULL, CLK_NAME_IS_STATIC,   \
                        clk_reg, clk_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
                                                                           \
      _mux = clk_register_mux("clk_pdm" #ctrl "_out_sel",                  \
                              g_pdm_sel_parents,                           \
                              nitems(g_pdm_sel_parents),                   \
                              CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,    \
                              sel_reg, sel_shift, 2, CLK_MUX_HIWORD_MASK); \
      if (!_mux)                                                           \
        {                                                                  \
          _err("CLK: failed to register clk_pdm" #ctrl "_out_sel\n");      \
          break;                                                           \
        }                                                                  \
                                                                           \
      clk_register_gate("clk_pdm" #ctrl "_out_en",                         \
                        "clk_pdm" #ctrl "_out_sel", CLK_NAME_IS_STATIC,    \
                        out_reg, out_bit,                                  \
                        CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);   \
    }                                                                      \
  while (0)

/****************************************************************************
 * Name: rk3576_clk_register_pdm
 *
 * Description:
 *   Register the clocks of the two PDM microphone-array controllers.  The
 *   device-tree clock-names are "pdm_hclk" / "pdm_clk" / "pdm_clk_out"; the
 *   node names below follow the hclk_ / clk_ convention of the main tree.
 *
 *   Vendor DTS clock ids: PDM0 (pdm@273b0000) 0x1fb hclk, 0x1fc clk,
 *   0x1ff clk_out, all in the PMU1_CRU domain; PDM1 (pdm@2a6e0000) 0x5b
 *   hclk, 0x5c clk, 0x29 clk_out in the main CRU.
 *
 *   TODO: the GATE_CON / CLKSEL_CON register indices and bit positions are
 *   derived from the Rockchip clock-ID ordering and still need a TRM
 *   cross-check.  The names, parents and flags are correct as written.
 *
 ****************************************************************************/

#ifdef CONFIG_RK3576_PDM

/* PDM bit-clock root mux: audio PLL / GPLL fractions, matching the parent
 * list the vendor driver uses for "pdm_clk_out".
 * TODO: confirm the 2-bit encoding against the TRM.
 */

static const char *g_pdm_sel_parents[] = {
  "clk_aupll",      /* 0b00 */
  "clk_gpll_div6",  /* 0b01 */
  "clk_cpll_div10", /* 0b10 */
  "xin_osc0",       /* 0b11 */
};

static void rk3576_clk_register_pdm(void)
{
  const unsigned long cru = RK3576_CRU_ADDR;
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;

  /* PDM0 — PMU1_CRU domain (pdm@273b0000).  TODO: verify. */

  RK3576_CLK_REGISTER_PDM_ONE(0, pmu1 + RK3576_PMU1CRU_CLKSEL_CON(7), 0,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(6), 6,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(6), 7,
                              pmu1 + RK3576_PMU1CRU_GATE_CON(6), 10);

  /* PDM1 — main CRU domain (pdm@2a6e0000).  TODO: verify. */

  RK3576_CLK_REGISTER_PDM_ONE(1, cru + RK3576_CRU_CLKSEL_CON(28), 0,
                              cru + RK3576_CRU_GATE_CON(6), 11,
                              cru + RK3576_CRU_GATE_CON(6), 12,
                              cru + RK3576_CRU_GATE_CON(3), 9);
}
#endif /* CONFIG_RK3576_PDM */

#undef RK3576_CLK_REGISTER_PDM_ONE

/****************************************************************************
 * Name: rk3576_clk_register_ir
 *
 * Description:
 *   Register the PWM0 clocks on behalf of the infrared receiver.
 *
 *   The IR receiver runs on PWM0 channel 0 in power-key capture mode and
 *   needs pclk_pwm0_en and clk_pwm0_osc_en even when the PWM waveform
 *   driver is not built.  rk3576_clk_register_pwm() in rk3576_clk_tree.c
 *   registers the very same nodes, so this function only exists when
 *   CONFIG_RK3576_PWM is off — there is never a double registration.
 *
 *   The register mapping is copied verbatim from rk3576_clk_tree.c:
 *     PMU1CRU_CLKSEL_CON(5) shift 2    — clk_pwm0_sel (2-bit mux)
 *     PMU1CRU_GATE_CON(4) bits 11/12/13 — pclk / clk / osc gates
 *
 ****************************************************************************/

#if defined(CONFIG_RK3576_IR) && !defined(CONFIG_RK3576_PWM)
static const char *g_ir_pwm0_sel_parents[] = {
  "clk_cpll_div10", /* 0b00 */
  "clk_cpll_div20", /* 0b01 */
  "xin_osc0",       /* 0b10 */
  "xin_osc0",       /* 0b11 — undefined, fallback */
};

static void rk3576_clk_register_ir(void)
{
  const unsigned long pmu1 = RK3576_PMU1_CRU_ADDR;
  struct clk_s *mux;

  mux = clk_register_mux("clk_pwm0_sel", g_ir_pwm0_sel_parents,
                         nitems(g_ir_pwm0_sel_parents),
                         CLK_SET_RATE_PARENT | CLK_NAME_IS_STATIC,
                         pmu1 + RK3576_PMU1CRU_CLKSEL_CON(5), 2, 2,
                         CLK_MUX_HIWORD_MASK);
  if (!mux)
    {
      _err("CLK: failed to register clk_pwm0_sel\n");
      return;
    }

  clk_register_gate("pclk_pwm0_en", NULL, CLK_NAME_IS_STATIC,
                    pmu1 + RK3576_PMU1CRU_GATE_CON(4), 11,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_pwm0_en", "clk_pwm0_sel", CLK_NAME_IS_STATIC,
                    pmu1 + RK3576_PMU1CRU_GATE_CON(4), 12,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);

  clk_register_gate("clk_pwm0_osc_en", "xin_osc0", CLK_NAME_IS_STATIC,
                    pmu1 + RK3576_PMU1CRU_GATE_CON(4), 13,
                    CLK_GATE_HIWORD_MASK | CLK_GATE_SET_TO_DISABLE);
}
#endif /* CONFIG_RK3576_IR && !CONFIG_RK3576_PWM */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_clk_tree_periph_initialize
 *
 * Description:
 *   Register the peripheral half of the RK3576 clock tree with the NuttX
 *   CLK framework.  Call this once during board init, right after
 *   rk3576_clk_tree_initialize() (which provides xin_osc0, the PLLs and
 *   their fixed-factor dividers) and before any peripheral driver calls
 *   clk_get().
 *
 *   The PMU power-domain driver (rk3576_pd.c) needs no node: the PMU
 *   register block sits in the always-on PD_PMU1 domain whose APB clock is
 *   enabled out of reset and has no gate in the TRM.
 *
 ****************************************************************************/

void rk3576_clk_tree_periph_initialize(void)
{
  /* PLLs and post-dividers the peripheral branches depend on. */

  rk3576_clk_register_periph_plls();

  /* Low-speed bus peripherals. */

#ifdef CONFIG_RK3576_WDT
  rk3576_clk_register_wdt();
#endif

#ifdef CONFIG_RK3576_SPI
  rk3576_clk_register_spi();
#endif

#ifdef CONFIG_RK3576_MAILBOX
  rk3576_clk_register_mailbox();
#endif

#ifdef CONFIG_RK3576_I3C
  rk3576_clk_register_i3c();
#endif

  /* Analog front ends. */

#ifdef CONFIG_RK3576_SARADC
  rk3576_clk_register_saradc();
#endif

#ifdef CONFIG_RK3576_TSADC
  rk3576_clk_register_tsadc();
#endif

  /* Security blocks (SECURECRU domain for OTP and RNG). */

#ifdef CONFIG_RK3576_CRYPTO
  rk3576_clk_register_crypto();
#endif

#ifdef CONFIG_RK3576_OTP
  rk3576_clk_register_otp();
#endif

#ifdef CONFIG_RK3576_RNG
  rk3576_clk_register_rng();
#endif

  /* Display and video codec. */

#ifdef CONFIG_RK3576_VOP
  rk3576_clk_register_vop();
#endif

#ifdef CONFIG_RK3576_HDMI
  rk3576_clk_register_hdmi();
#endif

#ifdef CONFIG_RK3576_RGA
  rk3576_clk_register_rga();
#endif

#ifdef CONFIG_RK3576_VDEC
  rk3576_clk_register_vdec();
#endif

  /* Camera capture pipeline. */

#ifdef CONFIG_RK3576_CSI
  rk3576_clk_register_csi();
#endif

#if defined(CONFIG_RK3576_VICAP) || defined(CONFIG_RK3576_ISP)
  rk3576_clk_register_vi();
#endif

  /* Compute accelerators. */

#ifdef CONFIG_RK3576_RKNPU
  rk3576_clk_register_rknpu();
#endif

#ifdef CONFIG_RK3576_MALI
  rk3576_clk_register_mali();
#endif

  /* Networking and high-speed serial. */

#ifdef CONFIG_RK3576_GMAC
  rk3576_clk_register_gmac();
#endif

#if defined(CONFIG_RK3576_PCIE) || defined(CONFIG_RK3576_COMBPHY)
  rk3576_clk_register_pcie();
#endif

  /* Audio capture. */

#ifdef CONFIG_RK3576_PDM
  rk3576_clk_register_pdm();
#endif

  /* Infrared receiver — only when the PWM driver does not already own the
   * PWM0 clock nodes.
   */

#if defined(CONFIG_RK3576_IR) && !defined(CONFIG_RK3576_PWM)
  rk3576_clk_register_ir();
#endif
}

#endif /* CONFIG_CLK */
