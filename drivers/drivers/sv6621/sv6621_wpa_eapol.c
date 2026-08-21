/****************************************************************************
 * drivers/drivers/sv6621/sv6621_wpa_eapol.c
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

#include "sv6621_wpa_eapol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_ETHERNET_HEADER_SIZE 14
#define SV6621_WPA_ETHERTYPE_OFFSET     12
#define SV6621_WPA_ETHERTYPE_EAPOL      0x888e
#define SV6621_WPA_EAPOL_HEADER_SIZE    4
#define SV6621_WPA_EAPOL_VERSION_MIN    1
#define SV6621_WPA_EAPOL_VERSION_MAX    2
#define SV6621_WPA_EAPOL_TYPE_KEY       3
#define SV6621_WPA_KEY_DESCRIPTOR_RSN   2
#define SV6621_WPA_CCMP_KEY_SIZE        16
#define SV6621_WPA_KEY_FIXED_SIZE       99
#define SV6621_WPA_KEY_INFO_OFFSET      5
#define SV6621_WPA_KEY_LENGTH_OFFSET    7
#define SV6621_WPA_KEY_REPLAY_OFFSET    9
#define SV6621_WPA_KEY_NONCE_OFFSET     17
#define SV6621_WPA_KEY_IV_OFFSET        49
#define SV6621_WPA_KEY_RSC_OFFSET       65
#define SV6621_WPA_KEY_MIC_OFFSET       81
#define SV6621_WPA_KEY_DATA_LEN_OFFSET  97
#define SV6621_WPA_KEY_DATA_OFFSET      99
#define SV6621_WPA_KEY_VERSION_MASK     0x0007
#define SV6621_WPA_KEY_VERSION_AKM      0x0000
#define SV6621_WPA_KEY_VERSION_SHA1     0x0002
#define SV6621_WPA_KEY_PAIRWISE         0x0008
#define SV6621_WPA_KEY_INSTALL          0x0040
#define SV6621_WPA_KEY_ACK              0x0080
#define SV6621_WPA_KEY_MIC              0x0100
#define SV6621_WPA_KEY_ERROR            0x0400
#define SV6621_WPA_KEY_REQUEST          0x0800
#define SV6621_WPA_KEY_SECURE           0x0200
#define SV6621_WPA_KEY_ENCRYPTED        0x1000
#define SV6621_WPA_EAPOL_MAX_SIZE       512
#define SV6621_WPA_IE_VENDOR            221
#define SV6621_WPA_GTK_KDE_HEADER_SIZE  6
#define SV6621_WPA_IGTK_KDE_HEADER_SIZE 12

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_wpa_rsn_psk_ccmp[] = {
  0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00,
  0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};

static const uint8_t g_sv6621_wpa_rsn_sae_ccmp[] = {
  0x30, 0x1a, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
  0x00, 0x0f, 0xac, 0x04, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x08,
  0xc0, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xac, 0x06
};

static const uint8_t g_sv6621_wpa_gtk_selector[] = { 0x00, 0x0f, 0xac, 0x01 };

static const uint8_t g_sv6621_wpa_igtk_selector[] = { 0x00, 0x0f, 0xac, 0x09 };

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_wpa_eapol_get_be16(FAR const uint8_t *value);
static void sv6621_wpa_eapol_put_be16(FAR uint8_t *output, uint16_t value);
static int sv6621_wpa_eapol_mic(FAR const uint8_t *eapol, size_t length,
                                enum sv6621_wpa_key_mgmt_e key_mgmt,
                                FAR const uint8_t kck[SV6621_WPA_MIC_SIZE],
                                uint8_t mic[SV6621_WPA_MIC_SIZE]);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_wpa_eapol_get_be16(FAR const uint8_t *value)
{
  return ((uint16_t)value[0] << 8) | value[1];
}

/****************************************************************************
 * Name: sv6621_wpa_eapol_put_be16
 ****************************************************************************/

static void sv6621_wpa_eapol_put_be16(FAR uint8_t *output, uint16_t value)
{
  output[0] = value >> 8;
  output[1] = value;
}

/****************************************************************************
 * Name: sv6621_wpa_eapol_mic
 ****************************************************************************/

