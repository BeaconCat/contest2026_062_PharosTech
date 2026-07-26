/****************************************************************************
 * chips/rk3576/hardware/rk3576_vop.h
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
 * RK3576 VOP (Video Output Processor) hardware register definitions.
 *
 * The RK3576 display controller is a Rockchip "VOP2" instance, the same
 * architecture family as RK3568/RK3588.  One VOP block drives three video
 * ports (VP0..VP2) and a set of overlay windows:
 *
 *   Cluster0/Cluster1 - full featured windows (AFBC, rotation, scaling)
 *   Esmart0/Esmart1   - scaling windows without AFBC
 *
 * Register space (vendor DTS node vop@27d00000):
 *
 *   0x27D00000 + 0x3000  "regs"        main register file (used here)
 *   0x27D05000 + 0x1000  "gamma_lut"   gamma LUT      (not used)
 *   0x27D06400 + 0x0800  "acm_regs"    colour management (not used)
 *   0x27D06C00 + 0x0300  "sharp_regs"  sharpness      (not used)
 *
 * The main register file is split into functional blocks:
 *
 *   0x0000  SYS      version, auto gating, display interface routing
 *   0x0600  OVL      overlay / layer mixing and per-VP layer routing
 *   0x0C00  VP0      video port 0 timing and display control
 *   0x0D00  VP1
 *   0x0E00  VP2
 *   0x1000  Cluster0
 *   0x1200  Cluster1
 *   0x1800  Esmart0
 *   0x1A00  Esmart1
 *
 * Writes to the timing/window registers are shadowed: they only take
 * effect when the matching bit is written to RK3576_VOP_REG_CFG_DONE.
 *
 * The register offsets and bit definitions below follow the public
 * Rockchip VOP2 programming model (RK3568/RK3588 generation), which the
 * RK3576 VOP is register compatible with for the subset used by this
 * driver.
 *
 * TODO: The display-interface routing block (RK3576_VOP_SYS_DSP_IF_*) is
 * the one area where RK3576 differs the most between SoC generations.
 * The HDMI bit positions below must be re-checked against the RK3576 TRM
 * "VOP" chapter once it is available; everything else has been kept to
 * the register set that is stable across the VOP2 family.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VOP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register bases.
 *
 * TODO: move RK3576_VOP_ADDR / RK3576_VOP_MMU_ADDR into
 * hardware/rk3576_memorymap.h once the shared header is updated.
 */

#ifndef RK3576_VOP_MMU_ADDR
#define RK3576_VOP_MMU_ADDR 0x27d07e00 /* rockchip,iommu-v2 for VOP   */
#endif

#define RK3576_VOP_REGS_SIZE 0x00003000 /* Size of the "regs" window   */

/* Interrupts.
 *
 * The vendor DTS declares four GIC SPI lines for the VOP:
 *
 *   interrupts = <0 0x156 4>,  SPI 342 -> INTID 374  "vop-sys"
 *                <0 0x17b 4>,  SPI 379 -> INTID 411  "vop-vp0"
 *                <0 0x17c 4>,  SPI 380 -> INTID 412  "vop-vp1"
 *                <0 0x17d 4>;  SPI 381 -> INTID 413  "vop-vp2"
 *
 * GIC INTID = SPI number + 32.  The "vop-sys" line is shared with the
 * VOP IOMMU (iommu@27d07e00 uses the same SPI 342).
 *
 * The vector numbers themselves live in chips/rk3576/include/irq.h as
 * RK3576_IRQ_VOP (vop-sys) and RK3576_IRQ_VOP_VP0..VP2.
 */

/* Block bases (offsets from RK3576_VOP_ADDR) ******************************/

#define RK3576_VOP_SYS_BASE       0x0000
#define RK3576_VOP_OVL_BASE       0x0600
#define RK3576_VOP_VP_BASE        0x0c00 /* VP0; VP1/VP2 add stride   */
#define RK3576_VOP_VP_STRIDE      0x0100
#define RK3576_VOP_CLUSTER_BASE   0x1000
#define RK3576_VOP_CLUSTER_STRIDE 0x0200
#define RK3576_VOP_ESMART_BASE    0x1800
#define RK3576_VOP_ESMART_STRIDE  0x0200

#define RK3576_VOP_VP(n)          (RK3576_VOP_VP_BASE + (n)*RK3576_VOP_VP_STRIDE)
#define RK3576_VOP_ESMART(n) \
  (RK3576_VOP_ESMART_BASE + (n)*RK3576_VOP_ESMART_STRIDE)
#define RK3576_VOP_CLUSTER(n) \
  (RK3576_VOP_CLUSTER_BASE + (n)*RK3576_VOP_CLUSTER_STRIDE)

