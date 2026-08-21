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
#define SV6621_WPA_REKEY_TIMEOUT_MS     5000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_wpa_clear(FAR void *buffer, size_t length);
static int sv6621_wpa_generate_nonce(uint8_t nonce[SV6621_WPA_NONCE_SIZE]);
static int sv6621_wpa_compare_replay(FAR const uint8_t *left,
                                     FAR const uint8_t *right);
static bool sv6621_wpa_is_frame_error(int error);
static int sv6621_wpa_schedule_locked(FAR struct sv6621_wpa_s *wpa);
static void sv6621_wpa_remove_keys(FAR struct sv6621_wpa_s *wpa);
static void sv6621_wpa_forget_keys(FAR struct sv6621_wpa_s *wpa);
static void sv6621_wpa_cancel_internal(FAR struct sv6621_wpa_s *wpa,
                                       int result, bool remove_keys);
static bool sv6621_wpa_group_key_matches(FAR const struct sv6621_wpa_s *wpa,
                                         uint8_t key_index,
                                         FAR const uint8_t *gtk,
                                         size_t gtk_length);
static void sv6621_wpa_finish(FAR struct sv6621_wpa_s *wpa, int result);
static int sv6621_wpa_send_response(FAR struct sv6621_wpa_s *wpa,
                                    enum sv6621_wpa_response_e response);
static int sv6621_wpa_restore_group_key(
    FAR struct sv6621_wpa_s *wpa, enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher, uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[SV6621_WPA_IPN_SIZE]);
static int sv6621_wpa_decode_gtk(FAR struct sv6621_wpa_s *wpa,
                                 FAR const struct sv6621_wpa_eapol_s *eapol,
                                 FAR uint8_t *key_index, FAR uint8_t *gtk,
                                 FAR size_t *gtk_length);
static int sv6621_wpa_decode_igtk(FAR struct sv6621_wpa_s *wpa,
                                  FAR const struct sv6621_wpa_eapol_s *eapol,
                                  FAR uint8_t *key_index,
                                  FAR uint8_t ipn[SV6621_WPA_IPN_SIZE],
                                  FAR uint8_t igtk[SV6621_WPA_IGTK_SIZE]);
static int
sv6621_wpa_process_message_1(FAR struct sv6621_wpa_s *wpa,
                             FAR const struct sv6621_wpa_eapol_s *eapol);
static int
sv6621_wpa_process_message_3(FAR struct sv6621_wpa_s *wpa,
                             FAR const struct sv6621_wpa_eapol_s *eapol);
static int
sv6621_wpa_process_group_message_1(FAR struct sv6621_wpa_s *wpa,
                                   FAR const struct sv6621_wpa_eapol_s *eapol);
static void sv6621_wpa_rekey_timeout_worker(FAR void *arg);
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

static int sv6621_wpa_generate_nonce(uint8_t nonce[SV6621_WPA_NONCE_SIZE])
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
 * Name: sv6621_wpa_is_frame_error
 ****************************************************************************/

static bool sv6621_wpa_is_frame_error(int error)
{
  return error == -EINVAL || error == -EPROTO || error == -EOPNOTSUPP ||
         error == -EKEYREJECTED || error == -E2BIG || error == -ENOENT;
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
  if (wpa->integrity_group_installed)
    {
      sv6621_security_delete_key(wpa->command,
                                 SV6621_SECURITY_KEY_INTEGRITY_GROUP,
                                 SV6621_SECURITY_CIPHER_BIP_CMAC_128,
                                 wpa->authenticator, wpa->igtk_index);
      wpa->integrity_group_installed = false;
    }

  if (wpa->group_installed)
    {
      sv6621_security_delete_key(wpa->command, SV6621_SECURITY_KEY_GROUP,
                                 SV6621_SECURITY_CIPHER_CCMP,
                                 wpa->authenticator, wpa->gtk_index);
      wpa->group_installed = false;
    }

  if (wpa->pairwise_installed)
    {
      sv6621_security_delete_key(wpa->command, SV6621_SECURITY_KEY_PAIRWISE,
                                 SV6621_SECURITY_CIPHER_CCMP,
                                 wpa->authenticator, 0);
      wpa->pairwise_installed = false;
    }

  sv6621_wpa_forget_keys(wpa);
}

