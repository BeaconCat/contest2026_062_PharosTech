/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_group.c
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

#include "sv6621_sae_crypto.h"
#include "sv6621_sae_group.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_GROUP_COMMIT_ATTEMPTS 8
#define SV6621_SAE_GROUP_PWE_ITERATIONS   40
#define SV6621_SAE_GROUP_PASSWORD_MAX    63

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_sae_pwe_label[] = "SAE Hunting and Pecking";

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_sae_group_load(mbedtls_ecp_group *group);
static int sv6621_sae_group_read_point(
    FAR const mbedtls_ecp_group *group,
    FAR const uint8_t encoded[SV6621_SAE_ELEMENT_SIZE],
    FAR mbedtls_ecp_point *point);
static int sv6621_sae_group_write_point(
    FAR const mbedtls_ecp_point *point,
    uint8_t encoded[SV6621_SAE_ELEMENT_SIZE]);
static int sv6621_sae_group_test_seed(
    FAR const mbedtls_ecp_group *group, FAR const mbedtls_mpi *sqrt_exponent,
    FAR const uint8_t prime[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t seed[SV6621_SAE_SHA256_SIZE],
    uint8_t element[SV6621_SAE_ELEMENT_SIZE], FAR bool *valid);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int sv6621_sae_group_load(mbedtls_ecp_group *group)
{
  int ret = mbedtls_ecp_group_load(group, MBEDTLS_ECP_DP_SECP256R1);

  return ret == 0 ? 0 : -EIO;
}

static int sv6621_sae_group_read_point(
    FAR const mbedtls_ecp_group *group,
    FAR const uint8_t encoded[SV6621_SAE_ELEMENT_SIZE],
    FAR mbedtls_ecp_point *point)
{
  int ret;

  ret = mbedtls_mpi_read_binary(&point->X, encoded, SV6621_SAE_SCALAR_SIZE);
  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&point->Y,
                                    encoded + SV6621_SAE_SCALAR_SIZE,
                                    SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_lset(&point->Z, 1);
    }

  if (ret == 0)
    {
      ret = mbedtls_ecp_check_pubkey(group, point);
    }

  return ret == 0 ? 0 : -EINVAL;
}

static int sv6621_sae_group_write_point(
    FAR const mbedtls_ecp_point *point,
    uint8_t encoded[SV6621_SAE_ELEMENT_SIZE])
{
  int ret;

  ret = mbedtls_mpi_write_binary(&point->X, encoded,
                                 SV6621_SAE_SCALAR_SIZE);
  if (ret == 0)
    {
      ret = mbedtls_mpi_write_binary(&point->Y,
                                     encoded + SV6621_SAE_SCALAR_SIZE,
                                     SV6621_SAE_SCALAR_SIZE);
    }

  return ret == 0 ? 0 : -EIO;
}

static int sv6621_sae_group_test_seed(
    FAR const mbedtls_ecp_group *group, FAR const mbedtls_mpi *sqrt_exponent,
    FAR const uint8_t prime[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t seed[SV6621_SAE_SHA256_SIZE],
    uint8_t element[SV6621_SAE_ELEMENT_SIZE], FAR bool *valid)
{
  uint8_t candidate[SV6621_SAE_SCALAR_SIZE];
  mbedtls_ecp_point point;
  mbedtls_mpi x2;
  mbedtls_mpi rhs;
  mbedtls_mpi temporary;
  int ret;

  mbedtls_ecp_point_init(&point);
  mbedtls_mpi_init(&x2);
  mbedtls_mpi_init(&rhs);
  mbedtls_mpi_init(&temporary);
  *valid = false;

  ret = sv6621_sae_kdf_hash_length(
      seed, SV6621_SAE_SHA256_SIZE, g_sv6621_sae_pwe_label,
      sizeof(g_sv6621_sae_pwe_label) - 1, prime, SV6621_SAE_SCALAR_SIZE,
      candidate, sizeof(candidate));
  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&point.X, candidate, sizeof(candidate));
    }

  if (ret == 0 && mbedtls_mpi_cmp_mpi(&point.X, &group->P) >= 0)
    {
      goto done;
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mul_mpi(&x2, &point.X, &point.X);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&x2, &x2, &group->P);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mul_mpi(&rhs, &x2, &point.X);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&rhs, &rhs, &group->P);
    }

  if (ret == 0 && group->A.p == NULL)
    {
      ret = mbedtls_mpi_mul_int(&temporary, &point.X, 3);
    }

  if (ret == 0 && group->A.p == NULL)
    {
      ret = mbedtls_mpi_sub_mpi(&rhs, &rhs, &temporary);
    }
  else if (ret == 0)
    {
      ret = mbedtls_mpi_mul_mpi(&temporary, &group->A, &point.X);
      if (ret == 0)
        {
          ret = mbedtls_mpi_add_mpi(&rhs, &rhs, &temporary);
        }
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&rhs, &rhs, &group->P);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_add_mpi(&rhs, &rhs, &group->B);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&rhs, &rhs, &group->P);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_exp_mod(&point.Y, &rhs, sqrt_exponent, &group->P,
                                NULL);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mul_mpi(&temporary, &point.Y, &point.Y);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_mod_mpi(&temporary, &temporary, &group->P);
    }

  if (ret == 0 && mbedtls_mpi_cmp_mpi(&temporary, &rhs) != 0)
    {
      goto done;
    }

  if (ret == 0 && mbedtls_mpi_get_bit(&point.Y, 0) !=
                      (seed[SV6621_SAE_SHA256_SIZE - 1] & 1))
    {
      ret = mbedtls_mpi_sub_mpi(&point.Y, &group->P, &point.Y);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_lset(&point.Z, 1);
    }

  if (ret == 0)
    {
      ret = mbedtls_ecp_check_pubkey(group, &point);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_group_write_point(&point, element);
      *valid = ret == 0;
    }

