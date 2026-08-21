/****************************************************************************
 * drivers/drivers/sv6621/sv6621_sae_keys.c
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

#include <mbedtls/ecp.h>

#include "sv6621_sae_keys.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_KEY_MATERIAL_SIZE \
  (SV6621_SAE_KCK_SIZE + SV6621_SAE_PMK_SIZE)
#define SV6621_SAE_CONFIRM_INPUT_SIZE \
  (2 + 2 * SV6621_SAE_SCALAR_SIZE + 2 * SV6621_SAE_ELEMENT_SIZE)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_sae_key_label[] = "SAE KCK and PMK";

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sae_derive_keys(
    FAR const uint8_t secret[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t own_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE],
    uint8_t kck[SV6621_SAE_KCK_SIZE], uint8_t pmk[SV6621_SAE_PMK_SIZE],
    uint8_t pmkid[SV6621_SAE_PMKID_SIZE])
{
  uint8_t zero_key[SV6621_SAE_SHA256_SIZE] = { 0 };
  uint8_t keyseed[SV6621_SAE_SHA256_SIZE];
  uint8_t scalar_sum[SV6621_SAE_SCALAR_SIZE];
  uint8_t key_material[SV6621_SAE_KEY_MATERIAL_SIZE];
  mbedtls_ecp_group group;
  mbedtls_mpi own;
  mbedtls_mpi peer;
  mbedtls_mpi sum;
  int ret;

  if (secret == NULL || own_scalar == NULL || peer_scalar == NULL ||
      kck == NULL || pmk == NULL || pmkid == NULL)
    {
      return -EINVAL;
    }

  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&own);
  mbedtls_mpi_init(&peer);
  mbedtls_mpi_init(&sum);
  ret = mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1);
  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&own, own_scalar, SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0)
    {
      ret =
          mbedtls_mpi_read_binary(&peer, peer_scalar, SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0 && (mbedtls_ecp_check_privkey(&group, &own) != 0 ||
                   mbedtls_ecp_check_privkey(&group, &peer) != 0 ||
                   mbedtls_mpi_cmp_int(&own, 1) <= 0 ||
                   mbedtls_mpi_cmp_int(&peer, 1) <= 0))
    {
      ret = -EINVAL;
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_add_mpi(&sum, &own, &peer);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&sum, &sum, &group.N);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_write_binary(&sum, scalar_sum, sizeof(scalar_sum));
    }

  if (ret == 0)
    {
      ret = sv6621_sae_hmac_sha256(zero_key, sizeof(zero_key), secret,
                                   SV6621_SAE_SCALAR_SIZE, keyseed);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_kdf_hash_length(
          keyseed, sizeof(keyseed), g_sv6621_sae_key_label,
          sizeof(g_sv6621_sae_key_label) - 1, scalar_sum, sizeof(scalar_sum),
          key_material, sizeof(key_material));
    }

  if (ret == 0)
    {
      memcpy(kck, key_material, SV6621_SAE_KCK_SIZE);
      memcpy(pmk, key_material + SV6621_SAE_KCK_SIZE, SV6621_SAE_PMK_SIZE);
      memcpy(pmkid, scalar_sum, SV6621_SAE_PMKID_SIZE);
    }
  else
    {
      if (ret != -EINVAL)
        {
          ret = -EIO;
        }

      sv6621_sae_zeroize(kck, SV6621_SAE_KCK_SIZE);
      sv6621_sae_zeroize(pmk, SV6621_SAE_PMK_SIZE);
      sv6621_sae_zeroize(pmkid, SV6621_SAE_PMKID_SIZE);
    }

  mbedtls_mpi_free(&sum);
  mbedtls_mpi_free(&peer);
  mbedtls_mpi_free(&own);
  mbedtls_ecp_group_free(&group);
  sv6621_sae_zeroize(zero_key, sizeof(zero_key));
  sv6621_sae_zeroize(keyseed, sizeof(keyseed));
  sv6621_sae_zeroize(scalar_sum, sizeof(scalar_sum));
  sv6621_sae_zeroize(key_material, sizeof(key_material));
  return ret;
}

int sv6621_sae_compute_confirm(
    FAR const uint8_t kck[SV6621_SAE_KCK_SIZE], uint16_t counter,
    FAR const uint8_t first_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t first_element[SV6621_SAE_ELEMENT_SIZE],
    FAR const uint8_t second_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t second_element[SV6621_SAE_ELEMENT_SIZE],
    uint8_t confirm[SV6621_SAE_CONFIRM_SIZE])
{
  uint8_t input[SV6621_SAE_CONFIRM_INPUT_SIZE];
  size_t offset = 0;
  int ret;

  if (kck == NULL || first_scalar == NULL || first_element == NULL ||
      second_scalar == NULL || second_element == NULL || confirm == NULL)
    {
      return -EINVAL;
    }

  input[offset++] = counter;
  input[offset++] = counter >> 8;
  memcpy(input + offset, first_scalar, SV6621_SAE_SCALAR_SIZE);
  offset += SV6621_SAE_SCALAR_SIZE;
  memcpy(input + offset, first_element, SV6621_SAE_ELEMENT_SIZE);
  offset += SV6621_SAE_ELEMENT_SIZE;
  memcpy(input + offset, second_scalar, SV6621_SAE_SCALAR_SIZE);
  offset += SV6621_SAE_SCALAR_SIZE;
  memcpy(input + offset, second_element, SV6621_SAE_ELEMENT_SIZE);

  ret = sv6621_sae_hmac_sha256(kck, SV6621_SAE_KCK_SIZE, input, sizeof(input),
                               confirm);
  sv6621_sae_zeroize(input, sizeof(input));
  return ret;
}
