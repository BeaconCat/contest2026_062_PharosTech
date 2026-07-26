/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_vdec.h
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
 * RK3576 RKVDEC (video decoder) hardware register definitions.
 *
 * The RK3576 decoder is an "RKVDEC v383" instance (vendor DTS compatible
 * "rockchip,rkv-decoder-rk3576", "rockchip,rkv-decoder-v383").  It decodes
 * H.264 / H.265 / VP9 / AVS2 / AV1 in hardware.
 *
 * Register space (vendor DTS node rkvdec@27b00000):
 *
 *   0x27B00000 + 0x100   "link"   link-list (command chain) mode registers
 *   0x27B00100 + 0x600   "regs"   decoder core register file (used here)
 *   0x27B00800 + 0x40    IOMMU    rkvdec MMU (read/write ports at +0x900)
 *
 * The core register file is a flat array of 32-bit "swreg" words.  A decode
 * task is issued by writing a complete register image (stream address,
 * output frame address, reference frame array, picture parameters) and then
 * setting the DEC_E bit of RKVDEC_REG_INTERRUPT.  Completion is signalled
 * by GIC INTID 308 (SPI 276) and reported in the same register.
 *
 * The offsets and bit positions below follow the public Rockchip RKVDEC
 * programming model (hardware facts only; no vendor source was copied).
 *
 * TODO: The RK3576 TRM chapter for RKVDEC is not publicly available.  The
 * fields marked "TODO: verify" are taken from the older RKVDEC generation
 * that the v383 core is register compatible with for the H.264 subset used
 * by this driver, and must be re-checked once the TRM is obtainable.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VDEC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VDEC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Block base addresses ******************************************************/

#define RK3576_VDEC_LINK_ADDR 0x27b00000 /* Link-mode registers      */
#define RK3576_VDEC_LINK_SIZE 0x00000100
#define RK3576_VDEC_ADDR      0x27b00100 /* Decoder core registers   */
#define RK3576_VDEC_SIZE      0x00000600
#define RK3576_VDEC_MMU_ADDR  0x27b00800 /* rkvdec IOMMU (v2)        */
#define RK3576_VDEC_MMU_SIZE  0x00000040

/* Number of 32-bit words in the core register file. */

#define RK3576_VDEC_REG_COUNT (RK3576_VDEC_SIZE / 4)

/* Core register offsets (relative to RK3576_VDEC_ADDR) *********************/

#define RKVDEC_REG_ID            0x0000 /* Hardware version / build id   */
#define RKVDEC_REG_INTERRUPT     0x0004 /* Start bit, IRQ and status     */
#define RKVDEC_REG_SYSCTRL       0x0008 /* Codec mode, endian, swap      */
#define RKVDEC_REG_PICPAR        0x000c /* Picture size in macroblocks   */
#define RKVDEC_REG_STRM_RLC_BASE 0x0010 /* Bitstream buffer address      */
#define RKVDEC_REG_STRM_LEN      0x0014 /* Bitstream length in bytes     */
#define RKVDEC_REG_CABACTBL_PROB_BASE \
  0x0018                                /* CABAC init table address      */
#define RKVDEC_REG_DECOUT_BASE   0x001c /* Decoded (output) frame        */
#define RKVDEC_REG_Y_VIRSTRIDE   0x0020 /* Luma line stride / 16         */
#define RKVDEC_REG_YUV_VIRSTRIDE 0x0024 /* Whole frame stride / 16       */
#define RKVDEC_REG_PPS_BASE      0x0028 /* SPS/PPS parameter set buffer  */
#define RKVDEC_REG_RPS_BASE      0x002c /* Reference picture set buffer  */
#define RKVDEC_REG_DIRMV_BASE    0x0030 /* Direct/collocated MV buffer   */
#define RKVDEC_REG_ERRINFO_BASE  0x0034 /* Error information output      */
#define RKVDEC_REG_ERR_CTRL      0x0038 /* Error concealment control     */
#define RKVDEC_REG_CACHE_CTRL    0x003c /* Read/write cache control      */

/* Reference frame descriptor array.  Each reference picture occupies one
 * base-address word and one POC/flags word.
 */

#define RKVDEC_MAX_REFS          16
#define RKVDEC_REG_REFER_BASE(n) (0x0040 + ((n)*4)) /* n = 0..15      */
#define RKVDEC_REG_REFER_POC(n)  (0x0080 + ((n)*4)) /* n = 0..15      */

/* Collocated (direct mode) MV buffer for each reference picture. */

