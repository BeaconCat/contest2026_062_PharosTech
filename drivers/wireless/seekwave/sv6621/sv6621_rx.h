/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_rx.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_RX_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_RX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/wqueue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621_packet.h"
#include "sv6621_transport.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_rx_stats_s
{
  uint32_t interrupts;
  uint32_t bursts;
  uint32_t packets;
  uint32_t drain_completions;
  uint32_t malformed_bursts;
  uint32_t transport_errors;
  uint32_t last_valid_length;
  uint32_t last_pending_count;
};

typedef void (*sv6621_rx_error_t)(int error, FAR void *arg);

struct sv6621_rx_s
{
  FAR struct sv6621_transport_s *transport;
  FAR struct sv6621_packet_router_s *router;
  FAR uint8_t *buffer;
  struct work_s work;
  struct sv6621_rx_stats_s stats;
  sv6621_rx_error_t error;
  FAR void *error_arg;
  volatile bool running;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_rx_init(FAR struct sv6621_rx_s *rx,
                   FAR struct sv6621_transport_s *transport,
                   FAR struct sv6621_packet_router_s *router,
                   sv6621_rx_error_t error, FAR void *error_arg);
void sv6621_rx_deinit(FAR struct sv6621_rx_s *rx);
int sv6621_rx_start(FAR struct sv6621_rx_s *rx);
void sv6621_rx_stop(FAR struct sv6621_rx_s *rx);
int sv6621_rx_parse_burst(FAR struct sv6621_rx_s *rx,
                          FAR const uint8_t *buffer, size_t length,
                          unsigned int slots, FAR uint32_t *pending_count);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_RX_H */
