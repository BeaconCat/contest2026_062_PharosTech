/****************************************************************************
 * chips/rk3576/hardware/rk3576_isp.h
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
 * RK3576 camera capture register definitions.
 *
 * Two blocks are described here because they belong to the same capture
 * pipeline and are always brought up together:
 *
 *   VICAP / CIF  0x27c10000 (0x800 bytes)  "rockchip,rk3576-cif"
 *                Raw capture DMA: takes a parallel (DVP) or CSI-2 virtual
 *                channel stream and writes it straight to memory using a
 *                two-frame ping-pong address pair.  This is the "bypass"
 *                path used when no ISP processing is required.
 *
 *   ISP          0x27c00000 (0x7f00 bytes) "rockchip,rk3576-rkisp"
 *                Bayer pipeline: black level subtraction, demosaic, white
 *                balance, gamma, YUV conversion, and a memory interface
 *                (MI) that writes the main picture path to DRAM.
 *
 * The VICAP register offsets are those given by the RK3576 TRM VICAP
 * chapter.  The ISP register offsets follow the Rockchip ISP register
 * layout inherited from the Marvin/ISP core (identical block base offsets
 * across rkisp generations); the individual tuning-block offsets are
 * marked TODO where they have not yet been cross-checked against the
 * RK3576 TRM on real silicon.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_ISP_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_ISP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Block base addresses ****************************************************/

#define RK3576_ISP_ADDR       0x27c00000 /* rkisp core                     */
#define RK3576_ISP_SIZE       0x00007f00
#define RK3576_ISP_MMU_ADDR   0x27c07f00 /* rockchip,iommu-v2 for the ISP  */
#define RK3576_ISP_MMU_SIZE   0x00000100

#define RK3576_VICAP_ADDR     0x27c10000 /* rkcif (VICAP)                  */
#define RK3576_VICAP_SIZE     0x00000800
#define RK3576_VICAP_MMU_ADDR 0x27c10800
#define RK3576_VICAP_MMU_SIZE 0x00000100

#define RK3576_VPSS_ADDR      0x27c30000 /* rkvpss (post scaler, unused)   */
#define RK3576_VPSS_SIZE      0x00003f00
#define RK3576_VPSS_MMU_ADDR  0x27c33f00
#define RK3576_VPSS_MMU_SIZE  0x00000100

/* VICAP / CIF register offsets ********************************************/

#define RK3576_CIF_CTRL_OFFSET           0x0000 /* Capture control          */
#define RK3576_CIF_INTEN_OFFSET          0x0004 /* Interrupt enable         */
#define RK3576_CIF_INTSTAT_OFFSET        0x0008 /* Interrupt status (W1C)   */
#define RK3576_CIF_FOR_OFFSET            0x000c /* Input/output format      */
#define RK3576_CIF_MULTI_ID_OFFSET       0x0010 /* Virtual channel / DT ID  */
#define RK3576_CIF_FRM0_ADDR_Y_OFFSET    0x0020 /* Ping buffer Y / RAW base */
#define RK3576_CIF_FRM0_ADDR_UV_OFFSET   0x0024 /* Ping buffer chroma base  */
#define RK3576_CIF_FRM1_ADDR_Y_OFFSET    0x0028 /* Pong buffer Y / RAW base */
#define RK3576_CIF_FRM1_ADDR_UV_OFFSET   0x002c /* Pong buffer chroma base  */
#define RK3576_CIF_VIR_LINE_WIDTH_OFFSET 0x0030 /* Line stride, in pixels */
#define RK3576_CIF_SET_SIZE_OFFSET       0x0034 /* Capture width / height   */
#define RK3576_CIF_SCL_CTRL_OFFSET       0x0040 /* Down-scaler control      */

/* CIF_CTRL bits */

