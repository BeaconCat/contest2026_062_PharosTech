/****************************************************************************
 * drivers/drivers/sv6621/sv6621_security.c
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

#include "sv6621_security.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SECURITY_INSTANCE           0
#define SV6621_SECURITY_COMMAND_ADD_KEY    12
#define SV6621_SECURITY_COMMAND_DEL_KEY    13
#define SV6621_SECURITY_COMMAND_TX_DATA    15
#define SV6621_SECURITY_COMMAND_TIMEOUT_MS 5000
#define SV6621_SECURITY_PACKET_NUMBER_SIZE 6
#define SV6621_SECURITY_CCMP_KEY_SIZE      16
#define SV6621_SECURITY_BIP_KEY_SIZE       16
#define SV6621_SECURITY_BIP_256_KEY_SIZE   32
#define SV6621_SECURITY_KEY_PAYLOAD_SIZE   48
#define SV6621_SECURITY_KEY_TYPE_OFFSET    6
#define SV6621_SECURITY_CIPHER_OFFSET      7
#define SV6621_SECURITY_PN_OFFSET          8
#define SV6621_SECURITY_KEY_INDEX_OFFSET   14
#define SV6621_SECURITY_KEY_LENGTH_OFFSET  15
#define SV6621_SECURITY_KEY_OFFSET         16
#define SV6621_SECURITY_ETHERTYPE_EAPOL    0x888e

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_security_add_key
 ****************************************************************************/

int sv6621_security_add_key(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_security_key_type_e type, enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[SV6621_SECURITY_PACKET_NUMBER_SIZE])
{
  return sv6621_security_add_key_instance(command, SV6621_SECURITY_INSTANCE,
                                          type, cipher, address, key_index,
                                          key, key_length, packet_number);
}

/****************************************************************************

 * * Name: sv6621_security_add_key_instance

 * ****************************************************************************/

int sv6621_security_add_key_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    enum sv6621_security_key_type_e type, enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[SV6621_SECURITY_PACKET_NUMBER_SIZE])
{
  uint8_t payload[SV6621_SECURITY_KEY_PAYLOAD_SIZE];
  bool valid_key;

  valid_key = cipher == SV6621_SECURITY_CIPHER_CCMP
                  ? key_length == SV6621_SECURITY_CCMP_KEY_SIZE
              : cipher == SV6621_SECURITY_CIPHER_BIP_CMAC_128 ||
                      cipher == SV6621_SECURITY_CIPHER_BIP_GMAC_128
                  ? key_length == SV6621_SECURITY_BIP_KEY_SIZE
              : cipher == SV6621_SECURITY_CIPHER_BIP_CMAC_256 ||
                      cipher == SV6621_SECURITY_CIPHER_BIP_GMAC_256
                  ? key_length == SV6621_SECURITY_BIP_256_KEY_SIZE
                  : false;

  if (command == NULL || address == NULL || key == NULL || !valid_key ||
      key_index > 6 || type > SV6621_SECURITY_KEY_BEACON_INTEGRITY_GROUP ||
      (type <= SV6621_SECURITY_KEY_GROUP &&
       cipher != SV6621_SECURITY_CIPHER_CCMP) ||
      (type >= SV6621_SECURITY_KEY_INTEGRITY_GROUP &&
       cipher == SV6621_SECURITY_CIPHER_CCMP))
    {
      return -EINVAL;
    }

  memset(payload, 0, sizeof(payload));
  memcpy(payload, address, SV6621_MAC_LENGTH);
  payload[SV6621_SECURITY_KEY_TYPE_OFFSET] = type;
  payload[SV6621_SECURITY_CIPHER_OFFSET] = cipher;
  payload[SV6621_SECURITY_PN_OFFSET] = 1;
  if (packet_number != NULL)
    {
      memcpy(payload + SV6621_SECURITY_PN_OFFSET, packet_number,
             SV6621_SECURITY_PACKET_NUMBER_SIZE);
    }

  payload[SV6621_SECURITY_KEY_INDEX_OFFSET] = key_index;
  payload[SV6621_SECURITY_KEY_LENGTH_OFFSET] = key_length;
  memcpy(payload + SV6621_SECURITY_KEY_OFFSET, key, key_length);

  return sv6621_command_execute(
      command, instance, SV6621_SECURITY_COMMAND_ADD_KEY, payload,
      sizeof(payload), NULL, NULL, SV6621_SECURITY_COMMAND_TIMEOUT_MS);
}

