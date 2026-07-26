/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_dphy.h
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
 * Public interface of the RK3576 MIPI RX D-PHY driver.  The D-PHY is the
 * analog/PPI front end sitting in front of a MIPI CSI-2 host controller; it
 * must be brought up (lanes enabled, HS-SETTLE programmed for the sensor's
 * lane rate) before the host controller is released from reset.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_DPHY_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_DPHY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_DPHY0 0
#define RK3576_DPHY1 1

/* Maximum number of data lanes on a RX D-PHY. */

#define RK3576_DPHY_MAX_LANES 4

/* Lane rate limits of the RK3576 RX D-PHY, in Mbps per lane. */

#define RK3576_DPHY_MIN_LANE_RATE_MBPS 80
#define RK3576_DPHY_MAX_LANE_RATE_MBPS 2500

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_dphy_initialize
 *
 * Description:
 *   Power up and configure one MIPI RX D-PHY: enable its APB clock, select
 *   CSI mode in the GRF, enable the clock lane plus num_lanes data lanes and
 *   program the HS-SETTLE counters derived from lane_rate_mbps.  The PHY is
 *   left running so that the CSI-2 host controller can be released next.
 *
 * Input Parameters:
 *   phy_id         - RK3576_DPHY0 or RK3576_DPHY1
 *   lane_rate_mbps - Per-lane HS bit rate of the attached sensor, in Mbps
 *   num_lanes      - Number of active data lanes (1..4)
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dphy_initialize(int phy_id, uint32_t lane_rate_mbps,
                           int num_lanes);

/****************************************************************************
 * Name: rk3576_dphy_uninitialize
 *
 * Description:
 *   Disable all lanes of a D-PHY and hold its digital core in reset.
 *
 * Input Parameters:
 *   phy_id - RK3576_DPHY0 or RK3576_DPHY1
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_dphy_uninitialize(int phy_id);

/****************************************************************************
 * Name: rk3576_dphy_lane_rate_mbps
 *
 * Description:
 *   Helper that computes the per-lane MIPI HS bit rate required to carry a
 *   given pixel stream:
 *
 *     lane_rate = pixel_rate * bits_per_pixel / num_lanes
 *
 *   where pixel_rate = width * height * fps (all active pixels, blanking
 *   excluded - add the usual 10..15% margin in the caller if the sensor
 *   datasheet does not give the link frequency directly).
 *
 * Input Parameters:
 *   pixel_rate - Pixels per second delivered by the sensor
 *   bpp        - Bits per pixel on the link (10 for RAW10, 16 for YUV422-8)
 *   num_lanes  - Number of active data lanes (1..4)
 *
 * Returned Value:
 *   Per-lane rate in Mbps, or 0 if the arguments are invalid.
 *
 ****************************************************************************/

uint32_t rk3576_dphy_lane_rate_mbps(uint64_t pixel_rate, uint32_t bpp,
                                    int num_lanes);

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_DPHY_H */
