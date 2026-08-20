/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_beacon.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_BEACON_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_BEACON_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_BEACON_HEAD_MAX_LENGTH     128
#define SV6621_AP_BEACON_TAIL_MAX_LENGTH     768
#define SV6621_AP_PROBE_RESPONSE_MAX_LENGTH 1024

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_ap_beacon_config_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t hidden_ssid;
  uint8_t channel;
  enum sv6621_band_e band;
  uint16_t beacon_interval;
  bool privacy;
  FAR const uint8_t *extra_ies;
  size_t extra_ies_length;
};

struct sv6621_ap_beacon_templates_s
{
  uint8_t beacon_head[SV6621_AP_BEACON_HEAD_MAX_LENGTH];
  size_t beacon_head_length;
  uint8_t beacon_tail[SV6621_AP_BEACON_TAIL_MAX_LENGTH];
  size_t beacon_tail_length;
  uint8_t probe_response[SV6621_AP_PROBE_RESPONSE_MAX_LENGTH];
  size_t probe_response_length;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_build_beacon_templates(
    FAR const struct sv6621_ap_beacon_config_s *config,
    FAR struct sv6621_ap_beacon_templates_s *templates);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_BEACON_H */
