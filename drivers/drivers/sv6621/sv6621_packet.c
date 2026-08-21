/****************************************************************************
 * drivers/drivers/sv6621/sv6621_packet.c
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

#include "sv6621_packet.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_PACKET_ALIGNMENT 4

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static size_t sv6621_packet_align(size_t value, size_t alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_packet_router_init(FAR struct sv6621_packet_router_s *router)
{
  if (router == NULL)
    {
      return -EINVAL;
    }

  memset(router, 0, sizeof(*router));
  return nxmutex_init(&router->lock);
}

void sv6621_packet_router_deinit(FAR struct sv6621_packet_router_s *router)
{
  if (router != NULL)
    {
      nxmutex_destroy(&router->lock);
    }
}

int sv6621_packet_subscribe(FAR struct sv6621_packet_router_s *router,
                            uint8_t channel, sv6621_packet_consumer_t callback,
                            FAR void *arg)
{
  int ret;

  if (router == NULL || callback == NULL ||
      channel >= SV6621_PACKET_CHANNEL_COUNT)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&router->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (router->consumer[channel].callback != NULL)
    {
      ret = -EBUSY;
    }
  else
    {
      router->consumer[channel].callback = callback;
      router->consumer[channel].arg = arg;
      ret = OK;
    }

  nxmutex_unlock(&router->lock);
  return ret;
}

int sv6621_packet_unsubscribe(FAR struct sv6621_packet_router_s *router,
                              uint8_t channel,
                              sv6621_packet_consumer_t callback, FAR void *arg)
{
  int ret;

  if (router == NULL || callback == NULL ||
      channel >= SV6621_PACKET_CHANNEL_COUNT)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&router->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (router->consumer[channel].callback != callback ||
      router->consumer[channel].arg != arg)
    {
      ret = -ENOENT;
    }
  else
    {
      router->consumer[channel].callback = NULL;
      router->consumer[channel].arg = NULL;
      ret = OK;
    }

  nxmutex_unlock(&router->lock);
  return ret;
}

int sv6621_packet_dispatch(FAR struct sv6621_packet_router_s *router,
                           FAR const uint8_t encoded[4],
                           FAR const uint8_t *payload, size_t available)
{
  struct sv6621_packet_header_s header;
  sv6621_packet_consumer_t callback;
  FAR void *arg;
  int ret;

  if (router == NULL || encoded == NULL || payload == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_protocol_decode_header(encoded, &header);
  if (ret < 0 || header.end_of_frame || header.length > available)
    {
      router->malformed++;
      return ret < 0 ? ret : -EPROTO;
    }

  ret = nxmutex_lock(&router->lock);
  if (ret < 0)
    {
      return ret;
    }

  callback = router->consumer[header.channel].callback;
  arg = router->consumer[header.channel].arg;
  if (callback == NULL)
    {
      router->dropped[header.channel]++;
    }
  else
    {
      router->received[header.channel]++;
    }

  nxmutex_unlock(&router->lock);

  if (callback == NULL)
    {
      return -ENOSYS;
    }

  callback(header.channel, encoded, payload, header.length, arg);
  return OK;
}

int sv6621_packet_build(uint8_t channel, FAR const void *payload,
                        size_t length, FAR uint8_t *buffer, size_t capacity,
                        FAR size_t *written)
{
  struct sv6621_packet_header_s header;
  struct sv6621_packet_header_s terminator;
  size_t aligned_length;
  size_t payload_end;
  size_t total;
  int ret;

  if (payload == NULL || buffer == NULL || written == NULL || length == 0 ||
      length > UINT16_MAX - (SV6621_PACKET_ALIGNMENT - 1) ||
      channel >= SV6621_PACKET_CHANNEL_COUNT)
    {
      return -EINVAL;
    }

  aligned_length = sv6621_packet_align(length, SV6621_PACKET_ALIGNMENT);
  payload_end = SV6621_PACKET_HEADER_SIZE + aligned_length;
  total = sv6621_packet_align(payload_end + SV6621_PACKET_HEADER_SIZE,
                              SV6621_SDIO_BLOCK_SIZE);
  if (total > capacity)
    {
      return -ENOSPC;
    }

  memset(buffer, 0, total);
  header.length = (uint16_t)aligned_length;
  header.padding = 0;
  header.end_of_frame = false;
  header.channel = channel;
  ret = sv6621_protocol_encode_header(&header, buffer);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(buffer + SV6621_PACKET_HEADER_SIZE, payload, length);

  terminator.length = 0;
  terminator.padding = 0;
  terminator.end_of_frame = true;
  terminator.channel = SV6621_CHANNEL_AT;
  ret = sv6621_protocol_encode_header(&terminator, buffer + payload_end);
  if (ret < 0)
    {
      return ret;
    }

  *written = total;
  return OK;
}