/****************************************************************************

 * * Name: sv6621_wpa_forget_keys

 * ****************************************************************************/

static void sv6621_wpa_forget_keys(FAR struct sv6621_wpa_s *wpa)
{
  wpa->integrity_group_installed = false;
  wpa->group_installed = false;
  wpa->pairwise_installed = false;
  sv6621_wpa_clear(wpa->igtk, sizeof(wpa->igtk));
  sv6621_wpa_clear(wpa->igtk_ipn, sizeof(wpa->igtk_ipn));
  sv6621_wpa_clear(wpa->gtk, sizeof(wpa->gtk));
  wpa->gtk_length = 0;
}

/****************************************************************************
 * Name: sv6621_wpa_group_key_matches
 ****************************************************************************/

static bool sv6621_wpa_group_key_matches(FAR const struct sv6621_wpa_s *wpa,
                                         uint8_t key_index,
                                         FAR const uint8_t *gtk,
                                         size_t gtk_length)
{
  return wpa->group_installed && wpa->gtk_index == key_index &&
         wpa->gtk_length == gtk_length &&
         memcmp(wpa->gtk, gtk, gtk_length) == 0;
}

/****************************************************************************
 * Name: sv6621_wpa_finish
 ****************************************************************************/

static void sv6621_wpa_finish(FAR struct sv6621_wpa_s *wpa, int result)
{
  work_cancel(LPWORK, &wpa->rekey_timeout_work);
  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  wpa->result = result;
  wpa->state = result == 0 ? SV6621_WPA_COMPLETE : SV6621_WPA_FAILED;
  wpa->rekeying = false;
  wpa->frame_pending = false;
  nxmutex_unlock(&wpa->lock);
  nxsem_post(&wpa->completion);
}

/****************************************************************************
 * Name: sv6621_wpa_send_response
 ****************************************************************************/

static int sv6621_wpa_send_response(FAR struct sv6621_wpa_s *wpa,
                                    enum sv6621_wpa_response_e response)
{
  uint8_t frame[SV6621_WPA_FRAME_CAPACITY];
  size_t eapol_length;
  int ret;

  memcpy(frame, wpa->authenticator, SV6621_MAC_LENGTH);
  memcpy(frame + SV6621_MAC_LENGTH, wpa->supplicant, SV6621_MAC_LENGTH);
  frame[12] = SV6621_WPA_ETHERTYPE_EAPOL >> 8;
  frame[13] = SV6621_WPA_ETHERTYPE_EAPOL & 0xff;
  ret = sv6621_wpa_eapol_build(
      response, wpa->key_mgmt, wpa->eapol_version, wpa->replay, wpa->snonce,
      wpa->ptk + SV6621_WPA_KCK_OFFSET,
      frame + SV6621_WPA_ETHERNET_HEADER_SIZE,
      sizeof(frame) - SV6621_WPA_ETHERNET_HEADER_SIZE, &eapol_length);
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_security_send_eapol(wpa->command, &wpa->tx_context, frame,
                                    SV6621_WPA_ETHERNET_HEADER_SIZE +
                                        eapol_length);
}

/****************************************************************************

 * * Name: sv6621_wpa_restore_group_key

 * ****************************************************************************/

static int sv6621_wpa_restore_group_key(
    FAR struct sv6621_wpa_s *wpa, enum sv6621_security_key_type_e type,
    enum sv6621_security_cipher_e cipher, uint8_t key_index,
    FAR const uint8_t *key, size_t key_length,
    FAR const uint8_t packet_number[SV6621_WPA_IPN_SIZE])
{
  return sv6621_security_add_key(wpa->command, type, cipher,
                                 wpa->authenticator, key_index, key,
                                 key_length, packet_number);
}

/****************************************************************************

 * * Name: sv6621_wpa_decode_gtk

 * ****************************************************************************/

