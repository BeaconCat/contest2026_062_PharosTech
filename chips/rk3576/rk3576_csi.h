/****************************************************************************
 * chips/rk3576/rk3576_csi.h
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
 * Public interface of the RK3576 MIPI CSI-2 host controller driver.
 *
 * NuttX has no generic CSI-2 receiver framework (include/nuttx/video only
 * covers the capture/ISP end of the pipeline, struct imgsensor_ops_s and
 * struct imgdata_ops_s), so this block exposes plain in-kernel functions.
 * A future VICAP/ISP capture driver implementing video/v4l2 will call these
 * to bring the link up before starting a stream.
 *
 * Bring-up order for one camera path:
 *
 *   1. board: power the sensor rail and its D-PHY supply, mux the I2C pins
 *   2. rk3576_dphy_initialize(phy, lane_rate_mbps, lanes)
 *   3. rk3576_csi_initialize(csi, &cfg)
 *   4. sensor: program registers over I2C and start streaming
 *   5. rk3576_csi_start(csi)
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_CSI0 0
#define RK3576_CSI1 1
#define RK3576_CSI2 2
#define RK3576_CSI3 3
#define RK3576_CSI4 4

/* Maximum number of virtual-channel/data-type pairs the controller can
 * route (CSI2_DATA_IDS_1 and CSI2_DATA_IDS_2, four slots each).
 */

#define RK3576_CSI_MAX_STREAMS 8

/* Maximum number of data lanes accepted by a host controller. */

#define RK3576_CSI_MAX_LANES 4

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One virtual channel / data type pair to accept on the link. */

struct rk3576_csi_stream_s
{
  uint8_t vc; /* Virtual channel, 0..3                              */
  uint8_t dt; /* CSI-2 data type, e.g. RK3576_CSI2_DT_RAW10         */
};

/* Configuration of one CSI-2 host controller. */

struct rk3576_csi_config_s
{
  int num_lanes; /* Active data lanes, 1..4                        */
  int phy_id;    /* D-PHY feeding this host, RK3576_DPHY0/1        */
  int nstreams;  /* Number of valid entries in streams[]           */
  struct rk3576_csi_stream_s streams[RK3576_CSI_MAX_STREAMS];
};

/* Error counters maintained by the interrupt handler. */

struct rk3576_csi_stats_s
{
  uint32_t phy_fatal;   /* Lane in an unrecoverable state             */
  uint32_t pkt_fatal;   /* Packet header ECC failure (uncorrectable)  */
  uint32_t frame_fatal; /* Frame boundary / sequence error            */
  uint32_t phy_err;     /* Recoverable PHY errors (SoT, ESC)          */
  uint32_t pkt_err;     /* Corrected ECC, payload CRC                 */
  uint32_t line_err;    /* Line boundary error                        */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_csi_initialize
 *
 * Description:
 *   Enable the clocks of one MIPI CSI-2 host controller, hold it in reset
 *   and program the lane count and the VC/DT routing table.  The D-PHY in
 *   front of the controller must already have been brought up with
 *   rk3576_dphy_initialize().  Reception stays off until rk3576_csi_start().
 *
 * Input Parameters:
 *   csi_id - RK3576_CSI0 .. RK3576_CSI4
 *   cfg    - Link configuration; copied, the caller may free it
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_initialize(int csi_id, const struct rk3576_csi_config_s *cfg);

/****************************************************************************
 * Name: rk3576_csi_start
 *
 * Description:
 *   Release the controller from reset, unmask the error interrupts and
 *   begin accepting packets.  The sensor should already be streaming so
 *   that the PHY sees a valid HS clock.
 *
 * Input Parameters:
 *   csi_id - RK3576_CSI0 .. RK3576_CSI4
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_start(int csi_id);

/****************************************************************************
 * Name: rk3576_csi_stop
 *
 * Description:
 *   Mask all interrupts and hold the controller in reset.  The D-PHY is
 *   left untouched so the same link can be restarted cheaply.
 *
 * Input Parameters:
 *   csi_id - RK3576_CSI0 .. RK3576_CSI4
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_stop(int csi_id);

/****************************************************************************
 * Name: rk3576_csi_get_stats
 *
 * Description:
 *   Copy out the link error counters accumulated since initialization.
 *
 * Input Parameters:
 *   csi_id - RK3576_CSI0 .. RK3576_CSI4
 *   stats  - Destination structure
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_csi_get_stats(int csi_id, struct rk3576_csi_stats_s *stats);

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_CSI_H */