#define RKVDEC_REG_REFER_COLMV_BASE(n) (0x00c0 + ((n)*4)) /* n = 0..15 */

/* Performance / debug counters (read only, not used by this driver). */

#define RKVDEC_REG_PERF_LATENCY  0x0100
#define RKVDEC_REG_PERF_RD_BYTES 0x0104
#define RKVDEC_REG_PERF_WR_BYTES 0x0108

/* RKVDEC_REG_INTERRUPT bit definitions *************************************/

#define RKVDEC_INT_DEC_E           (1 << 0)  /* Write 1: start decoding    */
#define RKVDEC_INT_DEC_IRQ_DIS     (1 << 1)  /* Mask the completion IRQ    */
#define RKVDEC_INT_DEC_TIMEOUT_E   (1 << 2)  /* Enable bus timeout detect  */
#define RKVDEC_INT_BUF_EMPTY_E     (1 << 3)  /* Enable buffer-empty detect */
#define RKVDEC_INT_DEC_IRQ         (1 << 4)  /* IRQ raised (write to clr)  */
#define RKVDEC_INT_DEC_RDY_STA     (1 << 8)  /* Picture decoded, no error  */
#define RKVDEC_INT_DEC_BUS_STA     (1 << 9)  /* AXI bus error              */
#define RKVDEC_INT_DEC_ERROR_STA   (1 << 10) /* Stream / decode error      */
#define RKVDEC_INT_DEC_TIMEOUT_STA (1 << 11) /* Hardware timeout           */
#define RKVDEC_INT_BUF_EMPTY_STA   (1 << 12) /* Bitstream exhausted        */
#define RKVDEC_INT_COLMV_REF_ERROR (1 << 13) /* Colocated MV read error    */
#define RKVDEC_INT_CABU_END_STA    (1 << 14) /* CABAC unit finished        */
#define RKVDEC_INT_SOFTRESET_RDY   (1 << 15) /* Soft reset completed       */

/* Every status bit, used to clear the register after a task. */

#define RKVDEC_INT_STA_MASK 0x0000fff0

/* Any status bit that means the picture is not usable. */

#define RKVDEC_INT_ERR_MASK                                \
  (RKVDEC_INT_DEC_BUS_STA | RKVDEC_INT_DEC_ERROR_STA |     \
   RKVDEC_INT_DEC_TIMEOUT_STA | RKVDEC_INT_BUF_EMPTY_STA | \
   RKVDEC_INT_COLMV_REF_ERROR)

/* Soft reset is requested through the same register. */

#define RKVDEC_INT_SOFTRESET_E (1 << 20) /* TODO: verify on RK3576     */

/* RKVDEC_REG_SYSCTRL bit definitions ***************************************/

#define RKVDEC_SYSCTRL_IN_ENDIAN  (1 << 0) /* 0: little endian input     */
#define RKVDEC_SYSCTRL_IN_SWAP32  (1 << 1)
#define RKVDEC_SYSCTRL_IN_SWAP64  (1 << 2)
#define RKVDEC_SYSCTRL_STR_ENDIAN (1 << 3)
#define RKVDEC_SYSCTRL_STR_SWAP32 (1 << 4)
#define RKVDEC_SYSCTRL_STR_SWAP64 (1 << 5)
#define RKVDEC_SYSCTRL_OUT_ENDIAN (1 << 6)
#define RKVDEC_SYSCTRL_OUT_SWAP32 (1 << 7)
#define RKVDEC_SYSCTRL_OUT_SWAP64 (1 << 8)
#define RKVDEC_SYSCTRL_RLC_DIRECT (1 << 9)  /* RLC direct write mode      */
#define RKVDEC_SYSCTRL_RLC_MODE   (1 << 10) /* 0: bitstream, 1: RLC       */

/* First valid bit of the bitstream inside the first word (0..63). */

#define RKVDEC_SYSCTRL_STRM_START_BIT_SHIFT 11
#define RKVDEC_SYSCTRL_STRM_START_BIT_MASK  (0x3f << 11)

/* Codec select field.  TODO: verify the v383 encoding against the TRM. */

#define RKVDEC_SYSCTRL_MODE_SHIFT 20
#define RKVDEC_SYSCTRL_MODE_MASK  (0x1f << 20)
#define RKVDEC_SYSCTRL_MODE_HEVC  0
#define RKVDEC_SYSCTRL_MODE_H264  1
#define RKVDEC_SYSCTRL_MODE_VP9   2
#define RKVDEC_SYSCTRL_MODE_AVS2  3
#define RKVDEC_SYSCTRL_MODE_AV1   4

