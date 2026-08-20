/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_sched_scan.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/mutex.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_command.h"
#include "sv6621_scan.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void (*sv6621_sched_scan_complete_t)(uint32_t request_id,
                                              FAR void *arg);
typedef void (*sv6621_sched_scan_result_t)(
    FAR const struct sv6621_bss_s *bss, FAR void *arg);

struct sv6621_sched_scan_s
{
  mutex_t lock;
  FAR struct sv6621_command_engine_s *command;
  FAR struct sv6621_scan_cache_s *cache;
  sv6621_sched_scan_result_t result;
  sv6621_sched_scan_complete_t complete;
  FAR void *complete_arg;
  uint32_t request_id;
  uint32_t generation;
  bool active;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_sched_scan_encode(
    FAR const struct sv6621_sched_scan_request_s *request,
    FAR uint8_t *payload, size_t capacity, FAR size_t *written);
int sv6621_sched_scan_start(
    FAR struct sv6621_command_engine_s *command,
    FAR const struct sv6621_sched_scan_request_s *request);
int sv6621_sched_scan_stop(FAR struct sv6621_command_engine_s *command);
int sv6621_sched_scan_init(FAR struct sv6621_sched_scan_s *scan,
                           FAR struct sv6621_command_engine_s *command,
                           FAR struct sv6621_scan_cache_s *cache,
                           sv6621_sched_scan_result_t result,
                           sv6621_sched_scan_complete_t complete,
                           FAR void *complete_arg);
void sv6621_sched_scan_deinit(FAR struct sv6621_sched_scan_s *scan);
int sv6621_sched_scan_begin(
    FAR struct sv6621_sched_scan_s *scan,
    FAR const struct sv6621_sched_scan_request_s *request);
int sv6621_sched_scan_cancel(FAR struct sv6621_sched_scan_s *scan);
void sv6621_sched_scan_command_event(uint8_t instance, uint8_t id,
                                     FAR const uint8_t *payload,
                                     size_t length, FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_SCHED_SCAN_H */
