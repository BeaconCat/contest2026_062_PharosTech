/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_sae.c
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

#include "sv6621_ap_sae.h"
#include "sv6621_management.h"
#include "sv6621_sae_crypto.h"
#include "sv6621_sae_group.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_SAE_BODY_CAPACITY  128
#define SV6621_AP_SAE_FRAME_CAPACITY 192

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_sae_clear(FAR void *data, size_t length);
static FAR struct sv6621_ap_sae_peer_s *sv6621_ap_sae_find(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH]);
static FAR struct sv6621_ap_sae_peer_s *sv6621_ap_sae_allocate(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH]);
static int sv6621_ap_sae_transmit(FAR struct sv6621_ap_sae_s *sae,
                                  FAR const uint8_t *frame,
                                  size_t frame_length);
static int sv6621_ap_sae_send_commit(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_ap_sae_peer_s *peer);
static int sv6621_ap_sae_send_confirm(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_ap_sae_peer_s *peer);
static int sv6621_ap_sae_handle_commit(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_sae_auth_frame_s *auth);
static int sv6621_ap_sae_handle_confirm(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_sae_auth_frame_s *auth, FAR bool *accepted);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_ap_sae_clear(FAR void *data, size_t length)
{
  FAR volatile uint8_t *bytes = data;

  while (length-- > 0)
    {
      *bytes++ = 0;
    }
}

static FAR struct sv6621_ap_sae_peer_s *sv6621_ap_sae_find(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  size_t index;

  for (index = 0; index < SV6621_AP_SAE_PEER_CAPACITY; index++)
    {
      if (sae->peers[index].state != SV6621_AP_SAE_IDLE &&
          memcmp(sae->peers[index].address, address,
                 SV6621_MAC_LENGTH) == 0)
        {
          return &sae->peers[index];
        }
    }

  return NULL;
}

static FAR struct sv6621_ap_sae_peer_s *sv6621_ap_sae_allocate(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_sae_peer_s *peer;
  size_t index;

  peer = sv6621_ap_sae_find(sae, address);
  if (peer != NULL)
    {
      sv6621_ap_sae_clear(peer, sizeof(*peer));
      memcpy(peer->address, address, SV6621_MAC_LENGTH);
      return peer;
    }

  for (index = 0; index < SV6621_AP_SAE_PEER_CAPACITY; index++)
    {
      peer = &sae->peers[index];
      if (peer->state == SV6621_AP_SAE_IDLE)
        {
          memcpy(peer->address, address, SV6621_MAC_LENGTH);
          return peer;
        }
    }

  return NULL;
}

static int sv6621_ap_sae_transmit(FAR struct sv6621_ap_sae_s *sae,
                                  FAR const uint8_t *frame,
                                  size_t frame_length)
{
  uint64_t cookie = ++sae->next_cookie;

  if (cookie == 0)
    {
      cookie = ++sae->next_cookie;
    }

  return sv6621_management_tx(sae->command, sae->instance, 0, cookie,
                              sae->channel, sae->band, false, frame,
                              frame_length, frame_length);
}

static int sv6621_ap_sae_send_commit(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_ap_sae_peer_s *peer)
{
  uint8_t body[SV6621_AP_SAE_BODY_CAPACITY];
  uint8_t frame[SV6621_AP_SAE_FRAME_CAPACITY];
  size_t body_length;
  size_t frame_length;
  int ret;

  ret = sv6621_sae_commit_build(SV6621_SAE_GROUP_19, NULL, 0,
                                 peer->scalar, peer->element, body,
                                 sizeof(body), &body_length);
  if (ret == 0)
    {
      ret = sv6621_sae_auth_build(peer->address, sae->address, sae->address,
                                  1, 0, body, body_length, frame,
                                  sizeof(frame), &frame_length);
    }

  if (ret == 0)
    {
      ret = sv6621_ap_sae_transmit(sae, frame, frame_length);
    }

  sv6621_ap_sae_clear(body, sizeof(body));
  sv6621_ap_sae_clear(frame, sizeof(frame));
  return ret;
}

