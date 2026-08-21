/****************************************************************************
 * drivers/drivers/sv6621/sv6621_ap_sae.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_SAE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_SAE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621.h"
#include "sv6621_command.h"
#include "sv6621_sae_frame.h"
#include "sv6621_sae_keys.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_SAE_PEER_CAPACITY 8

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_ap_sae_state_e
{
  SV6621_AP_SAE_IDLE = 0,
  SV6621_AP_SAE_WAIT_CONFIRM,
  SV6621_AP_SAE_ACCEPTED
};

struct sv6621_ap_sae_peer_s
{
  uint8_t address[SV6621_MAC_LENGTH];
  uint8_t pwe[SV6621_SAE_ELEMENT_SIZE];
  uint8_t private_random[SV6621_SAE_SCALAR_SIZE];
  uint8_t scalar[SV6621_SAE_SCALAR_SIZE];
  uint8_t element[SV6621_SAE_ELEMENT_SIZE];
  uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE];
  uint8_t peer_element[SV6621_SAE_ELEMENT_SIZE];
  uint8_t kck[SV6621_SAE_KCK_SIZE];
  uint8_t pmk[SV6621_SAE_PMK_SIZE];
  uint8_t pmkid[SV6621_SAE_PMKID_SIZE];
  uint16_t last_peer_confirm;
  enum sv6621_ap_sae_state_e state;
};

struct sv6621_ap_sae_s
{
  mutex_t lock;
  FAR struct sv6621_command_engine_s *command;
  struct sv6621_ap_sae_peer_s peers[SV6621_AP_SAE_PEER_CAPACITY];
  uint8_t address[SV6621_MAC_LENGTH];
  uint8_t password[SV6621_KEY_MAX_LENGTH];
  size_t password_length;
  uint64_t next_cookie;
  uint8_t instance;
  uint8_t channel;
  enum sv6621_band_e band;
  bool enabled;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_sae_init(FAR struct sv6621_ap_sae_s *sae,
                       FAR struct sv6621_command_engine_s *command,
                       FAR const uint8_t address[SV6621_MAC_LENGTH]);
void sv6621_ap_sae_deinit(FAR struct sv6621_ap_sae_s *sae);
int sv6621_ap_sae_enable(FAR struct sv6621_ap_sae_s *sae, uint8_t instance,
                         uint8_t channel, enum sv6621_band_e band,
                         FAR const uint8_t *password, size_t password_length);
void sv6621_ap_sae_disable(FAR struct sv6621_ap_sae_s *sae);
int sv6621_ap_sae_input(FAR struct sv6621_ap_sae_s *sae,
                        FAR const uint8_t *frame, size_t frame_length,
                        FAR bool *accepted,
                        FAR uint8_t address[SV6621_MAC_LENGTH]);
int sv6621_ap_sae_get_pmk(FAR struct sv6621_ap_sae_s *sae,
                          FAR const uint8_t address[SV6621_MAC_LENGTH],
                          uint8_t pmk[SV6621_SAE_PMK_SIZE]);
void sv6621_ap_sae_forget(FAR struct sv6621_ap_sae_s *sae,
                          FAR const uint8_t address[SV6621_MAC_LENGTH]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_SAE_H */
