/****************************************************************************
 * chips/rk3576/sv6621.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

#include "sv6621_transport.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Service bits reported by sv6621_state(). */

#define SV6621_STATE_BSP  (1 << 0) /* CP BSP up (trunk_W seen) */
#define SV6621_STATE_WIFI (1 << 1) /* WIFIREADY received */
#define SV6621_STATE_BT   (1 << 2) /* BTREADY received */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Platform integration supplies the SDIO transport, module power control,
 *
 * and CP firmware images.  None of these objects may be released while the
 *
 * driver is running.
 */

struct sv6621_config_s
{
  /* Drive the module power/reset environment.  on=false asserts the
   * module reset (WL_REG_ON low); on=true releases it.  The board is
   * responsible for any BT-side companion pins.
   */

  FAR struct sv6621_transport_s *transport;
  sv6621_power_t power;
  FAR void *power_arg;

  /* CP firmware images (SWT6621S IRAM = code, DRAM = data). */

  const uint8_t *iram;
  int iram_len;
  const uint8_t *dram;
  int dram_len;
  const uint8_t *nv; /* NV common config, patched into the IRAM
                      * image NV slot before download */
  int nv_len;
  const uint8_t *calib; /* RF calibration blob (PHY_BB_CFG payload) */
  int calib_len;
};

/* One scan result. */

struct sv6621_bss_s
{
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t ssid_len;
  uint8_t channel;
  uint8_t band; /* 0 = 2.4 GHz, 1 = 5 GHz */
  int16_t rssi;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: sv6621_initialize
 *
 * Description:
 *   Bring up the SeekWave SV6621 (SWT6621-S) WiFi/BT combo through the
 *
 *supplied transport, download and start the CP firmware, then start the
 *
 *WiFi service and its receive worker.
 * Input Parameters: config - Transport,
 *platform callbacks and firmware images.
 * Returned Value: OK on success
 *(WiFi service ready); a negated errno otherwise.
 *
 ****************************************************************************/

int sv6621_initialize(FAR const struct sv6621_config_s *config);

/****************************************************************************
 * Name: sv6621_state
 *
 * Description:
 *   Report the CP service state as a bitmask of SV6621_STATE_*.
 *
 ****************************************************************************/

uint32_t sv6621_state(void);

/****************************************************************************
 * Name: sv6621_scan
 *
 * Description:
 *   Run an active scan and copy up to max results into list.  Returns the
 *   number of BSSes found, or a negated errno.
 *
 ****************************************************************************/

int sv6621_scan(FAR struct sv6621_bss_s *list, int max);

/****************************************************************************
 * Name: sv6621_connect
 *
 * Description:
 *   Associate with the scanned BSS matching ssid (L2 association only:
 *   JOIN/AUTH-open/ASSOC).  Returns OK on ASSOCIATED, or a negated errno.
 *
 ****************************************************************************/

int sv6621_connect(FAR const char *ssid);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_H */
