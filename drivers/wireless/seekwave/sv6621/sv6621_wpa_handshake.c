/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_handshake.c
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

#include <errno.h>
#include <string.h>
#include <sys/random.h>

#include "sv6621_wpa_handshake.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_ETHERNET_HEADER_SIZE 14
#define SV6621_WPA_ETHERTYPE_EAPOL      0x888e
#define SV6621_WPA_KCK_OFFSET           0
#define SV6621_WPA_KEK_OFFSET           16
#define SV6621_WPA_TK_OFFSET            32
#define SV6621_WPA_TK_SIZE              16
#define SV6621_WPA_KEY_ENCRYPTED        0x1000
#define SV6621_WPA_REASON_UNSPECIFIED   1

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_wpa_clear(FAR void *buffer, size_t length);
static int sv6621_wpa_generate_nonce(
    uint8_t nonce[SV6621_WPA_NONCE_SIZE]);
static int sv6621_wpa_compare_replay(FAR const uint8_t *left,
                                     FAR const uint8_t *right);
static int sv6621_wpa_schedule_locked(FAR struct sv6621_wpa_s *wpa);
static void sv6621_wpa_remove_keys(FAR struct sv6621_wpa_s *wpa);
static void sv6621_wpa_finish(FAR struct sv6621_wpa_s *wpa, int result);
static int sv6621_wpa_send_response(
    FAR struct sv6621_wpa_s *wpa, enum sv6621_wpa_response_e response);
static int sv6621_wpa_decode_gtk(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol, FAR uint8_t *key_index,
    FAR uint8_t *gtk, FAR size_t *gtk_length);
