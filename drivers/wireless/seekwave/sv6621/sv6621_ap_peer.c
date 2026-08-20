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

#define SV6621_AP_COMMAND_ADD_PEER          21
#define SV6621_AP_COMMAND_REMOVE_PEER       22
#define SV6621_AP_REMOVE_PEER_PAYLOAD_SIZE   9
#define SV6621_AP_PEER_RESPONSE_SIZE         1
#define SV6621_AP_PEER_COMMAND_TIMEOUT_MS 1000
#define SV6621_AP_PEER_INDEX_MAX            31

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

  ret = sv6621_command_execute(command, instance,
                               SV6621_AP_COMMAND_ADD_PEER, address,
                               SV6621_MAC_LENGTH, response,
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
  return sv6621_command_execute(command, instance,
                                SV6621_AP_COMMAND_REMOVE_PEER, payload,
                                sizeof(payload), NULL, NULL,
                                SV6621_AP_PEER_COMMAND_TIMEOUT_MS);
}
