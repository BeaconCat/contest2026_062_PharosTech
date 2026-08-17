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
#include <string.h>

#include <mbedtls/md.h>

#include "sv6621_wpa_crypto.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_PASSPHRASE_MIN 8
#define SV6621_WPA_PASSPHRASE_MAX 63
#define SV6621_WPA_SSID_MAX        32
#define SV6621_WPA_PBKDF_ROUNDS    4096
#define SV6621_WPA_PBKDF_SALT_MAX  (SV6621_WPA_SSID_MAX + 4)

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
 * Name: sv6621_wpa_derive_pmk
 ****************************************************************************/

int sv6621_wpa_derive_pmk(FAR const uint8_t *passphrase,
                           size_t passphrase_length, FAR const uint8_t *ssid,
                           size_t ssid_length,
                           uint8_t pmk[SV6621_WPA_PMK_SIZE])
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
      passphrase_length < SV6621_WPA_PASSPHRASE_MIN ||
      passphrase_length > SV6621_WPA_PASSPHRASE_MAX || ssid_length == 0 ||
      ssid_length > SV6621_WPA_SSID_MAX)
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
