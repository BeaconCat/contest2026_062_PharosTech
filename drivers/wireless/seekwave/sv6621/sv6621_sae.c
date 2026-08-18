/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae.c
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

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>

#include "sv6621_management.h"
#include "sv6621_sae.h"
#include "sv6621_sae_crypto.h"
#include "sv6621_sae_group.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_TIMEOUT_MS        1000
#define SV6621_SAE_MAX_RETRIES       3
#define SV6621_SAE_BODY_CAPACITY     384

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool sv6621_sae_active(enum sv6621_sae_state_e state);
static void sv6621_sae_clear(FAR struct sv6621_sae_s *sae);
static void sv6621_sae_finish(FAR struct sv6621_sae_s *sae, int result);
static int sv6621_sae_send_commit(FAR struct sv6621_sae_s *sae,
                                  uint32_t generation);
static int sv6621_sae_send_confirm(FAR struct sv6621_sae_s *sae,
                                   uint32_t generation);
static int sv6621_sae_schedule_timeout(FAR struct sv6621_sae_s *sae,
                                       uint32_t generation);
static void sv6621_sae_worker(FAR void *arg);
static void sv6621_sae_timeout_worker(FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool sv6621_sae_active(enum sv6621_sae_state_e state)
{
  return state == SV6621_SAE_COMMIT_SENT ||
         state == SV6621_SAE_CONFIRM_SENT;
}

static void sv6621_sae_clear(FAR struct sv6621_sae_s *sae)
{
  sv6621_sae_zeroize(sae->pwe, sizeof(sae->pwe));
  sv6621_sae_zeroize(sae->private_random, sizeof(sae->private_random));
  sv6621_sae_zeroize(sae->scalar, sizeof(sae->scalar));
  sv6621_sae_zeroize(sae->element, sizeof(sae->element));
  sv6621_sae_zeroize(sae->peer_scalar, sizeof(sae->peer_scalar));
  sv6621_sae_zeroize(sae->peer_element, sizeof(sae->peer_element));
  sv6621_sae_zeroize(sae->kck, sizeof(sae->kck));
  sv6621_sae_zeroize(sae->pmk, sizeof(sae->pmk));
  sv6621_sae_zeroize(sae->pmkid, sizeof(sae->pmkid));
  sv6621_sae_zeroize(sae->token, sizeof(sae->token));
  sv6621_sae_zeroize(sae->pending_frame, sizeof(sae->pending_frame));
  sae->token_length = 0;
  sae->pending_frame_length = 0;
  sae->frame_pending = false;
  sae->send_confirm = 1;
  sae->retries = 0;
}

static void sv6621_sae_finish(FAR struct sv6621_sae_s *sae, int result)
{
  uint8_t pmk[SV6621_SAE_PMK_SIZE];
  uint8_t pmkid[SV6621_SAE_PMKID_SIZE];
  sv6621_sae_complete_t complete;
  FAR void *complete_arg;

  if (nxmutex_lock(&sae->lock) < 0)
    {
      return;
    }

  if (!sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return;
    }

  complete = sae->complete;
  complete_arg = sae->complete_arg;
  if (result == 0)
    {
      memcpy(pmk, sae->pmk, sizeof(pmk));
      memcpy(pmkid, sae->pmkid, sizeof(pmkid));
      sae->state = SV6621_SAE_ACCEPTED;
    }
  else
    {
      memset(pmk, 0, sizeof(pmk));
      memset(pmkid, 0, sizeof(pmkid));
      sae->state = SV6621_SAE_FAILED;
    }

  sv6621_sae_clear(sae);
  nxmutex_unlock(&sae->lock);
  if (complete != NULL)
    {
      complete(result, result == 0 ? pmk : NULL,
               result == 0 ? pmkid : NULL, complete_arg);
    }

  sv6621_sae_zeroize(pmk, sizeof(pmk));
  sv6621_sae_zeroize(pmkid, sizeof(pmkid));
}

static int sv6621_sae_schedule_timeout(FAR struct sv6621_sae_s *sae,
                                       uint32_t generation)
{
  int ret;

  if (nxmutex_lock(&sae->lock) < 0)
    {
      return -EINTR;
    }

  if (sae->generation != generation || !sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return -ECANCELED;
    }

  nxmutex_unlock(&sae->lock);
  ret = work_queue(LPWORK, &sae->timeout_work, sv6621_sae_timeout_worker,
                   sae, MSEC2TICK(SV6621_SAE_TIMEOUT_MS));
  return ret;
}

static int sv6621_sae_send_commit(FAR struct sv6621_sae_s *sae,
                                  uint32_t generation)
{
  uint8_t scalar[SV6621_SAE_SCALAR_SIZE];
  uint8_t element[SV6621_SAE_ELEMENT_SIZE];
  uint8_t token[SV6621_SAE_TOKEN_CAPACITY];
  uint8_t frame[SV6621_SAE_FRAME_CAPACITY];
  uint8_t body[SV6621_SAE_BODY_CAPACITY];
  uint8_t local[SV6621_MAC_LENGTH];
  uint8_t peer[SV6621_MAC_LENGTH];
  size_t token_length;
  size_t body_length;
  size_t frame_length;
  enum sv6621_band_e band;
  uint64_t cookie;
  uint8_t channel;
  uint8_t instance;
  int ret;

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sae->generation != generation ||
      sae->state != SV6621_SAE_COMMIT_SENT)
    {
      nxmutex_unlock(&sae->lock);
      return -ECANCELED;
    }

  memcpy(scalar, sae->scalar, sizeof(scalar));
  memcpy(element, sae->element, sizeof(element));
  memcpy(token, sae->token, sae->token_length);
  token_length = sae->token_length;
  memcpy(local, sae->local, sizeof(local));
  memcpy(peer, sae->peer, sizeof(peer));
  channel = sae->channel;
  band = sae->band;
  instance = sae->instance;
  cookie = ++sae->cookie;
  nxmutex_unlock(&sae->lock);

  ret = sv6621_sae_commit_build(SV6621_SAE_GROUP_19, token, token_length,
                                 scalar, element, body, sizeof(body),
                                 &body_length);
  if (ret == 0)
    {
      ret = sv6621_sae_auth_build(peer, local, peer, 1, 0, body, body_length,
                                  frame, sizeof(frame), &frame_length);
    }

  if (ret == 0)
    {
      ret = sv6621_management_tx(sae->command, instance, 0, cookie, channel,
                                 band, false, frame, frame_length,
                                 frame_length);
    }

  sv6621_sae_zeroize(scalar, sizeof(scalar));
  sv6621_sae_zeroize(element, sizeof(element));
  sv6621_sae_zeroize(token, sizeof(token));
  sv6621_sae_zeroize(body, sizeof(body));
  sv6621_sae_zeroize(frame, sizeof(frame));
  return ret;
}

