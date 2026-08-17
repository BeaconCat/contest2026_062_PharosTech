/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_protocol.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PROTOCOL_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PROTOCOL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SDIO_FUNCTION_CONTROL 0
#define SV6621_SDIO_FUNCTION_DATA    1
#define SV6621_SDIO_BLOCK_SIZE       512
#define SV6621_PACKET_SIZE           0x600
#define SV6621_PACKET_HEADER_SIZE    4
#define SV6621_PACKET_CHANNEL_COUNT  12

#define SV6621_SDIO_DT_WINDOW        0x00f
#define SV6621_SDIO_PACKET_WINDOW    0x020
#define SV6621_SDIO_DT_ADDRESS       0x15c
#define SV6621_SDIO_DOWNLOAD_STATUS  0x160
#define SV6621_SDIO_DMA_TYPE         0x165
#define SV6621_SDIO_SLEEP_CONTROL    0x167
#define SV6621_SDIO_CREDIT_TO_CP     0x168
#define SV6621_SDIO_RX_FLOW_LOW      0x16c
#define SV6621_SDIO_RX_FLOW_HIGH     0x16d
#define SV6621_SDIO_FIFO_INDICATOR   0x181
#define SV6621_SDIO_CREDIT_FROM_CP   0x184
#define SV6621_SDIO_AP_TO_CP_IRQ     0x1b0

#define SV6621_CP_CHIP_ID_ADDRESS    0x40000000
#define SV6621_CP_DOWNLOAD_FLAG      0x40100030
#define SV6621_CP_IRAM_ADDRESS       0x00100000
#define SV6621_CP_DRAM_ADDRESS       0x20200000

#define SV6621_CHANNEL_AT            0
#define SV6621_CHANNEL_LOOPCHECK     1
#define SV6621_CHANNEL_BT_COMMAND    2
#define SV6621_CHANNEL_BT_AUDIO      3
#define SV6621_CHANNEL_BT_ISO        4
#define SV6621_CHANNEL_BT_DATA       5
#define SV6621_CHANNEL_WIFI_COMMAND  6
#define SV6621_CHANNEL_WIFI_DATA     7
#define SV6621_CHANNEL_WIFI_DATA1    8
#define SV6621_CHANNEL_BSP_LOG       9
#define SV6621_CHANNEL_BT_LOG        10
#define SV6621_CHANNEL_BSP_UPDATE    11

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_packet_header_s
{
  uint16_t length;
  uint8_t padding;
  bool end_of_frame;
  uint8_t channel;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_protocol_decode_header(FAR const uint8_t encoded[4],
                                  FAR struct sv6621_packet_header_s *header);
int sv6621_protocol_encode_header(
    FAR const struct sv6621_packet_header_s *header, uint8_t encoded[4]);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_PROTOCOL_H */