static int sv6621_wpa_decode_gtk(FAR struct sv6621_wpa_s *wpa,
                                 FAR const struct sv6621_wpa_eapol_s *eapol,
                                 FAR uint8_t *key_index, FAR uint8_t *gtk,
                                 FAR size_t *gtk_length)
{
  uint8_t plain[SV6621_WPA_FRAME_CAPACITY];
  size_t plain_length;
  int ret;

  if ((eapol->key_info & SV6621_WPA_KEY_ENCRYPTED) == 0)
    {
      return -EOPNOTSUPP;
    }

  ret = sv6621_wpa_unwrap_key(wpa->ptk + SV6621_WPA_KEK_OFFSET,
                              eapol->key_data, eapol->key_data_length, plain,
                              sizeof(plain), &plain_length);
  if (ret == 0)
    {
      ret = sv6621_wpa_eapol_extract_gtk(plain, plain_length, key_index, gtk,
                                         SV6621_WPA_GTK_MAX_SIZE, gtk_length);
    }

  sv6621_wpa_clear(plain, sizeof(plain));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_decode_igtk
 ****************************************************************************/

static int sv6621_wpa_decode_igtk(FAR struct sv6621_wpa_s *wpa,
                                  FAR const struct sv6621_wpa_eapol_s *eapol,
                                  FAR uint8_t *key_index,
                                  FAR uint8_t ipn[SV6621_WPA_IPN_SIZE],
                                  FAR uint8_t igtk[SV6621_WPA_IGTK_SIZE])
{
  uint8_t plain[SV6621_WPA_FRAME_CAPACITY];
  size_t plain_length;
  int ret;

  if ((eapol->key_info & SV6621_WPA_KEY_ENCRYPTED) == 0)
    {
      return -EOPNOTSUPP;
    }

  ret = sv6621_wpa_unwrap_key(wpa->ptk + SV6621_WPA_KEK_OFFSET,
                              eapol->key_data, eapol->key_data_length, plain,
                              sizeof(plain), &plain_length);
  if (ret == 0)
    {
      ret = sv6621_wpa_eapol_extract_igtk(plain, plain_length, key_index, ipn,
                                          igtk);
    }

  sv6621_wpa_clear(plain, sizeof(plain));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_message_1
 ****************************************************************************/

static int
sv6621_wpa_process_message_1(FAR struct sv6621_wpa_s *wpa,
                             FAR const struct sv6621_wpa_eapol_s *eapol)
{
  int ret;

  if (wpa->replay_valid)
    {
      ret = sv6621_wpa_compare_replay(eapol->replay, wpa->replay);
      if (ret == 0)
        {
          return memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) == 0
                     ? sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_2)
                     : -EKEYREJECTED;
        }

      if (ret < 0)
        {
          return -EALREADY;
        }
    }

  if (wpa->state == SV6621_WPA_COMPLETE ||
      (wpa->state == SV6621_WPA_WAIT_MESSAGE_3 &&
       memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) != 0))
    {
      ret = sv6621_wpa_generate_nonce(wpa->snonce);
      if (ret < 0)
        {
          return ret;
        }
    }

  memcpy(wpa->anonce, eapol->nonce, sizeof(wpa->anonce));
  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));
  wpa->replay_valid = true;
  wpa->eapol_version = eapol->version;
  ret = wpa->key_mgmt == SV6621_WPA_KEY_MGMT_SAE
            ? sv6621_wpa_derive_ptk_sha256(wpa->pmk, wpa->authenticator,
                                           wpa->supplicant, wpa->anonce,
                                           wpa->snonce, wpa->ptk)
            : sv6621_wpa_derive_ptk(wpa->pmk, wpa->authenticator,
                                    wpa->supplicant, wpa->anonce, wpa->snonce,
                                    wpa->ptk);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_2);
  if (ret == 0)
    {
      bool rekeying;

      ret = nxmutex_lock(&wpa->lock);
      if (ret == 0)
        {
          if (wpa->state == SV6621_WPA_COMPLETE ||
              wpa->state == SV6621_WPA_FAILED)
            {
              wpa->rekeying = true;
            }

          wpa->state = SV6621_WPA_WAIT_MESSAGE_3;
          rekeying = wpa->rekeying;
          nxmutex_unlock(&wpa->lock);
          if (rekeying)
            {
              work_cancel(LPWORK, &wpa->rekey_timeout_work);
              ret = work_queue(LPWORK, &wpa->rekey_timeout_work,
                               sv6621_wpa_rekey_timeout_worker, wpa,
                               MSEC2TICK(SV6621_WPA_REKEY_TIMEOUT_MS));
            }
        }
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_message_3
 ****************************************************************************/

static int
sv6621_wpa_process_message_3(FAR struct sv6621_wpa_s *wpa,
                             FAR const struct sv6621_wpa_eapol_s *eapol)
{
  uint8_t gtk[SV6621_WPA_GTK_MAX_SIZE];
  uint8_t igtk[SV6621_WPA_IGTK_SIZE] = { 0 };
  uint8_t ipn[SV6621_WPA_IPN_SIZE] = { 0 };
  size_t gtk_length;
  uint8_t gtk_index;
  uint8_t igtk_index;
  bool install_group;
  bool install_igtk = false;
  int ret;

  if (sv6621_wpa_compare_replay(eapol->replay, wpa->replay) <= 0 ||
      memcmp(eapol->nonce, wpa->anonce, sizeof(wpa->anonce)) != 0)
    {
      return -EKEYREJECTED;
    }

  ret = sv6621_wpa_eapol_verify_mic(eapol, wpa->key_mgmt,
                                    wpa->ptk + SV6621_WPA_KCK_OFFSET);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_decode_gtk(wpa, eapol, &gtk_index, gtk, &gtk_length);
  if (ret < 0)
    {
      return ret;
    }

  if (wpa->key_mgmt == SV6621_WPA_KEY_MGMT_SAE)
    {
      ret = sv6621_wpa_decode_igtk(wpa, eapol, &igtk_index, ipn, igtk);
      if (ret < 0)
        {
          goto clear_keys;
        }

      install_igtk = !wpa->integrity_group_installed ||
                     wpa->igtk_index != igtk_index ||
                     memcmp(wpa->igtk, igtk, sizeof(igtk)) != 0 ||
                     memcmp(wpa->igtk_ipn, ipn, sizeof(ipn)) != 0;
    }

  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));
  wpa->eapol_version = eapol->version;

  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_4);
  if (ret < 0)
    {
      goto clear_keys;
    }

  ret = sv6621_security_add_key(
      wpa->command, SV6621_SECURITY_KEY_PAIRWISE, SV6621_SECURITY_CIPHER_CCMP,
      wpa->authenticator, 0, wpa->ptk + SV6621_WPA_TK_OFFSET,
      SV6621_WPA_TK_SIZE, NULL);
  if (ret < 0)
    {
      goto clear_keys;
    }

  wpa->pairwise_installed = true;

  install_group =
      !sv6621_wpa_group_key_matches(wpa, gtk_index, gtk, gtk_length);
  if (install_group)
    {
      (void)sv6621_security_delete_key(wpa->command, SV6621_SECURITY_KEY_GROUP,
                                       SV6621_SECURITY_CIPHER_CCMP,
                                       wpa->authenticator, gtk_index);
      ret = sv6621_security_add_key(
          wpa->command, SV6621_SECURITY_KEY_GROUP, SV6621_SECURITY_CIPHER_CCMP,
          wpa->authenticator, gtk_index, gtk, gtk_length, NULL);
      if (ret < 0)
        {
          goto clear_keys;
        }
      else
        {
          memcpy(wpa->gtk, gtk, gtk_length);
          wpa->gtk_length = gtk_length;
        }
    }

  if (install_igtk)
    {
      ret = sv6621_security_add_key(
          wpa->command, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
          SV6621_SECURITY_CIPHER_BIP_CMAC_128, wpa->authenticator, igtk_index,
          igtk, sizeof(igtk), ipn);
      if (ret < 0)
        {
          if (install_group)
            {
              sv6621_security_delete_key(
                  wpa->command, SV6621_SECURITY_KEY_GROUP,
                  SV6621_SECURITY_CIPHER_CCMP, wpa->authenticator, gtk_index);
            }

          goto clear_keys;
        }

      if (wpa->integrity_group_installed && wpa->igtk_index != igtk_index)
        {
          sv6621_security_delete_key(wpa->command,
                                     SV6621_SECURITY_KEY_INTEGRITY_GROUP,
                                     SV6621_SECURITY_CIPHER_BIP_CMAC_128,
                                     wpa->authenticator, wpa->igtk_index);
        }

      memcpy(wpa->igtk, igtk, sizeof(wpa->igtk));
      memcpy(wpa->igtk_ipn, ipn, sizeof(wpa->igtk_ipn));
      wpa->igtk_index = igtk_index;
      wpa->integrity_group_installed = true;
    }

  wpa->gtk_index = gtk_index;
  wpa->group_installed = true;
  ret = sv6621_station_mark_connected(wpa->station);