static int sv6621_wpa_eapol_mic(FAR const uint8_t *eapol, size_t length,
                                enum sv6621_wpa_key_mgmt_e key_mgmt,
                                FAR const uint8_t kck[SV6621_WPA_MIC_SIZE],
                                uint8_t mic[SV6621_WPA_MIC_SIZE])
{
  uint8_t copy[SV6621_WPA_EAPOL_MAX_SIZE];
  uint8_t digest[SV6621_WPA_SHA1_SIZE];
  int ret;

  if (eapol == NULL || kck == NULL || mic == NULL ||
      length < SV6621_WPA_KEY_FIXED_SIZE || length > sizeof(copy))
    {
      return -EINVAL;
    }

  memcpy(copy, eapol, length);
  memset(copy + SV6621_WPA_KEY_MIC_OFFSET, 0, SV6621_WPA_MIC_SIZE);
  if (key_mgmt == SV6621_WPA_KEY_MGMT_SAE)
    {
      ret = sv6621_wpa_aes_cmac(kck, copy, length, mic);
    }
  else
    {
      ret =
          sv6621_wpa_hmac_sha1(kck, SV6621_WPA_MIC_SIZE, copy, length, digest);
      if (ret == 0)
        {
          memcpy(mic, digest, SV6621_WPA_MIC_SIZE);
        }
    }

  memset(copy, 0, sizeof(copy));
  memset(digest, 0, sizeof(digest));
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_eapol_parse
 ****************************************************************************/

int sv6621_wpa_eapol_parse(FAR const uint8_t *frame, size_t frame_length,
                           enum sv6621_wpa_key_mgmt_e key_mgmt,
                           FAR struct sv6621_wpa_eapol_s *eapol)
{
  FAR const uint8_t *packet;
  size_t packet_length;
  size_t key_data_length;
  uint16_t key_info;

  if (frame == NULL || eapol == NULL || key_mgmt > SV6621_WPA_KEY_MGMT_SAE ||
      frame_length <
          SV6621_WPA_ETHERNET_HEADER_SIZE + SV6621_WPA_KEY_FIXED_SIZE ||
      sv6621_wpa_eapol_get_be16(frame + SV6621_WPA_ETHERTYPE_OFFSET) !=
          SV6621_WPA_ETHERTYPE_EAPOL)
    {
      return -EINVAL;
    }

  packet = frame + SV6621_WPA_ETHERNET_HEADER_SIZE;
  packet_length =
      SV6621_WPA_EAPOL_HEADER_SIZE + sv6621_wpa_eapol_get_be16(packet + 2);
  if (packet_length < SV6621_WPA_KEY_FIXED_SIZE ||
      packet_length > frame_length - SV6621_WPA_ETHERNET_HEADER_SIZE ||
      packet[0] < SV6621_WPA_EAPOL_VERSION_MIN ||
      packet[0] > SV6621_WPA_EAPOL_VERSION_MAX ||
      packet[1] != SV6621_WPA_EAPOL_TYPE_KEY ||
      packet[4] != SV6621_WPA_KEY_DESCRIPTOR_RSN)
    {
      return -EPROTO;
    }

  key_info = sv6621_wpa_eapol_get_be16(packet + SV6621_WPA_KEY_INFO_OFFSET);
  if ((key_info & SV6621_WPA_KEY_VERSION_MASK) !=
          (key_mgmt == SV6621_WPA_KEY_MGMT_SAE
               ? SV6621_WPA_KEY_VERSION_AKM
               : SV6621_WPA_KEY_VERSION_SHA1) ||
      (key_info & (SV6621_WPA_KEY_ERROR | SV6621_WPA_KEY_REQUEST)) != 0)
    {
      return -EOPNOTSUPP;
    }

  key_data_length =
      sv6621_wpa_eapol_get_be16(packet + SV6621_WPA_KEY_DATA_LEN_OFFSET);
  if (key_data_length != packet_length - SV6621_WPA_KEY_DATA_OFFSET)
    {
      return -EPROTO;
    }

  memset(eapol, 0, sizeof(*eapol));
  eapol->message = SV6621_WPA_MESSAGE_UNKNOWN;
  if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                   SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL)) ==
      (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK))
    {
      eapol->message = SV6621_WPA_MESSAGE_1;
    }
  else if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL |
                        SV6621_WPA_KEY_SECURE)) ==
           (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_MIC))
    {
      eapol->message = SV6621_WPA_MESSAGE_2;
    }
  else if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL |
                        SV6621_WPA_KEY_SECURE)) ==
           (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_MIC |
            SV6621_WPA_KEY_SECURE))
    {
      eapol->message = SV6621_WPA_MESSAGE_4;
    }
  else if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL)) ==
           (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK | SV6621_WPA_KEY_MIC |
            SV6621_WPA_KEY_INSTALL))
    {
      eapol->message = SV6621_WPA_MESSAGE_3;
    }
  else if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL |
                        SV6621_WPA_KEY_SECURE)) ==
           (SV6621_WPA_KEY_ACK | SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_SECURE))
    {
      eapol->message = SV6621_WPA_MESSAGE_GROUP_1;
    }
  else if ((key_info & (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL |
                        SV6621_WPA_KEY_SECURE)) ==
           (SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_SECURE))
    {
      eapol->message = SV6621_WPA_MESSAGE_GROUP_2;
    }

  eapol->eapol = packet;
  eapol->eapol_length = packet_length;
  eapol->version = packet[0];
  eapol->key_info = key_info;
  eapol->key_length =
      sv6621_wpa_eapol_get_be16(packet + SV6621_WPA_KEY_LENGTH_OFFSET);
  eapol->replay = packet + SV6621_WPA_KEY_REPLAY_OFFSET;
  eapol->nonce = packet + SV6621_WPA_KEY_NONCE_OFFSET;
  eapol->iv = packet + SV6621_WPA_KEY_IV_OFFSET;
  eapol->rsc = packet + SV6621_WPA_KEY_RSC_OFFSET;
  eapol->mic = packet + SV6621_WPA_KEY_MIC_OFFSET;
  eapol->key_data = packet + SV6621_WPA_KEY_DATA_OFFSET;
  eapol->key_data_length = key_data_length;
  return eapol->message == SV6621_WPA_MESSAGE_UNKNOWN ? -EPROTO : 0;
}

