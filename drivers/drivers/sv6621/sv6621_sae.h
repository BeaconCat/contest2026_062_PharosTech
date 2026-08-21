/****************************************************************************
 * drivers/drivers/sv6621/sv6621_sae.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

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

#define SV6621_SAE_TOKEN_CAPACITY 256
#define SV6621_SAE_FRAME_CAPACITY 512

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_sae_state_e
{
  SV6621_SAE_IDLE = 0,
  SV6621_SAE_COMMIT_SENT,
  SV6621_SAE_CONFIRM_SENT,
  SV6621_SAE_ACCEPTED,
  SV6621_SAE_FAILED
};

typedef void (*sv6621_sae_complete_t)(int result, FAR const uint8_t *pmk,
                                      FAR const uint8_t *pmkid, FAR void *arg);

struct sv6621_sae_s
{
  mutex_t lock;
  struct work_s work;
  struct work_s timeout_work;
  FAR struct sv6621_command_engine_s *command;
  sv6621_sae_complete_t complete;
  FAR void *complete_arg;
  enum sv6621_sae_state_e state;
  uint8_t local[SV6621_MAC_LENGTH];
  uint8_t peer[SV6621_MAC_LENGTH];
  uint8_t channel;
  enum sv6621_band_e band;
  uint8_t instance;
  uint8_t pwe[SV6621_SAE_ELEMENT_SIZE];
  uint8_t private_random[SV6621_SAE_SCALAR_SIZE];
  uint8_t scalar[SV6621_SAE_SCALAR_SIZE];
  uint8_t element[SV6621_SAE_ELEMENT_SIZE];
  uint8_t peer_scalar[SV6621_SAE_SCALAR_SIZE];
  uint8_t peer_element[SV6621_SAE_ELEMENT_SIZE];
  uint8_t kck[SV6621_SAE_KCK_SIZE];
  uint8_t pmk[SV6621_SAE_PMK_SIZE];
  uint8_t pmkid[SV6621_SAE_PMKID_SIZE];
  uint8_t token[SV6621_SAE_TOKEN_CAPACITY];
  size_t token_length;
  uint8_t pending_frame[SV6621_SAE_FRAME_CAPACITY];
  size_t pending_frame_length;
  uint64_t cookie;
  uint32_t generation;
  uint16_t send_confirm;
  uint8_t retries;
  bool frame_pending;
  bool shutting_down;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sae_init(FAR struct sv6621_sae_s *sae,
                    FAR struct sv6621_command_engine_s *command,
                    sv6621_sae_complete_t complete, FAR void *complete_arg);
void sv6621_sae_deinit(FAR struct sv6621_sae_s *sae);
int sv6621_sae_start(FAR struct sv6621_sae_s *sae,
                     FAR const uint8_t local[SV6621_MAC_LENGTH],
                     FAR const uint8_t peer[SV6621_MAC_LENGTH],
                     uint8_t instance, uint8_t channel,
                     enum sv6621_band_e band, FAR const uint8_t *password,
                     size_t password_length);
int sv6621_sae_input(FAR struct sv6621_sae_s *sae, FAR const uint8_t *frame,
                     size_t frame_length);
void sv6621_sae_cancel(FAR struct sv6621_sae_s *sae, int result);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SAE_H */