#define RK3576_CIF_CTRL_ENABLE_CAPTURE  (1 << 0) /* Start capture         */
#define RK3576_CIF_CTRL_MODE_SHIFT      (1)
#define RK3576_CIF_CTRL_MODE_MASK       (3 << RK3576_CIF_CTRL_MODE_SHIFT)
#define RK3576_CIF_CTRL_MODE_ONEFRAME   (0 << RK3576_CIF_CTRL_MODE_SHIFT)
#define RK3576_CIF_CTRL_MODE_PINGPONG   (1 << RK3576_CIF_CTRL_MODE_SHIFT)
#define RK3576_CIF_CTRL_MODE_LINELOOP   (2 << RK3576_CIF_CTRL_MODE_SHIFT)
#define RK3576_CIF_CTRL_SP_ENABLE       (1 << 4) /* Self path enable      */
#define RK3576_CIF_CTRL_MIPI_MODE       (1 << 5) /* CSI-2 source (vs DVP) */
#define RK3576_CIF_CTRL_AXI_BURST_SHIFT (12)
#define RK3576_CIF_CTRL_AXI_BURST_MASK  (0xf << RK3576_CIF_CTRL_AXI_BURST_SHIFT)
#define RK3576_CIF_CTRL_AXI_BURST_16    (0xf << RK3576_CIF_CTRL_AXI_BURST_SHIFT)

/* CIF_INTEN / CIF_INTSTAT bits.  INTSTAT is write-1-to-clear. */

#define RK3576_CIF_INT_FRAME_END         (1 << 0) /* Frame written out     */
#define RK3576_CIF_INT_LINE_ERR          (1 << 1) /* Line length mismatch  */
#define RK3576_CIF_INT_PST_INF_FRAME_END (1 << 9) /* Parallel-stream end   */
#define RK3576_CIF_INT_BUS_ERR           (1 << 6) /* AXI write error       */
#define RK3576_CIF_INT_FRAME_ID_SHIFT    (16)     /* HW ping-pong index    */
#define RK3576_CIF_INT_FRAME_ID_MASK     (3 << RK3576_CIF_INT_FRAME_ID_SHIFT)

/* CIF_FOR bits: input timing polarity, input mode, output mode */

#define RK3576_CIF_FOR_VSY_HIGH_ACTIVE   (1 << 0)
#define RK3576_CIF_FOR_HSY_LOW_ACTIVE    (1 << 1)
#define RK3576_CIF_FOR_INPUT_MODE_SHIFT  (2)
#define RK3576_CIF_FOR_INPUT_MODE_MASK   (7 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_YUV    (0 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_PAL    (2 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_NTSC   (3 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_RAW    (4 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_JPEG   (5 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_INPUT_MODE_BT1120 (6 << RK3576_CIF_FOR_INPUT_MODE_SHIFT)
#define RK3576_CIF_FOR_YUV_ORDER_SHIFT   (5)
#define RK3576_CIF_FOR_YUV_ORDER_MASK    (3 << RK3576_CIF_FOR_YUV_ORDER_SHIFT)
#define RK3576_CIF_FOR_YUV_ORDER_UYVY    (0 << RK3576_CIF_FOR_YUV_ORDER_SHIFT)
#define RK3576_CIF_FOR_YUV_ORDER_YVYU    (1 << RK3576_CIF_FOR_YUV_ORDER_SHIFT)
#define RK3576_CIF_FOR_YUV_ORDER_VYUY    (2 << RK3576_CIF_FOR_YUV_ORDER_SHIFT)
#define RK3576_CIF_FOR_YUV_ORDER_YUYV    (3 << RK3576_CIF_FOR_YUV_ORDER_SHIFT)
#define RK3576_CIF_FOR_YUV_INPUT_420     (1 << 7) /* 0: 4:2:2 input        */
#define RK3576_CIF_FOR_INPUT_420_ODD     (1 << 8)
#define RK3576_CIF_FOR_CCIR_ORDER_EVEN   (1 << 9)
#define RK3576_CIF_FOR_RAW_WIDTH_SHIFT   (11)
#define RK3576_CIF_FOR_RAW_WIDTH_MASK    (3 << RK3576_CIF_FOR_RAW_WIDTH_SHIFT)
#define RK3576_CIF_FOR_RAW_WIDTH_8       (0 << RK3576_CIF_FOR_RAW_WIDTH_SHIFT)
#define RK3576_CIF_FOR_RAW_WIDTH_10      (1 << RK3576_CIF_FOR_RAW_WIDTH_SHIFT)
#define RK3576_CIF_FOR_RAW_WIDTH_12      (2 << RK3576_CIF_FOR_RAW_WIDTH_SHIFT)
#define RK3576_CIF_FOR_YUV_OUTPUT_420    (1 << 16) /* 0: 4:2:2 output       */
#define RK3576_CIF_FOR_OUTPUT_420_ODD    (1 << 17)
#define RK3576_CIF_FOR_UV_ORDER_VUVU     (1 << 18) /* 0: UVUV (NV12/NV16)   */

