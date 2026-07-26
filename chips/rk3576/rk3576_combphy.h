/****************************************************************************
 * chips/rk3576/rk3576_combphy.h
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
 * RK3576 Naneng COMBPHY: public API.
 *
 * The RK3576 has two "naneng" combo SerDes PHYs (phy@2B050000 and
 * phy@2B060000) that can be muxed to PCIe, USB3, SATA or (Q)SGMII.  Only
 * the PCIe personality is implemented here, which is what the two PCIe
 * root ports need.
 *
 * There is no NuttX generic PHY framework, so this is a plain in-chip
 * helper consumed by rk3576_pcie.c rather than a registered device.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_COMBPHY_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_COMBPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* COMBPHY instance identifiers.  COMBPHY0 feeds PCIe port 0, COMBPHY1
 * feeds PCIe port 1 on the RK3576.
 */

#define RK3576_COMBPHY0    0
#define RK3576_COMBPHY1    1
#define RK3576_COMBPHY_NUM 2

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_combphy_pcie_init
 *
 * Description:
 *   Power up and configure one combo PHY for the PCIe personality: enable
 *   its clocks, release the APB and PHY resets, select PCIe mode in the
 *   pipe PHY GRF, apply the analog trim for the current reference clock
 *   and wait for the PHY-ready status.
 *
 *   Safe to call more than once; a PHY that is already initialised returns
 *   OK immediately.
 *
 * Input Parameters:
 *   phyid - RK3576_COMBPHY0 or RK3576_COMBPHY1
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_combphy_pcie_init(int phyid);

/****************************************************************************
 * Name: rk3576_combphy_uninit
 *
 * Description:
 *   Assert the PHY reset again and mark the instance as uninitialised.
 *   Used when PCIe bring-up fails so that a later retry starts clean.
 *
 * Input Parameters:
 *   phyid - RK3576_COMBPHY0 or RK3576_COMBPHY1
 *
 ****************************************************************************/

void rk3576_combphy_uninit(int phyid);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_COMBPHY_H */
