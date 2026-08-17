/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_command.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_COMMAND_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_COMMAND_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_COMMAND_HEADER_SIZE         8
#define SV6621_COMMAND_ACK_STATUS_SIZE     2
#define SV6621_COMMAND_MAX_MESSAGE_SIZE    1588

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum sv6621_message_type_e
{
  SV6621_MESSAGE_COMMAND = 0,
  SV6621_MESSAGE_COMMAND_ACK,
  SV6621_MESSAGE_EVENT,
  SV6621_MESSAGE_EVENT_LOCAL
};

struct sv6621_message_header_s
{
  uint8_t instance;
  enum sv6621_message_type_e type;
  uint8_t id;
  uint16_t sequence;
  uint16_t total_length;
};

struct sv6621_command_stats_s
{
  uint32_t commands;
  uint32_t timeouts;
  uint32_t cancelled;
  uint32_t events;
  uint32_t malformed;
  uint32_t stale_acknowledgements;
};

typedef int (*sv6621_command_sender_t)(FAR const uint8_t *packet,
                                       size_t length, FAR void *arg);
typedef void (*sv6621_command_event_t)(uint8_t instance, uint8_t id,
                                       FAR const uint8_t *payload,
                                       size_t length, FAR void *arg);

struct sv6621_command_engine_s
{
  mutex_t execute_lock;
  mutex_t state_lock;
  sem_t completion;
  sv6621_command_sender_t sender;
  FAR void *sender_arg;
  sv6621_command_event_t event;
  FAR void *event_arg;
  uint16_t next_sequence;
  uint16_t pending_sequence;
  uint8_t pending_id;
  bool pending;
  bool dispatching_event;
  int completion_result;
  uint16_t firmware_status;
  FAR uint8_t *response;
  size_t response_capacity;
  size_t response_length;
  struct sv6621_command_stats_s stats;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_command_encode_header(
    FAR const struct sv6621_message_header_s *header,
    uint8_t encoded[SV6621_COMMAND_HEADER_SIZE]);
int sv6621_command_decode_header(
    FAR const uint8_t encoded[SV6621_COMMAND_HEADER_SIZE], size_t available,
    FAR struct sv6621_message_header_s *header);
int sv6621_command_engine_init(FAR struct sv6621_command_engine_s *engine,
                               sv6621_command_sender_t sender,
                               FAR void *sender_arg,
                               sv6621_command_event_t event,
                               FAR void *event_arg);
void sv6621_command_engine_deinit(FAR struct sv6621_command_engine_s *engine);
int sv6621_command_execute(FAR struct sv6621_command_engine_s *engine,
                           uint8_t instance, uint8_t id,
                           FAR const void *payload, size_t payload_length,
                           FAR void *response, FAR size_t *response_length,
                           uint32_t timeout_ms);
int sv6621_command_cancel(FAR struct sv6621_command_engine_s *engine,
                          int result);
int sv6621_command_receive(FAR struct sv6621_command_engine_s *engine,
                           FAR const uint8_t *message, size_t length);
void sv6621_command_channel_consumer(uint8_t channel,
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_COMMAND_H */