/****************************************************************************
 * Name: sv6621_wpa_eapol_build
 ****************************************************************************/

int sv6621_wpa_eapol_build(enum sv6621_wpa_response_e response,
                           enum sv6621_wpa_key_mgmt_e key_mgmt,
                           uint8_t version,
                           FAR const uint8_t replay[SV6621_WPA_REPLAY_SIZE],
                           FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
                           FAR const uint8_t kck[SV6621_WPA_MIC_SIZE],
                           FAR uint8_t *output, size_t capacity,
                           FAR size_t *written)
{
  size_t key_data_length;
  size_t length;
  uint16_t key_info;
  int ret;

  if (replay == NULL || kck == NULL || output == NULL || written == NULL ||
      key_mgmt > SV6621_WPA_KEY_MGMT_SAE ||
      (response != SV6621_WPA_RESPONSE_2 &&
       response != SV6621_WPA_RESPONSE_4 &&
       response != SV6621_WPA_RESPONSE_GROUP_2) ||
      version < SV6621_WPA_EAPOL_VERSION_MIN ||
      version > SV6621_WPA_EAPOL_VERSION_MAX ||
      (response == SV6621_WPA_RESPONSE_2 && snonce == NULL))
    {
      return -EINVAL;
    }

  key_data_length = response != SV6621_WPA_RESPONSE_2 ? 0
                    : key_mgmt == SV6621_WPA_KEY_MGMT_SAE
                        ? sizeof(g_sv6621_wpa_rsn_sae_ccmp)
                        : sizeof(g_sv6621_wpa_rsn_psk_ccmp);
  length = SV6621_WPA_KEY_FIXED_SIZE + key_data_length;
  if (capacity < length)
    {
      return -ENOSPC;
    }

  memset(output, 0, length);
  output[0] = version;
  output[1] = SV6621_WPA_EAPOL_TYPE_KEY;
  sv6621_wpa_eapol_put_be16(output + 2, length - SV6621_WPA_EAPOL_HEADER_SIZE);
  output[4] = SV6621_WPA_KEY_DESCRIPTOR_RSN;
  key_info =
      (key_mgmt == SV6621_WPA_KEY_MGMT_SAE ? SV6621_WPA_KEY_VERSION_AKM
                                           : SV6621_WPA_KEY_VERSION_SHA1) |
      SV6621_WPA_KEY_MIC;
  if (response == SV6621_WPA_RESPONSE_4 ||
      response == SV6621_WPA_RESPONSE_GROUP_2)
    {
      key_info |= SV6621_WPA_KEY_SECURE;
    }

  if (response != SV6621_WPA_RESPONSE_GROUP_2)
    {
      key_info |= SV6621_WPA_KEY_PAIRWISE;
    }

  sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_INFO_OFFSET, key_info);
  sv6621_wpa_eapol_put_be16(
      output + SV6621_WPA_KEY_LENGTH_OFFSET,
      response == SV6621_WPA_RESPONSE_4 ? SV6621_WPA_CCMP_KEY_SIZE : 0);
  memcpy(output + SV6621_WPA_KEY_REPLAY_OFFSET, replay,
         SV6621_WPA_REPLAY_SIZE);
  if (response == SV6621_WPA_RESPONSE_2)
    {
      memcpy(output + SV6621_WPA_KEY_NONCE_OFFSET, snonce,
             SV6621_WPA_NONCE_SIZE);
      sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_DATA_LEN_OFFSET,
                                key_data_length);
      memcpy(output + SV6621_WPA_KEY_DATA_OFFSET,
             key_mgmt == SV6621_WPA_KEY_MGMT_SAE ? g_sv6621_wpa_rsn_sae_ccmp
                                                 : g_sv6621_wpa_rsn_psk_ccmp,
             key_data_length);
    }

  ret = sv6621_wpa_eapol_mic(output, length, key_mgmt, kck,
                             output + SV6621_WPA_KEY_MIC_OFFSET);
  if (ret < 0)
    {
      return ret;
    }

  *written = length;
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_eapol_verify_mic
 ****************************************************************************/