static int sv6621_ap_sae_send_confirm(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_ap_sae_peer_s *peer)
{
  uint8_t body[2 + SV6621_SAE_CONFIRM_SIZE];
  uint8_t frame[SV6621_AP_SAE_FRAME_CAPACITY];
  uint8_t confirm[SV6621_SAE_CONFIRM_SIZE];
  size_t body_length;
  size_t frame_length;
  int ret;

  ret = sv6621_sae_compute_confirm(
      peer->kck, 1, peer->scalar, peer->element, peer->peer_scalar,
      peer->peer_element, confirm);
  if (ret == 0)
    {
      ret = sv6621_sae_confirm_build(1, confirm, body, sizeof(body),
                                      &body_length);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_auth_build(peer->address, sae->address, sae->address,
                                  2, 0, body, body_length, frame,
                                  sizeof(frame), &frame_length);
    }

  if (ret == 0)
    {
      ret = sv6621_ap_sae_transmit(sae, frame, frame_length);
    }

  sv6621_ap_sae_clear(confirm, sizeof(confirm));
  sv6621_ap_sae_clear(body, sizeof(body));
  sv6621_ap_sae_clear(frame, sizeof(frame));
  return ret;
}

static int sv6621_ap_sae_handle_commit(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_sae_auth_frame_s *auth)
{
  FAR struct sv6621_ap_sae_peer_s *peer;
  struct sv6621_sae_commit_s commit;
  uint8_t secret[SV6621_SAE_SCALAR_SIZE];
  int ret;

  ret = sv6621_sae_commit_parse(auth, &commit);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_sae_allocate(sae, auth->source);
  if (peer == NULL)
    {
      return -ENOSPC;
    }

  ret = sv6621_sae_group_derive_pwe(
      sae->address, auth->source, sae->password, sae->password_length,
      peer->pwe);
  if (ret == 0)
    {
      ret = sv6621_sae_group_generate_commit(
          peer->pwe, peer->private_random, peer->scalar, peer->element);
    }

  if (ret == 0 &&
      memcmp(commit.scalar, peer->scalar, sizeof(peer->scalar)) == 0 &&
      memcmp(commit.element, peer->element, sizeof(peer->element)) == 0)
    {
      ret = -EKEYREJECTED;
    }

  if (ret == 0)
    {
      memcpy(peer->peer_scalar, commit.scalar, sizeof(peer->peer_scalar));
      memcpy(peer->peer_element, commit.element, sizeof(peer->peer_element));
      ret = sv6621_sae_group_derive_secret(
          peer->pwe, peer->private_random, peer->scalar,
          peer->peer_scalar, peer->peer_element, secret);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_derive_keys(
          secret, peer->scalar, peer->peer_scalar, peer->kck, peer->pmk,
          peer->pmkid);
    }

  if (ret == 0)
    {
      peer->state = SV6621_AP_SAE_WAIT_CONFIRM;
      ret = sv6621_ap_sae_send_commit(sae, peer);
    }

  if (ret < 0)
    {
      sv6621_ap_sae_clear(peer, sizeof(*peer));
    }

  sv6621_ap_sae_clear(secret, sizeof(secret));
  return ret;
}

static int sv6621_ap_sae_handle_confirm(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const struct sv6621_sae_auth_frame_s *auth, FAR bool *accepted)
{
  FAR struct sv6621_ap_sae_peer_s *peer;
  struct sv6621_sae_confirm_s received;
  uint8_t expected[SV6621_SAE_CONFIRM_SIZE];
  int ret;

  peer = sv6621_ap_sae_find(sae, auth->source);
  if (peer == NULL || peer->state != SV6621_AP_SAE_WAIT_CONFIRM)
    {
      return -ENOENT;
    }

  ret = sv6621_sae_confirm_parse(auth, &received);
  if (ret == 0 && received.counter <= peer->last_peer_confirm)
    {
      ret = -ESTALE;
    }

  if (ret == 0)
    {
      ret = sv6621_sae_compute_confirm(
          peer->kck, received.counter, peer->peer_scalar,
          peer->peer_element, peer->scalar, peer->element, expected);
    }

  if (ret == 0 &&
      !sv6621_sae_constant_equal(expected, received.value,
                                  sizeof(expected)))
    {
      ret = -EKEYREJECTED;
    }

  if (ret == 0)
    {
      peer->last_peer_confirm = received.counter;
      ret = sv6621_ap_sae_send_confirm(sae, peer);
    }

  if (ret == 0)
    {
      peer->state = SV6621_AP_SAE_ACCEPTED;
      *accepted = true;
    }

  sv6621_ap_sae_clear(expected, sizeof(expected));
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_sae_init(FAR struct sv6621_ap_sae_s *sae,
                       FAR struct sv6621_command_engine_s *command,
                       FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  int ret;

  if (sae == NULL || command == NULL || address == NULL)
    {
      return -EINVAL;
    }

  memset(sae, 0, sizeof(*sae));
  ret = nxmutex_init(&sae->lock);
  if (ret == 0)
    {
      sae->command = command;
      memcpy(sae->address, address, sizeof(sae->address));
    }

  return ret;
}

void sv6621_ap_sae_deinit(FAR struct sv6621_ap_sae_s *sae)
{
  if (sae != NULL)
    {
      sv6621_ap_sae_disable(sae);
      nxmutex_destroy(&sae->lock);
      sv6621_ap_sae_clear(sae, sizeof(*sae));
    }
}

int sv6621_ap_sae_enable(FAR struct sv6621_ap_sae_s *sae, uint8_t instance,
                         uint8_t channel, enum sv6621_band_e band,
                         FAR const uint8_t *password,
                         size_t password_length)
{
  int ret;

  if (sae == NULL || password == NULL || password_length < 8 ||
      password_length > sizeof(sae->password) || channel == 0 ||
      band > SV6621_BAND_5GHZ)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sae->enabled)
    {
      nxmutex_unlock(&sae->lock);
      return -EBUSY;
    }

  memset(sae->peers, 0, sizeof(sae->peers));
  memcpy(sae->password, password, password_length);
  sae->password_length = password_length;
  sae->instance = instance;
  sae->channel = channel;
  sae->band = band;
  sae->enabled = true;
  nxmutex_unlock(&sae->lock);
  return 0;
}