clear_keys:
  sv6621_wpa_clear(igtk, sizeof(igtk));
  sv6621_wpa_clear(ipn, sizeof(ipn));
  sv6621_wpa_clear(gtk, sizeof(gtk));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_process_group_message_1
 ****************************************************************************/

static int
sv6621_wpa_process_group_message_1(FAR struct sv6621_wpa_s *wpa,
                                   FAR const struct sv6621_wpa_eapol_s *eapol)
{
  uint8_t gtk[SV6621_WPA_GTK_MAX_SIZE];
  uint8_t igtk[SV6621_WPA_IGTK_SIZE] = { 0 };
  uint8_t ipn[SV6621_WPA_IPN_SIZE] = { 0 };
  size_t gtk_length;
  uint8_t gtk_index;
  uint8_t igtk_index = 0;
  int comparison;
  bool install_group;
  bool install_igtk = false;
  bool restore_gtk = false;
  bool restore_igtk = false;
  uint8_t old_gtk[SV6621_WPA_GTK_MAX_SIZE] = { 0 };
  uint8_t old_igtk[SV6621_WPA_IGTK_SIZE] = { 0 };
  uint8_t old_igtk_ipn[SV6621_WPA_IPN_SIZE] = { 0 };
  size_t old_gtk_length = 0;
  uint8_t old_gtk_index = 0;
  uint8_t old_igtk_index = 0;
  int restore_ret;
  int ret;

  ret = sv6621_wpa_eapol_verify_mic(eapol, wpa->key_mgmt,
                                    wpa->ptk + SV6621_WPA_KCK_OFFSET);
  if (ret < 0)
    {
      return ret;
    }

  comparison = sv6621_wpa_compare_replay(eapol->replay, wpa->replay);
  if (comparison == 0)
    {
      wpa->eapol_version = eapol->version;
      return sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_GROUP_2);
    }

  if (comparison < 0)
    {
      return -EALREADY;
    }

  ret = sv6621_wpa_decode_gtk(wpa, eapol, &gtk_index, gtk, &gtk_length);
  if (ret < 0)
    {
      goto clear_keys;
    }

  install_group =
      !sv6621_wpa_group_key_matches(wpa, gtk_index, gtk, gtk_length);
  if (install_group && wpa->group_installed && wpa->gtk_index == gtk_index)
    {
      restore_gtk = true;
      old_gtk_index = wpa->gtk_index;
      old_gtk_length = wpa->gtk_length;
      memcpy(old_gtk, wpa->gtk, old_gtk_length);
    }

  if (wpa->key_mgmt == SV6621_WPA_KEY_MGMT_SAE)
    {
      ret = sv6621_wpa_decode_igtk(wpa, eapol, &igtk_index, ipn, igtk);
      if (ret == 0)
        {
          install_igtk = !wpa->integrity_group_installed ||
                         wpa->igtk_index != igtk_index ||
                         memcmp(wpa->igtk, igtk, sizeof(igtk)) != 0 ||
                         memcmp(wpa->igtk_ipn, ipn, sizeof(ipn)) != 0;
          if (install_igtk && wpa->integrity_group_installed &&
              wpa->igtk_index == igtk_index)
            {
              restore_igtk = true;
              old_igtk_index = wpa->igtk_index;
              memcpy(old_igtk, wpa->igtk, sizeof(old_igtk));
              memcpy(old_igtk_ipn, wpa->igtk_ipn, sizeof(old_igtk_ipn));
            }
        }
      else if (ret != -ENOENT)
        {
          goto clear_keys;
        }
    }

  if (install_group)
    {
      ret = sv6621_security_add_key(
          wpa->command, SV6621_SECURITY_KEY_GROUP, SV6621_SECURITY_CIPHER_CCMP,
          wpa->authenticator, gtk_index, gtk, gtk_length, NULL);
      if (ret < 0)
        {
          if (restore_gtk)
            {
              restore_ret = sv6621_wpa_restore_group_key(
                  wpa, SV6621_SECURITY_KEY_GROUP, SV6621_SECURITY_CIPHER_CCMP,
                  old_gtk_index, old_gtk, old_gtk_length, NULL);
              if (restore_ret < 0)
                {
                  ret = restore_ret;
                }
            }

          goto clear_keys;
        }
    }

  if (install_igtk)
    {
      ret = sv6621_security_add_key(
          wpa->command, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
          SV6621_SECURITY_CIPHER_BIP_CMAC_128, wpa->authenticator, igtk_index,
          igtk, sizeof(igtk), ipn);
      if (ret < 0)
        {
          if (install_group)
            {
              sv6621_security_delete_key(
                  wpa->command, SV6621_SECURITY_KEY_GROUP,
                  SV6621_SECURITY_CIPHER_CCMP, wpa->authenticator, gtk_index);
            }

          if (restore_gtk)
            {
              restore_ret = sv6621_wpa_restore_group_key(
                  wpa, SV6621_SECURITY_KEY_GROUP, SV6621_SECURITY_CIPHER_CCMP,
                  old_gtk_index, old_gtk, old_gtk_length, NULL);
              if (restore_ret < 0)
                {
                  ret = restore_ret;
                }
            }

          if (restore_igtk)
            {
              restore_ret = sv6621_wpa_restore_group_key(
                  wpa, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
                  SV6621_SECURITY_CIPHER_BIP_CMAC_128, old_igtk_index,
                  old_igtk, sizeof(old_igtk), old_igtk_ipn);
              if (restore_ret < 0)
                {
                  ret = restore_ret;
                }
            }

          goto clear_keys;
        }
    }

  wpa->eapol_version = eapol->version;
  ret = sv6621_wpa_send_response(wpa, SV6621_WPA_RESPONSE_GROUP_2);
  if (ret < 0)
    {
      if (install_group)
        {
          sv6621_security_delete_key(wpa->command, SV6621_SECURITY_KEY_GROUP,
                                     SV6621_SECURITY_CIPHER_CCMP,
                                     wpa->authenticator, gtk_index);
        }

      if (install_igtk)
        {
          sv6621_security_delete_key(wpa->command,
                                     SV6621_SECURITY_KEY_INTEGRITY_GROUP,
                                     SV6621_SECURITY_CIPHER_BIP_CMAC_128,
                                     wpa->authenticator, igtk_index);
        }

      if (restore_gtk)
        {
          restore_ret = sv6621_wpa_restore_group_key(
              wpa, SV6621_SECURITY_KEY_GROUP, SV6621_SECURITY_CIPHER_CCMP,
              old_gtk_index, old_gtk, old_gtk_length, NULL);
          if (restore_ret < 0)
            {
              ret = restore_ret;
            }
        }

      if (restore_igtk)
        {
          restore_ret = sv6621_wpa_restore_group_key(
              wpa, SV6621_SECURITY_KEY_INTEGRITY_GROUP,
              SV6621_SECURITY_CIPHER_BIP_CMAC_128, old_igtk_index, old_igtk,
              sizeof(old_igtk), old_igtk_ipn);
          if (restore_ret < 0)
            {
              ret = restore_ret;
            }
        }

      goto clear_keys;
    }

  /* Commit the replay counter only after Group Message 2 has reached the
   *
   * firmware.  If transmission fails, the AP may retransmit the same Group

   * * Message 1 and the replacement keys must be installed again.
   */

  memcpy(wpa->replay, eapol->replay, sizeof(wpa->replay));

  if (install_group && wpa->group_installed && wpa->gtk_index != gtk_index)
    {
      sv6621_security_delete_key(wpa->command, SV6621_SECURITY_KEY_GROUP,
                                 SV6621_SECURITY_CIPHER_CCMP,
                                 wpa->authenticator, wpa->gtk_index);
    }

  if (install_group)
    {
      memcpy(wpa->gtk, gtk, gtk_length);
      wpa->gtk_length = gtk_length;
    }

  if (install_igtk && wpa->integrity_group_installed &&
      wpa->igtk_index != igtk_index)
    {
      sv6621_security_delete_key(wpa->command,
                                 SV6621_SECURITY_KEY_INTEGRITY_GROUP,
                                 SV6621_SECURITY_CIPHER_BIP_CMAC_128,
                                 wpa->authenticator, wpa->igtk_index);
    }

  if (install_igtk)
    {
      memcpy(wpa->igtk, igtk, sizeof(wpa->igtk));
      memcpy(wpa->igtk_ipn, ipn, sizeof(wpa->igtk_ipn));
      wpa->igtk_index = igtk_index;
      wpa->integrity_group_installed = true;
    }

  wpa->gtk_index = gtk_index;
  wpa->group_installed = true;