static int sv6621_wpa_process_message_1(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol);
static int sv6621_wpa_process_message_3(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol);
static int sv6621_wpa_process_group_message_1(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol);
static void sv6621_wpa_worker(FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_clear
 ****************************************************************************/

static void sv6621_wpa_clear(FAR void *buffer, size_t length)
{
  FAR volatile uint8_t *bytes = buffer;

  while (length-- > 0)
    {
      *bytes++ = 0;
    }
}

/****************************************************************************
 * Name: sv6621_wpa_generate_nonce
 ****************************************************************************/

static int sv6621_wpa_generate_nonce(
    uint8_t nonce[SV6621_WPA_NONCE_SIZE])
{
  ssize_t random_length;

  random_length = getrandom(nonce, SV6621_WPA_NONCE_SIZE, 0);
  if (random_length < 0)
    {
      return -errno;
    }

  return random_length == SV6621_WPA_NONCE_SIZE ? 0 : -EIO;
}

/****************************************************************************
 * Name: sv6621_wpa_compare_replay
 ****************************************************************************/

static int sv6621_wpa_compare_replay(FAR const uint8_t *left,
                                     FAR const uint8_t *right)
{
  size_t index;

  for (index = 0; index < SV6621_WPA_REPLAY_SIZE; index++)
    {
      if (left[index] != right[index])
        {
          return left[index] < right[index] ? -1 : 1;
        }
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_schedule_locked
 ****************************************************************************/

static int sv6621_wpa_schedule_locked(FAR struct sv6621_wpa_s *wpa)
{
  int ret;

  if (!wpa->peer_ready || !wpa->frame_pending || wpa->work_scheduled)
    {
      return 0;
    }

  wpa->work_scheduled = true;
  ret = work_queue(LPWORK, &wpa->work, sv6621_wpa_worker, wpa, 0);
  if (ret < 0)
    {
      wpa->work_scheduled = false;
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_remove_keys
 ****************************************************************************/

static void sv6621_wpa_remove_keys(FAR struct sv6621_wpa_s *wpa)
{
  uint8_t broadcast[SV6621_MAC_LENGTH];

  if (wpa->group_installed)
    {
      memset(broadcast, 0xff, sizeof(broadcast));
      sv6621_security_delete_key(
          wpa->command, SV6621_SECURITY_KEY_GROUP,
          SV6621_SECURITY_CIPHER_CCMP, broadcast, wpa->gtk_index);
      wpa->group_installed = false;
    }

  if (wpa->pairwise_installed)
    {
      sv6621_security_delete_key(
          wpa->command, SV6621_SECURITY_KEY_PAIRWISE,
          SV6621_SECURITY_CIPHER_CCMP, wpa->authenticator, 0);
      wpa->pairwise_installed = false;
    }
}

/****************************************************************************
 * Name: sv6621_wpa_finish
 ****************************************************************************/

static void sv6621_wpa_finish(FAR struct sv6621_wpa_s *wpa, int result)
{
  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  wpa->result = result;
  wpa->state = result == 0 ? SV6621_WPA_COMPLETE : SV6621_WPA_FAILED;
  wpa->frame_pending = false;
  nxmutex_unlock(&wpa->lock);
  nxsem_post(&wpa->completion);
}

/****************************************************************************
 * Name: sv6621_wpa_send_response
 ****************************************************************************/

static int sv6621_wpa_send_response(
    FAR struct sv6621_wpa_s *wpa, enum sv6621_wpa_response_e response)
{
  uint8_t frame[SV6621_WPA_FRAME_CAPACITY];
  size_t eapol_length;
  int ret;

  memcpy(frame, wpa->authenticator, SV6621_MAC_LENGTH);
  memcpy(frame + SV6621_MAC_LENGTH, wpa->supplicant, SV6621_MAC_LENGTH);
  frame[12] = SV6621_WPA_ETHERTYPE_EAPOL >> 8;
  frame[13] = SV6621_WPA_ETHERTYPE_EAPOL & 0xff;
  ret = sv6621_wpa_eapol_build(
      response, wpa->eapol_version, wpa->replay, wpa->snonce,
      wpa->ptk + SV6621_WPA_KCK_OFFSET,
      frame + SV6621_WPA_ETHERNET_HEADER_SIZE,
      sizeof(frame) - SV6621_WPA_ETHERNET_HEADER_SIZE, &eapol_length);
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_security_send_eapol(
      wpa->command, &wpa->tx_context, frame,
      SV6621_WPA_ETHERNET_HEADER_SIZE + eapol_length);
}

/****************************************************************************
 * Name: sv6621_wpa_decode_gtk
 ****************************************************************************/

static int sv6621_wpa_decode_gtk(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol, FAR uint8_t *key_index,
    FAR uint8_t *gtk, FAR size_t *gtk_length)
{
  uint8_t plain[SV6621_WPA_FRAME_CAPACITY];
  size_t plain_length;
  int ret;

  if ((eapol->key_info & SV6621_WPA_KEY_ENCRYPTED) == 0)
    {
      return -EOPNOTSUPP;
    }

  ret = sv6621_wpa_unwrap_key(
      wpa->ptk + SV6621_WPA_KEK_OFFSET, eapol->key_data,
      eapol->key_data_length, plain, sizeof(plain), &plain_length);
  if (ret == 0)
    {
      ret = sv6621_wpa_eapol_extract_gtk(
          plain, plain_length, key_index, gtk, SV6621_WPA_GTK_MAX_SIZE,
          gtk_length);
    }

  sv6621_wpa_clear(plain, sizeof(plain));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_message_1
 ****************************************************************************/

static int sv6621_wpa_process_message_1(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol)
{
  int ret;

  ret = sv6621_wpa_compare_replay(eapol->replay, wpa->replay);
  if (ret == 0)
    {
      return memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) == 0 ?
             sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_2) :
             -EKEYREJECTED;
    }

  if (ret < 0)
    {
      return -EALREADY;
    }

  if (wpa->state == SV6621_WPA_WAIT_MESSAGE_3 &&
      memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) != 0)
    {
      ret = sv6621_wpa_generate_nonce(wpa->snonce);
      if (ret < 0)
        {
          return ret;
        }
    }

  memcpy(wpa->anonce, eapol->nonce, sizeof(wpa->anonce));
  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));
  wpa->eapol_version = eapol->version;
  ret = sv6621_wpa_derive_ptk(
      wpa->pmk, wpa->authenticator, wpa->supplicant, wpa->anonce,
      wpa->snonce, wpa->ptk);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_2);
  if (ret == 0)
    {
      ret = nxmutex_lock(&wpa->lock);
      if (ret == 0)
        {
          wpa->state = SV6621_WPA_WAIT_MESSAGE_3;
          nxmutex_unlock(&wpa->lock);
        }
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_message_3
 ****************************************************************************/

static int sv6621_wpa_process_message_3(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol)
{
  uint8_t gtk[SV6621_WPA_GTK_MAX_SIZE];
  uint8_t broadcast[SV6621_MAC_LENGTH];
  size_t gtk_length;
  uint8_t gtk_index;
  int ret;

  if (sv6621_wpa_compare_replay(eapol->replay, wpa->replay) <= 0 ||
      memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) != 0)
    {
      return -EKEYREJECTED;
    }

  ret = sv6621_wpa_eapol_verify_mic(
      eapol, wpa->ptk + SV6621_WPA_KCK_OFFSET);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_decode_gtk(wpa, eapol, &gtk_index, gtk, &gtk_length);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_security_add_key(
      wpa->command, SV6621_SECURITY_KEY_PAIRWISE,
      SV6621_SECURITY_CIPHER_CCMP, wpa->authenticator, 0,
      wpa->ptk + SV6621_WPA_TK_OFFSET, SV6621_WPA_TK_SIZE, NULL);
  if (ret < 0)
    {
      goto clear_gtk;
    }

  wpa->pairwise_installed = true;

  memset(broadcast, 0xff, sizeof(broadcast));
  ret = sv6621_security_add_key(
      wpa->command, SV6621_SECURITY_KEY_GROUP,
      SV6621_SECURITY_CIPHER_CCMP, broadcast, gtk_index, gtk, gtk_length,
      eapol->rsc);
  if (ret < 0)
    {
      goto clear_gtk;
    }

  wpa->gtk_index = gtk_index;
  wpa->group_installed = true;

  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));
  wpa->eapol_version = eapol->version;
  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_4);
  if (ret == 0)
    {
      ret = sv6621_station_mark_connected(wpa->station);
    }

