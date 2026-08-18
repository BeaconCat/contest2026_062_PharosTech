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
#define SV6621_SAE_COMMIT_TRANSACTION 1
#define SV6621_SAE_CONFIRM_TRANSACTION 2
#define SV6621_SAE_COMMIT_FIXED_SIZE \
  (2 + SV6621_SAE_SCALAR_SIZE + SV6621_SAE_ELEMENT_SIZE)
#define SV6621_SAE_CONFIRM_BODY_SIZE \
  (2 + SV6621_SAE_CONFIRM_SIZE)

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

int sv6621_sae_commit_parse(FAR const struct sv6621_sae_auth_frame_s *auth,
                            FAR struct sv6621_sae_commit_s *commit)
{
  size_t token_length;

  if (auth == NULL || commit == NULL ||
      auth->transaction != SV6621_SAE_COMMIT_TRANSACTION ||
      auth->status != 0 || auth->body_length < SV6621_SAE_COMMIT_FIXED_SIZE)
    {
      return -EINVAL;
    }

  memset(commit, 0, sizeof(*commit));
  commit->group = sv6621_sae_frame_get_le16(auth->body);
  if (commit->group != SV6621_SAE_GROUP_19)
    {
      return -EOPNOTSUPP;
    }

  token_length = auth->body_length - SV6621_SAE_COMMIT_FIXED_SIZE;
  commit->token = auth->body + 2;
  commit->token_length = token_length;
  commit->scalar = auth->body + 2 + token_length;
  commit->element = commit->scalar + SV6621_SAE_SCALAR_SIZE;
  return 0;
}

int sv6621_sae_commit_build(
    uint16_t group, FAR const uint8_t *token, size_t token_length,
    FAR const uint8_t scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t element[SV6621_SAE_ELEMENT_SIZE], FAR uint8_t *body,
    size_t capacity, FAR size_t *body_length)
{
  size_t output_length;
  size_t offset;

  if (group != SV6621_SAE_GROUP_19 ||
      (token == NULL && token_length != 0) || scalar == NULL ||
      element == NULL || body == NULL || body_length == NULL ||
      token_length > SIZE_MAX - SV6621_SAE_COMMIT_FIXED_SIZE)
    {
      return -EINVAL;
    }

  output_length = SV6621_SAE_COMMIT_FIXED_SIZE + token_length;
  if (output_length > capacity)
    {
      return -ENOSPC;
    }

  sv6621_sae_frame_put_le16(body, group);
  offset = 2;
  if (token_length > 0)
    {
      memcpy(body + offset, token, token_length);
      offset += token_length;
    }

  memcpy(body + offset, scalar, SV6621_SAE_SCALAR_SIZE);
  offset += SV6621_SAE_SCALAR_SIZE;
  memcpy(body + offset, element, SV6621_SAE_ELEMENT_SIZE);
  *body_length = output_length;
  return 0;
}

int sv6621_sae_confirm_parse(FAR const struct sv6621_sae_auth_frame_s *auth,
                             FAR struct sv6621_sae_confirm_s *confirm)
{
  if (auth == NULL || confirm == NULL ||
      auth->transaction != SV6621_SAE_CONFIRM_TRANSACTION ||
      auth->status != 0 || auth->body_length != SV6621_SAE_CONFIRM_BODY_SIZE)
    {
      return -EINVAL;
    }

  confirm->counter = sv6621_sae_frame_get_le16(auth->body);
  confirm->value = auth->body + 2;
  return 0;
}

int sv6621_sae_confirm_build(
    uint16_t counter, FAR const uint8_t value[SV6621_SAE_CONFIRM_SIZE],
    FAR uint8_t *body, size_t capacity, FAR size_t *body_length)
{
  if (value == NULL || body == NULL || body_length == NULL)
    {
      return -EINVAL;
    }

  if (capacity < SV6621_SAE_CONFIRM_BODY_SIZE)
    {
      return -ENOSPC;
    }

  sv6621_sae_frame_put_le16(body, counter);
  memcpy(body + 2, value, SV6621_SAE_CONFIRM_SIZE);
  *body_length = SV6621_SAE_CONFIRM_BODY_SIZE;
  return 0;
}
