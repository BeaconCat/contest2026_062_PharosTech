/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_frame.c
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

#include "sv6621_sae_frame.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_FRAME_CONTROL_AUTH 0x00b0
#define SV6621_SAE_FRAME_HEADER_SIZE  24
#define SV6621_SAE_AUTH_FIXED_SIZE    6
#define SV6621_SAE_AUTH_FRAME_SIZE \
  (SV6621_SAE_FRAME_HEADER_SIZE + SV6621_SAE_AUTH_FIXED_SIZE)
#define SV6621_SAE_AUTH_ALGORITHM     3

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_sae_frame_get_le16(FAR const uint8_t *value);
static void sv6621_sae_frame_put_le16(FAR uint8_t *value, uint16_t number);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_sae_frame_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static void sv6621_sae_frame_put_le16(FAR uint8_t *value, uint16_t number)
{
  value[0] = number;
  value[1] = number >> 8;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sae_auth_parse(FAR const uint8_t *frame, size_t frame_length,
                          FAR struct sv6621_sae_auth_frame_s *auth)
{
  if (frame == NULL || auth == NULL ||
      frame_length < SV6621_SAE_AUTH_FRAME_SIZE)
    {
      return -EINVAL;
    }

  if ((sv6621_sae_frame_get_le16(frame) & 0x00fc) !=
          SV6621_SAE_FRAME_CONTROL_AUTH ||
      sv6621_sae_frame_get_le16(frame + SV6621_SAE_FRAME_HEADER_SIZE) !=
          SV6621_SAE_AUTH_ALGORITHM)
    {
      return -ENOMSG;
    }

  memset(auth, 0, sizeof(*auth));
  memcpy(auth->destination, frame + 4, SV6621_MAC_LENGTH);
  memcpy(auth->source, frame + 10, SV6621_MAC_LENGTH);
  memcpy(auth->bssid, frame + 16, SV6621_MAC_LENGTH);
  auth->transaction = sv6621_sae_frame_get_le16(frame + 26);
  auth->status = sv6621_sae_frame_get_le16(frame + 28);
  auth->body = frame + SV6621_SAE_AUTH_FRAME_SIZE;
  auth->body_length = frame_length - SV6621_SAE_AUTH_FRAME_SIZE;
  return 0;
}

int sv6621_sae_auth_build(
    FAR const uint8_t destination[SV6621_MAC_LENGTH],
    FAR const uint8_t source[SV6621_MAC_LENGTH],
    FAR const uint8_t bssid[SV6621_MAC_LENGTH], uint16_t transaction,
    uint16_t status, FAR const uint8_t *body, size_t body_length,
    FAR uint8_t *frame, size_t capacity, FAR size_t *frame_length)
{
  size_t output_length;

  if (destination == NULL || source == NULL || bssid == NULL ||
      (body == NULL && body_length != 0) || frame == NULL ||
      frame_length == NULL ||
      body_length > SIZE_MAX - SV6621_SAE_AUTH_FRAME_SIZE)
    {
      return -EINVAL;
    }

  output_length = SV6621_SAE_AUTH_FRAME_SIZE + body_length;
  if (output_length > capacity)
    {
      return -ENOSPC;
    }

  memset(frame, 0, output_length);
  sv6621_sae_frame_put_le16(frame, SV6621_SAE_FRAME_CONTROL_AUTH);
  memcpy(frame + 4, destination, SV6621_MAC_LENGTH);
  memcpy(frame + 10, source, SV6621_MAC_LENGTH);
  memcpy(frame + 16, bssid, SV6621_MAC_LENGTH);
  sv6621_sae_frame_put_le16(frame + 24, SV6621_SAE_AUTH_ALGORITHM);
  sv6621_sae_frame_put_le16(frame + 26, transaction);
  sv6621_sae_frame_put_le16(frame + 28, status);
  if (body_length > 0)
    {
      memcpy(frame + SV6621_SAE_AUTH_FRAME_SIZE, body, body_length);
    }

  *frame_length = output_length;
  return 0;
}