/* Number of hardware resources */

#define RK3576_VOP_NVP      3 /* VP0, VP1, VP2                  */
#define RK3576_VOP_NESMART  2 /* Esmart0, Esmart1               */
#define RK3576_VOP_NCLUSTER 2 /* Cluster0, Cluster1             */

/* SYS block **************************************************************/

#define RK3576_VOP_VERSION_INFO     0x0000 /* RO major/minor            */
#define RK3576_VOP_SYS_AUTO_GATING  0x0004 /* Automatic clock gating    */
#define RK3576_VOP_SYS_AXI_LUT_CTRL 0x0024
#define RK3576_VOP_SYS_DSP_IF_EN    0x0028 /* Display interface enable  */
#define RK3576_VOP_SYS_DSP_IF_CTRL  0x002c /* Display interface control */
#define RK3576_VOP_SYS_DSP_IF_POL   0x0030 /* Display interface polarity*/
#define RK3576_VOP_SYS_STATUS0      0x0060 /* RO                        */
#define RK3576_VOP_REG_CFG_DONE     0x000c /* Shadow register commit    */

/* RK3576_VOP_SYS_AUTO_GATING */

#define VOP_SYS_AUTO_GATING_EN (1u << 31) /* Enable auto gating    */

/* RK3576_VOP_REG_CFG_DONE.  The low bits are a per-VP "take my shadow
 * registers now" request; bit 15 is the global write enable that arms
 * them.  Writing 0 has no effect, so the register is write-only pulse.
 */

#define VOP_REG_CFG_DONE_VP(n) (1u << (n))
#define VOP_REG_CFG_DONE_EN    (1u << 15)

/* RK3576_VOP_SYS_DSP_IF_EN - one enable bit per output interface plus a
 * 2-bit "which VP feeds me" selector per interface.
 *
 * TODO: verify the HDMI bit/field positions against the RK3576 TRM.  The
 * values below are the VOP2 family layout used by RK3568/RK3588.
 */

#define VOP_DSP_IF_EN_RGB             (1u << 0)  /* Parallel RGB / BT656  */
#define VOP_DSP_IF_EN_HDMI0           (1u << 1)  /* HDMI TX0              */
#define VOP_DSP_IF_EN_EDP0            (1u << 3)  /* eDP0                  */
#define VOP_DSP_IF_EN_MIPI0           (1u << 4)  /* MIPI DSI0             */
#define VOP_DSP_IF_EN_LVDS0           (1u << 5)  /* LVDS0                 */
#define VOP_DSP_IF_EN_MIPI1           (1u << 20) /* MIPI DSI1             */
#define VOP_DSP_IF_EN_LVDS1           (1u << 24) /* LVDS1                 */

#define VOP_DSP_IF_EN_HDMI0_MUX_SHIFT 10 /* [11:10] source VP     */
#define VOP_DSP_IF_EN_HDMI0_MUX_MASK  (3u << VOP_DSP_IF_EN_HDMI0_MUX_SHIFT)
#define VOP_DSP_IF_EN_HDMI0_MUX(vp)                    \
  (((uint32_t)(vp) << VOP_DSP_IF_EN_HDMI0_MUX_SHIFT) & \
   VOP_DSP_IF_EN_HDMI0_MUX_MASK)

/* RK3576_VOP_SYS_DSP_IF_CTRL - pixel repetition / data width per output */

#define VOP_DSP_IF_CTRL_HDMI0_DCLK_DIV_SHIFT 0
#define VOP_DSP_IF_CTRL_HDMI0_DCLK_DIV_MASK  (3u << 0)
#define VOP_DSP_IF_CTRL_HDMI0_PIN_POL_SHIFT  4
#define VOP_DSP_IF_CTRL_HDMI0_PIN_POL_MASK   (0xfu << 4)

/* RK3576_VOP_SYS_DSP_IF_POL - sync polarity, 4 bits per interface.
 * Bit 0 of a field is HSYNC active-high, bit 1 is VSYNC active-high.
 */

#define VOP_DSP_IF_POL_HDMI0_SHIFT 16
#define VOP_DSP_IF_POL_HDMI0_MASK  (0xfu << VOP_DSP_IF_POL_HDMI0_SHIFT)
#define VOP_DSP_IF_POL_HSYNC_HIGH  (1u << 0)
#define VOP_DSP_IF_POL_VSYNC_HIGH  (1u << 1)
#define VOP_DSP_IF_POL_DEN_HIGH    (1u << 2)
#define VOP_DSP_IF_POL_DCLK_INV    (1u << 3)