/* Output pixel format / bit depth. */

#define RKVDEC_SYSCTRL_YUV_MODE_SHIFT 25 /* 0: 4:2:0, 1: 4:2:2, 3: mono */
#define RKVDEC_SYSCTRL_YUV_MODE_MASK  (0x3 << 25)
#define RKVDEC_SYSCTRL_BITDEPTH_SHIFT 27 /* 0: 8-bit, 1: 10-bit         */
#define RKVDEC_SYSCTRL_BITDEPTH_MASK  (0x3 << 27)

/* RKVDEC_REG_PICPAR bit definitions ****************************************/

/* Picture size expressed in 16x16 macroblocks, minus one. */

#define RKVDEC_PICPAR_WIDTH_SHIFT  0
#define RKVDEC_PICPAR_WIDTH_MASK   (0x1ff << 0)
#define RKVDEC_PICPAR_HEIGHT_SHIFT 9
#define RKVDEC_PICPAR_HEIGHT_MASK  (0x1ff << 9)
#define RKVDEC_PICPAR_FIELD_FLAG   (1 << 18) /* Field (interlaced) picture */
#define RKVDEC_PICPAR_TOPFIELD     (1 << 19) /* Current field is the top   */

/* RKVDEC_REG_REFER_POC bit definitions *************************************/

#define RKVDEC_REFER_POC_MASK   0x3fffffff /* Picture order count       */
#define RKVDEC_REFER_FIELD_FLAG (1u << 30) /* Reference is a field      */
#define RKVDEC_REFER_VALID      (1u << 31) /* Slot holds a valid frame  */

/* RKVDEC_REG_ERR_CTRL bit definitions **************************************/

#define RKVDEC_ERR_CTRL_CONCEAL_E (1 << 0) /* Conceal corrupted MBs      */
#define RKVDEC_ERR_CTRL_ERRINFO_E (1 << 1) /* Write per-MB error info    */

/* RKVDEC_REG_CACHE_CTRL bit definitions ************************************/

#define RKVDEC_CACHE_CTRL_RD_EN (1 << 0) /* Reference read cache       */
#define RKVDEC_CACHE_CTRL_WR_EN (1 << 1) /* Output write combine       */

/* Link (command chain) mode registers **************************************/

/* Only the enable/mode word is touched: this driver runs the decoder in
 * plain register mode, so link mode must be left disabled.
 */

#define RKVDEC_LINK_REG_EN        0x0000
#define RKVDEC_LINK_EN            (1 << 0)
#define RKVDEC_LINK_REG_MODE      0x0004
#define RKVDEC_LINK_REG_IP_ENABLE 0x0008

/* rkvdec IOMMU (Rockchip IOMMU v2) *****************************************/

#define RK3576_VDEC_MMU_DTE_ADDR              0x0000 /* Directory table base          */
#define RK3576_VDEC_MMU_STATUS                0x0004
#define RK3576_VDEC_MMU_COMMAND               0x0008
#define RK3576_VDEC_MMU_INT_MASK              0x001c
#define RK3576_VDEC_MMU_INT_STATUS            0x0018

#define RK3576_VDEC_MMU_CMD_ENABLE_PAGING     0
#define RK3576_VDEC_MMU_CMD_DISABLE_PAGING    1
#define RK3576_VDEC_MMU_CMD_ENABLE_STALL      2
#define RK3576_VDEC_MMU_CMD_DISABLE_STALL     3
#define RK3576_VDEC_MMU_CMD_ZAP_CACHE         4

#define RK3576_VDEC_MMU_STATUS_PAGING_ENABLED (1 << 0)

/* Buffer geometry constraints **********************************************/

/* All hardware addresses are programmed as 32-bit byte addresses and the
 * AXI master issues 16-byte bursts: every buffer handed to the decoder must
 * be physically contiguous, below 4GB and 16-byte aligned.  The decoded
 * picture buffer is additionally addressed in 256-byte units by the write
 * client, so it is aligned to 256 bytes here.
 */

#define RKVDEC_STREAM_ALIGN 16
#define RKVDEC_FRAME_ALIGN  256

/* Luma/chroma strides are programmed in units of 16 bytes. */

#define RKVDEC_VIRSTRIDE_UNIT 16

/* Size of the H.264 CABAC initialisation table (3680 x 4 bytes rounded up
 * to the AXI burst size).
 */

#define RKVDEC_H264_CABAC_TAB_SIZE 3680

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_VDEC_H */
