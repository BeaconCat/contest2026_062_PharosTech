/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_rga.h
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
 * Rockchip RGA2 (Raster Graphic Acceleration unit, version 2) register
 * definitions for the RK3576.  The SoC integrates two identical RGA2
 * cores:
 *
 *   RGA2 core0 (0x27920000, 4KB window, GIC INTID 360)
 *   RGA2 core1 (0x27930000, 4KB window, GIC INTID 361)
 *
 * Each core is driven in "command list" mode:  the CPU builds a block of
 * 32 consecutive 32-bit words in physically contiguous memory, publishes
 * the physical address of that block through RGA_CMD_BASE and kicks the
 * core through RGA_CMD_CTRL.  The word layout of the command list is the
 * very same layout as the memory-mapped mode register file that starts at
 * offset RGA2_MODE_REG_BASE, so a command word index is simply
 * (mode register offset - RGA2_MODE_REG_BASE) / 4.
 *
 * Reference: Rockchip RK3576 TRM, chapter "RGA2".  Bit positions that
 * could not be cross-checked against the TRM text are flagged TODO below
 * and were derived from the publicly documented RGA2 programming model
 * (identical IP on RK3399/RK3568/RK3588).
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RGA_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RGA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller instances *****************************************************/

#define RK3576_RGA_NCORES 2 /* Two identical RGA2 cores */

/* Global (non command list) registers, relative to the core base **********/

#define RK3576_RGA_SYS_CTRL_OFFSET   0x0000 /* System control            */
#define RK3576_RGA_CMD_CTRL_OFFSET   0x0004 /* Command list control      */
#define RK3576_RGA_CMD_BASE_OFFSET   0x0008 /* Command list phys address */
#define RK3576_RGA_STATUS_OFFSET     0x000c /* Core status               */
#define RK3576_RGA_INT_OFFSET        0x0010 /* Interrupt status/control  */
#define RK3576_RGA_MMU_CTRL0_OFFSET  0x0014 /* Internal MMU control      */
#define RK3576_RGA_MMU_CMD_BASE_OFFS 0x0018 /* MMU base of command list  */
#define RK3576_RGA_VERSION_OFFSET    0x0028 /* IP version (read-only)    */

/* RGA_SYS_CTRL bit fields **************************************************/

#define RK3576_RGA_SYS_CTRL_CMD_MODE     (1 << 0) /* 0: reg mode 1: list */
#define RK3576_RGA_SYS_CTRL_OP_ST_MODE   (1 << 1) /* Start-of-op mode    */
#define RK3576_RGA_SYS_CTRL_AUTO_CKG     (1 << 2) /* Auto clock gating   */
#define RK3576_RGA_SYS_CTRL_SOFT_RESET   (1 << 3) /* Self-clearing reset */
#define RK3576_RGA_SYS_CTRL_CCLK_SRESET  (1 << 4) /* Core clock reset    */
#define RK3576_RGA_SYS_CTRL_ACLK_SRESET  (1 << 6) /* AXI clock reset     */

/* Reset is complete once bit0 of SYS_CTRL reads back as zero. */

#define RK3576_RGA_SYS_CTRL_RST_BUSY     (1 << 0)

/* RGA_CMD_CTRL bit fields **************************************************/

#define RK3576_RGA_CMD_CTRL_START        (1 << 0) /* Kick the command list */
#define RK3576_RGA_CMD_CTRL_INCR_VALID   (1 << 1) /* Append to running list */
#define RK3576_RGA_CMD_CTRL_LINE_ST_EN   (1 << 2) /* Line-mode start       */
#define RK3576_RGA_CMD_CTRL_NR_SHIFT     3        /* [12:3] command count  */
#define RK3576_RGA_CMD_CTRL_NR_MASK      (0x3ff << RK3576_RGA_CMD_CTRL_NR_SHIFT)

/* RGA_STATUS bit fields ****************************************************/

#define RK3576_RGA_STATUS_RENDER_ACTIVE  (1 << 0) /* Rendering in progress */
#define RK3576_RGA_STATUS_CMD_ACTIVE     (1 << 1) /* Command fetch active  */
#define RK3576_RGA_STATUS_MMU_ACTIVE     (1 << 2) /* MMU walk active       */

