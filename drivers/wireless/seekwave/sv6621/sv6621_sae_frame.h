/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sae_frame.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_FRAME_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_FRAME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_sae_auth_frame_s
{
  uint8_t destination[SV6621_MAC_LENGTH];
  uint8_t source[SV6621_MAC_LENGTH];
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint16_t transaction;
  uint16_t status;
  FAR const uint8_t *body;
  size_t body_length;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sae_auth_parse(FAR const uint8_t *frame, size_t frame_length,
                          FAR struct sv6621_sae_auth_frame_s *auth);
int sv6621_sae_auth_build(
    FAR const uint8_t destination[SV6621_MAC_LENGTH],
    FAR const uint8_t source[SV6621_MAC_LENGTH],
    FAR const uint8_t bssid[SV6621_MAC_LENGTH], uint16_t transaction,
    uint16_t status, FAR const uint8_t *body, size_t body_length,
    FAR uint8_t *frame, size_t capacity, FAR size_t *frame_length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_FRAME_H */
