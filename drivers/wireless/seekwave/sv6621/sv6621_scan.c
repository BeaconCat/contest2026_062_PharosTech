/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_scan.c
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

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>

#include "sv6621_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCAN_INSTANCE             0
#define SV6621_SCAN_COMMAND_START        5
#define SV6621_SCAN_COMMAND_STOP         6
#define SV6621_SCAN_FIXED_SIZE           32
#define SV6621_SCAN_CHANNEL_SIZE         3
#define SV6621_SCAN_SSID_SIZE            (SV6621_SSID_MAX_LENGTH + 1)
#define SV6621_SCAN_COMMAND_TIMEOUT_MS   5000
#define SV6621_SCAN_STOP_TIMEOUT_MS      500
#define SV6621_SCAN_RECOVERY_GRACE_MS      500

#define SV6621_SCAN_CHANNEL_COUNT_OFFSET 8
#define SV6621_SCAN_CHANNEL_LIST_OFFSET  12
#define SV6621_SCAN_SSID_COUNT_OFFSET    16
#define SV6621_SCAN_SSID_LIST_OFFSET     20
#define SV6621_SCAN_IE_LENGTH_OFFSET     24
#define SV6621_SCAN_IE_OFFSET_OFFSET     28

#define SV6621_SCAN_REPORT_HEADER_SIZE   8
#define SV6621_SCAN_INFORMATION_OFFSET   36
#define SV6621_SCAN_BEACON_INTERVAL_OFFSET 32
#define SV6621_SCAN_BSSID_OFFSET         16
#define SV6621_SCAN_CAPABILITY_OFFSET    34
#define SV6621_SCAN_CAPABILITY_PRIVACY   (1 << 4)
#define SV6621_SCAN_FRAME_SUBTYPE_MASK   0xfc
#define SV6621_SCAN_FRAME_BEACON         0x80
#define SV6621_SCAN_FRAME_PROBE_RESPONSE 0x50
#define SV6621_SCAN_IE_SSID              0
#define SV6621_SCAN_IE_RSN               48
#define SV6621_SCAN_IE_HT_OPERATION      61
#define SV6621_SCAN_IE_VHT_OPERATION     192
#define SV6621_SCAN_IE_VENDOR            221
#define SV6621_SCAN_HT_OPERATION_MIN_SIZE 2
#define SV6621_SCAN_HT_SECONDARY_MASK     0x03
#define SV6621_SCAN_HT_SECONDARY_NONE     0
#define SV6621_SCAN_HT_SECONDARY_ABOVE    1
#define SV6621_SCAN_HT_SECONDARY_BELOW    3
#define SV6621_SCAN_VHT_OPERATION_MIN_SIZE 3
#define SV6621_SCAN_VHT_WIDTH_MAX          3
#define SV6621_SCAN_RSN_SUITE_SIZE       4
#define SV6621_SCAN_RSN_CIPHER_CCMP      4
#define SV6621_SCAN_RSN_CIPHER_BIP_CMAC  6
#define SV6621_SCAN_RSN_AKM_PSK          2
#define SV6621_SCAN_RSN_AKM_SAE          8
#define SV6621_SCAN_RSN_CAP_MFPR         (1 << 6)
#define SV6621_SCAN_RSN_CAP_MFPC         (1 << 7)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_scan_put_le32(FAR uint8_t *value, uint32_t number);
static uint16_t sv6621_scan_get_le16(FAR const uint8_t *value);
static int sv6621_scan_parse_rsn(FAR const uint8_t *data, size_t length,
                                 FAR struct sv6621_scan_entry_s *entry,
                                 FAR bool *psk, FAR bool *sae);
static bool sv6621_scan_is_rsn_suite(FAR const uint8_t *suite);
static bool sv6621_scan_is_wpa_ie(FAR const uint8_t *data, size_t length);
static bool sv6621_scan_security_matches(
    enum sv6621_security_e requested, enum sv6621_security_e advertised);
static bool sv6621_scan_connection_supported(
    enum sv6621_security_e requested,
    FAR const struct sv6621_scan_entry_s *entry);
