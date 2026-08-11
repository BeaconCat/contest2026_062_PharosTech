/****************************************************************************
 * chips/rk3576/rk3576_skw.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Service bits reported by rk3576_skw_state(). */

#define RK3576_SKW_STATE_BSP    (1 << 0)  /* CP BSP up (trunk_W seen) */
#define RK3576_SKW_STATE_WIFI   (1 << 1)  /* WIFIREADY received */
#define RK3576_SKW_STATE_BT     (1 << 2)  /* BTREADY received */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Board integration: the board supplies module power control (WL_REG_ON
 * plus any companion pin environment) and the CP firmware images.
 */

struct rk3576_skw_board_s
{
  /* Drive the module power/reset environment.  on=false asserts the
   * module reset (WL_REG_ON low); on=true releases it.  The board is
   * responsible for any BT-side companion pins.
   */

  void (*power)(bool on);

  /* CP firmware images (SWT6621S IRAM = code, DRAM = data). */

  const uint8_t *iram;
  int            iram_len;
  const uint8_t *dram;
  int            dram_len;
  const uint8_t *nv;       /* NV common config, patched into the IRAM
                            * image NV slot before download */
  int            nv_len;
  const uint8_t *calib;    /* RF calibration blob (PHY_BB_CFG payload) */
  int            calib_len;
};

/* One scan result. */

struct rk3576_skw_bss_s
{
  uint8_t  bssid[6];
  uint8_t  ssid[33];
  uint8_t  ssid_len;
  uint8_t  channel;
  uint8_t  band;     /* 0 = 2.4 GHz, 1 = 5 GHz */
  int16_t  rssi;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_skw_initialize
 *
 * Description:
 *   Bring up the SeekWave SV6621 (SWT6621-S) WiFi/BT combo on the RK3576
 *   SDIO host (mmc@2a320000): power sequence, SD-IO enumeration with the
 *   1.8 V signaling handshake, SDR104 selection, CP firmware download and
 *   boot, and the WiFi service start.  Spawns the receive thread that
 *   services the CP packet channels.
 *
 * Input Parameters:
 *   board - Board integration callbacks and firmware images.  The pointer
 *           must remain valid for the lifetime of the driver.
 *
 * Returned Value:
 *   OK on success (WiFi service ready); a negated errno otherwise.
 *
 ****************************************************************************/

int rk3576_skw_initialize(const struct rk3576_skw_board_s *board);

/****************************************************************************
 * Name: rk3576_skw_state
 *
 * Description:
 *   Report the CP service state as a bitmask of RK3576_SKW_STATE_*.
 *
 ****************************************************************************/

uint32_t rk3576_skw_state(void);

/****************************************************************************
 * Name: rk3576_skw_scan
 *
 * Description:
 *   Run an active scan and copy up to max results into list.  Returns the
 *   number of BSSes found, or a negated errno.
 *
 ****************************************************************************/

int rk3576_skw_scan(FAR struct rk3576_skw_bss_s *list, int max);

/****************************************************************************
 * Name: rk3576_skw_connect
 *
 * Description:
 *   Associate with the scanned BSS matching ssid (L2 association only:
 *   JOIN/AUTH-open/ASSOC).  Returns OK on ASSOCIATED, or a negated errno.
 *
 ****************************************************************************/

int rk3576_skw_connect(FAR const char *ssid);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_H */