/* OVL block **************************************************************/

#define RK3576_VOP_OVL_CTRL          0x0600 /* Global overlay control    */
#define RK3576_VOP_OVL_LAYER_SEL     0x0604 /* Z-order: window per layer */
#define RK3576_VOP_OVL_PORT_SEL      0x0608 /* Layer -> VP assignment    */
#define RK3576_VOP_VP_BG_MIX_CTRL(n) (0x0670 + (n)*4)

/* RK3576_VOP_OVL_CTRL */

#define VOP_OVL_CTRL_LAYER_SEL_PORT_SHIFT 0
#define VOP_OVL_CTRL_LAYER_SEL_PORT_MASK  (3u << 0)
#define VOP_OVL_CTRL_YUV_MODE_VP(n)       (1u << (16 + (n)))

/* RK3576_VOP_OVL_LAYER_SEL: 4 bits per overlay layer, selecting which
 * window feeds that layer.  Layer 0 is the bottom of the Z stack.
 */

#define VOP_OVL_LAYER_SEL_SHIFT(layer) ((layer)*4)
#define VOP_OVL_LAYER_SEL_MASK(layer)  (0xfu << VOP_OVL_LAYER_SEL_SHIFT(layer))
#define VOP_OVL_LAYER_SEL(layer, win) \
  (((uint32_t)(win)&0xfu) << VOP_OVL_LAYER_SEL_SHIFT(layer))

/* Window IDs as seen by OVL_LAYER_SEL */

#define VOP_OVL_WIN_CLUSTER0 0
#define VOP_OVL_WIN_CLUSTER1 1
#define VOP_OVL_WIN_ESMART0  2
#define VOP_OVL_WIN_ESMART1  3
#define VOP_OVL_WIN_SMART0   4
#define VOP_OVL_WIN_SMART1   5

/* RK3576_VOP_OVL_PORT_SEL: number of layers attached to each VP in the
 * low half, and a 2-bit destination VP per layer in the high half.
 */

#define VOP_OVL_PORT_SEL_NLAYER_SHIFT(vp) (4 + (vp)*4)
#define VOP_OVL_PORT_SEL_NLAYER_MASK(vp) \
  (0xfu << VOP_OVL_PORT_SEL_NLAYER_SHIFT(vp))
#define VOP_OVL_PORT_SEL_NLAYER(vp, n) \
  (((uint32_t)(n)&0xfu) << VOP_OVL_PORT_SEL_NLAYER_SHIFT(vp))

#define VOP_OVL_PORT_SEL_LAYER_SHIFT(layer) (16 + (layer)*2)
#define VOP_OVL_PORT_SEL_LAYER_MASK(layer) \
  (3u << VOP_OVL_PORT_SEL_LAYER_SHIFT(layer))
#define VOP_OVL_PORT_SEL_LAYER(layer, vp) \
  (((uint32_t)(vp)&3u) << VOP_OVL_PORT_SEL_LAYER_SHIFT(layer))

/* Video port block (offsets relative to RK3576_VOP_VP(n)) ****************/

#define RK3576_VOP_VP_DSP_CTRL             0x0000
#define RK3576_VOP_VP_MIPI_CTRL            0x0004
#define RK3576_VOP_VP_COLOR_BAR_CTRL       0x0008
#define RK3576_VOP_VP_3D_LUT_CTRL          0x0010
#define RK3576_VOP_VP_DSP_BG               0x002c
#define RK3576_VOP_VP_PRE_SCAN_HTIMING     0x0030
#define RK3576_VOP_VP_POST_DSP_HACT_INFO   0x0034
#define RK3576_VOP_VP_POST_DSP_VACT_INFO   0x0038
#define RK3576_VOP_VP_POST_SCL_FACTOR_YRGB 0x003c
#define RK3576_VOP_VP_POST_SCL_CTRL        0x0040
#define RK3576_VOP_VP_DSP_HTOTAL_HS_END    0x0048
#define RK3576_VOP_VP_DSP_HACT_ST_END      0x004c
#define RK3576_VOP_VP_DSP_VTOTAL_VS_END    0x0050
#define RK3576_VOP_VP_DSP_VACT_ST_END      0x0054
#define RK3576_VOP_VP_INT_EN               0x00a0
#define RK3576_VOP_VP_INT_CLR              0x00a4
#define RK3576_VOP_VP_INT_STATUS           0x00a8
#define RK3576_VOP_VP_INT_RAW_STATUS       0x00ac

/* VP DSP_CTRL */

