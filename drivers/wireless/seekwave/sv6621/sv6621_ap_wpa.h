/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_wpa.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_WPA_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_WPA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include <stdbool.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_ap_peer.h"
#include "sv6621_command.h"
#include "sv6621_data.h"
#include "sv6621_wpa_crypto.h"
#include "sv6621_wpa_eapol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_WPA_PEER_CAPACITY SV6621_AP_PEER_CAPACITY

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*sv6621_ap_wpa_error_t)(int error, FAR void *arg);

enum sv6621_ap_wpa_state_e
{
  SV6621_AP_WPA_IDLE = 0,
  SV6621_AP_WPA_WAIT_MESSAGE_2,
  SV6621_AP_WPA_WAIT_MESSAGE_4,
  SV6621_AP_WPA_COMPLETE
};

struct sv6621_ap_wpa_peer_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  uint8_t anonce[SV6621_WPA_NONCE_SIZE];
  uint8_t ptk[SV6621_WPA_PTK_SIZE];
  uint8_t replay[SV6621_WPA_REPLAY_SIZE];
  uint8_t peer_index;
  enum sv6621_ap_wpa_state_e state;
  bool group_rekey_pending;
};

struct sv6621_ap_wpa_s
{
  mutex_t lock;
  struct work_s rekey_timeout_work;
  FAR struct sv6621_command_engine_s *command;
  struct sv6621_ap_wpa_peer_s peers[SV6621_AP_WPA_PEER_CAPACITY];
  uint8_t authenticator[SV6621_MAC_LENGTH];
  uint8_t pmk[SV6621_WPA_PMK_SIZE];
  uint8_t gtk[16];
  uint8_t previous_gtk[16];
  uint8_t lmac_id;
  uint8_t instance;
  uint8_t multicast_index;
  uint8_t gtk_index;
  uint8_t previous_gtk_index;
  uint8_t group_rekey_retries;
  sv6621_ap_wpa_error_t error;
  FAR void *error_arg;
  bool group_rekey_active;
  bool enabled;
};

struct sv6621_ap_context_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_wpa_init(FAR struct sv6621_ap_wpa_s *wpa,
                        FAR struct sv6621_command_engine_s *command,
                        FAR const uint8_t address[SV6621_MAC_LENGTH],
                        sv6621_ap_wpa_error_t error, FAR void *error_arg);
void sv6621_ap_wpa_deinit(FAR struct sv6621_ap_wpa_s *wpa);
int sv6621_ap_wpa_enable(FAR struct sv6621_ap_wpa_s *wpa,
                          FAR const struct sv6621_ap_config_s *config,
                          FAR const struct sv6621_ap_context_s *context);
void sv6621_ap_wpa_disable(FAR struct sv6621_ap_wpa_s *wpa);
int sv6621_ap_wpa_begin(FAR struct sv6621_ap_wpa_s *wpa,
                         FAR const struct sv6621_ap_peer_s *peer);
int sv6621_ap_wpa_rekey(FAR struct sv6621_ap_wpa_s *wpa);
void sv6621_ap_wpa_forget(
    FAR struct sv6621_ap_wpa_s *wpa,
    FAR const uint8_t address[SV6621_MAC_LENGTH]);
int sv6621_ap_wpa_input(FAR struct sv6621_ap_wpa_s *wpa,
                         FAR const struct sv6621_data_rx_s *rx,
                         FAR bool *authorized,
                         FAR uint8_t address[SV6621_MAC_LENGTH]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_WPA_H */
