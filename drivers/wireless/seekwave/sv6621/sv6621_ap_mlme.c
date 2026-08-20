/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_mlme.c
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

#include "sv6621_ap_mlme.h"
#include "sv6621_management.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_MGMT_EVENT_HEADER_SIZE    8
#define SV6621_AP_MGMT_FRAME_HEADER_SIZE   24
#define SV6621_AP_MGMT_DESTINATION_OFFSET   4
#define SV6621_AP_MGMT_SOURCE_OFFSET       10
#define SV6621_AP_MGMT_BSSID_OFFSET        16
#define SV6621_AP_MGMT_SUBTYPE_MASK    0x00fc
#define SV6621_AP_FRAME_ASSOC_REQUEST   0x0000
#define SV6621_AP_FRAME_REASSOC_REQUEST 0x0020
#define SV6621_AP_FRAME_PROBE_REQUEST   0x0040
#define SV6621_AP_FRAME_DISASSOC        0x00a0
#define SV6621_AP_FRAME_AUTH            0x00b0
#define SV6621_AP_FRAME_DEAUTH          0x00c0
#define SV6621_AP_FRAME_ACTION          0x00d0
#define SV6621_AP_MGMT_ASSOC_FIXED_SIZE    28
#define SV6621_AP_MGMT_REASSOC_FIXED_SIZE  34
#define SV6621_AP_MGMT_AUTH_FIXED_SIZE     30
#define SV6621_AP_MGMT_REASON_FIXED_SIZE   26
#define SV6621_AP_MGMT_AUTH_RESPONSE_SIZE  30
#define SV6621_AP_MGMT_ASSOC_RESPONSE_SIZE 30
#define SV6621_AP_MGMT_RESPONSE_MAX_SIZE 1024
#define SV6621_AP_AUTH_OPEN                  0
#define SV6621_AP_AUTH_REQUEST_TRANSACTION   1
#define SV6621_AP_AUTH_RESPONSE_TRANSACTION  2
#define SV6621_AP_STATUS_SUCCESS             0
#define SV6621_AP_STATUS_UNSPECIFIED         1
#define SV6621_AP_STATUS_AUTH_UNSUPPORTED   13
#define SV6621_AP_STATUS_AUTH_TRANSACTION   14
#define SV6621_AP_STATUS_UNSUPPORTED_RATES  18
#define SV6621_AP_REASON_LEAVING             3
#define SV6621_AP_IE_SSID                     0
#define SV6621_AP_IE_SUPPORTED_RATES          1
#define SV6621_AP_IE_EXTENDED_RATES          50
#define SV6621_AP_IE_HT_CAPABILITY            45
#define SV6621_AP_IE_HT_OPERATION             61
#define SV6621_AP_IE_EXTENDED_CAPABILITY     127
#define SV6621_AP_IE_VHT_CAPABILITY          191
#define SV6621_AP_IE_VHT_OPERATION           192
#define SV6621_AP_IE_VENDOR                   221
#define SV6621_AP_IE_EXTENSION                255

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_ap_mlme_get_le16(FAR const uint8_t *value);
static void sv6621_ap_mlme_set_ies(FAR struct sv6621_ap_mgmt_s *event,
                                  size_t offset);
static void sv6621_ap_mlme_build_auth_response(
    FAR uint8_t response[SV6621_AP_MGMT_AUTH_RESPONSE_SIZE],
    FAR const uint8_t destination[SV6621_MAC_LENGTH],
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH], uint16_t algorithm,
    uint16_t transaction, uint16_t status);
static bool sv6621_ap_mlme_rate_supported(enum sv6621_band_e band,
                                          uint8_t rate);
