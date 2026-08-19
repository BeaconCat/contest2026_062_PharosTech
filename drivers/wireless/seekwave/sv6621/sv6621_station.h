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
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/wqueue.h>

#include "include/sv6621.h"
#include "sv6621_command.h"
#include "sv6621_connection.h"
#include "sv6621_scan.h"
#include "sv6621_sae.h"

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
  FAR const uint8_t *frame;
  size_t frame_length;
};

enum sv6621_station_state_e
{
  SV6621_STATION_IDLE = 0,
  SV6621_STATION_JOINING,
  SV6621_STATION_AUTHENTICATING,
  SV6621_STATION_ASSOCIATING,
  SV6621_STATION_ASSOCIATED,
  SV6621_STATION_CONNECTED,
  SV6621_STATION_DISCONNECTING
};

typedef void (*sv6621_station_event_t)(bool connected, bool remote,
                                       uint16_t reason, FAR void *arg);

struct sv6621_station_s
{
  mutex_t connect_lock;
  mutex_t lock;
  sem_t completion;
  struct work_s association_work;
  FAR struct sv6621_command_engine_s *command;
  FAR struct sv6621_scan_s *scan;
  sv6621_station_event_t event;
  FAR void *event_arg;
  enum sv6621_station_state_e state;
  struct sv6621_scan_entry_s target;
  struct sv6621_connect_s request;
  struct sv6621_connection_peer_s peer;
  struct sv6621_sae_s sae;
  uint32_t bandwidth_capabilities;
  uint8_t ht_capability[SV6621_CONNECTION_HT_CAPABILITY_SIZE];
  uint8_t vht_capability[SV6621_CONNECTION_VHT_CAPABILITY_SIZE];
  uint8_t association_ies[SV6621_CONNECTION_ASSOC_IE_CAPACITY];
  size_t association_ie_length;
  uint8_t local_address[SV6621_MAC_LENGTH];
  uint8_t sae_pmk[SV6621_SAE_PMK_SIZE];
  uint8_t sae_pmkid[SV6621_SAE_PMKID_SIZE];
  int result;
  bool local_address_valid;
  bool sae_pmk_valid;
  bool shutting_down;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_station_parse_mgmt(FAR const uint8_t *payload, size_t length,
                              FAR struct sv6621_station_mgmt_s *event);
int sv6621_station_parse_disconnect(
    FAR const uint8_t *payload, size_t length,
    uint8_t bssid[SV6621_MAC_LENGTH], FAR uint16_t *reason);
int sv6621_station_init(FAR struct sv6621_station_s *station,
                        FAR struct sv6621_command_engine_s *command,
                        FAR struct sv6621_scan_s *scan,
                        sv6621_station_event_t event, FAR void *event_arg);
int sv6621_station_configure_ht(FAR struct sv6621_station_s *station,
                                uint16_t capabilities,
                                uint16_t extended_capabilities,
                                uint16_t ampdu_parameters,
                                uint32_t tx_mcs, uint32_t rx_mcs);
int sv6621_station_configure_vht(FAR struct sv6621_station_s *station,
                                 uint32_t capabilities,
                                 uint16_t tx_mcs, uint16_t rx_mcs);
int sv6621_station_configure_bandwidth(FAR struct sv6621_station_s *station,
                                       uint32_t capabilities);
int sv6621_station_set_local_address(
    FAR struct sv6621_station_s *station,
    FAR const uint8_t address[SV6621_MAC_LENGTH]);
void sv6621_station_deinit(FAR struct sv6621_station_s *station);
int sv6621_station_connect(FAR struct sv6621_station_s *station,
                           FAR const struct sv6621_connect_s *request,
                           uint32_t timeout_ms);
int sv6621_station_disconnect(FAR struct sv6621_station_s *station,
                              uint16_t reason);
int sv6621_station_mark_connected(FAR struct sv6621_station_s *station);
int sv6621_station_get_sae_pmk(
    FAR struct sv6621_station_s *station,
    uint8_t pmk[SV6621_SAE_PMK_SIZE],
    uint8_t pmkid[SV6621_SAE_PMKID_SIZE]);
void sv6621_station_reset(FAR struct sv6621_station_s *station, int result);
void sv6621_station_command_event(uint8_t instance, uint8_t id,
                                  FAR const uint8_t *payload, size_t length,
                                  FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_STATION_H */