clear_keys:
  sv6621_wpa_clear(igtk, sizeof(igtk));
  sv6621_wpa_clear(ipn, sizeof(ipn));
  sv6621_wpa_clear(gtk, sizeof(gtk));
  sv6621_wpa_clear(old_gtk, sizeof(old_gtk));
  sv6621_wpa_clear(old_igtk, sizeof(old_igtk));
  sv6621_wpa_clear(old_igtk_ipn, sizeof(old_igtk_ipn));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_rekey_timeout_worker
 ****************************************************************************/

static void sv6621_wpa_rekey_timeout_worker(FAR void *arg)
{
  FAR struct sv6621_wpa_s *wpa = arg;
  bool timed_out;
  int ret;

  if (nxmutex_lock(&wpa->lock) < 0)
    {
      return;
    }

  timed_out = wpa->state == SV6621_WPA_WAIT_MESSAGE_3 && wpa->rekeying;
  nxmutex_unlock(&wpa->lock);
  if (!timed_out)
    {
      return;
    }

  ret = sv6621_station_disconnect(wpa->station, SV6621_WPA_REASON_UNSPECIFIED);
  if (ret >= 0)
    {
      sv6621_wpa_forget_keys(wpa);
    }
  else
    {
      sv6621_wpa_remove_keys(wpa);
    }

  sv6621_wpa_finish(wpa, -ETIMEDOUT);
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
  bool rekeying;
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
      rekeying = wpa->rekeying;
      nxmutex_unlock(&wpa->lock);

      ret = sv6621_wpa_eapol_parse(frame, length, wpa->key_mgmt, &eapol);
      if (ret == 0 && eapol.message == SV6621_WPA_MESSAGE_1 &&
          (state == SV6621_WPA_WAIT_MESSAGE_1 ||
           state == SV6621_WPA_WAIT_MESSAGE_3 || state == SV6621_WPA_COMPLETE))
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
          ret = sv6621_wpa_eapol_verify_mic(&eapol, wpa->key_mgmt,
                                            wpa->ptk + SV6621_WPA_KCK_OFFSET);
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
          if (sv6621_wpa_is_frame_error(ret))
            {
              continue;
            }

          if (state == SV6621_WPA_COMPLETE || rekeying)
            {
              int disconnect_ret = sv6621_station_disconnect(
                  wpa->station, SV6621_WPA_REASON_UNSPECIFIED);

              if (disconnect_ret >= 0)
                {
                  sv6621_wpa_forget_keys(wpa);
                }
              else
                {
                  sv6621_wpa_remove_keys(wpa);
                }
            }
          else
            {
              sv6621_wpa_remove_keys(wpa);
            }
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
  int ret;

  if (wpa == NULL || request == NULL || supplicant == NULL ||
      authenticator == NULL ||
      (request->security != SV6621_SECURITY_WPA2_PSK &&
       request->security != SV6621_SECURITY_WPA2_WPA3_PSK))
    {
      return -EINVAL;
    }

  ret = sv6621_wpa_derive_pmk(request->credential, request->credential_length,
                              request->ssid, request->ssid_length, pmk);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_wpa_prepare_pmk(wpa, pmk, SV6621_WPA_KEY_MGMT_PSK, supplicant,
                               authenticator);
  sv6621_wpa_clear(pmk, sizeof(pmk));
  return ret;
}

