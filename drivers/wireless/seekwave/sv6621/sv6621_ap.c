/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap.c
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
#include <limits.h>
#include <string.h>

#include "sv6621_ap.h"
#include "sv6621_ap_mlme.h"
#include "sv6621_management.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_COMMAND_START              19
#define SV6621_AP_COMMAND_STOP               20
#define SV6621_AP_COMMAND_TIMEOUT_MS        5000
#define SV6621_AP_FIXED_SIZE                  64
#define SV6621_AP_RESPONSE_SIZE                3
#define SV6621_AP_MAX_PAYLOAD \
  (SV6621_COMMAND_MAX_MESSAGE_SIZE - SV6621_COMMAND_HEADER_SIZE)

#define SV6621_AP_BEACON_INTERVAL_OFFSET      0
#define SV6621_AP_DTIM_OFFSET                  4
#define SV6621_AP_FLAGS_OFFSET                 5
#define SV6621_AP_CHANNEL_OFFSET               6
#define SV6621_AP_WIDTH_OFFSET                 7
#define SV6621_AP_CENTER1_OFFSET               8
#define SV6621_AP_CENTER2_OFFSET               9
#define SV6621_AP_BAND_OFFSET                 10
#define SV6621_AP_SSID_LENGTH_OFFSET          11
#define SV6621_AP_SSID_OFFSET                 12
#define SV6621_AP_BLOB_TABLE_OFFSET           44
#define SV6621_AP_BLOB_COUNT                   5
#define SV6621_AP_PROBE_RESPONSE_IE_OFFSET     36 /* Header + fixed fields */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sv6621_ap_blob_field_s
{
  FAR const struct sv6621_ap_blob_s *blob;
  size_t table_offset;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_put_le16(FAR uint8_t *value, uint16_t number);
static void sv6621_ap_put_le32(FAR uint8_t *value, uint32_t number);
static uint64_t sv6621_ap_next_cookie(FAR struct sv6621_ap_s *ap);
static bool sv6621_ap_expected_event_error(int error);
static bool sv6621_ap_tx_client_event(
    FAR struct sv6621_ap_s *ap, FAR const uint8_t *payload, size_t length,
    FAR struct sv6621_ap_client_event_s *event);
static int sv6621_ap_dispatch_event(uint8_t instance, uint8_t id,
                                    FAR const uint8_t *payload,
                                    size_t length, FAR void *arg);
static void sv6621_ap_dispatch_error(int error, FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_ap_put_le16(FAR uint8_t *value, uint16_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
}

static void sv6621_ap_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

static uint64_t sv6621_ap_next_cookie(FAR struct sv6621_ap_s *ap)
{
  uint64_t cookie = ap->next_cookie++;

  if (cookie == 0)
    {
      cookie = ap->next_cookie++;
    }

  return cookie;
}

static bool sv6621_ap_expected_event_error(int error)
{
  return error == -EACCES || error == -ECONNREFUSED || error == -ENOENT ||
         error == -ENOMSG || error == -EPROTO || error == -ESTALE;
}

static bool sv6621_ap_tx_client_event(
    FAR struct sv6621_ap_s *ap, FAR const uint8_t *payload, size_t length,
    FAR struct sv6621_ap_client_event_s *event)
{
  struct sv6621_management_tx_status_s status;
  struct sv6621_ap_peer_s peer;

  if (sv6621_management_parse_tx_status(payload, length, &status) < 0 ||
      !status.acknowledged || status.frame_length < 30 ||
      status.frame[0] != 0x10)
    {
      return false;
    }

  memcpy(event->address, status.frame + 4, SV6621_MAC_LENGTH);
  if (sv6621_ap_peer_lookup(&ap->peers, event->address, &peer) < 0 ||
      peer.state != SV6621_AP_PEER_ASSOCIATED)
    {
      return false;
    }

  event->aid = peer.aid;
  event->reason = 0;
  return true;
}

static int sv6621_ap_dispatch_event(uint8_t instance, uint8_t id,
                                    FAR const uint8_t *payload,
                                    size_t length, FAR void *arg)
{
  FAR struct sv6621_ap_s *ap = arg;
  struct sv6621_ap_mgmt_s mgmt;
  uint8_t address[SV6621_MAC_LENGTH];
  struct sv6621_ap_client_event_s client_event;
  uint16_t reason;
  bool accepted;
  bool notify_connected = false;
  bool notify_disconnected = false;
  int ret;

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active || instance != ap->instance)
    {
      nxmutex_unlock(&ap->lock);
      return 0;
    }

  if (id == SV6621_AP_EVENT_MGMT_TX_STATUS)
    {
      ret = sv6621_ap_handle_tx_status(&ap->peers, ap->command,
                                       ap->instance, payload, length);
      if (ret == 0)
        {
          notify_connected = sv6621_ap_tx_client_event(
              ap, payload, length, &client_event);
          if (notify_connected &&
              ap->config.security == SV6621_SECURITY_OPEN)
            {
              ret = sv6621_ap_peer_authorize(&ap->peers,
                                             client_event.address);
              if (ret < 0)
                {
                  notify_connected = false;
                }
            }
        }
    }
  else if (id == SV6621_AP_EVENT_DEL_STA)
    {
      ret = sv6621_ap_parse_peer_departure(payload, length, address, &reason);
      if (ret == 0)
        {
          memcpy(client_event.address, address, SV6621_MAC_LENGTH);
          client_event.aid = 0;
          client_event.reason = reason;
          ret = sv6621_ap_peer_departed(&ap->peers, ap->command,
                                        ap->instance, address, reason, true);
          notify_disconnected = ret == 0;
        }
    }
  else
    {
      ret = sv6621_ap_parse_mgmt(payload, length, &mgmt);
      if (ret == 0 && mgmt.type == SV6621_AP_MGMT_AUTH)
        {
          ret = sv6621_ap_authenticate_open(
              &ap->peers, ap->command, ap->instance, ap->config.channel,
              ap->config.band, ap->address, &mgmt,
              sv6621_ap_next_cookie(ap), &accepted);
        }
      else if (ret == 0 &&
               (mgmt.type == SV6621_AP_MGMT_ASSOC_REQUEST ||
                mgmt.type == SV6621_AP_MGMT_REASSOC_REQUEST))
        {
          if (ap->templates.probe_response_length <
              SV6621_AP_PROBE_RESPONSE_IE_OFFSET)
            {
              ret = -EPROTO;
            }
          else
            {
              ret = sv6621_ap_respond_association(
                  &ap->peers, ap->command, ap->instance,
                  ap->config.channel, ap->config.band, ap->address,
                  ap->config.ssid, ap->config.ssid_length,
                  ap->templates.probe_response +
                      SV6621_AP_PROBE_RESPONSE_IE_OFFSET,
                  ap->templates.probe_response_length -
                      SV6621_AP_PROBE_RESPONSE_IE_OFFSET,
                  &mgmt, sv6621_ap_next_cookie(ap), &accepted);
            }
        }
      else if (ret == 0 &&
               (mgmt.type == SV6621_AP_MGMT_DEAUTH ||
                mgmt.type == SV6621_AP_MGMT_DISASSOC))
        {
          memcpy(client_event.address, mgmt.source, SV6621_MAC_LENGTH);
          client_event.aid = 0;
          client_event.reason = mgmt.reason;
          ret = sv6621_ap_handle_departure(&ap->peers, ap->command,
                                           ap->instance, &mgmt);
          notify_disconnected = ret == 0;
        }
    }

  nxmutex_unlock(&ap->lock);
  if (ap->client != NULL && notify_connected)
    {
      ap->client(true, &client_event, ap->client_arg);
    }
  else if (ap->client != NULL && notify_disconnected)
    {
      ap->client(false, &client_event, ap->client_arg);
    }

  return sv6621_ap_expected_event_error(ret) ? 0 : ret;
}

static void sv6621_ap_dispatch_error(int error, FAR void *arg)
{
  FAR struct sv6621_ap_s *ap = arg;

  if (ap->error != NULL)
    {
      ap->error(error, ap->error_arg);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_encode_start(FAR const struct sv6621_ap_start_s *config,
                           FAR uint8_t *payload, size_t capacity,
                           FAR size_t *written)
{
  struct sv6621_ap_blob_field_s fields[SV6621_AP_BLOB_COUNT];
  size_t total = SV6621_AP_FIXED_SIZE;
  size_t offset;
  size_t index;

  if (config == NULL || payload == NULL || written == NULL ||
      config->beacon_interval == 0 || config->beacon_interval > INT_MAX ||
      config->dtim_period == 0 ||
      config->hidden_ssid > 2 || config->channel == 0 ||
      config->channel_width > SV6621_CHANNEL_WIDTH_160 ||
      config->band > SV6621_BAND_5GHZ ||
      config->ssid_length > SV6621_SSID_MAX_LENGTH)
    {
      return -EINVAL;
    }

  fields[0].blob = &config->beacon_head;
  fields[1].blob = &config->beacon_tail;
  fields[2].blob = &config->beacon_ies;
  fields[3].blob = &config->probe_response_ies;
  fields[4].blob = &config->association_response_ies;
  for (index = 0; index < SV6621_AP_BLOB_COUNT; index++)
    {
      fields[index].table_offset =
          SV6621_AP_BLOB_TABLE_OFFSET + index * 4;
      if ((fields[index].blob->length != 0 &&
           fields[index].blob->data == NULL) ||
          fields[index].blob->length > UINT16_MAX ||
          fields[index].blob->length > SV6621_AP_MAX_PAYLOAD - total)
        {
          return -EINVAL;
        }

      total += fields[index].blob->length;
    }

  if (capacity < total)
    {
      return -ENOSPC;
    }

  memset(payload, 0, total);
  sv6621_ap_put_le32(payload + SV6621_AP_BEACON_INTERVAL_OFFSET,
                     config->beacon_interval);
  payload[SV6621_AP_DTIM_OFFSET] = config->dtim_period;
  payload[SV6621_AP_FLAGS_OFFSET] = config->hidden_ssid;
  payload[SV6621_AP_CHANNEL_OFFSET] = config->channel;
  payload[SV6621_AP_WIDTH_OFFSET] = config->channel_width;
  payload[SV6621_AP_CENTER1_OFFSET] = config->center_channel1;
  payload[SV6621_AP_CENTER2_OFFSET] = config->center_channel2;
  payload[SV6621_AP_BAND_OFFSET] = config->band;
  payload[SV6621_AP_SSID_LENGTH_OFFSET] = config->ssid_length;
  memcpy(payload + SV6621_AP_SSID_OFFSET, config->ssid,
         config->ssid_length);

  offset = SV6621_AP_FIXED_SIZE;
  for (index = 0; index < SV6621_AP_BLOB_COUNT; index++)
    {
      if (fields[index].blob->length != 0)
        {
          sv6621_ap_put_le16(payload + fields[index].table_offset,
                             offset);
          sv6621_ap_put_le16(payload + fields[index].table_offset + 2,
                             fields[index].blob->length);
          memcpy(payload + offset, fields[index].blob->data,
                 fields[index].blob->length);
          offset += fields[index].blob->length;
        }
    }

  *written = offset;
  return 0;
}

int sv6621_ap_start(FAR struct sv6621_command_engine_s *command,
                    uint8_t instance,
                    FAR const struct sv6621_ap_start_s *config,
                    FAR struct sv6621_ap_context_s *context)
{
  FAR uint8_t *payload;
  uint8_t response[SV6621_AP_RESPONSE_SIZE];
  size_t payload_length;
  size_t response_length = sizeof(response);
  int ret;

  if (command == NULL || config == NULL || context == NULL)
    {
      return -EINVAL;
    }

  payload = kmm_malloc(SV6621_AP_MAX_PAYLOAD);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  ret = sv6621_ap_encode_start(config, payload, SV6621_AP_MAX_PAYLOAD,
                               &payload_length);
  if (ret == 0)
    {
      ret = sv6621_command_execute(command, instance,
                                   SV6621_AP_COMMAND_START, payload,
                                   payload_length, response,
                                   &response_length,
                                   SV6621_AP_COMMAND_TIMEOUT_MS);
    }

  kmm_free(payload);
  if (ret < 0)
    {
      return ret;
    }

  if (response_length != sizeof(response))
    {
      return -EPROTO;
    }

  context->lmac_id = response[0];
  context->instance = response[1];
  context->multicast_index = response[2];
  return 0;
}

int sv6621_ap_stop(FAR struct sv6621_command_engine_s *command,
                   uint8_t instance)
{
  if (command == NULL)
    {
      return -EINVAL;
    }

  return sv6621_command_execute(command, instance, SV6621_AP_COMMAND_STOP,
                                NULL, 0, NULL, NULL,
                                SV6621_AP_COMMAND_TIMEOUT_MS);
}

int sv6621_ap_init(FAR struct sv6621_ap_s *ap,
                   FAR struct sv6621_command_engine_s *command,
                   uint8_t max_stations,
                   FAR const uint8_t address[SV6621_MAC_LENGTH],
                   sv6621_ap_error_t error, FAR void *error_arg,
                   sv6621_ap_client_t client, FAR void *client_arg)
{
  int ret;

  if (ap == NULL || command == NULL || address == NULL ||
      max_stations == 0)
    {
      return -EINVAL;
    }

  memset(ap, 0, sizeof(*ap));
  ret = nxmutex_init(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_ap_peer_table_init(
      &ap->peers, max_stations > SV6621_AP_PEER_CAPACITY ?
                  SV6621_AP_PEER_CAPACITY : max_stations);
  if (ret < 0)
    {
      nxmutex_destroy(&ap->lock);
      return ret;
    }

  ret = sv6621_ap_event_queue_init(&ap->events, sv6621_ap_dispatch_event,
                                   sv6621_ap_dispatch_error, ap);
  if (ret < 0)
    {
      sv6621_ap_peer_table_deinit(&ap->peers);
      nxmutex_destroy(&ap->lock);
      return ret;
    }

  ap->command = command;
  ap->next_cookie = 1;
  ap->error = error;
  ap->error_arg = error_arg;
  ap->client = client;
  ap->client_arg = client_arg;
  memcpy(ap->address, address, SV6621_MAC_LENGTH);
  return 0;
}

void sv6621_ap_deinit(FAR struct sv6621_ap_s *ap)
{
  if (ap == NULL)
    {
      return;
    }

  if (nxmutex_lock(&ap->lock) >= 0)
    {
      DEBUGASSERT(!ap->active);
      nxmutex_unlock(&ap->lock);
    }

  sv6621_ap_event_queue_deinit(&ap->events);
  sv6621_ap_peer_table_deinit(&ap->peers);
  nxmutex_destroy(&ap->lock);
  memset(ap, 0, sizeof(*ap));
}

int sv6621_ap_enable(FAR struct sv6621_ap_s *ap, uint8_t instance,
                     FAR const struct sv6621_ap_config_s *config)
{
  struct sv6621_ap_beacon_config_s beacon;
  struct sv6621_ap_start_s start;
  struct sv6621_ap_context_s context;
  int ret;

  if (ap == NULL || config == NULL || config->ssid_length == 0 ||
      config->ssid_length > SV6621_SSID_MAX_LENGTH ||
      config->hidden_ssid > 2 || config->channel == 0 ||
      config->channel_width > SV6621_CHANNEL_WIDTH_160 ||
      config->band > SV6621_BAND_5GHZ || config->beacon_interval == 0 ||
      config->dtim_period == 0)
    {
      return -EINVAL;
    }

  if (config->security != SV6621_SECURITY_OPEN ||
      config->credential_length != 0)
    {
      return -EOPNOTSUPP;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return -EBUSY;
    }

  memset(&beacon, 0, sizeof(beacon));
  memcpy(beacon.address, ap->address, SV6621_MAC_LENGTH);
  memcpy(beacon.ssid, config->ssid, config->ssid_length);
  beacon.ssid_length = config->ssid_length;
  beacon.hidden_ssid = config->hidden_ssid;
  beacon.channel = config->channel;
  beacon.band = config->band;
  beacon.beacon_interval = config->beacon_interval;
  ret = sv6621_ap_build_beacon_templates(&beacon, &ap->templates);
  if (ret < 0)
    {
      nxmutex_unlock(&ap->lock);
      return ret;
    }

  memset(&start, 0, sizeof(start));
  start.beacon_interval = config->beacon_interval;
  start.dtim_period = config->dtim_period;
  start.hidden_ssid = config->hidden_ssid;
  start.channel = config->channel;
  start.channel_width = config->channel_width;
  start.center_channel1 = config->center_channel1;
  start.center_channel2 = config->center_channel2;
  start.band = config->band;
  memcpy(start.ssid, config->ssid, config->ssid_length);
  start.ssid_length = config->ssid_length;
  start.beacon_head.data = ap->templates.beacon_head;
  start.beacon_head.length = ap->templates.beacon_head_length;
  start.beacon_tail.data = ap->templates.beacon_tail;
  start.beacon_tail.length = ap->templates.beacon_tail_length;
  start.probe_response_ies.data = ap->templates.probe_response;
  start.probe_response_ies.length = ap->templates.probe_response_length;
  ret = sv6621_ap_start(ap->command, instance, &start, &context);
  if (ret == 0)
    {
      ap->config = *config;
      ap->context = context;
      ap->instance = instance;
      ap->active = true;
    }

  nxmutex_unlock(&ap->lock);
  return ret;
}

int sv6621_ap_disable(FAR struct sv6621_ap_s *ap)
{
  bool reset = false;
  int ret;

  if (ap == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return 0;
    }

  ret = sv6621_ap_stop(ap->command, ap->instance);
  if (ret == 0)
    {
      sv6621_ap_peer_table_reset(&ap->peers);
      memset(&ap->config, 0, sizeof(ap->config));
      memset(&ap->templates, 0, sizeof(ap->templates));
      memset(&ap->context, 0, sizeof(ap->context));
      ap->instance = 0;
      ap->active = false;
      reset = true;
    }

  nxmutex_unlock(&ap->lock);
  if (reset)
    {
      ret = sv6621_ap_event_queue_reset(&ap->events);
    }

  return ret;
}

int sv6621_ap_reset(FAR struct sv6621_ap_s *ap)
{
  int event_ret;
  int ret;

  if (ap == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_ap_peer_table_reset(&ap->peers);
  memset(&ap->config, 0, sizeof(ap->config));
  memset(&ap->templates, 0, sizeof(ap->templates));
  memset(&ap->context, 0, sizeof(ap->context));
  ap->instance = 0;
  ap->active = false;
  nxmutex_unlock(&ap->lock);

  event_ret = sv6621_ap_event_queue_reset(&ap->events);
  return ret < 0 ? ret : event_ret;
}

int sv6621_ap_queue_event(FAR struct sv6621_ap_s *ap, uint8_t instance,
                          uint8_t id, FAR const uint8_t *payload,
                          size_t length)
{
  if (ap == NULL)
    {
      return -EINVAL;
    }

  return sv6621_ap_event_queue_submit(&ap->events, instance, id, payload,
                                      length);
}

int sv6621_ap_resolve_tx(
    FAR struct sv6621_ap_s *ap, FAR const uint8_t *frame, size_t length,
    FAR struct sv6621_data_tx_context_s *context)
{
  struct sv6621_ap_peer_s peer;
  bool multicast;
  int ret;

  if (ap == NULL || frame == NULL || length < SV6621_MAC_LENGTH ||
      context == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return -ENETDOWN;
    }

  multicast = (frame[0] & 1) != 0;
  if (!multicast)
    {
      ret = sv6621_ap_peer_lookup(&ap->peers, frame, &peer);
      if (ret < 0)
        {
          nxmutex_unlock(&ap->lock);
          return ret;
        }

      if (!peer.bound ||
          (ap->config.security == SV6621_SECURITY_OPEN ?
           peer.state < SV6621_AP_PEER_ASSOCIATED :
           peer.state < SV6621_AP_PEER_AUTHORIZED))
        {
          nxmutex_unlock(&ap->lock);
          return -EHOSTUNREACH;
        }

      context->peer_index = peer.peer_index;
    }
  else
    {
      context->peer_index = ap->context.multicast_index;
    }

  context->multicast_index = ap->context.multicast_index;
  context->instance = ap->context.instance;
  context->lmac_id = ap->context.lmac_id;
  context->tid = 0;
  nxmutex_unlock(&ap->lock);
  return 0;
}

int sv6621_ap_validate_rx(FAR struct sv6621_ap_s *ap,
                          FAR const struct sv6621_data_rx_s *rx)
{
  struct sv6621_ap_peer_s peer;
  size_t index;
  bool matched = false;
  int ret;

  if (ap == NULL || rx == NULL || !rx->instance_valid || !rx->peer_valid)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active || rx->instance != ap->context.instance ||
      rx->lmac_id != ap->context.lmac_id)
    {
      nxmutex_unlock(&ap->lock);
      return -ENETDOWN;
    }

  ret = nxmutex_lock(&ap->peers.lock);
  if (ret < 0)
    {
      nxmutex_unlock(&ap->lock);
      return ret;
    }

  for (index = 0; index < ap->peers.capacity; index++)
    {
      peer = ap->peers.peers[index];
      if (peer.state != SV6621_AP_PEER_FREE && peer.bound &&
          peer.peer_index == rx->peer_index)
        {
          matched = ap->config.security == SV6621_SECURITY_OPEN ?
              peer.state >= SV6621_AP_PEER_ASSOCIATED :
              peer.state >= SV6621_AP_PEER_AUTHORIZED;
          break;
        }
    }

  nxmutex_unlock(&ap->peers.lock);
  nxmutex_unlock(&ap->lock);
  return matched ? 0 : -EHOSTUNREACH;
}

int sv6621_ap_forward_policy(FAR struct sv6621_ap_s *ap,
                             FAR const struct sv6621_data_rx_s *rx,
                             FAR bool *forward, FAR bool *deliver_local)
{
  int ret;

  if (ap == NULL || rx == NULL || forward == NULL || deliver_local == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return -ENETDOWN;
    }

  *forward = rx->need_forward && !ap->config.isolate;
  *deliver_local = !*forward || rx->multicast;
  nxmutex_unlock(&ap->lock);
  return 0;
}

bool sv6621_ap_is_active(FAR struct sv6621_ap_s *ap)
{
  bool active = false;

  if (ap != NULL && nxmutex_lock(&ap->lock) >= 0)
    {
      active = ap->active;
      nxmutex_unlock(&ap->lock);
    }

  return active;
}
