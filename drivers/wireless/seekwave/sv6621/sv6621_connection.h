/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_connection.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CONNECTION_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CONNECTION_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "sv6621_command.h"
#include "sv6621_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_CONNECTION_HT_CAPABILITY_SIZE  26
#define SV6621_CONNECTION_VHT_CAPABILITY_SIZE 12
#define SV6621_CONNECTION_ASSOC_IE_CAPACITY   128

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_connection_peer_s
{
  uint8_t peer_index;
  uint8_t lmac_id;
  uint8_t instance;
  uint8_t multicast_index;
};

enum sv6621_connection_auth_algorithm_e
{
  SV6621_CONNECTION_AUTH_OPEN = 0,
  SV6621_CONNECTION_AUTH_SHARED_KEY = 1,
  SV6621_CONNECTION_AUTH_FT = 2,
  SV6621_CONNECTION_AUTH_SAE = 3
};

enum sv6621_connection_disconnect_mode_e
{
  SV6621_CONNECTION_DISCONNECT_ONLY = 0,
  SV6621_CONNECTION_DISCONNECT_DISASSOC = 1,
  SV6621_CONNECTION_DISCONNECT_DEAUTH = 2
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_connection_join(FAR struct sv6621_command_engine_s *command,
                           FAR const struct sv6621_scan_entry_s *entry,
                           FAR struct sv6621_connection_peer_s *peer);
int sv6621_connection_authenticate(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_connection_auth_algorithm_e algorithm,
    FAR const uint8_t *auth_data, size_t auth_data_length,
    FAR const uint8_t *auth_ies, size_t auth_ies_length);
int sv6621_connection_associate(
    FAR struct sv6621_command_engine_s *command,
    FAR const uint8_t bssid[SV6621_MAC_LENGTH],
    FAR const uint8_t ht_capability[SV6621_CONNECTION_HT_CAPABILITY_SIZE],
    FAR const uint8_t vht_capability[SV6621_CONNECTION_VHT_CAPABILITY_SIZE],
    FAR const uint8_t *ies, size_t ies_length);
int sv6621_connection_disconnect(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_connection_disconnect_mode_e mode, bool local_state_change,
    uint16_t reason, FAR const uint8_t *ies, size_t ies_length);
int sv6621_connection_build_association_ies(
    FAR const struct sv6621_scan_entry_s *entry,
    enum sv6621_security_e security, FAR uint8_t *ies, size_t capacity,
    FAR size_t *length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_CONNECTION_H */
