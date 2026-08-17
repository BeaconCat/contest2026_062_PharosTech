/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_tx.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TX_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621_transport.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_tx_stats_s
{
  uint32_t packets;
  uint64_t bytes;
  uint32_t transport_errors;
  uint32_t doorbell_errors;
};

struct sv6621_tx_s
{
  FAR struct sv6621_transport_s *transport;
  mutex_t lock;
  struct sv6621_tx_stats_s stats;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_tx_init(FAR struct sv6621_tx_s *tx,
                   FAR struct sv6621_transport_s *transport);
void sv6621_tx_deinit(FAR struct sv6621_tx_s *tx);
int sv6621_tx_send(FAR struct sv6621_tx_s *tx, FAR const uint8_t *packet,
                   size_t length);
int sv6621_tx_command_sender(FAR const uint8_t *packet, size_t length,
                             FAR void *arg);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TX_H */