int sv6621_wpa_eapol_verify_mic(FAR const struct sv6621_wpa_eapol_s *eapol,
                                enum sv6621_wpa_key_mgmt_e key_mgmt,
                                FAR const uint8_t kck[SV6621_WPA_MIC_SIZE])
{
  uint8_t expected[SV6621_WPA_MIC_SIZE];
  uint8_t difference = 0;
  size_t index;
  int ret;

  if (eapol == NULL || kck == NULL || eapol->mic == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_wpa_eapol_mic(eapol->eapol, eapol->eapol_length, key_mgmt, kck,
                             expected);
  if (ret < 0)
    {
      return ret;
    }

  for (index = 0; index < sizeof(expected); index++)
    {
      difference |= expected[index] ^ eapol->mic[index];
    }

  memset(expected, 0, sizeof(expected));
  return difference == 0 ? 0 : -EKEYREJECTED;
}

/****************************************************************************
 * Name: sv6621_wpa_eapol_extract_gtk
 ****************************************************************************/

int sv6621_wpa_eapol_extract_gtk(FAR const uint8_t *key_data,
                                 size_t key_data_length,
                                 FAR uint8_t *key_index, FAR uint8_t *gtk,
                                 size_t capacity, FAR size_t *gtk_length)
{
  size_t offset = 0;

  if (key_data == NULL || key_index == NULL || gtk == NULL ||
      gtk_length == NULL)
    {
      return -EINVAL;
    }

  while (offset + 2 <= key_data_length)
    {
      uint8_t id = key_data[offset];
      size_t length = key_data[offset + 1];
      FAR const uint8_t *value = key_data + offset + 2;
      size_t key_length;

      if (length == 0)
        {
          break;
        }

      if (length > key_data_length - offset - 2)
        {
          return -EPROTO;
        }

      if (id == SV6621_WPA_IE_VENDOR &&
          length >= SV6621_WPA_GTK_KDE_HEADER_SIZE &&
          memcmp(value, g_sv6621_wpa_gtk_selector,
                 sizeof(g_sv6621_wpa_gtk_selector)) == 0)
        {
          key_length = length - SV6621_WPA_GTK_KDE_HEADER_SIZE;
          if (key_length == 0 || key_length > SV6621_WPA_GTK_MAX_SIZE ||
              key_length > capacity)
            {
              return -E2BIG;
            }

          *key_index = value[4] & 0x03;
          memcpy(gtk, value + SV6621_WPA_GTK_KDE_HEADER_SIZE, key_length);
          *gtk_length = key_length;
          return 0;
        }

      offset += length + 2;
    }

  return -ENOENT;
}

/****************************************************************************

 * * Name: sv6621_wpa_eapol_build_authenticator

 * ****************************************************************************/

int sv6621_wpa_eapol_build_authenticator(
    enum sv6621_wpa_message_e message, enum sv6621_wpa_key_mgmt_e key_mgmt,
    uint8_t version, FAR const uint8_t replay[SV6621_WPA_REPLAY_SIZE],
    FAR const uint8_t anonce[SV6621_WPA_NONCE_SIZE],
    FAR const uint8_t kck[SV6621_WPA_MIC_SIZE],
    FAR const uint8_t kek[SV6621_WPA_KEK_SIZE], FAR const uint8_t *key_data,
    size_t key_data_length, FAR uint8_t *output, size_t capacity,
    FAR size_t *written)
{
  size_t encoded_key_data_length = 0;
  size_t length;
  uint16_t key_info;
  int ret;

  if (replay == NULL || anonce == NULL || output == NULL || written == NULL ||
      key_mgmt > SV6621_WPA_KEY_MGMT_SAE ||
      (message != SV6621_WPA_MESSAGE_1 && message != SV6621_WPA_MESSAGE_3 &&
       message != SV6621_WPA_MESSAGE_GROUP_1) ||
      version < SV6621_WPA_EAPOL_VERSION_MIN ||
      version > SV6621_WPA_EAPOL_VERSION_MAX ||
      (message == SV6621_WPA_MESSAGE_1 && key_data_length != 0) ||
      ((message == SV6621_WPA_MESSAGE_3 ||
        message == SV6621_WPA_MESSAGE_GROUP_1) &&
       (kck == NULL || kek == NULL || key_data == NULL ||
        key_data_length < 16 || (key_data_length & 7) != 0)))
    {
      return -EINVAL;
    }

  if (message == SV6621_WPA_MESSAGE_3 || message == SV6621_WPA_MESSAGE_GROUP_1)
    {
      encoded_key_data_length = key_data_length + 8;
    }

  length = SV6621_WPA_KEY_FIXED_SIZE + encoded_key_data_length;
  if (capacity < length)
    {
      return -ENOSPC;
    }

  memset(output, 0, length);
  output[0] = version;
  output[1] = SV6621_WPA_EAPOL_TYPE_KEY;
  sv6621_wpa_eapol_put_be16(output + 2, length - SV6621_WPA_EAPOL_HEADER_SIZE);
  output[4] = SV6621_WPA_KEY_DESCRIPTOR_RSN;
  key_info =
      (key_mgmt == SV6621_WPA_KEY_MGMT_SAE ? SV6621_WPA_KEY_VERSION_AKM
                                           : SV6621_WPA_KEY_VERSION_SHA1) |
      SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK;
  if (message == SV6621_WPA_MESSAGE_3 || message == SV6621_WPA_MESSAGE_GROUP_1)
    {
      key_info |= SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_SECURE |
                  SV6621_WPA_KEY_ENCRYPTED;

      if (message == SV6621_WPA_MESSAGE_3)
        {
          key_info |= SV6621_WPA_KEY_INSTALL;
        }
      else
        {
          key_info &= (uint16_t)~SV6621_WPA_KEY_PAIRWISE;
        }
    }

  sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_INFO_OFFSET, key_info);
  sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_LENGTH_OFFSET,
                            SV6621_WPA_CCMP_KEY_SIZE);
  memcpy(output + SV6621_WPA_KEY_REPLAY_OFFSET, replay,
         SV6621_WPA_REPLAY_SIZE);
  memcpy(output + SV6621_WPA_KEY_NONCE_OFFSET, anonce, SV6621_WPA_NONCE_SIZE);
  if (message == SV6621_WPA_MESSAGE_3 || message == SV6621_WPA_MESSAGE_GROUP_1)
    {
      ret = sv6621_wpa_wrap_key(
          kek, key_data, key_data_length, output + SV6621_WPA_KEY_DATA_OFFSET,
          encoded_key_data_length, &encoded_key_data_length);
      if (ret < 0)
        {
          return ret;
        }

      sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_DATA_LEN_OFFSET,
                                encoded_key_data_length);
      ret = sv6621_wpa_eapol_mic(output, length, key_mgmt, kck,
                                 output + SV6621_WPA_KEY_MIC_OFFSET);
      if (ret < 0)
        {
          return ret;
        }
    }

  *written = length;
  return 0;
}

