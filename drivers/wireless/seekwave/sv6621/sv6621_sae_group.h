/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_group.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_GROUP_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_GROUP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "sv6621_sae_frame.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sae_group_validate_element(
    FAR const uint8_t element[SV6621_SAE_ELEMENT_SIZE]);
int sv6621_sae_group_derive_pwe(
    FAR const uint8_t address1[SV6621_MAC_LENGTH],
    FAR const uint8_t address2[SV6621_MAC_LENGTH],
    FAR const uint8_t *password, size_t password_length,
    uint8_t pwe[SV6621_SAE_ELEMENT_SIZE]);
int sv6621_sae_group_generate_commit(
    FAR const uint8_t pwe[SV6621_SAE_ELEMENT_SIZE],
    uint8_t private_random[SV6621_SAE_SCALAR_SIZE],
    uint8_t scalar[SV6621_SAE_SCALAR_SIZE],
    uint8_t element[SV6621_SAE_ELEMENT_SIZE]);
int sv6621_sae_group_derive_secret(
    FAR const uint8_t pwe[SV6621_SAE_ELEMENT_SIZE],
    FAR const uint8_t private_random[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t own_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE],
    FAR const uint8_t peer_element[SV6621_SAE_ELEMENT_SIZE],
    uint8_t secret[SV6621_SAE_SCALAR_SIZE]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_GROUP_H */
