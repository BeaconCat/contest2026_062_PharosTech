/****************************************************************************
 * chips/rk3576/rk3576_skw_wpa.c
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
 * Host-side WPA2-PSK (CCMP) supplicant for the SeekWave SV6621 FullMAC
 * combo.  The chip offloads authentication and association but not the
 * EAPOL 4-way handshake, so the pairwise/group keys are derived here and
 * pushed to the CP with ADD_KEY.
 *
 * Crypto is layered on mbedtls but kept to the most basic primitives
 * (HMAC-SHA1 and AES-ECB) so it does not depend on the higher-level
 * mbedtls modules (PKCS5, NIST-KW) being enabled in a given config:
 *   - PMK  = PBKDF2-SHA1(passphrase, ssid, 4096, 32)          [RFC 2898]
 *   - PTK  = PRF-384(PMK, "Pairwise key expansion", ...)      [IEEE 802.11]
 *   - MIC  = HMAC-SHA1-128 over the EAPOL frame (key desc v2)
 *   - GTK  = AES key unwrap (RFC 3394) of the msg3 key data
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <sys/random.h>

#include <mbedtls/md.h>
#include <mbedtls/aes.h>

#include "rk3576_skw.h"
#include "rk3576_skw_internal.h"
#include "rk3576_skw_wpa.h"

#ifdef CONFIG_RK3576_SKW

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* EAPOL-Key frame geometry (IEEE 802.1X-2004 + key descriptor). */

#define EAPOL_HDR_LEN        4             /* version, type, length(2) */
#define EAPOL_TYPE_KEY       3
#define KEY_DESC_RSN         2             /* key descriptor type */

/* Key-descriptor field offsets within the 802.1X payload. */

#define KD_OFF_DESCTYPE      4             /* 1 byte  */
#define KD_OFF_KEYINFO       5             /* 2 bytes, big-endian */
#define KD_OFF_KEYLEN        7             /* 2 bytes */
#define KD_OFF_REPLAY        9             /* 8 bytes */
#define KD_OFF_NONCE         17            /* 32 bytes */
#define KD_OFF_IV            49            /* 16 bytes */
#define KD_OFF_RSC           65            /* 8 bytes */
#define KD_OFF_RESERVED      73            /* 8 bytes */
#define KD_OFF_MIC           81            /* 16 bytes */
#define KD_OFF_DATALEN       97            /* 2 bytes */
#define KD_OFF_DATA          99
#define KD_FIXED_LEN         99            /* through key_data_length */

/* key_info bit fields (big-endian 16-bit). */

#define KI_VERSION_MASK      0x0007
#define KI_VERSION_SHA1      0x0002        /* HMAC-SHA1 MIC + AES key wrap */
#define KI_PAIRWISE          0x0008
#define KI_INSTALL           0x0040
#define KI_ACK               0x0080
#define KI_MIC               0x0100
#define KI_SECURE            0x0200
#define KI_ERROR             0x0400
#define KI_REQUEST           0x0800
#define KI_ENCRYPTED         0x1000

#define WPA_NONCE_LEN        32
#define WPA_MIC_LEN          16
#define WPA_KCK_LEN          16
#define WPA_KEK_LEN          16
#define WPA_TK_LEN           16            /* CCMP */
#define WPA_PTK_LEN          (WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN)
#define WPA_PMK_LEN          32
#define WPA_GTK_MAX          32

#define ETH_ALEN             6
#define ETHERTYPE_EAPOL      0x888e

/* skw_key_params.key_type (skw_cfg80211.h). */

#define SKW_KEY_PTK          0
#define SKW_KEY_GTK          1

/* skw_key_params.cipher_type: CCMP. */

#define SKW_CIPHER_CCMP      8            /* CP internal enum, not IEEE suite */

/* Commands needed when the station transitions to an authorized state. */

#define SKW_CMD_SET_IP       16
#define SKW_CMD_GET_STA      23
#define SKW_CMD_SET_MC_ADDR  26

#define WPA_SSID_MAX_LEN     32
#define WPA_PASSPHRASE_MIN   8
#define WPA_PASSPHRASE_MAX   63

