/****************************************************************************
 * drivers/drivers/sv6621/sv6621_sae_crypto.c
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

#include <nuttx/kmalloc.h>

#include <sys/random.h>

#include <errno.h>
#include <string.h>

#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "sv6621_sae_crypto.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static FAR const mbedtls_md_info_t *sv6621_sae_sha256_info(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR const mbedtls_md_info_t *sv6621_sae_sha256_info(void)
{
  return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sae_sha256(FAR const uint8_t *data, size_t data_length,
                      uint8_t output[SV6621_SAE_SHA256_SIZE])
{
  FAR const mbedtls_md_info_t *info;
  int ret;

  if ((data == NULL && data_length != 0) || output == NULL)
    {
      return -EINVAL;
    }

  info = sv6621_sae_sha256_info();
  if (info == NULL)
    {
      return -ENOSYS;
    }

  ret = mbedtls_md(info, data, data_length, output);
  return ret == 0 ? 0 : -EIO;
}

int sv6621_sae_hmac_sha256(FAR const uint8_t *key, size_t key_length,
                           FAR const uint8_t *data, size_t data_length,
                           uint8_t output[SV6621_SAE_SHA256_SIZE])
{
  FAR const mbedtls_md_info_t *info;
  int ret;

  if ((key == NULL && key_length != 0) || (data == NULL && data_length != 0) ||
      output == NULL)
    {
      return -EINVAL;
    }

  info = sv6621_sae_sha256_info();
  if (info == NULL)
    {
      return -ENOSYS;
    }

  ret = mbedtls_md_hmac(info, key, key_length, data, data_length, output);
  return ret == 0 ? 0 : -EIO;
}

int sv6621_sae_hkdf_extract(FAR const uint8_t *salt, size_t salt_length,
                            FAR const uint8_t *input, size_t input_length,
                            uint8_t output[SV6621_SAE_SHA256_SIZE])
{
  FAR const mbedtls_md_info_t *info;
  int ret;

  if ((salt == NULL && salt_length != 0) ||
      (input == NULL && input_length != 0) || output == NULL)
    {
      return -EINVAL;
    }

  info = sv6621_sae_sha256_info();
  if (info == NULL)
    {
      return -ENOSYS;
    }

  ret = mbedtls_hkdf_extract(info, salt, salt_length, input, input_length,
                             output);
  return ret == 0 ? 0 : -EIO;
}

int sv6621_sae_hkdf_expand(FAR const uint8_t key[SV6621_SAE_SHA256_SIZE],
                           FAR const uint8_t *info, size_t info_length,
                           FAR uint8_t *output, size_t output_length)
{
  FAR const mbedtls_md_info_t *md_info;
  int ret;

  if (key == NULL || (info == NULL && info_length != 0) || output == NULL ||
      output_length == 0)
    {
      return -EINVAL;
    }

  md_info = sv6621_sae_sha256_info();
  if (md_info == NULL)
    {
      return -ENOSYS;
    }

  ret = mbedtls_hkdf_expand(md_info, key, SV6621_SAE_SHA256_SIZE, info,
                            info_length, output, output_length);
  return ret == 0 ? 0 : -EIO;
}

int sv6621_sae_kdf_hash_length(FAR const uint8_t *key, size_t key_length,
                               FAR const uint8_t *label, size_t label_length,
                               FAR const uint8_t *context,
                               size_t context_length, FAR uint8_t *output,
                               size_t output_length)
{
  uint8_t digest[SV6621_SAE_SHA256_SIZE];
  FAR uint8_t *input;
  size_t input_length;
  size_t produced = 0;
  uint16_t output_bits;
  uint16_t counter = 1;
  int ret = 0;

  if (key == NULL || key_length == 0 || label == NULL || label_length == 0 ||
      (context == NULL && context_length != 0) || output == NULL ||
      output_length == 0 || output_length > UINT16_MAX / 8 ||
      context_length > SIZE_MAX - 4 ||
      label_length > SIZE_MAX - context_length - 4)
    {
      return -EINVAL;
    }

  input_length = 2 + label_length + context_length + 2;
  input = kmm_malloc(input_length);
  if (input == NULL)
    {
      return -ENOMEM;
    }

  output_bits = output_length * 8;
  memcpy(input + 2, label, label_length);
  if (context_length > 0)
    {
      memcpy(input + 2 + label_length, context, context_length);
    }
  input[input_length - 2] = output_bits;
  input[input_length - 1] = output_bits >> 8;

  while (produced < output_length)
    {
      size_t copy_length = output_length - produced;

      input[0] = counter;
      input[1] = counter >> 8;
      ret =
          sv6621_sae_hmac_sha256(key, key_length, input, input_length, digest);
      if (ret < 0)
        {
          break;
        }

      if (copy_length > sizeof(digest))
        {
          copy_length = sizeof(digest);
        }

      memcpy(output + produced, digest, copy_length);
      produced += copy_length;
      counter++;
    }

  sv6621_sae_zeroize(digest, sizeof(digest));
  sv6621_sae_zeroize(input, input_length);
  kmm_free(input);
  return ret;
}

int sv6621_sae_random(FAR uint8_t *output, size_t output_length)
{
  size_t offset = 0;

  if (output == NULL || output_length == 0)
    {
      return -EINVAL;
    }

  while (offset < output_length)
    {
      ssize_t received = getrandom(output + offset, output_length - offset, 0);

      if (received < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (received == 0)
        {
          return -EIO;
        }

      offset += received;
    }

  return 0;
}

int sv6621_sae_random_callback(FAR void *arg, FAR unsigned char *output,
                               size_t output_length)
{
  (void)arg;
  return sv6621_sae_random(output, output_length) == 0 ? 0 : -1;
}

bool sv6621_sae_constant_equal(FAR const uint8_t *left,
                               FAR const uint8_t *right, size_t length)
{
  uint8_t difference = 0;
  size_t index;

  if (left == NULL || right == NULL)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      difference |= left[index] ^ right[index];
    }

  return difference == 0;
}

void sv6621_sae_zeroize(FAR void *data, size_t length)
{
  FAR volatile uint8_t *bytes = data;

  if (bytes == NULL)
    {
      return;
    }

  while (length-- > 0)
    {
      *bytes++ = 0;
    }
}
