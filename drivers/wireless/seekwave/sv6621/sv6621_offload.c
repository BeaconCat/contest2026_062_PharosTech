/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_offload.c
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

#include "sv6621_offload.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_OFFLOAD_COMMAND_SET_IP    16
#define SV6621_OFFLOAD_ENTRY_SIZE        17
#define SV6621_OFFLOAD_TYPE_IPV4         0
#define SV6621_OFFLOAD_TYPE_IPV6         1
#define SV6621_OFFLOAD_COMMAND_TIMEOUT_MS 2000
#define SV6621_OFFLOAD_ENTRY_LIMIT       (SV6621_OFFLOAD_IPV6_LIMIT + 1)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_offload_set_addresses
 ****************************************************************************/

int sv6621_offload_set_addresses(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const struct sv6621_offload_addresses_s *addresses)
{
  uint8_t payload[SV6621_OFFLOAD_ENTRY_LIMIT * SV6621_OFFLOAD_ENTRY_SIZE];
  size_t count = 0;
  size_t index;

  if (command == NULL || addresses == NULL || instance > 0x0f ||
      addresses->ipv6_count > SV6621_OFFLOAD_IPV6_LIMIT)
    {
      return -EINVAL;
    }

  memset(payload, 0, sizeof(payload));
  if (addresses->ipv4_valid)
    {
      payload[0] = SV6621_OFFLOAD_TYPE_IPV4;
      memcpy(payload + 1, addresses->ipv4, SV6621_OFFLOAD_IPV4_LENGTH);
      count++;
    }

  for (index = 0; index < addresses->ipv6_count; index++)
    {
      size_t offset = count * SV6621_OFFLOAD_ENTRY_SIZE;

      payload[offset] = SV6621_OFFLOAD_TYPE_IPV6;
      memcpy(payload + offset + 1, addresses->ipv6[index],
             SV6621_OFFLOAD_IPV6_LENGTH);
      count++;
    }

  return sv6621_command_execute(command, instance,
                                SV6621_OFFLOAD_COMMAND_SET_IP, payload,
                                count * SV6621_OFFLOAD_ENTRY_SIZE, NULL, NULL,
                                SV6621_OFFLOAD_COMMAND_TIMEOUT_MS);
}