/****************************************************************************
 * Name: sv6621_wpa_prepare_pmk
 ****************************************************************************/

int sv6621_wpa_prepare_pmk(FAR struct sv6621_wpa_s *wpa,
                           FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
                           enum sv6621_wpa_key_mgmt_e key_mgmt,
                           FAR const uint8_t supplicant[SV6621_MAC_LENGTH],
                           FAR const uint8_t authenticator[SV6621_MAC_LENGTH])
{
  uint8_t snonce[SV6621_WPA_NONCE_SIZE];
  int ret;

  if (wpa == NULL || pmk == NULL || supplicant == NULL ||
      authenticator == NULL || key_mgmt > SV6621_WPA_KEY_MGMT_SAE)
    {
      return -EINVAL;
    }

  ret = sv6621_wpa_generate_nonce(snonce);
  if (ret < 0)
    {
      return ret;
    }

  sv6621_wpa_cancel(wpa, -ECANCELED);
  ret = nxmutex_lock(&wpa->lock);
  if (ret < 0)
    {
      sv6621_wpa_clear(snonce, sizeof(snonce));
      return ret;
    }

  if (wpa->state != SV6621_WPA_IDLE && wpa->state != SV6621_WPA_COMPLETE &&
      wpa->state != SV6621_WPA_FAILED)
    {
      nxmutex_unlock(&wpa->lock);
      sv6621_wpa_clear(snonce, sizeof(snonce));
      return -EBUSY;
    }

  nxsem_reset(&wpa->completion, 0);
  sv6621_wpa_clear(wpa->ptk, sizeof(wpa->ptk));
  memcpy(wpa->pmk, pmk, sizeof(wpa->pmk));
  memcpy(wpa->snonce, snonce, sizeof(wpa->snonce));
  memcpy(wpa->supplicant, supplicant, sizeof(wpa->supplicant));
  memcpy(wpa->authenticator, authenticator, sizeof(wpa->authenticator));
  wpa->key_mgmt = key_mgmt;
  memset(wpa->replay, 0, sizeof(wpa->replay));
  wpa->replay_valid = false;
  wpa->rekeying = false;
  wpa->result = -EINPROGRESS;
  wpa->eapol_version = 0;
  wpa->peer_ready = false;
  wpa->frame_pending = false;
  wpa->state = SV6621_WPA_WAIT_MESSAGE_1;
  nxmutex_unlock(&wpa->lock);
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
      int wait_result = ret;

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

      sv6621_wpa_cancel(wpa, wait_result);
      return wait_result;
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
 * Name: sv6621_wpa_cancel_internal

 * ****************************************************************************/

static void sv6621_wpa_cancel_internal(FAR struct sv6621_wpa_s *wpa,
                                       int result, bool remove_keys)
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
  work_cancel_sync(LPWORK, &wpa->rekey_timeout_work);
  if (remove_keys)
    {
      sv6621_wpa_remove_keys(wpa);
    }
  else
    {
      sv6621_wpa_forget_keys(wpa);
    }

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
  wpa->rekeying = false;
  wpa->gtk_index = 0;
  wpa->igtk_index = 0;
  sv6621_wpa_clear(wpa->pmk, sizeof(wpa->pmk));
  sv6621_wpa_clear(wpa->ptk, sizeof(wpa->ptk));
  nxmutex_unlock(&wpa->lock);
  if (pending)
    {
      nxsem_post(&wpa->completion);
    }
}

