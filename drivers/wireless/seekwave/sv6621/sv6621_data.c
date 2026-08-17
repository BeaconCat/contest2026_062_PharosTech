/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_data.c
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

#include "sv6621_data.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_DATA_RX_STATUS_OFFSET       0
#define SV6621_DATA_RX_MSDU_LENGTH_OFFSET  2
#define SV6621_DATA_RX_ATTRIBUTES_OFFSET   4
#define SV6621_DATA_RX_CHECKSUM_OFFSET     5
#define SV6621_DATA_RX_FILTER_OFFSET       7
#define SV6621_DATA_RX_SEQUENCE_OFFSET     8
#define SV6621_DATA_RX_CONTEXT_OFFSET      10
#define SV6621_DATA_RX_PN_OFFSET           12
#define SV6621_DATA_RX_REUSED_LENGTH_OFFSET 16
#define SV6621_DATA_RX_MSDU_OFFSET_OFFSET  18
#define SV6621_DATA_RX_AMSDU_INDEX_OFFSET  19
#define SV6621_DATA_RX_ETHERNET_HEADER_TAIL 6
#define SV6621_DATA_RX_MORE_DATA_MASK      (1 << 1)
#define SV6621_DATA_RX_RETRY_MASK          (1 << 3)
#define SV6621_DATA_RX_CIPHER_MASK         0x0f
#define SV6621_DATA_RX_EAPOL_MASK          (1 << 6)
#define SV6621_DATA_RX_CHECKSUM_VALID_MASK (1 << 0)
#define SV6621_DATA_RX_AMPDU_MASK          (1 << 1)
#define SV6621_DATA_RX_SNAP_MATCH_MASK     (1 << 2)
#define SV6621_DATA_RX_AMSDU_MASK          (1 << 3)
#define SV6621_DATA_RX_QOS_DATA_MASK       (1 << 4)
#define SV6621_DATA_RX_AMSDU_FIRST_MASK    (1 << 5)
#define SV6621_DATA_RX_AMSDU_LAST_MASK     (1 << 6)
#define SV6621_DATA_RX_MORE_FRAGMENTS_MASK (1 << 3)
#define SV6621_DATA_RX_FIRST_MSDU_MASK     (1 << 11)
#define SV6621_DATA_RX_AMSDU_INDEX_MASK    0x3f
#define SV6621_DATA_RX_NEED_FORWARD_MASK   (1 << 6)
#define SV6621_DATA_RX_MAC_DROP_FRAG_MASK  (1 << 7)
#define SV6621_DATA_ETHERNET_HEADER_SIZE    14
#define SV6621_DATA_MSDU_LENGTH_MASK        0x0fff
#define SV6621_DATA_PEER_INDEX_MASK         0x1f
#define SV6621_DATA_INSTANCE_MASK           0x03
#define SV6621_DATA_LMAC_MASK               0x03
#define SV6621_DATA_TID_MASK                0x0f
#define SV6621_DATA_ETHERTYPE_EAPOL         0x888e
#define SV6621_DATA_CIPHER_CCMP              8
#define SV6621_DATA_CIPHER_CCMP_256          9
#define SV6621_DATA_CIPHER_GCMP              10
#define SV6621_DATA_CIPHER_GCMP_256          11

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_data_get_le16(FAR const uint8_t *value);
static void sv6621_data_put_le16(FAR uint8_t *output, uint16_t value);
static void sv6621_data_packet(uint8_t channel,
                               FAR const uint8_t *payload, size_t length,
                               FAR void *arg);
static FAR struct sv6621_data_fragment_s *
sv6621_data_find_fragment(FAR struct sv6621_data_s *data,
                          FAR const struct sv6621_data_rx_s *rx);
static FAR struct sv6621_data_fragment_s *
sv6621_data_select_fragment(FAR struct sv6621_data_s *data);
static bool sv6621_data_cipher_uses_pn(uint8_t cipher);
static bool sv6621_data_pn_follows(FAR const uint8_t *previous,
                                   FAR const uint8_t *current,
                                   size_t length);
static int sv6621_data_reassemble(FAR struct sv6621_data_s *data,
                                  FAR struct sv6621_data_rx_s *rx);
static bool sv6621_data_take_credit(FAR struct sv6621_data_s *data,
                                    uint8_t lmac_id);