/* CIF_MULTI_ID: CSI-2 virtual channel / data type selector for stream 0 */

#define RK3576_CIF_MULTI_ID_VC_SHIFT (0)
#define RK3576_CIF_MULTI_ID_VC_MASK  (3 << RK3576_CIF_MULTI_ID_VC_SHIFT)
#define RK3576_CIF_MULTI_ID_DT_SHIFT (8)
#define RK3576_CIF_MULTI_ID_DT_MASK  (0x3f << RK3576_CIF_MULTI_ID_DT_SHIFT)
#define RK3576_CIF_MULTI_ID_EN       (1 << 15) /* Enable VC/DT filter   */

/* CIF_SET_SIZE: width in [15:0], height in [31:16] */

#define RK3576_CIF_SET_SIZE(w, h) (((w)&0xffff) | (((h)&0xffff) << 16))

/* CIF_SCL_CTRL: the down-scaler is bypassed by this driver */

#define RK3576_CIF_SCL_CTRL_ENABLE (1 << 0)

/* ISP register offsets ****************************************************/

/* Core acquisition / output window and top-level interrupts.  These
 * offsets are stable across all Rockchip ISP generations.
 */

#define RK3576_ISP_CTRL_OFFSET       0x0000 /* ISP enable / mode           */
#define RK3576_ISP_ACQ_PROP_OFFSET   0x0004 /* Input timing + Bayer phase  */
#define RK3576_ISP_ACQ_H_OFFS_OFFSET 0x0008
#define RK3576_ISP_ACQ_V_OFFS_OFFSET 0x000c
#define RK3576_ISP_ACQ_H_SIZE_OFFSET 0x0010
#define RK3576_ISP_ACQ_V_SIZE_OFFSET 0x0014
#define RK3576_ISP_ACQ_NR_FRAMES_OFF 0x0018 /* 0 = free running            */
#define RK3576_ISP_OUT_H_OFFS_OFFSET 0x0030
#define RK3576_ISP_OUT_V_OFFS_OFFSET 0x0034
#define RK3576_ISP_OUT_H_SIZE_OFFSET 0x0038
#define RK3576_ISP_OUT_V_SIZE_OFFSET 0x003c
#define RK3576_ISP_IMSC_OFFSET       0x0044 /* Interrupt mask              */
#define RK3576_ISP_RIS_OFFSET        0x0048 /* Raw interrupt status        */
#define RK3576_ISP_MIS_OFFSET        0x004c /* Masked interrupt status     */
#define RK3576_ISP_ICR_OFFSET        0x0050 /* Interrupt clear (W1C)       */
#define RK3576_ISP_ISR_OFFSET        0x0054 /* Interrupt set               */

/* ISP_CTRL bits */

