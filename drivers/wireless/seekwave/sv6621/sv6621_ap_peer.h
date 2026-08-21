/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_peer.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_PEER_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_PEER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_command.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_PEER_CAPACITY 32

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_ap_peer_state_e
{
  SV6621_AP_PEER_FREE,
  SV6621_AP_PEER_AUTHENTICATED,
  SV6621_AP_PEER_ASSOCIATING,
  SV6621_AP_PEER_ASSOCIATED,
  SV6621_AP_PEER_AUTHORIZED
};

struct sv6621_ap_peer_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  enum sv6621_ap_peer_state_e state;
  uint8_t peer_index;
  uint16_t aid;
  uint16_t capability;
  enum sv6621_ap_peer_state_e previous_state;
  uint16_t previous_aid;
  uint16_t previous_capability;
  uint64_t pending_cookie;
  bool bound;
  bool tx_pending;
};

struct sv6621_ap_peer_table_s
{
  mutex_t lock;
  struct sv6621_ap_peer_s peers[SV6621_AP_PEER_CAPACITY];
  uint16_t next_aid;
  uint8_t capacity;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_add_peer(FAR struct sv6621_command_engine_s *command,
                       uint8_t instance,
                       FAR const uint8_t address[SV6621_MAC_LENGTH],
                       FAR uint8_t *peer_index);
int sv6621_ap_remove_peer(FAR struct sv6621_command_engine_s *command,
                          uint8_t instance,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          uint16_t reason, bool transmit_frame);
int sv6621_ap_peer_table_init(FAR struct sv6621_ap_peer_table_s *table,
                              uint8_t capacity);
void sv6621_ap_peer_table_deinit(FAR struct sv6621_ap_peer_table_s *table);
int sv6621_ap_peer_table_reset(FAR struct sv6621_ap_peer_table_s *table);
int sv6621_ap_peer_authenticate(FAR struct sv6621_ap_peer_table_s *table,
                                FAR const uint8_t address[SV6621_MAC_LENGTH]);
int sv6621_ap_peer_bind(FAR struct sv6621_ap_peer_table_s *table,
                        FAR const uint8_t address[SV6621_MAC_LENGTH],
                        uint8_t peer_index);
int sv6621_ap_peer_prepare_association(
    FAR struct sv6621_ap_peer_table_s *table,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint16_t capability,
    FAR uint16_t *aid);
int sv6621_ap_peer_cancel_association(FAR struct sv6621_ap_peer_table_s *table,
                                      FAR const uint8_t
                                          address[SV6621_MAC_LENGTH]);
int sv6621_ap_peer_begin_tx(FAR struct sv6621_ap_peer_table_s *table,
                            FAR const uint8_t address[SV6621_MAC_LENGTH],
                            uint64_t cookie);
int sv6621_ap_peer_cancel_tx(FAR struct sv6621_ap_peer_table_s *table,
                             FAR const uint8_t address[SV6621_MAC_LENGTH],
                             uint64_t cookie);
int sv6621_ap_peer_complete_tx(FAR struct sv6621_ap_peer_table_s *table,
                               FAR const uint8_t address[SV6621_MAC_LENGTH],
                               uint64_t cookie, bool association, bool success,
                               FAR bool *remove);
int sv6621_ap_peer_authorize(FAR struct sv6621_ap_peer_table_s *table,
                             FAR const uint8_t address[SV6621_MAC_LENGTH]);
int sv6621_ap_peer_lookup(FAR struct sv6621_ap_peer_table_s *table,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          FAR struct sv6621_ap_peer_s *peer);
int sv6621_ap_peer_forget(FAR struct sv6621_ap_peer_table_s *table,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          FAR struct sv6621_ap_peer_s *peer);
int sv6621_ap_parse_peer_departure(FAR const uint8_t *payload,
                                   size_t payload_length,
                                   FAR uint8_t address[SV6621_MAC_LENGTH],
                                   FAR uint16_t *reason);
int sv6621_ap_peer_departed(FAR struct sv6621_ap_peer_table_s *table,
                            FAR struct sv6621_command_engine_s *command,
                            uint8_t instance,
                            FAR const uint8_t address[SV6621_MAC_LENGTH],
                            uint16_t reason, bool firmware_event);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_PEER_H */