static size_t
sv6621_scan_cache_weakest(FAR const struct sv6621_scan_cache_s *cache);
static void sv6621_scan_timeout_worker(FAR void *arg);
static int sv6621_scan_queue_timeout(FAR struct sv6621_scan_s *scan);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_scan_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

static uint16_t sv6621_scan_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static bool sv6621_scan_is_rsn_suite(FAR const uint8_t *suite)
{
  return suite[0] == 0x00 && suite[1] == 0x0f && suite[2] == 0xac;
}

static int sv6621_scan_parse_rsn(FAR const uint8_t *data, size_t length,
                                 FAR struct sv6621_scan_entry_s *entry,
                                 FAR bool *psk, FAR bool *sae)
{
  FAR const uint8_t *suite;
  uint16_t suite_count;
  size_t offset;
  unsigned int index;

  if (length < 8 || sv6621_scan_get_le16(data) != 1)
    {
      return -EPROTO;
    }

  suite = data + 2;
  if (!sv6621_scan_is_rsn_suite(suite))
    {
      return -EPROTO;
    }

  entry->rsn_group_cipher = suite[3];
  suite_count = sv6621_scan_get_le16(data + 6);
  if (suite_count == 0)
    {
      return -EPROTO;
    }

  offset = 8 + (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE;
  if (offset + 2 > length)
    {
      return -EPROTO;
    }

  for (index = 0; index < suite_count; index++)
    {
      suite = data + 8 + index * SV6621_SCAN_RSN_SUITE_SIZE;
      if (sv6621_scan_is_rsn_suite(suite) &&
          suite[3] == SV6621_SCAN_RSN_CIPHER_CCMP)
        {
          entry->rsn_pairwise_ccmp = true;
        }
    }

  suite_count = sv6621_scan_get_le16(data + offset);
  offset += 2;
  if (offset + (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE > length)
    {
      return -EPROTO;
    }

  for (index = 0; index < suite_count; index++)
    {
      suite = data + offset + index * SV6621_SCAN_RSN_SUITE_SIZE;

      if (!sv6621_scan_is_rsn_suite(suite))
        {
          continue;
        }

      if (suite[3] == SV6621_SCAN_RSN_AKM_PSK)
        {
          *psk = true;
        }
      else if (suite[3] == SV6621_SCAN_RSN_AKM_SAE)
        {
          *sae = true;
        }
    }

  offset += (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE;
  if (offset < length)
    {
      if (length - offset < 2)
        {
          return -EPROTO;
        }

      entry->rsn_capabilities = sv6621_scan_get_le16(data + offset);
      offset += 2;
      if ((entry->rsn_capabilities & SV6621_SCAN_RSN_CAP_MFPC) != 0)
        {
          entry->rsn_group_management_cipher =
              SV6621_SCAN_RSN_CIPHER_BIP_CMAC;
        }
    }

  if (offset < length)
    {
      suite_count = sv6621_scan_get_le16(data + offset);
      offset += 2;
      if (offset + (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE > length)
        {
          return -EPROTO;
        }

      offset += (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE;
    }

  if (offset < length)
    {
      if (length - offset < SV6621_SCAN_RSN_SUITE_SIZE)
        {
          return -EPROTO;
        }

      suite = data + offset;
      if (!sv6621_scan_is_rsn_suite(suite))
        {
          return -EPROTO;
        }

      entry->rsn_group_management_cipher = suite[3];
      offset += SV6621_SCAN_RSN_SUITE_SIZE;
    }

  if (offset != length)
    {
      return -EPROTO;
    }

  entry->rsn_present = true;

  return 0;
}

static bool sv6621_scan_is_wpa_ie(FAR const uint8_t *data, size_t length)
{
  static const uint8_t wpa_type[] = { 0x00, 0x50, 0xf2, 0x01 };

  return length >= sizeof(wpa_type) &&
         memcmp(data, wpa_type, sizeof(wpa_type)) == 0;
}

/****************************************************************************
 * Name: sv6621_scan_security_matches
 ****************************************************************************/

static bool sv6621_scan_security_matches(
    enum sv6621_security_e requested, enum sv6621_security_e advertised)
{
  if (requested == SV6621_SECURITY_OPEN)
    {
      return advertised == SV6621_SECURITY_OPEN;
    }

  if (requested == SV6621_SECURITY_WPA2_PSK ||
      requested == SV6621_SECURITY_WPA2_WPA3_PSK)
    {
      return advertised == SV6621_SECURITY_WPA2_PSK ||
             advertised == SV6621_SECURITY_WPA2_WPA3_PSK;
    }

  if (requested == SV6621_SECURITY_WPA3_SAE)
    {
      return advertised == SV6621_SECURITY_WPA3_SAE ||
             advertised == SV6621_SECURITY_WPA2_WPA3_PSK;
    }

  return false;
}

static bool sv6621_scan_connection_supported(
    enum sv6621_security_e requested,
    FAR const struct sv6621_scan_entry_s *entry)
{
  if (requested == SV6621_SECURITY_OPEN)
    {
      return true;
    }

  if (!entry->rsn_present ||
      entry->rsn_group_cipher != SV6621_SCAN_RSN_CIPHER_CCMP ||
      !entry->rsn_pairwise_ccmp)
    {
      return false;
    }

  if (requested == SV6621_SECURITY_WPA3_SAE)
    {
      return (entry->rsn_capabilities &
              (SV6621_SCAN_RSN_CAP_MFPR | SV6621_SCAN_RSN_CAP_MFPC)) ==
                 (SV6621_SCAN_RSN_CAP_MFPR | SV6621_SCAN_RSN_CAP_MFPC) &&
             entry->rsn_group_management_cipher ==
                 SV6621_SCAN_RSN_CIPHER_BIP_CMAC;
    }

  return (entry->rsn_capabilities & SV6621_SCAN_RSN_CAP_MFPR) == 0;
}

static size_t
sv6621_scan_cache_weakest(FAR const struct sv6621_scan_cache_s *cache)
{
  size_t weakest = 0;
  size_t index;

  for (index = 1; index < cache->count; index++)
    {
      if (cache->entries[index].bss.signal_dbm <
          cache->entries[weakest].bss.signal_dbm)
        {
          weakest = index;
        }
    }

  return weakest;
}

static void sv6621_scan_timeout_worker(FAR void *arg)
{
  FAR struct sv6621_scan_s *scan = arg;
  sv6621_scan_complete_t complete = NULL;
  FAR void *complete_arg = NULL;

  if (nxmutex_lock(&scan->lock) < 0)
    {
      return;
    }

  if (scan->active)
    {
      if (!scan->recovery_pending)
        {
          scan->recovery_pending = true;
          nxmutex_unlock(&scan->lock);
          sv6621_rx_kick(scan->rx);
          work_queue(LPWORK, &scan->timeout_work,
                     sv6621_scan_timeout_worker, scan,
                     MSEC2TICK(SV6621_SCAN_RECOVERY_GRACE_MS));
          return;
        }

      scan->stats.timed_out++;
      scan->active = false;
      scan->stopping = false;
      complete = scan->complete;
      complete_arg = scan->complete_arg;
    }

  nxmutex_unlock(&scan->lock);
  if (complete != NULL)
    {
      complete(-ETIMEDOUT, complete_arg);
    }
}

static int sv6621_scan_queue_timeout(FAR struct sv6621_scan_s *scan)
{
  return work_queue(LPWORK, &scan->timeout_work,
                    sv6621_scan_timeout_worker, scan,
                    MSEC2TICK(scan->timeout_ms));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_scan_start(FAR struct sv6621_command_engine_s *command,
                      FAR const struct sv6621_scan_channel_s *channels,
                      size_t channel_count, FAR const uint8_t *ssid,
                      size_t ssid_length)
{
  FAR uint8_t *payload;
  FAR uint8_t *ssid_entry;
  size_t payload_length;
  size_t ssid_offset;
  size_t index;
  int ret;

  if (command == NULL || channels == NULL || channel_count == 0 ||
      channel_count > SV6621_SCAN_MAX_CHANNELS ||
      ssid_length > SV6621_SSID_MAX_LENGTH ||
      (ssid_length != 0 && ssid == NULL))
    {
      return -EINVAL;
    }

  ssid_offset = SV6621_SCAN_FIXED_SIZE +
                channel_count * SV6621_SCAN_CHANNEL_SIZE;
  payload_length = ssid_offset +
                   (ssid_length != 0 ? SV6621_SCAN_SSID_SIZE : 0);
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_COUNT_OFFSET,
                       channel_count);
  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_LIST_OFFSET,
                       SV6621_SCAN_FIXED_SIZE);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_COUNT_OFFSET,
                       ssid_length != 0 ? 1 : 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_LIST_OFFSET,
                       ssid_length != 0 ? ssid_offset : 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_IE_LENGTH_OFFSET, 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_IE_OFFSET_OFFSET, 0);

  for (index = 0; index < channel_count; index++)
    {
      FAR uint8_t *encoded =
          payload + SV6621_SCAN_FIXED_SIZE + index * SV6621_SCAN_CHANNEL_SIZE;

      if (channels[index].number == 0 ||
          channels[index].band > SV6621_SCAN_BAND_5GHZ)
        {
          ret = -EINVAL;
          goto free_payload;
        }

      encoded[0] = channels[index].number;
      encoded[1] = channels[index].band;
      encoded[2] = channels[index].flags;
    }

  if (ssid_length != 0)
    {
      ssid_entry = payload + ssid_offset;
      memcpy(ssid_entry, ssid, ssid_length);
      ssid_entry[SV6621_SSID_MAX_LENGTH] = ssid_length;
    }

  ret = sv6621_command_execute(
      command, SV6621_SCAN_INSTANCE, SV6621_SCAN_COMMAND_START, payload,
      payload_length, NULL, NULL, SV6621_SCAN_COMMAND_TIMEOUT_MS);
  if (ret > 0)
    {
      ret = -EREMOTEIO;
    }

free_payload:
  kmm_free(payload);
  return ret;
}

int sv6621_scan_stop(FAR struct sv6621_command_engine_s *command)
{
  int ret;

  if (command == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_command_execute(command, SV6621_SCAN_INSTANCE,
                               SV6621_SCAN_COMMAND_STOP, NULL, 0, NULL, NULL,
                               SV6621_SCAN_STOP_TIMEOUT_MS);
  return ret > 0 ? -EREMOTEIO : ret;
}

int sv6621_scan_parse_report(FAR const uint8_t *payload, size_t length,
                             FAR struct sv6621_scan_entry_s *entry)
{
  FAR struct sv6621_bss_s *bss;
  FAR const uint8_t *frame;
  uint16_t frame_length;
  uint16_t capability;
  size_t offset;
  bool legacy_security = false;
  bool psk = false;
  bool sae = false;
  bool privacy;

  if (payload == NULL || entry == NULL ||
      length < SV6621_SCAN_REPORT_HEADER_SIZE + SV6621_SCAN_INFORMATION_OFFSET)
    {
      return -EINVAL;
    }

  if (payload[1] > SV6621_SCAN_BAND_5GHZ || payload[0] == 0)
    {
      return -EPROTO;
    }

  frame_length = sv6621_scan_get_le16(payload + 4);
  if (frame_length < SV6621_SCAN_INFORMATION_OFFSET ||
      frame_length > length - SV6621_SCAN_REPORT_HEADER_SIZE)
    {
      return -EPROTO;
    }

  frame = payload + SV6621_SCAN_REPORT_HEADER_SIZE;
  if ((frame[0] & SV6621_SCAN_FRAME_SUBTYPE_MASK) !=
          SV6621_SCAN_FRAME_BEACON &&
      (frame[0] & SV6621_SCAN_FRAME_SUBTYPE_MASK) !=
          SV6621_SCAN_FRAME_PROBE_RESPONSE)
    {
      return -EPROTO;
    }

  memset(entry, 0, sizeof(*entry));
  bss = &entry->bss;
  memcpy(bss->bssid, frame + SV6621_SCAN_BSSID_OFFSET, SV6621_MAC_LENGTH);
  bss->channel = payload[0];
  bss->band = payload[1] == SV6621_SCAN_BAND_2GHZ ? SV6621_BAND_2GHZ
                                                  : SV6621_BAND_5GHZ;
  bss->signal_dbm = (int16_t)sv6621_scan_get_le16(payload + 2);
  capability = sv6621_scan_get_le16(frame + SV6621_SCAN_CAPABILITY_OFFSET);
  entry->beacon_interval =
      sv6621_scan_get_le16(frame + SV6621_SCAN_BEACON_INTERVAL_OFFSET);
  entry->capability = capability;
  privacy = (capability & SV6621_SCAN_CAPABILITY_PRIVACY) != 0;

  for (offset = SV6621_SCAN_INFORMATION_OFFSET; offset < frame_length;)
    {
      uint8_t id;
      uint8_t ie_length;

      if (frame_length - offset < 2)
        {
          return -EPROTO;
        }

      id = frame[offset];
      ie_length = frame[offset + 1];
      offset += 2;
      if (ie_length > frame_length - offset)
        {
          return -EPROTO;
        }

      if (id == SV6621_SCAN_IE_SSID)
        {
          bss->ssid_length = ie_length > SV6621_SSID_MAX_LENGTH
                                 ? SV6621_SSID_MAX_LENGTH
                                 : ie_length;
          memcpy(bss->ssid, frame + offset, bss->ssid_length);
        }
      else if (id == SV6621_SCAN_IE_RSN)
        {
          if (sv6621_scan_parse_rsn(frame + offset, ie_length, entry, &psk,
                                    &sae) < 0)
            {
              return -EPROTO;
            }
        }
      else if (id == SV6621_SCAN_IE_HT_OPERATION &&
               ie_length >= SV6621_SCAN_HT_OPERATION_MIN_SIZE)
        {
          uint8_t secondary =
              frame[offset + 1] & SV6621_SCAN_HT_SECONDARY_MASK;

          if (frame[offset] != 0 &&
              (secondary == SV6621_SCAN_HT_SECONDARY_NONE ||
               secondary == SV6621_SCAN_HT_SECONDARY_ABOVE ||
               secondary == SV6621_SCAN_HT_SECONDARY_BELOW))
            {
              entry->ht_primary_channel = frame[offset];
              entry->ht_secondary_offset = secondary;
              entry->ht_operation_present = true;
            }
        }
      else if (id == SV6621_SCAN_IE_VHT_OPERATION &&
               ie_length >= SV6621_SCAN_VHT_OPERATION_MIN_SIZE)
        {
          uint8_t width = frame[offset];
          uint8_t segment0 = frame[offset + 1];

          if (width <= SV6621_SCAN_VHT_WIDTH_MAX &&
              (width == 0 || segment0 != 0))
            {
              entry->vht_channel_width = width;
              entry->vht_center_segment0 = segment0;
              entry->vht_center_segment1 = frame[offset + 2];
              entry->vht_operation_present = true;
            }
        }
      else if (id == SV6621_SCAN_IE_VENDOR &&
               sv6621_scan_is_wpa_ie(frame + offset, ie_length))
        {
          legacy_security = true;
        }

      if (entry->ie_length + 2 + ie_length <= SV6621_SCAN_IE_CAPACITY)
        {
          entry->ies[entry->ie_length++] = id;
          entry->ies[entry->ie_length++] = ie_length;
          memcpy(entry->ies + entry->ie_length, frame + offset, ie_length);
          entry->ie_length += ie_length;
        }
      else
        {
          entry->ies_truncated = true;
        }

      offset += ie_length;
    }

  if (sae && psk)
    {
      bss->security = SV6621_SECURITY_WPA2_WPA3_PSK;
    }
  else if (sae)
    {
      bss->security = SV6621_SECURITY_WPA3_SAE;
    }
  else if (psk)
    {
      bss->security = SV6621_SECURITY_WPA2_PSK;
    }
  else if (legacy_security || privacy)
    {
      bss->security = SV6621_SECURITY_LEGACY;
    }
  else
    {
      bss->security = SV6621_SECURITY_OPEN;
    }

  return 0;
}

int sv6621_scan_cache_init(FAR struct sv6621_scan_cache_s *cache)
{
  if (cache == NULL)
    {
      return -EINVAL;
    }

  memset(cache, 0, sizeof(*cache));
  return nxmutex_init(&cache->lock);
}

void sv6621_scan_cache_deinit(FAR struct sv6621_scan_cache_s *cache)
{
  if (cache != NULL)
    {
      nxmutex_destroy(&cache->lock);
    }
}

int sv6621_scan_cache_reset(FAR struct sv6621_scan_cache_s *cache)
{
  int ret;

  if (cache == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&cache->lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(cache->entries, 0, sizeof(cache->entries));
  cache->count = 0;
  nxmutex_unlock(&cache->lock);
  return 0;
}

int sv6621_scan_cache_store(FAR struct sv6621_scan_cache_s *cache,
                            FAR const struct sv6621_scan_entry_s *entry,
                            FAR bool *inserted)
{
  size_t index;
  int ret;

  if (cache == NULL || entry == NULL || inserted == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&cache->lock);
  if (ret < 0)
    {
      return ret;
    }

  *inserted = false;
  for (index = 0; index < cache->count; index++)
    {
      if (memcmp(cache->entries[index].bss.bssid, entry->bss.bssid,
                 SV6621_MAC_LENGTH) == 0)
        {
          if (entry->bss.ssid_length == 0 &&
              cache->entries[index].bss.ssid_length > 0)
            {
              struct sv6621_scan_entry_s updated = *entry;

              updated.bss.ssid_length =
                  cache->entries[index].bss.ssid_length;
              memcpy(updated.bss.ssid, cache->entries[index].bss.ssid,
                     updated.bss.ssid_length);
              cache->entries[index] = updated;
            }
          else
            {
              cache->entries[index] = *entry;
            }

          nxmutex_unlock(&cache->lock);
          return 0;
        }
    }

  if (cache->count < SV6621_SCAN_CACHE_CAPACITY)
    {
      cache->entries[cache->count++] = *entry;
      *inserted = true;
    }
  else
    {
      index = sv6621_scan_cache_weakest(cache);
      if (entry->bss.signal_dbm <= cache->entries[index].bss.signal_dbm)
        {
          cache->dropped++;
          ret = -ENOSPC;
          goto unlock_cache;
        }

      cache->entries[index] = *entry;
      cache->replacements++;
      *inserted = true;
    }

  ret = 0;

unlock_cache:
  nxmutex_unlock(&cache->lock);
  return ret;
}

int sv6621_scan_cache_snapshot(FAR struct sv6621_scan_cache_s *cache,
                               FAR struct sv6621_bss_s *entries,
                               FAR size_t *count)
{
  size_t copy_count;
  int ret;

  if (cache == NULL || count == NULL || (entries == NULL && *count != 0))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&cache->lock);
  if (ret < 0)
    {
      return ret;
    }

  copy_count = *count < cache->count ? *count : cache->count;
  if (copy_count > 0)
    {
      size_t index;

      for (index = 0; index < copy_count; index++)
        {
          entries[index] = cache->entries[index].bss;
        }
    }

  *count = cache->count;
  nxmutex_unlock(&cache->lock);
  return copy_count < cache->count ? -ENOSPC : 0;
}

int sv6621_scan_cache_find(FAR struct sv6621_scan_cache_s *cache,
                           FAR const struct sv6621_connect_s *request,
                           FAR struct sv6621_scan_entry_s *entry)
{
  size_t index;
  int ret;

  if (cache == NULL || request == NULL || entry == NULL ||
      request->ssid_length > SV6621_SSID_MAX_LENGTH ||
      (request->ssid_length == 0 && !request->bssid_valid))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&cache->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = -ENOENT;
  for (index = 0; index < cache->count; index++)
    {
      FAR const struct sv6621_bss_s *bss = &cache->entries[index].bss;

      if (request->ssid_length != 0 &&
          (bss->ssid_length != request->ssid_length ||
           memcmp(bss->ssid, request->ssid, request->ssid_length) != 0))
        {
          continue;
        }

      if (request->bssid_valid &&
          memcmp(bss->bssid, request->bssid, SV6621_MAC_LENGTH) != 0)
        {
          continue;
        }

      if (request->channel != 0 && bss->channel != request->channel)
        {
          continue;
        }

      if (!sv6621_scan_security_matches(request->security, bss->security))
        {
          continue;
        }

      if (!sv6621_scan_connection_supported(request->security,
                                             &cache->entries[index]))
        {
          continue;
        }

      if (ret == -ENOENT ||
          bss->signal_dbm > entry->bss.signal_dbm)
        {
          *entry = cache->entries[index];
          ret = 0;
        }

      if (request->bssid_valid)
        {
          break;
        }
    }

  nxmutex_unlock(&cache->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_scan_cache_find_roam_candidate
 ****************************************************************************/

int sv6621_scan_cache_find_roam_candidate(
    FAR struct sv6621_scan_cache_s *cache,
    FAR const struct sv6621_connect_s *request,
    FAR const uint8_t current_bssid[SV6621_MAC_LENGTH],
    int16_t current_signal_dbm, uint8_t minimum_gain_db,
    FAR struct sv6621_scan_entry_s *entry)
{
  int32_t required_signal_dbm;
  size_t index;
  int ret;

  if (cache == NULL || request == NULL || current_bssid == NULL ||
      entry == NULL || request->ssid_length == 0 || request->bssid_valid)
    {
      return -EINVAL;
    }

  required_signal_dbm = (int32_t)current_signal_dbm + minimum_gain_db;
  ret = nxmutex_lock(&cache->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = -ENOENT;
  for (index = 0; index < cache->count; index++)
    {
      FAR const struct sv6621_scan_entry_s *candidate =
          &cache->entries[index];
      FAR const struct sv6621_bss_s *bss = &candidate->bss;

      if (memcmp(bss->bssid, current_bssid, SV6621_MAC_LENGTH) == 0 ||
          bss->ssid_length != request->ssid_length ||
          memcmp(bss->ssid, request->ssid, request->ssid_length) != 0 ||
          !sv6621_scan_security_matches(request->security, bss->security) ||
          !sv6621_scan_connection_supported(request->security, candidate) ||
          bss->signal_dbm < required_signal_dbm)
        {
          continue;
        }

      if (ret == -ENOENT || bss->signal_dbm > entry->bss.signal_dbm)
        {
          *entry = *candidate;
          ret = 0;
        }
    }

  nxmutex_unlock(&cache->lock);
  return ret;
}

int sv6621_scan_controller_init(FAR struct sv6621_scan_s *scan,
                                FAR struct sv6621_command_engine_s *command,
                                FAR struct sv6621_rx_s *rx,
                                uint32_t timeout_ms,
                                sv6621_scan_complete_t complete,
                                FAR void *complete_arg)
{
  int ret;

  if (scan == NULL || command == NULL || rx == NULL || timeout_ms == 0)
    {
      return -EINVAL;
    }

  memset(scan, 0, sizeof(*scan));
  ret = nxmutex_init(&scan->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_scan_cache_init(&scan->cache);
  if (ret < 0)
    {
      nxmutex_destroy(&scan->lock);
      return ret;
    }

  scan->command = command;
  scan->rx = rx;
  scan->timeout_ms = timeout_ms;
  scan->complete = complete;
  scan->complete_arg = complete_arg;
  return 0;
}

void sv6621_scan_controller_deinit(FAR struct sv6621_scan_s *scan)
{
  if (scan != NULL)
    {
      work_cancel_sync(LPWORK, &scan->timeout_work);
      sv6621_scan_cache_deinit(&scan->cache);
      nxmutex_destroy(&scan->lock);
    }
}

int sv6621_scan_controller_begin(
    FAR struct sv6621_scan_s *scan,
    FAR const struct sv6621_scan_channel_s *channels, size_t channel_count,
    FAR const uint8_t *ssid, size_t ssid_length)
{
  int ret;

  if (scan == NULL || channels == NULL || channel_count == 0 ||
      channel_count > SV6621_SCAN_MAX_CHANNELS ||
      ssid_length > SV6621_SSID_MAX_LENGTH ||
      (ssid_length != 0 && ssid == NULL))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&scan->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (scan->active || scan->stopping)
    {
      nxmutex_unlock(&scan->lock);
      return -EBUSY;
    }

  ret = sv6621_scan_cache_reset(&scan->cache);
  if (ret < 0)
    {
      nxmutex_unlock(&scan->lock);
      return ret;
    }

  scan->active = true;
  scan->stopping = false;
  scan->recovery_pending = false;
  memcpy(scan->channels, channels, channel_count * sizeof(*channels));
  scan->channel_count = channel_count;
  scan->ssid_length = ssid_length;
  if (ssid_length > 0)
    {
      memcpy(scan->ssid, ssid, ssid_length);
    }

  scan->stats.started++;
  nxmutex_unlock(&scan->lock);

  ret = sv6621_scan_start(scan->command, scan->channels,
                          scan->channel_count,
                          scan->ssid_length > 0 ? scan->ssid : NULL,
                          scan->ssid_length);
  if (ret == 0)
    {
      ret = sv6621_scan_queue_timeout(scan);
    }

  if (ret < 0 && nxmutex_lock(&scan->lock) >= 0)
    {
      scan->active = false;
      nxmutex_unlock(&scan->lock);
    }

  return ret;
}

int sv6621_scan_controller_cancel(FAR struct sv6621_scan_s *scan)
{
  bool active;
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

  active = scan->active;
  scan->active = false;
  if (active)
    {
      scan->stopping = true;
      scan->stats.cancelled++;
    }

  nxmutex_unlock(&scan->lock);
  if (!active)
    {
      return 0;
    }

  work_cancel_sync(LPWORK, &scan->timeout_work);
  ret = sv6621_scan_stop(scan->command);
  if (nxmutex_lock(&scan->lock) >= 0)
    {
      scan->stopping = false;
      nxmutex_unlock(&scan->lock);
    }

  return ret;
}

void sv6621_scan_command_event(uint8_t instance, uint8_t id,
                               FAR const uint8_t *payload, size_t length,
                               FAR void *arg)
{
  FAR struct sv6621_scan_s *scan = arg;
  FAR struct sv6621_scan_entry_s *entry;
  sv6621_scan_complete_t complete;
  FAR void *complete_arg;
  bool inserted;

  if (scan == NULL || instance != SV6621_SCAN_INSTANCE)
    {
      return;
    }

  if (nxmutex_lock(&scan->lock) < 0)
    {
      return;
    }

  if (!scan->active)
    {
      if (id == SV6621_SCAN_EVENT_COMPLETE || id == SV6621_SCAN_EVENT_REPORT)
        {
          scan->stats.late_events++;
        }

      nxmutex_unlock(&scan->lock);
      return;
    }

  if (id == SV6621_SCAN_EVENT_COMPLETE)
    {
      scan->recovery_pending = false;
      scan->stats.completed++;
      scan->active = false;
      scan->stopping = true;
      complete = scan->complete;
      complete_arg = scan->complete_arg;

      nxmutex_unlock(&scan->lock);
      work_cancel(LPWORK, &scan->timeout_work);
      if (nxmutex_lock(&scan->lock) >= 0)
        {
          scan->stopping = false;
          nxmutex_unlock(&scan->lock);
        }

      if (complete != NULL)
        {
          complete(0, complete_arg);
        }

      return;
    }

  nxmutex_unlock(&scan->lock);
  if (id != SV6621_SCAN_EVENT_REPORT)
    {
      return;
    }

  entry = kmm_malloc(sizeof(*entry));
  if (entry == NULL)
    {
      return;
    }

  if (sv6621_scan_parse_report(payload, length, entry) < 0)
    {
      if (nxmutex_lock(&scan->lock) >= 0)
        {
          scan->stats.malformed_reports++;
          nxmutex_unlock(&scan->lock);
        }

      kmm_free(entry);
      return;
    }

  if (entry->ies_truncated && nxmutex_lock(&scan->lock) >= 0)
    {
      scan->stats.truncated_reports++;
      nxmutex_unlock(&scan->lock);
    }

  sv6621_scan_cache_store(&scan->cache, entry, &inserted);
  kmm_free(entry);
}
