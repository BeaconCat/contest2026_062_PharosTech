/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_keys.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_KEYS_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_KEYS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621_sae_crypto.h"
#include "sv6621_sae_frame.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SAE_KCK_SIZE   32
#define SV6621_SAE_PMK_SIZE   32
#define SV6621_SAE_PMKID_SIZE 16

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sae_derive_keys(
    FAR const uint8_t secret[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t own_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE],
    uint8_t kck[SV6621_SAE_KCK_SIZE], uint8_t pmk[SV6621_SAE_PMK_SIZE],
    uint8_t pmkid[SV6621_SAE_PMKID_SIZE]);
int sv6621_sae_compute_confirm(
    FAR const uint8_t kck[SV6621_SAE_KCK_SIZE], uint16_t counter,
    FAR const uint8_t first_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t first_element[SV6621_SAE_ELEMENT_SIZE],
    FAR const uint8_t second_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t second_element[SV6621_SAE_ELEMENT_SIZE],
    uint8_t confirm[SV6621_SAE_CONFIRM_SIZE]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_KEYS_H */