/* RGA_INT bit fields.  The raw status bits are in the low nibble, the
 * matching write-1-to-clear bits sit at [10:8].  TODO: cross-check the
 * enable bits [6:4] against the RK3576 TRM.
 */

#define RK3576_RGA_INT_RAW_ERROR         (1 << 0) /* Bus/config error      */
#define RK3576_RGA_INT_RAW_DONE          (1 << 1) /* Command list finished */
#define RK3576_RGA_INT_RAW_MMU_ERROR     (1 << 2) /* MMU page fault        */
#define RK3576_RGA_INT_EN_ERROR          (1 << 4)
#define RK3576_RGA_INT_EN_DONE           (1 << 5)
#define RK3576_RGA_INT_EN_MMU_ERROR      (1 << 6)
#define RK3576_RGA_INT_CLR_ERROR         (1 << 8)
#define RK3576_RGA_INT_CLR_DONE          (1 << 9)
#define RK3576_RGA_INT_CLR_MMU_ERROR     (1 << 10)

#define RK3576_RGA_INT_RAW_ALL \
  (RK3576_RGA_INT_RAW_ERROR | RK3576_RGA_INT_RAW_DONE | \
   RK3576_RGA_INT_RAW_MMU_ERROR)

#define RK3576_RGA_INT_EN_ALL \
  (RK3576_RGA_INT_EN_ERROR | RK3576_RGA_INT_EN_DONE | \
   RK3576_RGA_INT_EN_MMU_ERROR)

#define RK3576_RGA_INT_CLR_ALL \
  (RK3576_RGA_INT_CLR_ERROR | RK3576_RGA_INT_CLR_DONE | \
   RK3576_RGA_INT_CLR_MMU_ERROR)

/* RGA_MMU_CTRL0 bit fields *************************************************/

#define RK3576_RGA_MMU_CTRL0_ENABLE      (1 << 0) /* Enable internal MMU   */

/* Command list layout ******************************************************/

/* The mode register file is mirrored at this offset inside the 4KB
 * register window.  Command list word N corresponds to
 * RGA2_MODE_REG_BASE + 4 * N.
 */

#define RK3576_RGA_MODE_REG_BASE  0x0100

/* Command list word indices (see the layout note at the top). */

#define RK3576_RGA_CMD_MODE_CTRL          0  /* 0x0100 */
#define RK3576_RGA_CMD_SRC_INFO           1  /* 0x0104 */
#define RK3576_RGA_CMD_SRC_Y_RGB_BASE     2  /* 0x0108 */
#define RK3576_RGA_CMD_SRC_CB_BASE        3  /* 0x010c */
#define RK3576_RGA_CMD_SRC_CR_BASE        4  /* 0x0110 */
#define RK3576_RGA_CMD_SRC1_RGB_BASE      5  /* 0x0114 */
#define RK3576_RGA_CMD_SRC_VIR_INFO       6  /* 0x0118 */
#define RK3576_RGA_CMD_SRC_ACT_INFO       7  /* 0x011c */
#define RK3576_RGA_CMD_SRC_X_FACTOR       8  /* 0x0120 */
#define RK3576_RGA_CMD_SRC_Y_FACTOR       9  /* 0x0124 */
#define RK3576_RGA_CMD_SRC_BG_COLOR       10 /* 0x0128 */
#define RK3576_RGA_CMD_SRC_FG_COLOR       11 /* 0x012c */
#define RK3576_RGA_CMD_SRC_TR_COLOR0      12 /* 0x0130 */
#define RK3576_RGA_CMD_SRC_TR_COLOR1      13 /* 0x0134 */
#define RK3576_RGA_CMD_DST_INFO           14 /* 0x0138 */
#define RK3576_RGA_CMD_DST_Y_RGB_BASE     15 /* 0x013c */
#define RK3576_RGA_CMD_DST_CB_BASE        16 /* 0x0140 */
#define RK3576_RGA_CMD_DST_CR_BASE        17 /* 0x0144 */
#define RK3576_RGA_CMD_DST_VIR_INFO       18 /* 0x0148 */
#define RK3576_RGA_CMD_DST_ACT_INFO       19 /* 0x014c */
#define RK3576_RGA_CMD_ALPHA_CTRL0        20 /* 0x0150 */
#define RK3576_RGA_CMD_ALPHA_CTRL1        21 /* 0x0154 */
#define RK3576_RGA_CMD_FADING_CTRL        22 /* 0x0158 */
#define RK3576_RGA_CMD_PAT_CON            23 /* 0x015c */
#define RK3576_RGA_CMD_ROP_CTRL0          24 /* 0x0160 */
#define RK3576_RGA_CMD_ROP_CTRL1          25 /* 0x0164 */
#define RK3576_RGA_CMD_MASK_BASE          26 /* 0x0168 */
#define RK3576_RGA_CMD_MMU_CTRL1          27 /* 0x016c */
#define RK3576_RGA_CMD_MMU_SRC_BASE       28 /* 0x0170 */
#define RK3576_RGA_CMD_MMU_SRC1_BASE      29 /* 0x0174 */
#define RK3576_RGA_CMD_MMU_DST_BASE       30 /* 0x0178 */
#define RK3576_RGA_CMD_MMU_ELS_BASE       31 /* 0x017c */

