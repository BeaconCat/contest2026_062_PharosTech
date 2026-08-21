/****************************************************************************
 * drivers/drivers/sv6621/sv6621_management.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MANAGEMENT_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MANAGEMENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621.h"
#include "sv6621_command.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_management_tx_status_s
{
  uint64_t cookie;
  bool acknowledged;
  FAR const uint8_t *frame;
  size_t frame_length;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_management_tx(FAR struct sv6621_command_engine_s *command,
                         uint8_t instance, uint32_t wait_ms, uint64_t cookie,
                         uint8_t channel, enum sv6621_band_e band, bool no_ack,
                         FAR const uint8_t *frame, size_t frame_length,
                         size_t total_frame_length);
int sv6621_management_parse_tx_status(
    FAR const uint8_t *payload, size_t payload_length,
    FAR struct sv6621_management_tx_status_s *status);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MANAGEMENT_H */
