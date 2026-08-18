/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_crypto.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_CRYPTO_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_CRYPTO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_SHA256_SIZE 32

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sae_sha256(FAR const uint8_t *data, size_t data_length,
                       uint8_t output[SV6621_SAE_SHA256_SIZE]);
int sv6621_sae_hmac_sha256(
    FAR const uint8_t *key, size_t key_length, FAR const uint8_t *data,
    size_t data_length, uint8_t output[SV6621_SAE_SHA256_SIZE]);
int sv6621_sae_hkdf_extract(
    FAR const uint8_t *salt, size_t salt_length, FAR const uint8_t *input,
    size_t input_length, uint8_t output[SV6621_SAE_SHA256_SIZE]);
int sv6621_sae_hkdf_expand(
    FAR const uint8_t key[SV6621_SAE_SHA256_SIZE], FAR const uint8_t *info,
    size_t info_length, FAR uint8_t *output, size_t output_length);
int sv6621_sae_kdf_hash_length(
    FAR const uint8_t *key, size_t key_length, FAR const uint8_t *label,
    size_t label_length, FAR const uint8_t *context, size_t context_length,
    FAR uint8_t *output, size_t output_length);
int sv6621_sae_random(FAR uint8_t *output, size_t output_length);
int sv6621_sae_random_callback(FAR void *arg, FAR unsigned char *output,
                               size_t output_length);
bool sv6621_sae_constant_equal(FAR const uint8_t *left,
                               FAR const uint8_t *right, size_t length);
void sv6621_sae_zeroize(FAR void *data, size_t length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_CRYPTO_H */