#define RK3576_ISP_CTRL_ENABLE        (1 << 0) /* isp_enable               */
#define RK3576_ISP_CTRL_MODE_SHIFT    (1)
#define RK3576_ISP_CTRL_MODE_MASK     (0xf << RK3576_ISP_CTRL_MODE_SHIFT)
#define RK3576_ISP_CTRL_MODE_RAW      (0xa << RK3576_ISP_CTRL_MODE_SHIFT)
#define RK3576_ISP_CTRL_MODE_ITU656   (0x1 << RK3576_ISP_CTRL_MODE_SHIFT)
#define RK3576_ISP_CTRL_MODE_ITU601   (0x2 << RK3576_ISP_CTRL_MODE_SHIFT)
#define RK3576_ISP_CTRL_MODE_BAYER    (0x0 << RK3576_ISP_CTRL_MODE_SHIFT)
#define RK3576_ISP_CTRL_INFORM_ENABLE (1 << 7)  /* Input formatter enable   */
#define RK3576_ISP_CTRL_CFG_UPD       (1 << 10) /* Latch shadow registers   */
#define RK3576_ISP_CTRL_GEN_CFG_UPD   (1 << 11) /* Immediate shadow latch   */

/* ISP_ACQ_PROP bits: sampling edge, sync polarity, Bayer pattern */

#define RK3576_ISP_ACQ_SAMPL_EDGE_POS (1 << 0)
#define RK3576_ISP_ACQ_HSY_LOW_ACTIVE (1 << 1)
#define RK3576_ISP_ACQ_VSY_LOW_ACTIVE (1 << 2)
#define RK3576_ISP_ACQ_BAYER_SHIFT    (3)
#define RK3576_ISP_ACQ_BAYER_MASK     (3 << RK3576_ISP_ACQ_BAYER_SHIFT)
#define RK3576_ISP_ACQ_BAYER_RGGB     (0 << RK3576_ISP_ACQ_BAYER_SHIFT)
#define RK3576_ISP_ACQ_BAYER_GRBG     (1 << RK3576_ISP_ACQ_BAYER_SHIFT)
#define RK3576_ISP_ACQ_BAYER_GBRG     (2 << RK3576_ISP_ACQ_BAYER_SHIFT)
#define RK3576_ISP_ACQ_BAYER_BGGR     (3 << RK3576_ISP_ACQ_BAYER_SHIFT)
#define RK3576_ISP_ACQ_INPUT_SEL_12B  (2 << 5) /* 12-bit input selection   */

/* ISP top-level interrupt bits (IMSC / RIS / MIS / ICR) */

#define RK3576_ISP_INT_ISP_OFF      (1 << 0)
#define RK3576_ISP_INT_FRAME        (1 << 1) /* Frame processed          */
#define RK3576_ISP_INT_DATA_LOSS    (1 << 2)
#define RK3576_ISP_INT_PIC_SIZE_ERR (1 << 3)
#define RK3576_ISP_INT_V_START      (1 << 4)
#define RK3576_ISP_INT_H_START      (1 << 5)
#define RK3576_ISP_INT_FRAME_IN     (1 << 6) /* Frame received           */
#define RK3576_ISP_INT_ALL          (0x7f)

/* Black level subtraction (BLS).  TODO: verify the block base against the
 * RK3576 TRM; taken from the Rockchip ISP register layout.
 */

#define RK3576_ISP_BLS_CTRL_OFFSET    0x0c00
#define RK3576_ISP_BLS_SAMPLES_OFFSET 0x0c04
#define RK3576_ISP_BLS_A_FIXED_OFFSET 0x0c24
#define RK3576_ISP_BLS_B_FIXED_OFFSET 0x0c28
#define RK3576_ISP_BLS_C_FIXED_OFFSET 0x0c2c
#define RK3576_ISP_BLS_D_FIXED_OFFSET 0x0c30

#define RK3576_ISP_BLS_CTRL_ENABLE    (1 << 0)
#define RK3576_ISP_BLS_CTRL_MODE_FIX  (0 << 1) /* Use the *_FIXED values   */
#define RK3576_ISP_BLS_CTRL_MODE_MEAS (1 << 1)

