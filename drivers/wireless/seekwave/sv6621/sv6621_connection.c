/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_connection.c
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

#include "sv6621_connection.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_CONNECTION_INSTANCE           0
#define SV6621_CONNECTION_COMMAND_JOIN        9
#define SV6621_CONNECTION_COMMAND_AUTH        10
#define SV6621_CONNECTION_COMMAND_ASSOC       11
#define SV6621_CONNECTION_COMMAND_DISCONNECT  17
#define SV6621_CONNECTION_COMMAND_TIMEOUT_MS  5000
#define SV6621_CONNECTION_JOIN_HEADER_SIZE    25
#define SV6621_CONNECTION_JOIN_RESPONSE_SIZE  4
#define SV6621_CONNECTION_JOIN_BANDWIDTH_20MHZ 0
#define SV6621_CONNECTION_AUTH_HEADER_SIZE     14
#define SV6621_CONNECTION_AUTH_MAX_DATA_SIZE   512
#define SV6621_CONNECTION_ASSOC_HEADER_SIZE    54
#define SV6621_CONNECTION_ASSOC_BSSID_OFFSET   38
#define SV6621_CONNECTION_ASSOC_IE_OFFSET_OFFSET  50
#define SV6621_CONNECTION_ASSOC_IE_LENGTH_OFFSET  52
#define SV6621_CONNECTION_ASSOC_MAX_IE_SIZE       1024
#define SV6621_CONNECTION_DISCONNECT_HEADER_SIZE  8
#define SV6621_CONNECTION_DISCONNECT_MAX_IE_SIZE  512
#define SV6621_CONNECTION_IE_SUPPORTED_RATES       1
#define SV6621_CONNECTION_IE_RSN                   48
#define SV6621_CONNECTION_IE_EXT_SUPPORTED_RATES   50

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_connection_rsn_psk_ccmp[] = {
  SV6621_CONNECTION_IE_RSN, 20, 1, 0, 0x00, 0x0f, 0xac, 4,
  1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_connection_put_le16(FAR uint8_t *output, uint16_t value);
static void sv6621_connection_put_le32(FAR uint8_t *output, uint32_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_connection_put_le16
 ****************************************************************************/

static void sv6621_connection_put_le16(FAR uint8_t *output, uint16_t value)
{
  output[0] = value & 0xff;
  output[1] = value >> 8;
}

/****************************************************************************
 * Name: sv6621_connection_put_le32
 ****************************************************************************/

static void sv6621_connection_put_le32(FAR uint8_t *output, uint32_t value)
{
  output[0] = value & 0xff;
  output[1] = (value >> 8) & 0xff;
  output[2] = (value >> 16) & 0xff;
  output[3] = value >> 24;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_connection_join
 ****************************************************************************/

int sv6621_connection_join(FAR struct sv6621_command_engine_s *command,
                           FAR const struct sv6621_scan_entry_s *entry,
                           FAR struct sv6621_connection_peer_s *peer)
{
  uint8_t response[SV6621_CONNECTION_JOIN_RESPONSE_SIZE];
  FAR uint8_t *payload;
  size_t payload_length;
  size_t response_length = sizeof(response);
  int ret;

  if (command == NULL || entry == NULL || peer == NULL ||
      entry->bss.channel == 0 || entry->ie_length > SV6621_SCAN_IE_CAPACITY)
    {
      return -EINVAL;
    }

  payload_length = SV6621_CONNECTION_JOIN_HEADER_SIZE + entry->ie_length;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  payload[0] = entry->bss.channel;
  payload[1] = entry->bss.channel;
  payload[3] = SV6621_CONNECTION_JOIN_BANDWIDTH_20MHZ;
  payload[4] = entry->bss.band == SV6621_BAND_2GHZ ? 0 : 1;
  sv6621_connection_put_le16(payload + 5, entry->beacon_interval);
  sv6621_connection_put_le16(payload + 7, entry->capability);
  payload[9] = entry->bssid_index;
  payload[10] = entry->max_bssid_indicator;
  memcpy(payload + 11, entry->bss.bssid, SV6621_MAC_LENGTH);
  sv6621_connection_put_le16(payload + 19,
                             SV6621_CONNECTION_JOIN_HEADER_SIZE);
  sv6621_connection_put_le32(payload + 21, entry->ie_length);
  memcpy(payload + SV6621_CONNECTION_JOIN_HEADER_SIZE, entry->ies,
         entry->ie_length);

  ret = sv6621_command_execute(
      command, SV6621_CONNECTION_INSTANCE, SV6621_CONNECTION_COMMAND_JOIN,
      payload, payload_length, response, &response_length,
      SV6621_CONNECTION_COMMAND_TIMEOUT_MS);
  kmm_free(payload);
  if (ret != 0)
    {
      return ret < 0 ? ret : -EREMOTEIO;
    }

  if (response_length != sizeof(response))
    {
      return -EPROTO;
    }

  peer->peer_index = response[0];
  peer->lmac_id = response[1];
  peer->instance = response[2];
  peer->multicast_index = response[3];
  return 0;
}

/****************************************************************************
 * Name: sv6621_connection_authenticate
 ****************************************************************************/

int sv6621_connection_authenticate(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_connection_auth_algorithm_e algorithm,
    FAR const uint8_t *auth_data, size_t auth_data_length,
    FAR const uint8_t *auth_ies, size_t auth_ies_length)
{
  FAR uint8_t *payload;
  size_t payload_length;
  size_t offset;
  int ret;

  if (command == NULL || algorithm < SV6621_CONNECTION_AUTH_OPEN ||
      algorithm > SV6621_CONNECTION_AUTH_SAE ||
      (auth_data == NULL && auth_data_length != 0) ||
      (auth_ies == NULL && auth_ies_length != 0) ||
      auth_data_length > SV6621_CONNECTION_AUTH_MAX_DATA_SIZE ||
      auth_ies_length > SV6621_CONNECTION_AUTH_MAX_DATA_SIZE)
    {
      return -EINVAL;
    }

  payload_length = SV6621_CONNECTION_AUTH_HEADER_SIZE + auth_data_length +
                   auth_ies_length;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  sv6621_connection_put_le16(payload, algorithm);
  offset = SV6621_CONNECTION_AUTH_HEADER_SIZE;
  if (auth_data_length > 0)
    {
      sv6621_connection_put_le16(payload + 6, offset);
      sv6621_connection_put_le16(payload + 8, auth_data_length);
      memcpy(payload + offset, auth_data, auth_data_length);
      offset += auth_data_length;
    }

  if (auth_ies_length > 0)
    {
      sv6621_connection_put_le16(payload + 10, offset);
      sv6621_connection_put_le16(payload + 12, auth_ies_length);
      memcpy(payload + offset, auth_ies, auth_ies_length);
    }

  ret = sv6621_command_execute(
      command, SV6621_CONNECTION_INSTANCE, SV6621_CONNECTION_COMMAND_AUTH,
      payload, payload_length, NULL, NULL,
      SV6621_CONNECTION_COMMAND_TIMEOUT_MS);
  kmm_free(payload);
  return ret == 0 ? 0 : (ret < 0 ? ret : -EREMOTEIO);
}

/****************************************************************************
 * Name: sv6621_connection_associate
 ****************************************************************************/

int sv6621_connection_associate(
    FAR struct sv6621_command_engine_s *command,
    FAR const uint8_t bssid[SV6621_MAC_LENGTH],
    FAR const uint8_t ht_capability[SV6621_CONNECTION_HT_CAPABILITY_SIZE],
    FAR const uint8_t vht_capability[SV6621_CONNECTION_VHT_CAPABILITY_SIZE],
    FAR const uint8_t *ies, size_t ies_length)
{
  FAR uint8_t *payload;
  size_t payload_length;
  int ret;

  if (command == NULL || bssid == NULL || ht_capability == NULL ||
      vht_capability == NULL || (ies == NULL && ies_length != 0) ||
      ies_length > SV6621_CONNECTION_ASSOC_MAX_IE_SIZE)
    {
      return -EINVAL;
    }

  payload_length = SV6621_CONNECTION_ASSOC_HEADER_SIZE + ies_length;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  memcpy(payload, ht_capability, SV6621_CONNECTION_HT_CAPABILITY_SIZE);
  memcpy(payload + SV6621_CONNECTION_HT_CAPABILITY_SIZE, vht_capability,
         SV6621_CONNECTION_VHT_CAPABILITY_SIZE);
  memcpy(payload + SV6621_CONNECTION_ASSOC_BSSID_OFFSET, bssid,
         SV6621_MAC_LENGTH);
  sv6621_connection_put_le16(
      payload + SV6621_CONNECTION_ASSOC_IE_OFFSET_OFFSET,
      SV6621_CONNECTION_ASSOC_HEADER_SIZE);
  sv6621_connection_put_le16(
      payload + SV6621_CONNECTION_ASSOC_IE_LENGTH_OFFSET, ies_length);
  if (ies_length > 0)
    {
      memcpy(payload + SV6621_CONNECTION_ASSOC_HEADER_SIZE, ies, ies_length);
    }

  ret = sv6621_command_execute(
      command, SV6621_CONNECTION_INSTANCE, SV6621_CONNECTION_COMMAND_ASSOC,
      payload, payload_length, NULL, NULL,
      SV6621_CONNECTION_COMMAND_TIMEOUT_MS);
  kmm_free(payload);
  return ret == 0 ? 0 : (ret < 0 ? ret : -EREMOTEIO);
}

/****************************************************************************
 * Name: sv6621_connection_disconnect
 ****************************************************************************/

int sv6621_connection_disconnect(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_connection_disconnect_mode_e mode, bool local_state_change,
    uint16_t reason, FAR const uint8_t *ies, size_t ies_length)
{
  FAR uint8_t *payload;
  size_t payload_length;
  int ret;

  if (command == NULL || mode < SV6621_CONNECTION_DISCONNECT_ONLY ||
      mode > SV6621_CONNECTION_DISCONNECT_DEAUTH ||
      (ies == NULL && ies_length != 0) ||
      ies_length > SV6621_CONNECTION_DISCONNECT_MAX_IE_SIZE)
    {
      return -EINVAL;
    }

  payload_length = SV6621_CONNECTION_DISCONNECT_HEADER_SIZE + ies_length;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  payload[0] = mode;
  payload[1] = local_state_change;
  sv6621_connection_put_le16(payload + 2, reason);
  if (ies_length > 0)
    {
      sv6621_connection_put_le16(
          payload + 4, SV6621_CONNECTION_DISCONNECT_HEADER_SIZE);
      sv6621_connection_put_le16(payload + 6, ies_length);
      memcpy(payload + SV6621_CONNECTION_DISCONNECT_HEADER_SIZE, ies,
             ies_length);
    }

  ret = sv6621_command_execute(
      command, SV6621_CONNECTION_INSTANCE,
      SV6621_CONNECTION_COMMAND_DISCONNECT, payload, payload_length, NULL,
      NULL, SV6621_CONNECTION_COMMAND_TIMEOUT_MS);
  kmm_free(payload);
  return ret == 0 ? 0 : (ret < 0 ? ret : -EREMOTEIO);
}

/****************************************************************************
 * Name: sv6621_connection_build_association_ies
 ****************************************************************************/

int sv6621_connection_build_association_ies(
    FAR const struct sv6621_scan_entry_s *entry,
    enum sv6621_security_e security, FAR uint8_t *ies, size_t capacity,
    FAR size_t *length)
{
  size_t input = 0;
  size_t output = 0;

  if (entry == NULL || ies == NULL || length == NULL || capacity == 0 ||
      entry->ie_length > SV6621_SCAN_IE_CAPACITY)
    {
      return -EINVAL;
    }

  if (security == SV6621_SECURITY_WPA3_SAE)
    {
      return -EOPNOTSUPP;
    }

  if (security != SV6621_SECURITY_OPEN &&
      security != SV6621_SECURITY_WPA2_PSK &&
      security != SV6621_SECURITY_WPA2_WPA3_PSK)
    {
      return -EINVAL;
    }

  while (input < entry->ie_length)
    {
      uint8_t id;
      uint8_t ie_length;
      size_t encoded_length;

      if (entry->ie_length - input < 2)
        {
          return -EPROTO;
        }

      id = entry->ies[input];
      ie_length = entry->ies[input + 1];
      encoded_length = (size_t)ie_length + 2;
      if (encoded_length > entry->ie_length - input)
        {
          return -EPROTO;
        }

      if (id == SV6621_CONNECTION_IE_SUPPORTED_RATES ||
          id == SV6621_CONNECTION_IE_EXT_SUPPORTED_RATES)
        {
          if (encoded_length > capacity - output)
            {
              return -ENOSPC;
            }

          memcpy(ies + output, entry->ies + input, encoded_length);
          output += encoded_length;
        }

      input += encoded_length;
    }

  if (security != SV6621_SECURITY_OPEN)
    {
      if (sizeof(g_sv6621_connection_rsn_psk_ccmp) > capacity - output)
        {
          return -ENOSPC;
        }

      memcpy(ies + output, g_sv6621_connection_rsn_psk_ccmp,
             sizeof(g_sv6621_connection_rsn_psk_ccmp));
      output += sizeof(g_sv6621_connection_rsn_psk_ccmp);
    }

  if (output == 0)
    {
      return -EPROTO;
    }

  *length = output;
  return 0;
}
