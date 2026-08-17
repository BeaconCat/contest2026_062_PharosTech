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

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_wpa_eapol_get_be16(FAR const uint8_t *value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_wpa_eapol_get_be16(FAR const uint8_t *value)
{
  return ((uint16_t)value[0] << 8) | value[1];
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