static bool sv6621_ap_mlme_assoc_response_ie(uint8_t identifier);
static int sv6621_ap_mlme_copy_response_ies(
    FAR uint8_t *response, size_t capacity, FAR size_t *response_length,
    FAR const uint8_t *ies, size_t ies_length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_ap_mlme_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static void sv6621_ap_mlme_set_ies(FAR struct sv6621_ap_mgmt_s *event,
                                  size_t offset)
{
  event->information_elements = event->frame + offset;
  event->information_element_length = event->frame_length - offset;
}

static void sv6621_ap_mlme_build_auth_response(
    FAR uint8_t response[SV6621_AP_MGMT_AUTH_RESPONSE_SIZE],
    FAR const uint8_t destination[SV6621_MAC_LENGTH],
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH], uint16_t algorithm,
    uint16_t transaction, uint16_t status)
{
  memset(response, 0, SV6621_AP_MGMT_AUTH_RESPONSE_SIZE);
  response[0] = SV6621_AP_FRAME_AUTH;
  memcpy(response + SV6621_AP_MGMT_DESTINATION_OFFSET, destination,
         SV6621_MAC_LENGTH);
  memcpy(response + SV6621_AP_MGMT_SOURCE_OFFSET, ap_address,
         SV6621_MAC_LENGTH);
  memcpy(response + SV6621_AP_MGMT_BSSID_OFFSET, ap_address,
         SV6621_MAC_LENGTH);
  response[24] = algorithm;
  response[25] = algorithm >> 8;
  response[26] = transaction;
  response[27] = transaction >> 8;
  response[28] = status;
  response[29] = status >> 8;
}

static bool sv6621_ap_mlme_rate_supported(enum sv6621_band_e band,
                                          uint8_t rate)
{
  static const uint8_t rates_2ghz[] =
  {
    2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108
  };
  static const uint8_t rates_5ghz[] =
  {
    12, 18, 24, 36, 48, 72, 96, 108
  };
  FAR const uint8_t *rates = band == SV6621_BAND_2GHZ ? rates_2ghz :
                                                              rates_5ghz;
  size_t count = band == SV6621_BAND_2GHZ ? sizeof(rates_2ghz) :
                                            sizeof(rates_5ghz);
  size_t index;

  rate &= 0x7f;
  for (index = 0; index < count; index++)
    {
      if (rates[index] == rate)
        {
          return true;
        }
    }

  return false;
}

static bool sv6621_ap_mlme_assoc_response_ie(uint8_t identifier)
{
  return identifier == SV6621_AP_IE_SUPPORTED_RATES ||
         identifier == SV6621_AP_IE_EXTENDED_RATES ||
         identifier == SV6621_AP_IE_HT_CAPABILITY ||
         identifier == SV6621_AP_IE_HT_OPERATION ||
         identifier == SV6621_AP_IE_EXTENDED_CAPABILITY ||
         identifier == SV6621_AP_IE_VHT_CAPABILITY ||
         identifier == SV6621_AP_IE_VHT_OPERATION ||
         identifier == SV6621_AP_IE_VENDOR ||
         identifier == SV6621_AP_IE_EXTENSION;
}

