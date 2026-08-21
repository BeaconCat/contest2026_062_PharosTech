/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_mlme.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_MLME_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_MLME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_ap_peer.h"
#include "sv6621_command.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_ap_mgmt_type_e
{
  SV6621_AP_MGMT_AUTH,
  SV6621_AP_MGMT_ASSOC_REQUEST,
  SV6621_AP_MGMT_REASSOC_REQUEST,
  SV6621_AP_MGMT_PROBE_REQUEST,
  SV6621_AP_MGMT_DEAUTH,
  SV6621_AP_MGMT_DISASSOC,
  SV6621_AP_MGMT_ACTION
};

struct sv6621_ap_mgmt_s
{
  enum sv6621_ap_mgmt_type_e type;
  uint8_t channel;
  enum sv6621_band_e band;
  int16_t signal_dbm;
  uint8_t destination[SV6621_MAC_LENGTH];
  uint8_t source[SV6621_MAC_LENGTH];
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint16_t capability;
  uint16_t listen_interval;
  uint16_t algorithm;
  uint16_t transaction;
  uint16_t status;
  uint16_t reason;
  FAR const uint8_t *information_elements;
  size_t information_element_length;
  FAR const uint8_t *frame;
  size_t frame_length;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_parse_mgmt(FAR const uint8_t *payload, size_t payload_length,
                         FAR struct sv6621_ap_mgmt_s *event);
int sv6621_ap_authenticate_open(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    uint8_t channel, enum sv6621_band_e band,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH],
    FAR const struct sv6621_ap_mgmt_s *request, uint64_t cookie,
    FAR bool *accepted);
int sv6621_ap_validate_association(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH], FAR const uint8_t *ssid,
    size_t ssid_length, FAR const struct sv6621_ap_mgmt_s *request,
    FAR uint16_t *status);
int sv6621_ap_respond_association(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    uint8_t channel, enum sv6621_band_e band,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH], FAR const uint8_t *ssid,
    size_t ssid_length, FAR const uint8_t *response_ies,
    size_t response_ies_length, FAR const struct sv6621_ap_mgmt_s *request,
    uint64_t cookie, FAR bool *accepted);
int sv6621_ap_handle_tx_status(FAR struct sv6621_ap_peer_table_s *peers,
                               FAR struct sv6621_command_engine_s *command,
                               uint8_t instance, FAR const uint8_t *payload,
                               size_t payload_length);
int sv6621_ap_handle_departure(FAR struct sv6621_ap_peer_table_s *peers,
                               FAR struct sv6621_command_engine_s *command,
                               uint8_t instance,
                               FAR const struct sv6621_ap_mgmt_s *event);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_MLME_H */