static void sv6621_data_restore_credit(FAR struct sv6621_data_s *data,
                                       uint8_t lmac_id);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_data_get_le16
 ****************************************************************************/

static uint16_t sv6621_data_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

/****************************************************************************
 * Name: sv6621_data_put_le16
 ****************************************************************************/

static void sv6621_data_put_le16(FAR uint8_t *output, uint16_t value)
{
  output[0] = value & 0xff;
  output[1] = value >> 8;
}

/****************************************************************************
 * Name: sv6621_data_find_fragment
 ****************************************************************************/

static FAR struct sv6621_data_fragment_s *
sv6621_data_find_fragment(FAR struct sv6621_data_s *data,
                          FAR const struct sv6621_data_rx_s *rx)
{
  unsigned int index;

  for (index = 0; index < SV6621_DATA_FRAGMENT_ENTRIES; index++)
    {
      FAR struct sv6621_data_fragment_s *entry = &data->fragments[index];

      if (entry->active && entry->sequence == rx->sequence &&
          entry->tid == rx->tid && entry->instance == rx->instance &&
          entry->instance_valid == rx->instance_valid &&
          entry->peer_index == rx->peer_index &&
          entry->peer_valid == rx->peer_valid)
        {
          return entry;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: sv6621_data_select_fragment
 ****************************************************************************/

static FAR struct sv6621_data_fragment_s *
sv6621_data_select_fragment(FAR struct sv6621_data_s *data)
{
  FAR struct sv6621_data_fragment_s *oldest = &data->fragments[0];
  unsigned int index;

  for (index = 0; index < SV6621_DATA_FRAGMENT_ENTRIES; index++)
    {
      FAR struct sv6621_data_fragment_s *entry = &data->fragments[index];

      if (!entry->active)
        {
          return entry;
        }

      if ((int32_t)(entry->age - oldest->age) < 0)
        {
          oldest = entry;
        }
    }

  data->stats.fragment_evictions++;
  return oldest;
}

/****************************************************************************
 * Name: sv6621_data_cipher_uses_pn
 ****************************************************************************/

static bool sv6621_data_cipher_uses_pn(uint8_t cipher)
{
  return cipher == SV6621_DATA_CIPHER_CCMP ||
         cipher == SV6621_DATA_CIPHER_CCMP_256 ||
         cipher == SV6621_DATA_CIPHER_GCMP ||
         cipher == SV6621_DATA_CIPHER_GCMP_256;
}

/****************************************************************************
 * Name: sv6621_data_pn_follows
 ****************************************************************************/

static bool sv6621_data_pn_follows(FAR const uint8_t *previous,
                                   FAR const uint8_t *current,
                                   size_t length)
{
  unsigned int carry = 1;
  size_t index;

  for (index = 0; index < length; index++)
    {
      uint8_t expected = previous[index] + carry;

      carry = carry != 0 && previous[index] == UINT8_MAX;
      if (current[index] != expected)
        {
          return false;
        }
    }

  return carry == 0;
}

/****************************************************************************
 * Name: sv6621_data_reassemble
 ****************************************************************************/

static int sv6621_data_reassemble(FAR struct sv6621_data_s *data,
                                  FAR struct sv6621_data_rx_s *rx)
{
  FAR struct sv6621_data_fragment_s *entry;
  size_t append_length;

  if (rx->fragment == 0 && !rx->more_fragments)
    {
      return 0;
    }

  data->stats.fragments++;
  if (rx->fragment == 0)
    {
      if (rx->frame_length > SV6621_DATA_MAX_FRAME_SIZE)
        {
          data->stats.fragment_drops++;
          return -EMSGSIZE;
        }

      entry = sv6621_data_find_fragment(data, rx);
      if (entry == NULL)
        {
          entry = sv6621_data_select_fragment(data);
        }

      memset(entry, 0, sizeof(*entry));
      entry->first = *rx;
      entry->age = ++data->fragment_age;
      entry->length = rx->frame_length;
      entry->sequence = rx->sequence;
      entry->expected_fragment = 1;
      entry->instance = rx->instance;
      entry->instance_valid = rx->instance_valid;
      entry->peer_index = rx->peer_index;
      entry->peer_valid = rx->peer_valid;
      entry->tid = rx->tid;
      entry->active = true;
      memcpy(entry->last_packet_number, rx->packet_number,
             sizeof(entry->last_packet_number));
      memcpy(entry->frame, rx->frame, rx->frame_length);
      return -EINPROGRESS;
    }

  entry = sv6621_data_find_fragment(data, rx);
  if (entry == NULL || entry->expected_fragment != rx->fragment ||
      rx->frame_length <= 12)
    {
      if (entry != NULL)
        {
          entry->active = false;
        }

      data->stats.fragment_drops++;
      return -EPROTO;
    }

  if (sv6621_data_cipher_uses_pn(entry->first.cipher))
    {
      size_t pn_length = data->pn_reuse ? 4 : 6;

      if (rx->cipher != entry->first.cipher ||
          !sv6621_data_pn_follows(entry->last_packet_number,
                                  rx->packet_number, pn_length))
        {
          entry->active = false;
          data->stats.fragment_drops++;
          data->stats.fragment_pn_drops++;
          return -EACCES;
        }

      memcpy(entry->last_packet_number, rx->packet_number,
             sizeof(entry->last_packet_number));
    }

  append_length = rx->frame_length - 12;
  if (append_length > sizeof(entry->frame) - entry->length)
    {
      entry->active = false;
      data->stats.fragment_drops++;
      return -EMSGSIZE;
    }

  memcpy(entry->frame + entry->length, rx->frame + 12, append_length);
  entry->length += append_length;
  entry->expected_fragment++;
  if (rx->more_fragments)
    {
      return -EINPROGRESS;
    }

  *rx = entry->first;
  rx->frame = entry->frame;
  rx->frame_length = entry->length;
  rx->checksum = 0;
  rx->checksum_valid = false;
  rx->more_fragments = false;
  entry->active = false;
  data->stats.reassembled++;
  return 0;
}

/****************************************************************************
 * Name: sv6621_data_packet
 ****************************************************************************/

static void sv6621_data_packet(uint8_t channel, FAR const uint8_t *payload,
                               size_t length, FAR void *arg)
{
  FAR struct sv6621_data_s *data = arg;
  struct sv6621_data_rx_s rx;
  int ret;

  if (sv6621_data_decode_rx(payload, length, data->pn_reuse, &rx) < 0)
    {
      data->stats.malformed++;
      return;
    }

  ret = sv6621_data_reassemble(data, &rx);
  if (ret == -EINPROGRESS)
    {
      return;
    }

  if (ret < 0)
    {
      data->stats.malformed++;
      return;
    }

  data->stats.received++;
  data->stats.received_bytes += rx.frame_length;
  if ((rx.eapol ||
       (((uint16_t)rx.frame[12] << 8) | rx.frame[13]) ==
           SV6621_DATA_ETHERTYPE_EAPOL) &&
      data->eapol_input != NULL)
    {
      data->eapol_input(&rx, data->eapol_arg);
    }
  else
    {
      data->input(&rx, data->input_arg);
    }
  (void)channel;
}

/****************************************************************************
 * Name: sv6621_data_take_credit
 ****************************************************************************/

static bool sv6621_data_take_credit(FAR struct sv6621_data_s *data,
                                    uint8_t lmac_id)
{
  irqstate_t flags;
  bool available;

  flags = spin_lock_irqsave(&data->credit_lock);
  available = lmac_id < SV6621_DATA_LMAC_COUNT &&
              data->credits[lmac_id] > 0;
  if (available)
    {
      data->credits[lmac_id]--;
    }

  spin_unlock_irqrestore(&data->credit_lock, flags);
  return available;
}

/****************************************************************************
 * Name: sv6621_data_restore_credit
 ****************************************************************************/

static void sv6621_data_restore_credit(FAR struct sv6621_data_s *data,
                                       uint8_t lmac_id)
{
  irqstate_t flags;

  if (lmac_id >= SV6621_DATA_LMAC_COUNT)
    {
      return;
    }

  flags = spin_lock_irqsave(&data->credit_lock);
  if (data->credits[lmac_id] < UINT16_MAX)
    {
      data->credits[lmac_id]++;
    }

  spin_unlock_irqrestore(&data->credit_lock, flags);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_data_decode_rx
 ****************************************************************************/

int sv6621_data_decode_rx(FAR const uint8_t *payload, size_t length,
                          bool pn_reuse, FAR struct sv6621_data_rx_s *rx)
{
  uint16_t sequence;
  uint16_t context;
  size_t frame_length;
  uint8_t frame_offset;

  if (payload == NULL || rx == NULL ||
      length < SV6621_DATA_RX_DESCRIPTOR_SIZE)
    {
      return -EINVAL;
    }

  frame_offset = payload[SV6621_DATA_RX_MSDU_OFFSET_OFFSET];
  frame_length = sv6621_data_get_le16(
                     payload + (pn_reuse ?
                       SV6621_DATA_RX_REUSED_LENGTH_OFFSET :
                       SV6621_DATA_RX_MSDU_LENGTH_OFFSET)) +
                 SV6621_DATA_RX_ETHERNET_HEADER_TAIL;
  if (frame_offset < SV6621_DATA_RX_PREFIX_SIZE ||
      frame_length < 14 || frame_offset > length ||
      frame_length > length - frame_offset)
    {
      return -EPROTO;
    }

  sequence = sv6621_data_get_le16(
      payload + SV6621_DATA_RX_SEQUENCE_OFFSET);
  context = sv6621_data_get_le16(payload + SV6621_DATA_RX_CONTEXT_OFFSET);

  memset(rx, 0, sizeof(*rx));
  rx->frame = payload + frame_offset;
  rx->frame_length = frame_length;
  rx->sequence = sequence & 0x0fff;
  rx->fragment = sequence >> 12;
  rx->instance = context & 0x03;
  rx->instance_valid = (context & (1 << 2)) != 0;
  rx->peer_index = (context >> 4) & 0x1f;
  rx->peer_valid = (context & (1 << 9)) != 0;
  rx->multicast = (context & (1 << 10)) != 0;
  rx->tid = context >> 12;
  rx->checksum =
      sv6621_data_get_le16(payload + SV6621_DATA_RX_CHECKSUM_OFFSET);
  rx->cipher =
      payload[SV6621_DATA_RX_STATUS_OFFSET + 1] & SV6621_DATA_RX_CIPHER_MASK;
  rx->msdu_filter = payload[SV6621_DATA_RX_FILTER_OFFSET];
  memcpy(rx->packet_number, payload + SV6621_DATA_RX_PN_OFFSET,
         sizeof(rx->packet_number));
  if (pn_reuse)
    {
      rx->packet_number[4] = 0;
      rx->packet_number[5] = 0;
    }
  rx->amsdu_index = payload[SV6621_DATA_RX_AMSDU_INDEX_OFFSET] &
                    SV6621_DATA_RX_AMSDU_INDEX_MASK;
  rx->eapol = (payload[SV6621_DATA_RX_STATUS_OFFSET + 1] &
               SV6621_DATA_RX_EAPOL_MASK) != 0;
  rx->more_data = (payload[SV6621_DATA_RX_STATUS_OFFSET] &
                   SV6621_DATA_RX_MORE_DATA_MASK) != 0;
  rx->retry = (payload[SV6621_DATA_RX_STATUS_OFFSET] &
               SV6621_DATA_RX_RETRY_MASK) != 0;
  rx->checksum_valid = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
                        SV6621_DATA_RX_CHECKSUM_VALID_MASK) != 0;
  rx->ampdu = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
               SV6621_DATA_RX_AMPDU_MASK) != 0;
  rx->snap_match = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
                    SV6621_DATA_RX_SNAP_MATCH_MASK) != 0;
  rx->amsdu = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
               SV6621_DATA_RX_AMSDU_MASK) != 0;
  rx->qos_data = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
                  SV6621_DATA_RX_QOS_DATA_MASK) != 0;
  rx->amsdu_first = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
                     SV6621_DATA_RX_AMSDU_FIRST_MASK) != 0;
  rx->amsdu_last = (payload[SV6621_DATA_RX_ATTRIBUTES_OFFSET] &
                    SV6621_DATA_RX_AMSDU_LAST_MASK) != 0;
  rx->more_fragments = (context & SV6621_DATA_RX_MORE_FRAGMENTS_MASK) != 0;
  rx->first_msdu = (context & SV6621_DATA_RX_FIRST_MSDU_MASK) != 0;
  rx->need_forward = (payload[SV6621_DATA_RX_AMSDU_INDEX_OFFSET] &
                      SV6621_DATA_RX_NEED_FORWARD_MASK) != 0;
  rx->mac_dropped_fragments =
      (payload[SV6621_DATA_RX_AMSDU_INDEX_OFFSET] &
       SV6621_DATA_RX_MAC_DROP_FRAG_MASK) != 0;
  return 0;
}

