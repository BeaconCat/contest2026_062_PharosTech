/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_pcie.h
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
 * RK3576 PCIe register definitions.
 *
 * The controller is a Synopsys DesignWare PCIe core wrapped in a Rockchip
 * "client" glue block.  Three distinct register regions are involved:
 *
 *   1. Client APB block ("pcie-apb", 0x2A200000): Rockchip-specific glue
 *      (RC/EP mode select, LTSSM enable, link status, legacy INTx and misc
 *      interrupt status/mask).  All writes use the Rockchip hiword-mask
 *      scheme: bits [31:16] are the write-enable mask for bits [15:0].
 *
 *   2. DesignWare DBI ("pcie-dbi", 0x22000000, 4MB): the root complex' own
 *      type-1 configuration space plus the DW "port logic" registers, plus
 *      the unrolled iATU register file at DBI + 0x300000.
 *
 *   3. Config space window ("config", 0x20000000, 1MB): a CPU address range
 *      that an outbound iATU region retargets to PCIe CFG0/CFG1 TLPs in
 *      order to reach downstream devices.
 *
 * Reference: Rockchip RK3576 TRM "PCIe2.1 Controller"; Synopsys DesignWare
 * PCIe databook (port logic and iATU register maps).
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_PCIE_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_PCIE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Rockchip hiword-mask helpers -- bits [31:16] enable the write of the
 * matching bits in [15:0].  Only the low 16 bits of a register can be
 * updated this way.
 */

#define RK3576_PCIE_HIWORD(mask, val) \
  ((((uint32_t)(mask)&0xffffu) << 16) | ((uint32_t)(val)&0xffffu))
#define RK3576_PCIE_HIWORD_SET(bits)  RK3576_PCIE_HIWORD(bits, bits)
#define RK3576_PCIE_HIWORD_CLR(bits)  RK3576_PCIE_HIWORD(bits, 0)

/* Client APB register offsets (relative to the "pcie-apb" base) ***********/

#define RK3576_PCIE_CLIENT_GENERAL_CON        0x0000
#define RK3576_PCIE_CLIENT_INTR_STATUS_LEGACY 0x0008
#define RK3576_PCIE_CLIENT_INTR_STATUS_MISC   0x0010
#define RK3576_PCIE_CLIENT_INTR_MASK_LEGACY   0x001c
#define RK3576_PCIE_CLIENT_INTR_MASK_MISC     0x0024
#define RK3576_PCIE_CLIENT_GENERAL_DEBUG      0x0104
#define RK3576_PCIE_CLIENT_HOT_RESET_CTRL     0x0180
#define RK3576_PCIE_CLIENT_LTSSM_STATUS       0x0300

/* RK3576_PCIE_CLIENT_GENERAL_CON bit fields (hiword-masked) */

#define RK3576_PCIE_CLIENT_MODE_MASK 0x00f0 /* device_type[3:0] */
#define RK3576_PCIE_CLIENT_RC_MODE   0x0040 /* root complex */
#define RK3576_PCIE_CLIENT_EP_MODE   0x0000 /* endpoint */
#define RK3576_PCIE_CLIENT_LTSSM_MASK                                       \
  0x000c                                     /* app_ltssm_enable[1:0] */
#define RK3576_PCIE_CLIENT_LTSSM_ENABLE  0x000c
#define RK3576_PCIE_CLIENT_LTSSM_DISABLE 0x0008

/* RK3576_PCIE_CLIENT_HOT_RESET_CTRL bit fields (hiword-masked) */

#define RK3576_PCIE_CLIENT_LTSSM_EN_ENHANCE (1 << 4)

/* RK3576_PCIE_CLIENT_LTSSM_STATUS -- plain read-only status, no hiword */

#define RK3576_PCIE_LTSSM_STATE_MASK 0x0000003f /* current LTSSM state */
#define RK3576_PCIE_LTSSM_STATE_L0   0x00000011 /* L0: link operational */
#define RK3576_PCIE_SMLH_LINKUP      (1 << 16)  /* physical layer up */
#define RK3576_PCIE_RDLH_LINKUP      (1 << 17)  /* data link layer up */
#define RK3576_PCIE_LINKUP \
  (RK3576_PCIE_SMLH_LINKUP | RK3576_PCIE_RDLH_LINKUP)

/* RK3576_PCIE_CLIENT_INTR_STATUS_MISC / _MASK_MISC bits used here */

#define RK3576_PCIE_MISC_RDLH_LINK_UP_CHGED (1 << 1)
#define RK3576_PCIE_MISC_LINK_REQ_RST_NOT   (1 << 2)

/* Legacy INTx: four lines, INTA..INTD in bits [3:0] */

#define RK3576_PCIE_LEGACY_INT_MASK 0x0000000f

/* DesignWare DBI: standard type-1 header offsets we touch *****************/

#define RK3576_PCIE_DBI_VENDOR_ID    0x0000 /* 16-bit */
#define RK3576_PCIE_DBI_DEVICE_ID    0x0002 /* 16-bit */
#define RK3576_PCIE_DBI_COMMAND      0x0004 /* 16-bit */
#define RK3576_PCIE_DBI_CLASS_REV    0x0008 /* 32-bit, class in [31:8] */
#define RK3576_PCIE_DBI_HEADER_TYPE  0x000e /* 8-bit */
#define RK3576_PCIE_DBI_BAR0         0x0010
#define RK3576_PCIE_DBI_BAR1         0x0014
#define RK3576_PCIE_DBI_PRIMARY_BUS  0x0018 /* prim/sec/sub/latency */

