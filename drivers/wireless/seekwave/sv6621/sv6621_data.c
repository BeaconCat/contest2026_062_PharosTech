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

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_data_get_le16(FAR const uint8_t *value);
static void sv6621_data_packet(uint8_t channel,
                               FAR const uint8_t *payload, size_t length,
                               FAR void *arg);

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
  data->stats.bytes += rx.frame_length;
  data->input(&rx, data->input_arg);
  (void)channel;
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
 * Name: sv6621_data_init
 ****************************************************************************/

int sv6621_data_init(FAR struct sv6621_data_s *data,
                     FAR struct sv6621_packet_router_s *router,
                     sv6621_data_input_t input, FAR void *input_arg)
{
  int ret;

  if (data == NULL || router == NULL || input == NULL)
    {
      return -EINVAL;
    }

  memset(data, 0, sizeof(*data));
  data->router = router;
  data->input = input;
  data->input_arg = input_arg;

  ret = sv6621_packet_subscribe(router, SV6621_CHANNEL_WIFI_DATA,
                                sv6621_data_packet, data);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_packet_subscribe(router, SV6621_CHANNEL_WIFI_DATA1,
                                sv6621_data_packet, data);
  if (ret < 0)
    {
      sv6621_packet_unsubscribe(router, SV6621_CHANNEL_WIFI_DATA,
                                sv6621_data_packet, data);
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
  data->router = NULL;
}