/* Demosaic and static white balance gains.  TODO: verify for RK3576. */

#define RK3576_ISP_DEMOSAIC_OFFSET    0x0238
#define RK3576_ISP_AWB_GAIN_G_OFFSET  0x023c /* [27:16] gain_gr, [11:0] gb  */
#define RK3576_ISP_AWB_GAIN_RB_OFFSET 0x0240 /* [27:16] gain_r,  [11:0] b */

#define RK3576_ISP_DEMOSAIC_BYPASS    (1 << 10)
#define RK3576_ISP_DEMOSAIC_TH_MASK   (0xff)

/* Unity white-balance gain: the gain fields are U4.8, so 0x100 == 1.0x */

#define RK3576_ISP_AWB_GAIN_UNITY 0x100
#define RK3576_ISP_AWB_GAIN_MAX   0xfff

/* Gamma-out curve.  16 sample points, equidistant segmentation.
 * TODO: verify the block base and the sample count for RK3576.
 */

#define RK3576_ISP_GAMMA_OUT_MODE_OFF 0x0148
#define RK3576_ISP_GAMMA_OUT_Y_OFFSET 0x014c /* First of 17 sample words */
#define RK3576_ISP_GAMMA_OUT_NPOINTS  17
#define RK3576_ISP_GAMMA_OUT_EQU_SEG  0 /* Equidistant segmentation  */

/* Colour space conversion (YUV output) coefficients and range control */

#define RK3576_ISP_CTRL_YUV_FULL (1 << 15) /* Full-range YUV output    */

/* Memory interface (MI), main picture path.  TODO: verify for RK3576. */

#define RK3576_ISP_MI_CTRL_OFFSET       0x1400
#define RK3576_ISP_MI_INIT_OFFSET       0x1404
#define RK3576_ISP_MI_MP_Y_BASE_OFFSET  0x1408
#define RK3576_ISP_MI_MP_Y_SIZE_OFFSET  0x140c
#define RK3576_ISP_MI_MP_Y_OFFS_OFFSET  0x1410
#define RK3576_ISP_MI_MP_CB_BASE_OFFSET 0x1420
#define RK3576_ISP_MI_MP_CB_SIZE_OFFSET 0x1424
#define RK3576_ISP_MI_MP_CB_OFFS_OFFSET 0x1428
#define RK3576_ISP_MI_MP_CR_BASE_OFFSET 0x1438
#define RK3576_ISP_MI_MP_CR_SIZE_OFFSET 0x143c
#define RK3576_ISP_MI_IMSC_OFFSET       0x15c4
#define RK3576_ISP_MI_RIS_OFFSET        0x15c8
#define RK3576_ISP_MI_MIS_OFFSET        0x15cc
#define RK3576_ISP_MI_ICR_OFFSET        0x15d0

#define RK3576_ISP_MI_CTRL_MP_ENABLE    (1 << 0)
#define RK3576_ISP_MI_CTRL_MP_WRITE_YUV (1 << 3) /* YUV (vs raw) output  */
#define RK3576_ISP_MI_CTRL_MP_FMT_SEMI  (2 << 4) /* Semi-planar (NV12)   */
#define RK3576_ISP_MI_CTRL_BURST_LEN_16 (2 << 16)
#define RK3576_ISP_MI_CTRL_INIT_BASE_EN (1 << 4)
#define RK3576_ISP_MI_CTRL_INIT_OFFS_EN (1 << 5)

#define RK3576_ISP_MI_INIT_SOFT_UPD     (1 << 4) /* Latch MI shadow regs */

#define RK3576_ISP_MI_INT_MP_FRAME      (1 << 0) /* Main path frame done */
#define RK3576_ISP_MI_INT_FILL_MP_Y     (1 << 11)
#define RK3576_ISP_MI_INT_ALL           (0xffff)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_ISP_H */
