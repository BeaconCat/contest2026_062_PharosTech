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
