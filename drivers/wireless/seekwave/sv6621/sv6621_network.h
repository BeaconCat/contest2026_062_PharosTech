/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_network.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_NET

#include <nuttx/net/netdev.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#include <stdbool.h>
#include <stdint.h>

#include "include/sv6621.h"
#include "sv6621_command.h"
#include "sv6621_data.h"
#include "sv6621_offload.h"
#include "sv6621_ioctl.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_NETWORK_RX_DEPTH 4
#define SV6621_NETWORK_MULTICAST_CAPACITY 32

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_network_s
{
  struct net_driver_s dev;
  spinlock_t lock;
  struct work_s rx_work;
  struct work_s tx_work;
  struct work_s multicast_work;
  FAR struct sv6621_data_s *data;
  FAR struct sv6621_command_engine_s *command;
  struct sv6621_ioctl_s ioctl;
  struct sv6621_data_tx_context_s tx_context;
  bool registered;
  bool interface_up;
  bool link_up;
  bool rx_scheduled;
  uint8_t rx_head;
  uint8_t rx_tail;
  uint8_t multicast_count;
  uint8_t multicast_limit;
  uint8_t multicast[SV6621_NETWORK_MULTICAST_CAPACITY][SV6621_MAC_LENGTH];
  uint16_t rx_length[SV6621_NETWORK_RX_DEPTH];
  uint8_t rx_frame[SV6621_NETWORK_RX_DEPTH][MAX_NETDEV_PKTSIZE];
  uint8_t tx_frame[MAX_NETDEV_PKTSIZE] aligned_data(4);
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_network_init(FAR struct sv6621_network_s *network,
                        FAR struct sv6621_dev_s *owner,
                        FAR struct sv6621_data_s *data,
                        FAR struct sv6621_command_engine_s *command,
                        uint8_t multicast_limit,
                        FAR const uint8_t mac[SV6621_MAC_LENGTH]);
void sv6621_network_deinit(FAR struct sv6621_network_s *network);
int sv6621_network_sync_multicast(FAR struct sv6621_network_s *network);
int sv6621_network_sync_addresses(FAR struct sv6621_network_s *network);
void sv6621_network_set_link(
    FAR struct sv6621_network_s *network, bool link_up,
    FAR const struct sv6621_data_tx_context_s *context);
void sv6621_network_credit_available(FAR struct sv6621_network_s *network);
void sv6621_network_input(FAR const struct sv6621_data_rx_s *rx,
                          FAR void *arg);

#endif /* CONFIG_NET */
#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_NETWORK_H */