/****************************************************************************
 * Name: sv6621_security_delete_key
 ****************************************************************************/

int sv6621_security_delete_key(FAR struct sv6621_command_engine_s *command,
                               enum sv6621_security_key_type_e type,
                               enum sv6621_security_cipher_e cipher,
                               FAR const uint8_t address[SV6621_MAC_LENGTH],
                               uint8_t key_index)
{
  return sv6621_security_delete_key_instance(command, SV6621_SECURITY_INSTANCE,
                                             type, cipher, address, key_index);
}

/****************************************************************************

 * * Name: sv6621_security_delete_key_instance

 * ****************************************************************************/

int sv6621_security_delete_key_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    enum sv6621_security_key_type_e type, enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index)
{
  uint8_t payload[SV6621_SECURITY_KEY_PAYLOAD_SIZE];

  if (command == NULL || address == NULL || key_index > 6 ||
      type > SV6621_SECURITY_KEY_BEACON_INTEGRITY_GROUP ||
      cipher < SV6621_SECURITY_CIPHER_CCMP ||
      cipher > SV6621_SECURITY_CIPHER_BIP_GMAC_256 ||
      (cipher > SV6621_SECURITY_CIPHER_CCMP &&
       cipher < SV6621_SECURITY_CIPHER_BIP_CMAC_128) ||
      (type <= SV6621_SECURITY_KEY_GROUP &&
       cipher != SV6621_SECURITY_CIPHER_CCMP) ||
      (type >= SV6621_SECURITY_KEY_INTEGRITY_GROUP &&
       cipher == SV6621_SECURITY_CIPHER_CCMP))
    {
      return -EINVAL;
    }

  memset(payload, 0, sizeof(payload));
  memcpy(payload, address, SV6621_MAC_LENGTH);
  payload[SV6621_SECURITY_KEY_TYPE_OFFSET] = type;
  payload[SV6621_SECURITY_CIPHER_OFFSET] = cipher;
  payload[SV6621_SECURITY_KEY_INDEX_OFFSET] = key_index;

  return sv6621_command_execute(
      command, instance, SV6621_SECURITY_COMMAND_DEL_KEY, payload,
      sizeof(payload), NULL, NULL, SV6621_SECURITY_COMMAND_TIMEOUT_MS);
}

/****************************************************************************
 * Name: sv6621_security_send_eapol
 ****************************************************************************/

int sv6621_security_send_eapol(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length)
{
  return sv6621_security_send_eapol_instance(command, SV6621_SECURITY_INSTANCE,
                                             context, frame, frame_length);
}

/****************************************************************************

 * * Name: sv6621_security_send_eapol_instance

 * ****************************************************************************/

int sv6621_security_send_eapol_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length)
{
  FAR uint8_t *payload;
  size_t payload_length;
  int ret;

  if (command == NULL || context == NULL || frame == NULL ||
      frame_length < 14 || frame_length > SV6621_DATA_MAX_FRAME_SIZE ||
      (((uint16_t)frame[12] << 8) | frame[13]) !=
          SV6621_SECURITY_ETHERTYPE_EAPOL)
    {
      return -EINVAL;
    }

  payload = kmm_malloc(SV6621_DATA_TX_DESCRIPTOR_SIZE + frame_length);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  ret = sv6621_data_encode_tx(context, frame, frame_length, payload,
                              SV6621_DATA_TX_DESCRIPTOR_SIZE + frame_length,
                              &payload_length);
  if (ret == 0)
    {
      ret = sv6621_command_execute(
          command, instance, SV6621_SECURITY_COMMAND_TX_DATA, payload,
          payload_length, NULL, NULL, SV6621_SECURITY_COMMAND_TIMEOUT_MS);
    }

  kmm_free(payload);
  return ret;
}