done:
  sv6621_sae_zeroize(candidate, sizeof(candidate));
  mbedtls_mpi_free(&temporary);
  mbedtls_mpi_free(&rhs);
  mbedtls_mpi_free(&x2);
  mbedtls_ecp_point_free(&point);
  return ret == 0 ? 0 : (ret == -EINVAL ? ret : -EIO);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_sae_group_validate_element(
    FAR const uint8_t element[SV6621_SAE_ELEMENT_SIZE])
{
  mbedtls_ecp_group group;
  mbedtls_ecp_point point;
  int ret;

  if (element == NULL)
    {
      return -EINVAL;
    }

  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&point);
  ret = sv6621_sae_group_load(&group);
  if (ret == 0)
    {
      ret = sv6621_sae_group_read_point(&group, element, &point);
    }

  mbedtls_ecp_point_free(&point);
  mbedtls_ecp_group_free(&group);
  return ret;
}

int sv6621_sae_group_derive_pwe(
    FAR const uint8_t address1[SV6621_MAC_LENGTH],
    FAR const uint8_t address2[SV6621_MAC_LENGTH],
    FAR const uint8_t *password, size_t password_length,
    uint8_t pwe[SV6621_SAE_ELEMENT_SIZE])
{
  uint8_t address_key[SV6621_MAC_LENGTH * 2];
  uint8_t input[SV6621_SAE_GROUP_PASSWORD_MAX + 1];
  uint8_t seed[SV6621_SAE_SHA256_SIZE];
  uint8_t candidate[SV6621_SAE_ELEMENT_SIZE];
  uint8_t prime[SV6621_SAE_SCALAR_SIZE];
  mbedtls_ecp_group group;
  mbedtls_mpi sqrt_exponent;
  unsigned int counter;
  bool found = false;
  int ret;

  if (address1 == NULL || address2 == NULL || password == NULL ||
      password_length == 0 ||
      password_length > SV6621_SAE_GROUP_PASSWORD_MAX || pwe == NULL)
    {
      return -EINVAL;
    }

  if (memcmp(address1, address2, SV6621_MAC_LENGTH) > 0)
    {
      memcpy(address_key, address1, SV6621_MAC_LENGTH);
      memcpy(address_key + SV6621_MAC_LENGTH, address2, SV6621_MAC_LENGTH);
    }
  else
    {
      memcpy(address_key, address2, SV6621_MAC_LENGTH);
      memcpy(address_key + SV6621_MAC_LENGTH, address1, SV6621_MAC_LENGTH);
    }

  memcpy(input, password, password_length);
  memset(pwe, 0, SV6621_SAE_ELEMENT_SIZE);
  memset(candidate, 0, sizeof(candidate));
  mbedtls_ecp_group_init(&group);
  mbedtls_mpi_init(&sqrt_exponent);
  ret = sv6621_sae_group_load(&group);
  if (ret == 0)
    {
      ret = mbedtls_mpi_write_binary(&group.P, prime, sizeof(prime));
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_add_int(&sqrt_exponent, &group.P, 1);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_shift_r(&sqrt_exponent, 2);
    }

  for (counter = 1;
       ret == 0 && counter <= SV6621_SAE_GROUP_PWE_ITERATIONS; counter++)
    {
      bool valid = false;
      uint8_t select;
      size_t index;

      input[password_length] = counter;
      ret = sv6621_sae_hmac_sha256(address_key, sizeof(address_key), input,
                                    password_length + 1, seed);
      if (ret == 0)
        {
          ret = sv6621_sae_group_test_seed(&group, &sqrt_exponent, prime,
                                           seed, candidate, &valid);
        }

      select = (uint8_t)-(int)(valid && !found);
      for (index = 0; index < sizeof(candidate); index++)
        {
          pwe[index] = (pwe[index] & ~select) | (candidate[index] & select);
        }

      found = found || valid;
    }

  if (ret == 0 && !found)
    {
      ret = -EACCES;
    }
  else if (ret != 0 && ret != -EINVAL && ret != -EACCES)
    {
      ret = -EIO;
    }

  mbedtls_mpi_free(&sqrt_exponent);
  mbedtls_ecp_group_free(&group);
  sv6621_sae_zeroize(address_key, sizeof(address_key));
  sv6621_sae_zeroize(input, sizeof(input));
  sv6621_sae_zeroize(seed, sizeof(seed));
  sv6621_sae_zeroize(candidate, sizeof(candidate));
  sv6621_sae_zeroize(prime, sizeof(prime));
  if (ret != 0)
    {
      sv6621_sae_zeroize(pwe, SV6621_SAE_ELEMENT_SIZE);
    }

  return ret;
}