/* RSN information element the supplicant advertises in msg2 (WPA2-PSK,
 * CCMP pairwise + group, PSK AKM) -- matches the association request.
 */

static const uint8_t g_wpa_rsn_ie[] =
{
  0x30, 0x14, 0x01, 0x00,             /* RSN, len 20, version 1 */
  0x00, 0x0f, 0xac, 0x04,             /* group cipher: CCMP */
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, /* 1 pairwise: CCMP */
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, /* 1 AKM: PSK */
  0x00, 0x00                          /* RSN capabilities */
};

#define WPA_STATE_IDLE       0            /* supplicant not running */
#define WPA_STATE_WAIT_MSG1  1            /* armed, waiting for msg1 */
#define WPA_STATE_WAIT_MSG3  2            /* got msg1, sent msg2 */
#define WPA_STATE_DONE       3            /* handshake complete */
#define WPA_STATE_FAILED     4

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_wpa_s
{
  uint8_t  pmk[WPA_PMK_LEN];
  uint8_t  ptk[WPA_PTK_LEN];             /* KCK|KEK|TK */
  uint8_t  anonce[WPA_NONCE_LEN];
  uint8_t  snonce[WPA_NONCE_LEN];
  uint8_t  aa[ETH_ALEN];                 /* authenticator (AP) address */
  uint8_t  spa[ETH_ALEN];                /* supplicant (STA) address */
  uint8_t  replay[8];                    /* last seen replay counter */
  uint8_t  gtk[WPA_GTK_MAX];
  int      gtk_len;
  uint8_t  gtk_id;
  int      state;
  int      result;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_wpa_s g_wpa;
static spinlock_t g_wpa_rxlock = SP_UNLOCKED;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void wpa_clear(void *buffer, size_t length);
static int wpa_replay_compare(const uint8_t lhs[8], const uint8_t rhs[8]);
static int wpa_generate_nonce(uint8_t nonce[WPA_NONCE_LEN]);
static int wpa_hmac_sha1(const uint8_t *key, int key_length,
                         const uint8_t *first, int first_length,
                         const uint8_t *second, int second_length,
                         uint8_t output[20]);
static int wpa_pbkdf2_sha1(const char *passphrase, const uint8_t *salt,
                           int salt_length, uint8_t pmk[WPA_PMK_LEN]);
static int wpa_prf(const uint8_t *key, int key_length, const char *label,
                   const uint8_t *seed, int seed_length,
                   uint8_t *output, int output_length);
static int wpa_derive_ptk(struct rk3576_wpa_s *wpa);
static int wpa_aes_unwrap(const uint8_t *kek, const uint8_t *cipher,
                          int cipher_length, uint8_t *plain);
static int wpa_tx_eapol(struct rk3576_wpa_s *wpa, uint8_t *descriptor,
                        int descriptor_length, bool include_mic);
static int wpa_send_msg2(struct rk3576_wpa_s *wpa);
static int wpa_send_msg4(struct rk3576_wpa_s *wpa);
static bool wpa_check_mic(struct rk3576_wpa_s *wpa,
                          const uint8_t *descriptor, int length);
static int wpa_extract_gtk(struct rk3576_wpa_s *wpa,
                           const uint8_t *encrypted, int length);
static void wpa_handle_msg1(struct rk3576_wpa_s *wpa,
                            const uint8_t *descriptor, int length);
static void wpa_handle_msg3(struct rk3576_wpa_s *wpa,
                            const uint8_t *descriptor, int length);
static void wpa_process(const uint8_t *data, int length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wpa_clear
 ****************************************************************************/

static void wpa_clear(void *buffer, size_t length)
{
  volatile uint8_t *p = buffer;

  while (length-- > 0)
    {
      *p++ = 0;
    }
}

/****************************************************************************
 * Name: wpa_replay_compare
 ****************************************************************************/

static int wpa_replay_compare(const uint8_t lhs[8], const uint8_t rhs[8])
{
  int i;

  for (i = 0; i < 8; i++)
    {
      if (lhs[i] != rhs[i])
        {
          return lhs[i] < rhs[i] ? -1 : 1;
        }
    }

  return 0;
}


#endif /* CONFIG_RK3576_SKW */
