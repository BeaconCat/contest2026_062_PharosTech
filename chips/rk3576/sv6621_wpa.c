/****************************************************************************
 * chips/rk3576/sv6621_wpa.c
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

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/spinlock.h>
#include <sys/random.h>

#include <mbedtls/aes.h>
#include <mbedtls/md.h>

#include "sv6621.h"
#include "sv6621_internal.h"
#include "sv6621_wpa.h"

#ifdef CONFIG_SV6621

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* EAPOL-Key frame geometry (IEEE 802.1X-2004 + key descriptor). */

#define EAPOL_HDR_LEN  4 /* version, type, length(2) */
#define EAPOL_TYPE_KEY 3
#define KEY_DESC_RSN   2 /* key descriptor type */

/* Key-descriptor field offsets within the 802.1X payload. */

#define KD_OFF_DESCTYPE 4  /* 1 byte  */
#define KD_OFF_KEYINFO  5  /* 2 bytes, big-endian */
#define KD_OFF_KEYLEN   7  /* 2 bytes */
#define KD_OFF_REPLAY   9  /* 8 bytes */
#define KD_OFF_NONCE    17 /* 32 bytes */
#define KD_OFF_IV       49 /* 16 bytes */
#define KD_OFF_RSC      65 /* 8 bytes */
#define KD_OFF_RESERVED 73 /* 8 bytes */
#define KD_OFF_MIC      81 /* 16 bytes */
#define KD_OFF_DATALEN  97 /* 2 bytes */
#define KD_OFF_DATA     99
#define KD_FIXED_LEN    99 /* through key_data_length */

/* key_info bit fields (big-endian 16-bit). */

#define KI_VERSION_MASK 0x0007
#define KI_VERSION_SHA1 0x0002 /* HMAC-SHA1 MIC + AES key wrap */
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ERROR        0x0400
#define KI_REQUEST      0x0800
#define KI_ENCRYPTED    0x1000

#define WPA_NONCE_LEN   32
#define WPA_MIC_LEN     16
#define WPA_KCK_LEN     16
#define WPA_KEK_LEN     16
#define WPA_TK_LEN      16 /* CCMP */
#define WPA_PTK_LEN     (WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN)
#define WPA_PMK_LEN     32
#define WPA_GTK_MAX     32

#define ETH_ALEN        6
#define ETHERTYPE_EAPOL 0x888e

/* sv6621_key_params.key_type (sv6621_cfg80211.h). */

#define SKW_KEY_PTK 0
#define SKW_KEY_GTK 1

/* sv6621_key_params.cipher_type: CCMP. */

#define SKW_CIPHER_CCMP 8 /* CP internal enum, not IEEE suite */

/* Commands needed when the station transitions to an authorized state. */

#define SKW_CMD_SET_IP      16
#define SKW_CMD_GET_STA     23
#define SKW_CMD_SET_MC_ADDR 26

#define WPA_SSID_MAX_LEN    32
#define WPA_PASSPHRASE_MIN  8
#define WPA_PASSPHRASE_MAX  63

/* RSN information element the supplicant advertises in msg2 (WPA2-PSK,
 * CCMP pairwise + group, PSK AKM) -- matches the association request.
 */

static const uint8_t g_wpa_rsn_ie[] = {
  0x30, 0x14, 0x01, 0x00,             /* RSN, len 20, version 1 */
  0x00, 0x0f, 0xac, 0x04,             /* group cipher: CCMP */
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x04, /* 1 pairwise: CCMP */
  0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, /* 1 AKM: PSK */
  0x00, 0x00                          /* RSN capabilities */
};

#define WPA_STATE_IDLE      0 /* supplicant not running */
#define WPA_STATE_WAIT_MSG1 1 /* armed, waiting for msg1 */
#define WPA_STATE_WAIT_MSG3 2 /* got msg1, sent msg2 */
#define WPA_STATE_DONE      3 /* handshake complete */
#define WPA_STATE_FAILED    4

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sv6621_wpa_s
{
  uint8_t pmk[WPA_PMK_LEN];
  uint8_t ptk[WPA_PTK_LEN]; /* KCK|KEK|TK */
  uint8_t anonce[WPA_NONCE_LEN];
  uint8_t snonce[WPA_NONCE_LEN];
  uint8_t aa[ETH_ALEN];  /* authenticator (AP) address */
  uint8_t spa[ETH_ALEN]; /* supplicant (STA) address */
  uint8_t replay[8];     /* last seen replay counter */
  uint8_t gtk[WPA_GTK_MAX];
  int gtk_len;
  uint8_t gtk_id;
  int state;
  int result;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct sv6621_wpa_s g_wpa;
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
                   const uint8_t *seed, int seed_length, uint8_t *output,
                   int output_length);
