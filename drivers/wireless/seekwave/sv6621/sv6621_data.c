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

#define SV6621_DATA_RX_MSDU_LENGTH_OFFSET  2
#define SV6621_DATA_RX_EAPOL_OFFSET        1
#define SV6621_DATA_RX_SEQUENCE_OFFSET     8
#define SV6621_DATA_RX_CONTEXT_OFFSET      10
#define SV6621_DATA_RX_MSDU_OFFSET_OFFSET  18
#define SV6621_DATA_RX_ETHERNET_HEADER_TAIL 6
#define SV6621_DATA_RX_EAPOL_MASK          (1 << 6)
#define SV6621_DATA_ETHERNET_HEADER_SIZE    14
#define SV6621_DATA_MSDU_LENGTH_MASK        0x0fff
#define SV6621_DATA_PEER_INDEX_MASK         0x1f
#define SV6621_DATA_INSTANCE_MASK           0x03
#define SV6621_DATA_LMAC_MASK               0x03
#define SV6621_DATA_TID_MASK                0x0f
#define SV6621_DATA_ETHERTYPE_EAPOL         0x888e

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_data_get_le16(FAR const uint8_t *value);
static void sv6621_data_put_le16(FAR uint8_t *output, uint16_t value);
static void sv6621_data_packet(uint8_t channel,
                               FAR const uint8_t *payload, size_t length,
                               FAR void *arg);
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
 * Name: sv6621_data_packet
 ****************************************************************************/

static void sv6621_data_packet(uint8_t channel, FAR const uint8_t *payload,
                               size_t length, FAR void *arg)
{
  FAR struct sv6621_data_s *data = arg;
  struct sv6621_data_rx_s rx;

  if (sv6621_data_decode_rx(payload, length, &rx) < 0)
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
                          FAR struct sv6621_data_rx_s *rx)
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
                     payload + SV6621_DATA_RX_MSDU_LENGTH_OFFSET) +
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
  rx->eapol =
      (payload[SV6621_DATA_RX_EAPOL_OFFSET] & SV6621_DATA_RX_EAPOL_MASK) != 0;
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
 * Name: sv6621_data_set_tx_blocked
 ****************************************************************************/

int sv6621_data_set_tx_blocked(FAR struct sv6621_data_s *data, bool blocked)
{
  int ret;

  if (data == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&data->tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  data->tx_blocked = blocked;
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

  if (data->tx_blocked)
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
