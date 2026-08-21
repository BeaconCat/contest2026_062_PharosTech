/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_peer.c
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

#include "sv6621_ap_peer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_COMMAND_ADD_PEER         21
#define SV6621_AP_COMMAND_REMOVE_PEER      22
#define SV6621_AP_REMOVE_PEER_PAYLOAD_SIZE 9
#define SV6621_AP_PEER_RESPONSE_SIZE       1
#define SV6621_AP_PEER_COMMAND_TIMEOUT_MS  1000
#define SV6621_AP_PEER_INDEX_MAX           31
#define SV6621_AP_PEER_DEPARTURE_SIZE      7
#define SV6621_AP_AID_MIN                  1
#define SV6621_AP_AID_MAX                  2007

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR struct sv6621_ap_peer_s *
sv6621_ap_peer_find(FAR struct sv6621_ap_peer_table_s *table,
                    FAR const uint8_t address[SV6621_MAC_LENGTH]);
static bool
sv6621_ap_peer_aid_used(FAR const struct sv6621_ap_peer_table_s *table,
                        uint16_t aid);
static uint16_t
sv6621_ap_peer_allocate_aid(FAR struct sv6621_ap_peer_table_s *table);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct sv6621_ap_peer_s *
sv6621_ap_peer_find(FAR struct sv6621_ap_peer_table_s *table,
                    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  unsigned int index;

  for (index = 0; index < table->capacity; index++)
    {
      if (table->peers[index].state != SV6621_AP_PEER_FREE &&
          memcmp(table->peers[index].address, address, SV6621_MAC_LENGTH) == 0)
        {
          return &table->peers[index];
        }
    }

  return NULL;
}

static bool
sv6621_ap_peer_aid_used(FAR const struct sv6621_ap_peer_table_s *table,
                        uint16_t aid)
{
  unsigned int index;

  for (index = 0; index < table->capacity; index++)
    {
      if (table->peers[index].state >= SV6621_AP_PEER_ASSOCIATING &&
          table->peers[index].aid == aid)
        {
          return true;
        }
    }

  return false;
}

