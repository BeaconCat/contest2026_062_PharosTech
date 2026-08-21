/****************************************************************************
 * drivers/drivers/sv6621/sv6621_sched_scan.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>

#include "sv6621_sched_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCHED_SCAN_FIXED_SIZE         75
#define SV6621_SCHED_SCAN_SSID_SIZE          33
#define SV6621_SCHED_SCAN_CHANNEL_SIZE       3
#define SV6621_SCHED_SCAN_MATCH_SIZE         44
#define SV6621_SCHED_SCAN_PLAN_SIZE          8
#define SV6621_SCHED_SCAN_PASSIVE_FLAG       (1 << 7)
#define SV6621_SCHED_SCAN_INSTANCE           0
#define SV6621_SCHED_SCAN_COMMAND_START      7
#define SV6621_SCHED_SCAN_COMMAND_STOP       8
#define SV6621_SCHED_SCAN_EVENT_COMPLETE     1
#define SV6621_SCHED_SCAN_EVENT_REPORT       11
#define SV6621_SCHED_SCAN_COMMAND_TIMEOUT_MS 5000
#define SV6621_SCHED_SCAN_STOP_PAYLOAD_SIZE  8
#define SV6621_SCHED_SCAN_PAYLOAD_CAPACITY                        \
  (SV6621_SCHED_SCAN_FIXED_SIZE +                                 \
   SV6621_SCHED_SCAN_MAX_SSIDS * SV6621_SCHED_SCAN_SSID_SIZE +    \
   UINT8_MAX * SV6621_SCHED_SCAN_CHANNEL_SIZE +                   \
   SV6621_SCHED_SCAN_MAX_MATCHES * SV6621_SCHED_SCAN_MATCH_SIZE + \
   SV6621_SCHED_SCAN_MAX_PLANS * SV6621_SCHED_SCAN_PLAN_SIZE +    \
   SV6621_SCHED_SCAN_MAX_IE_LENGTH)

#define SV6621_SCHED_SCAN_REQUEST_ID_OFFSET     0
#define SV6621_SCHED_SCAN_FLAGS_OFFSET          4
#define SV6621_SCHED_SCAN_MIN_RSSI_OFFSET       8
#define SV6621_SCHED_SCAN_DELAY_OFFSET          12
#define SV6621_SCHED_SCAN_ADDRESS_OFFSET        16
#define SV6621_SCHED_SCAN_ADDRESS_MASK_OFFSET   22
#define SV6621_SCHED_SCAN_RELATIVE_SET_OFFSET   28
#define SV6621_SCHED_SCAN_RELATIVE_RSSI_OFFSET  29
#define SV6621_SCHED_SCAN_WIDTH_OFFSET          30
#define SV6621_SCHED_SCAN_SSID_COUNT_OFFSET     31
#define SV6621_SCHED_SCAN_SSID_LENGTH_OFFSET    32
#define SV6621_SCHED_SCAN_SSID_OFFSET_OFFSET    36
#define SV6621_SCHED_SCAN_IE_LENGTH_OFFSET      40
#define SV6621_SCHED_SCAN_IE_OFFSET_OFFSET      44
#define SV6621_SCHED_SCAN_CHANNEL_COUNT_OFFSET  48
#define SV6621_SCHED_SCAN_CHANNEL_LENGTH_OFFSET 49
#define SV6621_SCHED_SCAN_CHANNEL_OFFSET_OFFSET 53
#define SV6621_SCHED_SCAN_MATCH_COUNT_OFFSET    57
#define SV6621_SCHED_SCAN_MATCH_LENGTH_OFFSET   58
#define SV6621_SCHED_SCAN_MATCH_OFFSET_OFFSET   62
#define SV6621_SCHED_SCAN_PLAN_COUNT_OFFSET     66
#define SV6621_SCHED_SCAN_PLAN_LENGTH_OFFSET    67
#define SV6621_SCHED_SCAN_PLAN_OFFSET_OFFSET    71

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_sched_scan_put_le16(FAR uint8_t *value, uint16_t number);
static void sv6621_sched_scan_put_le32(FAR uint8_t *value, uint32_t number);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_sched_scan_put_le16(FAR uint8_t *value, uint16_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
}

static void sv6621_sched_scan_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sched_scan_encode(
    FAR const struct sv6621_sched_scan_request_s *request,
    FAR uint8_t *payload, size_t capacity, FAR size_t *written)
{
  size_t ssid_length;
  size_t channel_length;
  size_t match_length;
  size_t plan_length;
  size_t total;
  size_t offset;
  size_t index;

  if (request == NULL || payload == NULL || written == NULL ||
      request->ssid_count > SV6621_SCHED_SCAN_MAX_SSIDS ||
      request->channel_count == 0 || request->channel_count > UINT8_MAX ||
      request->match_count > SV6621_SCHED_SCAN_MAX_MATCHES ||
      request->plan_count > SV6621_SCHED_SCAN_MAX_PLANS ||
      request->information_element_length > SV6621_SCHED_SCAN_MAX_IE_LENGTH ||
      (request->ssid_count != 0 && request->ssids == NULL) ||
      request->channels == NULL ||
      (request->match_count != 0 && request->matches == NULL) ||
      (request->plan_count != 0 && request->plans == NULL) ||
      (request->information_element_length != 0 &&
       request->information_elements == NULL))
    {
      return -EINVAL;
    }

  for (index = 0; index < request->ssid_count; index++)
    {
      if (request->ssids[index].length > SV6621_SSID_MAX_LENGTH)
        {
          return -EINVAL;
        }
    }

  for (index = 0; index < request->match_count; index++)
    {
      if (request->matches[index].ssid_length > SV6621_SSID_MAX_LENGTH)
        {
          return -EINVAL;
        }
    }

  for (index = 0; index < request->channel_count; index++)
    {
      if (request->channels[index].number == 0 ||
          request->channels[index].band > SV6621_BAND_5GHZ)
        {
          return -EINVAL;
        }
    }

  ssid_length = request->ssid_count * SV6621_SCHED_SCAN_SSID_SIZE;
  channel_length = request->channel_count * SV6621_SCHED_SCAN_CHANNEL_SIZE;
  match_length = request->match_count * SV6621_SCHED_SCAN_MATCH_SIZE;
  plan_length = request->plan_count * SV6621_SCHED_SCAN_PLAN_SIZE;
  total = SV6621_SCHED_SCAN_FIXED_SIZE + ssid_length + match_length +
          plan_length + request->information_element_length + channel_length;
  if (capacity < total)
    {
      return -ENOSPC;
    }

  memset(payload, 0, total);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_REQUEST_ID_OFFSET,
                             request->request_id);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_FLAGS_OFFSET,
                             request->flags);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_MIN_RSSI_OFFSET,
                             (uint32_t)request->minimum_rssi_dbm);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_DELAY_OFFSET,
                             request->delay_seconds);
  memcpy(payload + SV6621_SCHED_SCAN_ADDRESS_OFFSET, request->random_address,
         SV6621_MAC_LENGTH);
  memcpy(payload + SV6621_SCHED_SCAN_ADDRESS_MASK_OFFSET,
         request->random_address_mask, SV6621_MAC_LENGTH);
  payload[SV6621_SCHED_SCAN_RELATIVE_SET_OFFSET] = request->relative_rssi_set;
  payload[SV6621_SCHED_SCAN_RELATIVE_RSSI_OFFSET] =
      (uint8_t)request->relative_rssi_db;
  payload[SV6621_SCHED_SCAN_WIDTH_OFFSET] = request->scan_width;

  offset = SV6621_SCHED_SCAN_FIXED_SIZE;
  payload[SV6621_SCHED_SCAN_SSID_COUNT_OFFSET] = (uint8_t)request->ssid_count;
  if (request->ssid_count != 0)
    {
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_SSID_LENGTH_OFFSET, ssid_length);
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_SSID_OFFSET_OFFSET, offset);
    }

  for (index = 0; index < request->ssid_count; index++)
    {
      memcpy(payload + offset, request->ssids[index].ssid,
             request->ssids[index].length);
      payload[offset + SV6621_SSID_MAX_LENGTH] = request->ssids[index].length;
      offset += SV6621_SCHED_SCAN_SSID_SIZE;
    }

  payload[SV6621_SCHED_SCAN_MATCH_COUNT_OFFSET] =
      (uint8_t)request->match_count;
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_MATCH_LENGTH_OFFSET,
                             match_length);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_MATCH_OFFSET_OFFSET,
                             offset);
  for (index = 0; index < request->match_count; index++)
    {
      memcpy(payload + offset, request->matches[index].ssid,
             request->matches[index].ssid_length);
      sv6621_sched_scan_put_le16(payload + offset + 32,
                                 request->matches[index].ssid_length);
      memcpy(payload + offset + 34, request->matches[index].bssid,
             SV6621_MAC_LENGTH);
      sv6621_sched_scan_put_le32(
          payload + offset + 40,
          (uint32_t)request->matches[index].rssi_threshold_dbm);
      offset += SV6621_SCHED_SCAN_MATCH_SIZE;
    }

  payload[SV6621_SCHED_SCAN_PLAN_COUNT_OFFSET] = (uint8_t)request->plan_count;
  if (request->plan_count != 0)
    {
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_PLAN_LENGTH_OFFSET, plan_length);
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_PLAN_OFFSET_OFFSET, offset);
    }

  for (index = 0; index < request->plan_count; index++)
    {
      sv6621_sched_scan_put_le32(payload + offset,
                                 request->plans[index].interval_seconds);
      sv6621_sched_scan_put_le32(payload + offset + 4,
                                 request->plans[index].iterations);
      offset += SV6621_SCHED_SCAN_PLAN_SIZE;
    }

  if (request->information_element_length != 0)
    {
      sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_IE_LENGTH_OFFSET,
                                 request->information_element_length);
      sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_IE_OFFSET_OFFSET,
                                 offset);
      memcpy(payload + offset, request->information_elements,
             request->information_element_length);
    }

  offset += request->information_element_length;

  payload[SV6621_SCHED_SCAN_CHANNEL_COUNT_OFFSET] =
      (uint8_t)request->channel_count;
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_CHANNEL_LENGTH_OFFSET,
                             channel_length);
  sv6621_sched_scan_put_le32(payload + SV6621_SCHED_SCAN_CHANNEL_OFFSET_OFFSET,
                             offset);
  for (index = 0; index < request->channel_count; index++)
    {
      payload[offset] = request->channels[index].number;
      payload[offset + 1] = request->channels[index].band;
      payload[offset + 2] = request->channels[index].passive
                                ? SV6621_SCHED_SCAN_PASSIVE_FLAG
                                : 0;
      offset += SV6621_SCHED_SCAN_CHANNEL_SIZE;
    }

  *written = offset;
  return 0;
}

int sv6621_sched_scan_start(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_sched_scan_request_s *request)
{
  FAR uint8_t *payload;
  size_t payload_length;
  int ret;

  if (command == NULL || request == NULL)
    {
      return -EINVAL;
    }

  payload = kmm_malloc(SV6621_SCHED_SCAN_PAYLOAD_CAPACITY);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  ret = sv6621_sched_scan_encode(
      request, payload, SV6621_SCHED_SCAN_PAYLOAD_CAPACITY, &payload_length);
  if (ret == 0)
    {
      ret = sv6621_command_execute(command, SV6621_SCHED_SCAN_INSTANCE,
                                   SV6621_SCHED_SCAN_COMMAND_START, payload,
                                   payload_length, NULL, NULL,
                                   SV6621_SCHED_SCAN_COMMAND_TIMEOUT_MS);
    }

  kmm_free(payload);
  return ret;
}

int sv6621_sched_scan_stop(FAR struct sv6621_command_engine_s *command)
{
  uint8_t scan_id[SV6621_SCHED_SCAN_STOP_PAYLOAD_SIZE] = { 0 };

  if (command == NULL)
    {
      return -EINVAL;
    }

  return sv6621_command_execute(command, SV6621_SCHED_SCAN_INSTANCE,
                                SV6621_SCHED_SCAN_COMMAND_STOP, scan_id,
                                sizeof(scan_id), NULL, NULL,
                                SV6621_SCHED_SCAN_COMMAND_TIMEOUT_MS);
}

int sv6621_sched_scan_init(FAR struct sv6621_sched_scan_s *scan,
                           FAR struct sv6621_command_engine_s *command,
                           FAR struct sv6621_scan_cache_s *cache,
                           sv6621_sched_scan_result_t result,
                           sv6621_sched_scan_complete_t complete,
                           FAR void *complete_arg)
{
  int ret;

  if (scan == NULL || command == NULL || cache == NULL)
    {
      return -EINVAL;
    }

  memset(scan, 0, sizeof(*scan));
  ret = nxmutex_init(&scan->lock);
  if (ret < 0)
    {
      return ret;
    }

  scan->command = command;
  scan->cache = cache;
  scan->result = result;
  scan->complete = complete;
  scan->complete_arg = complete_arg;
  return 0;
}

void sv6621_sched_scan_deinit(FAR struct sv6621_sched_scan_s *scan)
{
  if (scan != NULL)
    {
      nxmutex_destroy(&scan->lock);
    }
}

int sv6621_sched_scan_begin(
    FAR struct sv6621_sched_scan_s *scan,
    FAR const struct sv6621_sched_scan_request_s *request)
{
  int ret;

  if (scan == NULL || request == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&scan->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (scan->active)
    {
      nxmutex_unlock(&scan->lock);
      return -EBUSY;
    }

  ret = sv6621_sched_scan_start(scan->command, request);
  if (ret == 0)
    {
      scan->active = true;
      scan->request_id = request->request_id;
      scan->generation++;
    }

  nxmutex_unlock(&scan->lock);
  return ret;
}

int sv6621_sched_scan_cancel(FAR struct sv6621_sched_scan_s *scan)
{
  int ret;

  if (scan == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&scan->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!scan->active)
    {
      nxmutex_unlock(&scan->lock);
      return 0;
    }

  scan->active = false;
  scan->generation++;
  ret = sv6621_sched_scan_stop(scan->command);
  nxmutex_unlock(&scan->lock);
  return ret;
}

void sv6621_sched_scan_command_event(uint8_t instance, uint8_t id,
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg)
{
  FAR struct sv6621_sched_scan_s *scan = arg;
  FAR struct sv6621_bss_s *entries;
  struct sv6621_scan_entry_s entry;
  sv6621_sched_scan_complete_t complete;
  sv6621_sched_scan_result_t result;
  FAR void *complete_arg;
  uint32_t request_id;
  uint32_t generation;
  size_t count;
  size_t index;
  bool inserted;

  if (scan == NULL || instance != SV6621_SCHED_SCAN_INSTANCE ||
      (id != SV6621_SCHED_SCAN_EVENT_COMPLETE &&
       id != SV6621_SCHED_SCAN_EVENT_REPORT))
    {
      return;
    }

  if (nxmutex_lock(&scan->lock) < 0)
    {
      return;
    }

  if (!scan->active)
    {
      nxmutex_unlock(&scan->lock);
      return;
    }

  complete = scan->complete;
  result = scan->result;
  complete_arg = scan->complete_arg;
  request_id = scan->request_id;
  generation = scan->generation;
  nxmutex_unlock(&scan->lock);

  if (id == SV6621_SCHED_SCAN_EVENT_COMPLETE)
    {
      if (nxmutex_lock(&scan->lock) < 0)
        {
          return;
        }

      if (!scan->active || scan->generation != generation)
        {
          nxmutex_unlock(&scan->lock);
          return;
        }

      nxmutex_unlock(&scan->lock);
      if (result != NULL)
        {
          count = SV6621_SCAN_CACHE_CAPACITY;
          entries = kmm_malloc(sizeof(*entries) * count);
          if (entries != NULL)
            {
              if (sv6621_scan_cache_snapshot(scan->cache, entries, &count) ==
                  0)
                {
                  for (index = 0; index < count; index++)
                    {
                      result(&entries[index], complete_arg);
                    }
                }

              kmm_free(entries);
            }
        }

      if (complete != NULL)
        {
          complete(request_id, complete_arg);
        }

      return;
    }

  if (sv6621_scan_parse_report(payload, length, &entry) == 0)
    {
      (void)sv6621_scan_cache_store(scan->cache, &entry, &inserted);
    }
}
