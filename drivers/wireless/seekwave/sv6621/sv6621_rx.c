/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_rx.c
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

#include <nuttx/kmalloc.h>
#include <nuttx/wqueue.h>

#include <errno.h>
#include <string.h>

#include "sv6621_protocol.h"
#include "sv6621_rx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_RX_MAX_SLOTS         8
#define SV6621_RX_TRAILER_SIZE      1024
#define SV6621_RX_VALID_LENGTH_SIZE 8
#define SV6621_RX_PENDING_SIZE      4
#define SV6621_RX_MAX_DRAIN_BURSTS  32
#define SV6621_RX_EXT_INTERRUPT     0x182
#define SV6621_RX_BUFFER_SIZE \
  (SV6621_PACKET_SIZE * SV6621_RX_MAX_SLOTS + SV6621_RX_TRAILER_SIZE)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t sv6621_rx_get_le32(FAR const uint8_t *data);
static void sv6621_rx_interrupt(FAR void *arg);
static void sv6621_rx_worker(FAR void *arg);
static void sv6621_rx_report_error(FAR struct sv6621_rx_s *rx, int error);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_rx_get_le32
 ****************************************************************************/

static uint32_t sv6621_rx_get_le32(FAR const uint8_t *data)
{
  return data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

/****************************************************************************
 * Name: sv6621_rx_interrupt
 ****************************************************************************/

static void sv6621_rx_interrupt(FAR void *arg)
{
  FAR struct sv6621_rx_s *rx = arg;

  if (rx == NULL || !rx->running || rx->suspended)
    {
      return;
    }

  rx->stats.interrupts++;
  if (work_available(&rx->work))
    {
      work_queue(HPWORK, &rx->work, sv6621_rx_worker, rx, 0);
    }
}

/****************************************************************************
 * Name: sv6621_rx_report_error
 ****************************************************************************/

static void sv6621_rx_report_error(FAR struct sv6621_rx_s *rx, int error)
{
  if (rx->error != NULL)
    {
      rx->error(error, rx->error_arg);
    }
}

/****************************************************************************
 * Name: sv6621_rx_worker
 ****************************************************************************/

static void sv6621_rx_worker(FAR void *arg)
{
  FAR struct sv6621_rx_s *rx = arg;
  unsigned int slots = 1;
  unsigned int burst;

  for (burst = 0;
       burst < SV6621_RX_MAX_DRAIN_BURSTS && rx->running && !rx->suspended;
       burst++)
    {
      size_t length = slots * SV6621_PACKET_SIZE + SV6621_RX_TRAILER_SIZE;
      uint32_t pending_count;
      int ret;

      ret = rx->transport->ops->read(rx->transport, SV6621_SDIO_FUNCTION_DATA,
                                     SV6621_SDIO_PACKET_WINDOW, false,
                                     rx->buffer, length);
      if (ret < 0)
        {
          rx->stats.transport_errors++;
          sv6621_rx_report_error(rx, ret);
          return;
        }

      rx->stats.bursts++;
      ret =
          sv6621_rx_parse_burst(rx, rx->buffer, length, slots, &pending_count);
      if (ret < 0)
        {
          rx->stats.malformed_bursts++;
          sv6621_rx_report_error(rx, ret);
          return;
        }

      if (pending_count == 0)
        {
          uint8_t extended_interrupt;

          rx->stats.drain_completions++;
          ret = rx->transport->ops->read_byte(
              rx->transport, SV6621_SDIO_FUNCTION_CONTROL,
              SV6621_RX_EXT_INTERRUPT, &extended_interrupt);
          if (ret < 0)
            {
              rx->stats.transport_errors++;
              sv6621_rx_report_error(rx, ret);
            }

          return;
        }

      slots = pending_count;
      if (slots > SV6621_RX_MAX_SLOTS)
        {
          slots = SV6621_RX_MAX_SLOTS;
        }
    }

  if (rx->running && !rx->suspended)
    {
      sv6621_rx_report_error(rx, -ELOOP);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_rx_init(FAR struct sv6621_rx_s *rx,
                   FAR struct sv6621_transport_s *transport,
                   FAR struct sv6621_packet_router_s *router,
                   sv6621_rx_error_t error, FAR void *error_arg)
{
  int ret;

  if (rx == NULL || router == NULL)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  memset(rx, 0, sizeof(*rx));
  rx->buffer = kmm_malloc(SV6621_RX_BUFFER_SIZE);
  if (rx->buffer == NULL)
    {
      return -ENOMEM;
    }

  rx->transport = transport;
  rx->router = router;
  rx->error = error;
  rx->error_arg = error_arg;
  return 0;
}

void sv6621_rx_deinit(FAR struct sv6621_rx_s *rx)
{
  if (rx != NULL)
    {
      sv6621_rx_stop(rx);
      kmm_free(rx->buffer);
      rx->buffer = NULL;
    }
}

int sv6621_rx_start(FAR struct sv6621_rx_s *rx)
{
  int ret;

  if (rx == NULL || rx->buffer == NULL || rx->running)
    {
      return -EINVAL;
    }

  ret = rx->transport->ops->attach_irq(rx->transport, sv6621_rx_interrupt, rx);
  if (ret < 0)
    {
      return ret;
    }

  rx->running = true;
  rx->suspended = false;
  ret = rx->transport->ops->enable_irq(rx->transport, true);
  if (ret < 0)
    {
      rx->running = false;
      rx->transport->ops->attach_irq(rx->transport, NULL, NULL);
      return ret;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_rx_suspend
 ****************************************************************************/

int sv6621_rx_suspend(FAR struct sv6621_rx_s *rx)
{
  int ret;

  if (rx == NULL || !rx->running)
    {
      return -EINVAL;
    }

  if (rx->suspended)
    {
      return 0;
    }

  rx->suspended = true;
  ret = rx->transport->ops->enable_irq(rx->transport, false);
  if (ret < 0)
    {
      rx->suspended = false;
      return ret;
    }

  work_cancel_sync(HPWORK, &rx->work);
  return 0;
}

/****************************************************************************
 * Name: sv6621_rx_resume
 ****************************************************************************/

int sv6621_rx_resume(FAR struct sv6621_rx_s *rx)
{
  int ret;

  if (rx == NULL || !rx->running)
    {
      return -EINVAL;
    }

  if (!rx->suspended)
    {
      return 0;
    }

  rx->suspended = false;
  ret = rx->transport->ops->enable_irq(rx->transport, true);
  if (ret < 0)
    {
      rx->suspended = true;
    }

  return ret;
}

void sv6621_rx_stop(FAR struct sv6621_rx_s *rx)
{
  if (rx == NULL || !rx->running)
    {
      return;
    }

  rx->running = false;
  rx->suspended = false;
  rx->transport->ops->enable_irq(rx->transport, false);
  rx->transport->ops->attach_irq(rx->transport, NULL, NULL);
  work_cancel_sync(HPWORK, &rx->work);
}

int sv6621_rx_parse_burst(FAR struct sv6621_rx_s *rx,
                          FAR const uint8_t *buffer, size_t length,
                          unsigned int slots, FAR uint32_t *pending_count)
{
  size_t packet_region;
  unsigned int slot;

  if (rx == NULL || rx->router == NULL || buffer == NULL ||
      pending_count == NULL || slots == 0 || slots > SV6621_RX_MAX_SLOTS)
    {
      return -EINVAL;
    }

  packet_region = slots * SV6621_PACKET_SIZE;
  if (length != packet_region + SV6621_RX_TRAILER_SIZE)
    {
      return -EMSGSIZE;
    }

  /* Firmware revisions disagree on the valid-length field semantics.  Keep
   * it for diagnostics, but bound every packet against its fixed stride.
   */

  rx->stats.last_valid_length =
      sv6621_rx_get_le32(buffer + length - SV6621_RX_VALID_LENGTH_SIZE);
  *pending_count =
      sv6621_rx_get_le32(buffer + length - SV6621_RX_PENDING_SIZE);
  rx->stats.last_pending_count = *pending_count;

  for (slot = 0; slot < slots; slot++)
    {
      FAR const uint8_t *packet = buffer + slot * SV6621_PACKET_SIZE;
      struct sv6621_packet_header_s header;
      size_t available = SV6621_PACKET_SIZE - SV6621_PACKET_HEADER_SIZE;
      int ret;

      ret = sv6621_protocol_decode_header(packet, &header);
      if (ret < 0)
        {
          return ret;
        }

      if (header.end_of_frame)
        {
          break;
        }

      if ((size_t)header.length + header.padding > available)
        {
          return -EPROTO;
        }

      ret = sv6621_packet_dispatch(
          rx->router, packet, packet + SV6621_PACKET_HEADER_SIZE, available);
      if (ret < 0 && ret != -ENOSYS)
        {
          return ret;
        }

      rx->stats.packets++;
    }

  return 0;
}
