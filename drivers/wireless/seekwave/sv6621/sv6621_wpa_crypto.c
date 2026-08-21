/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_crypto.c
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
#include <sched.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/cmac.h>
#include <mbedtls/md.h>

#include "sv6621_wpa_crypto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_PASSPHRASE_MIN 8
#define SV6621_WPA_PASSPHRASE_MAX 63
#define SV6621_WPA_HEX_PSK_SIZE   64
#define SV6621_WPA_SSID_MAX       32
#define SV6621_WPA_PBKDF_ROUNDS   4096
#define SV6621_WPA_PBKDF_SALT_MAX (SV6621_WPA_SSID_MAX + 4)
#define SV6621_WPA_MAC_SIZE       6
#define SV6621_WPA_PTK_SEED_SIZE \
  (SV6621_WPA_MAC_SIZE * 2 + SV6621_WPA_NONCE_SIZE * 2)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_wpa_ptk_label[] = "Pairwise key expansion";
static const uint8_t g_sv6621_wpa_wrap_iv[8] = { 0xa6, 0xa6, 0xa6, 0xa6,
                                                 0xa6, 0xa6, 0xa6, 0xa6 };

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_wpa_hex_value(uint8_t character);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_hex_value
 ****************************************************************************/

static int sv6621_wpa_hex_value(uint8_t character)
{
  if (character >= '0' && character <= '9')
    {
      return character - '0';
    }

  if (character >= 'a' && character <= 'f')
    {
      return character - 'a' + 10;
    }

  if (character >= 'A' && character <= 'F')
    {
      return character - 'A' + 10;
    }

  return -EINVAL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_wpa_hmac_sha1
 ****************************************************************************/

int sv6621_wpa_hmac_sha1(FAR const uint8_t *key, size_t key_length,
                         FAR const uint8_t *data, size_t data_length,
                         uint8_t output[SV6621_WPA_SHA1_SIZE])
{
  FAR const mbedtls_md_info_t *info;
  mbedtls_md_context_t context;
  int ret;

  if (key == NULL || key_length == 0 || data == NULL || output == NULL)
    {
      return -EINVAL;
    }

  info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (info == NULL)
    {
      return -ENOSYS;
    }

  mbedtls_md_init(&context);
  ret = mbedtls_md_setup(&context, info, 1);
  if (ret == 0)
    {
      ret = mbedtls_md_hmac_starts(&context, key, key_length);
    }

  if (ret == 0)
    {
      ret = mbedtls_md_hmac_update(&context, data, data_length);
    }

  if (ret == 0)
    {
      ret = mbedtls_md_hmac_finish(&context, output);
    }

  mbedtls_md_free(&context);
  return ret == 0 ? 0 : -EIO;
}

/****************************************************************************
 * Name: sv6621_wpa_hmac_sha256
 ****************************************************************************/

int sv6621_wpa_hmac_sha256(FAR const uint8_t *key, size_t key_length,
                           FAR const uint8_t *data, size_t data_length,
                           uint8_t output[SV6621_WPA_SHA256_SIZE])
{
  FAR const mbedtls_md_info_t *info;
  mbedtls_md_context_t context;
  int ret;

  if (key == NULL || key_length == 0 || data == NULL || output == NULL)
    {
      return -EINVAL;
    }

  info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == NULL)
    {
      return -ENOSYS;
    }

  mbedtls_md_init(&context);
  ret = mbedtls_md_setup(&context, info, 1);
  if (ret == 0)
    {
      ret = mbedtls_md_hmac_starts(&context, key, key_length);
    }

  if (ret == 0)
    {
      ret = mbedtls_md_hmac_update(&context, data, data_length);
    }

  if (ret == 0)
    {
      ret = mbedtls_md_hmac_finish(&context, output);
    }

  mbedtls_md_free(&context);
  return ret == 0 ? 0 : -EIO;
}

/****************************************************************************
 * Name: sv6621_wpa_aes_cmac
 ****************************************************************************/

int sv6621_wpa_aes_cmac(FAR const uint8_t key[16], FAR const uint8_t *data,
                        size_t data_length, uint8_t output[16])
{
  FAR const mbedtls_cipher_info_t *info;
  int ret;

  if (key == NULL || data == NULL || output == NULL)
    {
      return -EINVAL;
    }

  info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
  if (info == NULL)
    {
      return -ENOSYS;
    }

  ret = mbedtls_cipher_cmac(info, key, 128, data, data_length, output);
  return ret == 0 ? 0 : -EIO;
}

/****************************************************************************
 * Name: sv6621_wpa_derive_pmk
 ****************************************************************************/

