/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_stats.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/clock.h>

#include <errno.h>
#include <string.h>

#include "sv6621_stats.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_STATS_COMMAND_GET_STA    23
#define SV6621_STATS_COMMAND_TIMEOUT_MS 5000
#define SV6621_STATS_REQUEST_SIZE       16
#define SV6621_STATS_RESPONSE_SIZE      172

#define SV6621_STATS_TX_RATE_OFFSET     0
#define SV6621_STATS_SIGNAL_OFFSET      9
#define SV6621_STATS_NOISE_OFFSET       10
#define SV6621_STATS_TX_PSR_OFFSET      11
#define SV6621_STATS_TX_FAILED_OFFSET   12
#define SV6621_STATS_RX_RATE_OFFSET     156
#define SV6621_STATS_TX_PERCENT_OFFSET  170
#define SV6621_STATS_RX_PERCENT_OFFSET  171

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_stats_get_le16(FAR const uint8_t *data);
static uint32_t sv6621_stats_get_le32(FAR const uint8_t *data);
static void sv6621_stats_put_le64(FAR uint8_t *data, uint64_t value);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_stats_get_le16
 ****************************************************************************/

static uint16_t sv6621_stats_get_le16(FAR const uint8_t *data)
{
  return data[0] | ((uint16_t)data[1] << 8);
}

/****************************************************************************
 * Name: sv6621_stats_get_le32
 ****************************************************************************/

static uint32_t sv6621_stats_get_le32(FAR const uint8_t *data)
{
  return data[0] | ((uint32_t)data[1] << 8) |
         ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

/****************************************************************************
 * Name: sv6621_stats_put_le64
 ****************************************************************************/

static void sv6621_stats_put_le64(FAR uint8_t *data, uint64_t value)
{
  unsigned int index;

  for (index = 0; index < 8; index++)
    {
      data[index] = value >> (index * 8);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_stats_query
 ****************************************************************************/

int sv6621_stats_query(FAR struct sv6621_command_engine_s *command,
                       uint8_t instance,
                       FAR const uint8_t mac[SV6621_MAC_LENGTH],
                       FAR struct sv6621_link_stats_s *stats)
{
  uint8_t request[SV6621_STATS_REQUEST_SIZE];
  uint8_t response[SV6621_STATS_RESPONSE_SIZE];
  size_t response_length = sizeof(response);
  uint64_t timestamp_ms;
  int ret;

  if (command == NULL || mac == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  memset(request, 0, sizeof(request));
  memcpy(request, mac, SV6621_MAC_LENGTH);
  timestamp_ms = TICK2MSEC(clock_systime_ticks());
  sv6621_stats_put_le64(request + 8, timestamp_ms);

  ret = sv6621_command_execute(command, instance,
                               SV6621_STATS_COMMAND_GET_STA, request,
                               sizeof(request), response, &response_length,
                               SV6621_STATS_COMMAND_TIMEOUT_MS);
  if (ret != 0)
    {
      return ret < 0 ? ret : -EREMOTEIO;
    }

  if (response_length != sizeof(response))
    {
      return -EPROTO;
    }

  memset(stats, 0, sizeof(*stats));
  stats->tx.flags = response[SV6621_STATS_TX_RATE_OFFSET];
  stats->tx.mcs = response[SV6621_STATS_TX_RATE_OFFSET + 1];
  stats->tx.legacy_100kbps =
      sv6621_stats_get_le16(response + SV6621_STATS_TX_RATE_OFFSET + 2);
  stats->tx.nss = response[SV6621_STATS_TX_RATE_OFFSET + 4];
  stats->tx.bandwidth = response[SV6621_STATS_TX_RATE_OFFSET + 5];
  stats->tx.guard_interval = response[SV6621_STATS_TX_RATE_OFFSET + 6];
  stats->signal_dbm = (int8_t)response[SV6621_STATS_SIGNAL_OFFSET];
  stats->noise_dbm = (int8_t)response[SV6621_STATS_NOISE_OFFSET];
  stats->tx_success_percent = response[SV6621_STATS_TX_PSR_OFFSET];
  stats->tx_failed =
      sv6621_stats_get_le32(response + SV6621_STATS_TX_FAILED_OFFSET);
  stats->rx.mode = response[SV6621_STATS_RX_RATE_OFFSET];
  stats->rx.rate = response[SV6621_STATS_RX_RATE_OFFSET + 1];
  stats->rx.nss = response[SV6621_STATS_RX_RATE_OFFSET + 2];
  stats->rx.bandwidth = response[SV6621_STATS_RX_RATE_OFFSET + 3];
  stats->rx.guard_interval = response[SV6621_STATS_RX_RATE_OFFSET + 4];
  stats->rx.resource_unit = response[SV6621_STATS_RX_RATE_OFFSET + 5];
  stats->rx.dcm = response[SV6621_STATS_RX_RATE_OFFSET + 6];
  stats->rx.snr_db = response[SV6621_STATS_RX_RATE_OFFSET + 9];
  stats->rx.rssi =
      sv6621_stats_get_le16(response + SV6621_STATS_RX_RATE_OFFSET + 10);
  stats->tx_airtime_percent = response[SV6621_STATS_TX_PERCENT_OFFSET];
  stats->rx_airtime_percent = response[SV6621_STATS_RX_PERCENT_OFFSET];
  return 0;
}