#define RK3576_RGA_CMD_NWORDS             32

/* The core fetches the command list in 16-byte bursts, so the buffer must
 * be at least 16-byte aligned.  rk3576_dma_alloc() returns 64-byte aligned
 * memory which satisfies this with room to spare.
 */

#define RK3576_RGA_CMD_ALIGN              16

/* MODE_CTRL (command word 0) bit fields ************************************/

#define RK3576_RGA_MODE_RENDER_SHIFT      0
#define RK3576_RGA_MODE_RENDER_MASK       (0x7 << RK3576_RGA_MODE_RENDER_SHIFT)
#define RK3576_RGA_MODE_RENDER_BITBLT     0 /* Copy / scale / rotate      */
#define RK3576_RGA_MODE_RENDER_PALETTE    1 /* Colour palette            */
#define RK3576_RGA_MODE_RENDER_FILL       2 /* Rectangle colour fill     */
#define RK3576_RGA_MODE_RENDER_UPD_PAL    3 /* Update palette table      */
#define RK3576_RGA_MODE_RENDER_UPD_PAT    4 /* Update pattern buffer     */

#define RK3576_RGA_MODE_BITBLT_SHIFT      3 /* 0: one source 1: two src  */
#define RK3576_RGA_MODE_BITBLT_MASK       (0x1 << RK3576_RGA_MODE_BITBLT_SHIFT)
#define RK3576_RGA_MODE_BITBLT_1SRC       0
#define RK3576_RGA_MODE_BITBLT_2SRC       1

#define RK3576_RGA_MODE_CF_ROP4_PAT       (1 << 4) /* ROP4 pattern      */
#define RK3576_RGA_MODE_ALPHA_ZERO_KEY    (1 << 5) /* Zero alpha as key */
#define RK3576_RGA_MODE_GRADIENT_SAT      (1 << 6) /* Saturate gradient */
#define RK3576_RGA_MODE_INTR_CFG_SHIFT    7        /* [8:7] interrupt   */
#define RK3576_RGA_MODE_INTR_CFG_MASK     (0x3 << RK3576_RGA_MODE_INTR_CFG_SHIFT)

/* SRC_INFO (command word 1) bit fields.
 * TODO: [17:14] scaling-mode field positions are derived from the public
 * RGA2 programming model; re-check against the RK3576 TRM.
 */

#define RK3576_RGA_SRC_FMT_SHIFT          0
#define RK3576_RGA_SRC_FMT_MASK           (0xf << RK3576_RGA_SRC_FMT_SHIFT)
#define RK3576_RGA_SRC_RB_SWAP            (1 << 4)
#define RK3576_RGA_SRC_ALPHA_SWAP         (1 << 5)
#define RK3576_RGA_SRC_UV_SWAP            (1 << 6)
#define RK3576_RGA_SRC_CSC_SHIFT          8  /* [9:8]   YUV<->RGB matrix */
#define RK3576_RGA_SRC_CSC_MASK           (0x3 << RK3576_RGA_SRC_CSC_SHIFT)
#define RK3576_RGA_SRC_ROT_SHIFT          10 /* [11:10] rotation         */
#define RK3576_RGA_SRC_ROT_MASK           (0x3 << RK3576_RGA_SRC_ROT_SHIFT)
#define RK3576_RGA_SRC_MIRROR_X           (1 << 12)
#define RK3576_RGA_SRC_MIRROR_Y           (1 << 13)
#define RK3576_RGA_SRC_HSCL_SHIFT         14 /* [15:14] horizontal scale */
#define RK3576_RGA_SRC_HSCL_MASK          (0x3 << RK3576_RGA_SRC_HSCL_SHIFT)
#define RK3576_RGA_SRC_VSCL_SHIFT         16 /* [17:16] vertical scale   */
#define RK3576_RGA_SRC_VSCL_MASK          (0x3 << RK3576_RGA_SRC_VSCL_SHIFT)
#define RK3576_RGA_SRC_TRANS_ENABLE       (1 << 18)

