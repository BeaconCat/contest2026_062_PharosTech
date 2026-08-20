/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_ap_beacon.h"
#include "sv6621_ap_event.h"
#include "sv6621_ap_peer.h"
#include "sv6621_command.h"
#include "sv6621_data.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*sv6621_ap_error_t)(int error, FAR void *arg);
typedef void (*sv6621_ap_client_t)(
    bool connected, FAR const struct sv6621_ap_client_event_s *event,
    FAR void *arg);

struct sv6621_ap_blob_s
{
  FAR const uint8_t *data;
  size_t length;
};

struct sv6621_ap_start_s
{
  uint32_t beacon_interval;
  uint8_t dtim_period;
  uint8_t hidden_ssid;
  uint8_t channel;
  enum sv6621_channel_width_e channel_width;
  uint8_t center_channel1;
  uint8_t center_channel2;
  enum sv6621_band_e band;
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  struct sv6621_ap_blob_s beacon_head;
  struct sv6621_ap_blob_s beacon_tail;
  struct sv6621_ap_blob_s beacon_ies;
  struct sv6621_ap_blob_s probe_response_ies;
  struct sv6621_ap_blob_s association_response_ies;
};

struct sv6621_ap_context_s
{
  uint8_t lmac_id;
  uint8_t instance;
  uint8_t multicast_index;
};

struct sv6621_ap_s
{
  mutex_t lock;
  FAR struct sv6621_command_engine_s *command;
  struct sv6621_ap_event_queue_s events;
  struct sv6621_ap_peer_table_s peers;
  struct sv6621_ap_beacon_templates_s templates;
  struct sv6621_ap_config_s config;
  struct sv6621_ap_context_s context;
  uint8_t address[SV6621_MAC_LENGTH];
  uint64_t next_cookie;
  sv6621_ap_error_t error;
  FAR void *error_arg;
  sv6621_ap_client_t client;
  FAR void *client_arg;
  uint8_t instance;
  bool active;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_encode_start(FAR const struct sv6621_ap_start_s *config,
                           FAR uint8_t *payload, size_t capacity,
                           FAR size_t *written);
int sv6621_ap_start(FAR struct sv6621_command_engine_s *command,
                    uint8_t instance,
                    FAR const struct sv6621_ap_start_s *config,
                    FAR struct sv6621_ap_context_s *context);
int sv6621_ap_stop(FAR struct sv6621_command_engine_s *command,
                   uint8_t instance);
int sv6621_ap_init(FAR struct sv6621_ap_s *ap,
                   FAR struct sv6621_command_engine_s *command,
                   uint8_t max_stations,
                   FAR const uint8_t address[SV6621_MAC_LENGTH],
                   sv6621_ap_error_t error, FAR void *error_arg,
                   sv6621_ap_client_t client, FAR void *client_arg);
void sv6621_ap_deinit(FAR struct sv6621_ap_s *ap);
int sv6621_ap_enable(FAR struct sv6621_ap_s *ap, uint8_t instance,
                     FAR const struct sv6621_ap_config_s *config);
int sv6621_ap_disable(FAR struct sv6621_ap_s *ap);
int sv6621_ap_reset(FAR struct sv6621_ap_s *ap);
int sv6621_ap_queue_event(FAR struct sv6621_ap_s *ap, uint8_t instance,
                          uint8_t id, FAR const uint8_t *payload,
                          size_t length);
int sv6621_ap_resolve_tx(
    FAR struct sv6621_ap_s *ap, FAR const uint8_t *frame, size_t length,
    FAR struct sv6621_data_tx_context_s *context);
int sv6621_ap_validate_rx(FAR struct sv6621_ap_s *ap,
                          FAR const struct sv6621_data_rx_s *rx);
int sv6621_ap_forward_policy(FAR struct sv6621_ap_s *ap,
                             FAR const struct sv6621_data_rx_s *rx,
                             FAR bool *forward, FAR bool *deliver_local);
bool sv6621_ap_is_active(FAR struct sv6621_ap_s *ap);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_H */
