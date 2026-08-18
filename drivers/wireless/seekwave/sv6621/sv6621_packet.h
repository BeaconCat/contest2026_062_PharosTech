/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_packet.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PACKET_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PACKET_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621_protocol.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*sv6621_packet_consumer_t)(
    uint8_t channel, FAR const uint8_t encoded[SV6621_PACKET_HEADER_SIZE],
    FAR const uint8_t *payload, size_t length, FAR void *arg);

struct sv6621_packet_consumer_s
{
  sv6621_packet_consumer_t callback;
  FAR void *arg;
};

struct sv6621_packet_router_s
{
  mutex_t lock;
  struct sv6621_packet_consumer_s consumer[SV6621_PACKET_CHANNEL_COUNT];
  uint32_t received[SV6621_PACKET_CHANNEL_COUNT];
  uint32_t dropped[SV6621_PACKET_CHANNEL_COUNT];
  uint32_t malformed;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_packet_router_init(FAR struct sv6621_packet_router_s *router);
void sv6621_packet_router_deinit(FAR struct sv6621_packet_router_s *router);
int sv6621_packet_subscribe(FAR struct sv6621_packet_router_s *router,
                            uint8_t channel, sv6621_packet_consumer_t callback,
                            FAR void *arg);
int sv6621_packet_unsubscribe(FAR struct sv6621_packet_router_s *router,
                              uint8_t channel,
                              sv6621_packet_consumer_t callback,
                              FAR void *arg);
int sv6621_packet_dispatch(FAR struct sv6621_packet_router_s *router,
                           FAR const uint8_t encoded[4],
                           FAR const uint8_t *payload, size_t available);
int sv6621_packet_build(uint8_t channel, FAR const void *payload,
                        size_t length, FAR uint8_t *buffer, size_t capacity,
                        FAR size_t *written);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PACKET_H */