static int sv6621_ap_mlme_copy_response_ies(
    FAR uint8_t *response, size_t capacity, FAR size_t *response_length,
    FAR const uint8_t *ies, size_t ies_length)
{
  while (ies_length != 0)
    {
      size_t element_length;

      if (ies_length < 2 || (size_t)ies[1] > ies_length - 2)
        {
          return -EPROTO;
        }

      element_length = (size_t)ies[1] + 2;
      if (sv6621_ap_mlme_assoc_response_ie(ies[0]))
        {
          if (*response_length > capacity ||
              element_length > capacity - *response_length)
            {
              return -ENOSPC;
            }

          memcpy(response + *response_length, ies, element_length);
          *response_length += element_length;
        }

      ies += element_length;
      ies_length -= element_length;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_parse_mgmt(FAR const uint8_t *payload, size_t payload_length,
                         FAR struct sv6621_ap_mgmt_s *event)
{
  FAR const uint8_t *frame;
  uint16_t frame_control;
  uint16_t frame_length;

  if (payload == NULL || event == NULL ||
      payload_length < SV6621_AP_MGMT_EVENT_HEADER_SIZE +
                       SV6621_AP_MGMT_FRAME_HEADER_SIZE ||
      payload[0] == 0 || payload[1] > SV6621_BAND_5GHZ)
    {
      return -EINVAL;
    }

  frame_length = sv6621_ap_mlme_get_le16(payload + 4);
  if (frame_length < SV6621_AP_MGMT_FRAME_HEADER_SIZE ||
      frame_length > payload_length - SV6621_AP_MGMT_EVENT_HEADER_SIZE)
    {
      return -EPROTO;
    }

  frame = payload + SV6621_AP_MGMT_EVENT_HEADER_SIZE;
  frame_control = sv6621_ap_mlme_get_le16(frame) &
                  SV6621_AP_MGMT_SUBTYPE_MASK;
  memset(event, 0, sizeof(*event));
  event->channel = payload[0];
  event->band = payload[1] == 0 ? SV6621_BAND_2GHZ : SV6621_BAND_5GHZ;
  event->signal_dbm = (int16_t)sv6621_ap_mlme_get_le16(payload + 2);
  memcpy(event->destination, frame + SV6621_AP_MGMT_DESTINATION_OFFSET,
         SV6621_MAC_LENGTH);
  memcpy(event->source, frame + SV6621_AP_MGMT_SOURCE_OFFSET,
         SV6621_MAC_LENGTH);
  memcpy(event->bssid, frame + SV6621_AP_MGMT_BSSID_OFFSET,
         SV6621_MAC_LENGTH);
  event->frame = frame;
  event->frame_length = frame_length;

  switch (frame_control)
    {
      case SV6621_AP_FRAME_AUTH:
        if (frame_length < SV6621_AP_MGMT_AUTH_FIXED_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_AP_MGMT_AUTH;
        event->algorithm = sv6621_ap_mlme_get_le16(frame + 24);
        event->transaction = sv6621_ap_mlme_get_le16(frame + 26);
        event->status = sv6621_ap_mlme_get_le16(frame + 28);
        sv6621_ap_mlme_set_ies(event, SV6621_AP_MGMT_AUTH_FIXED_SIZE);
        break;

      case SV6621_AP_FRAME_ASSOC_REQUEST:
      case SV6621_AP_FRAME_REASSOC_REQUEST:
        if (frame_length < (frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                                ? SV6621_AP_MGMT_ASSOC_FIXED_SIZE
                                : SV6621_AP_MGMT_REASSOC_FIXED_SIZE))
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                          ? SV6621_AP_MGMT_ASSOC_REQUEST
                          : SV6621_AP_MGMT_REASSOC_REQUEST;
        event->capability = sv6621_ap_mlme_get_le16(frame + 24);
        event->listen_interval = sv6621_ap_mlme_get_le16(frame + 26);
        sv6621_ap_mlme_set_ies(
            event, frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                       ? SV6621_AP_MGMT_ASSOC_FIXED_SIZE
                       : SV6621_AP_MGMT_REASSOC_FIXED_SIZE);
        break;

      case SV6621_AP_FRAME_PROBE_REQUEST:
        event->type = SV6621_AP_MGMT_PROBE_REQUEST;
        sv6621_ap_mlme_set_ies(event, SV6621_AP_MGMT_FRAME_HEADER_SIZE);
        break;

      case SV6621_AP_FRAME_DEAUTH:
      case SV6621_AP_FRAME_DISASSOC:
        if (frame_length < SV6621_AP_MGMT_REASON_FIXED_SIZE)
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_AP_FRAME_DEAUTH
                          ? SV6621_AP_MGMT_DEAUTH
                          : SV6621_AP_MGMT_DISASSOC;
        event->reason = sv6621_ap_mlme_get_le16(frame + 24);
        break;

      case SV6621_AP_FRAME_ACTION:
        event->type = SV6621_AP_MGMT_ACTION;
        break;

      default:
        return -ENOMSG;
    }

  return 0;
}

int sv6621_ap_authenticate_open(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    uint8_t channel, enum sv6621_band_e band,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH],
    FAR const struct sv6621_ap_mgmt_s *request, uint64_t cookie,
    FAR bool *accepted)
{
  struct sv6621_ap_peer_s peer;
  uint8_t response[SV6621_AP_MGMT_AUTH_RESPONSE_SIZE];
  uint16_t status = SV6621_AP_STATUS_SUCCESS;
  int ret;

  if (peers == NULL || command == NULL || ap_address == NULL ||
      request == NULL || accepted == NULL || channel == 0 ||
      band > SV6621_BAND_5GHZ || request->type != SV6621_AP_MGMT_AUTH)
    {
      return -EINVAL;
    }

  *accepted = false;
  if (memcmp(request->bssid, ap_address, SV6621_MAC_LENGTH) != 0 ||
      memcmp(request->destination, ap_address, SV6621_MAC_LENGTH) != 0)
    {
      status = SV6621_AP_STATUS_UNSPECIFIED;
    }
  else if (request->algorithm != SV6621_AP_AUTH_OPEN)
    {
      status = SV6621_AP_STATUS_AUTH_UNSUPPORTED;
    }
  else if (request->transaction != SV6621_AP_AUTH_REQUEST_TRANSACTION)
    {
      status = SV6621_AP_STATUS_AUTH_TRANSACTION;
    }
  else if (request->status != SV6621_AP_STATUS_SUCCESS)
    {
      return -ECONNREFUSED;
    }

  if (status == SV6621_AP_STATUS_SUCCESS)
    {
      ret = sv6621_ap_peer_authenticate(peers, request->source);
      if (ret < 0)
        {
          status = SV6621_AP_STATUS_UNSPECIFIED;
        }
      else
        {
          ret = sv6621_ap_peer_lookup(peers, request->source, &peer);
          if (ret < 0)
            {
              return ret;
            }

          if (!peer.bound)
            {
              ret = sv6621_ap_add_peer(command, instance, request->source,
                                       &peer.peer_index);
              if (ret < 0)
                {
                  sv6621_ap_peer_forget(peers, request->source, NULL);
                  status = SV6621_AP_STATUS_UNSPECIFIED;
                }
              else
                {
                  ret = sv6621_ap_peer_bind(peers, request->source,
                                            peer.peer_index);
                  if (ret < 0)
                    {
                      sv6621_ap_remove_peer(command, instance,
                                            request->source,
                                            SV6621_AP_REASON_LEAVING, false);
                      sv6621_ap_peer_forget(peers, request->source, NULL);
                      status = SV6621_AP_STATUS_UNSPECIFIED;
                    }
                }
            }
        }
    }

  sv6621_ap_mlme_build_auth_response(
      response, request->source, ap_address, request->algorithm,
      request->transaction + 1, status);
  if (status == SV6621_AP_STATUS_SUCCESS)
    {
      ret = sv6621_ap_peer_begin_tx(peers, request->source, cookie);
      if (ret < 0)
        {
          sv6621_ap_remove_peer(command, instance, request->source,
                                SV6621_AP_REASON_LEAVING, false);
          sv6621_ap_peer_forget(peers, request->source, NULL);
          return ret;
        }
    }

  ret = sv6621_management_tx(command, instance, 0, cookie, channel, band,
                             false, response, sizeof(response),
                             sizeof(response));
  if (ret < 0)
    {
      if (status == SV6621_AP_STATUS_SUCCESS)
        {
          sv6621_ap_peer_cancel_tx(peers, request->source, cookie);
          sv6621_ap_remove_peer(command, instance, request->source,
                                SV6621_AP_REASON_LEAVING, false);
          sv6621_ap_peer_forget(peers, request->source, NULL);
        }

      return ret;
    }

  *accepted = status == SV6621_AP_STATUS_SUCCESS;
  return 0;
}

int sv6621_ap_validate_association(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH],
    FAR const uint8_t *ssid, size_t ssid_length,
    FAR const struct sv6621_ap_mgmt_s *request, FAR uint16_t *status)
{
  struct sv6621_ap_peer_s peer;
  FAR const uint8_t *position;
  size_t remaining;
  bool rates_present = false;
  bool rates_compatible = false;
  bool ssid_present = false;
  int ret;

  if (peers == NULL || ap_address == NULL || ssid == NULL ||
      ssid_length == 0 || ssid_length > SV6621_SSID_MAX_LENGTH ||
      request == NULL || status == NULL ||
      (request->type != SV6621_AP_MGMT_ASSOC_REQUEST &&
       request->type != SV6621_AP_MGMT_REASSOC_REQUEST))
    {
      return -EINVAL;
    }

  *status = SV6621_AP_STATUS_UNSPECIFIED;
  if (memcmp(request->bssid, ap_address, SV6621_MAC_LENGTH) != 0 ||
      memcmp(request->destination, ap_address, SV6621_MAC_LENGTH) != 0)
    {
      return 0;
    }

  ret = sv6621_ap_peer_lookup(peers, request->source, &peer);
  if (ret < 0)
    {
      return ret;
    }

  if (peer.state < SV6621_AP_PEER_AUTHENTICATED || !peer.bound)
    {
      return -EACCES;
    }

  position = request->information_elements;
  remaining = request->information_element_length;
  while (remaining != 0)
    {
      uint8_t identifier;
      uint8_t length;
      size_t index;

      if (remaining < 2)
        {
          return -EPROTO;
        }

      identifier = position[0];
      length = position[1];
      if ((size_t)length > remaining - 2)
        {
          return -EPROTO;
        }

      if (identifier == SV6621_AP_IE_SSID)
        {
          if (ssid_present || length != ssid_length ||
              memcmp(position + 2, ssid, ssid_length) != 0)
            {
              return 0;
            }

          ssid_present = true;
        }
      else if (identifier == SV6621_AP_IE_SUPPORTED_RATES ||
               identifier == SV6621_AP_IE_EXTENDED_RATES)
        {
          rates_present = true;
          for (index = 0; index < length; index++)
            {
              if (sv6621_ap_mlme_rate_supported(request->band,
                                                position[2 + index]))
                {
                  rates_compatible = true;
                }
            }
        }

      position += (size_t)length + 2;
      remaining -= (size_t)length + 2;
    }

  if (!ssid_present)
    {
      return 0;
    }

  if (!rates_present || !rates_compatible)
    {
      *status = SV6621_AP_STATUS_UNSUPPORTED_RATES;
      return 0;
    }

  *status = SV6621_AP_STATUS_SUCCESS;
  return 0;
}

int sv6621_ap_respond_association(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    uint8_t channel, enum sv6621_band_e band,
    FAR const uint8_t ap_address[SV6621_MAC_LENGTH],
    FAR const uint8_t *ssid, size_t ssid_length,
    FAR const uint8_t *response_ies, size_t response_ies_length,
    FAR const struct sv6621_ap_mgmt_s *request, uint64_t cookie,
    FAR bool *accepted)
{
  uint8_t response[SV6621_AP_MGMT_RESPONSE_MAX_SIZE];
  size_t response_length = SV6621_AP_MGMT_ASSOC_RESPONSE_SIZE;
  uint16_t status;
  uint16_t aid = 0;
  int ret;

  if (peers == NULL || command == NULL || ap_address == NULL ||
      ssid == NULL || request == NULL || accepted == NULL || channel == 0 ||
      band > SV6621_BAND_5GHZ ||
      (response_ies_length != 0 && response_ies == NULL))
    {
      return -EINVAL;
    }

  *accepted = false;
  ret = sv6621_ap_validate_association(peers, ap_address, ssid, ssid_length,
                                       request, &status);
  if (ret < 0)
    {
      return ret;
    }

  if (status == SV6621_AP_STATUS_SUCCESS)
    {
      ret = sv6621_ap_peer_prepare_association(
          peers, request->source, request->capability, &aid);
      if (ret < 0)
        {
          status = SV6621_AP_STATUS_UNSPECIFIED;
        }
    }

  memset(response, 0, sizeof(response));
  response[0] = request->type == SV6621_AP_MGMT_REASSOC_REQUEST ?
                0x30 : 0x10;
  memcpy(response + SV6621_AP_MGMT_DESTINATION_OFFSET, request->source,
         SV6621_MAC_LENGTH);
  memcpy(response + SV6621_AP_MGMT_SOURCE_OFFSET, ap_address,
         SV6621_MAC_LENGTH);
  memcpy(response + SV6621_AP_MGMT_BSSID_OFFSET, ap_address,
         SV6621_MAC_LENGTH);
  response[24] = request->capability;
  response[25] = request->capability >> 8;
  response[26] = status;
  response[27] = status >> 8;
  response[28] = aid;
  response[29] = aid >> 8;

  if (status == SV6621_AP_STATUS_SUCCESS)
    {
      ret = sv6621_ap_mlme_copy_response_ies(
          response, sizeof(response), &response_length, response_ies,
          response_ies_length);
      if (ret < 0)
        {
          sv6621_ap_peer_cancel_association(peers, request->source);
          return ret;
        }
    }

  ret = sv6621_ap_peer_begin_tx(peers, request->source, cookie);
  if (ret < 0)
    {
      if (status == SV6621_AP_STATUS_SUCCESS)
        {
          sv6621_ap_peer_cancel_association(peers, request->source);
        }

      return ret;
    }

  ret = sv6621_management_tx(command, instance, 0, cookie, channel, band,
                             false, response, response_length,
                             response_length);
  if (ret < 0)
    {
      sv6621_ap_peer_cancel_tx(peers, request->source, cookie);
      if (status == SV6621_AP_STATUS_SUCCESS)
        {
          sv6621_ap_peer_cancel_association(peers, request->source);
        }

      return ret;
    }

  *accepted = status == SV6621_AP_STATUS_SUCCESS;
  return 0;
}

int sv6621_ap_handle_tx_status(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const uint8_t *payload, size_t payload_length)
{
  struct sv6621_management_tx_status_s status;
  FAR const uint8_t *address;
  uint16_t frame_control;
  uint16_t response_status;
  bool association;
  bool remove;
  int ret;

  if (peers == NULL || command == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_management_parse_tx_status(payload, payload_length, &status);
  if (ret < 0)
    {
      return ret;
    }

  if (status.frame_length < SV6621_AP_MGMT_AUTH_RESPONSE_SIZE)
    {
      return -EPROTO;
    }

  frame_control = sv6621_ap_mlme_get_le16(status.frame) &
                  SV6621_AP_MGMT_SUBTYPE_MASK;
  if (frame_control == SV6621_AP_FRAME_AUTH)
    {
      association = false;
      response_status = sv6621_ap_mlme_get_le16(status.frame + 28);
    }
  else if (frame_control == 0x10 || frame_control == 0x30)
    {
      association = true;
      response_status = sv6621_ap_mlme_get_le16(status.frame + 26);
    }
  else
    {
      return -ENOMSG;
    }

  address = status.frame + SV6621_AP_MGMT_DESTINATION_OFFSET;
  ret = sv6621_ap_peer_complete_tx(
      peers, address, status.cookie, association,
      status.acknowledged && response_status == SV6621_AP_STATUS_SUCCESS,
      &remove);
  if (ret < 0 || !remove)
    {
      return ret;
    }

  ret = sv6621_ap_remove_peer(command, instance, address,
                              SV6621_AP_REASON_LEAVING, false);
  return ret;
}

int sv6621_ap_handle_departure(
    FAR struct sv6621_ap_peer_table_s *peers,
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const struct sv6621_ap_mgmt_s *event)
{
  if (peers == NULL || command == NULL || event == NULL ||
      (event->type != SV6621_AP_MGMT_DEAUTH &&
       event->type != SV6621_AP_MGMT_DISASSOC))
    {
      return -EINVAL;
    }

  return sv6621_ap_peer_departed(peers, command, instance, event->source,
                                 event->reason, false);
}
