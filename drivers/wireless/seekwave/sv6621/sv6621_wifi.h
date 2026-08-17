/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wifi.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WIFI_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WIFI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "include/sv6621.h"
#include "sv6621_command.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WIFI_PRIVATE_PN_REUSE (1 << 2)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_wifi_info_s
{
  uint16_t encryption_capabilities;
  uint32_t chip_model;
  uint32_t chip_version;
  uint32_t firmware_version;
  uint32_t firmware_capabilities;
  uint32_t bandwidth_capabilities;
  uint32_t private_capabilities;
  uint8_t max_stations;
  uint8_t max_multicast_addresses;
  uint8_t max_scan_ssids;
  uint8_t mac[SV6621_MAC_LENGTH];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_wifi_sync_versions(FAR struct sv6621_command_engine_s *command);
int sv6621_wifi_get_info(FAR struct sv6621_command_engine_s *command,
                         FAR const struct sv6621_board_ops_s *board_ops,
                         FAR void *board_arg,
                         FAR struct sv6621_wifi_info_s *info);
int sv6621_wifi_download_calibration(
    FAR struct sv6621_command_engine_s *command, FAR const uint8_t *data,
    size_t length);
int sv6621_wifi_open_station(FAR struct sv6621_command_engine_s *command,
                             FAR const uint8_t address[SV6621_MAC_LENGTH]);
int sv6621_wifi_close_station(FAR struct sv6621_command_engine_s *command);
int sv6621_wifi_set_mib(FAR struct sv6621_command_engine_s *command,
                        uint16_t type, FAR const void *value, uint16_t length);
int sv6621_wifi_configure_baseline(
    FAR struct sv6621_command_engine_s *command);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WIFI_H */
