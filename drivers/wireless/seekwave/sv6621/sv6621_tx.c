/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_tx.c
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

#include <errno.h>
#include <string.h>

#include "sv6621_protocol.h"
#include "sv6621_tx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_TX_NOTIFY_WIFI 0x01

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_tx_init(FAR struct sv6621_tx_s *tx,
                   FAR struct sv6621_transport_s *transport)
{
  int ret;

  if (tx == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  memset(tx, 0, sizeof(*tx));
  tx->transport = transport;
  return nxmutex_init(&tx->lock);
}

void sv6621_tx_deinit(FAR struct sv6621_tx_s *tx)
{
  if (tx != NULL)
    {
      nxmutex_destroy(&tx->lock);
    }
}

int sv6621_tx_send(FAR struct sv6621_tx_s *tx, FAR const uint8_t *packet,
                   size_t length)
{
  int ret;

  if (tx == NULL || packet == NULL || length == 0 ||
      length % SV6621_SDIO_BLOCK_SIZE != 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&tx->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = tx->transport->ops->write(tx->transport, SV6621_SDIO_FUNCTION_DATA,
                                  SV6621_SDIO_PACKET_WINDOW, false, packet,
                                  length);
  if (ret < 0)
    {
      tx->stats.transport_errors++;
      goto unlock;
    }

  ret = tx->transport->ops->write_byte(
      tx->transport, SV6621_SDIO_FUNCTION_CONTROL, SV6621_SDIO_AP_TO_CP_IRQ,
      SV6621_TX_NOTIFY_WIFI);
  if (ret < 0)
    {
      tx->stats.doorbell_errors++;
      goto unlock;
    }

  tx->stats.packets++;
  tx->stats.bytes += length;

unlock:
  nxmutex_unlock(&tx->lock);
  return ret;
}

int sv6621_tx_command_sender(FAR const uint8_t *packet, size_t length,
                             FAR void *arg)
{
  return sv6621_tx_send(arg, packet, length);
}
