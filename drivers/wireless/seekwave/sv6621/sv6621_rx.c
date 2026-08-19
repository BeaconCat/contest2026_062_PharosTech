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
#include <syslog.h>

#include "sv6621_protocol.h"
#include "sv6621_rx.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_RX_MAX_SLOTS         8
#define SV6621_RX_TRAILER_SIZE      SV6621_SDIO_BLOCK_SIZE
#define SV6621_RX_VALID_LENGTH_SIZE 8
#define SV6621_RX_PENDING_SIZE      4
#define SV6621_RX_MAX_DRAIN_BURSTS  32
#define SV6621_RX_FIFO_INDICATOR    0x181
#define SV6621_RX_EXT_INTERRUPT     0x182
#define SV6621_RX_CCCR_INT_PENDING  0x05
#define SV6621_RX_FUNCTION1_PENDING (1 << 1)
#define SV6621_RX_FIFO_ASSERT       0xff
#define SV6621_RX_BUFFER_SIZE \
  (SV6621_PACKET_SIZE * SV6621_RX_MAX_SLOTS + SV6621_RX_TRAILER_SIZE)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t sv6621_rx_get_le32(FAR const uint8_t *data);
static void sv6621_rx_interrupt(FAR void *arg);
static void sv6621_rx_worker(FAR void *arg);
static int sv6621_rx_drain(FAR struct sv6621_rx_s *rx);
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
  irqstate_t flags;
  bool schedule = false;
  int ret;

  if (rx == NULL || !rx->running || rx->suspended)
    {
      return;
    }

  rx->stats.interrupts++;
  flags = spin_lock_irqsave(&rx->schedule_lock);
  if (!rx->work_scheduled)
    {
      rx->work_scheduled = true;
      schedule = true;
    }
  else
    {
      rx->work_reschedule = true;
    }

  spin_unlock_irqrestore(&rx->schedule_lock, flags);
  if (!schedule)
    {
      return;
    }

  ret = work_queue(HPWORK, &rx->work, sv6621_rx_worker, rx, 0);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&rx->schedule_lock);
      rx->work_scheduled = false;
      rx->work_reschedule = false;
      spin_unlock_irqrestore(&rx->schedule_lock, flags);
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
 * Name: sv6621_rx_drain
 ****************************************************************************/