/****************************************************************************

 * * Name: sv6621_wpa_eapol_build_gtk_kde

 * ****************************************************************************/

int sv6621_wpa_eapol_build_gtk_kde(uint8_t key_index, FAR const uint8_t *gtk,
                                   size_t gtk_length, FAR uint8_t *output,
                                   size_t capacity, FAR size_t *written)
{
  size_t length = 2 + SV6621_WPA_GTK_KDE_HEADER_SIZE + gtk_length;

  if (gtk == NULL || output == NULL || written == NULL || key_index > 3 ||
      gtk_length == 0 || gtk_length > SV6621_WPA_GTK_MAX_SIZE ||
      length > UINT8_MAX || (length & 7) != 0)
    {
      return -EINVAL;
    }

  if (capacity < length)
    {
      return -ENOSPC;
    }

  output[0] = SV6621_WPA_IE_VENDOR;
  output[1] = length - 2;
  memcpy(output + 2, g_sv6621_wpa_gtk_selector,
         sizeof(g_sv6621_wpa_gtk_selector));
  output[6] = key_index;
  output[7] = 0;
  memcpy(output + 8, gtk, gtk_length);
  *written = length;
  return 0;
}

/****************************************************************************

 * * Name: sv6621_wpa_eapol_build_igtk_kde

 * ****************************************************************************/

