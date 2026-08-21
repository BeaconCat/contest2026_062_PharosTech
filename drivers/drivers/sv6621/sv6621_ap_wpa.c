/****************************************************************************
 * drivers/drivers/sv6621/sv6621_ap_wpa.c
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
#include "sv6621_ap_beacon.h"
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
#define SV6621_AP_WPA_REKEY_TIMEOUT_MS     1000
#define SV6621_AP_WPA_REKEY_RETRY_LIMIT    3

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_wpa_clear(FAR void *data, size_t length);
static int sv6621_ap_wpa_random(FAR uint8_t *data, size_t length);
static FAR struct sv6621_ap_wpa_peer_s *
sv6621_ap_wpa_find(FAR struct sv6621_ap_wpa_s *wpa,
                   FAR const uint8_t address[SV6621_MAC_LENGTH]);
static void
sv6621_ap_wpa_increment_replay(FAR uint8_t replay[SV6621_WPA_REPLAY_SIZE]);
static bool
sv6621_ap_wpa_rekey_complete(FAR const struct sv6621_ap_wpa_s *wpa);
static int
sv6621_ap_wpa_build_group_key_data(FAR const struct sv6621_ap_wpa_s *wpa,
                                   FAR uint8_t *key_data, size_t capacity,
                                   FAR size_t *length);
static void sv6621_ap_wpa_rekey_timeout_worker(FAR void *arg);
static int sv6621_ap_wpa_send(FAR struct sv6621_ap_wpa_s *wpa,
                              FAR struct sv6621_ap_wpa_peer_s *peer,
                              enum sv6621_wpa_message_e message,
                              FAR const uint8_t *key_data,
                              size_t key_data_length);
static int sv6621_ap_wpa_begin_key(FAR struct sv6621_ap_wpa_s *wpa,
                                   FAR const struct sv6621_ap_peer_s *peer,
                                   FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
                                   enum sv6621_wpa_key_mgmt_e key_mgmt);

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

static FAR struct sv6621_ap_wpa_peer_s *
sv6621_ap_wpa_find(FAR struct sv6621_ap_wpa_s *wpa,
                   FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  size_t index;

  for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
    {
      if (wpa->peers[index].state != SV6621_AP_WPA_IDLE &&
          memcmp(wpa->peers[index].address, address, SV6621_MAC_LENGTH) == 0)
        {
          return &wpa->peers[index];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_increment_replay
 ****************************************************************************/

static void
sv6621_ap_wpa_increment_replay(FAR uint8_t replay[SV6621_WPA_REPLAY_SIZE])
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
  memcpy(frame + SV6621_MAC_LENGTH, wpa->authenticator, SV6621_MAC_LENGTH);
  frame[12] = SV6621_AP_WPA_ETHERTYPE_EAPOL >> 8;
  frame[13] = SV6621_AP_WPA_ETHERTYPE_EAPOL & 0xff;
  ret = sv6621_wpa_eapol_build_authenticator(
      message, peer->key_mgmt, SV6621_AP_WPA_EAPOL_VERSION, peer->replay,
      peer->anonce,
      (message == SV6621_WPA_MESSAGE_3 ||
       message == SV6621_WPA_MESSAGE_GROUP_1)
          ? peer->ptk + SV6621_AP_WPA_KCK_OFFSET
          : NULL,
      (message == SV6621_WPA_MESSAGE_3 ||
       message == SV6621_WPA_MESSAGE_GROUP_1)
          ? peer->ptk + SV6621_AP_WPA_KEK_OFFSET
          : NULL,
      key_data, key_data_length, frame + SV6621_AP_WPA_ETHERNET_HEADER_SIZE,
      sizeof(frame) - SV6621_AP_WPA_ETHERNET_HEADER_SIZE, &eapol_length);
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

