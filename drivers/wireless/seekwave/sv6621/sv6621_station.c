/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_station.c
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

#include "sv6621_station.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_STATION_MGMT_HEADER_SIZE       8
#define SV6621_STATION_FRAME_HEADER_SIZE      24
#define SV6621_STATION_FRAME_BSSID_OFFSET     16
#define SV6621_STATION_FRAME_SUBTYPE_MASK     0x00fc
#define SV6621_STATION_FRAME_AUTH             0x00b0
#define SV6621_STATION_FRAME_ASSOC_RESPONSE   0x0010
#define SV6621_STATION_FRAME_REASSOC_RESPONSE 0x0030
#define SV6621_STATION_FRAME_DEAUTH           0x00c0
#define SV6621_STATION_FRAME_DISASSOC         0x00a0
#define SV6621_STATION_AUTH_FRAME_SIZE        30
#define SV6621_STATION_ASSOC_FRAME_SIZE       30
#define SV6621_STATION_REASON_FRAME_SIZE      26
#define SV6621_STATION_DISCONNECT_EVENT_SIZE  8

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_station_get_le16(FAR const uint8_t *value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_station_get_le16
 ****************************************************************************/

static uint16_t sv6621_station_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_station_parse_mgmt
 ****************************************************************************/

int sv6621_station_parse_mgmt(FAR const uint8_t *payload, size_t length,
                              FAR struct sv6621_station_mgmt_s *event)
{
  FAR const uint8_t *frame;
  uint16_t frame_control;
  uint16_t frame_length;

  if (payload == NULL || event == NULL ||
      length < SV6621_STATION_MGMT_HEADER_SIZE +
                   SV6621_STATION_FRAME_HEADER_SIZE ||
      payload[0] == 0 || payload[1] > SV6621_BAND_5GHZ)
    {
      return -EINVAL;
    }

  frame_length = sv6621_station_get_le16(payload + 4);
  if (frame_length < SV6621_STATION_FRAME_HEADER_SIZE ||
      frame_length > length - SV6621_STATION_MGMT_HEADER_SIZE)
    {
      return -EPROTO;
    }

  frame = payload + SV6621_STATION_MGMT_HEADER_SIZE;
  frame_control = sv6621_station_get_le16(frame) &
                  SV6621_STATION_FRAME_SUBTYPE_MASK;
  memset(event, 0, sizeof(*event));
  event->channel = payload[0];
  event->band = payload[1] == 0 ? SV6621_BAND_2GHZ : SV6621_BAND_5GHZ;
  event->signal_dbm = (int16_t)sv6621_station_get_le16(payload + 2);
  memcpy(event->bssid, frame + SV6621_STATION_FRAME_BSSID_OFFSET,
         SV6621_MAC_LENGTH);

  switch (frame_control)
    {
      case SV6621_STATION_FRAME_AUTH:
        if (frame_length < SV6621_STATION_AUTH_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_STATION_MGMT_AUTH;
        event->algorithm = sv6621_station_get_le16(frame + 24);
        event->transaction = sv6621_station_get_le16(frame + 26);
        event->status = sv6621_station_get_le16(frame + 28);
        break;

      case SV6621_STATION_FRAME_ASSOC_RESPONSE:
      case SV6621_STATION_FRAME_REASSOC_RESPONSE:
        if (frame_length < SV6621_STATION_ASSOC_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_STATION_MGMT_ASSOC;
        event->status = sv6621_station_get_le16(frame + 26);
        break;

      case SV6621_STATION_FRAME_DEAUTH:
      case SV6621_STATION_FRAME_DISASSOC:
        if (frame_length < SV6621_STATION_REASON_FRAME_SIZE)
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_STATION_FRAME_DEAUTH
                          ? SV6621_STATION_MGMT_DEAUTH
                          : SV6621_STATION_MGMT_DISASSOC;
        event->reason = sv6621_station_get_le16(frame + 24);
        break;

      default:
        return -ENOMSG;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_station_parse_disconnect
 ****************************************************************************/

int sv6621_station_parse_disconnect(
    FAR const uint8_t *payload, size_t length,
    uint8_t bssid[SV6621_MAC_LENGTH], FAR uint16_t *reason)
{
  if (payload == NULL || bssid == NULL || reason == NULL ||
      length != SV6621_STATION_DISCONNECT_EVENT_SIZE)
    {
      return -EINVAL;
    }

  *reason = sv6621_station_get_le16(payload);
  memcpy(bssid, payload + 2, SV6621_MAC_LENGTH);
  return 0;
}