int sv6621_wpa_derive_pmk(FAR const uint8_t *passphrase,
                          size_t passphrase_length, FAR const uint8_t *ssid,
                          size_t ssid_length, uint8_t pmk[SV6621_WPA_PMK_SIZE])
{
  uint8_t salt[SV6621_WPA_PBKDF_SALT_MAX];
  uint8_t digest[SV6621_WPA_SHA1_SIZE];
  uint8_t accumulator[SV6621_WPA_SHA1_SIZE];
  size_t produced = 0;
  uint32_t block = 1;
  unsigned int round;
  size_t index;
  int ret;

  if (passphrase == NULL || ssid == NULL || pmk == NULL ||
      passphrase_length < SV6621_WPA_PASSPHRASE_MIN || ssid_length == 0 ||
      ssid_length > SV6621_WPA_SSID_MAX)
    {
      return -EINVAL;
    }

  if (passphrase_length == SV6621_WPA_HEX_PSK_SIZE)
    {
      for (index = 0; index < SV6621_WPA_PMK_SIZE; index++)
        {
          int high = sv6621_wpa_hex_value(passphrase[index * 2]);
          int low = sv6621_wpa_hex_value(passphrase[index * 2 + 1]);

          if (high < 0 || low < 0)
            {
              memset(pmk, 0, SV6621_WPA_PMK_SIZE);
              return -EINVAL;
            }

          pmk[index] = (high << 4) | low;
        }

      return 0;
    }

  if (passphrase_length > SV6621_WPA_PASSPHRASE_MAX)
    {
      return -EINVAL;
    }

  memcpy(salt, ssid, ssid_length);
  while (produced < SV6621_WPA_PMK_SIZE)
    {
      size_t copy_length;

      salt[ssid_length] = block >> 24;
      salt[ssid_length + 1] = block >> 16;
      salt[ssid_length + 2] = block >> 8;
      salt[ssid_length + 3] = block;
      ret = sv6621_wpa_hmac_sha1(passphrase, passphrase_length, salt,
                                 ssid_length + 4, digest);
      if (ret < 0)
        {
          return ret;
        }

      memcpy(accumulator, digest, sizeof(accumulator));
      for (round = 1; round < SV6621_WPA_PBKDF_ROUNDS; round++)
        {
          ret = sv6621_wpa_hmac_sha1(passphrase, passphrase_length, digest,
                                     sizeof(digest), digest);
          if (ret < 0)
            {
              return ret;
            }

          for (index = 0; index < sizeof(accumulator); index++)
            {
              accumulator[index] ^= digest[index];
            }

          if ((round & 63) == 0)
            {
              sched_yield();
            }
        }

      copy_length = SV6621_WPA_PMK_SIZE - produced;
      if (copy_length > sizeof(accumulator))
        {
          copy_length = sizeof(accumulator);
        }

      memcpy(pmk + produced, accumulator, copy_length);
      produced += copy_length;
      block++;
    }

  memset(salt, 0, sizeof(salt));
  memset(digest, 0, sizeof(digest));
  memset(accumulator, 0, sizeof(accumulator));
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_derive_ptk
 ****************************************************************************/

int sv6621_wpa_derive_ptk(FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
                          FAR const uint8_t authenticator[SV6621_WPA_MAC_SIZE],
                          FAR const uint8_t supplicant[SV6621_WPA_MAC_SIZE],
                          FAR const uint8_t anonce[SV6621_WPA_NONCE_SIZE],
                          FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
                          uint8_t ptk[SV6621_WPA_PTK_SIZE])
{
  uint8_t input[sizeof(g_sv6621_wpa_ptk_label) + SV6621_WPA_PTK_SEED_SIZE + 1];
  uint8_t digest[SV6621_WPA_SHA1_SIZE];
  FAR const uint8_t *first;
  FAR const uint8_t *second;
  size_t offset = 0;
  size_t produced = 0;
  uint8_t counter = 0;
  int ret;

  if (pmk == NULL || authenticator == NULL || supplicant == NULL ||
      anonce == NULL || snonce == NULL || ptk == NULL)
    {
      return -EINVAL;
    }

  memcpy(input, g_sv6621_wpa_ptk_label, sizeof(g_sv6621_wpa_ptk_label) - 1);
  offset = sizeof(g_sv6621_wpa_ptk_label) - 1;
  input[offset++] = 0;

  if (memcmp(authenticator, supplicant, SV6621_WPA_MAC_SIZE) <= 0)
    {
      first = authenticator;
      second = supplicant;
    }
  else
    {
      first = supplicant;
      second = authenticator;
    }

  memcpy(input + offset, first, SV6621_WPA_MAC_SIZE);
  offset += SV6621_WPA_MAC_SIZE;
  memcpy(input + offset, second, SV6621_WPA_MAC_SIZE);
  offset += SV6621_WPA_MAC_SIZE;

  if (memcmp(anonce, snonce, SV6621_WPA_NONCE_SIZE) <= 0)
    {
      first = anonce;
      second = snonce;
    }
  else
    {
      first = snonce;
      second = anonce;
    }

  memcpy(input + offset, first, SV6621_WPA_NONCE_SIZE);
  offset += SV6621_WPA_NONCE_SIZE;
  memcpy(input + offset, second, SV6621_WPA_NONCE_SIZE);
  offset += SV6621_WPA_NONCE_SIZE;

  while (produced < SV6621_WPA_PTK_SIZE)
    {
      size_t copy_length = SV6621_WPA_PTK_SIZE - produced;

      input[offset] = counter++;
      ret = sv6621_wpa_hmac_sha1(pmk, SV6621_WPA_PMK_SIZE, input, offset + 1,
                                 digest);
      if (ret < 0)
        {
          memset(input, 0, sizeof(input));
          return ret;
        }

      if (copy_length > sizeof(digest))
        {
          copy_length = sizeof(digest);
        }

      memcpy(ptk + produced, digest, copy_length);
      produced += copy_length;
    }

  memset(input, 0, sizeof(input));
  memset(digest, 0, sizeof(digest));
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_derive_ptk_sha256
 ****************************************************************************/

int sv6621_wpa_derive_ptk_sha256(
    FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
    FAR const uint8_t authenticator[SV6621_WPA_MAC_SIZE],
    FAR const uint8_t supplicant[SV6621_WPA_MAC_SIZE],
    FAR const uint8_t anonce[SV6621_WPA_NONCE_SIZE],
    FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
    uint8_t ptk[SV6621_WPA_PTK_SIZE])
{
  uint8_t data[SV6621_WPA_PTK_SEED_SIZE];
  uint8_t input[2 + sizeof(g_sv6621_wpa_ptk_label) - 1 +
                SV6621_WPA_PTK_SEED_SIZE + 2];
  uint8_t digest[SV6621_WPA_SHA256_SIZE];
  FAR const uint8_t *first;
  FAR const uint8_t *second;
  size_t produced = 0;
  size_t offset = 0;
  uint16_t counter = 1;
  int ret;

  if (pmk == NULL || authenticator == NULL || supplicant == NULL ||
      anonce == NULL || snonce == NULL || ptk == NULL)
    {
      return -EINVAL;
    }

  if (memcmp(authenticator, supplicant, SV6621_WPA_MAC_SIZE) <= 0)
    {
      first = authenticator;
      second = supplicant;
    }
  else
    {
      first = supplicant;
      second = authenticator;
    }

  memcpy(data + offset, first, SV6621_WPA_MAC_SIZE);
  offset += SV6621_WPA_MAC_SIZE;
  memcpy(data + offset, second, SV6621_WPA_MAC_SIZE);
  offset += SV6621_WPA_MAC_SIZE;

  if (memcmp(anonce, snonce, SV6621_WPA_NONCE_SIZE) <= 0)
    {
      first = anonce;
      second = snonce;
    }
  else
    {
      first = snonce;
      second = anonce;
    }

  memcpy(data + offset, first, SV6621_WPA_NONCE_SIZE);
  offset += SV6621_WPA_NONCE_SIZE;
  memcpy(data + offset, second, SV6621_WPA_NONCE_SIZE);

  memcpy(input + 2, g_sv6621_wpa_ptk_label,
         sizeof(g_sv6621_wpa_ptk_label) - 1);
  memcpy(input + 2 + sizeof(g_sv6621_wpa_ptk_label) - 1, data, sizeof(data));
  input[sizeof(input) - 2] = (SV6621_WPA_PTK_SIZE * 8) & 0xff;
  input[sizeof(input) - 1] = (SV6621_WPA_PTK_SIZE * 8) >> 8;

  while (produced < SV6621_WPA_PTK_SIZE)
    {
      size_t copy_length = SV6621_WPA_PTK_SIZE - produced;

      input[0] = counter & 0xff;
      input[1] = counter >> 8;
      ret = sv6621_wpa_hmac_sha256(pmk, SV6621_WPA_PMK_SIZE, input,
                                   sizeof(input), digest);
      if (ret < 0)
        {
          memset(data, 0, sizeof(data));
          memset(input, 0, sizeof(input));
          memset(digest, 0, sizeof(digest));
          return ret;
        }

      if (copy_length > sizeof(digest))
        {
          copy_length = sizeof(digest);
        }

      memcpy(ptk + produced, digest, copy_length);
      produced += copy_length;
      counter++;
    }

  memset(data, 0, sizeof(data));
  memset(input, 0, sizeof(input));
  memset(digest, 0, sizeof(digest));
  return 0;
}

/****************************************************************************
 * Name: sv6621_wpa_unwrap_key
 ****************************************************************************/

int sv6621_wpa_unwrap_key(FAR const uint8_t kek[SV6621_WPA_KEK_SIZE],
                          FAR const uint8_t *wrapped, size_t wrapped_length,
                          FAR uint8_t *plain, size_t capacity,
                          FAR size_t *plain_length)
{
  mbedtls_aes_context aes;
  uint8_t block[16];
  uint8_t a[8];
  size_t count;
  size_t index;
  unsigned int round;
  int ret;

  if (kek == NULL || wrapped == NULL || plain == NULL ||
      plain_length == NULL || wrapped_length < 24 ||
      (wrapped_length & 7) != 0 || capacity < wrapped_length - 8)
    {
      return -EINVAL;
    }

  count = wrapped_length / 8 - 1;
  memcpy(a, wrapped, sizeof(a));
  memcpy(plain, wrapped + 8, wrapped_length - 8);

  mbedtls_aes_init(&aes);
  ret = mbedtls_aes_setkey_dec(&aes, kek, 128);
  for (round = 6; ret == 0 && round > 0; round--)
    {
      for (index = count; index > 0; index--)
        {
          uint64_t counter = (uint64_t)(round - 1) * count + index;
          size_t byte;

          memcpy(block, a, sizeof(a));
          for (byte = 0; byte < sizeof(a); byte++)
            {
              block[sizeof(a) - 1 - byte] ^= counter & 0xff;
              counter >>= 8;
            }

          memcpy(block + sizeof(a), plain + (index - 1) * 8, 8);
          ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, block, block);
          if (ret != 0)
            {
              break;
            }

          memcpy(a, block, sizeof(a));
          memcpy(plain + (index - 1) * 8, block + sizeof(a), 8);
        }
    }

  mbedtls_aes_free(&aes);
  if (ret != 0 || memcmp(a, g_sv6621_wpa_wrap_iv, sizeof(a)) != 0)
    {
      memset(plain, 0, wrapped_length - 8);
      ret = ret != 0 ? -EIO : -EKEYREJECTED;
    }
  else
    {
      *plain_length = wrapped_length - 8;
      ret = 0;
    }

  memset(a, 0, sizeof(a));
  memset(block, 0, sizeof(block));
  return ret;
}

/****************************************************************************

 * * Name: sv6621_wpa_wrap_key

 * ****************************************************************************/

int sv6621_wpa_wrap_key(FAR const uint8_t kek[SV6621_WPA_KEK_SIZE],
                        FAR const uint8_t *plain, size_t plain_length,
                        FAR uint8_t *wrapped, size_t capacity,
                        FAR size_t *wrapped_length)
{
  mbedtls_aes_context aes;
  uint8_t block[16];
  uint8_t a[8];
  size_t count;
  size_t index;
  unsigned int round;
  int ret;

  if (kek == NULL || plain == NULL || wrapped == NULL ||
      wrapped_length == NULL || plain_length < 16 || (plain_length & 7) != 0 ||
      capacity < plain_length + 8)
    {
      return -EINVAL;
    }

  count = plain_length / 8;
  memcpy(a, g_sv6621_wpa_wrap_iv, sizeof(a));
  memcpy(wrapped + sizeof(a), plain, plain_length);
  mbedtls_aes_init(&aes);
  ret = mbedtls_aes_setkey_enc(&aes, kek, 128);
  for (round = 0; ret == 0 && round < 6; round++)
    {
      for (index = 1; index <= count; index++)
        {
          uint64_t counter = (uint64_t)round * count + index;
          size_t byte;

          memcpy(block, a, sizeof(a));
          memcpy(block + sizeof(a), wrapped + index * 8, 8);
          ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, block);
          if (ret != 0)
            {
              break;
            }

          memcpy(a, block, sizeof(a));
          for (byte = 0; byte < sizeof(a); byte++)
            {
              a[sizeof(a) - 1 - byte] ^= counter & 0xff;
              counter >>= 8;
            }

          memcpy(wrapped + index * 8, block + sizeof(a), 8);
        }
    }

  mbedtls_aes_free(&aes);
  if (ret == 0)
    {
      memcpy(wrapped, a, sizeof(a));
      *wrapped_length = plain_length + sizeof(a);
    }
  else
    {
      memset(wrapped, 0, plain_length + sizeof(a));
      ret = -EIO;
    }

  memset(a, 0, sizeof(a));
  memset(block, 0, sizeof(block));
  return ret;
}
