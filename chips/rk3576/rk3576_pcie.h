/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pcie.h
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
 * RK3576 PCIe root complex driver: public API.
 *
 * The RK3576 exposes two Synopsys DesignWare PCIe 2.1 root ports wrapped in
 * Rockchip glue.  Port 0 (pcie@2a200000, combphy0) is the one routed to the
 * M.2 slot on the KICKPI-K7; port 1 (pcie@2a210000, combphy1) is disabled in
 * the vendor device tree but is described here for completeness.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_PCIE_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_PCIE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Root port identifiers */

#define RK3576_PCIE_PORT0 0
#define RK3576_PCIE_PORT1 1
#define RK3576_PCIE_NPORTS 2

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pcie_initialize
 *
 * Description:
 *   Bring up one RK3576 PCIe root port and register it with the NuttX PCI
 *   subsystem, which then enumerates the bus behind it.
 *
 *   The sequence is: combo PHY power-up, controller clocks, controller and
 *   PIPE reset release, board PERST# pulse, DesignWare port-logic and DBI
 *   setup, iATU window programming, LTSSM enable, link training wait, and
 *   finally pci_register_controller().
 *
 *   Must be called from board_late_initialize(), after
 *   rk3576_clk_tree_initialize().
 *
 * Input Parameters:
 *   port - RK3576_PCIE_PORT0 or RK3576_PCIE_PORT1
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.  -ENODEV is returned
 *   when the link never trains, which is the normal result for an empty
 *   slot and is not fatal to the rest of the system.
 *
 ****************************************************************************/

int rk3576_pcie_initialize(int port);

/****************************************************************************
 * Name: rk3576_pcie_legacy_irq
 *
 * Description:
 *   Return the GIC interrupt number on which the root port aggregates the
 *   four legacy INTx lines.  Device drivers that cannot use MSI attach a
 *   shared handler to it.
 *
 * Input Parameters:
 *   port - RK3576_PCIE_PORT0 or RK3576_PCIE_PORT1
 *
 * Returned Value:
 *   The IRQ number, or a negated errno value if the port is invalid.
 *
 ****************************************************************************/

int rk3576_pcie_legacy_irq(int port);

/****************************************************************************
 * Name: rk3576_pcie_board_perst
 *
 * Description:
 *   Drive the board's PERST# (reset-gpios) line for one root port.  The
 *   chip driver provides a do-nothing weak default; a board that wires a
 *   GPIO to the slot overrides it.  On the KICKPI-K7 port 0 uses the GPIO
 *   named by "reset-gpios" in the vendor device tree.
 *
 * Input Parameters:
 *   port    - RK3576_PCIE_PORT0 or RK3576_PCIE_PORT1
 *   asserted - true drives PERST# low (device held in reset), false
 *              releases it
 *
 ****************************************************************************/

void rk3576_pcie_board_perst(int port, bool asserted);

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_PCIE_H */
