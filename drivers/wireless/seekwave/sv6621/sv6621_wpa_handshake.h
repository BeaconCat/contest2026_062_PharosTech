/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_handshake.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_HANDSHAKE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_HANDSHAKE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/wqueue.h>

#include "include/sv6621.h"
#include "sv6621_connection.h"
#include "sv6621_data.h"
#include "sv6621_security.h"
#include "sv6621_station.h"
#include "sv6621_wpa_crypto.h"
#include "sv6621_wpa_eapol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_FRAME_CAPACITY 640

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_wpa_state_e
{
  SV6621_WPA_IDLE = 0,
  SV6621_WPA_WAIT_MESSAGE_1,
  SV6621_WPA_WAIT_MESSAGE_3,
  SV6621_WPA_COMPLETE,
  SV6621_WPA_FAILED
};

struct sv6621_wpa_s
{
  mutex_t lock;
  sem_t completion;
  struct work_s work;
  struct work_s rekey_timeout_work;
  FAR struct sv6621_command_engine_s *command;
  FAR struct sv6621_station_s *station;
  enum sv6621_wpa_state_e state;
  struct sv6621_data_tx_context_s tx_context;
  uint8_t supplicant[SV6621_MAC_LENGTH];
  uint8_t authenticator[SV6621_MAC_LENGTH];
  uint8_t pmk[SV6621_WPA_PMK_SIZE];
  uint8_t ptk[SV6621_WPA_PTK_SIZE];
  uint8_t anonce[SV6621_WPA_NONCE_SIZE];
  uint8_t snonce[SV6621_WPA_NONCE_SIZE];
  uint8_t replay[SV6621_WPA_REPLAY_SIZE];
  uint8_t gtk[SV6621_WPA_GTK_MAX_SIZE];
  uint8_t frame[SV6621_WPA_FRAME_CAPACITY];
  size_t frame_length;
  size_t gtk_length;
  int result;
  uint8_t eapol_version;
  uint8_t gtk_index;
  bool peer_ready;
  bool frame_pending;
  bool work_scheduled;
  bool canceling;
  bool replay_valid;
  bool rekeying;
  bool pairwise_installed;
  bool group_installed;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_wpa_init(FAR struct sv6621_wpa_s *wpa,
                     FAR struct sv6621_command_engine_s *command,
                     FAR struct sv6621_station_s *station);
void sv6621_wpa_deinit(FAR struct sv6621_wpa_s *wpa);
int sv6621_wpa_prepare(FAR struct sv6621_wpa_s *wpa,
                        FAR const struct sv6621_connect_s *request,
                        FAR const uint8_t supplicant[SV6621_MAC_LENGTH],
                        FAR const uint8_t authenticator[SV6621_MAC_LENGTH]);
int sv6621_wpa_run(FAR struct sv6621_wpa_s *wpa,
                    FAR const struct sv6621_connection_peer_s *peer,
                    uint32_t timeout_ms);
void sv6621_wpa_cancel(FAR struct sv6621_wpa_s *wpa, int result);
void sv6621_wpa_input(FAR const struct sv6621_data_rx_s *rx, FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_HANDSHAKE_H */
