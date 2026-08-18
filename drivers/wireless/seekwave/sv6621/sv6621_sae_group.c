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

#include <mbedtls/ecp.h>

#include "sv6621_sae_crypto.h"
#include "sv6621_sae_group.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_GROUP_COMMIT_ATTEMPTS 8

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
