/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sched_scan.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H

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
#include "sv6621_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCHED_SCAN_MAX_SSIDS       10
#define SV6621_SCHED_SCAN_MAX_MATCHES     10
#define SV6621_SCHED_SCAN_MAX_PLANS       4
#define SV6621_SCHED_SCAN_MAX_IE_LENGTH   512

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_sched_scan_ssid_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t length;
};

struct sv6621_sched_scan_channel_s
{
  uint8_t number;
  enum sv6621_band_e band;
  bool passive;
};

struct sv6621_sched_scan_match_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint16_t ssid_length;
  uint8_t bssid[SV6621_MAC_LENGTH];
  int32_t rssi_threshold_dbm;
};

struct sv6621_sched_scan_plan_s
{
  uint32_t interval_seconds;
  uint32_t iterations;
};

struct sv6621_sched_scan_request_s
{
  uint32_t request_id;
  uint32_t flags;
  int32_t minimum_rssi_dbm;
  uint32_t delay_seconds;
  uint8_t random_address[SV6621_MAC_LENGTH];
  uint8_t random_address_mask[SV6621_MAC_LENGTH];
  bool relative_rssi_set;
  int8_t relative_rssi_db;
  uint8_t scan_width;
  FAR const struct sv6621_sched_scan_ssid_s *ssids;
  size_t ssid_count;
  FAR const struct sv6621_sched_scan_channel_s *channels;
  size_t channel_count;
  FAR const struct sv6621_sched_scan_match_s *matches;
  size_t match_count;
  FAR const struct sv6621_sched_scan_plan_s *plans;
  size_t plan_count;
  FAR const uint8_t *information_elements;
  size_t information_element_length;
};

typedef void (*sv6621_sched_scan_complete_t)(uint32_t request_id,
                                              FAR void *arg);

struct sv6621_sched_scan_s
{
  mutex_t lock;
  FAR struct sv6621_command_engine_s *command;
  FAR struct sv6621_scan_cache_s *cache;
  sv6621_sched_scan_complete_t complete;
  FAR void *complete_arg;
  uint32_t request_id;
  uint32_t generation;
  bool active;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sched_scan_encode(
    FAR const struct sv6621_sched_scan_request_s *request,
    FAR uint8_t *payload, size_t capacity, FAR size_t *written);
int sv6621_sched_scan_start(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_sched_scan_request_s *request);
int sv6621_sched_scan_stop(FAR struct sv6621_command_engine_s *command);
int sv6621_sched_scan_init(FAR struct sv6621_sched_scan_s *scan,
                           FAR struct sv6621_command_engine_s *command,
                           FAR struct sv6621_scan_cache_s *cache,
                           sv6621_sched_scan_complete_t complete,
                           FAR void *complete_arg);
void sv6621_sched_scan_deinit(FAR struct sv6621_sched_scan_s *scan);
int sv6621_sched_scan_begin(
    FAR struct sv6621_sched_scan_s *scan,
    FAR const struct sv6621_sched_scan_request_s *request);
int sv6621_sched_scan_cancel(FAR struct sv6621_sched_scan_s *scan);
void sv6621_sched_scan_command_event(uint8_t instance, uint8_t id,
                                     FAR const uint8_t *payload,
                                     size_t length, FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H */