int sv6621_wpa_eapol_build_igtk_kde(
    uint8_t key_index, FAR const uint8_t ipn[SV6621_WPA_IPN_SIZE],
    FAR const uint8_t igtk[SV6621_WPA_IGTK_SIZE], FAR uint8_t *output,
    size_t capacity, FAR size_t *written)
{
  const size_t length =
      2 + SV6621_WPA_IGTK_KDE_HEADER_SIZE + SV6621_WPA_IGTK_SIZE;

  if (key_index < 4 || key_index > 5 || ipn == NULL || igtk == NULL ||
      output == NULL || written == NULL)
    {
      return -EINVAL;
    }

  if (capacity < length)
    {
      return -ENOSPC;
    }

  output[0] = SV6621_WPA_IE_VENDOR;
  output[1] = length - 2;
  memcpy(output + 2, g_sv6621_wpa_igtk_selector,
         sizeof(g_sv6621_wpa_igtk_selector));
  output[6] = key_index;
  output[7] = 0;
  memcpy(output + 8, ipn, SV6621_WPA_IPN_SIZE);
  memcpy(output + 14, igtk, SV6621_WPA_IGTK_SIZE);
  *written = length;
  return 0;
}

/****************************************************************************

 * * Name: sv6621_wpa_eapol_extract_igtk

 * ****************************************************************************/

int sv6621_wpa_eapol_extract_igtk(FAR const uint8_t *key_data,
                                  size_t key_data_length,
                                  FAR uint8_t *key_index,
                                  FAR uint8_t ipn[SV6621_WPA_IPN_SIZE],
                                  FAR uint8_t igtk[SV6621_WPA_IGTK_SIZE])
{
  size_t offset = 0;

  if (key_data == NULL || key_index == NULL || ipn == NULL || igtk == NULL)
    {
      return -EINVAL;
    }

  while (offset < key_data_length)
    {
      FAR const uint8_t *value;
      uint8_t id;
      uint8_t length;

      if (key_data_length - offset < 2)
        {
          return -EPROTO;
        }

      id = key_data[offset];
      length = key_data[offset + 1];
      if ((size_t)length + 2 > key_data_length - offset)
        {
          return -EPROTO;
        }

      value = key_data + offset + 2;
      if (id == SV6621_WPA_IE_VENDOR &&
          length == SV6621_WPA_IGTK_KDE_HEADER_SIZE + SV6621_WPA_IGTK_SIZE &&
          memcmp(value, g_sv6621_wpa_igtk_selector,
                 sizeof(g_sv6621_wpa_igtk_selector)) == 0)
        {
          uint16_t index = value[4] | ((uint16_t)value[5] << 8);

          if (index < 4 || index > 5)
            {
              return -EPROTO;
            }

          *key_index = index;
          memcpy(ipn, value + 6, SV6621_WPA_IPN_SIZE);
          memcpy(igtk, value + SV6621_WPA_IGTK_KDE_HEADER_SIZE,
                 SV6621_WPA_IGTK_SIZE);
          return 0;
        }

      offset += (size_t)length + 2;
    }

  return -ENOENT;
}
