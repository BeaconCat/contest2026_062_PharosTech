/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_data.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_DATA_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_DATA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/spinlock.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sv6621_packet.h"
#include "sv6621_tx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_DATA_RX_DESCRIPTOR_SIZE 20
#define SV6621_DATA_RX_PREFIX_SIZE     52
#define SV6621_DATA_TX_DESCRIPTOR_SIZE 8
#define SV6621_DATA_MAX_FRAME_SIZE     1536
#define SV6621_DATA_TX_BUFFER_SIZE     2048
#define SV6621_DATA_LMAC_COUNT         2

#define SV6621_DATA_TX_BLOCK_SLEEP     (1 << 0)
#define SV6621_DATA_TX_BLOCK_THERMAL   (1 << 1)
#define SV6621_DATA_TX_BLOCK_RECOVERY  (1 << 2)
#define SV6621_DATA_TX_BLOCK_CHANNEL   (1 << 3)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_data_rx_s
{
  FAR const uint8_t *frame;
  size_t frame_length;
  uint16_t sequence;
  uint8_t fragment;
  uint8_t instance;
  uint8_t peer_index;
  uint8_t tid;
  bool instance_valid;
  bool peer_valid;
  bool multicast;
  bool eapol;
};

struct sv6621_data_tx_context_s
{
  uint8_t peer_index;
  uint8_t multicast_index;
  uint8_t instance;
  uint8_t lmac_id;
  uint8_t tid;
};

typedef void (*sv6621_data_input_t)(FAR const struct sv6621_data_rx_s *rx,
                                    FAR void *arg);

struct sv6621_data_stats_s
{
  uint32_t received;
  uint32_t received_bytes;
  uint32_t malformed;
  uint32_t transmitted;
  uint32_t transmitted_bytes;
  uint32_t transmit_errors;
  uint32_t credit_updates;
  uint32_t credit_starvations;
};

struct sv6621_data_s
{
  FAR struct sv6621_packet_router_s *router;
  FAR struct sv6621_tx_s *tx;
  mutex_t tx_lock;
  spinlock_t credit_lock;
  sv6621_data_input_t input;
  FAR void *input_arg;
  sv6621_data_input_t eapol_input;
  FAR void *eapol_arg;
  struct sv6621_data_stats_s stats;
  uint16_t credits[SV6621_DATA_LMAC_COUNT];
  uint8_t tx_block_reasons;
  uint8_t tx_buffer[SV6621_DATA_TX_BUFFER_SIZE];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_data_decode_rx(FAR const uint8_t *payload, size_t length,
                          FAR struct sv6621_data_rx_s *rx);
int sv6621_data_encode_tx(
    FAR const struct sv6621_data_tx_context_s *context,
    FAR const uint8_t *frame, size_t frame_length, FAR uint8_t *payload,
    size_t capacity, FAR size_t *written);
int sv6621_data_init(FAR struct sv6621_data_s *data,
                     FAR struct sv6621_packet_router_s *router,
                     FAR struct sv6621_tx_s *tx, sv6621_data_input_t input,
                     FAR void *input_arg);
void sv6621_data_deinit(FAR struct sv6621_data_s *data);
void sv6621_data_set_eapol_input(FAR struct sv6621_data_s *data,
                                  sv6621_data_input_t input, FAR void *arg);
void sv6621_data_add_credits(FAR struct sv6621_data_s *data,
                             uint16_t lmac0, uint16_t lmac1);
void sv6621_data_reset_credits(FAR struct sv6621_data_s *data);
int sv6621_data_set_tx_block(FAR struct sv6621_data_s *data, uint8_t reason,
                             bool blocked);
int sv6621_data_send(FAR struct sv6621_data_s *data,
                     FAR const struct sv6621_data_tx_context_s *context,
                     FAR const uint8_t *frame, size_t frame_length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_DATA_H */
