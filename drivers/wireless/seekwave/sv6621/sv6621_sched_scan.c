/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sched_scan.c
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

#include <errno.h>
#include <string.h>

#include "sv6621_sched_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCHED_SCAN_FIXED_SIZE             75
#define SV6621_SCHED_SCAN_SSID_SIZE               33
#define SV6621_SCHED_SCAN_CHANNEL_SIZE             3
#define SV6621_SCHED_SCAN_MATCH_SIZE              44
#define SV6621_SCHED_SCAN_PLAN_SIZE                8
#define SV6621_SCHED_SCAN_PASSIVE_FLAG       (1 << 7)

#define SV6621_SCHED_SCAN_REQUEST_ID_OFFSET        0
#define SV6621_SCHED_SCAN_FLAGS_OFFSET             4
#define SV6621_SCHED_SCAN_MIN_RSSI_OFFSET          8
#define SV6621_SCHED_SCAN_DELAY_OFFSET            12
#define SV6621_SCHED_SCAN_ADDRESS_OFFSET          16
#define SV6621_SCHED_SCAN_ADDRESS_MASK_OFFSET     22
#define SV6621_SCHED_SCAN_RELATIVE_SET_OFFSET     28
#define SV6621_SCHED_SCAN_RELATIVE_RSSI_OFFSET    29
#define SV6621_SCHED_SCAN_WIDTH_OFFSET            30
#define SV6621_SCHED_SCAN_SSID_COUNT_OFFSET       31
#define SV6621_SCHED_SCAN_SSID_LENGTH_OFFSET      32
#define SV6621_SCHED_SCAN_SSID_OFFSET_OFFSET      36
#define SV6621_SCHED_SCAN_IE_LENGTH_OFFSET        40
#define SV6621_SCHED_SCAN_IE_OFFSET_OFFSET        44
#define SV6621_SCHED_SCAN_CHANNEL_COUNT_OFFSET    48
#define SV6621_SCHED_SCAN_CHANNEL_LENGTH_OFFSET   49
#define SV6621_SCHED_SCAN_CHANNEL_OFFSET_OFFSET   53
#define SV6621_SCHED_SCAN_MATCH_COUNT_OFFSET      57
#define SV6621_SCHED_SCAN_MATCH_LENGTH_OFFSET     58
#define SV6621_SCHED_SCAN_MATCH_OFFSET_OFFSET     62
#define SV6621_SCHED_SCAN_PLAN_COUNT_OFFSET       66
#define SV6621_SCHED_SCAN_PLAN_LENGTH_OFFSET      67
#define SV6621_SCHED_SCAN_PLAN_OFFSET_OFFSET      71

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_sched_scan_put_le16(FAR uint8_t *value,
                                       uint16_t number);
static void sv6621_sched_scan_put_le32(FAR uint8_t *value,
                                       uint32_t number);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_sched_scan_put_le16(FAR uint8_t *value,
                                       uint16_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
}

static void sv6621_sched_scan_put_le32(FAR uint8_t *value,
                                       uint32_t number)
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
      request->channel_count == 0 ||
      request->channel_count > UINT8_MAX ||
      request->match_count > SV6621_SCHED_SCAN_MAX_MATCHES ||
      request->plan_count > SV6621_SCHED_SCAN_MAX_PLANS ||
      request->information_element_length >
          SV6621_SCHED_SCAN_MAX_IE_LENGTH ||
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
  memcpy(payload + SV6621_SCHED_SCAN_ADDRESS_OFFSET,
         request->random_address, SV6621_MAC_LENGTH);
  memcpy(payload + SV6621_SCHED_SCAN_ADDRESS_MASK_OFFSET,
         request->random_address_mask, SV6621_MAC_LENGTH);
  payload[SV6621_SCHED_SCAN_RELATIVE_SET_OFFSET] =
      request->relative_rssi_set;
  payload[SV6621_SCHED_SCAN_RELATIVE_RSSI_OFFSET] =
      (uint8_t)request->relative_rssi_db;
  payload[SV6621_SCHED_SCAN_WIDTH_OFFSET] = request->scan_width;

  offset = SV6621_SCHED_SCAN_FIXED_SIZE;
  payload[SV6621_SCHED_SCAN_SSID_COUNT_OFFSET] =
      (uint8_t)request->ssid_count;
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
      payload[offset + SV6621_SSID_MAX_LENGTH] =
          request->ssids[index].length;
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

  payload[SV6621_SCHED_SCAN_PLAN_COUNT_OFFSET] =
      (uint8_t)request->plan_count;
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
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_IE_LENGTH_OFFSET,
          request->information_element_length);
      sv6621_sched_scan_put_le32(
          payload + SV6621_SCHED_SCAN_IE_OFFSET_OFFSET, offset);
      memcpy(payload + offset, request->information_elements,
             request->information_element_length);
    }

  offset += request->information_element_length;

  payload[SV6621_SCHED_SCAN_CHANNEL_COUNT_OFFSET] =
      (uint8_t)request->channel_count;
  sv6621_sched_scan_put_le32(
      payload + SV6621_SCHED_SCAN_CHANNEL_LENGTH_OFFSET, channel_length);
  sv6621_sched_scan_put_le32(
      payload + SV6621_SCHED_SCAN_CHANNEL_OFFSET_OFFSET, offset);
  for (index = 0; index < request->channel_count; index++)
    {
      payload[offset] = request->channels[index].number;
      payload[offset + 1] = request->channels[index].band;
      payload[offset + 2] = request->channels[index].passive ?
                            SV6621_SCHED_SCAN_PASSIVE_FLAG : 0;
      offset += SV6621_SCHED_SCAN_CHANNEL_SIZE;
    }

  *written = offset;
  return 0;
}