int sv6621_sae_group_generate_commit(
    FAR const uint8_t pwe[SV6621_SAE_ELEMENT_SIZE],
    uint8_t private_random[SV6621_SAE_SCALAR_SIZE],
    uint8_t scalar[SV6621_SAE_SCALAR_SIZE],
    uint8_t element[SV6621_SAE_ELEMENT_SIZE])
{
  mbedtls_ecp_group group;
  mbedtls_ecp_point password_element;
  mbedtls_ecp_point commit_element;
  mbedtls_mpi random;
  mbedtls_mpi mask;
  mbedtls_mpi commit_scalar;
  unsigned int attempt;
  int ret;

  if (pwe == NULL || private_random == NULL || scalar == NULL ||
      element == NULL)
    {
      return -EINVAL;
    }

  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&password_element);
  mbedtls_ecp_point_init(&commit_element);
  mbedtls_mpi_init(&random);
  mbedtls_mpi_init(&mask);
  mbedtls_mpi_init(&commit_scalar);

  ret = sv6621_sae_group_load(&group);
  if (ret == 0)
    {
      ret = sv6621_sae_group_read_point(&group, pwe, &password_element);
    }

  for (attempt = 0;
       ret == 0 && attempt < SV6621_SAE_GROUP_COMMIT_ATTEMPTS; attempt++)
    {
      ret = mbedtls_ecp_gen_privkey(&group, &random,
                                    sv6621_sae_random_callback, NULL);
      if (ret == 0)
        {
          ret = mbedtls_ecp_gen_privkey(&group, &mask,
                                        sv6621_sae_random_callback, NULL);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_add_mpi(&commit_scalar, &random, &mask);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_mod_mpi(&commit_scalar, &commit_scalar,
                                    &group.N);
        }

      if (ret == 0 && mbedtls_mpi_cmp_int(&commit_scalar, 1) <= 0)
        {
          continue;
        }

      if (ret == 0)
        {
          ret = mbedtls_ecp_mul(&group, &commit_element, &mask,
                                &password_element,
                                sv6621_sae_random_callback, NULL);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_sub_mpi(&commit_element.Y, &group.P,
                                    &commit_element.Y);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_mod_mpi(&commit_element.Y, &commit_element.Y,
                                    &group.P);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_write_binary(&random, private_random,
                                         SV6621_SAE_SCALAR_SIZE);
        }

      if (ret == 0)
        {
          ret = mbedtls_mpi_write_binary(&commit_scalar, scalar,
                                         SV6621_SAE_SCALAR_SIZE);
        }

      if (ret == 0)
        {
          ret = sv6621_sae_group_write_point(&commit_element, element);
        }

      break;
    }

  if (ret == 0 && attempt == SV6621_SAE_GROUP_COMMIT_ATTEMPTS)
    {
      ret = -EAGAIN;
    }
  else if (ret != 0 && ret != -EINVAL && ret != -EAGAIN)
    {
      ret = -EIO;
    }

  mbedtls_mpi_free(&commit_scalar);
  mbedtls_mpi_free(&mask);
  mbedtls_mpi_free(&random);
  mbedtls_ecp_point_free(&commit_element);
  mbedtls_ecp_point_free(&password_element);
  mbedtls_ecp_group_free(&group);
  if (ret != 0)
    {
      sv6621_sae_zeroize(private_random, SV6621_SAE_SCALAR_SIZE);
      sv6621_sae_zeroize(scalar, SV6621_SAE_SCALAR_SIZE);
      sv6621_sae_zeroize(element, SV6621_SAE_ELEMENT_SIZE);
    }

  return ret;
}

