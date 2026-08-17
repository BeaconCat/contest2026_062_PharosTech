/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_filter.c
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

#include "sv6621_filter.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_FILTER_INSTANCE             0
#define SV6621_FILTER_COMMAND_SET_MC_ADDR 26
#define SV6621_FILTER_COMMAND_TIMEOUT_MS 5000
#define SV6621_FILTER_MAX_MULTICAST         32
#define SV6621_FILTER_COUNT_SIZE             2

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_filter_put_le16(FAR uint8_t *output, uint16_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_filter_put_le16
 ****************************************************************************/

static void sv6621_filter_put_le16(FAR uint8_t *output, uint16_t value)
{
  output[0] = value;
  output[1] = value >> 8;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_filter_set_multicast
 ****************************************************************************/

int sv6621_filter_set_multicast(
    FAR struct sv6621_command_engine_s *command,
    FAR const uint8_t (*addresses)[SV6621_MAC_LENGTH], size_t count)
{
  uint8_t payload[SV6621_FILTER_COUNT_SIZE +
                  SV6621_FILTER_MAX_MULTICAST * SV6621_MAC_LENGTH];
  size_t payload_length;

  if (command == NULL || count > SV6621_FILTER_MAX_MULTICAST ||
      (count > 0 && addresses == NULL))
    {
      return -EINVAL;
    }

  payload_length = SV6621_FILTER_COUNT_SIZE + count * SV6621_MAC_LENGTH;
  sv6621_filter_put_le16(payload, count);
  if (count > 0)
    {
      memcpy(payload + SV6621_FILTER_COUNT_SIZE, addresses,
             count * SV6621_MAC_LENGTH);
    }

  return sv6621_command_execute(
      command, SV6621_FILTER_INSTANCE, SV6621_FILTER_COMMAND_SET_MC_ADDR,
      payload, payload_length, NULL, NULL,
      SV6621_FILTER_COMMAND_TIMEOUT_MS);
}