static int wpa_derive_ptk(struct sv6621_wpa_s *wpa);
static int wpa_aes_unwrap(const uint8_t *kek, const uint8_t *cipher,
                          int cipher_length, uint8_t *plain);
static int wpa_tx_eapol(struct sv6621_wpa_s *wpa, uint8_t *descriptor,
                        int descriptor_length, bool include_mic);
static int wpa_send_msg2(struct sv6621_wpa_s *wpa);
static int wpa_send_msg4(struct sv6621_wpa_s *wpa);
static bool wpa_check_mic(struct sv6621_wpa_s *wpa, const uint8_t *descriptor,
                          int length);
static int wpa_extract_gtk(struct sv6621_wpa_s *wpa, const uint8_t *encrypted,
                           int length);
static void wpa_handle_msg1(struct sv6621_wpa_s *wpa,
                            const uint8_t *descriptor, int length);
static void wpa_handle_msg3(struct sv6621_wpa_s *wpa,
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

static int wpa_hmac_sha1(const uint8_t *key, int keylen, const uint8_t *a,
                         int alen, const uint8_t *b, int blen, uint8_t out[20])
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

static int wpa_pbkdf2_sha1(const char *pass, const uint8_t *salt, int saltlen,
                           uint8_t pmk[WPA_PMK_LEN])
{
  uint8_t u[20];
  uint8_t t[20];
  uint8_t block[64];
  int passlen = strlen(pass);
  int blk;
  int i;
  int j;

  for (blk = 1; blk <= 2; blk++) /* 2 blocks -> 40 bytes */
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

      if (wpa_hmac_sha1((const uint8_t *)pass, passlen, block, off + 4, NULL,
                        0, u) < 0)
        {
          return -EIO;
        }

      memcpy(t, u, 20);

      for (i = 1; i < 4096; i++)
        {
          if (wpa_hmac_sha1((const uint8_t *)pass, passlen, u, 20, NULL, 0,
                            u) < 0)
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
                   const uint8_t *seed, int seedlen, uint8_t *out, int outlen)
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

static int wpa_derive_ptk(struct sv6621_wpa_s *w)
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

  return wpa_prf(w->pmk, WPA_PMK_LEN, "Pairwise key expansion", seed,
                 sizeof(seed), w->ptk, WPA_PTK_LEN);
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

static int wpa_tx_eapol(struct sv6621_wpa_s *w, uint8_t *kd, int kdlen,
                        bool domic)
{
  uint8_t frame[14 + KD_OFF_DATA + sizeof(g_wpa_rsn_ie) + 64];
  int flen = 14 + kdlen;

  if (flen > (int)sizeof(frame))
    {
      return -E2BIG;
    }

  memcpy(frame, w->aa, ETH_ALEN);      /* dst = AP */
  memcpy(frame + 6, w->spa, ETH_ALEN); /* src = STA */
  frame[12] = (ETHERTYPE_EAPOL >> 8) & 0xff;
  frame[13] = ETHERTYPE_EAPOL & 0xff;
  memcpy(frame + 14, kd, kdlen);

  if (domic)
    {
      uint8_t mic[20];

      memset(frame + 14 + KD_OFF_MIC, 0, WPA_MIC_LEN);
      if (wpa_hmac_sha1(w->ptk, WPA_KCK_LEN, frame + 14, kdlen, NULL, 0, mic) <
          0)
        {
          return -EIO;
        }

      memcpy(frame + 14 + KD_OFF_MIC, mic, WPA_MIC_LEN);
    }

  int txr = sv6621_data_tx(frame, flen);

  return txr;
}

/****************************************************************************
 * Name: wpa_send_msg2
 ****************************************************************************/

static int wpa_send_msg2(struct sv6621_wpa_s *w)
{
  uint8_t kd[KD_OFF_DATA + sizeof(g_wpa_rsn_ie)];
  int datalen = sizeof(g_wpa_rsn_ie);
  int kdlen = KD_OFF_DATA + datalen;
  uint16_t ki = KI_VERSION_SHA1 | KI_PAIRWISE | KI_MIC;

  memset(kd, 0, sizeof(kd));
  kd[0] = 1; /* 802.1X version (msg2=1) */
  kd[1] = EAPOL_TYPE_KEY;
  kd[2] = ((kdlen - EAPOL_HDR_LEN) >> 8) & 0xff;
  kd[3] = (kdlen - EAPOL_HDR_LEN) & 0xff;
  kd[KD_OFF_DESCTYPE] = KEY_DESC_RSN;
  kd[KD_OFF_KEYINFO] = (ki >> 8) & 0xff;
  kd[KD_OFF_KEYINFO + 1] = ki & 0xff;
  kd[KD_OFF_KEYLEN] = 0;
  kd[KD_OFF_KEYLEN + 1] = 0; /* msg2 key length must be 0 */
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

static int wpa_send_msg4(struct sv6621_wpa_s *w)
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

/****************************************************************************
 * Name: wpa_check_mic
 *
 * Description:
 *   Verify the MIC on a received EAPOL-Key frame: save the MIC, zero the
 *   field, recompute HMAC-SHA1-128 with KCK, compare.
 *
 ****************************************************************************/

static bool wpa_check_mic(struct sv6621_wpa_s *w, const uint8_t *kd, int kdlen)
{
  uint8_t tmp[256];
  uint8_t rx_mic[WPA_MIC_LEN];
  uint8_t calc[20];

  if (kdlen > (int)sizeof(tmp))
    {
      return false;
    }

  memcpy(tmp, kd, kdlen);
  memcpy(rx_mic, tmp + KD_OFF_MIC, WPA_MIC_LEN);
  memset(tmp + KD_OFF_MIC, 0, WPA_MIC_LEN);

  if (wpa_hmac_sha1(w->ptk, WPA_KCK_LEN, tmp, kdlen, NULL, 0, calc) < 0)
    {
      return false;
    }

  return memcmp(calc, rx_mic, WPA_MIC_LEN) == 0;
}

/****************************************************************************
 * Name: wpa_extract_gtk
 *
 * Description:
 *   Recover the GTK from the (AES-key-wrapped) key data of msg3 and locate
 *   the GTK KDE (OUI 00-0F-AC, data type 1).
 *
 ****************************************************************************/

static int wpa_extract_gtk(struct sv6621_wpa_s *w, const uint8_t *enc,
                           int enclen)
{
  uint8_t plain[256];
  int plainlen = enclen - 8;
  int p = 0;

  if (enclen < 24 || plainlen > (int)sizeof(plain))
    {
      return -EINVAL;
    }

  if (wpa_aes_unwrap(w->ptk + WPA_KCK_LEN, enc, enclen, plain) < 0)
    {
      return -EBADMSG;
    }

  /* Walk KDEs: dd len 00-0F-AC <type> <data>.  GTK KDE type = 1. */

  while (p + 2 <= plainlen)
    {
      int elen = plain[p + 1];

      /* AES key-unwrap zero-pads the tail with 0xdd 0x00...; a zero-length
       * element (or one that overruns) marks the end.  The key data leads
       * with the RSN IE (0x30) and other KDEs before the GTK KDE, so skip
       * non-matching elements rather than stopping at the first one.
       */

      if (elen == 0 || p + 2 + elen > plainlen)
        {
          break;
        }

      if (plain[p] == 0xdd && elen >= 6 && plain[p + 2] == 0x00 &&
          plain[p + 3] == 0x0f && plain[p + 4] == 0xac && plain[p + 5] == 0x01)
        {
          /* GTK KDE: keyid(1, low 2 bits) + reserved(1) + GTK[] */

          int gtklen = elen - 6;

          if (gtklen <= 0 || gtklen > WPA_GTK_MAX)
            {
              return -EINVAL;
            }

          w->gtk_id = plain[p + 6] & 0x03;
          w->gtk_len = gtklen;
          memcpy(w->gtk, plain + p + 8, gtklen);
          return OK;
        }

      p += 2 + elen;
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: wpa_handle_msg1
 ****************************************************************************/

static void wpa_handle_msg1(struct sv6621_wpa_s *w, const uint8_t *kd,
                            int kdlen)
{
  bool nonce_empty = true;
  int i;

  sv6621_get_bssid(w->aa);

  if (kdlen < KD_FIXED_LEN)
    {
      return;
    }

  /* SNonce is fixed for the whole handshake: a message 1 retransmit must
   * reuse it (regenerating produces a fresh PTK the AP has not seen).
   */

  for (i = 0; i < WPA_NONCE_LEN; i++)
    {
      if (w->snonce[i] != 0)
        {
          nonce_empty = false;
          break;
        }
    }

  if (!nonce_empty)
    {
      int comparison = wpa_replay_compare(kd + KD_OFF_REPLAY, w->replay);

      if (comparison < 0)
        {
          wlwarn("WPA: stale msg1 replay counter\n");
          return;
        }

      if (comparison > 0)
        {
          wpa_clear(w->snonce, sizeof(w->snonce));
          nonce_empty = true;
        }
    }

  memcpy(w->replay, kd + KD_OFF_REPLAY, 8);
  memcpy(w->anonce, kd + KD_OFF_NONCE, WPA_NONCE_LEN);

  if (nonce_empty)
    {
      int ret = wpa_generate_nonce(w->snonce);

      if (ret < 0)
        {
          w->state = WPA_STATE_FAILED;
          w->result = ret;
          return;
        }
    }

  if (wpa_derive_ptk(w) < 0)
    {
      w->state = WPA_STATE_FAILED;
      w->result = -EIO;
      return;
    }

  {
    int r2 = wpa_send_msg2(w);

    if (r2 < 0)
      {
        w->state = WPA_STATE_FAILED;
        w->result = -EIO;
        return;
      }

    w->state = WPA_STATE_WAIT_MSG3;
    wlinfo("WPA: msg1 rx, msg2 sent (PTK derived)\n");
  }
}

/****************************************************************************
 * Name: wpa_handle_msg3
 ****************************************************************************/

static void wpa_handle_msg3(struct sv6621_wpa_s *w, const uint8_t *kd,
                            int kdlen)
{
  int datalen;
  int ret;

  if (kdlen < KD_FIXED_LEN)
    {
      return;
    }

  if (!wpa_check_mic(w, kd, kdlen))
    {
      wlerr("WPA: msg3 MIC check failed\n");
      w->state = WPA_STATE_FAILED;
      w->result = -EBADMSG;
      return;
    }

  if (wpa_replay_compare(kd + KD_OFF_REPLAY, w->replay) <= 0)
    {
      wlwarn("WPA: stale msg3 replay counter\n");
      return;
    }

  memcpy(w->replay, kd + KD_OFF_REPLAY, 8);

  datalen = (kd[KD_OFF_DATALEN] << 8) | kd[KD_OFF_DATALEN + 1];
  if (datalen > 0 && KD_OFF_DATA + datalen <= kdlen)
    {
      ret = wpa_extract_gtk(w, kd + KD_OFF_DATA, datalen);
      if (ret < 0)
        {
          wlwarn("WPA: no GTK in msg3 (%d); pairwise-only\n", ret);
        }
    }

  /* Send msg4 first (acknowledge), then install the keys. */

  if (wpa_send_msg4(w) < 0)
    {
      w->state = WPA_STATE_FAILED;
      w->result = -EIO;
      return;
    }

  /* The pairwise key's TX PN starts fresh at 1 (independent of the AP's
   * RSC in msg3, which is the group key's receive counter).  The reference
   * driver seeds pn[0] = 1 for CCMP.
   */

  {
    uint8_t ptk_pn[6] = { 1, 0, 0, 0, 0, 0 };

    ret =
        sv6621_add_key(SKW_KEY_PTK, SKW_CIPHER_CCMP, w->aa, 0,
                       w->ptk + WPA_KCK_LEN + WPA_KEK_LEN, WPA_TK_LEN, ptk_pn);
  }
  if (ret >= 0 && w->gtk_len > 0)
    {
      /* The GTK is a receive key: its replay baseline is the AP's current
       * group packet number, carried as the RSC in msg3.  Installing pn = 1
       * would make the CP reject the AP's next broadcast (PN 1 <= 1) as a
       * replay, so seed the CCMP replay counter from the RSC instead.
       */

      /* Group keys install against the broadcast address, matching the
       * reference driver's addr == NULL branch (mac_addr = ff:..:ff).
       */

      uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

      ret = sv6621_add_key(SKW_KEY_GTK, SKW_CIPHER_CCMP, bcast, w->gtk_id,
                           w->gtk, w->gtk_len, kd + KD_OFF_RSC);
    }

  /* Golden post-key sequence (matches the reference driver's authorized
   * transition): GET_STA + SET_MC_ADDR gate the CP into forwarding data
   * frames to the host.  Without them the CP queues only the EAPOL frames.
   */

  if (ret >= 0)
    {
      uint8_t bssid[6];
      uint8_t mc[21] = {
        0x03, 0x00,                         /* count = 3 */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, /* broadcast */
        0x01, 0x00, 0x5e, 0x00, 0x00, 0x01, /* 224.0.0.1 */
        0x01, 0x00, 0x5e, 0x00, 0x00, 0xfb  /* 224.0.0.251 */
      };

      /* sv6621_setip_param: ip_type(1) + addr; IPv6 = 16 bytes.  The reference
       * driver sends SET_IP with the interface's IPv6 link-local address at
       * the COMPLETED transition (before DHCP), which appears to arm the CP
       * to forward data frames to the host.
       */

      uint8_t mac[6];
      uint8_t setip[17] = { 0x01, /* SKW_IP_IPV6 */
                            0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

      sv6621_get_bssid(bssid);
      sv6621_get_mac(mac);

      /* Form the IPv6 interface identifier from the station MAC (modified
       * EUI-64) instead of embedding a board-specific address.
       */

      setip[9] = mac[0] ^ 0x02;
      setip[10] = mac[1];
      setip[11] = mac[2];
      setip[12] = 0xff;
      setip[13] = 0xfe;
      setip[14] = mac[3];
      setip[15] = mac[4];
      setip[16] = mac[5];

      ret = sv6621_send_control(SKW_CMD_GET_STA, bssid, sizeof(bssid));
      if (ret >= 0)
        {
          ret = sv6621_send_control(SKW_CMD_SET_MC_ADDR, mc, sizeof(mc));
        }

      if (ret >= 0)
        {
          ret = sv6621_send_control(SKW_CMD_SET_IP, setip, sizeof(setip));
        }
    }

  w->state = WPA_STATE_DONE;
  w->result = ret >= 0 ? OK : ret;
  wlinfo("WPA: msg3 rx, msg4 sent, keys installed (%d)\n", w->result);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_eapol_input
 ****************************************************************************/

static void wpa_process(const uint8_t *data, int len)
{
  struct sv6621_wpa_s *w = &g_wpa;
  uint16_t ki;

  if (len < KD_FIXED_LEN || data[1] != EAPOL_TYPE_KEY)
    {
      return;
    }

  ki = (data[KD_OFF_KEYINFO] << 8) | data[KD_OFF_KEYINFO + 1];

  /* msg1: pairwise + ack, no MIC.  msg3: pairwise + ack + mic + install. */

  if ((w->state == WPA_STATE_WAIT_MSG1 || w->state == WPA_STATE_WAIT_MSG3) &&
      (ki & KI_MIC) == 0 && (ki & KI_ACK) != 0)
    {
      /* Fresh message 1 or a retransmit (the AP did not accept our
       * message 2): restart the handshake state.
       */

      w->state = WPA_STATE_WAIT_MSG1;
      wpa_handle_msg1(w, data, len);
    }
  else if (w->state == WPA_STATE_WAIT_MSG3 && (ki & KI_MIC) != 0 &&
           (ki & KI_ACK) != 0)
    {
      wpa_handle_msg3(w, data, len);
    }
}

/* EAPOL arrives on the rx thread; process on the connect() thread so
 * msg2/msg4/ADD_KEY use the main-thread command path (waits for ACK,
 * no deadlock, frame actually transmitted).
 */

static uint8_t g_wpa_rx[256];
static int g_wpa_rxlen;
static bool g_wpa_rxpending;

void sv6621_wpa_eapol_input(const uint8_t *data, int len)
{
  irqstate_t flags;

  if (g_wpa.state == WPA_STATE_IDLE || len <= 0 || len > (int)sizeof(g_wpa_rx))
    {
      return;
    }

  flags = spin_lock_irqsave(&g_wpa_rxlock);
  memcpy(g_wpa_rx, data, len);
  g_wpa_rxlen = len;
  g_wpa_rxpending = true;
  spin_unlock_irqrestore(&g_wpa_rxlock, flags);
}

/****************************************************************************
 * Name: sv6621_wpa_connect
 ****************************************************************************/

int sv6621_wpa_connect(const char *ssid, const char *passphrase)
{
  struct sv6621_wpa_s *w = &g_wpa;
  size_t passphrase_len;
  size_t ssid_len;
  int result;
  int ret;

  if (ssid == NULL || passphrase == NULL)
    {
      return -EINVAL;
    }

  ssid_len = strnlen(ssid, WPA_SSID_MAX_LEN + 1);
  passphrase_len = strnlen(passphrase, WPA_PASSPHRASE_MAX + 1);
  if (ssid_len == 0 || ssid_len > WPA_SSID_MAX_LEN ||
      passphrase_len < WPA_PASSPHRASE_MIN ||
      passphrase_len > WPA_PASSPHRASE_MAX)
    {
      return -EINVAL;
    }

  memset(w, 0, sizeof(*w));

  {
    irqstate_t flags = spin_lock_irqsave(&g_wpa_rxlock);

    g_wpa_rxlen = 0;
    g_wpa_rxpending = false;
    spin_unlock_irqrestore(&g_wpa_rxlock, flags);
  }

  /* PMK from the passphrase + SSID salt (this is the slow part). */

  ret = wpa_pbkdf2_sha1(passphrase, (const uint8_t *)ssid, ssid_len, w->pmk);
  if (ret < 0)
    {
      wlerr("WPA: PBKDF2 failed: %d\n", ret);
      result = ret;
      goto out;
    }

  sv6621_get_mac(w->spa);

  /* Arm the EAPOL RX path before association: the AP sends message 1
   * right after assoc-resp, racing the connect return.  The authenticator
   * address is captured lazily on message 1 (the JOIN recorded it).
   */

  /* L2 association (JOIN/AUTH/ASSOC) advertising the RSN IE.  Arm the
   * EAPOL path only AFTER association completes: processing message 1 on
   * the rx thread during association corrupts the shared command slot the
   * main thread uses for the ASSOC ACK.  The AP retransmits message 1, so
   * a dropped first copy is harmless.
   */

  ret = sv6621_connect(ssid);
  if (ret < 0)
    {
      result = ret;
      goto out;
    }

  sv6621_get_bssid(w->aa);
  w->state = WPA_STATE_WAIT_MSG1;

  /* Wait for the 4-way handshake to complete (driven by EAPOL RX). */

  {
    int waited;

    for (waited = 0; waited < 8000; waited += 10)
      {
        irqstate_t flags;
        uint8_t local[256];
        int llen = 0;

        flags = spin_lock_irqsave(&g_wpa_rxlock);
        if (g_wpa_rxpending)
          {
            llen = g_wpa_rxlen;
            if (llen > (int)sizeof(local))
              {
                llen = sizeof(local);
              }

            memcpy(local, g_wpa_rx, llen);
            g_wpa_rxpending = false;
          }

        spin_unlock_irqrestore(&g_wpa_rxlock, flags);

        if (llen > 0)
          {
            wpa_process(local, llen);
            wpa_clear(local, sizeof(local));
          }

        if (w->state == WPA_STATE_DONE || w->state == WPA_STATE_FAILED)
          {
            break;
          }

        up_mdelay(10);
      }
  }

  if (w->state != WPA_STATE_DONE)
    {
      w->state = WPA_STATE_FAILED;
      result = w->result < 0 ? w->result : -ETIMEDOUT;
      goto out;
    }

  result = w->result;

out:
  wpa_clear(w->pmk, sizeof(w->pmk));
  wpa_clear(w->ptk, sizeof(w->ptk));
  wpa_clear(w->anonce, sizeof(w->anonce));
  wpa_clear(w->snonce, sizeof(w->snonce));
  wpa_clear(w->gtk, sizeof(w->gtk));
  w->state = WPA_STATE_IDLE;
  return result;
}

#endif /* CONFIG_SV6621 */
