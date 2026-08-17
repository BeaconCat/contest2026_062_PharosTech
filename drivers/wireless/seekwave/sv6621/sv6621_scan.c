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
#define SV6621_SCAN_MAX_CHANNELS         64
#define SV6621_SCAN_COMMAND_TIMEOUT_MS   5000

#define SV6621_SCAN_CHANNEL_COUNT_OFFSET 8
#define SV6621_SCAN_CHANNEL_LIST_OFFSET  12
#define SV6621_SCAN_SSID_COUNT_OFFSET    16
#define SV6621_SCAN_SSID_LIST_OFFSET     20
#define SV6621_SCAN_IE_LENGTH_OFFSET     24
#define SV6621_SCAN_IE_OFFSET_OFFSET     28

#define SV6621_SCAN_REPORT_HEADER_SIZE   8
#define SV6621_SCAN_INFORMATION_OFFSET   36
#define SV6621_SCAN_BSSID_OFFSET         16
#define SV6621_SCAN_CAPABILITY_OFFSET    34
#define SV6621_SCAN_CAPABILITY_PRIVACY   (1 << 4)
#define SV6621_SCAN_FRAME_SUBTYPE_MASK   0xfc
#define SV6621_SCAN_FRAME_BEACON         0x80
#define SV6621_SCAN_FRAME_PROBE_RESPONSE 0x50
#define SV6621_SCAN_IE_SSID              0
#define SV6621_SCAN_IE_RSN               48
#define SV6621_SCAN_IE_VENDOR            221
#define SV6621_SCAN_RSN_SUITE_SIZE       4
#define SV6621_SCAN_RSN_AKM_PSK          2
#define SV6621_SCAN_RSN_AKM_SAE          8

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_scan_put_le32(FAR uint8_t *value, uint32_t number);
static uint16_t sv6621_scan_get_le16(FAR const uint8_t *value);
static int sv6621_scan_parse_rsn(FAR const uint8_t *data, size_t length,
                                 FAR bool *psk, FAR bool *sae);
static bool sv6621_scan_is_wpa_ie(FAR const uint8_t *data, size_t length);

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

static int sv6621_scan_parse_rsn(FAR const uint8_t *data, size_t length,
                                 FAR bool *psk, FAR bool *sae)
{
  uint16_t suite_count;
  size_t offset;
  unsigned int index;

  if (length < 8 || sv6621_scan_get_le16(data) != 1)
    {
      return -EPROTO;
    }

  suite_count = sv6621_scan_get_le16(data + 6);
  offset = 8 + (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE;
  if (offset + 2 > length)
    {
      return -EPROTO;
    }

  suite_count = sv6621_scan_get_le16(data + offset);
  offset += 2;
  if (offset + (size_t)suite_count * SV6621_SCAN_RSN_SUITE_SIZE > length)
    {
      return -EPROTO;
    }

  for (index = 0; index < suite_count; index++)
    {
      FAR const uint8_t *suite =
          data + offset + index * SV6621_SCAN_RSN_SUITE_SIZE;

      if (suite[0] != 0x00 || suite[1] != 0x0f || suite[2] != 0xac)
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

  return 0;
}

static bool sv6621_scan_is_wpa_ie(FAR const uint8_t *data, size_t length)
{
  static const uint8_t wpa_type[] = { 0x00, 0x50, 0xf2, 0x01 };

  return length >= sizeof(wpa_type) &&
         memcmp(data, wpa_type, sizeof(wpa_type)) == 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_scan_start(FAR struct sv6621_command_engine_s *command,
                      FAR const struct sv6621_scan_channel_s *channels,
                      size_t channel_count)
{
  FAR uint8_t *payload;
  size_t payload_length;
  size_t index;
  int ret;

  if (command == NULL || channels == NULL || channel_count == 0 ||
      channel_count > SV6621_SCAN_MAX_CHANNELS)
    {
      return -EINVAL;
    }

  payload_length =
      SV6621_SCAN_FIXED_SIZE + channel_count * SV6621_SCAN_CHANNEL_SIZE;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_COUNT_OFFSET,
                       channel_count);
  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_LIST_OFFSET,
                       SV6621_SCAN_FIXED_SIZE);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_COUNT_OFFSET, 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_LIST_OFFSET, 0);
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
                               SV6621_SCAN_COMMAND_TIMEOUT_MS);
  return ret > 0 ? -EREMOTEIO : ret;
}

int sv6621_scan_parse_report(FAR const uint8_t *payload, size_t length,
                             FAR struct sv6621_bss_s *bss)
{
  FAR const uint8_t *frame;
  uint16_t frame_length;
  uint16_t capability;
  size_t offset;
  bool psk = false;
  bool sae = false;
  bool privacy;

  if (payload == NULL || bss == NULL ||
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

  memset(bss, 0, sizeof(*bss));
  memcpy(bss->bssid, frame + SV6621_SCAN_BSSID_OFFSET, SV6621_MAC_LENGTH);
  bss->channel = payload[0];
  bss->band = payload[1] == SV6621_SCAN_BAND_2GHZ ? SV6621_BAND_2GHZ
                                                  : SV6621_BAND_5GHZ;
  bss->signal_dbm = (int16_t)sv6621_scan_get_le16(payload + 2);
  capability = sv6621_scan_get_le16(frame + SV6621_SCAN_CAPABILITY_OFFSET);
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
          if (sv6621_scan_parse_rsn(frame + offset, ie_length, &psk, &sae) < 0)
            {
              return -EPROTO;
            }
        }
      else if (id == SV6621_SCAN_IE_VENDOR &&
               sv6621_scan_is_wpa_ie(frame + offset, ie_length))
        {
          psk = true;
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
  else if (psk || privacy)
    {
      bss->security = SV6621_SECURITY_WPA2_PSK;
    }
  else
    {
      bss->security = SV6621_SECURITY_OPEN;
    }

  return 0;
}