static int sv6621_sae_send_confirm(FAR struct sv6621_sae_s *sae,
                                   uint32_t generation)
{
  uint8_t frame[SV6621_SAE_FRAME_CAPACITY];
  uint8_t body[2 + SV6621_SAE_CONFIRM_SIZE];
  uint8_t confirm[SV6621_SAE_CONFIRM_SIZE];
  uint8_t local[SV6621_MAC_LENGTH];
  uint8_t peer[SV6621_MAC_LENGTH];
  size_t body_length;
  size_t frame_length;
  enum sv6621_band_e band;
  uint64_t cookie;
  uint16_t counter;
  uint8_t channel;
  uint8_t instance;
  int ret;

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sae->generation != generation ||
      sae->state != SV6621_SAE_CONFIRM_SENT)
    {
      nxmutex_unlock(&sae->lock);
      return -ECANCELED;
    }

  counter = sae->send_confirm;
  ret = sv6621_sae_compute_confirm(
      sae->kck, counter, sae->scalar, sae->element, sae->peer_scalar,
      sae->peer_element, confirm);
  memcpy(local, sae->local, sizeof(local));
  memcpy(peer, sae->peer, sizeof(peer));
  channel = sae->channel;
  band = sae->band;
  instance = sae->instance;
  cookie = ++sae->cookie;
  nxmutex_unlock(&sae->lock);

  if (ret == 0)
    {
      ret = sv6621_sae_confirm_build(counter, confirm, body, sizeof(body),
                                      &body_length);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_auth_build(peer, local, peer, 2, 0, body, body_length,
                                  frame, sizeof(frame), &frame_length);
    }

  if (ret == 0)
    {
      ret = sv6621_management_tx(sae->command, instance, 0, cookie, channel,
                                 band, false, frame, frame_length,
                                 frame_length);
    }

  sv6621_sae_zeroize(confirm, sizeof(confirm));
  sv6621_sae_zeroize(body, sizeof(body));
  sv6621_sae_zeroize(frame, sizeof(frame));
  return ret;
}

