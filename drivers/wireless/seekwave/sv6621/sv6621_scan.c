/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_scan.c
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

#include "sv6621_scan.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SCAN_INSTANCE             0
#define SV6621_SCAN_COMMAND_START        5
#define SV6621_SCAN_COMMAND_STOP         6
#define SV6621_SCAN_FIXED_SIZE           32
#define SV6621_SCAN_CHANNEL_SIZE         3
#define SV6621_SCAN_MAX_CHANNELS         64
#define SV6621_SCAN_COMMAND_TIMEOUT_MS   5000

#define SV6621_SCAN_CHANNEL_COUNT_OFFSET 8
#define SV6621_SCAN_CHANNEL_LIST_OFFSET  12
#define SV6621_SCAN_SSID_COUNT_OFFSET    16
#define SV6621_SCAN_SSID_LIST_OFFSET     20
#define SV6621_SCAN_IE_LENGTH_OFFSET     24
#define SV6621_SCAN_IE_OFFSET_OFFSET     28

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_scan_put_le32(FAR uint8_t *value, uint32_t number);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_scan_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_scan_start(FAR struct sv6621_command_engine_s *command,
                      FAR const struct sv6621_scan_channel_s *channels,
                      size_t channel_count)
{
  FAR uint8_t *payload;
  size_t payload_length;
  size_t index;
  int ret;

  if (command == NULL || channels == NULL || channel_count == 0 ||
      channel_count > SV6621_SCAN_MAX_CHANNELS)
    {
      return -EINVAL;
    }

  payload_length =
      SV6621_SCAN_FIXED_SIZE + channel_count * SV6621_SCAN_CHANNEL_SIZE;
  payload = kmm_zalloc(payload_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_COUNT_OFFSET,
                       channel_count);
  sv6621_scan_put_le32(payload + SV6621_SCAN_CHANNEL_LIST_OFFSET,
                       SV6621_SCAN_FIXED_SIZE);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_COUNT_OFFSET, 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_SSID_LIST_OFFSET, 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_IE_LENGTH_OFFSET, 0);
  sv6621_scan_put_le32(payload + SV6621_SCAN_IE_OFFSET_OFFSET, 0);

  for (index = 0; index < channel_count; index++)
    {
      FAR uint8_t *encoded =
          payload + SV6621_SCAN_FIXED_SIZE + index * SV6621_SCAN_CHANNEL_SIZE;

      if (channels[index].number == 0 ||
          channels[index].band > SV6621_SCAN_BAND_5GHZ)
        {
          ret = -EINVAL;
          goto free_payload;
        }

      encoded[0] = channels[index].number;
      encoded[1] = channels[index].band;
      encoded[2] = channels[index].flags;
    }

  ret = sv6621_command_execute(
      command, SV6621_SCAN_INSTANCE, SV6621_SCAN_COMMAND_START, payload,
      payload_length, NULL, NULL, SV6621_SCAN_COMMAND_TIMEOUT_MS);
  if (ret > 0)
    {
      ret = -EREMOTEIO;
    }

free_payload:
  kmm_free(payload);
  return ret;
}

int sv6621_scan_stop(FAR struct sv6621_command_engine_s *command)
{
  int ret;

  if (command == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_command_execute(command, SV6621_SCAN_INSTANCE,
                               SV6621_SCAN_COMMAND_STOP, NULL, 0, NULL, NULL,
                               SV6621_SCAN_COMMAND_TIMEOUT_MS);
  return ret > 0 ? -EREMOTEIO : ret;
}
