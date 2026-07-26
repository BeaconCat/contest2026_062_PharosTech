/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_gmac.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_GMAC

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_gmac_initialize
 *
 * Description:
 *   Initialise one RK3576 gigabit Ethernet controller and register it with
 *   the network stack as "ethN".  The board logic must have muxed the RGMII
 *   and MDIO pins and released the PHY from reset before calling this.
 *
 * Input Parameters:
 *   intf - Controller index, 0 for gmac0 (0x2A220000) or 1 for gmac1
 *          (0x2A230000).
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_gmac_initialize(int intf);

/****************************************************************************
 * Name: rk3576_gmac_mdio_read
 *
 * Description:
 *   Perform a clause-22 MDIO read on the management bus of one controller.
 *   Exposed so that board logic can probe or quirk its PHY.
 *
 * Input Parameters:
 *   intf   - Controller index (0 or 1)
 *   phyad  - PHY address on the MDIO bus (0..31)
 *   regad  - PHY register address (0..31)
 *   value  - Location to return the 16-bit register contents
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_gmac_mdio_read(int intf, uint8_t phyad, uint8_t regad,
                          uint16_t *value);

/****************************************************************************
 * Name: rk3576_gmac_mdio_write
 *
 * Description:
 *   Perform a clause-22 MDIO write on the management bus of one controller.
 *
 * Input Parameters:
 *   intf  - Controller index (0 or 1)
 *   phyad - PHY address on the MDIO bus (0..31)
 *   regad - PHY register address (0..31)
 *   value - 16-bit value to write
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_gmac_mdio_write(int intf, uint8_t phyad, uint8_t regad,
                           uint16_t value);

/****************************************************************************
 * Name: rk3576_gmac_board_phy_reset
 *
 * Description:
 *   Board hook that toggles the PHY hardware reset line of one controller.
 *   A weak default that does nothing is provided by the driver, so a board
 *   without a reset GPIO needs no implementation.  On the KICKPI-K7 the
 *   vendor DTS wires gmac0 to GPIO2_B5 and gmac1 to GPIO3_A3, both active
 *   low, with a 20 ms assert and a 100 ms post-release delay.
 *
 * Input Parameters:
 *   intf - Controller index (0 or 1)
 *
 * Returned Value:
 *   None.
 *
 ****************************************************************************/

void rk3576_gmac_board_phy_reset(int intf);

/****************************************************************************
 * Name: rk3576_gmac_get_macaddr
 *
 * Description:
 *   Obtain the six-byte station address for one controller.  The driver
 *   provides a weak default that derives a stable locally administered
 *   address from the SoC serial number when an OTP driver is available and
 *   falls back to a fixed locally administered address otherwise.  Boards
 *   that store their address elsewhere may override this function.
 *
 * Input Parameters:
 *   intf - Controller index (0 or 1)
 *   mac  - Six-byte buffer that receives the address
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_gmac_get_macaddr(int intf, uint8_t *mac);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_RK3576_GMAC */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_GMAC_H */