#define VOP_VP_DSP_CTRL_OUT_MODE_SHIFT     0
#define VOP_VP_DSP_CTRL_OUT_MODE_MASK      (0xfu << 0)
#define VOP_VP_DSP_CTRL_OUT_MODE(m)        (((uint32_t)(m)&0xfu) << 0)
#define VOP_VP_DSP_CTRL_CORE_DCLK_DIV      (1u << 4)
#define VOP_VP_DSP_CTRL_P2I_EN             (1u << 5)
#define VOP_VP_DSP_CTRL_FIELD_POL          (1u << 6)
#define VOP_VP_DSP_CTRL_INTERLACE          (1u << 7)
#define VOP_VP_DSP_CTRL_DSP_FILTER_EN      (1u << 8)
#define VOP_VP_DSP_CTRL_BG_SWAP            (1u << 9)
#define VOP_VP_DSP_CTRL_RB_SWAP            (1u << 10)
#define VOP_VP_DSP_CTRL_RG_SWAP            (1u << 11)
#define VOP_VP_DSP_CTRL_DELTA_SWAP         (1u << 12)
#define VOP_VP_DSP_CTRL_DSP_BLANK          (1u << 14)
#define VOP_VP_DSP_CTRL_OUT_R601           (1u << 15)
#define VOP_VP_DSP_CTRL_PRE_DITHER_DOWN_EN (1u << 16)
#define VOP_VP_DSP_CTRL_DITHER_DOWN_EN     (1u << 17)
#define VOP_VP_DSP_CTRL_POST_DSP_OUT_R601  (1u << 21)
#define VOP_VP_DSP_CTRL_DSP_LUT_EN         (1u << 28)
#define VOP_VP_DSP_CTRL_STANDBY            (1u << 31)

/* VP DSP_CTRL out_mode values (pixel format on the display interface) */

#define VOP_VP_OUT_MODE_P888   0x0 /* 24-bit RGB parallel    */
#define VOP_VP_OUT_MODE_P666   0x1
#define VOP_VP_OUT_MODE_P565   0x2
#define VOP_VP_OUT_MODE_YUV420 0xe
#define VOP_VP_OUT_MODE_AAAA   0xf /* 30-bit / HDMI RGB      */

/* VP interrupt bits (INT_EN / INT_CLR / INT_STATUS share the layout).
 * The enable register is hiword-masked: the upper 16 bits arm the write.
 */

#define VOP_VP_INT_FS_FIELD       (1u << 0) /* Frame start      */
#define VOP_VP_INT_FS             (1u << 1)
#define VOP_VP_INT_LINE_FLAG0     (1u << 2)
#define VOP_VP_INT_LINE_FLAG1     (1u << 3)
#define VOP_VP_INT_POST_BUF_EMPTY (1u << 4)
#define VOP_VP_INT_DSP_HOLD_VALID (1u << 5)
#define VOP_VP_INT_FS_NEW         (1u << 6)

/* The VP interrupt enable register is hiword-masked: the upper 16 bits
 * are a per-bit write-enable for the lower 16 bits.  INT_CLR follows the
 * same scheme, while INT_STATUS / INT_RAW_STATUS are plain read-only.
 */

#define VOP_INT_HIWORD(bits)     (((uint32_t)(bits) << 16) | (uint32_t)(bits))
#define VOP_INT_HIWORD_CLR(bits) ((uint32_t)(bits) << 16)

/* Timing register packing helpers.  Every timing register packs two
 * 16-bit values: the "start"/"total" field in bits [31:16] and the
 * "end" field in bits [15:0].
 */

#define VOP_TIMING_PACK(hi, lo) \
  ((((uint32_t)(hi)&0xffffu) << 16) | ((uint32_t)(lo)&0xffffu))

/* Esmart window (offsets relative to RK3576_VOP_ESMART(n)) ***************
 *
 * An Esmart window has four "regions"; only region 0 is used here, which
 * makes the window behave like a plain single-rectangle plane.
 */

#define RK3576_VOP_ESMART_CTRL0                   0x0000
#define RK3576_VOP_ESMART_CTRL1                   0x0004
#define RK3576_VOP_ESMART_AXI_CTRL                0x0008
#define RK3576_VOP_ESMART_REGION0_CTRL            0x0010
#define RK3576_VOP_ESMART_REGION0_YRGB_MST        0x0014
#define RK3576_VOP_ESMART_REGION0_CBR_MST         0x0018
#define RK3576_VOP_ESMART_REGION0_VIR             0x001c
#define RK3576_VOP_ESMART_REGION0_ACT_INFO        0x0020
#define RK3576_VOP_ESMART_REGION0_DSP_INFO        0x0024
#define RK3576_VOP_ESMART_REGION0_DSP_ST          0x0028
#define RK3576_VOP_ESMART_REGION0_SCL_CTRL        0x002c
#define RK3576_VOP_ESMART_REGION0_SCL_FACTOR_YRGB 0x0030
#define RK3576_VOP_ESMART_REGION0_SCL_FACTOR_CBR  0x0034
#define RK3576_VOP_ESMART_REGION0_SCL_OFFSET      0x0038

