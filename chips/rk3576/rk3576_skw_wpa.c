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

/****************************************************************************
 * Name: wpa_generate_nonce
 ****************************************************************************/

static int wpa_generate_nonce(uint8_t nonce[WPA_NONCE_LEN])
{
  size_t offset = 0;

  while (offset < WPA_NONCE_LEN)
    {
      ssize_t nread = getrandom(nonce + offset, WPA_NONCE_LEN - offset, 0);

      if (nread < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (nread == 0)
        {
          return -EIO;
        }

      offset += nread;
    }

  return OK;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wpa_hmac_sha1
 *
 * Description:
 *   HMAC-SHA1 over up to two concatenated input segments.
 *
 ****************************************************************************/

static int wpa_hmac_sha1(const uint8_t *key, int keylen,
                         const uint8_t *a, int alen,
                         const uint8_t *b, int blen,
                         uint8_t out[20])
{
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  mbedtls_md_context_t ctx;
  int ret;

  if (md == NULL)
    {
      return -ENOTSUP;
    }

  mbedtls_md_init(&ctx);
  ret = mbedtls_md_setup(&ctx, md, 1);
  if (ret == 0)
    {
      ret = mbedtls_md_hmac_starts(&ctx, key, keylen);
    }

  if (ret == 0 && alen > 0)
    {
      ret = mbedtls_md_hmac_update(&ctx, a, alen);
    }

  if (ret == 0 && blen > 0)
    {
      ret = mbedtls_md_hmac_update(&ctx, b, blen);
    }

  if (ret == 0)
    {
      ret = mbedtls_md_hmac_finish(&ctx, out);
    }

  mbedtls_md_free(&ctx);
  return ret == 0 ? OK : -EIO;
}

/****************************************************************************
 * Name: wpa_pbkdf2_sha1
 *
 * Description:
 *   PBKDF2-HMAC-SHA1 (RFC 2898) producing a 32-byte PMK.  Fixed to the
 *   WPA2 profile: 4096 iterations, dklen 32.
 *
 ****************************************************************************/

static int wpa_pbkdf2_sha1(const char *pass, const uint8_t *salt,
                           int saltlen, uint8_t pmk[WPA_PMK_LEN])
{
  uint8_t u[20];
  uint8_t t[20];
  uint8_t block[64];
  int passlen = strlen(pass);
  int blk;
  int i;
  int j;

  for (blk = 1; blk <= 2; blk++)              /* 2 blocks -> 40 bytes */
    {
      int off = saltlen;

      if (saltlen > (int)sizeof(block) - 4)
        {
          return -E2BIG;
        }

      memcpy(block, salt, saltlen);
      block[off + 0] = (blk >> 24) & 0xff;
      block[off + 1] = (blk >> 16) & 0xff;
      block[off + 2] = (blk >> 8) & 0xff;
      block[off + 3] = blk & 0xff;

      if (wpa_hmac_sha1((const uint8_t *)pass, passlen,
                        block, off + 4, NULL, 0, u) < 0)
        {
          return -EIO;
        }

      memcpy(t, u, 20);

      for (i = 1; i < 4096; i++)
        {
          if (wpa_hmac_sha1((const uint8_t *)pass, passlen,
                            u, 20, NULL, 0, u) < 0)
            {
              return -EIO;
            }

          for (j = 0; j < 20; j++)
            {
              t[j] ^= u[j];
            }

          /* Yield every 64 rounds: on a UP core this loop otherwise
           * starves the SDIO rx thread and the CP declares the host
           * dead (loopcheck pings go unserviced).
           */

          if ((i & 63) == 0)
            {
              usleep(1000);
            }
        }

      memcpy(pmk + (blk - 1) * 20, t, blk == 1 ? 20 : 12);
    }

  return OK;
}

/****************************************************************************
 * Name: wpa_prf
 *
 * Description:
 *   IEEE 802.11 PRF over HMAC-SHA1 producing bits bytes of output.  The
 *   data block is (label, 0x00, seed, counter).
 *
 ****************************************************************************/

static int wpa_prf(const uint8_t *key, int keylen, const char *label,
                   const uint8_t *seed, int seedlen,
                   uint8_t *out, int outlen)
{
  uint8_t buf[128];
  uint8_t digest[20];
  int labellen = strlen(label);
  int pos = 0;
  uint8_t counter = 0;
  int blocklen;

  if (labellen + 1 + seedlen + 1 > (int)sizeof(buf))
    {
      return -E2BIG;
    }

  memcpy(buf, label, labellen);
  buf[labellen] = 0x00;
  memcpy(buf + labellen + 1, seed, seedlen);
  blocklen = labellen + 1 + seedlen + 1;

  while (pos < outlen)
    {
      int chunk;

      buf[blocklen - 1] = counter;
      if (wpa_hmac_sha1(key, keylen, buf, blocklen, NULL, 0, digest) < 0)
        {
          return -EIO;
        }

      chunk = (outlen - pos) < 20 ? (outlen - pos) : 20;
      memcpy(out + pos, digest, chunk);
      pos += chunk;
      counter++;
    }

  return OK;
}

/****************************************************************************
 * Name: wpa_derive_ptk
 *
 * Description:
 *   PTK = PRF-384(PMK, "Pairwise key expansion",
 *                 min(AA,SPA) || max(AA,SPA) ||
 *                 min(ANonce,SNonce) || max(ANonce,SNonce)).
 *
 ****************************************************************************/

static int wpa_derive_ptk(struct rk3576_wpa_s *w)
{
  uint8_t seed[2 * ETH_ALEN + 2 * WPA_NONCE_LEN];
  const uint8_t *a1;
  const uint8_t *a2;
  const uint8_t *n1;
  const uint8_t *n2;

  if (memcmp(w->aa, w->spa, ETH_ALEN) < 0)
    {
      a1 = w->aa;
      a2 = w->spa;
    }
  else
    {
      a1 = w->spa;
      a2 = w->aa;
    }

  if (memcmp(w->anonce, w->snonce, WPA_NONCE_LEN) < 0)
    {
      n1 = w->anonce;
      n2 = w->snonce;
    }
  else
    {
      n1 = w->snonce;
      n2 = w->anonce;
    }

  memcpy(seed, a1, ETH_ALEN);
  memcpy(seed + ETH_ALEN, a2, ETH_ALEN);
  memcpy(seed + 2 * ETH_ALEN, n1, WPA_NONCE_LEN);
  memcpy(seed + 2 * ETH_ALEN + WPA_NONCE_LEN, n2, WPA_NONCE_LEN);

  return wpa_prf(w->pmk, WPA_PMK_LEN, "Pairwise key expansion",
                 seed, sizeof(seed), w->ptk, WPA_PTK_LEN);
}

/****************************************************************************
 * Name: wpa_aes_unwrap
 *
 * Description:
 *   NIST AES Key Wrap decrypt (RFC 3394), used to recover the GTK KDE
 *   from the encrypted key data of msg3.  n = number of 64-bit blocks in
 *   the plaintext (cipherlen/8 - 1).
 *
 ****************************************************************************/

static int wpa_aes_unwrap(const uint8_t *kek, const uint8_t *cipher,
                          int cipherlen, uint8_t *plain)
{
  mbedtls_aes_context aes;
  uint8_t a[8];
  uint8_t b[16];
  int n = cipherlen / 8 - 1;
  int i;
  int j;
  int ret;

  if (cipherlen < 24 || (cipherlen % 8) != 0)
    {
      return -EINVAL;
    }

  mbedtls_aes_init(&aes);
  ret = mbedtls_aes_setkey_dec(&aes, kek, 128);
  if (ret != 0)
    {
      mbedtls_aes_free(&aes);
      return -EIO;
    }

  memcpy(a, cipher, 8);
  memcpy(plain, cipher + 8, n * 8);

  for (j = 5; j >= 0; j--)
    {
      for (i = n; i >= 1; i--)
        {
          uint64_t t = (uint64_t)n * j + i;
          int k;

          memcpy(b, a, 8);
          for (k = 0; k < 8; k++)
            {
              b[7 - k] ^= (uint8_t)(t >> (8 * k));
            }

          memcpy(b + 8, plain + (i - 1) * 8, 8);
          mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, b, b);
          memcpy(a, b, 8);
          memcpy(plain + (i - 1) * 8, b + 8, 8);
        }
    }

  mbedtls_aes_free(&aes);

  /* Verify the default integrity check register (A6A6A6A6A6A6A6A6). */

  for (i = 0; i < 8; i++)
    {
      if (a[i] != 0xa6)
        {
          return -EBADMSG;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: wpa_tx_eapol
 *
 * Description:
 *   Wrap an 802.1X EAPOL-Key payload in an Ethernet header (STA->AP,
 *   EtherType 0x888e) and hand it to the SKW core channel-7 transmit.
 *   If domic is set, compute the HMAC-SHA1-128 MIC over the frame (MIC
 *   field pre-zeroed) with KCK and patch it in before sending.
 *
 ****************************************************************************/

static int wpa_tx_eapol(struct rk3576_wpa_s *w, uint8_t *kd, int kdlen,
                        bool domic)
{
  uint8_t frame[14 + KD_OFF_DATA + sizeof(g_wpa_rsn_ie) + 64];
  int flen = 14 + kdlen;

  if (flen > (int)sizeof(frame))
    {
      return -E2BIG;
    }

  memcpy(frame, w->aa, ETH_ALEN);            /* dst = AP */
  memcpy(frame + 6, w->spa, ETH_ALEN);       /* src = STA */
  frame[12] = (ETHERTYPE_EAPOL >> 8) & 0xff;
  frame[13] = ETHERTYPE_EAPOL & 0xff;
  memcpy(frame + 14, kd, kdlen);

  if (domic)
    {
      uint8_t mic[20];

      memset(frame + 14 + KD_OFF_MIC, 0, WPA_MIC_LEN);
      if (wpa_hmac_sha1(w->ptk, WPA_KCK_LEN, frame + 14, kdlen,
                        NULL, 0, mic) < 0)
        {
          return -EIO;
        }

      memcpy(frame + 14 + KD_OFF_MIC, mic, WPA_MIC_LEN);
    }

  int txr = rk3576_skw_data_tx(frame, flen);

  return txr;
}

/****************************************************************************
 * Name: wpa_send_msg2
 ****************************************************************************/

static int wpa_send_msg2(struct rk3576_wpa_s *w)
{
  uint8_t kd[KD_OFF_DATA + sizeof(g_wpa_rsn_ie)];
  int datalen = sizeof(g_wpa_rsn_ie);
  int kdlen = KD_OFF_DATA + datalen;
  uint16_t ki = KI_VERSION_SHA1 | KI_PAIRWISE | KI_MIC;

  memset(kd, 0, sizeof(kd));
  kd[0] = 1;                                 /* 802.1X version (msg2=1) */
  kd[1] = EAPOL_TYPE_KEY;
  kd[2] = ((kdlen - EAPOL_HDR_LEN) >> 8) & 0xff;
  kd[3] = (kdlen - EAPOL_HDR_LEN) & 0xff;
  kd[KD_OFF_DESCTYPE] = KEY_DESC_RSN;
  kd[KD_OFF_KEYINFO] = (ki >> 8) & 0xff;
  kd[KD_OFF_KEYINFO + 1] = ki & 0xff;
  kd[KD_OFF_KEYLEN] = 0;
  kd[KD_OFF_KEYLEN + 1] = 0;                  /* msg2 key length must be 0 */
  memcpy(kd + KD_OFF_REPLAY, w->replay, 8);
  memcpy(kd + KD_OFF_NONCE, w->snonce, WPA_NONCE_LEN);
  kd[KD_OFF_DATALEN] = (datalen >> 8) & 0xff;
  kd[KD_OFF_DATALEN + 1] = datalen & 0xff;
  memcpy(kd + KD_OFF_DATA, g_wpa_rsn_ie, datalen);

  return wpa_tx_eapol(w, kd, kdlen, true);
}

/****************************************************************************
 * Name: wpa_send_msg4
 ****************************************************************************/

static int wpa_send_msg4(struct rk3576_wpa_s *w)
{
  uint8_t kd[KD_OFF_DATA];
  uint16_t ki = KI_VERSION_SHA1 | KI_PAIRWISE | KI_MIC | KI_SECURE;

  memset(kd, 0, sizeof(kd));
  kd[0] = 2;
  kd[1] = EAPOL_TYPE_KEY;
  kd[2] = ((KD_OFF_DATA - EAPOL_HDR_LEN) >> 8) & 0xff;
  kd[3] = (KD_OFF_DATA - EAPOL_HDR_LEN) & 0xff;
  kd[KD_OFF_DESCTYPE] = KEY_DESC_RSN;
  kd[KD_OFF_KEYINFO] = (ki >> 8) & 0xff;
  kd[KD_OFF_KEYINFO + 1] = ki & 0xff;
  kd[KD_OFF_KEYLEN] = 0;
  kd[KD_OFF_KEYLEN + 1] = WPA_TK_LEN;
  memcpy(kd + KD_OFF_REPLAY, w->replay, 8);
  kd[KD_OFF_DATALEN] = 0;
  kd[KD_OFF_DATALEN + 1] = 0;

  return wpa_tx_eapol(w, kd, KD_OFF_DATA, true);
}


#endif /* CONFIG_RK3576_SKW */