int sv6621_sae_group_derive_secret(
    FAR const uint8_t pwe[SV6621_SAE_ELEMENT_SIZE],
    FAR const uint8_t private_random[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t own_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_element[SV6621_SAE_ELEMENT_SIZE],
    uint8_t secret[SV6621_SAE_SCALAR_SIZE])
{
  mbedtls_ecp_group group;
  mbedtls_ecp_point password_element;
  mbedtls_ecp_point element;
  mbedtls_ecp_point sum;
  mbedtls_ecp_point shared;
  mbedtls_mpi random;
  mbedtls_mpi own;
  mbedtls_mpi peer;
  mbedtls_mpi one;
  int ret;

  if (pwe == NULL || private_random == NULL || own_scalar == NULL ||
      peer_scalar == NULL || peer_element == NULL || secret == NULL)
    {
      return -EINVAL;
    }

  mbedtls_ecp_group_init(&group);
  mbedtls_ecp_point_init(&password_element);
  mbedtls_ecp_point_init(&element);
  mbedtls_ecp_point_init(&sum);
  mbedtls_ecp_point_init(&shared);
  mbedtls_mpi_init(&random);
  mbedtls_mpi_init(&own);
  mbedtls_mpi_init(&peer);
  mbedtls_mpi_init(&one);

  ret = sv6621_sae_group_load(&group);
  if (ret == 0)
    {
      ret = sv6621_sae_group_read_point(&group, pwe, &password_element);
    }

  if (ret == 0)
    {
      ret = sv6621_sae_group_read_point(&group, peer_element, &element);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&random, private_random,
                                    SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&own, own_scalar,
                                    SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_read_binary(&peer, peer_scalar,
                                    SV6621_SAE_SCALAR_SIZE);
    }

  if (ret == 0 &&
      (mbedtls_ecp_check_privkey(&group, &random) != 0 ||
       mbedtls_ecp_check_privkey(&group, &own) != 0 ||
       mbedtls_ecp_check_privkey(&group, &peer) != 0 ||
       mbedtls_mpi_cmp_int(&own, 1) <= 0 ||
       mbedtls_mpi_cmp_int(&peer, 1) <= 0 ||
       mbedtls_mpi_cmp_mpi(&own, &peer) == 0))
    {
      ret = -EINVAL;
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_lset(&one, 1);
    }

  if (ret == 0)
    {
      ret = mbedtls_ecp_muladd(&group, &sum, &peer, &password_element,
                               &one, &element);
    }

  if (ret == 0 && mbedtls_ecp_is_zero(&sum))
    {
      ret = -EINVAL;
    }

  if (ret == 0)
    {
      ret = mbedtls_ecp_mul(&group, &shared, &random, &sum,
                            sv6621_sae_random_callback, NULL);
    }

  if (ret == 0 && mbedtls_ecp_is_zero(&shared))
    {
      ret = -EINVAL;
    }

  if (ret == 0)
    {
      ret = mbedtls_mpi_write_binary(&shared.X, secret,
                                     SV6621_SAE_SCALAR_SIZE);
    }

  if (ret != 0 && ret != -EINVAL)
    {
      ret = -EIO;
    }

  mbedtls_mpi_free(&one);
  mbedtls_mpi_free(&peer);
  mbedtls_mpi_free(&own);
  mbedtls_mpi_free(&random);
  mbedtls_ecp_point_free(&shared);
  mbedtls_ecp_point_free(&sum);
  mbedtls_ecp_point_free(&element);
  mbedtls_ecp_point_free(&password_element);
  mbedtls_ecp_group_free(&group);
  if (ret != 0)
    {
      sv6621_sae_zeroize(secret, SV6621_SAE_SCALAR_SIZE);
    }

  return ret;
}