static uint16_t
sv6621_ap_peer_allocate_aid(FAR struct sv6621_ap_peer_table_s *table)
{
  uint16_t aid;
  uint16_t attempts;

  for (attempts = 0; attempts <= SV6621_AP_AID_MAX - SV6621_AP_AID_MIN;
       attempts++)
    {
      aid = table->next_aid;
      table->next_aid = aid == SV6621_AP_AID_MAX ? SV6621_AP_AID_MIN : aid + 1;
      if (!sv6621_ap_peer_aid_used(table, aid))
        {
          return aid;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_add_peer(FAR struct sv6621_command_engine_s *command,
                       uint8_t instance,
                       FAR const uint8_t address[SV6621_MAC_LENGTH],
                       FAR uint8_t *peer_index)
{
  uint8_t response[SV6621_AP_PEER_RESPONSE_SIZE];
  size_t response_length = sizeof(response);
  int ret;

  if (command == NULL || address == NULL || peer_index == NULL ||
      (address[0] & 1) != 0)
    {
      return -EINVAL;
    }

  ret = sv6621_command_execute(command, instance, SV6621_AP_COMMAND_ADD_PEER,
                               address, SV6621_MAC_LENGTH, response,
                               &response_length,
                               SV6621_AP_PEER_COMMAND_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (response_length != sizeof(response) ||
      response[0] > SV6621_AP_PEER_INDEX_MAX)
    {
      return -EPROTO;
    }

  *peer_index = response[0];
  return 0;
}

int sv6621_ap_remove_peer(FAR struct sv6621_command_engine_s *command,
                          uint8_t instance,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          uint16_t reason, bool transmit_frame)
{
  uint8_t payload[SV6621_AP_REMOVE_PEER_PAYLOAD_SIZE];

  if (command == NULL || address == NULL || (address[0] & 1) != 0)
    {
      return -EINVAL;
    }

  memcpy(payload, address, SV6621_MAC_LENGTH);
  payload[6] = reason;
  payload[7] = reason >> 8;
  payload[8] = transmit_frame;
  return sv6621_command_execute(
      command, instance, SV6621_AP_COMMAND_REMOVE_PEER, payload,
      sizeof(payload), NULL, NULL, SV6621_AP_PEER_COMMAND_TIMEOUT_MS);
}

int sv6621_ap_peer_table_init(FAR struct sv6621_ap_peer_table_s *table,
                              uint8_t capacity)
{
  if (table == NULL || capacity == 0 || capacity > SV6621_AP_PEER_CAPACITY)
    {
      return -EINVAL;
    }

  memset(table, 0, sizeof(*table));
  table->capacity = capacity;
  table->next_aid = SV6621_AP_AID_MIN;
  return nxmutex_init(&table->lock);
}

void sv6621_ap_peer_table_deinit(FAR struct sv6621_ap_peer_table_s *table)
{
  if (table == NULL)
    {
      return;
    }

  nxmutex_destroy(&table->lock);
  memset(table, 0, sizeof(*table));
}

int sv6621_ap_peer_table_reset(FAR struct sv6621_ap_peer_table_s *table)
{
  int ret;

  if (table == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(table->peers, 0, sizeof(table->peers));
  table->next_aid = SV6621_AP_AID_MIN;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_authenticate(FAR struct sv6621_ap_peer_table_s *table,
                                FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_peer_s *peer = NULL;
  unsigned int index;
  int ret;

  if (table == NULL || address == NULL || (address[0] & 1) != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer != NULL)
    {
      peer->state = SV6621_AP_PEER_AUTHENTICATED;
      peer->aid = 0;
      peer->capability = 0;
      peer->previous_state = SV6621_AP_PEER_FREE;
      peer->previous_aid = 0;
      peer->previous_capability = 0;
      nxmutex_unlock(&table->lock);
      return 0;
    }

  for (index = 0; index < table->capacity; index++)
    {
      if (table->peers[index].state == SV6621_AP_PEER_FREE)
        {
          peer = &table->peers[index];
          break;
        }
    }

  if (peer == NULL)
    {
      nxmutex_unlock(&table->lock);
      return -ENOSPC;
    }

  memset(peer, 0, sizeof(*peer));
  memcpy(peer->address, address, SV6621_MAC_LENGTH);
  peer->state = SV6621_AP_PEER_AUTHENTICATED;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_bind(FAR struct sv6621_ap_peer_table_s *table,
                        FAR const uint8_t address[SV6621_MAC_LENGTH],
                        uint8_t peer_index)
{
  FAR struct sv6621_ap_peer_s *peer;
  unsigned int index;
  int ret;

  if (table == NULL || address == NULL ||
      peer_index > SV6621_AP_PEER_INDEX_MAX)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || peer->state != SV6621_AP_PEER_AUTHENTICATED)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -EALREADY;
    }

  for (index = 0; index < table->capacity; index++)
    {
      if (&table->peers[index] != peer &&
          table->peers[index].state != SV6621_AP_PEER_FREE &&
          table->peers[index].bound &&
          table->peers[index].peer_index == peer_index)
        {
          nxmutex_unlock(&table->lock);
          return -EEXIST;
        }
    }

  peer->peer_index = peer_index;
  peer->bound = true;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_prepare_association(
    FAR struct sv6621_ap_peer_table_s *table,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint16_t capability,
    FAR uint16_t *aid)
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL || aid == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || peer->state < SV6621_AP_PEER_AUTHENTICATED ||
      peer->state == SV6621_AP_PEER_ASSOCIATING || !peer->bound)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : !peer->bound ? -EAGAIN : -EALREADY;
    }

  peer->previous_state = peer->state;
  peer->previous_aid = peer->aid;
  peer->previous_capability = peer->capability;
  if (peer->state == SV6621_AP_PEER_AUTHENTICATED)
    {
      peer->aid = sv6621_ap_peer_allocate_aid(table);
      if (peer->aid == 0)
        {
          nxmutex_unlock(&table->lock);
          return -ENOSPC;
        }
    }

  peer->capability = capability;
  peer->state = SV6621_AP_PEER_ASSOCIATING;
  *aid = peer->aid;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_cancel_association(FAR struct sv6621_ap_peer_table_s *table,
                                      FAR const uint8_t
                                          address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || peer->state != SV6621_AP_PEER_ASSOCIATING)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -EALREADY;
    }

  peer->aid = peer->previous_aid;
  peer->capability = peer->previous_capability;
  peer->state = peer->previous_state;
  peer->previous_aid = 0;
  peer->previous_capability = 0;
  peer->previous_state = SV6621_AP_PEER_FREE;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_begin_tx(FAR struct sv6621_ap_peer_table_s *table,
                            FAR const uint8_t address[SV6621_MAC_LENGTH],
                            uint64_t cookie)
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || peer->tx_pending)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -EBUSY;
    }

  peer->pending_cookie = cookie;
  peer->tx_pending = true;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_cancel_tx(FAR struct sv6621_ap_peer_table_s *table,
                             FAR const uint8_t address[SV6621_MAC_LENGTH],
                             uint64_t cookie)
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || !peer->tx_pending || peer->pending_cookie != cookie)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -ESTALE;
    }

  peer->pending_cookie = 0;
  peer->tx_pending = false;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_complete_tx(FAR struct sv6621_ap_peer_table_s *table,
                               FAR const uint8_t address[SV6621_MAC_LENGTH],
                               uint64_t cookie, bool association, bool success,
                               FAR bool *remove)
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL || remove == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || !peer->tx_pending || peer->pending_cookie != cookie ||
      (association && peer->state != SV6621_AP_PEER_ASSOCIATING))
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -ESTALE;
    }

  peer->pending_cookie = 0;
  peer->tx_pending = false;
  *remove = !success;
  if (success && association)
    {
      peer->state = SV6621_AP_PEER_ASSOCIATED;
      peer->previous_state = SV6621_AP_PEER_FREE;
      peer->previous_aid = 0;
      peer->previous_capability = 0;
    }
  else if (!success)
    {
      memset(peer, 0, sizeof(*peer));
    }

  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_authorize(FAR struct sv6621_ap_peer_table_s *table,
                             FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_peer_s *peer;
  int ret;

  if (table == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_peer_find(table, address);
  if (peer == NULL || peer->state < SV6621_AP_PEER_ASSOCIATED)
    {
      nxmutex_unlock(&table->lock);
      return peer == NULL ? -ENOENT : -EAGAIN;
    }

  peer->state = SV6621_AP_PEER_AUTHORIZED;
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_peer_lookup(FAR struct sv6621_ap_peer_table_s *table,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          FAR struct sv6621_ap_peer_s *peer)
{
  FAR struct sv6621_ap_peer_s *entry;
  int ret;

  if (table == NULL || address == NULL || peer == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  entry = sv6621_ap_peer_find(table, address);
  if (entry != NULL)
    {
      *peer = *entry;
    }

  nxmutex_unlock(&table->lock);
  return entry == NULL ? -ENOENT : 0;
}

int sv6621_ap_peer_forget(FAR struct sv6621_ap_peer_table_s *table,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          FAR struct sv6621_ap_peer_s *peer)
{
  FAR struct sv6621_ap_peer_s *entry;
  int ret;

  if (table == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&table->lock);
  if (ret < 0)
    {
      return ret;
    }

  entry = sv6621_ap_peer_find(table, address);
  if (entry == NULL)
    {
      nxmutex_unlock(&table->lock);
      return -ENOENT;
    }

  if (peer != NULL)
    {
      *peer = *entry;
    }

  memset(entry, 0, sizeof(*entry));
  nxmutex_unlock(&table->lock);
  return 0;
}

int sv6621_ap_parse_peer_departure(FAR const uint8_t *payload,
                                   size_t payload_length,
                                   FAR uint8_t address[SV6621_MAC_LENGTH],
                                   FAR uint16_t *reason)
{
  if (payload == NULL || address == NULL || reason == NULL ||
      payload_length != SV6621_AP_PEER_DEPARTURE_SIZE)
    {
      return -EINVAL;
    }

  *reason = payload[0];
  memcpy(address, payload + 1, SV6621_MAC_LENGTH);
  if ((address[0] & 1) != 0)
    {
      return -EPROTO;
    }

  return 0;
}

int sv6621_ap_peer_departed(FAR struct sv6621_ap_peer_table_s *table,
                            FAR struct sv6621_command_engine_s *command,
                            uint8_t instance,
                            FAR const uint8_t address[SV6621_MAC_LENGTH],
                            uint16_t reason, bool firmware_event)
{
  struct sv6621_ap_peer_s peer;
  int ret;

  if (table == NULL || command == NULL || address == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_ap_peer_lookup(table, address, &peer);
  if (ret < 0)
    {
      return ret;
    }

  if (!firmware_event && peer.bound)
    {
      ret = sv6621_ap_remove_peer(command, instance, address, reason, false);
      if (ret < 0)
        {
          return ret;
        }
    }

  return sv6621_ap_peer_forget(table, address, NULL);
}
