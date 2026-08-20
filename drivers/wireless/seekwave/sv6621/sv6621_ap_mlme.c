/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_mlme.c
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

#include "sv6621_ap_mlme.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_MGMT_EVENT_HEADER_SIZE    8
#define SV6621_AP_MGMT_FRAME_HEADER_SIZE   24
#define SV6621_AP_MGMT_DESTINATION_OFFSET   4
#define SV6621_AP_MGMT_SOURCE_OFFSET       10
#define SV6621_AP_MGMT_BSSID_OFFSET        16
#define SV6621_AP_MGMT_SUBTYPE_MASK    0x00fc
#define SV6621_AP_FRAME_ASSOC_REQUEST   0x0000
#define SV6621_AP_FRAME_REASSOC_REQUEST 0x0020
#define SV6621_AP_FRAME_PROBE_REQUEST   0x0040
#define SV6621_AP_FRAME_DISASSOC        0x00a0
#define SV6621_AP_FRAME_AUTH            0x00b0
#define SV6621_AP_FRAME_DEAUTH          0x00c0
#define SV6621_AP_FRAME_ACTION          0x00d0
#define SV6621_AP_MGMT_ASSOC_FIXED_SIZE    28
#define SV6621_AP_MGMT_REASSOC_FIXED_SIZE  34
#define SV6621_AP_MGMT_AUTH_FIXED_SIZE     30
#define SV6621_AP_MGMT_REASON_FIXED_SIZE   26

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_ap_mlme_get_le16(FAR const uint8_t *value);
static void sv6621_ap_mlme_set_ies(FAR struct sv6621_ap_mgmt_s *event,
                                  size_t offset);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_ap_mlme_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static void sv6621_ap_mlme_set_ies(FAR struct sv6621_ap_mgmt_s *event,
                                  size_t offset)
{
  event->information_elements = event->frame + offset;
  event->information_element_length = event->frame_length - offset;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_parse_mgmt(FAR const uint8_t *payload, size_t payload_length,
                         FAR struct sv6621_ap_mgmt_s *event)
{
  FAR const uint8_t *frame;
  uint16_t frame_control;
  uint16_t frame_length;

  if (payload == NULL || event == NULL ||
      payload_length < SV6621_AP_MGMT_EVENT_HEADER_SIZE +
                       SV6621_AP_MGMT_FRAME_HEADER_SIZE ||
      payload[0] == 0 || payload[1] > SV6621_BAND_5GHZ)
    {
      return -EINVAL;
    }

  frame_length = sv6621_ap_mlme_get_le16(payload + 4);
  if (frame_length < SV6621_AP_MGMT_FRAME_HEADER_SIZE ||
      frame_length > payload_length - SV6621_AP_MGMT_EVENT_HEADER_SIZE)
    {
      return -EPROTO;
    }

  frame = payload + SV6621_AP_MGMT_EVENT_HEADER_SIZE;
  frame_control = sv6621_ap_mlme_get_le16(frame) &
                  SV6621_AP_MGMT_SUBTYPE_MASK;
  memset(event, 0, sizeof(*event));
  event->channel = payload[0];
  event->band = payload[1] == 0 ? SV6621_BAND_2GHZ : SV6621_BAND_5GHZ;
  event->signal_dbm = (int16_t)sv6621_ap_mlme_get_le16(payload + 2);
  memcpy(event->destination, frame + SV6621_AP_MGMT_DESTINATION_OFFSET,
         SV6621_MAC_LENGTH);
  memcpy(event->source, frame + SV6621_AP_MGMT_SOURCE_OFFSET,
         SV6621_MAC_LENGTH);
  memcpy(event->bssid, frame + SV6621_AP_MGMT_BSSID_OFFSET,
         SV6621_MAC_LENGTH);
  event->frame = frame;
  event->frame_length = frame_length;

  switch (frame_control)
    {
      case SV6621_AP_FRAME_AUTH:
        if (frame_length < SV6621_AP_MGMT_AUTH_FIXED_SIZE)
          {
            return -EPROTO;
          }

        event->type = SV6621_AP_MGMT_AUTH;
        event->algorithm = sv6621_ap_mlme_get_le16(frame + 24);
        event->transaction = sv6621_ap_mlme_get_le16(frame + 26);
        event->status = sv6621_ap_mlme_get_le16(frame + 28);
        sv6621_ap_mlme_set_ies(event, SV6621_AP_MGMT_AUTH_FIXED_SIZE);
        break;

      case SV6621_AP_FRAME_ASSOC_REQUEST:
      case SV6621_AP_FRAME_REASSOC_REQUEST:
        if (frame_length < (frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                                ? SV6621_AP_MGMT_ASSOC_FIXED_SIZE
                                : SV6621_AP_MGMT_REASSOC_FIXED_SIZE))
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                          ? SV6621_AP_MGMT_ASSOC_REQUEST
                          : SV6621_AP_MGMT_REASSOC_REQUEST;
        event->capability = sv6621_ap_mlme_get_le16(frame + 24);
        event->listen_interval = sv6621_ap_mlme_get_le16(frame + 26);
        sv6621_ap_mlme_set_ies(
            event, frame_control == SV6621_AP_FRAME_ASSOC_REQUEST
                       ? SV6621_AP_MGMT_ASSOC_FIXED_SIZE
                       : SV6621_AP_MGMT_REASSOC_FIXED_SIZE);
        break;

      case SV6621_AP_FRAME_PROBE_REQUEST:
        event->type = SV6621_AP_MGMT_PROBE_REQUEST;
        sv6621_ap_mlme_set_ies(event, SV6621_AP_MGMT_FRAME_HEADER_SIZE);
        break;

      case SV6621_AP_FRAME_DEAUTH:
      case SV6621_AP_FRAME_DISASSOC:
        if (frame_length < SV6621_AP_MGMT_REASON_FIXED_SIZE)
          {
            return -EPROTO;
          }

        event->type = frame_control == SV6621_AP_FRAME_DEAUTH
                          ? SV6621_AP_MGMT_DEAUTH
                          : SV6621_AP_MGMT_DISASSOC;
        event->reason = sv6621_ap_mlme_get_le16(frame + 24);
        break;

      case SV6621_AP_FRAME_ACTION:
        event->type = SV6621_AP_MGMT_ACTION;
        break;

      default:
        return -ENOMSG;
    }

  return 0;
}
