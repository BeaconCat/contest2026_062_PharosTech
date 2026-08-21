/****************************************************************************
 * drivers/drivers/sv6621/sv6621_management.c
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

#include "sv6621_management.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_MANAGEMENT_COMMAND_TX         14
#define SV6621_MANAGEMENT_TX_HEADER_SIZE     17
#define SV6621_MANAGEMENT_TX_STATUS_SIZE     12
#define SV6621_MANAGEMENT_COMMAND_TIMEOUT_MS 1000
#define SV6621_MANAGEMENT_MAX_FRAME_SIZE                          \
  (SV6621_COMMAND_MAX_MESSAGE_SIZE - SV6621_COMMAND_HEADER_SIZE - \
   SV6621_MANAGEMENT_TX_HEADER_SIZE)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_management_put_le16(FAR uint8_t *value, uint16_t number);
static void sv6621_management_put_le32(FAR uint8_t *value, uint32_t number);
static void sv6621_management_put_le64(FAR uint8_t *value, uint64_t number);
static uint16_t sv6621_management_get_le16(FAR const uint8_t *value);
static uint64_t sv6621_management_get_le64(FAR const uint8_t *value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_management_put_le16(FAR uint8_t *value, uint16_t number)
{
  value[0] = number;
  value[1] = number >> 8;
}

static void sv6621_management_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

static void sv6621_management_put_le64(FAR uint8_t *value, uint64_t number)
{
  unsigned int index;

  for (index = 0; index < sizeof(number); index++)
    {
      value[index] = number >> (index * 8);
    }
}

static uint16_t sv6621_management_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static uint64_t sv6621_management_get_le64(FAR const uint8_t *value)
{
  uint64_t number = 0;
  unsigned int index;

  for (index = 0; index < sizeof(number); index++)
    {
      number |= (uint64_t)value[index] << (index * 8);
    }

  return number;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_management_tx(FAR struct sv6621_command_engine_s *command,
                         uint8_t instance, uint32_t wait_ms, uint64_t cookie,
                         uint8_t channel, enum sv6621_band_e band, bool no_ack,
                         FAR const uint8_t *frame, size_t frame_length,
                         size_t total_frame_length)
{
  FAR uint8_t *payload;
  size_t payload_length;
  int ret;

  if (command == NULL || frame == NULL || frame_length == 0 || channel == 0 ||
      band > SV6621_BAND_5GHZ || frame_length > total_frame_length ||
      frame_length > SV6621_MANAGEMENT_MAX_FRAME_SIZE ||
      total_frame_length > UINT16_MAX)
    {
      return -EINVAL;
    }

  payload_length = SV6621_MANAGEMENT_TX_HEADER_SIZE + frame_length;
  payload = kmm_malloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  sv6621_management_put_le32(payload, wait_ms);
  sv6621_management_put_le64(payload + 4, cookie);
  payload[12] = channel;
  payload[13] = band == SV6621_BAND_2GHZ ? 0 : 1;
  payload[14] = no_ack;
  sv6621_management_put_le16(payload + 15, total_frame_length);
  memcpy(payload + SV6621_MANAGEMENT_TX_HEADER_SIZE, frame, frame_length);

  ret = sv6621_command_execute(command, instance, SV6621_MANAGEMENT_COMMAND_TX,
                               payload, payload_length, NULL, NULL,
                               SV6621_MANAGEMENT_COMMAND_TIMEOUT_MS);
  kmm_free(payload);
  return ret == 0 ? 0 : (ret < 0 ? ret : -EREMOTEIO);
}

int sv6621_management_parse_tx_status(
    FAR const uint8_t *payload, size_t payload_length,
    FAR struct sv6621_management_tx_status_s *status)
{
  uint16_t frame_length;

  if (payload == NULL || status == NULL ||
      payload_length < SV6621_MANAGEMENT_TX_STATUS_SIZE)
    {
      return -EINVAL;
    }

  frame_length = sv6621_management_get_le16(payload + 10);
  if (frame_length != payload_length - SV6621_MANAGEMENT_TX_STATUS_SIZE)
    {
      return -EPROTO;
    }

  status->cookie = sv6621_management_get_le64(payload);
  status->acknowledged = sv6621_management_get_le16(payload + 8) != 0;
  status->frame = payload + SV6621_MANAGEMENT_TX_STATUS_SIZE;
  status->frame_length = frame_length;
  return 0;
}
