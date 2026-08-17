/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_offload.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_OFFLOAD_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_OFFLOAD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621_command.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_OFFLOAD_IPV4_LENGTH 4
#define SV6621_OFFLOAD_IPV6_LENGTH 16
#define SV6621_OFFLOAD_IPV6_LIMIT  3

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_offload_addresses_s
{
  uint8_t ipv4[SV6621_OFFLOAD_IPV4_LENGTH];
  uint8_t ipv6[SV6621_OFFLOAD_IPV6_LIMIT][SV6621_OFFLOAD_IPV6_LENGTH];
  size_t ipv6_count;
  bool ipv4_valid;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_offload_set_addresses(
    FAR struct sv6621_command_engine_s *command, uint8_t instance,
    FAR const struct sv6621_offload_addresses_s *addresses);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_OFFLOAD_H */