/****************************************************************************
 * Name: sv6621_data_encode_tx
 ****************************************************************************/

int sv6621_data_encode_tx(
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length, FAR uint8_t *payload,
    size_t capacity, FAR size_t *written)
{
  uint16_t descriptor;
  uint8_t peer_index;

  if (context == NULL || frame == NULL || payload == NULL || written == NULL ||
      frame_length < SV6621_DATA_ETHERNET_HEADER_SIZE ||
      frame_length > SV6621_DATA_MSDU_LENGTH_MASK ||
      capacity < SV6621_DATA_TX_DESCRIPTOR_SIZE + frame_length ||
      context->peer_index > SV6621_DATA_PEER_INDEX_MASK ||
      context->multicast_index > SV6621_DATA_PEER_INDEX_MASK ||
      context->instance > SV6621_DATA_INSTANCE_MASK ||
      context->lmac_id > SV6621_DATA_LMAC_MASK ||
      context->tid > SV6621_DATA_TID_MASK)
    {
      return -EINVAL;
    }

  peer_index = (frame[0] & 1) != 0 ? context->multicast_index :
                                     context->peer_index;
  descriptor = ((uint16_t)context->instance << 2) |
               ((uint16_t)context->tid << 4) |
               ((uint16_t)peer_index << 8);
  sv6621_data_put_le16(payload, descriptor);

  descriptor = (uint16_t)frame_length |
               ((uint16_t)context->lmac_id << 12);
  sv6621_data_put_le16(payload + 2, descriptor);

  payload[4] = frame[12];
  payload[5] = frame[13];
  payload[6] = 0;
  payload[7] = 0;
  memcpy(payload + SV6621_DATA_TX_DESCRIPTOR_SIZE, frame, frame_length);
  *written = SV6621_DATA_TX_DESCRIPTOR_SIZE + frame_length;
  return 0;
}

