/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_station.c
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
#include <nuttx/signal.h>

#include <errno.h>
#include <string.h>

#include "sv6621_sae_crypto.h"
#include "sv6621_station.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_STATION_MGMT_HEADER_SIZE       8
#define SV6621_STATION_FRAME_HEADER_SIZE      24
#define SV6621_STATION_FRAME_BSSID_OFFSET     16
#define SV6621_STATION_FRAME_SUBTYPE_MASK     0x00fc
#define SV6621_STATION_FRAME_AUTH             0x00b0
#define SV6621_STATION_FRAME_ASSOC_RESPONSE   0x0010
#define SV6621_STATION_FRAME_REASSOC_RESPONSE 0x0030
#define SV6621_STATION_FRAME_DEAUTH           0x00c0
#define SV6621_STATION_FRAME_DISASSOC         0x00a0
#define SV6621_STATION_AUTH_FRAME_SIZE        30
#define SV6621_STATION_ASSOC_FRAME_SIZE       30
#define SV6621_STATION_REASON_FRAME_SIZE      26
#define SV6621_STATION_DISCONNECT_EVENT_SIZE  8
#define SV6621_STATION_EVENT_DISCONNECT        2
#define SV6621_STATION_EVENT_RX_MGMT           4
#define SV6621_STATION_AUTH_SAE                 3
#define SV6621_STATION_AUTH_SUCCESS_TRANSACTION 2
#define SV6621_STATION_REASON_UNSPECIFIED       1
#define SV6621_STATION_HT_CAPABILITY_OFFSET     0
#define SV6621_STATION_HT_AMPDU_OFFSET          2
#define SV6621_STATION_HT_RX_MCS_OFFSET         3
#define SV6621_STATION_HT_TX_PARAMETERS_OFFSET 15
#define SV6621_STATION_HT_EXT_CAPABILITY_OFFSET 19
#define SV6621_STATION_HT_TX_DEFINED            (1 << 0)
#define SV6621_STATION_HT_TX_RX_DIFFERENT        (1 << 1)
#define SV6621_STATION_HT_TX_STREAMS_SHIFT       2
#define SV6621_STATION_VHT_CAPABILITY_OFFSET      0
#define SV6621_STATION_VHT_RX_MCS_OFFSET          4
#define SV6621_STATION_VHT_TX_MCS_OFFSET          8

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_station_ht_capability
    [SV6621_CONNECTION_HT_CAPABILITY_SIZE] = {
      0x6e, 0x00, 0x17, 0xff
    };
/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_station_get_le16(FAR const uint8_t *value);
static bool sv6621_station_security_matches(enum sv6621_security_e requested,
                                            enum sv6621_security_e advertised);
static void sv6621_station_association_worker(FAR void *arg);
static void sv6621_station_sae_complete(int result, FAR const uint8_t *pmk,
                                        FAR const uint8_t *pmkid,
                                        FAR void *arg);
static void sv6621_station_finish(FAR struct sv6621_station_s *station,
                                  enum sv6621_station_state_e state,
                                  int result);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_station_get_le16
 ****************************************************************************/

static uint16_t sv6621_station_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

/****************************************************************************
 * Name: sv6621_station_security_matches
 ****************************************************************************/

static bool sv6621_station_security_matches(enum sv6621_security_e requested,
                                            enum sv6621_security_e advertised)
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

/****************************************************************************
 * Name: sv6621_station_finish
 ****************************************************************************/

static void sv6621_station_finish(FAR struct sv6621_station_s *station,
                                  enum sv6621_station_state_e state,
                                  int result)
{
  if (nxmutex_lock(&station->lock) < 0)
    {
      return;
    }

  station->state = state;
  station->result = result;
  nxmutex_unlock(&station->lock);
  nxsem_post(&station->completion);
}

/****************************************************************************
 * Name: sv6621_station_association_worker
 ****************************************************************************/

