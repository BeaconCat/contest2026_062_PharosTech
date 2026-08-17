/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_protocol.c
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

#include "sv6621_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_HEADER_LENGTH_MASK   UINT32_C(0x0000ffff)
#define SV6621_HEADER_PADDING_MASK  UINT32_C(0x007f0000)
#define SV6621_HEADER_PADDING_SHIFT 16
#define SV6621_HEADER_EOF           UINT32_C(0x00800000)
#define SV6621_HEADER_CHANNEL_SHIFT 24

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t sv6621_protocol_getle32(FAR const uint8_t encoded[4])
{
  return (uint32_t)encoded[0] | (uint32_t)encoded[1] << 8 |
         (uint32_t)encoded[2] << 16 | (uint32_t)encoded[3] << 24;
}

static void sv6621_protocol_putle32(uint32_t value, uint8_t encoded[4])
{
  encoded[0] = (uint8_t)value;
  encoded[1] = (uint8_t)(value >> 8);
  encoded[2] = (uint8_t)(value >> 16);
  encoded[3] = (uint8_t)(value >> 24);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_protocol_decode_header(FAR const uint8_t encoded[4],
                                  FAR struct sv6621_packet_header_s *header)
{
  uint32_t value;

  if (encoded == NULL || header == NULL)
    {
      return -EINVAL;
    }

  value = sv6621_protocol_getle32(encoded);
  header->length = (uint16_t)(value & SV6621_HEADER_LENGTH_MASK);
  header->padding = (uint8_t)((value & SV6621_HEADER_PADDING_MASK) >>
                              SV6621_HEADER_PADDING_SHIFT);
  header->end_of_frame = (value & SV6621_HEADER_EOF) != 0;
  header->channel = (uint8_t)(value >> SV6621_HEADER_CHANNEL_SHIFT);

  if (!header->end_of_frame &&
      header->channel >= SV6621_PACKET_CHANNEL_COUNT)
    {
      return -EPROTO;
    }

  if (!header->end_of_frame && header->length == 0)
    {
      return -EPROTO;
    }

  return OK;
}

int sv6621_protocol_encode_header(
    FAR const struct sv6621_packet_header_s *header, uint8_t encoded[4])
{
  uint32_t value;

  if (header == NULL || encoded == NULL)
    {
      return -EINVAL;
    }

  if ((!header->end_of_frame &&
       header->channel >= SV6621_PACKET_CHANNEL_COUNT) ||
      header->padding > 0x7f)
    {
      return -EINVAL;
    }

  if (!header->end_of_frame && header->length == 0)
    {
      return -EINVAL;
    }

  value = header->length |
          (uint32_t)header->padding << SV6621_HEADER_PADDING_SHIFT |
          (uint32_t)header->channel << SV6621_HEADER_CHANNEL_SHIFT;
  if (header->end_of_frame)
    {
      value |= SV6621_HEADER_EOF;
    }

  sv6621_protocol_putle32(value, encoded);
  return OK;
}
