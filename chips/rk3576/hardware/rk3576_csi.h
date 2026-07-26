/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_csi.h
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
 * RK3576 MIPI CSI-2 receive path register definitions.
 *
 * Two distinct blocks are described here:
 *
 *  1) MIPI CSI-2 host controller ("csihost", Synopsys DesignWare MIPI CSI-2
 *     Host v1.x).  Five instances, each a 64KB window:
 *
 *       CSI2HOST0 0x27C80000  IRQ 312 / 313
 *       CSI2HOST1 0x27C90000  IRQ 314 / 315
 *       CSI2HOST2 0x27CA0000  IRQ 316 / 317
 *       CSI2HOST3 0x27CB0000  IRQ 383 / 384
 *       CSI2HOST4 0x27CC0000  IRQ 385 / 386
 *
 *     The host controller only terminates the CSI-2 link protocol (lane
 *     merging, ECC/CRC, VC/DT filtering).  Pixel data is handed to VICAP /
 *     ISP over an internal bus, so this block has no DMA of its own.
 *
 *  2) Rockchip MIPI RX D-PHY ("csi2-dphy-hw", Innosilicon).  Two instances,
 *     each a 32KB window plus a GRF companion:
 *
 *       DPHY0 0x2B030000, GRF 0x2603A000
 *       DPHY1 0x2B070000, GRF 0x2604C000
 *
 *     A separate DC-PHY (combo D-PHY/C-PHY) lives at 0x2B020000 with GRF
 *     0x26034000; it is not driven by this header.
 *
 * Sources: reverse-engineered board DTS
 * (4-HardwareData/k7_debian_vendor.dts) for base addresses, interrupts and
 * GRF phandles; Synopsys MIPI CSI-2 host programming model for the csihost
 * register map.  D-PHY offsets follow the Rockchip vendor register model
 * (byte offset == PHY register index * 4).
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_CSI_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_CSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Block base addresses ****************************************************/

#define RK3576_CSI2HOST0_ADDR 0x27c80000
#define RK3576_CSI2HOST1_ADDR 0x27c90000
#define RK3576_CSI2HOST2_ADDR 0x27ca0000
#define RK3576_CSI2HOST3_ADDR 0x27cb0000
#define RK3576_CSI2HOST4_ADDR 0x27cc0000

#define RK3576_CSI2HOST_SIZE  0x10000
#define RK3576_CSI2HOST_NHOST 5

#define RK3576_CSI2DPHY0_ADDR 0x2b030000
#define RK3576_CSI2DPHY1_ADDR 0x2b070000
#define RK3576_CSI2DPHY_SIZE  0x8000
#define RK3576_CSI2DPHY_NPHY  2

/* GRF windows associated with each D-PHY. */

#define RK3576_CSI2DPHY0_GRF_ADDR 0x2603a000
#define RK3576_CSI2DPHY1_GRF_ADDR 0x2604c000

/* Combo DC-PHY (D-PHY/C-PHY), listed for completeness. */

#define RK3576_MIPI_DCPHY_ADDR     0x2b020000
#define RK3576_MIPI_DCPHY_GRF_ADDR 0x26034000

/* CSI-2 host controller registers *****************************************/

#define RK3576_CSI2_VERSION_OFFSET        0x0000 /* IP core version (RO)    */
#define RK3576_CSI2_N_LANES_OFFSET        0x0004 /* Active lane count       */
#define RK3576_CSI2_RESETN_OFFSET         0x0008 /* Controller reset (low)  */
#define RK3576_CSI2_INT_ST_MAIN_OFFSET    0x000c /* Top-level IRQ status    */
#define RK3576_CSI2_DATA_IDS_1_OFFSET     0x0010 /* DT filter for DI0..DI3  */
#define RK3576_CSI2_DATA_IDS_2_OFFSET     0x0014 /* DT filter for DI4..DI7  */
#define RK3576_CSI2_INT_ST_PHY_FATAL_OFF  0x0020 /* PHY fatal status (RC)   */
#define RK3576_CSI2_INT_MSK_PHY_FATAL_OFF 0x0024 /* PHY fatal mask */
#define RK3576_CSI2_INT_FORCE_PHY_FATAL_O 0x0028 /* PHY fatal force */
#define RK3576_CSI2_INT_ST_PKT_FATAL_OFF  0x0030 /* Packet fatal status     */
#define RK3576_CSI2_INT_MSK_PKT_FATAL_OFF 0x0034 /* Packet fatal mask */
#define RK3576_CSI2_INT_FORCE_PKT_FATAL_O 0x0038 /* Packet fatal force */
#define RK3576_CSI2_INT_ST_FRAME_FATAL_OF 0x0040 /* Frame fatal status */
#define RK3576_CSI2_INT_MSK_FRAME_FATAL_O 0x0044 /* Frame fatal mask */
#define RK3576_CSI2_INT_FORCE_FRAME_FAT_O 0x0048 /* Frame fatal force */
#define RK3576_CSI2_INT_ST_PHY_OFFSET     0x0050 /* PHY error status        */
#define RK3576_CSI2_INT_MSK_PHY_OFFSET    0x0054 /* PHY error mask          */
#define RK3576_CSI2_INT_FORCE_PHY_OFFSET  0x0058 /* PHY error force         */
#define RK3576_CSI2_INT_ST_PKT_OFFSET     0x0060 /* Packet error status     */
#define RK3576_CSI2_INT_MSK_PKT_OFFSET    0x0064 /* Packet error mask       */
#define RK3576_CSI2_INT_FORCE_PKT_OFFSET  0x0068 /* Packet error force      */
#define RK3576_CSI2_INT_ST_LINE_OFFSET    0x0070 /* Line error status       */
#define RK3576_CSI2_INT_MSK_LINE_OFFSET   0x0074 /* Line error mask         */
#define RK3576_CSI2_INT_FORCE_LINE_OFFSET 0x0078 /* Line error force */