clear_gtk:
  sv6621_wpa_clear(gtk, sizeof(gtk));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_group_message_1
 ****************************************************************************/

static int sv6621_wpa_process_group_message_1(
    FAR struct sv6621_wpa_s *wpa,
    FAR const struct sv6621_wpa_eapol_s *eapol)
{
  uint8_t gtk[SV6621_WPA_GTK_MAX_SIZE];
  uint8_t broadcast[SV6621_MAC_LENGTH];
  size_t gtk_length;
  uint8_t gtk_index;
  int comparison;
  int ret;

  ret = sv6621_wpa_eapol_verify_mic(
      eapol, wpa->ptk + SV6621_WPA_KCK_OFFSET);
  if (ret < 0)
    {
      return ret;
    }

  comparison = sv6621_wpa_compare_replay(eapol->replay, wpa->replay);
  if (comparison == 0)
    {
      wpa->eapol_version = eapol->version;
      return sv6621_wpa_send_response(
          wpa, SV6621_WPA_RESPONSE_GROUP_2);
    }

  if (comparison < 0)
    {
      return -EALREADY;
    }

  ret = sv6621_wpa_decode_gtk(wpa, eapol, &gtk_index, gtk, &gtk_length);
  if (ret < 0)
    {
      goto clear_gtk;
    }

  memset(broadcast, 0xff, sizeof(broadcast));
  ret = sv6621_security_add_key(
      wpa->command, SV6621_SECURITY_KEY_GROUP,
      SV6621_SECURITY_CIPHER_CCMP, broadcast, gtk_index, gtk, gtk_length,
      eapol->rsc);
  if (ret < 0)
    {
      goto clear_gtk;
    }

  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));
  wpa->eapol_version = eapol->version;
  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_GROUP_2);
  if (ret < 0)
    {
      sv6621_security_delete_key(
          wpa->command, SV6621_SECURITY_KEY_GROUP,
          SV6621_SECURITY_CIPHER_CCMP, broadcast, gtk_index);
      goto clear_gtk;
    }

  if (wpa->group_installed && wpa->gtk_index != gtk_index)
    {
      sv6621_security_delete_key(
          wpa->command, SV6621_SECURITY_KEY_GROUP,
          SV6621_SECURITY_CIPHER_CCMP, broadcast, wpa->gtk_index);
    }

  wpa->gtk_index = gtk_index;
  wpa->group_installed = true;

