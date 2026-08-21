/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_event.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_EVENT_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_EVENT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621_command.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_EVENT_RX_MGMT          4
#define SV6621_AP_EVENT_DEL_STA          9
#define SV6621_AP_EVENT_MGMT_TX_STATUS  12
#define SV6621_AP_EVENT_EAPOL          254

#define SV6621_AP_EVENT_QUEUE_DEPTH      8
#define SV6621_AP_EVENT_MAX_PAYLOAD \
  (SV6621_COMMAND_MAX_MESSAGE_SIZE - SV6621_COMMAND_HEADER_SIZE)

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef int (*sv6621_ap_event_handler_t)(uint8_t instance, uint8_t id,
                                         FAR const uint8_t *payload,
                                         size_t length, FAR void *arg);
typedef void (*sv6621_ap_event_error_t)(int error, FAR void *arg);

struct sv6621_ap_event_slot_s
{
  size_t length;
  uint8_t instance;
  uint8_t id;
  uint8_t payload[SV6621_AP_EVENT_MAX_PAYLOAD];
};

struct sv6621_ap_event_queue_s
{
  mutex_t lock;
  struct work_s work;
  sv6621_ap_event_handler_t handler;
  sv6621_ap_event_error_t error;
  FAR void *arg;
  struct sv6621_ap_event_slot_s slots[SV6621_AP_EVENT_QUEUE_DEPTH];
  uint8_t head;
  uint8_t tail;
  uint8_t count;
  bool work_scheduled;
  bool stopping;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ap_event_queue_init(FAR struct sv6621_ap_event_queue_s *queue,
                               sv6621_ap_event_handler_t handler,
                               sv6621_ap_event_error_t error,
                               FAR void *arg);
void sv6621_ap_event_queue_deinit(FAR struct sv6621_ap_event_queue_s *queue);
int sv6621_ap_event_queue_reset(FAR struct sv6621_ap_event_queue_s *queue);
int sv6621_ap_event_queue_submit(FAR struct sv6621_ap_event_queue_s *queue,
                                 uint8_t instance, uint8_t id,
                                 FAR const uint8_t *payload, size_t length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_AP_EVENT_H */