/****************************************************************************
 * Name: sv6621_data_init
 ****************************************************************************/

int sv6621_data_init(FAR struct sv6621_data_s *data,
                     FAR struct sv6621_packet_router_s *router,
                     FAR struct sv6621_tx_s *tx, sv6621_data_input_t input,
                     FAR void *input_arg)
{
  int ret;

  if (data == NULL || router == NULL || tx == NULL || input == NULL)
    {
      return -EINVAL;
    }

  memset(data, 0, sizeof(*data));
  data->router = router;
  data->tx = tx;
  data->input = input;
  data->input_arg = input_arg;

  ret = nxmutex_init(&data->tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_packet_subscribe(router, SV6621_CHANNEL_WIFI_DATA,
                                sv6621_data_packet, data);
  if (ret < 0)
    {
      nxmutex_destroy(&data->tx_lock);
      return ret;
    }

  ret = sv6621_packet_subscribe(router, SV6621_CHANNEL_WIFI_DATA1,
                                sv6621_data_packet, data);
  if (ret < 0)
    {
      sv6621_packet_unsubscribe(router, SV6621_CHANNEL_WIFI_DATA,
                                sv6621_data_packet, data);
      nxmutex_destroy(&data->tx_lock);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_data_deinit
 ****************************************************************************/

void sv6621_data_deinit(FAR struct sv6621_data_s *data)
{
  if (data == NULL || data->router == NULL)
    {
      return;
    }

  sv6621_packet_unsubscribe(data->router, SV6621_CHANNEL_WIFI_DATA1,
                            sv6621_data_packet, data);
  sv6621_packet_unsubscribe(data->router, SV6621_CHANNEL_WIFI_DATA,
                            sv6621_data_packet, data);
  nxmutex_destroy(&data->tx_lock);
  sv6621_data_reset_fragments(data);
  data->router = NULL;
  data->tx = NULL;
}

/****************************************************************************
 * Name: sv6621_data_set_eapol_input
 ****************************************************************************/

void sv6621_data_set_eapol_input(FAR struct sv6621_data_s *data,
                                  sv6621_data_input_t input, FAR void *arg)
{
  if (data != NULL)
    {
      data->eapol_input = input;
      data->eapol_arg = arg;
    }
}

/****************************************************************************
 * Name: sv6621_data_add_credits
 ****************************************************************************/

void sv6621_data_add_credits(FAR struct sv6621_data_s *data,
                             uint16_t lmac0, uint16_t lmac1)
{
  const uint16_t added[SV6621_DATA_LMAC_COUNT] = { lmac0, lmac1 };
  irqstate_t flags;
  unsigned int index;

  if (data == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(&data->credit_lock);
  for (index = 0; index < SV6621_DATA_LMAC_COUNT; index++)
    {
      uint32_t total = (uint32_t)data->credits[index] + added[index];

      data->credits[index] = total > UINT16_MAX ? UINT16_MAX : total;
    }

  data->stats.credit_updates++;
  spin_unlock_irqrestore(&data->credit_lock, flags);
}

/****************************************************************************
 * Name: sv6621_data_reset_credits
 ****************************************************************************/

void sv6621_data_reset_credits(FAR struct sv6621_data_s *data)
{
  irqstate_t flags;

  if (data == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(&data->credit_lock);
  memset(data->credits, 0, sizeof(data->credits));
  spin_unlock_irqrestore(&data->credit_lock, flags);
}

/****************************************************************************
 * Name: sv6621_data_reset_fragments
 ****************************************************************************/

void sv6621_data_reset_fragments(FAR struct sv6621_data_s *data)
{
  if (data != NULL)
    {
      memset(data->fragments, 0, sizeof(data->fragments));
      data->fragment_age = 0;
    }
}

/****************************************************************************
 * Name: sv6621_data_set_pn_reuse
 ****************************************************************************/

void sv6621_data_set_pn_reuse(FAR struct sv6621_data_s *data, bool enabled)
{
  if (data != NULL)
    {
      data->pn_reuse = enabled;
    }
}

/****************************************************************************
 * Name: sv6621_data_set_tx_block
 ****************************************************************************/

int sv6621_data_set_tx_block(FAR struct sv6621_data_s *data, uint8_t reason,
                             bool blocked)
{
  int ret;

  if (data == NULL || reason == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&data->tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (blocked)
    {
      data->tx_block_reasons |= reason;
    }
  else
    {
      data->tx_block_reasons &= ~reason;
    }

  nxmutex_unlock(&data->tx_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_data_send
 ****************************************************************************/

int sv6621_data_send(FAR struct sv6621_data_s *data,
                     FAR const struct sv6621_data_tx_context_s *context,
                     FAR const uint8_t *frame, size_t frame_length)
{
  uint8_t payload[SV6621_DATA_TX_DESCRIPTOR_SIZE +
                  SV6621_DATA_MAX_FRAME_SIZE];
  size_t payload_length;
  size_t packet_length;
  int ret;

  if (data == NULL || data->tx == NULL || frame == NULL ||
      frame_length > SV6621_DATA_MAX_FRAME_SIZE)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&data->tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (data->tx_block_reasons != 0)
    {
      ret = -EAGAIN;
      goto unlock;
    }

  ret = sv6621_data_encode_tx(context, frame, frame_length, payload,
                              sizeof(payload), &payload_length);
  if (ret < 0)
    {
      goto unlock;
    }

  ret = sv6621_packet_build(SV6621_CHANNEL_WIFI_DATA, payload,
                            payload_length, data->tx_buffer,
                            sizeof(data->tx_buffer), &packet_length);
  if (ret < 0)
    {
      goto unlock;
    }

  if (!sv6621_data_take_credit(data, context->lmac_id))
    {
      data->stats.credit_starvations++;
      ret = -EAGAIN;
      goto unlock;
    }

  ret = sv6621_tx_send(data->tx, data->tx_buffer, packet_length);
  if (ret < 0)
    {
      sv6621_data_restore_credit(data, context->lmac_id);
      data->stats.transmit_errors++;
    }
  else
    {
      data->stats.transmitted++;
      data->stats.transmitted_bytes += frame_length;
    }

unlock:
  nxmutex_unlock(&data->tx_lock);
  return ret;
}