clear_gtk:
  sv6621_wpa_clear(gtk, sizeof(gtk));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_worker
 ****************************************************************************/

static void sv6621_wpa_worker(FAR void *arg)
{
  FAR struct sv6621_wpa_s *wpa = arg;
  struct sv6621_wpa_eapol_s eapol;
  uint8_t frame[SV6621_WPA_FRAME_CAPACITY];
  enum sv6621_wpa_state_e state;
  size_t length;
  int ret;

  for (;;)
    {
      if (nxmutex_lock(&wpa->lock) < 0)
        {
          return;
        }

      if (!wpa->frame_pending || !wpa->peer_ready)
        {
          wpa->work_scheduled = false;
          nxmutex_unlock(&wpa->lock);
          return;
        }

      length = wpa->frame_length;
      memcpy(frame, wpa->frame, length);
      wpa->frame_pending = false;
      state = wpa->state;
      nxmutex_unlock(&wpa->lock);

      ret = sv6621_wpa_eapol_parse(frame, length, &eapol);
      if (ret == 0 && eapol.message == SV6621_WPA_MESSAGE_1 &&
          (state == SV6621_WPA_WAIT_MESSAGE_1 ||
           state == SV6621_WPA_WAIT_MESSAGE_3))
        {
          ret = sv6621_wpa_process_message_1(wpa, &eapol);
        }
      else if (ret == 0 && state == SV6621_WPA_WAIT_MESSAGE_3 &&
               eapol.message == SV6621_WPA_MESSAGE_3)
        {
          ret = sv6621_wpa_process_message_3(wpa, &eapol);
          if (ret == 0)
            {
              sv6621_wpa_finish(wpa, 0);
              continue;
            }
        }
      else if (ret == 0 && state == SV6621_WPA_COMPLETE &&
               eapol.message == SV6621_WPA_MESSAGE_3 &&
               sv6621_wpa_compare_replay(eapol.replay, wpa->replay) == 0 &&
               memcmp(eapol.nonce, wpa->anonce, sizeof(wpa->anonce)) == 0)
        {
          ret = sv6621_wpa_eapol_verify_mic(
              &eapol, wpa->ptk + SV6621_WPA_KCK_OFFSET);
          if (ret == 0)
            {
              ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_4);
            }
        }
      else if (ret == 0 && state == SV6621_WPA_COMPLETE &&
               eapol.message == SV6621_WPA_MESSAGE_GROUP_1)
        {
          ret = sv6621_wpa_process_group_message_1(wpa, &eapol);
        }
      else if (ret == 0)
        {
          ret = state == SV6621_WPA_COMPLETE ? -EALREADY : -EPROTO;
        }

      if (ret < 0 && ret != -EALREADY)
        {
          if (state == SV6621_WPA_COMPLETE)
            {
              sv6621_station_disconnect(wpa->station,
                                         SV6621_WPA_REASON_UNSPECIFIED);
            }

          sv6621_wpa_remove_keys(wpa);
          sv6621_wpa_finish(wpa, ret);
          continue;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_init
 ****************************************************************************/

int sv6621_wpa_init(FAR struct sv6621_wpa_s *wpa,
                     FAR struct sv6621_command_engine_s *command,
                     FAR struct sv6621_station_s *station)
{
  int ret;

  if (wpa == NULL || command == NULL || station == NULL)
    {
      return -EINVAL;
    }

  memset(wpa, 0, sizeof(*wpa));
  ret = nxmutex_init(&wpa->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxsem_init(&wpa->completion, 0, 0);
  if (ret < 0)
    {
      nxmutex_destroy(&wpa->lock);
      return ret;
    }

  wpa->command = command;
  wpa->station = station;
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_deinit
 ****************************************************************************/

void sv6621_wpa_deinit(FAR struct sv6621_wpa_s *wpa)
{
  if (wpa != NULL)
    {
      sv6621_wpa_cancel(wpa, -ESHUTDOWN);
      work_cancel_sync(LPWORK, &wpa->work);
      nxsem_destroy(&wpa->completion);
      nxmutex_destroy(&wpa->lock);
      sv6621_wpa_clear(wpa->pmk, sizeof(wpa->pmk));
      sv6621_wpa_clear(wpa->ptk, sizeof(wpa->ptk));
    }
}

/****************************************************************************
 * Name: sv6621_wpa_prepare
 ****************************************************************************/

int sv6621_wpa_prepare(FAR struct sv6621_wpa_s *wpa,
                        FAR const struct sv6621_connect_s *request,
                        FAR const uint8_t supplicant[SV6621_MAC_LENGTH],
                        FAR const uint8_t authenticator[SV6621_MAC_LENGTH])
{
  uint8_t pmk[SV6621_WPA_PMK_SIZE];
  uint8_t snonce[SV6621_WPA_NONCE_SIZE];
  int ret;

  if (wpa == NULL || request == NULL || supplicant == NULL ||
      authenticator == NULL ||
      request->security != SV6621_SECURITY_WPA2_PSK)
    {
      return -EINVAL;
    }

  ret = sv6621_wpa_derive_pmk(request->credential,
                              request->credential_length, request->ssid,
                              request->ssid_length, pmk);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_generate_nonce(snonce);
  if (ret < 0)
    {
      sv6621_wpa_clear(pmk, sizeof(pmk));
      return ret;
    }

  sv6621_wpa_cancel(wpa, -ECANCELED);
  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      sv6621_wpa_clear(pmk, sizeof(pmk));
      sv6621_wpa_clear(snonce, sizeof(snonce));
      return ret;
    }

  if (wpa->state != SV6621_WPA_IDLE && wpa->state != SV6621_WPA_COMPLETE &&
      wpa->state != SV6621_WPA_FAILED)
    {
      nxmutex_unlock(&wpa->lock);
      sv6621_wpa_clear(pmk, sizeof(pmk));
      sv6621_wpa_clear(snonce, sizeof(snonce));
      return -EBUSY;
    }

  nxsem_reset(&wpa->completion, 0);
  sv6621_wpa_clear(wpa->ptk, sizeof(wpa->ptk));
  memcpy(wpa->pmk, pmk, sizeof(wpa->pmk));
  memcpy(wpa->snonce, snonce, sizeof(wpa->snonce));
  memcpy(wpa->supplicant, supplicant, sizeof(wpa->supplicant));
  memcpy(wpa->authenticator, authenticator, sizeof(wpa->authenticator));
  memset(wpa->replay, 0, sizeof(wpa->replay));
  wpa->result = -EINPROGRESS;
  wpa->eapol_version = 0;
  wpa->peer_ready = false;
  wpa->frame_pending = false;
  wpa->state = SV6621_WPA_WAIT_MESSAGE_1;
  nxmutex_unlock(&wpa->lock);
  sv6621_wpa_clear(pmk, sizeof(pmk));
  sv6621_wpa_clear(snonce, sizeof(snonce));
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_run
 ****************************************************************************/

int sv6621_wpa_run(FAR struct sv6621_wpa_s *wpa,
                    FAR const struct sv6621_connection_peer_s *peer,
                    uint32_t timeout_ms)
{
  int ret;

  if (wpa == NULL || peer == NULL || timeout_ms == 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (wpa->state != SV6621_WPA_WAIT_MESSAGE_1)
    {
      nxmutex_unlock(&wpa->lock);
      return -EINVAL;
    }

  if (wpa->canceling)
    {
      nxmutex_unlock(&wpa->lock);
      return -EBUSY;
    }

  wpa->tx_context.peer_index = peer->peer_index;
  wpa->tx_context.multicast_index = peer->multicast_index;
  wpa->tx_context.instance = peer->instance;
  wpa->tx_context.lmac_id = peer->lmac_id;
  wpa->tx_context.tid = 0;
  wpa->peer_ready = true;
  ret = sv6621_wpa_schedule_locked(wpa);
  if (ret < 0)
    {
      nxmutex_unlock(&wpa->lock);
      return ret;
    }

  nxmutex_unlock(&wpa->lock);
  ret = nxsem_tickwait(&wpa->completion, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      if (nxmutex_lock(&wpa->lock) == 0)
        {
          if (wpa->state == SV6621_WPA_COMPLETE)
            {
              ret = wpa->result;
              nxmutex_unlock(&wpa->lock);
              return ret;
            }

          nxmutex_unlock(&wpa->lock);
        }

      sv6621_wpa_cancel(wpa, -ETIMEDOUT);
      return -ETIMEDOUT;
    }

  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return -EINTR;
    }

  ret = wpa->result;
  nxmutex_unlock(&wpa->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_cancel
 ****************************************************************************/

void sv6621_wpa_cancel(FAR struct sv6621_wpa_s *wpa, int result)
{
  bool pending;

  if (wpa == NULL)
    {
      return;
    }

  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  pending = wpa->state == SV6621_WPA_WAIT_MESSAGE_1 ||
            wpa->state == SV6621_WPA_WAIT_MESSAGE_3;
  wpa->canceling = true;
  wpa->peer_ready = false;
  wpa->frame_pending = false;
  nxmutex_unlock(&wpa->lock);

  work_cancel_sync(LPWORK, &wpa->work);
  sv6621_wpa_remove_keys(wpa);
  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  wpa->state = SV6621_WPA_IDLE;
  wpa->result = result;
  wpa->peer_ready = false;
  wpa->frame_pending = false;
  wpa->work_scheduled = false;
  wpa->canceling = false;
  wpa->gtk_index = 0;
  sv6621_wpa_clear(wpa->pmk, sizeof(wpa->pmk));
  sv6621_wpa_clear(wpa->ptk, sizeof(wpa->ptk));
  nxmutex_unlock(&wpa->lock);
  if (pending)
    {
      nxsem_post(&wpa->completion);
    }
}

/****************************************************************************
 * Name: sv6621_wpa_input
 ****************************************************************************/

void sv6621_wpa_input(FAR const struct sv6621_data_rx_s *rx, FAR void *arg)
{
  FAR struct sv6621_wpa_s *wpa = arg;
  int ret = 0;

  if (wpa == NULL || rx == NULL || rx->frame_length > sizeof(wpa->frame) ||
      nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  if ((wpa->state == SV6621_WPA_WAIT_MESSAGE_1 ||
       wpa->state == SV6621_WPA_WAIT_MESSAGE_3 ||
       wpa->state == SV6621_WPA_COMPLETE) &&
      !wpa->frame_pending && !wpa->canceling)
    {
      memcpy(wpa->frame, rx->frame, rx->frame_length);
      wpa->frame_length = rx->frame_length;
      wpa->frame_pending = true;
      ret = sv6621_wpa_schedule_locked(wpa);
    }

  nxmutex_unlock(&wpa->lock);
  if (ret < 0)
    {
      sv6621_station_disconnect(wpa->station,
                                SV6621_WPA_REASON_UNSPECIFIED);
      sv6621_wpa_remove_keys(wpa);
      sv6621_wpa_finish(wpa, ret);
    }
}