void sv6621_ap_sae_disable(FAR struct sv6621_ap_sae_s *sae)
{
  if (sae != NULL && nxmutex_lock(&sae->lock) == 0)
    {
      sv6621_ap_sae_clear(sae->peers, sizeof(sae->peers));
      sv6621_ap_sae_clear(sae->password, sizeof(sae->password));
      sae->password_length = 0;
      sae->enabled = false;
      nxmutex_unlock(&sae->lock);
    }
}

int sv6621_ap_sae_input(FAR struct sv6621_ap_sae_s *sae,
                        FAR const uint8_t *frame, size_t frame_length,
                        FAR bool *accepted,
                        FAR uint8_t address[SV6621_MAC_LENGTH])
{
  struct sv6621_sae_auth_frame_s auth;
  int ret;

  if (sae == NULL || frame == NULL || accepted == NULL || address == NULL)
    {
      return -EINVAL;
    }

  *accepted = false;
  ret = sv6621_sae_auth_parse(frame, frame_length, &auth);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!sae->enabled)
    {
      ret = -ENETDOWN;
    }
  else if (memcmp(auth.destination, sae->address, SV6621_MAC_LENGTH) != 0 ||
           memcmp(auth.bssid, sae->address, SV6621_MAC_LENGTH) != 0)
    {
      ret = -EACCES;
    }
  else if (auth.status != 0)
    {
      ret = -EACCES;
    }
  else if (auth.transaction == 1)
    {
      ret = sv6621_ap_sae_handle_commit(sae, &auth);
    }
  else if (auth.transaction == 2)
    {
      ret = sv6621_ap_sae_handle_confirm(sae, &auth, accepted);
    }
  else
    {
      ret = -EPROTO;
    }

  if (ret == 0)
    {
      memcpy(address, auth.source, SV6621_MAC_LENGTH);
    }

  nxmutex_unlock(&sae->lock);
  return ret;
}

int sv6621_ap_sae_get_pmk(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH],
    uint8_t pmk[SV6621_SAE_PMK_SIZE])
{
  FAR struct sv6621_ap_sae_peer_s *peer;
  int ret;

  if (sae == NULL || address == NULL || pmk == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  peer = sv6621_ap_sae_find(sae, address);
  if (peer == NULL || peer->state != SV6621_AP_SAE_ACCEPTED)
    {
      ret = -ENOENT;
    }
  else
    {
      memcpy(pmk, peer->pmk, SV6621_SAE_PMK_SIZE);
      ret = 0;
    }

  nxmutex_unlock(&sae->lock);
  return ret;
}

void sv6621_ap_sae_forget(
    FAR struct sv6621_ap_sae_s *sae,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_sae_peer_s *peer;

  if (sae != NULL && address != NULL && nxmutex_lock(&sae->lock) == 0)
    {
      peer = sv6621_ap_sae_find(sae, address);
      if (peer != NULL)
        {
          sv6621_ap_sae_clear(peer, sizeof(*peer));
        }

      nxmutex_unlock(&sae->lock);
    }
}
