/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wpa_crypto.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_CRYPTO_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_CRYPTO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WPA_SHA1_SIZE   20
#define SV6621_WPA_SHA256_SIZE 32
#define SV6621_WPA_PMK_SIZE    32
#define SV6621_WPA_NONCE_SIZE  32
#define SV6621_WPA_PTK_SIZE    48
#define SV6621_WPA_KEK_SIZE    16

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_wpa_hmac_sha1(FAR const uint8_t *key, size_t key_length,
                         FAR const uint8_t *data, size_t data_length,
                         uint8_t output[SV6621_WPA_SHA1_SIZE]);
int sv6621_wpa_hmac_sha256(FAR const uint8_t *key, size_t key_length,
                           FAR const uint8_t *data, size_t data_length,
                           uint8_t output[SV6621_WPA_SHA256_SIZE]);
int sv6621_wpa_aes_cmac(FAR const uint8_t key[16], FAR const uint8_t *data,
                        size_t data_length, uint8_t output[16]);
int sv6621_wpa_derive_pmk(FAR const uint8_t *passphrase,
                          size_t passphrase_length, FAR const uint8_t *ssid,
                          size_t ssid_length,
                          uint8_t pmk[SV6621_WPA_PMK_SIZE]);
int sv6621_wpa_derive_ptk(FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
                          FAR const uint8_t authenticator[6],
                          FAR const uint8_t supplicant[6],
                          FAR const uint8_t anonce[SV6621_WPA_NONCE_SIZE],
                          FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
                          uint8_t ptk[SV6621_WPA_PTK_SIZE]);
int sv6621_wpa_derive_ptk_sha256(
    FAR const uint8_t pmk[SV6621_WPA_PMK_SIZE],
    FAR const uint8_t authenticator[6], FAR const uint8_t supplicant[6],
    FAR const uint8_t anonce[SV6621_WPA_NONCE_SIZE],
    FAR const uint8_t snonce[SV6621_WPA_NONCE_SIZE],
    uint8_t ptk[SV6621_WPA_PTK_SIZE]);
int sv6621_wpa_unwrap_key(FAR const uint8_t kek[SV6621_WPA_KEK_SIZE],
                          FAR const uint8_t *wrapped, size_t wrapped_length,
                          FAR uint8_t *plain, size_t capacity,
                          FAR size_t *plain_length);
int sv6621_wpa_wrap_key(FAR const uint8_t kek[SV6621_WPA_KEK_SIZE],
                        FAR const uint8_t *plain, size_t plain_length,
                        FAR uint8_t *wrapped, size_t capacity,
                        FAR size_t *wrapped_length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_WPA_CRYPTO_H */
