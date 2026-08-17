/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_eapol.c
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
#define SV6621_WPA_EAPOL_TYPE_KEY       3
#define SV6621_WPA_KEY_DESCRIPTOR_RSN   2
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
#define SV6621_WPA_KEY_VERSION_SHA1     0x0002
#define SV6621_WPA_KEY_PAIRWISE         0x0008
#define SV6621_WPA_KEY_INSTALL          0x0040
#define SV6621_WPA_KEY_ACK              0x0080
#define SV6621_WPA_KEY_MIC              0x0100
#define SV6621_WPA_KEY_ERROR            0x0400
#define SV6621_WPA_KEY_REQUEST          0x0800
#define SV6621_WPA_KEY_SECURE           0x0200
#define SV6621_WPA_EAPOL_MAX_SIZE       512

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_wpa_rsn_psk_ccmp[] = {
  0x30, 0x14, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, 0x01, 0x00,
  0x00, 0x0f, 0xac, 0x02, 0x00, 0x00
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_wpa_eapol_get_be16(FAR const uint8_t *value);
static void sv6621_wpa_eapol_put_be16(FAR uint8_t *output, uint16_t value);
static int sv6621_wpa_eapol_mic(
    FAR const uint8_t *eapol, size_t length,
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

static int sv6621_wpa_eapol_mic(
    FAR const uint8_t *eapol, size_t length,
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
  ret = sv6621_wpa_hmac_sha1(kck, SV6621_WPA_MIC_SIZE, copy, length,
                              digest);
  if (ret == 0)
    {
      memcpy(mic, digest, SV6621_WPA_MIC_SIZE);
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
                            FAR struct sv6621_wpa_eapol_s *eapol)
{
  FAR const uint8_t *packet;
  size_t packet_length;
  size_t key_data_length;
  uint16_t key_info;

  if (frame == NULL || eapol == NULL ||
      frame_length < SV6621_WPA_ETHERNET_HEADER_SIZE +
                         SV6621_WPA_KEY_FIXED_SIZE ||
      sv6621_wpa_eapol_get_be16(frame + SV6621_WPA_ETHERTYPE_OFFSET) !=
          SV6621_WPA_ETHERTYPE_EAPOL)
    {
      return -EINVAL;
    }

  packet = frame + SV6621_WPA_ETHERNET_HEADER_SIZE;
  packet_length = SV6621_WPA_EAPOL_HEADER_SIZE +
                  sv6621_wpa_eapol_get_be16(packet + 2);
  if (packet_length < SV6621_WPA_KEY_FIXED_SIZE ||
      packet_length > frame_length - SV6621_WPA_ETHERNET_HEADER_SIZE ||
      packet[1] != SV6621_WPA_EAPOL_TYPE_KEY ||
      packet[4] != SV6621_WPA_KEY_DESCRIPTOR_RSN)
    {
      return -EPROTO;
    }

  key_info = sv6621_wpa_eapol_get_be16(
      packet + SV6621_WPA_KEY_INFO_OFFSET);
  if ((key_info & SV6621_WPA_KEY_VERSION_MASK) !=
          SV6621_WPA_KEY_VERSION_SHA1 ||
      (key_info & (SV6621_WPA_KEY_ERROR | SV6621_WPA_KEY_REQUEST)) != 0)
    {
      return -EOPNOTSUPP;
    }

  key_data_length = sv6621_wpa_eapol_get_be16(
      packet + SV6621_WPA_KEY_DATA_LEN_OFFSET);
  if (key_data_length > packet_length - SV6621_WPA_KEY_DATA_OFFSET)
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
                        SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL)) ==
           (SV6621_WPA_KEY_PAIRWISE | SV6621_WPA_KEY_ACK |
            SV6621_WPA_KEY_MIC | SV6621_WPA_KEY_INSTALL))
    {
      eapol->message = SV6621_WPA_MESSAGE_3;
    }

  eapol->eapol = packet;
  eapol->eapol_length = packet_length;
  eapol->key_info = key_info;
  eapol->key_length = sv6621_wpa_eapol_get_be16(
      packet + SV6621_WPA_KEY_LENGTH_OFFSET);
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

int sv6621_wpa_eapol_build(
    enum sv6621_wpa_response_e response,
    FAR const uint8_t replay[SV6621_WPA_REPLAY_SIZE],
    FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
    FAR const uint8_t kck[SV6621_WPA_MIC_SIZE], FAR uint8_t *output,
    size_t capacity, FAR size_t *written)
{
  size_t key_data_length;
  size_t length;
  uint16_t key_info;
  int ret;

  if (replay == NULL || kck == NULL || output == NULL || written == NULL ||
      (response != SV6621_WPA_RESPONSE_2 &&
       response != SV6621_WPA_RESPONSE_4) ||
      (response == SV6621_WPA_RESPONSE_2 && snonce == NULL))
    {
      return -EINVAL;
    }

  key_data_length = response == SV6621_WPA_RESPONSE_2 ?
                    sizeof(g_sv6621_wpa_rsn_psk_ccmp) : 0;
  length = SV6621_WPA_KEY_FIXED_SIZE + key_data_length;
  if (capacity < length)
    {
      return -ENOSPC;
    }

  memset(output, 0, length);
  output[0] = response == SV6621_WPA_RESPONSE_2 ? 1 : 2;
  output[1] = SV6621_WPA_EAPOL_TYPE_KEY;
  sv6621_wpa_eapol_put_be16(output + 2,
                             length - SV6621_WPA_EAPOL_HEADER_SIZE);
  output[4] = SV6621_WPA_KEY_DESCRIPTOR_RSN;
  key_info = SV6621_WPA_KEY_VERSION_SHA1 | SV6621_WPA_KEY_PAIRWISE |
             SV6621_WPA_KEY_MIC;
  if (response == SV6621_WPA_RESPONSE_4)
    {
      key_info |= SV6621_WPA_KEY_SECURE;
    }

  sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_INFO_OFFSET, key_info);
  sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_LENGTH_OFFSET,
                             response == SV6621_WPA_RESPONSE_4 ? 16 : 0);
  memcpy(output + SV6621_WPA_KEY_REPLAY_OFFSET, replay,
         SV6621_WPA_REPLAY_SIZE);
  if (response == SV6621_WPA_RESPONSE_2)
    {
      memcpy(output + SV6621_WPA_KEY_NONCE_OFFSET, snonce,
             SV6621_WPA_NONCE_SIZE);
      sv6621_wpa_eapol_put_be16(output + SV6621_WPA_KEY_DATA_LEN_OFFSET,
                                 key_data_length);
      memcpy(output + SV6621_WPA_KEY_DATA_OFFSET,
             g_sv6621_wpa_rsn_psk_ccmp, key_data_length);
    }

  ret = sv6621_wpa_eapol_mic(output, length, kck,
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

int sv6621_wpa_eapol_verify_mic(
    FAR const struct sv6621_wpa_eapol_s *eapol,
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

  ret = sv6621_wpa_eapol_mic(eapol->eapol, eapol->eapol_length, kck,
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