/* Esmart CTRL0 */

#define VOP_ESMART_CTRL0_RGB2YUV_EN     (1u << 1)
#define VOP_ESMART_CTRL0_CSC_MODE_SHIFT 2
#define VOP_ESMART_CTRL0_CSC_MODE_MASK  (3u << 2)
#define VOP_ESMART_CTRL0_YUV_CLIP       (1u << 5)
#define VOP_ESMART_CTRL0_XMIRROR_EN     (1u << 22)
#define VOP_ESMART_CTRL0_YMIRROR_EN     (1u << 23)

/* Esmart CTRL1 - AXI outstanding / byte-swap knobs, left at reset. */

#define VOP_ESMART_CTRL1_AXI_YRGB_ID_SHIFT 4
#define VOP_ESMART_CTRL1_AXI_UV_ID_SHIFT   12

/* Esmart REGION0_CTRL */

#define VOP_ESMART_REGION_CTRL_WIN_EN             (1u << 0)
#define VOP_ESMART_REGION_CTRL_FMT_SHIFT          1
#define VOP_ESMART_REGION_CTRL_FMT_MASK           (0x1fu << 1)
#define VOP_ESMART_REGION_CTRL_FMT(f)             (((uint32_t)(f)&0x1fu) << 1)
#define VOP_ESMART_REGION_CTRL_RB_SWAP            (1u << 14)
#define VOP_ESMART_REGION_CTRL_UV_SWAP            (1u << 16)
#define VOP_ESMART_REGION_CTRL_GLOBAL_ALPHA_SHIFT 24
#define VOP_ESMART_REGION_CTRL_GLOBAL_ALPHA_MASK  (0xffu << 24)
#define VOP_ESMART_REGION_CTRL_GLOBAL_ALPHA(a)    (((uint32_t)(a)&0xffu) << 24)

/* Window pixel format codes (VOP2 family win_format field) */

#define VOP_WIN_FMT_ARGB8888 0
#define VOP_WIN_FMT_ABGR8888 1
#define VOP_WIN_FMT_RGB888   2
#define VOP_WIN_FMT_BGR888   3
#define VOP_WIN_FMT_RGB565   4
#define VOP_WIN_FMT_BGR565   5
#define VOP_WIN_FMT_YUV420SP 16
#define VOP_WIN_FMT_YUV422SP 18
#define VOP_WIN_FMT_YUV444SP 20

/* Esmart REGION0_VIR: virtual stride in 32-bit words (bits [15:0]) */

#define VOP_ESMART_VIR_STRIDE_MASK 0x0000ffffu
#define VOP_ESMART_VIR_STRIDE(words) \
  ((uint32_t)(words)&VOP_ESMART_VIR_STRIDE_MASK)

/* ACT_INFO / DSP_INFO pack (height-1)<<16 | (width-1); DSP_ST packs the
 * destination position as y<<16 | x, both relative to the active area.
 */

#define VOP_WIN_SIZE_PACK(w, h) \
  ((((uint32_t)(h)-1u) << 16) | (((uint32_t)(w)-1u) & 0xffffu))
#define VOP_WIN_POS_PACK(x, y) \
  ((((uint32_t)(y)&0xffffu) << 16) | ((uint32_t)(x)&0xffffu))

/* Esmart REGION0_SCL_CTRL: bypass (1:1) scaling */

#define VOP_ESMART_SCL_CTRL_BYPASS 0x00000000u

/* VOP IOMMU (rockchip,iommu-v2) ******************************************/

#define RK3576_VOP_MMU_DTE_ADDR        0x0000
#define RK3576_VOP_MMU_STATUS          0x0004
#define RK3576_VOP_MMU_COMMAND         0x0008
#define RK3576_VOP_MMU_PAGE_FAULT_ADDR 0x000c
#define RK3576_VOP_MMU_INT_MASK        0x001c
#define RK3576_VOP_MMU_INT_STATUS      0x0020

#define VOP_MMU_STATUS_PAGING_ENABLED  (1u << 0)

#define VOP_MMU_CMD_ENABLE_PAGING      0
#define VOP_MMU_CMD_DISABLE_PAGING     1
#define VOP_MMU_CMD_ZAP_CACHE          3

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VOP_H */