/* Scaling mode values shared by the HSCL/VSCL fields. */

#define RK3576_RGA_SCL_OFF                0 /* 1:1, no resampling       */
#define RK3576_RGA_SCL_UP                 1 /* Bicubic up-scale         */
#define RK3576_RGA_SCL_DOWN_AVG           2 /* Averaging down-scale     */
#define RK3576_RGA_SCL_DOWN_BIC           3 /* Bicubic down-scale       */

/* Rotation field values (source read order). */

#define RK3576_RGA_ROT_0                  0
#define RK3576_RGA_ROT_90                 1
#define RK3576_RGA_ROT_180                2
#define RK3576_RGA_ROT_270                3

/* Colour-space conversion matrix selection (used by both SRC and DST). */

#define RK3576_RGA_CSC_BYPASS             0
#define RK3576_RGA_CSC_BT601_LIMIT        1
#define RK3576_RGA_CSC_BT601_FULL         2
#define RK3576_RGA_CSC_BT709_LIMIT        3

/* DST_INFO (command word 14) bit fields ************************************/

#define RK3576_RGA_DST_FMT_SHIFT          0
#define RK3576_RGA_DST_FMT_MASK           (0xf << RK3576_RGA_DST_FMT_SHIFT)
#define RK3576_RGA_DST_RB_SWAP            (1 << 4)
#define RK3576_RGA_DST_ALPHA_SWAP         (1 << 5)
#define RK3576_RGA_DST_UV_SWAP            (1 << 6)
#define RK3576_RGA_DST_CSC_SHIFT          8 /* [9:8] RGB->YUV matrix */
#define RK3576_RGA_DST_CSC_MASK           (0x3 << RK3576_RGA_DST_CSC_SHIFT)
#define RK3576_RGA_DST_DITHER_UP_EN       (1 << 10)
#define RK3576_RGA_DST_DITHER_DOWN_EN     (1 << 11)
#define RK3576_RGA_DST_DITHER_MODE_SHIFT  12
#define RK3576_RGA_DST_DITHER_MODE_MASK   (0x3 << RK3576_RGA_DST_DITHER_MODE_SHIFT)

/* VIR_INFO: stride expressed in 32-bit words ******************************/

#define RK3576_RGA_SRC_VIR_STRIDE_SHIFT   0
#define RK3576_RGA_SRC_VIR_STRIDE_MASK    (0xffff << 0)
#define RK3576_RGA_SRC1_VIR_STRIDE_SHIFT  16
#define RK3576_RGA_SRC1_VIR_STRIDE_MASK   (0x7ff << 16)
#define RK3576_RGA_DST_VIR_STRIDE_SHIFT   0
#define RK3576_RGA_DST_VIR_STRIDE_MASK    (0xffff << 0)

/* ACT_INFO: (width - 1) and (height - 1) in pixels ************************/

#define RK3576_RGA_ACT_WIDTH_SHIFT        0
#define RK3576_RGA_ACT_WIDTH_MASK         (0x1fff << 0)
#define RK3576_RGA_ACT_HEIGHT_SHIFT       16
#define RK3576_RGA_ACT_HEIGHT_MASK        (0x1fff << 16)

/* Scale factors are unsigned 16.16 style fractions in the low 16 bits of
 * SRC_X_FACTOR / SRC_Y_FACTOR.
 */

#define RK3576_RGA_FACTOR_SHIFT           0
#define RK3576_RGA_FACTOR_MASK            (0xffff << 0)
#define RK3576_RGA_FACTOR_ONE             0x10000 /* 1.0 in 16.16 */