static void sv6621_sae_worker(FAR void *arg)
{
  FAR struct sv6621_sae_s *sae = arg;
  struct sv6621_sae_auth_frame_s auth;
  struct sv6621_sae_commit_s commit;
  struct sv6621_sae_confirm_s received;
  FAR uint8_t *frame;
  uint8_t expected[SV6621_SAE_CONFIRM_SIZE];
  uint8_t secret[SV6621_SAE_SCALAR_SIZE];
  uint32_t generation;
  size_t frame_length;
  int ret;

  if (nxmutex_lock(&sae->lock) < 0)
    {
      return;
    }

  if (!sae->frame_pending || !sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return;
    }

  frame_length = sae->pending_frame_length;
  frame = kmm_malloc(frame_length);
  if (frame == NULL)
    {
      sae->frame_pending = false;
      nxmutex_unlock(&sae->lock);
      sv6621_sae_finish(sae, -ENOMEM);
      return;
    }

  memcpy(frame, sae->pending_frame, frame_length);
  sae->frame_pending = false;
  generation = sae->generation;
  nxmutex_unlock(&sae->lock);
  work_cancel(LPWORK, &sae->timeout_work);

  ret = sv6621_sae_auth_parse(frame, frame_length, &auth);
  if (ret < 0 ||
      memcmp(auth.destination, sae->local, SV6621_MAC_LENGTH) != 0 ||
      memcmp(auth.source, sae->peer, SV6621_MAC_LENGTH) != 0 ||
      memcmp(auth.bssid, sae->peer, SV6621_MAC_LENGTH) != 0)
    {
      ret = ret < 0 ? ret : -EACCES;
      goto finish;
    }

  if (auth.status == SV6621_SAE_STATUS_UNSUPPORTED_GROUP)
    {
      ret = -EOPNOTSUPP;
      goto finish;
    }

  if (auth.status == SV6621_SAE_STATUS_ANTI_CLOGGING_TOKEN)
    {
      FAR const uint8_t *token;
      size_t token_length;

      ret = sv6621_sae_token_parse(&auth, &token, &token_length);
      if (ret < 0 || token_length > SV6621_SAE_TOKEN_CAPACITY)
        {
          ret = ret < 0 ? ret : -E2BIG;
          goto finish;
        }

      if (nxmutex_lock(&sae->lock) < 0)
        {
          ret = -EINTR;
          goto finish;
        }

      if (sae->generation != generation ||
          sae->state != SV6621_SAE_COMMIT_SENT)
        {
          nxmutex_unlock(&sae->lock);
          ret = -ECANCELED;
          goto done;
        }

      memcpy(sae->token, token, token_length);
      sae->token_length = token_length;
      sae->retries = 0;
      nxmutex_unlock(&sae->lock);
      ret = sv6621_sae_send_commit(sae, generation);
      if (ret == 0)
        {
          ret = sv6621_sae_schedule_timeout(sae, generation);
        }

      goto finish;
    }

  if (auth.status != 0)
    {
      ret = -EACCES;
      goto finish;
    }

  if (auth.transaction == 1)
    {
      ret = sv6621_sae_commit_parse(&auth, &commit);
      if (ret < 0)
        {
          goto finish;
        }

      ret = sv6621_sae_group_derive_secret(
          sae->pwe, sae->private_random, sae->scalar, commit.scalar,
          commit.element, secret);
      if (ret < 0)
        {
          goto finish;
        }

      if (nxmutex_lock(&sae->lock) < 0)
        {
          ret = -EINTR;
          goto finish;
        }

      if (sae->generation != generation ||
          sae->state != SV6621_SAE_COMMIT_SENT)
        {
          nxmutex_unlock(&sae->lock);
          ret = -ECANCELED;
          goto done;
        }

      memcpy(sae->peer_scalar, commit.scalar, sizeof(sae->peer_scalar));
      memcpy(sae->peer_element, commit.element, sizeof(sae->peer_element));
      ret = sv6621_sae_derive_keys(
          secret, sae->scalar, sae->peer_scalar, sae->kck, sae->pmk,
          sae->pmkid);
      if (ret == 0)
        {
          sae->state = SV6621_SAE_CONFIRM_SENT;
          sae->retries = 0;
        }

      nxmutex_unlock(&sae->lock);
      if (ret == 0)
        {
          ret = sv6621_sae_send_confirm(sae, generation);
        }

      if (ret == 0)
        {
          ret = sv6621_sae_schedule_timeout(sae, generation);
        }

      goto finish;
    }

  if (auth.transaction == 2)
    {
      ret = sv6621_sae_confirm_parse(&auth, &received);
      if (ret < 0)
        {
          goto finish;
        }

      if (nxmutex_lock(&sae->lock) < 0)
        {
          ret = -EINTR;
          goto finish;
        }

      if (sae->generation != generation ||
          sae->state != SV6621_SAE_CONFIRM_SENT)
        {
          nxmutex_unlock(&sae->lock);
          ret = -ECANCELED;
          goto done;
        }

      ret = sv6621_sae_compute_confirm(
          sae->kck, received.counter, sae->peer_scalar, sae->peer_element,
          sae->scalar, sae->element, expected);
      nxmutex_unlock(&sae->lock);
      if (ret == 0 &&
          !sv6621_sae_constant_equal(expected, received.value,
                                      sizeof(expected)))
        {
          ret = -EKEYREJECTED;
        }

      if (ret == 0)
        {
          sv6621_sae_finish(sae, 0);
          goto done;
        }

      goto finish;
    }

  ret = -EPROTO;

finish:
  if (ret < 0 && ret != -ECANCELED)
    {
      sv6621_sae_finish(sae, ret);
    }

done:
  sv6621_sae_zeroize(expected, sizeof(expected));
  sv6621_sae_zeroize(secret, sizeof(secret));
  sv6621_sae_zeroize(frame, frame_length);
  kmm_free(frame);
}