/* Class code written into the RC's own header: PCI-to-PCI bridge
 * (base class 0x06, sub class 0x04, prog-if 0x00).
 */

#define RK3576_PCIE_CLASS_BRIDGE_PCI 0x060400

/* PCI Express capability structure -- fixed at 0x70 in the DW core.
 * TODO: confirm against the RK3576 TRM capability pointer chain; the DW
 * default of 0x70 is used by all Rockchip DW PCIe ports to date.
 */

#define RK3576_PCIE_CAP_BASE       0x0070
#define RK3576_PCIE_CAP_LINK_CAP   (RK3576_PCIE_CAP_BASE + 0x0c)
#define RK3576_PCIE_CAP_LINK_CTRL2 (RK3576_PCIE_CAP_BASE + 0x30)
#define RK3576_PCIE_LINK_SPEED_MASK 0x0000000f

/* DesignWare "port logic" registers (DBI space) */

#define RK3576_PCIE_PL_PORT_LINK_CTRL 0x0710
#define RK3576_PCIE_PL_LINK_MODE_MASK 0x003f0000 /* LINK_CAPABLE[21:16] */
#define RK3576_PCIE_PL_LINK_MODE_1_LANE  (0x01 << 16)
#define RK3576_PCIE_PL_LINK_MODE_2_LANES (0x03 << 16)
#define RK3576_PCIE_PL_LINK_MODE_4_LANES (0x07 << 16)

#define RK3576_PCIE_PL_GEN2_CTRL       0x080c
#define RK3576_PCIE_PL_LINK_WIDTH_MASK 0x00001f00 /* [12:8] */
#define RK3576_PCIE_PL_LINK_WIDTH(n)   (((n)&0x1f) << 8)
#define RK3576_PCIE_PL_SPEED_CHANGE    (1 << 17)  /* DIRECTED_SPEED_CHANGE */

#define RK3576_PCIE_MISC_CONTROL_1 0x08bc
#define RK3576_PCIE_DBI_RO_WR_EN   (1 << 0)

/* Unrolled iATU register file *********************************************/

/* Base of the unrolled iATU window inside DBI space. */

#define RK3576_PCIE_ATU_UNROLL_BASE 0x00300000

/* Per-region stride is 0x200; inbound regions are selected by bit 8. */

#define RK3576_PCIE_ATU_REGION_STRIDE 0x200
#define RK3576_PCIE_ATU_REGION_INBOUND_FLAG 0x100

#define RK3576_PCIE_ATU_OB_OFFSET(r) \
  (RK3576_PCIE_ATU_UNROLL_BASE + ((r)*RK3576_PCIE_ATU_REGION_STRIDE))
#define RK3576_PCIE_ATU_IB_OFFSET(r)                                        \
  (RK3576_PCIE_ATU_UNROLL_BASE + ((r)*RK3576_PCIE_ATU_REGION_STRIDE) +      \
   RK3576_PCIE_ATU_REGION_INBOUND_FLAG)

/* Register offsets within one iATU region */

#define RK3576_PCIE_ATU_CTRL1       0x00
#define RK3576_PCIE_ATU_CTRL2       0x04
#define RK3576_PCIE_ATU_LOWER_BASE  0x08
#define RK3576_PCIE_ATU_UPPER_BASE  0x0c
#define RK3576_PCIE_ATU_LIMIT       0x10
#define RK3576_PCIE_ATU_LOWER_TGT   0x14
#define RK3576_PCIE_ATU_UPPER_TGT   0x18
#define RK3576_PCIE_ATU_UPPER_LIMIT 0x20

/* CTRL1: TLP type in [4:0] */

#define RK3576_PCIE_ATU_TYPE_MEM  0x0
#define RK3576_PCIE_ATU_TYPE_IO   0x2
#define RK3576_PCIE_ATU_TYPE_CFG0 0x4
#define RK3576_PCIE_ATU_TYPE_CFG1 0x5
#define RK3576_PCIE_ATU_INCREASE_REGION_SIZE (1 << 13)

/* CTRL2 */

#define RK3576_PCIE_ATU_ENABLE          (1u << 31)
#define RK3576_PCIE_ATU_BAR_MODE_ENABLE (1u << 30)

/* Pipe PHY GRF (syscon "rockchip,rk3576-pipe-phy-grf") register offsets.
 * Every field is written with the hiword-mask scheme.
 */

#define RK3576_PIPE_PHY_GRF_CON0 0x0000
#define RK3576_PIPE_PHY_GRF_CON1 0x0004
#define RK3576_PIPE_PHY_GRF_CON2 0x0008
#define RK3576_PIPE_PHY_GRF_CON3 0x000c
#define RK3576_PIPE_PHY_GRF_STATUS0 0x0034

#define RK3576_PIPE_PHY_STATUS0_PHY_READY (1 << 6)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_PCIE_H */
