/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_eapol.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_EAPOL_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_EAPOL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621_wpa_crypto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_REPLAY_SIZE 8
#define SV6621_WPA_MIC_SIZE    16
#define SV6621_WPA_GTK_MAX_SIZE 32
#define SV6621_WPA_IGTK_SIZE    16
#define SV6621_WPA_IPN_SIZE      6

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_wpa_message_e
{
  SV6621_WPA_MESSAGE_UNKNOWN = 0,
  SV6621_WPA_MESSAGE_1,
  SV6621_WPA_MESSAGE_3,
  SV6621_WPA_MESSAGE_GROUP_1
};

enum sv6621_wpa_response_e
{
  SV6621_WPA_RESPONSE_2 = 2,
  SV6621_WPA_RESPONSE_4 = 4,
  SV6621_WPA_RESPONSE_GROUP_2 = 6
};

enum sv6621_wpa_key_mgmt_e
{
  SV6621_WPA_KEY_MGMT_PSK = 0,
  SV6621_WPA_KEY_MGMT_SAE
};

struct sv6621_wpa_eapol_s
{
  enum sv6621_wpa_message_e message;
  FAR const uint8_t *eapol;
  size_t eapol_length;
  uint8_t version;
  uint16_t key_info;
  uint16_t key_length;
  FAR const uint8_t *replay;
  FAR const uint8_t *nonce;
  FAR const uint8_t *iv;
  FAR const uint8_t *rsc;
  FAR const uint8_t *mic;
  FAR const uint8_t *key_data;
  size_t key_data_length;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_wpa_eapol_parse(
    FAR const uint8_t *frame, size_t frame_length,
    enum sv6621_wpa_key_mgmt_e key_mgmt,
    FAR struct sv6621_wpa_eapol_s *eapol);
int sv6621_wpa_eapol_build(
    enum sv6621_wpa_response_e response,
    enum sv6621_wpa_key_mgmt_e key_mgmt, uint8_t version,
    FAR const uint8_t replay[SV6621_WPA_REPLAY_SIZE],
    FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
    FAR const uint8_t kck[SV6621_WPA_MIC_SIZE], FAR uint8_t *output,
    size_t capacity, FAR size_t *written);
int sv6621_wpa_eapol_verify_mic(
    FAR const struct sv6621_wpa_eapol_s *eapol,
    enum sv6621_wpa_key_mgmt_e key_mgmt,
    FAR const uint8_t kck[SV6621_WPA_MIC_SIZE]);
int sv6621_wpa_eapol_extract_gtk(
    FAR const uint8_t *key_data, size_t key_data_length,
    FAR uint8_t *key_index, FAR uint8_t *gtk, size_t capacity,
    FAR size_t *gtk_length);
int sv6621_wpa_eapol_extract_igtk(
    FAR const uint8_t *key_data, size_t key_data_length,
    FAR uint8_t *key_index, FAR uint8_t ipn[SV6621_WPA_IPN_SIZE],
    FAR uint8_t igtk[SV6621_WPA_IGTK_SIZE]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_EAPOL_H */
