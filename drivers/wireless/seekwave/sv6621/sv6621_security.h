/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_security.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SECURITY_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SECURITY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "include/sv6621.h"
#include "sv6621_command.h"
#include "sv6621_data.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_security_key_type_e
{
  SV6621_SECURITY_KEY_PAIRWISE = 0,
  SV6621_SECURITY_KEY_GROUP = 1,
  SV6621_SECURITY_KEY_INTEGRITY_GROUP = 2,
  SV6621_SECURITY_KEY_BEACON_INTEGRITY_GROUP = 3
};

enum sv6621_security_cipher_e
{
  SV6621_SECURITY_CIPHER_CCMP = 8,
  SV6621_SECURITY_CIPHER_BIP_CMAC_128 = 12,
  SV6621_SECURITY_CIPHER_BIP_CMAC_256 = 13,
  SV6621_SECURITY_CIPHER_BIP_GMAC_128 = 14,
  SV6621_SECURITY_CIPHER_BIP_GMAC_256 = 15
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_security_add_key(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[6]);
int sv6621_security_add_key_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[6]);
int sv6621_security_delete_key(
    FAR struct sv6621_command_engine_s *command,
    enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index);
int sv6621_security_delete_key_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher,
    FAR const uint8_t address[SV6621_MAC_LENGTH], uint8_t key_index);
int sv6621_security_send_eapol(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length);
int sv6621_security_send_eapol_instance(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SECURITY_H */