static bool sv6621_ap_wpa_rekey_complete(FAR const struct sv6621_ap_wpa_s *wpa)
{
  size_t index;

  for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
    {
      if (wpa->peers[index].state == SV6621_AP_WPA_COMPLETE &&
          wpa->peers[index].group_rekey_pending)
        {
          return false;
        }
    }

  return true;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_build_group_key_data
 ****************************************************************************/

static int
sv6621_ap_wpa_build_group_key_data(FAR const struct sv6621_ap_wpa_s *wpa,
                                   FAR uint8_t *key_data, size_t capacity,
                                   FAR size_t *length)
{
  size_t gtk_kde_length;
  size_t igtk_kde_length;
  int ret;

  ret = sv6621_wpa_eapol_build_gtk_kde(wpa->gtk_index, wpa->gtk,
                                       sizeof(wpa->gtk), key_data, capacity,
                                       &gtk_kde_length);
  if (ret < 0)
    {
      return ret;
    }

  *length = gtk_kde_length;
  if (wpa->pmf_enabled)
    {
      ret = sv6621_wpa_eapol_build_igtk_kde(
          wpa->igtk_index, wpa->igtk_ipn, wpa->igtk, key_data + *length,
          capacity - *length, &igtk_kde_length);
      if (ret < 0)
        {
          return ret;
        }

      *length += igtk_kde_length;
    }

  while ((*length & 7) != 0)
    {
      if (*length >= capacity)
        {
          return -ENOSPC;
        }

      key_data[(*length)++] = 0;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_rekey_timeout_worker
 ****************************************************************************/

static void sv6621_ap_wpa_rekey_timeout_worker(FAR void *arg)
{
  FAR struct sv6621_ap_wpa_s *wpa = arg;
  uint8_t key_data[128];
  size_t key_data_length;
  size_t index;
  bool failed = false;
  int ret;

  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      return;
    }

  if (!wpa->enabled || !wpa->group_rekey_active)
    {
      nxmutex_unlock(&wpa->lock);
      return;
    }

  if (ret == 0)
    {
      ret = sv6621_ap_wpa_build_group_key_data(wpa, key_data, sizeof(key_data),
                                               &key_data_length);
    }
  for (index = 0; ret == 0 && index < SV6621_AP_WPA_PEER_CAPACITY; index++)
    {
      FAR struct sv6621_ap_wpa_peer_s *peer = &wpa->peers[index];

      if (peer->state == SV6621_AP_WPA_COMPLETE && peer->group_rekey_pending)
        {
          sv6621_ap_wpa_increment_replay(peer->replay);
          ret = sv6621_ap_wpa_send(wpa, peer, SV6621_WPA_MESSAGE_GROUP_1,
                                   key_data, key_data_length);
        }
    }

  if (ret == 0 && ++wpa->group_rekey_retries < SV6621_AP_WPA_REKEY_RETRY_LIMIT)
    {
      ret = work_queue_next(LPWORK, &wpa->rekey_timeout_work,
                            sv6621_ap_wpa_rekey_timeout_worker, wpa,
                            MSEC2TICK(SV6621_AP_WPA_REKEY_TIMEOUT_MS));
    }

  if (ret < 0 || wpa->group_rekey_retries >= SV6621_AP_WPA_REKEY_RETRY_LIMIT)
    {
      wpa->group_rekey_active = false;
      for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
        {
          wpa->peers[index].group_rekey_pending = false;
        }

      failed = true;
    }

  sv6621_ap_wpa_clear(key_data, sizeof(key_data));
  nxmutex_unlock(&wpa->lock);
  if (failed && wpa->error != NULL)
    {
      wpa->error(ret < 0 ? ret : -ETIMEDOUT, wpa->error_arg);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_ap_wpa_init
 ****************************************************************************/

int sv6621_ap_wpa_init(FAR struct sv6621_ap_wpa_s *wpa,
                       FAR struct sv6621_command_engine_s *command,
                       FAR const uint8_t address[SV6621_MAC_LENGTH],
                       sv6621_ap_wpa_error_t error, FAR void *error_arg)
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
      wpa->error = error;
      wpa->error_arg = error_arg;
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
  static const uint8_t broadcast[SV6621_MAC_LENGTH] = { 0xff, 0xff, 0xff,
                                                        0xff, 0xff, 0xff };
  uint8_t pmk[SV6621_WPA_PMK_SIZE];
  uint8_t gtk[SV6621_AP_WPA_KEY_SIZE];
  uint8_t igtk[SV6621_WPA_IGTK_SIZE];
  int ret;

  if (wpa == NULL || config == NULL || context == NULL ||
      (config->security != SV6621_SECURITY_WPA2_PSK &&
       config->security != SV6621_SECURITY_WPA3_SAE &&
       config->security != SV6621_SECURITY_WPA2_WPA3_PSK))
    {
      return -EINVAL;
    }

  memset(pmk, 0, sizeof(pmk));
  if (config->security == SV6621_SECURITY_WPA3_SAE)
    {
      ret = 0;
    }
  else
    {
      ret =
          sv6621_wpa_derive_pmk(config->credential, config->credential_length,
                                config->ssid, config->ssid_length, pmk);
    }
  if (ret == 0)
    {
      ret = sv6621_ap_wpa_random(gtk, sizeof(gtk));
    }

  if (ret == 0 && config->security != SV6621_SECURITY_WPA2_PSK)
    {
      ret = sv6621_ap_wpa_random(igtk, sizeof(igtk));
    }

  if (ret == 0)
    {
      ret = sv6621_security_add_key_instance(
          wpa->command, context->instance, SV6621_SECURITY_KEY_GROUP,
          SV6621_SECURITY_CIPHER_CCMP, broadcast, 1, gtk, sizeof(gtk), NULL);
    }

  if (ret == 0 && config->security != SV6621_SECURITY_WPA2_PSK)
    {
      ret = sv6621_security_add_key_instance(
          wpa->command, context->instance, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
          SV6621_SECURITY_CIPHER_BIP_CMAC_128, broadcast, 4, igtk,
          sizeof(igtk), NULL);
    }

  if (ret == 0)
    {
      ret = nxmutex_lock(&wpa->lock);
      if (ret == 0)
        {
          memset(wpa->peers, 0, sizeof(wpa->peers));
          memcpy(wpa->pmk, pmk, sizeof(wpa->pmk));
          memcpy(wpa->gtk, gtk, sizeof(wpa->gtk));
          memcpy(wpa->igtk, igtk, sizeof(wpa->igtk));
          memset(wpa->igtk_ipn, 0, sizeof(wpa->igtk_ipn));
          wpa->gtk_index = 1;
          wpa->previous_gtk_index = 0;
          wpa->igtk_index = 4;
          wpa->previous_igtk_index = 0;
          wpa->group_rekey_active = false;
          wpa->lmac_id = context->lmac_id;
          wpa->instance = context->instance;
          wpa->multicast_index = context->multicast_index;
          wpa->security = config->security;
          wpa->pmf_enabled = config->security != SV6621_SECURITY_WPA2_PSK;
          wpa->enabled = true;
          nxmutex_unlock(&wpa->lock);
        }
    }

  sv6621_ap_wpa_clear(pmk, sizeof(pmk));
  sv6621_ap_wpa_clear(gtk, sizeof(gtk));
  sv6621_ap_wpa_clear(igtk, sizeof(igtk));
  return ret;
}

int sv6621_ap_wpa_rekey(FAR struct sv6621_ap_wpa_s *wpa)
{
  static const uint8_t broadcast[SV6621_MAC_LENGTH] = { 0xff, 0xff, 0xff,
                                                        0xff, 0xff, 0xff };
  uint8_t key_data[128];
  uint8_t gtk[sizeof(wpa->gtk)];
  uint8_t igtk[sizeof(wpa->igtk)];
  size_t key_data_length;
  size_t index;
  int ret;

  if (wpa == NULL || nxmutex_lock(&wpa->lock) < 0)
    {
      return -EINVAL;
    }

  if (!wpa->enabled || wpa->group_rekey_active)
    {
      nxmutex_unlock(&wpa->lock);
      return wpa->enabled ? -EBUSY : -ENETDOWN;
    }

  ret = sv6621_ap_wpa_random(gtk, sizeof(gtk));
  if (ret == 0 && wpa->pmf_enabled)
    {
      ret = sv6621_ap_wpa_random(igtk, sizeof(igtk));
    }

  if (ret < 0)
    {
      nxmutex_unlock(&wpa->lock);
      return ret;
    }

  memcpy(wpa->previous_gtk, wpa->gtk, sizeof(wpa->gtk));
  wpa->previous_gtk_index = wpa->gtk_index;
  wpa->gtk_index = wpa->gtk_index >= 3 ? 1 : wpa->gtk_index + 1;
  memcpy(wpa->gtk, gtk, sizeof(wpa->gtk));
  ret = sv6621_security_add_key_instance(
      wpa->command, wpa->instance, SV6621_SECURITY_KEY_GROUP,
      SV6621_SECURITY_CIPHER_CCMP, broadcast, wpa->gtk_index, wpa->gtk,
      sizeof(wpa->gtk), NULL);
  if (ret < 0)
    {
      memcpy(wpa->gtk, wpa->previous_gtk, sizeof(wpa->gtk));
      wpa->gtk_index = wpa->previous_gtk_index;
      sv6621_ap_wpa_clear(gtk, sizeof(gtk));
      nxmutex_unlock(&wpa->lock);
      return ret;
    }

  if (wpa->pmf_enabled)
    {
      memcpy(wpa->previous_igtk, wpa->igtk, sizeof(wpa->igtk));
      wpa->previous_igtk_index = wpa->igtk_index;
      wpa->igtk_index = wpa->igtk_index == 4 ? 5 : 4;
      memcpy(wpa->igtk, igtk, sizeof(wpa->igtk));
      memset(wpa->igtk_ipn, 0, sizeof(wpa->igtk_ipn));
      ret = sv6621_security_add_key_instance(
          wpa->command, wpa->instance, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
          SV6621_SECURITY_CIPHER_BIP_CMAC_128, broadcast, wpa->igtk_index,
          wpa->igtk, sizeof(wpa->igtk), NULL);
      if (ret < 0)
        {
          memcpy(wpa->igtk, wpa->previous_igtk, sizeof(wpa->igtk));
          wpa->igtk_index = wpa->previous_igtk_index;
        }
    }

  if (ret == 0)
    {
      ret = sv6621_ap_wpa_build_group_key_data(wpa, key_data, sizeof(key_data),
                                               &key_data_length);
    }
  if (ret == 0)
    {
      wpa->group_rekey_active = true;
      wpa->group_rekey_retries = 0;
      for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
        {
          FAR struct sv6621_ap_wpa_peer_s *peer = &wpa->peers[index];

          if (peer->state == SV6621_AP_WPA_COMPLETE)
            {
              sv6621_ap_wpa_increment_replay(peer->replay);
              ret = sv6621_ap_wpa_send(wpa, peer, SV6621_WPA_MESSAGE_GROUP_1,
                                       key_data, key_data_length);
              if (ret < 0)
                {
                  break;
                }

              peer->group_rekey_pending = true;
            }
        }
    }

  if (ret < 0)
    {
      memcpy(wpa->gtk, wpa->previous_gtk, sizeof(wpa->gtk));
      wpa->gtk_index = wpa->previous_gtk_index;
      if (wpa->pmf_enabled)
        {
          memcpy(wpa->igtk, wpa->previous_igtk, sizeof(wpa->igtk));
          wpa->igtk_index = wpa->previous_igtk_index;
        }
      wpa->group_rekey_active = false;
      for (index = 0; index < SV6621_AP_WPA_PEER_CAPACITY; index++)
        {
          wpa->peers[index].group_rekey_pending = false;
        }
    }
  else if (sv6621_ap_wpa_rekey_complete(wpa))
    {
      wpa->group_rekey_active = false;
    }
  else
    {
      ret = work_queue(LPWORK, &wpa->rekey_timeout_work,
                       sv6621_ap_wpa_rekey_timeout_worker, wpa,
                       MSEC2TICK(SV6621_AP_WPA_REKEY_TIMEOUT_MS));
    }

  sv6621_ap_wpa_clear(gtk, sizeof(gtk));
  sv6621_ap_wpa_clear(igtk, sizeof(igtk));
  nxmutex_unlock(&wpa->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_disable
 ****************************************************************************/

void sv6621_ap_wpa_disable(FAR struct sv6621_ap_wpa_s *wpa)
{
  if (wpa != NULL)
    {
      work_cancel_sync(LPWORK, &wpa->rekey_timeout_work);
    }

  if (wpa != NULL && nxmutex_lock(&wpa->lock) == 0)
    {
      sv6621_ap_wpa_clear(wpa->peers, sizeof(wpa->peers));
      sv6621_ap_wpa_clear(wpa->pmk, sizeof(wpa->pmk));
      sv6621_ap_wpa_clear(wpa->gtk, sizeof(wpa->gtk));
      sv6621_ap_wpa_clear(wpa->igtk, sizeof(wpa->igtk));
      sv6621_ap_wpa_clear(wpa->previous_igtk, sizeof(wpa->previous_igtk));
      wpa->lmac_id = 0;
      wpa->instance = 0;
      wpa->multicast_index = 0;
      wpa->gtk_index = 0;
      wpa->igtk_index = 0;
      wpa->pmf_enabled = false;
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
  if (wpa == NULL)
    {
      return -EINVAL;
    }

  return sv6621_ap_wpa_begin_key(wpa, peer, wpa->pmk, SV6621_WPA_KEY_MGMT_PSK);
}

/****************************************************************************
 * Name: sv6621_ap_wpa_begin_pmk
 ****************************************************************************/

int sv6621_ap_wpa_begin_pmk(FAR struct sv6621_ap_wpa_s *wpa,
                            FAR const struct sv6621_ap_peer_s *peer,
                            FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE])
{
  return sv6621_ap_wpa_begin_key(wpa, peer, pmk, SV6621_WPA_KEY_MGMT_SAE);
}

static int sv6621_ap_wpa_begin_key(FAR struct sv6621_ap_wpa_s *wpa,
                                   FAR const struct sv6621_ap_peer_s *peer,
                                   FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
                                   enum sv6621_wpa_key_mgmt_e key_mgmt)
{
  FAR struct sv6621_ap_wpa_peer_s *entry = NULL;
  size_t index;
  int ret;

  if (wpa == NULL || peer == NULL || pmk == NULL || !peer->bound)
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
  memcpy(entry->pmk, pmk, sizeof(entry->pmk));
  entry->key_mgmt = key_mgmt;
  entry->peer_index = peer->peer_index;
  entry->replay[SV6621_WPA_REPLAY_SIZE - 1] = 1;
  ret = sv6621_ap_wpa_random(entry->anonce, sizeof(entry->anonce));
  if (ret == 0)
    {
      ret = sv6621_ap_wpa_send(wpa, entry, SV6621_WPA_MESSAGE_1, NULL, 0);
    }

  entry->state = ret == 0 ? SV6621_AP_WPA_WAIT_MESSAGE_2 : SV6621_AP_WPA_IDLE;
  nxmutex_unlock(&wpa->lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_ap_wpa_forget
 ****************************************************************************/

void sv6621_ap_wpa_forget(FAR struct sv6621_ap_wpa_s *wpa,
                          FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  FAR struct sv6621_ap_wpa_peer_s *peer;

  if (wpa != NULL && address != NULL && nxmutex_lock(&wpa->lock) == 0)
    {
      peer = sv6621_ap_wpa_find(wpa, address);
      if (peer != NULL)
        {
          sv6621_ap_wpa_clear(peer, sizeof(*peer));
          if (wpa->group_rekey_active && sv6621_ap_wpa_rekey_complete(wpa))
            {
              wpa->group_rekey_active = false;
              wpa->group_rekey_retries = 0;
              work_cancel(LPWORK, &wpa->rekey_timeout_work);
              sv6621_ap_wpa_clear(wpa->previous_gtk,
                                  sizeof(wpa->previous_gtk));
              wpa->previous_gtk_index = 0;
              sv6621_ap_wpa_clear(wpa->previous_igtk,
                                  sizeof(wpa->previous_igtk));
              wpa->previous_igtk_index = 0;
            }
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
  uint8_t key_data[128];
  size_t gtk_kde_length;
  size_t key_data_length;
  int ret;

  if (wpa == NULL || rx == NULL || authorized == NULL || address == NULL ||
      rx->frame == NULL ||
      rx->frame_length < SV6621_AP_WPA_ETHERNET_HEADER_SIZE)
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

  ret = sv6621_wpa_eapol_parse(rx->frame, rx->frame_length, peer->key_mgmt,
                               &eapol);
  if (ret == 0 &&
      memcmp(eapol.replay, peer->replay, SV6621_WPA_REPLAY_SIZE) != 0)
    {
      ret = -EALREADY;
    }

  if (ret == 0 && peer->state == SV6621_AP_WPA_WAIT_MESSAGE_2 &&
      eapol.message == SV6621_WPA_MESSAGE_2)
    {
      ret = peer->key_mgmt == SV6621_WPA_KEY_MGMT_SAE
                ? sv6621_wpa_derive_ptk_sha256(peer->pmk, wpa->authenticator,
                                               peer->address, peer->anonce,
                                               eapol.nonce, peer->ptk)
                : sv6621_wpa_derive_ptk(peer->pmk, wpa->authenticator,
                                        peer->address, peer->anonce,
                                        eapol.nonce, peer->ptk);
      if (ret == 0)
        {
          ret = sv6621_wpa_eapol_verify_mic(
              &eapol, peer->key_mgmt, peer->ptk + SV6621_AP_WPA_KCK_OFFSET);
        }

      if (ret == 0)
        {
          ret = sv6621_ap_build_rsn_ie(
              wpa->security, wpa->security == SV6621_SECURITY_WPA3_SAE,
              key_data, sizeof(key_data), &key_data_length);
        }

      if (ret == 0)
        {
          ret = sv6621_wpa_eapol_build_gtk_kde(
              wpa->gtk_index, wpa->gtk, sizeof(wpa->gtk),
              key_data + key_data_length, sizeof(key_data) - key_data_length,
              &gtk_kde_length);
          if (ret == 0)
            {
              key_data_length += gtk_kde_length;
            }
        }

      if (ret == 0 && wpa->pmf_enabled)
        {
          ret = sv6621_wpa_eapol_build_igtk_kde(
              wpa->igtk_index, wpa->igtk_ipn, wpa->igtk,
              key_data + key_data_length, sizeof(key_data) - key_data_length,
              &gtk_kde_length);
          if (ret == 0)
            {
              key_data_length += gtk_kde_length;
            }
        }

      if (ret == 0 && (key_data_length & 7) != 0)
        {
          key_data[key_data_length++] = 0xdd;
          while ((key_data_length & 7) != 0)
            {
              key_data[key_data_length++] = 0;
            }
        }

      if (ret == 0)
        {
          sv6621_ap_wpa_increment_replay(peer->replay);
          ret = sv6621_ap_wpa_send(wpa, peer, SV6621_WPA_MESSAGE_3, key_data,
                                   key_data_length);
        }

      if (ret == 0)
        {
          peer->state = SV6621_AP_WPA_WAIT_MESSAGE_4;
        }
    }
  else if (ret == 0 && peer->state == SV6621_AP_WPA_WAIT_MESSAGE_4 &&
           eapol.message == SV6621_WPA_MESSAGE_4)
    {
      ret = sv6621_wpa_eapol_verify_mic(&eapol, peer->key_mgmt,
                                        peer->ptk + SV6621_AP_WPA_KCK_OFFSET);
      if (ret == 0)
        {
          ret = sv6621_security_add_key_instance(
              wpa->command, wpa->instance, SV6621_SECURITY_KEY_PAIRWISE,
              SV6621_SECURITY_CIPHER_CCMP, peer->address, 0,
              peer->ptk + SV6621_AP_WPA_TK_OFFSET, SV6621_AP_WPA_KEY_SIZE,
              NULL);
        }

      if (ret == 0)
        {
          peer->state = SV6621_AP_WPA_COMPLETE;
          memcpy(address, peer->address, SV6621_MAC_LENGTH);
          *authorized = true;
        }
    }
  else if (ret == 0 && wpa->group_rekey_active &&
           peer->state == SV6621_AP_WPA_COMPLETE &&
           peer->group_rekey_pending &&
           eapol.message == SV6621_WPA_MESSAGE_GROUP_2)
    {
      ret = sv6621_wpa_eapol_verify_mic(&eapol, peer->key_mgmt,
                                        peer->ptk + SV6621_AP_WPA_KCK_OFFSET);
      if (ret == 0)
        {
          peer->group_rekey_pending = false;
          if (sv6621_ap_wpa_rekey_complete(wpa))
            {
              wpa->group_rekey_active = false;
              wpa->group_rekey_retries = 0;
              work_cancel(LPWORK, &wpa->rekey_timeout_work);
              sv6621_ap_wpa_clear(wpa->previous_gtk,
                                  sizeof(wpa->previous_gtk));
              wpa->previous_gtk_index = 0;
              sv6621_ap_wpa_clear(wpa->previous_igtk,
                                  sizeof(wpa->previous_igtk));
              wpa->previous_igtk_index = 0;
            }
        }
    }
  else if (ret == 0)
    {
      ret = -EPROTO;
    }

  nxmutex_unlock(&wpa->lock);
  return ret;
}