static void sv6621_station_association_worker(FAR void *arg)
{
  FAR struct sv6621_station_s *station = arg;
  int ret;

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return;
    }

  if (station->state != SV6621_STATION_ASSOCIATING)
    {
      nxmutex_unlock(&station->lock);
      return;
    }

  nxmutex_unlock(&station->lock);
  ret = sv6621_connection_associate(
      station->command, station->target.bss.bssid,
      station->ht_capability, station->vht_capability,
      station->association_ies, station->association_ie_length);
  if (ret < 0)
    {
      sv6621_station_finish(station, SV6621_STATION_IDLE, ret);
    }
}

static void sv6621_station_sae_complete(int result, FAR const uint8_t *pmk,
                                        FAR const uint8_t *pmkid,
                                        FAR void *arg)
{
  FAR struct sv6621_station_s *station = arg;
  int ret;

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return;
    }

  if (station->state != SV6621_STATION_AUTHENTICATING ||
      station->request.security != SV6621_SECURITY_WPA3_SAE)
    {
      nxmutex_unlock(&station->lock);
      return;
    }

  if (result < 0 || pmk == NULL || pmkid == NULL)
    {
      station->state = SV6621_STATION_IDLE;
      station->result = result < 0 ? result : -EPROTO;
      nxmutex_unlock(&station->lock);
      nxsem_post(&station->completion);
      return;
    }

  memcpy(station->sae_pmk, pmk, sizeof(station->sae_pmk));
  memcpy(station->sae_pmkid, pmkid, sizeof(station->sae_pmkid));
  station->sae_pmk_valid = true;
  station->state = SV6621_STATION_ASSOCIATING;
  nxmutex_unlock(&station->lock);
  ret = work_queue(LPWORK, &station->association_work,
                   sv6621_station_association_worker, station, 0);
  if (ret < 0)
    {
      sv6621_station_finish(station, SV6621_STATION_IDLE, ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_station_parse_mgmt
 ****************************************************************************/

int sv6621_station_parse_mgmt(FAR const uint8_t *payload, size_t length,
                              FAR struct sv6621_station_mgmt_s *event)
{
  FAR const uint8_t *frame;
  uint16_t frame_control;
  uint16_t frame_length;

  if (payload == NULL || event == NULL ||
      length < SV6621_STATION_MGMT_HEADER_SIZE +
                   SV6621_STATION_FRAME_HEADER_SIZE ||
      payload[0] == 0 || payload[1] > SV6621_BAND_5GHZ)
    {
      return -EINVAL;
    }

  frame_length = sv6621_station_get_le16(payload + 4);
  if (frame_length < SV6621_STATION_FRAME_HEADER_SIZE ||
      frame_length > length - SV6621_STATION_MGMT_HEADER_SIZE)
    {
      return -EPROTO;
    }

  frame = payload + SV6621_STATION_MGMT_HEADER_SIZE;
  frame_control = sv6621_station_get_le16(frame) &
                  SV6621_STATION_FRAME_SUBTYPE_MASK;
  memset(event, 0, sizeof(*event));
  event->channel = payload[0];
  event->band = payload[1] == 0 ? SV6621_BAND_2GHZ : SV6621_BAND_5GHZ;
  event->signal_dbm = (int16_t)sv6621_station_get_le16(payload + 2);
  memcpy(event->bssid, frame + SV6621_STATION_FRAME_BSSID_OFFSET,
         SV6621_MAC_LENGTH);
  event->frame = frame;
  event->frame_length = frame_length;

  switch (frame_control)
    {
      case SV6621_STATION_FRAME_AUTH:
        if (frame_length < SV6621_STATION_AUTH_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_STATION_MGMT_AUTH;
        event->algorithm = sv6621_station_get_le16(frame + 24);
        event->transaction = sv6621_station_get_le16(frame + 26);
        event->status = sv6621_station_get_le16(frame + 28);
        break;

      case SV6621_STATION_FRAME_ASSOC_RESPONSE:
      case SV6621_STATION_FRAME_REASSOC_RESPONSE:
        if (frame_length < SV6621_STATION_ASSOC_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_STATION_MGMT_ASSOC;
        event->status = sv6621_station_get_le16(frame + 26);
        break;

      case SV6621_STATION_FRAME_DEAUTH:
      case SV6621_STATION_FRAME_DISASSOC:
        if (frame_length < SV6621_STATION_REASON_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_STATION_FRAME_DEAUTH
                          ? SV6621_STATION_MGMT_DEAUTH
                          : SV6621_STATION_MGMT_DISASSOC;
        event->reason = sv6621_station_get_le16(frame + 24);
        break;

      default:
        return -ENOMSG;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_station_parse_disconnect
 ****************************************************************************/

int sv6621_station_parse_disconnect(
    FAR const uint8_t *payload, size_t length,
    uint8_t bssid[SV6621_MAC_LENGTH], FAR uint16_t *reason)
{
  if (payload == NULL || bssid == NULL || reason == NULL ||
      length != SV6621_STATION_DISCONNECT_EVENT_SIZE)
    {
      return -EINVAL;
    }

  *reason = sv6621_station_get_le16(payload);
  memcpy(bssid, payload + 2, SV6621_MAC_LENGTH);
  return 0;
}

/****************************************************************************
 * Name: sv6621_station_init
 ****************************************************************************/

int sv6621_station_init(FAR struct sv6621_station_s *station,
                        FAR struct sv6621_command_engine_s *command,
                        FAR struct sv6621_scan_s *scan,
                        sv6621_station_event_t event, FAR void *event_arg)
{
  int ret;

  if (station == NULL || command == NULL || scan == NULL)
    {
      return -EINVAL;
    }

  memset(station, 0, sizeof(*station));
  ret = nxmutex_init(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_init(&station->connect_lock);
  if (ret < 0)
    {
      nxmutex_destroy(&station->lock);
      return ret;
    }

  ret = nxsem_init(&station->completion, 0, 0);
  if (ret < 0)
    {
      nxmutex_destroy(&station->connect_lock);
      nxmutex_destroy(&station->lock);
      return ret;
    }

  ret = sv6621_sae_init(&station->sae, command,
                        sv6621_station_sae_complete, station);
  if (ret < 0)
    {
      nxsem_destroy(&station->completion);
      nxmutex_destroy(&station->connect_lock);
      nxmutex_destroy(&station->lock);
      return ret;
    }

  station->command = command;
  station->scan = scan;
  station->event = event;
  station->event_arg = event_arg;
  memcpy(station->ht_capability, g_sv6621_station_ht_capability,
         sizeof(station->ht_capability));
  return 0;
}

int sv6621_station_set_local_address(
    FAR struct sv6621_station_s *station,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  int ret;

  if (station == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (station->state != SV6621_STATION_IDLE)
    {
      nxmutex_unlock(&station->lock);
      return -EBUSY;
    }

  memcpy(station->local_address, address, SV6621_MAC_LENGTH);
  station->local_address_valid = true;
  nxmutex_unlock(&station->lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_station_configure_ht
 ****************************************************************************/

int sv6621_station_configure_ht(FAR struct sv6621_station_s *station,
                                uint16_t capabilities,
                                uint16_t extended_capabilities,
                                uint16_t ampdu_parameters,
                                uint32_t tx_mcs, uint32_t rx_mcs)
{
  uint8_t tx_streams = 0;
  uint8_t rx_streams = 0;
  uint8_t tx_parameters = 0;
  unsigned int index;
  int ret;

  if (station == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (station->state != SV6621_STATION_IDLE)
    {
      nxmutex_unlock(&station->lock);
      return -EBUSY;
    }

  memset(station->ht_capability, 0, sizeof(station->ht_capability));
  station->ht_capability[SV6621_STATION_HT_CAPABILITY_OFFSET] =
      capabilities & 0xff;
  station->ht_capability[SV6621_STATION_HT_CAPABILITY_OFFSET + 1] =
      capabilities >> 8;
  station->ht_capability[SV6621_STATION_HT_AMPDU_OFFSET] =
      ampdu_parameters & 0xff;
  station->ht_capability[SV6621_STATION_HT_EXT_CAPABILITY_OFFSET] =
      extended_capabilities & 0xff;
  station->ht_capability[SV6621_STATION_HT_EXT_CAPABILITY_OFFSET + 1] =
      extended_capabilities >> 8;

  for (index = 0; index < sizeof(rx_mcs); index++)
    {
      uint8_t rx_map = rx_mcs >> (index * 8);
      uint8_t tx_map = tx_mcs >> (index * 8);

      station->ht_capability[SV6621_STATION_HT_RX_MCS_OFFSET + index] =
          rx_map;
      rx_streams += rx_map != 0;
      tx_streams += tx_map != 0;
    }

  if (tx_streams != 0)
    {
      tx_parameters = SV6621_STATION_HT_TX_DEFINED;
      if (tx_mcs != rx_mcs)
        {
          tx_parameters |= SV6621_STATION_HT_TX_RX_DIFFERENT;
          tx_parameters |= (tx_streams - 1) <<
                           SV6621_STATION_HT_TX_STREAMS_SHIFT;
        }
    }

  station->ht_capability[SV6621_STATION_HT_TX_PARAMETERS_OFFSET] =
      tx_parameters;
  nxmutex_unlock(&station->lock);
  return rx_streams == 0 ? -EPROTO : 0;
}

/****************************************************************************
 * Name: sv6621_station_configure_vht
 ****************************************************************************/

int sv6621_station_configure_vht(FAR struct sv6621_station_s *station,
                                 uint32_t capabilities,
                                 uint16_t tx_mcs, uint16_t rx_mcs)
{
  int ret;

  if (station == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (station->state != SV6621_STATION_IDLE)
    {
      nxmutex_unlock(&station->lock);
      return -EBUSY;
    }

  memset(station->vht_capability, 0, sizeof(station->vht_capability));
  station->vht_capability[SV6621_STATION_VHT_CAPABILITY_OFFSET] =
      capabilities & 0xff;
  station->vht_capability[SV6621_STATION_VHT_CAPABILITY_OFFSET + 1] =
      (capabilities >> 8) & 0xff;
  station->vht_capability[SV6621_STATION_VHT_CAPABILITY_OFFSET + 2] =
      (capabilities >> 16) & 0xff;
  station->vht_capability[SV6621_STATION_VHT_CAPABILITY_OFFSET + 3] =
      capabilities >> 24;
  station->vht_capability[SV6621_STATION_VHT_RX_MCS_OFFSET] = rx_mcs & 0xff;
  station->vht_capability[SV6621_STATION_VHT_RX_MCS_OFFSET + 1] = rx_mcs >> 8;
  station->vht_capability[SV6621_STATION_VHT_TX_MCS_OFFSET] = tx_mcs & 0xff;
  station->vht_capability[SV6621_STATION_VHT_TX_MCS_OFFSET + 1] = tx_mcs >> 8;
  nxmutex_unlock(&station->lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_station_configure_bandwidth
 ****************************************************************************/

int sv6621_station_configure_bandwidth(FAR struct sv6621_station_s *station,
                                       uint32_t capabilities)
{
  int ret;

  if (station == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (station->state != SV6621_STATION_IDLE)
    {
      nxmutex_unlock(&station->lock);
      return -EBUSY;
    }

  station->bandwidth_capabilities = capabilities;
  nxmutex_unlock(&station->lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_station_deinit
 ****************************************************************************/

void sv6621_station_deinit(FAR struct sv6621_station_s *station)
{
  if (station == NULL)
    {
      return;
    }

  if (nxmutex_lock(&station->lock) >= 0)
    {
      station->shutting_down = true;
      nxmutex_unlock(&station->lock);
    }

  sv6621_station_reset(station, -ESHUTDOWN);
  if (nxmutex_lock(&station->connect_lock) >= 0)
    {
      nxmutex_unlock(&station->connect_lock);
    }

  sv6621_sae_deinit(&station->sae);
  nxsem_destroy(&station->completion);
  nxmutex_destroy(&station->connect_lock);
  nxmutex_destroy(&station->lock);
}

/****************************************************************************
 * Name: sv6621_station_connect
 ****************************************************************************/

int sv6621_station_connect(FAR struct sv6621_station_s *station,
                           FAR const struct sv6621_connect_s *request,
                           uint32_t timeout_ms)
{
  FAR struct sv6621_scan_entry_s *target;
  bool direct_association = false;
  int ret;

  if (station == NULL || request == NULL || timeout_ms == 0 ||
      request->ssid_length == 0 ||
      request->ssid_length > SV6621_SSID_MAX_LENGTH ||
      request->credential_length > SV6621_KEY_MAX_LENGTH)
    {
      return -EINVAL;
    }

  if ((request->security == SV6621_SECURITY_OPEN &&
       request->credential_length != 0) ||
      ((request->security == SV6621_SECURITY_WPA2_PSK ||
        request->security == SV6621_SECURITY_WPA2_WPA3_PSK) &&
       (request->credential_length < 8 || request->credential_length > 64)))
    {
      return -EINVAL;
    }

  if (request->security == SV6621_SECURITY_WPA3_SAE &&
      (request->credential_length == 0 || request->credential_length > 63))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->connect_lock);
  if (ret < 0)
    {
      return ret;
    }

  target = kmm_malloc(sizeof(*target));
  if (target == NULL)
    {
      ret = -ENOMEM;
      goto unlock_connect;
    }

  ret = sv6621_scan_cache_find(&station->scan->cache, request, target);
  if (ret < 0)
    {
      goto free_target;
    }

  if (!sv6621_station_security_matches(request->security,
                                       target->bss.security))
    {
      ret = -EACCES;
      goto free_target;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      goto free_target;
    }

  if (station->state != SV6621_STATION_IDLE)
    {
      nxmutex_unlock(&station->lock);
      ret = -EBUSY;
      goto free_target;
    }

  if (station->shutting_down)
    {
      nxmutex_unlock(&station->lock);
      ret = -ESHUTDOWN;
      goto free_target;
    }

  ret = sv6621_connection_build_association_ies(
      target, request->security, station->association_ies,
      sizeof(station->association_ies), &station->association_ie_length);
  if (ret < 0)
    {
      nxmutex_unlock(&station->lock);
      goto free_target;
    }

  nxsem_reset(&station->completion, 0);
  station->target = *target;
  station->request = *request;
  sv6621_sae_zeroize(station->sae_pmk, sizeof(station->sae_pmk));
  sv6621_sae_zeroize(station->sae_pmkid, sizeof(station->sae_pmkid));
  station->sae_pmk_valid = false;
  station->result = -EINPROGRESS;
  station->state = SV6621_STATION_JOINING;
  nxmutex_unlock(&station->lock);
  kmm_free(target);
  target = NULL;

  ret = sv6621_connection_join(
      station->command, &station->target, station->bandwidth_capabilities,
      &station->peer);
  if (ret < 0)
    {
      sv6621_station_finish(station, SV6621_STATION_IDLE, ret);
      nxsem_trywait(&station->completion);
      goto unlock_connect;
    }

  nxsig_usleep(50 * 1000);

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      goto unlock_connect;
    }

  if (station->state != SV6621_STATION_JOINING)
    {
      ret = station->result;
      nxmutex_unlock(&station->lock);
      nxsem_trywait(&station->completion);
      goto unlock_connect;
    }

  station->state = SV6621_STATION_AUTHENTICATING;
  nxmutex_unlock(&station->lock);
  if (request->security == SV6621_SECURITY_WPA3_SAE)
    {
      if (!station->local_address_valid)
        {
          ret = -EADDRNOTAVAIL;
        }
      else
        {
          ret = sv6621_sae_start(
              &station->sae, station->local_address,
              station->target.bss.bssid, station->peer.instance,
              station->target.bss.channel, station->target.bss.band,
              request->credential, request->credential_length);
        }
    }
  else
    {
      ret = sv6621_connection_authenticate(
          station->command, SV6621_CONNECTION_AUTH_OPEN, NULL, 0, NULL, 0);
    }
  if (ret < 0)
    {
      sv6621_station_finish(station, SV6621_STATION_IDLE, ret);
      nxsem_trywait(&station->completion);
      goto unlock_connect;
    }

  if (request->security != SV6621_SECURITY_WPA3_SAE)
    {
      ret = nxmutex_lock(&station->lock);
      if (ret < 0)
        {
          goto unlock_connect;
        }

      if (station->state == SV6621_STATION_AUTHENTICATING)
        {
          station->state = SV6621_STATION_ASSOCIATING;
          direct_association = true;
          ret = 0;
        }
      else if (station->state == SV6621_STATION_ASSOCIATING ||
               station->state == SV6621_STATION_ASSOCIATED)
        {
          direct_association =
              station->state == SV6621_STATION_ASSOCIATING;
          ret = 0;
        }
      else
        {
          ret = station->result;
        }

      nxmutex_unlock(&station->lock);
      if (ret < 0)
        {
          sv6621_station_finish(station, SV6621_STATION_IDLE, ret);
          nxsem_trywait(&station->completion);
          goto unlock_connect;
        }

      if (direct_association)
        {
          nxsig_usleep(100 * 1000);
          sv6621_station_association_worker(station);
        }
    }

  ret = nxsem_tickwait(&station->completion, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      int wait_result = ret;

      work_cancel_sync(LPWORK, &station->association_work);
      ret = nxmutex_lock(&station->lock);
      if (ret < 0)
        {
          goto unlock_connect;
        }

      if (station->result != -EINPROGRESS)
        {
          ret = station->result;
          nxmutex_unlock(&station->lock);
          nxsem_trywait(&station->completion);
          goto unlock_connect;
        }

      station->state = SV6621_STATION_IDLE;
      station->result = wait_result;
      nxmutex_unlock(&station->lock);
      sv6621_sae_cancel(&station->sae, wait_result);
      sv6621_connection_disconnect(
          station->command, SV6621_CONNECTION_DISCONNECT_ONLY, true,
          SV6621_STATION_REASON_UNSPECIFIED, NULL, 0);
      ret = wait_result;
      goto unlock_connect;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      goto unlock_connect;
    }

  ret = station->result;
  nxmutex_unlock(&station->lock);
  goto unlock_connect;

free_target:
  kmm_free(target);

unlock_connect:
  if (ret < 0)
    {
      sv6621_sae_cancel(&station->sae, ret);
    }

  nxmutex_unlock(&station->connect_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_station_disconnect
 ****************************************************************************/

int sv6621_station_disconnect(FAR struct sv6621_station_s *station,
                              uint16_t reason)
{
  enum sv6621_station_state_e previous_state;
  int ret;

  if (station == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  previous_state = station->state;
  if (previous_state != SV6621_STATION_ASSOCIATED &&
      previous_state != SV6621_STATION_CONNECTED)
    {
      nxmutex_unlock(&station->lock);
      return -ENOTCONN;
    }

  station->state = SV6621_STATION_DISCONNECTING;
  nxmutex_unlock(&station->lock);
  ret = sv6621_connection_disconnect(
      station->command, SV6621_CONNECTION_DISCONNECT_DEAUTH, false, reason,
      NULL, 0);
  if (ret < 0)
    {
      if (nxmutex_lock(&station->lock) >= 0)
        {
          if (station->state == SV6621_STATION_DISCONNECTING)
            {
              station->state = previous_state;
            }
          else if (station->state == SV6621_STATION_IDLE)
            {
              ret = 0;
            }

          nxmutex_unlock(&station->lock);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  if (nxmutex_lock(&station->lock) >= 0)
    {
      station->state = SV6621_STATION_IDLE;
      nxmutex_unlock(&station->lock);
    }

  if (previous_state == SV6621_STATION_CONNECTED && station->event != NULL)
    {
      station->event(false, false, reason, station->event_arg);
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_station_mark_connected
 ****************************************************************************/

int sv6621_station_mark_connected(FAR struct sv6621_station_s *station)
{
  int ret;

  if (station == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (station->state != SV6621_STATION_ASSOCIATED)
    {
      nxmutex_unlock(&station->lock);
      return station->state == SV6621_STATION_CONNECTED ? 0 : -ENOTCONN;
    }

  station->state = SV6621_STATION_CONNECTED;
  nxmutex_unlock(&station->lock);
  if (station->event != NULL)
    {
      station->event(true, false, 0, station->event_arg);
    }

  return 0;
}

int sv6621_station_get_sae_pmk(
    FAR struct sv6621_station_s *station,
    uint8_t pmk[SV6621_SAE_PMK_SIZE],
    uint8_t pmkid[SV6621_SAE_PMKID_SIZE])
{
  int ret;

  if (station == NULL || pmk == NULL || pmkid == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&station->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!station->sae_pmk_valid)
    {
      nxmutex_unlock(&station->lock);
      return -ENOENT;
    }

  memcpy(pmk, station->sae_pmk, SV6621_SAE_PMK_SIZE);
  memcpy(pmkid, station->sae_pmkid, SV6621_SAE_PMKID_SIZE);
  nxmutex_unlock(&station->lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_station_reset
 ****************************************************************************/

void sv6621_station_reset(FAR struct sv6621_station_s *station, int result)
{
  bool pending = false;

  if (station == NULL)
    {
      return;
    }

  work_cancel_sync(LPWORK, &station->association_work);
  if (nxmutex_lock(&station->lock) >= 0)
    {
      pending = station->state == SV6621_STATION_JOINING ||
                station->state == SV6621_STATION_AUTHENTICATING ||
                station->state == SV6621_STATION_ASSOCIATING;
      station->state = SV6621_STATION_IDLE;
      station->result = result;
      sv6621_sae_zeroize(station->sae_pmk, sizeof(station->sae_pmk));
      sv6621_sae_zeroize(station->sae_pmkid, sizeof(station->sae_pmkid));
      station->sae_pmk_valid = false;
      nxmutex_unlock(&station->lock);
    }

  sv6621_sae_cancel(&station->sae, result);

  if (pending)
    {
      nxsem_post(&station->completion);
    }
}

/****************************************************************************
 * Name: sv6621_station_command_event
 ****************************************************************************/

void sv6621_station_command_event(uint8_t instance, uint8_t id,
                                  FAR const uint8_t *payload, size_t length,
                                  FAR void *arg)
{
  FAR struct sv6621_station_s *station = arg;
  struct sv6621_station_mgmt_s mgmt;
  uint8_t bssid[SV6621_MAC_LENGTH];
  enum sv6621_station_state_e state;
  uint16_t reason;
  bool notify = false;
  bool notify_connected = false;
  bool complete = false;
  int result = 0;
  int ret;

  if (station == NULL || instance != 0)
    {
      return;
    }

  if (id == SV6621_STATION_EVENT_DISCONNECT)
    {
      ret = sv6621_station_parse_disconnect(payload, length, bssid, &reason);
      if (ret < 0)
        {
          return;
        }

      ret = nxmutex_lock(&station->lock);
      if (ret < 0)
        {
          return;
        }

      state = station->state;
      if (state != SV6621_STATION_IDLE &&
          memcmp(bssid, station->target.bss.bssid, SV6621_MAC_LENGTH) == 0)
        {
          complete = state == SV6621_STATION_JOINING ||
                     state == SV6621_STATION_AUTHENTICATING ||
                     state == SV6621_STATION_ASSOCIATING;
          notify = state == SV6621_STATION_ASSOCIATED ||
                   state == SV6621_STATION_CONNECTED;
          notify_connected = false;
          station->state = SV6621_STATION_IDLE;
          station->result = -ECONNRESET;
        }

      nxmutex_unlock(&station->lock);
    }
  else if (id == SV6621_STATION_EVENT_RX_MGMT)
    {
      ret = sv6621_station_parse_mgmt(payload, length, &mgmt);
      if (ret < 0)
        {
          return;
        }

      ret = nxmutex_lock(&station->lock);
      if (ret < 0)
        {
          return;
        }

      state = station->state;
      if (memcmp(mgmt.bssid, station->target.bss.bssid,
                 SV6621_MAC_LENGTH) != 0)
        {
          nxmutex_unlock(&station->lock);
          return;
        }

      if (mgmt.type == SV6621_STATION_MGMT_AUTH &&
          state == SV6621_STATION_AUTHENTICATING)
        {
          if (station->request.security == SV6621_SECURITY_WPA3_SAE)
            {
              nxmutex_unlock(&station->lock);
              if (mgmt.algorithm == SV6621_STATION_AUTH_SAE)
                {
                  ret = sv6621_sae_input(&station->sae, mgmt.frame,
                                         mgmt.frame_length);
                  if (ret < 0 && ret != -EBUSY && ret != -ENOTCONN)
                    {
                      sv6621_station_finish(station, SV6621_STATION_IDLE,
                                            ret);
                    }
                }

              return;
            }

          if (mgmt.algorithm != SV6621_CONNECTION_AUTH_OPEN ||
              mgmt.transaction != SV6621_STATION_AUTH_SUCCESS_TRANSACTION ||
              mgmt.status != 0)
            {
              station->state = SV6621_STATION_IDLE;
              station->result = -EACCES;
              complete = true;
            }
          else
            {
              station->state = SV6621_STATION_ASSOCIATING;
            }
        }
      else if (mgmt.type == SV6621_STATION_MGMT_ASSOC &&
               state == SV6621_STATION_ASSOCIATING)
        {
          result = mgmt.status == 0 ? 0 : -ECONNREFUSED;
          station->state = result == 0 ? SV6621_STATION_ASSOCIATED
                                       : SV6621_STATION_IDLE;
          station->result = result;
          complete = true;
          if (result == 0 &&
              station->request.security == SV6621_SECURITY_OPEN)
            {
              station->state = SV6621_STATION_CONNECTED;
              notify = true;
              notify_connected = true;
            }

          reason = mgmt.status;
        }
      else if ((mgmt.type == SV6621_STATION_MGMT_DEAUTH ||
                mgmt.type == SV6621_STATION_MGMT_DISASSOC) &&
               state != SV6621_STATION_IDLE)
        {
          complete = state == SV6621_STATION_JOINING ||
                     state == SV6621_STATION_AUTHENTICATING ||
                     state == SV6621_STATION_ASSOCIATING;
          notify = state == SV6621_STATION_ASSOCIATED ||
                   state == SV6621_STATION_CONNECTED;
          notify_connected = false;
          station->state = SV6621_STATION_IDLE;
          station->result = -ECONNRESET;
          reason = mgmt.reason;
        }

      nxmutex_unlock(&station->lock);
    }
  else
    {
      return;
    }

  if (complete)
    {
      nxsem_post(&station->completion);
    }

  if (notify && station->event != NULL)
    {
      station->event(notify_connected, !notify_connected, reason,
                     station->event_arg);
    }
}