static void sv6621_sae_timeout_worker(FAR void *arg)
{
  FAR struct sv6621_sae_s *sae = arg;
  enum sv6621_sae_state_e state;
  uint32_t generation;
  int ret;

  if (nxmutex_lock(&sae->lock) < 0)
    {
      return;
    }

  if (!sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return;
    }

  if (sae->retries >= SV6621_SAE_MAX_RETRIES)
    {
      nxmutex_unlock(&sae->lock);
      sv6621_sae_finish(sae, -ETIMEDOUT);
      return;
    }

  sae->retries++;
  state = sae->state;
  generation = sae->generation;
  nxmutex_unlock(&sae->lock);
  ret = state == SV6621_SAE_COMMIT_SENT ?
            sv6621_sae_send_commit(sae, generation) :
            sv6621_sae_send_confirm(sae, generation);
  if (ret == 0)
    {
      ret = sv6621_sae_schedule_timeout(sae, generation);
    }

  if (ret < 0 && ret != -ECANCELED)
    {
      sv6621_sae_finish(sae, ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sae_init(FAR struct sv6621_sae_s *sae,
                    FAR struct sv6621_command_engine_s *command,
                    sv6621_sae_complete_t complete, FAR void *complete_arg)
{
  int ret;

  if (sae == NULL || command == NULL)
    {
      return -EINVAL;
    }

  memset(sae, 0, sizeof(*sae));
  ret = nxmutex_init(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  sae->command = command;
  sae->complete = complete;
  sae->complete_arg = complete_arg;
  sae->state = SV6621_SAE_IDLE;
  sae->send_confirm = 1;
  return 0;
}

void sv6621_sae_deinit(FAR struct sv6621_sae_s *sae)
{
  if (sae == NULL)
    {
      return;
    }

  if (nxmutex_lock(&sae->lock) >= 0)
    {
      sae->shutting_down = true;
      nxmutex_unlock(&sae->lock);
    }

  sv6621_sae_cancel(sae, -ESHUTDOWN);
  work_cancel_sync(LPWORK, &sae->work);
  work_cancel_sync(LPWORK, &sae->timeout_work);
  sv6621_sae_clear(sae);
  nxmutex_destroy(&sae->lock);
}

int sv6621_sae_start(
    FAR struct sv6621_sae_s *sae,
    FAR const uint8_t local[SV6621_MAC_LENGTH],
    FAR const uint8_t peer[SV6621_MAC_LENGTH], uint8_t instance,
    uint8_t channel, enum sv6621_band_e band, FAR const uint8_t *password,
    size_t password_length)
{
  uint32_t generation;
  int ret;

  if (sae == NULL || local == NULL || peer == NULL || password == NULL ||
      instance > 0x0f || channel == 0 || band > SV6621_BAND_5GHZ ||
      password_length == 0 || password_length > 63)
    {
      return -EINVAL;
    }

  work_cancel_sync(LPWORK, &sae->work);
  work_cancel_sync(LPWORK, &sae->timeout_work);
  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (sae->shutting_down)
    {
      nxmutex_unlock(&sae->lock);
      return -ESHUTDOWN;
    }

  if (sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return -EBUSY;
    }

  sv6621_sae_clear(sae);
  memcpy(sae->local, local, sizeof(sae->local));
  memcpy(sae->peer, peer, sizeof(sae->peer));
  sae->instance = instance;
  sae->channel = channel;
  sae->band = band;
  sae->generation++;
  generation = sae->generation;
  ret = sv6621_sae_group_derive_pwe(local, peer, password, password_length,
                                     sae->pwe);
  if (ret == 0)
    {
      ret = sv6621_sae_group_generate_commit(
          sae->pwe, sae->private_random, sae->scalar, sae->element);
    }

  if (ret == 0)
    {
      sae->state = SV6621_SAE_COMMIT_SENT;
    }
  else
    {
      sae->state = SV6621_SAE_FAILED;
      sv6621_sae_clear(sae);
    }

  nxmutex_unlock(&sae->lock);
  if (ret == 0)
    {
      ret = sv6621_sae_send_commit(sae, generation);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_schedule_timeout(sae, generation);
    }

  if (ret < 0)
    {
      if (nxmutex_lock(&sae->lock) >= 0)
        {
          if (sae->generation == generation)
            {
              sae->state = SV6621_SAE_FAILED;
              sv6621_sae_clear(sae);
            }

          nxmutex_unlock(&sae->lock);
        }
    }

  return ret;
}

int sv6621_sae_input(FAR struct sv6621_sae_s *sae,
                     FAR const uint8_t *frame, size_t frame_length)
{
  int ret;

  if (sae == NULL || frame == NULL || frame_length == 0 ||
      frame_length > SV6621_SAE_FRAME_CAPACITY)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&sae->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!sv6621_sae_active(sae->state))
    {
      nxmutex_unlock(&sae->lock);
      return -ENOTCONN;
    }

  if (sae->frame_pending)
    {
      nxmutex_unlock(&sae->lock);
      return -EBUSY;
    }

  memcpy(sae->pending_frame, frame, frame_length);
  sae->pending_frame_length = frame_length;
  sae->frame_pending = true;
  nxmutex_unlock(&sae->lock);
  ret = work_queue(LPWORK, &sae->work, sv6621_sae_worker, sae, 0);
  if (ret < 0 && nxmutex_lock(&sae->lock) >= 0)
    {
      sae->frame_pending = false;
      sae->pending_frame_length = 0;
      nxmutex_unlock(&sae->lock);
    }

  return ret;
}

void sv6621_sae_cancel(FAR struct sv6621_sae_s *sae, int result)
{
  bool active;

  if (sae == NULL)
    {
      return;
    }

  work_cancel_sync(LPWORK, &sae->work);
  work_cancel_sync(LPWORK, &sae->timeout_work);
  if (nxmutex_lock(&sae->lock) < 0)
    {
      return;
    }

  active = sv6621_sae_active(sae->state);
  sae->generation++;
  if (active)
    {
      sae->state = SV6621_SAE_FAILED;
    }

  sv6621_sae_clear(sae);

  nxmutex_unlock(&sae->lock);
  if (active && sae->complete != NULL)
    {
      sae->complete(result, NULL, NULL, sae->complete_arg);
    }
}