static int sv6621_rx_drain(FAR struct sv6621_rx_s *rx)
{
  unsigned int slots;
  unsigned int burst;
  uint8_t fifo_indicator;
  int ret;

  slots = rx->pending_slots == 0 ? 1 : rx->pending_slots;
  if (rx->pending_slots == 0)
    {
      ret = rx->transport->ops->read_byte(rx->transport,
                                          SV6621_SDIO_FUNCTION_CONTROL,
                                          SV6621_RX_FIFO_INDICATOR,
                                          &fifo_indicator);
      if (ret < 0)
        {
          rx->stats.transport_errors++;
          sv6621_rx_report_error(rx, ret);
          return ret;
        }

      if (fifo_indicator == SV6621_RX_FIFO_ASSERT)
        {
          sv6621_rx_report_error(rx, -EIO);
          return -EIO;
        }

      if (rx->fifo_indicator_valid &&
          fifo_indicator == rx->stats.last_fifo_indicator)
        {
          rx->stats.duplicate_interrupts++;
          return 0;
        }

      rx->stats.last_fifo_indicator = fifo_indicator;
      rx->fifo_indicator_valid = true;
    }

  rx->pending_slots = 0;

  for (burst = 0;
       burst < SV6621_RX_MAX_DRAIN_BURSTS && rx->running && !rx->suspended;
       burst++)
    {
      size_t length = slots * SV6621_PACKET_SIZE + SV6621_RX_TRAILER_SIZE;
      uint32_t pending_count = 0;
      ret = rx->transport->ops->read(rx->transport, SV6621_SDIO_FUNCTION_DATA,
                                     SV6621_SDIO_PACKET_WINDOW, false,
                                     rx->buffer, length);
      if (ret < 0)
        {
          rx->stats.transport_errors++;
          sv6621_rx_report_error(rx, ret);
          return ret;
        }

      rx->stats.bursts++;
      ret =
          sv6621_rx_parse_burst(rx, rx->buffer, length, slots, &pending_count);
      if (ret < 0)
        {
          rx->stats.malformed_bursts++;
          sv6621_rx_report_error(rx, ret);
          return ret;
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
              return ret;
            }

          return 0;
        }

      slots = pending_count;
      if (slots > SV6621_RX_MAX_SLOTS)
        {
          slots = SV6621_RX_MAX_SLOTS;
        }
    }

  if (rx->running && !rx->suspended)
    {
      rx->pending_slots = slots;
      rx->stats.drain_yields++;
      return -EAGAIN;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_rx_worker
 ****************************************************************************/

static void sv6621_rx_worker(FAR void *arg)
{
  FAR struct sv6621_rx_s *rx = arg;
  irqstate_t flags;
  bool defer;
  bool retry;
  int ret;

  do
    {
      flags = spin_lock_irqsave(&rx->schedule_lock);
      rx->work_reschedule = false;
      spin_unlock_irqrestore(&rx->schedule_lock, flags);

      ret = sv6621_rx_drain(rx);

      if (ret != -EAGAIN)
        {
          int ack_ret = rx->transport->ops->ack_irq(rx->transport);

          if (ack_ret < 0 && ret >= 0)
            {
              ret = ack_ret;
              sv6621_rx_report_error(rx, ack_ret);
            }
        }

      flags = spin_lock_irqsave(&rx->schedule_lock);
      defer = ret == -EAGAIN && rx->running && !rx->suspended;
      if (defer)
        {
          rx->work_reschedule = false;
          spin_unlock_irqrestore(&rx->schedule_lock, flags);
          ret = work_queue(HPWORK, &rx->work, sv6621_rx_worker, rx, 0);
          if (ret >= 0)
            {
              return;
            }

          flags = spin_lock_irqsave(&rx->schedule_lock);
          rx->work_scheduled = false;
          rx->work_reschedule = false;
          spin_unlock_irqrestore(&rx->schedule_lock, flags);
          sv6621_rx_report_error(rx, ret);
          return;
        }

      retry = ret == 0 && rx->work_reschedule && rx->running &&
              !rx->suspended;
      if (!retry)
        {
          rx->work_scheduled = false;
        }

      spin_unlock_irqrestore(&rx->schedule_lock, flags);
    }
  while (retry);
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
  irqstate_t flags;
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
  rx->fifo_indicator_valid = false;
  rx->pending_slots = 0;
  flags = spin_lock_irqsave(&rx->schedule_lock);
  rx->work_scheduled = false;
  rx->work_reschedule = false;
  spin_unlock_irqrestore(&rx->schedule_lock, flags);
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
  irqstate_t flags;
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
  rx->pending_slots = 0;
  ret = rx->transport->ops->enable_irq(rx->transport, false);
  if (ret < 0)
    {
      rx->suspended = false;
      return ret;
    }

  work_cancel_sync(HPWORK, &rx->work);
  flags = spin_lock_irqsave(&rx->schedule_lock);
  rx->work_scheduled = false;
  rx->work_reschedule = false;
  spin_unlock_irqrestore(&rx->schedule_lock, flags);
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
  irqstate_t flags;

  if (rx == NULL || !rx->running)
    {
      return;
    }

  rx->running = false;
  rx->suspended = false;
  rx->pending_slots = 0;
  rx->transport->ops->enable_irq(rx->transport, false);
  rx->transport->ops->attach_irq(rx->transport, NULL, NULL);
  work_cancel_sync(HPWORK, &rx->work);
  flags = spin_lock_irqsave(&rx->schedule_lock);
  rx->work_scheduled = false;
  rx->work_reschedule = false;
  spin_unlock_irqrestore(&rx->schedule_lock, flags);
}

void sv6621_rx_kick(FAR struct sv6621_rx_s *rx)
{
  if (rx == NULL || !rx->running || rx->suspended)
    {
      return;
    }

  rx->fifo_indicator_valid = false;
  sv6621_rx_interrupt(rx);
}

int sv6621_rx_poll(FAR struct sv6621_rx_s *rx)
{
  irqstate_t flags;
  uint8_t fifo_indicator;
  uint8_t pending;
  bool changed;
  bool idle;
  int ret;

  if (rx == NULL || !rx->running || rx->suspended)
    {
      return -ENODEV;
    }

  ret = rx->transport->ops->read_byte(rx->transport,
                                      SV6621_SDIO_FUNCTION_CONTROL,
                                      SV6621_RX_CCCR_INT_PENDING, &pending);
  if (ret < 0)
    {
      return ret;
    }

  if ((pending & SV6621_RX_FUNCTION1_PENDING) != 0)
    {
      ret = rx->transport->ops->read_byte(rx->transport,
                                          SV6621_SDIO_FUNCTION_CONTROL,
                                          SV6621_RX_FIFO_INDICATOR,
                                          &fifo_indicator);
      if (ret < 0)
        {
          return ret;
        }

      flags = spin_lock_irqsave(&rx->schedule_lock);
      idle = !rx->work_scheduled;
      changed = !rx->fifo_indicator_valid ||
                fifo_indicator != rx->stats.last_fifo_indicator;
      spin_unlock_irqrestore(&rx->schedule_lock, flags);
      if (idle && changed)
        {
          sv6621_rx_interrupt(rx);
        }
    }

  return 0;
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
          syslog(LOG_ERR,
                 "SV6621 RX header decode failed: slot=%u ret=%d head=%02x"
                 " %02x %02x %02x\n",
                 slot, ret, packet[0], packet[1], packet[2], packet[3]);
          return ret;
        }

      if (header.end_of_frame)
        {
          break;
        }

      if ((size_t)header.length + header.padding > available)
        {
          syslog(LOG_ERR,
                 "SV6621 RX bounds check failed: slot=%u ch=%u len=%u"
                 " pad=%u available=%zu\n",
                 slot, header.channel, header.length, header.padding,
                 available);
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
