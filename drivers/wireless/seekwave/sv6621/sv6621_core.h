/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_core.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include "include/sv6621.h"
#include "sv6621_command.h"
#include "sv6621_data.h"
#include "sv6621_network.h"
#include "sv6621_packet.h"
#include "sv6621_regulatory.h"
#include "sv6621_rx.h"
#include "sv6621_scan.h"
#include "sv6621_service.h"
#include "sv6621_station.h"
#include "sv6621_tx.h"
#include "sv6621_wifi.h"
#include "sv6621_wpa_handshake.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s
{
  struct sv6621_config_s config;
  mutex_t lifecycle_lock;
  mutex_t status_lock;
  struct sv6621_status_s status;
  struct sv6621_packet_router_s router;
  struct sv6621_tx_s tx;
  struct sv6621_data_s data;
#ifdef CONFIG_NET
  struct sv6621_network_s network;
#endif
  struct sv6621_command_engine_s command;
  struct sv6621_scan_s scan;
  struct sv6621_station_s station;
  struct sv6621_wpa_s wpa;
  struct sv6621_service_s service;
  struct sv6621_rx_s rx;
  struct sv6621_wifi_info_s wifi_info;
  struct sv6621_scan_channel_s
      scan_channels[SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY];
  size_t scan_channel_count;
  struct work_s event_work;
  struct work_s scan_work;
  struct work_s station_work;
  int scan_result;
  uint16_t station_reason;
  bool station_connected;
  bool scan_reporting;
  bool powered;
  bool transport_open;
  bool station_open;
};

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CORE_H */