/****************************************************************************

 * * Name: sv6621_wpa_cancel

 * ****************************************************************************/

void sv6621_wpa_cancel(FAR struct sv6621_wpa_s *wpa, int result)
{
  sv6621_wpa_cancel_internal(wpa, result, true);
}

/****************************************************************************

 * * Name: sv6621_wpa_disconnected

 * ****************************************************************************/

void sv6621_wpa_disconnected(FAR struct sv6621_wpa_s *wpa, int result)
{
  sv6621_wpa_cancel_internal(wpa, result, false);
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

  if (rx->frame_length >= SV6621_WPA_ETHERNET_HEADER_SIZE &&
      memcmp(rx->frame, wpa->supplicant, SV6621_MAC_LENGTH) == 0 &&
      memcmp(rx->frame + SV6621_MAC_LENGTH, wpa->authenticator,
             SV6621_MAC_LENGTH) == 0 &&
      (wpa->state == SV6621_WPA_WAIT_MESSAGE_1 ||
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
      int disconnect_ret = sv6621_station_disconnect(
          wpa->station, SV6621_WPA_REASON_UNSPECIFIED);

      if (disconnect_ret >= 0)
        {
          sv6621_wpa_forget_keys(wpa);
        }
      else
        {
          sv6621_wpa_remove_keys(wpa);
        }

      sv6621_wpa_finish(wpa, ret);
    }
}