/* ALPHA_CTRL0 (command word 20) bit fields ********************************/

#define RK3576_RGA_ALPHA_ROP_EN           (1 << 0) /* Enable alpha/ROP path */
#define RK3576_RGA_ALPHA_ROP_SEL          (1 << 1) /* 0: alpha 1: ROP       */
#define RK3576_RGA_ALPHA_ROP_MODE_SHIFT   2        /* [3:2] ROP2/3/4        */
#define RK3576_RGA_ALPHA_ROP_MODE_MASK    (0x3 << RK3576_RGA_ALPHA_ROP_MODE_SHIFT)
#define RK3576_RGA_ALPHA_GLOBAL_SHIFT     4        /* [11:4] global alpha   */
#define RK3576_RGA_ALPHA_GLOBAL_MASK      (0xff << RK3576_RGA_ALPHA_GLOBAL_SHIFT)

/* ROP mode selection. */

#define RK3576_RGA_ROP_MODE_2             0
#define RK3576_RGA_ROP_MODE_3             1
#define RK3576_RGA_ROP_MODE_4             2

/* ALPHA_CTRL1 (command word 21): per-channel blend factor selection.  The
 * register is split into a source half [15:0] and a destination half
 * [31:16], each holding {colour factor, alpha factor, mode} triples.
 * TODO: verify the sub-field widths against the RK3576 TRM; the presets
 * below cover the two blend equations the driver exposes.
 */

#define RK3576_RGA_ALPHA_CTRL1_SRC_SHIFT  0
#define RK3576_RGA_ALPHA_CTRL1_DST_SHIFT  16

/* Straight (non-premultiplied) SRC-OVER: Cd = Cs*As + Cd*(1-As). */

#define RK3576_RGA_BLEND_SRC_OVER         0x0a0a0a0a

/* Premultiplied SRC-OVER: Cd = Cs + Cd*(1-As). */

#define RK3576_RGA_BLEND_SRC_OVER_PREMUL  0x0a080a08

/* ROP_CTRL0 (command word 24): 8-bit raster operation code in [7:0]. */

#define RK3576_RGA_ROP_CODE_SHIFT         0
#define RK3576_RGA_ROP_CODE_MASK          (0xff << 0)
#define RK3576_RGA_ROP_CODE_COPY          0xcc /* dst = src            */
#define RK3576_RGA_ROP_CODE_AND           0x88 /* dst = src & dst      */
#define RK3576_RGA_ROP_CODE_OR            0xee /* dst = src | dst      */
#define RK3576_RGA_ROP_CODE_XOR           0x66 /* dst = src ^ dst      */
#define RK3576_RGA_ROP_CODE_NOT_SRC       0x33 /* dst = ~src           */

/* Pixel format codes shared by SRC_INFO and DST_INFO *********************/

#define RK3576_RGA_FMT_RGBA8888           0x0
#define RK3576_RGA_FMT_RGBX8888           0x1
#define RK3576_RGA_FMT_RGB888             0x2
#define RK3576_RGA_FMT_BGRA8888           0x3
#define RK3576_RGA_FMT_RGB565             0x4
#define RK3576_RGA_FMT_RGBA5551           0x5
#define RK3576_RGA_FMT_RGBA4444           0x6
#define RK3576_RGA_FMT_BGR888             0x7
#define RK3576_RGA_FMT_YUV422SP           0x8
#define RK3576_RGA_FMT_YUV422P            0x9
#define RK3576_RGA_FMT_YUV420SP           0xa
#define RK3576_RGA_FMT_YUV420P            0xb
#define RK3576_RGA_FMT_YUYV422            0xc
#define RK3576_RGA_FMT_YUYV420            0xd
#define RK3576_RGA_FMT_UYVY422            0xe
#define RK3576_RGA_FMT_UYVY420            0xf

/* Hardware limits *********************************************************/

#define RK3576_RGA_MAX_WIDTH              8192
#define RK3576_RGA_MAX_HEIGHT             8192
#define RK3576_RGA_MAX_SCALE_UP           16 /* Max up-scaling ratio   */
#define RK3576_RGA_MAX_SCALE_DOWN         16 /* Max down-scaling ratio */

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_RGA_H */
