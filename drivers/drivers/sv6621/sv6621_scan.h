/****************************************************************************
 * drivers/drivers/sv6621/sv6621_scan.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCAN_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCAN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621.h"
#include "sv6621_command.h"
#include "sv6621_rx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCAN_FLAG_RANDOM_ADDRESS (1 << 0)
#define SV6621_SCAN_FLAG_ACS            (1 << 1)
#define SV6621_SCAN_FLAG_PASSIVE        (1 << 7)
#define SV6621_SCAN_CACHE_CAPACITY      64
#define SV6621_SCAN_IE_CAPACITY         512
#define SV6621_SCAN_EVENT_COMPLETE      0
#define SV6621_SCAN_EVENT_REPORT        11
#define SV6621_SCAN_MAX_CHANNELS        64

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_scan_band_e
{
  SV6621_SCAN_BAND_2GHZ = 0,
  SV6621_SCAN_BAND_5GHZ
};

struct sv6621_scan_channel_s
{
  uint8_t number;
  enum sv6621_scan_band_e band;
  uint8_t flags;
};

struct sv6621_scan_entry_s
{
  struct sv6621_bss_s bss;
  uint16_t beacon_interval;
  uint16_t capability;
  uint8_t ht_primary_channel;
  uint8_t ht_secondary_offset;
  uint8_t vht_channel_width;
  uint8_t vht_center_segment0;
  uint8_t vht_center_segment1;
  uint8_t bssid_index;
  uint8_t max_bssid_indicator;
  uint8_t rsn_group_cipher;
  uint8_t rsn_group_management_cipher;
  uint16_t rsn_capabilities;
  uint16_t ie_length;
  bool ht_operation_present;
  bool vht_operation_present;
  bool rsn_present;
  bool rsn_pairwise_ccmp;
  bool ies_truncated;
  uint8_t ies[SV6621_SCAN_IE_CAPACITY];
};

struct sv6621_scan_cache_s
{
  mutex_t lock;
  struct sv6621_scan_entry_s entries[SV6621_SCAN_CACHE_CAPACITY];
  size_t count;
  uint32_t replacements;
  uint32_t dropped;
};

struct sv6621_scan_stats_s
{
  uint32_t started;
  uint32_t completed;
  uint32_t cancelled;
  uint32_t timed_out;
  uint32_t malformed_reports;
  uint32_t truncated_reports;
  uint32_t late_events;
};

typedef void (*sv6621_scan_complete_t)(int result, FAR void *arg);

struct sv6621_scan_s
{
  mutex_t lock;
  FAR struct sv6621_command_engine_s *command;
  FAR struct sv6621_rx_s *rx;
  struct sv6621_scan_cache_s cache;
  struct work_s timeout_work;
  sv6621_scan_complete_t complete;
  FAR void *complete_arg;
  uint32_t timeout_ms;
  struct sv6621_scan_channel_s channels[SV6621_SCAN_MAX_CHANNELS];
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  size_t channel_count;
  size_t ssid_length;
  bool active;
  bool stopping;
  bool recovery_pending;
  struct sv6621_scan_stats_s stats;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_scan_start(FAR struct sv6621_command_engine_s *command,
                      FAR const struct sv6621_scan_channel_s *channels,
                      size_t channel_count, FAR const uint8_t *ssid,
                      size_t ssid_length);
int sv6621_scan_stop(FAR struct sv6621_command_engine_s *command);
int sv6621_scan_parse_report(FAR const uint8_t *payload, size_t length,
                             FAR struct sv6621_scan_entry_s *entry);
int sv6621_scan_cache_init(FAR struct sv6621_scan_cache_s *cache);
void sv6621_scan_cache_deinit(FAR struct sv6621_scan_cache_s *cache);
int sv6621_scan_cache_reset(FAR struct sv6621_scan_cache_s *cache);
int sv6621_scan_cache_store(FAR struct sv6621_scan_cache_s *cache,
                            FAR const struct sv6621_scan_entry_s *entry,
                            FAR bool *inserted);
int sv6621_scan_cache_snapshot(FAR struct sv6621_scan_cache_s *cache,
                               FAR struct sv6621_bss_s *entries,
                               FAR size_t *count);
int sv6621_scan_cache_find(FAR struct sv6621_scan_cache_s *cache,
                           FAR const struct sv6621_connect_s *request,
                           FAR struct sv6621_scan_entry_s *entry);
int sv6621_scan_cache_find_roam_candidate(
    FAR struct sv6621_scan_cache_s *cache,
    FAR const struct sv6621_connect_s *request,
    FAR const uint8_t current_bssid[SV6621_MAC_LENGTH],
    int16_t current_signal_dbm, uint8_t minimum_gain_db,
    FAR struct sv6621_scan_entry_s *entry);
int sv6621_scan_controller_init(FAR struct sv6621_scan_s *scan,
                                FAR struct sv6621_command_engine_s *command,
                                FAR struct sv6621_rx_s *rx,
                                uint32_t timeout_ms,
                                sv6621_scan_complete_t complete,
                                FAR void *complete_arg);
void sv6621_scan_controller_deinit(FAR struct sv6621_scan_s *scan);
int sv6621_scan_controller_begin(
    FAR struct sv6621_scan_s *scan,
    FAR const struct sv6621_scan_channel_s *channels, size_t channel_count,
    FAR const uint8_t *ssid, size_t ssid_length);
int sv6621_scan_controller_cancel(FAR struct sv6621_scan_s *scan);
void sv6621_scan_command_event(uint8_t instance, uint8_t id,
                               FAR const uint8_t *payload, size_t length,
                               FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCAN_H */
