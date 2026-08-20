/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_wpa.c
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

#include <sys/random.h>

#include <errno.h>
#include <string.h>

#include "sv6621_ap.h"
#include "sv6621_ap_wpa.h"
#include "sv6621_security.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_WPA_ETHERNET_HEADER_SIZE 14
#define SV6621_AP_WPA_ETHERTYPE_EAPOL      0x888e
#define SV6621_AP_WPA_EAPOL_VERSION        2
#define SV6621_AP_WPA_KCK_OFFSET           0
#define SV6621_AP_WPA_KEK_OFFSET           16
#define SV6621_AP_WPA_TK_OFFSET            32
#define SV6621_AP_WPA_KEY_SIZE             16
#define SV6621_AP_WPA_FRAME_CAPACITY       640

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_wpa_clear(FAR void *data, size_t length);
static int sv6621_ap_wpa_random(FAR uint8_t *data, size_t length);
static FAR struct sv6621_ap_wpa_peer_s *sv6621_ap_wpa_find(
    FAR struct sv6621_ap_wpa_s *wpa,
    FAR const uint8_t address[SV6621_MAC_LENGTH]);
static void sv6621_ap_wpa_increment_replay(
    FAR uint8_t replay[SV6621_WPA_REPLAY_SIZE]);
static int sv6621_ap_wpa_send(FAR struct sv6621_ap_wpa_s *wpa,
                              FAR struct sv6621_ap_wpa_peer_s *peer,
                              enum sv6621_wpa_message_e message,
                              FAR const uint8_t *key_data,
                              size_t key_data_length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_ap_wpa_clear
 ****************************************************************************/

static void sv6621_ap_wpa_clear(FAR void *data, size_t length)
{
  volatile uint8_t *bytes = data;

  while (length-- != 0)
    {
      *bytes++ = 0;
    }
}

/****************************************************************************
 * Name: sv6621_ap_wpa_random
 ****************************************************************************/

static int sv6621_ap_wpa_random(FAR uint8_t *data, size_t length)
{
  ssize_t result = getrandom(data, length, 0);

  if (result < 0)
    {
      return -errno;
    }

  return (size_t)result == length ? 0 : -EIO;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_find
 ****************************************************************************/

static FAR struct sv6621_ap_wpa_peer_s *sv6621_ap_wpa_find(
    FAR struct sv6621_ap_wpa_s *wpa,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  size_t index;

  for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
    {
      if (wpa->peers[index].state != SV6621_AP_WPA_IDLE &&
          memcmp(wpa->peers[index].address, address,
                 SV6621_MAC_LENGTH) == 0)
        {
          return &wpa->peers[index];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_increment_replay
 ****************************************************************************/

static void sv6621_ap_wpa_increment_replay(
    FAR uint8_t replay[SV6621_WPA_REPLAY_SIZE])
{
  size_t index = SV6621_WPA_REPLAY_SIZE;

  while (index-- != 0 && ++replay[index] == 0)
    {
    }
}

/****************************************************************************
 * Name: sv6621_ap_wpa_send
 ****************************************************************************/

static int sv6621_ap_wpa_send(FAR struct sv6621_ap_wpa_s *wpa,
                              FAR struct sv6621_ap_wpa_peer_s *peer,
                              enum sv6621_wpa_message_e message,
                              FAR const uint8_t *key_data,
                              size_t key_data_length)
{
  struct sv6621_data_tx_context_s tx;
  uint8_t frame[SV6621_AP_WPA_FRAME_CAPACITY];
  size_t eapol_length;
  int ret;

  memcpy(frame, peer->address, SV6621_MAC_LENGTH);
  memcpy(frame + SV6621_MAC_LENGTH, wpa->authenticator,
         SV6621_MAC_LENGTH);
  frame[12] = SV6621_AP_WPA_ETHERTYPE_EAPOL >> 8;
  frame[13] = SV6621_AP_WPA_ETHERTYPE_EAPOL & 0xff;
  ret = sv6621_wpa_eapol_build_authenticator(
      message, SV6621_WPA_KEY_MGMT_PSK, SV6621_AP_WPA_EAPOL_VERSION,
      peer->replay, peer->anonce,
      message == SV6621_WPA_MESSAGE_3 ?
          peer->ptk + SV6621_AP_WPA_KCK_OFFSET : NULL,
      message == SV6621_WPA_MESSAGE_3 ?
          peer->ptk + SV6621_AP_WPA_KEK_OFFSET : NULL,
      key_data, key_data_length,
      frame + SV6621_AP_WPA_ETHERNET_HEADER_SIZE,
      sizeof(frame) - SV6621_AP_WPA_ETHERNET_HEADER_SIZE,
      &eapol_length);
  if (ret < 0)
    {
      return ret;
    }

  tx.peer_index = peer->peer_index;
  tx.multicast_index = wpa->multicast_index;
  tx.instance = wpa->instance;
  tx.lmac_id = wpa->lmac_id;
  tx.tid = 0;
  return sv6621_security_send_eapol_instance(
      wpa->command, wpa->instance, &tx, frame,
      SV6621_AP_WPA_ETHERNET_HEADER_SIZE + eapol_length);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_ap_wpa_init
 ****************************************************************************/

int sv6621_ap_wpa_init(FAR struct sv6621_ap_wpa_s *wpa,
                        FAR struct sv6621_command_engine_s *command,
                        FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  int ret;

  if (wpa == NULL || command == NULL || address == NULL)
    {
      return -EINVAL;
    }

  memset(wpa, 0, sizeof(*wpa));
  ret = nxmutex_init(&wpa->lock);
  if (ret == 0)
    {
      wpa->command = command;
      memcpy(wpa->authenticator, address, SV6621_MAC_LENGTH);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_deinit
 ****************************************************************************/

void sv6621_ap_wpa_deinit(FAR struct sv6621_ap_wpa_s *wpa)
{
  if (wpa != NULL)
    {
      sv6621_ap_wpa_disable(wpa);
      nxmutex_destroy(&wpa->lock);
      sv6621_ap_wpa_clear(wpa, sizeof(*wpa));
    }
}

/****************************************************************************
 * Name: sv6621_ap_wpa_enable
 ****************************************************************************/

int sv6621_ap_wpa_enable(FAR struct sv6621_ap_wpa_s *wpa,
                          FAR const struct sv6621_ap_config_s *config,
                          FAR const struct sv6621_ap_context_s *context)
{
  static const uint8_t broadcast[SV6621_MAC_LENGTH] =
    { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
  uint8_t pmk[SV6621_WPA_PMK_SIZE];
  uint8_t gtk[SV6621_AP_WPA_KEY_SIZE];
  int ret;

  if (wpa == NULL || config == NULL || context == NULL ||
      config->security != SV6621_SECURITY_WPA2_PSK)
    {
      return -EINVAL;
    }

  ret = sv6621_wpa_derive_pmk(config->credential,
                              config->credential_length, config->ssid,
                              config->ssid_length, pmk);
  if (ret == 0)
    {
      ret = sv6621_ap_wpa_random(gtk, sizeof(gtk));
    }

  if (ret == 0)
    {
      ret = sv6621_security_add_key_instance(
          wpa->command, context->instance, SV6621_SECURITY_KEY_GROUP,
          SV6621_SECURITY_CIPHER_CCMP, broadcast, 1, gtk, sizeof(gtk),
          NULL);
    }

  if (ret == 0)
    {
      ret = nxmutex_lock(&wpa->lock);
      if (ret == 0)
        {
          memset(wpa->peers, 0, sizeof(wpa->peers));
          memcpy(wpa->pmk, pmk, sizeof(wpa->pmk));
          memcpy(wpa->gtk, gtk, sizeof(wpa->gtk));
          wpa->gtk_index = 1;
          wpa->lmac_id = context->lmac_id;
          wpa->instance = context->instance;
          wpa->multicast_index = context->multicast_index;
          wpa->enabled = true;
          nxmutex_unlock(&wpa->lock);
        }
    }

  sv6621_ap_wpa_clear(pmk, sizeof(pmk));
  sv6621_ap_wpa_clear(gtk, sizeof(gtk));
  return ret;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_disable
 ****************************************************************************/

void sv6621_ap_wpa_disable(FAR struct sv6621_ap_wpa_s *wpa)
{
  if (wpa != NULL && nxmutex_lock(&wpa->lock) == 0)
    {
      sv6621_ap_wpa_clear(wpa->peers, sizeof(wpa->peers));
      sv6621_ap_wpa_clear(wpa->pmk, sizeof(wpa->pmk));
      sv6621_ap_wpa_clear(wpa->gtk, sizeof(wpa->gtk));
      wpa->lmac_id = 0;
      wpa->instance = 0;
      wpa->multicast_index = 0;
      wpa->gtk_index = 0;
      wpa->enabled = false;
      nxmutex_unlock(&wpa->lock);
    }
}

/****************************************************************************
 * Name: sv6621_ap_wpa_begin
 ****************************************************************************/

int sv6621_ap_wpa_begin(FAR struct sv6621_ap_wpa_s *wpa,
                         FAR const struct sv6621_ap_peer_s *peer)
{
  FAR struct sv6621_ap_wpa_peer_s *entry = NULL;
  size_t index;
  int ret;

  if (wpa == NULL || peer == NULL || !peer->bound)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!wpa->enabled)
    {
      nxmutex_unlock(&wpa->lock);
      return -ENETDOWN;
    }

  entry = sv6621_ap_wpa_find(wpa, peer->address);
  if (entry == NULL)
    {
      for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
        {
          if (wpa->peers[index].state == SV6621_AP_WPA_IDLE)
            {
              entry = &wpa->peers[index];
              break;
            }
        }
    }

  if (entry == NULL)
    {
      nxmutex_unlock(&wpa->lock);
      return -ENOSPC;
    }

  sv6621_ap_wpa_clear(entry, sizeof(*entry));
  memcpy(entry->address, peer->address, SV6621_MAC_LENGTH);
  entry->peer_index = peer->peer_index;
  entry->replay[SV6621_WPA_REPLAY_SIZE - 1] = 1;
  ret = sv6621_ap_wpa_random(entry->anonce, sizeof(entry->anonce));
  if (ret == 0)
    {
      ret = sv6621_ap_wpa_send(wpa, entry, SV6621_WPA_MESSAGE_1, NULL, 0);
    }

  entry->state = ret == 0 ? SV6621_AP_WPA_WAIT_MESSAGE_2 :
                            SV6621_AP_WPA_IDLE;
  nxmutex_unlock(&wpa->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_forget
 ****************************************************************************/

void sv6621_ap_wpa_forget(
    FAR struct sv6621_ap_wpa_s *wpa,
    FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_wpa_peer_s *peer;

  if (wpa != NULL && address != NULL && nxmutex_lock(&wpa->lock) == 0)
    {
      peer = sv6621_ap_wpa_find(wpa, address);
      if (peer != NULL)
        {
          sv6621_ap_wpa_clear(peer, sizeof(*peer));
        }

      nxmutex_unlock(&wpa->lock);
    }
}

/****************************************************************************
 * Name: sv6621_ap_wpa_input
 ****************************************************************************/

int sv6621_ap_wpa_input(FAR struct sv6621_ap_wpa_s *wpa,
                         FAR const struct sv6621_data_rx_s *rx,
                         FAR bool *authorized,
                         FAR uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_wpa_peer_s *peer;
  struct sv6621_wpa_eapol_s eapol;
  uint8_t key_data[32];
  size_t key_data_length;
  int ret;

  if (wpa == NULL || rx == NULL || authorized == NULL || address == NULL ||
      rx->frame == NULL || rx->frame_length <
      SV6621_AP_WPA_ETHERNET_HEADER_SIZE)
    {
      return -EINVAL;
    }

  *authorized = false;
  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!wpa->enabled || rx->instance != wpa->instance ||
      memcmp(rx->frame, wpa->authenticator, SV6621_MAC_LENGTH) != 0)
    {
      nxmutex_unlock(&wpa->lock);
      return -ENOENT;
    }

  peer = sv6621_ap_wpa_find(wpa, rx->frame + SV6621_MAC_LENGTH);
  if (peer == NULL || !rx->peer_valid || rx->peer_index != peer->peer_index)
    {
      nxmutex_unlock(&wpa->lock);
      return -ENOENT;
    }

  ret = sv6621_wpa_eapol_parse(rx->frame, rx->frame_length,
                                SV6621_WPA_KEY_MGMT_PSK, &eapol);
  if (ret == 0 && memcmp(eapol.replay, peer->replay,
                          SV6621_WPA_REPLAY_SIZE) != 0)
    {
      ret = -EALREADY;
    }

  if (ret == 0 && peer->state == SV6621_AP_WPA_WAIT_MESSAGE_2 &&
      eapol.message == SV6621_WPA_MESSAGE_2)
    {
      ret = sv6621_wpa_derive_ptk(
          wpa->pmk, wpa->authenticator, peer->address, peer->anonce,
          eapol.nonce, peer->ptk);
      if (ret == 0)
        {
          ret = sv6621_wpa_eapol_verify_mic(
              &eapol, SV6621_WPA_KEY_MGMT_PSK,
              peer->ptk + SV6621_AP_WPA_KCK_OFFSET);
        }

      if (ret == 0)
        {
          ret = sv6621_security_add_key_instance(
              wpa->command, wpa->instance,
              SV6621_SECURITY_KEY_PAIRWISE, SV6621_SECURITY_CIPHER_CCMP,
              peer->address, 0, peer->ptk + SV6621_AP_WPA_TK_OFFSET,
              SV6621_AP_WPA_KEY_SIZE, NULL);
        }

      if (ret == 0)
        {
          ret = sv6621_wpa_eapol_build_gtk_kde(
              wpa->gtk_index, wpa->gtk, sizeof(wpa->gtk), key_data,
              sizeof(key_data), &key_data_length);
        }

      if (ret == 0)
        {
          sv6621_ap_wpa_increment_replay(peer->replay);
          ret = sv6621_ap_wpa_send(wpa, peer, SV6621_WPA_MESSAGE_3,
                                    key_data, key_data_length);
        }

      if (ret == 0)
        {
          peer->state = SV6621_AP_WPA_WAIT_MESSAGE_4;
        }
    }
  else if (ret == 0 && peer->state == SV6621_AP_WPA_WAIT_MESSAGE_4 &&
           eapol.message == SV6621_WPA_MESSAGE_4)
    {
      ret = sv6621_wpa_eapol_verify_mic(
          &eapol, SV6621_WPA_KEY_MGMT_PSK,
          peer->ptk + SV6621_AP_WPA_KCK_OFFSET);
      if (ret == 0)
        {
          peer->state = SV6621_AP_WPA_COMPLETE;
          memcpy(address, peer->address, SV6621_MAC_LENGTH);
          *authorized = true;
        }
    }
  else if (ret == 0)
    {
      ret = -EPROTO;
    }

  nxmutex_unlock(&wpa->lock);
  return ret;
}
