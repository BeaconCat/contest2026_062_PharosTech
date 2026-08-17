/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_station.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_STATION_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_STATION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "include/sv6621.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_station_mgmt_type_e
{
  SV6621_STATION_MGMT_AUTH = 0,
  SV6621_STATION_MGMT_ASSOC,
  SV6621_STATION_MGMT_DEAUTH,
  SV6621_STATION_MGMT_DISASSOC
};

struct sv6621_station_mgmt_s
{
  enum sv6621_station_mgmt_type_e type;
  uint8_t channel;
  enum sv6621_band_e band;
  int16_t signal_dbm;
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint16_t algorithm;
  uint16_t transaction;
  uint16_t status;
  uint16_t reason;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_station_parse_mgmt(FAR const uint8_t *payload, size_t length,
                              FAR struct sv6621_station_mgmt_s *event);
int sv6621_station_parse_disconnect(
    FAR const uint8_t *payload, size_t length,
    uint8_t bssid[SV6621_MAC_LENGTH], FAR uint16_t *reason);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_STATION_H */