/* CSI2_N_LANES: number of active data lanes minus one, bits [2:0]. */

#define RK3576_CSI2_N_LANES_MASK 0x00000007

/* CSI2_CSI2_RESETN: bit0 low holds the controller in reset. */

#define RK3576_CSI2_RESETN_ENABLE 0x00000001

/* CSI2_INT_ST_MAIN: one bit per error group. */

#define RK3576_CSI2_INT_MAIN_PHY_FATAL   (1 << 0)
#define RK3576_CSI2_INT_MAIN_PKT_FATAL   (1 << 1)
#define RK3576_CSI2_INT_MAIN_FRAME_FATAL (1 << 2)
#define RK3576_CSI2_INT_MAIN_PHY         (1 << 16)
#define RK3576_CSI2_INT_MAIN_PKT         (1 << 17)
#define RK3576_CSI2_INT_MAIN_LINE        (1 << 18)

/* CSI2_DATA_IDS_1/2: four 8-bit data-identifier slots per register.  A slot
 * holds VC[7:6] | DT[5:0] and enables ECC-corrected routing of that DI.
 */

#define RK3576_CSI2_DATA_IDS_PER_REG 4
#define RK3576_CSI2_DATA_ID_SHIFT(n) (((n)&3) * 8)
#define RK3576_CSI2_DATA_ID_MASK(n)  (0xffu << RK3576_CSI2_DATA_ID_SHIFT(n))
#define RK3576_CSI2_DATA_ID(vc, dt)  ((((vc)&0x3) << 6) | ((dt)&0x3f))

/* CSI-2 data types (MIPI CSI-2 spec, table "Data Type Classes"). */

#define RK3576_CSI2_DT_YUV422_8B  0x1e
#define RK3576_CSI2_DT_YUV422_10B 0x1f
#define RK3576_CSI2_DT_RGB888     0x24
#define RK3576_CSI2_DT_RAW8       0x2a
#define RK3576_CSI2_DT_RAW10      0x2b
#define RK3576_CSI2_DT_RAW12      0x2c

/* Rockchip MIPI RX D-PHY registers ****************************************
 *
 * The Innosilicon PHY is addressed as 8-bit registers on a 32-bit stride:
 * byte offset = phy register index * 4.  Only the low 8 bits are valid.
 */

#define RK3576_DPHY_REG(idx) ((idx)*4)

/* Lane enable / global control (PHY index 0x00). */

#define RK3576_DPHY_CTRL_LANE_ENABLE_OFFSET RK3576_DPHY_REG(0x00)
#define RK3576_DPHY_CTRL_LANE_ENABLE_CK     (1 << 6)
#define RK3576_DPHY_CTRL_LANE_ENABLE_SHIFT  2 /* data lane 0 at bit 2 */
#define RK3576_DPHY_CTRL_LANE_ENABLE_MASK   0x3c
#define RK3576_DPHY_CTRL_ENABLE             (1 << 0)

/* Digital reset / power control (PHY index 0x21). */

#define RK3576_DPHY_CTRL_DIG_RST_OFFSET RK3576_DPHY_REG(0x21)
#define RK3576_DPHY_CTRL_DIG_RST_RESET  0x00 /* hold digital core in reset */
#define RK3576_DPHY_CTRL_DIG_RST_RUN    0x01 /* release                    */

/* THS-SETTLE counters.  One per lane plus one for the clock lane; the
 * counter unit is one PHY reference-clock period.
 */

#define RK3576_DPHY_CLK_WR_THS_SETTLE_OFFSET   RK3576_DPHY_REG(0x58)
#define RK3576_DPHY_LANE0_WR_THS_SETTLE_OFFSET RK3576_DPHY_REG(0x78)
#define RK3576_DPHY_LANE_WR_THS_SETTLE_STRIDE  RK3576_DPHY_REG(0x40)
#define RK3576_DPHY_THS_SETTLE_MASK            0x7f

/* Per-lane calibration enable (skew calibration for >1.5Gbps). */

#define RK3576_DPHY_CLK_CALIB_EN_OFFSET   RK3576_DPHY_REG(0x5a)
#define RK3576_DPHY_LANE0_CALIB_EN_OFFSET RK3576_DPHY_REG(0x7a)
#define RK3576_DPHY_LANE_CALIB_EN_STRIDE  RK3576_DPHY_REG(0x40)
#define RK3576_DPHY_CALIB_ENABLE          0x01
#define RK3576_DPHY_CALIB_DISABLE         0x00

/* MIPI D-PHY GRF registers ************************************************/

#define RK3576_DPHY_GRF_CON0_OFFSET 0x0000 /* hiword-masked control */

/* CON0 fields.  The register is hiword-masked: bits [31:16] enable the
 * write of the corresponding bit in [15:0].
 */

#define RK3576_DPHY_GRF_CON0_LANE_MASK    0x000f /* per-lane enable    */
#define RK3576_DPHY_GRF_CON0_CLK_LANE_EN  (1 << 4)
#define RK3576_DPHY_GRF_CON0_PHY_MODE_CSI (1 << 5) /* 0 = DSI, 1 = CSI */
#define RK3576_DPHY_GRF_HIWORD(mask, val) \
  ((uint32_t)(mask) << 16 | ((uint32_t)(val) & (uint32_t)(mask)))

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_CSI_H */
