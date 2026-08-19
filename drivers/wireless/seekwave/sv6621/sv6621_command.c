/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_command.c
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
#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include "sv6621_command.h"
#include "sv6621_packet.h"
#include "sv6621_protocol.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int
sv6621_command_complete_ack(FAR struct sv6621_command_engine_s *engine,
                            FAR const struct sv6621_message_header_s *header,
                            FAR const uint8_t *payload, size_t length);
static int
sv6621_command_dispatch_event(FAR struct sv6621_command_engine_s *engine,
                              FAR const struct sv6621_message_header_s *header,
                              FAR const uint8_t *payload, size_t length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_command_complete_ack
 ****************************************************************************/

static int
sv6621_command_complete_ack(FAR struct sv6621_command_engine_s *engine,
                            FAR const struct sv6621_message_header_s *header,
                            FAR const uint8_t *payload, size_t length)
{
  size_t copy_length;
  int ret;

  if (length < SV6621_COMMAND_ACK_STATUS_SIZE)
    {
      return -EPROTO;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!engine->pending || header->instance != engine->pending_instance ||
      header->id != engine->pending_id ||
      header->sequence != engine->pending_sequence)
    {
      engine->stats.stale_acknowledgements++;
      nxmutex_unlock(&engine->state_lock);
      return -ESTALE;
    }

  engine->firmware_status = payload[0] | ((uint16_t)payload[1] << 8);
  engine->response_length = length - SV6621_COMMAND_ACK_STATUS_SIZE;
  copy_length = engine->response_length;
  if (engine->response != NULL && copy_length > engine->response_capacity)
    {
      copy_length = engine->response_capacity;
      engine->completion_result = -ENOSPC;
    }
  else if (engine->response == NULL)
    {
      copy_length = 0;
      engine->completion_result = 0;
    }
  else
    {
      engine->completion_result = 0;
    }

  if (copy_length > 0)
    {
      memcpy(engine->response, payload + SV6621_COMMAND_ACK_STATUS_SIZE,
             copy_length);
    }

  engine->response = NULL;
  engine->response_capacity = 0;
  engine->pending = false;
  nxsem_post(&engine->completion);
  nxmutex_unlock(&engine->state_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_command_dispatch_event
 ****************************************************************************/

static int
sv6621_command_dispatch_event(FAR struct sv6621_command_engine_s *engine,
                              FAR const struct sv6621_message_header_s *header,
                              FAR const uint8_t *payload, size_t length)
{
  sv6621_command_event_t callback;
  FAR void *arg;
  uint16_t expected;
  int ret;

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (engine->event_sequence_valid)
    {
      expected = engine->event_sequence + 1;
      if (header->sequence != expected)
        {
          engine->stats.missed_events++;
        }
    }

  engine->event_sequence = header->sequence;
  engine->event_sequence_valid = true;
  callback = engine->event;
  arg = engine->event_arg;
  engine->dispatching_event = true;
  engine->stats.events++;
  nxmutex_unlock(&engine->state_lock);

  if (callback != NULL)
    {
      callback(header->instance, header->id, payload, length, arg);
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret >= 0)
    {
      engine->dispatching_event = false;
      nxmutex_unlock(&engine->state_lock);
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_command_encode_header(
    FAR const struct sv6621_message_header_s *header,
    uint8_t encoded[SV6621_COMMAND_HEADER_SIZE])
{
  if (header == NULL || encoded == NULL || header->instance > 0x0f ||
      header->type > SV6621_MESSAGE_EVENT_LOCAL ||
      header->total_length < SV6621_COMMAND_HEADER_SIZE)
    {
      return -EINVAL;
    }

  encoded[0] = header->instance | ((uint8_t)header->type << 4);
  encoded[1] = header->id;
  encoded[2] = header->sequence & 0xff;
  encoded[3] = header->sequence >> 8;
  encoded[4] = header->total_length & 0xff;
  encoded[5] = header->total_length >> 8;
  encoded[6] = 0;
  encoded[7] = 0;
  return 0;
}

int sv6621_command_decode_header(
    FAR const uint8_t encoded[SV6621_COMMAND_HEADER_SIZE], size_t available,
    FAR struct sv6621_message_header_s *header)
{
  if (encoded == NULL || header == NULL ||
      available < SV6621_COMMAND_HEADER_SIZE)
    {
      return -EINVAL;
    }

  header->instance = encoded[0] & 0x0f;
  header->type = (enum sv6621_message_type_e)(encoded[0] >> 4);
  header->id = encoded[1];
  header->sequence = encoded[2] | ((uint16_t)encoded[3] << 8);
  header->total_length = encoded[4] | ((uint16_t)encoded[5] << 8);
  if (header->type > SV6621_MESSAGE_EVENT_LOCAL ||
      header->total_length < SV6621_COMMAND_HEADER_SIZE ||
      header->total_length > available)
    {
      return -EPROTO;
    }

  return 0;
}

int sv6621_command_engine_init(FAR struct sv6621_command_engine_s *engine,
                               sv6621_command_sender_t sender,
                               FAR void *sender_arg,
                               sv6621_command_event_t event,
                               FAR void *event_arg,
                               sv6621_command_error_t error,
                               FAR void *error_arg)
{
  int ret;

  if (engine == NULL || sender == NULL)
    {
      return -EINVAL;
    }

  memset(engine, 0, sizeof(*engine));
  ret = nxmutex_init(&engine->execute_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_init(&engine->state_lock);
  if (ret < 0)
    {
      nxmutex_destroy(&engine->execute_lock);
      return ret;
    }

  ret = nxmutex_init(&engine->receive_lock);
  if (ret < 0)
    {
      nxmutex_destroy(&engine->state_lock);
      nxmutex_destroy(&engine->execute_lock);
      return ret;
    }

  ret = nxsem_init(&engine->completion, 0, 0);
  if (ret < 0)
    {
      nxmutex_destroy(&engine->receive_lock);
      nxmutex_destroy(&engine->state_lock);
      nxmutex_destroy(&engine->execute_lock);
      return ret;
    }

  engine->sender = sender;
  engine->sender_arg = sender_arg;
  engine->event = event;
  engine->event_arg = event_arg;
  engine->error = error;
  engine->error_arg = error_arg;
  return 0;
}

void sv6621_command_engine_deinit(FAR struct sv6621_command_engine_s *engine)
{
  int ret;

  if (engine == NULL)
    {
      return;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret >= 0)
    {
      engine->shutting_down = true;
      if (engine->pending)
        {
          engine->pending = false;
          engine->completion_result = -ESHUTDOWN;
          engine->response = NULL;
          engine->response_capacity = 0;
          engine->stats.cancelled++;
          nxsem_post(&engine->completion);
        }

      nxmutex_unlock(&engine->state_lock);
    }

  /* Wake the current waiter above, then wait for both outbound command
   * execution and inbound callback dispatch to leave the engine.
   */

  ret = nxmutex_lock(&engine->execute_lock);
  if (ret >= 0)
    {
      nxmutex_unlock(&engine->execute_lock);
    }

  ret = nxmutex_lock(&engine->receive_lock);
  if (ret >= 0)
    {
      nxmutex_unlock(&engine->receive_lock);
    }

  nxsem_destroy(&engine->completion);
  nxmutex_destroy(&engine->receive_lock);
  nxmutex_destroy(&engine->state_lock);
  nxmutex_destroy(&engine->execute_lock);
}

/****************************************************************************
 * Name: sv6621_command_reset
 ****************************************************************************/

int sv6621_command_reset(FAR struct sv6621_command_engine_s *engine)
{
  int ret;

  if (engine == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (engine->shutting_down)
    {
      ret = -ESHUTDOWN;
    }
  else if (engine->pending || engine->dispatching_event)
    {
      ret = -EBUSY;
    }
  else
    {
      engine->event_sequence = 0;
      engine->event_sequence_valid = false;
      ret = 0;
    }

  nxmutex_unlock(&engine->state_lock);
  return ret;
}

int sv6621_command_execute(FAR struct sv6621_command_engine_s *engine,
                           uint8_t instance, uint8_t id,
                           FAR const void *payload, size_t payload_length,
                           FAR void *response, FAR size_t *response_length,
                           uint32_t timeout_ms)
{
  struct sv6621_message_header_s header = {0};
  FAR uint8_t *message;
  FAR uint8_t *packet;
  size_t message_length;
  size_t packet_capacity;
  size_t packet_length;
  size_t response_capacity;
  bool transport_failure = false;
  int ret;

  if (engine == NULL || instance > 0x0f || timeout_ms == 0 ||
      (payload == NULL && payload_length != 0) ||
      (response == NULL && response_length != NULL && *response_length != 0) ||
      (response != NULL && response_length == NULL))
    {
      return -EINVAL;
    }

  message_length = SV6621_COMMAND_HEADER_SIZE + payload_length;
  if (message_length > SV6621_COMMAND_MAX_MESSAGE_SIZE)
    {
      return -E2BIG;
    }

  response_capacity = response_length == NULL ? 0 : *response_length;
  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (engine->shutting_down)
    {
      nxmutex_unlock(&engine->state_lock);
      return -ESHUTDOWN;
    }

  if (engine->dispatching_event)
    {
      nxmutex_unlock(&engine->state_lock);
      return -EDEADLK;
    }

  nxmutex_unlock(&engine->state_lock);
  packet_capacity = SV6621_PACKET_HEADER_SIZE + message_length +
                    SV6621_PACKET_HEADER_SIZE + SV6621_SDIO_BLOCK_SIZE;
  message = kmm_malloc(message_length);
  packet = kmm_malloc(packet_capacity);
  if (message == NULL || packet == NULL)
    {
      kmm_free(packet);
      kmm_free(message);
      return -ENOMEM;
    }

  ret = nxmutex_lock(&engine->execute_lock);
  if (ret < 0)
    {
      goto free_buffers;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      goto unlock_execute;
    }

  if (engine->shutting_down)
    {
      ret = -ESHUTDOWN;
      nxmutex_unlock(&engine->state_lock);
      goto unlock_execute;
    }

  if (engine->dispatching_event)
    {
      ret = -EDEADLK;
      nxmutex_unlock(&engine->state_lock);
      goto unlock_execute;
    }

  nxsem_reset(&engine->completion, 0);
  engine->next_sequence++;
  header.instance = instance;
  header.type = SV6621_MESSAGE_COMMAND;
  header.id = id;
  header.sequence = engine->next_sequence;
  header.total_length = (uint16_t)message_length;
  engine->pending_instance = instance;
  engine->pending_id = id;
  engine->pending_sequence = header.sequence;
  engine->pending = true;
  engine->completion_result = -EINPROGRESS;
  engine->firmware_status = 0;
  engine->response = response;
  engine->response_capacity = response_capacity;
  engine->response_length = 0;
  engine->stats.commands++;
  nxmutex_unlock(&engine->state_lock);

  ret = sv6621_command_encode_header(&header, message);
  if (ret < 0)
    {
      goto cancel_command;
    }

  if (payload_length > 0)
    {
      memcpy(message + SV6621_COMMAND_HEADER_SIZE, payload, payload_length);
    }

  ret =
      sv6621_packet_build(SV6621_CHANNEL_WIFI_COMMAND, message, message_length,
                          packet, packet_capacity, &packet_length);
  if (ret < 0)
    {
      goto cancel_command;
    }

  ret = engine->sender(packet, packet_length, engine->sender_arg);
  if (ret < 0)
    {
      transport_failure = true;
      goto cancel_command;
    }

  ret = nxsem_tickwait(&engine->completion, MSEC2TICK(timeout_ms));
  if (ret < 0)
    {
      int wait_result = ret;
      int lock_ret = nxmutex_lock(&engine->state_lock);

      if (lock_ret < 0)
        {
          ret = lock_ret;
        }
      else if (engine->pending)
        {
          engine->pending = false;
          engine->completion_result = wait_result;
          engine->response = NULL;
          engine->response_capacity = 0;
          if (wait_result == -ETIMEDOUT)
            {
              engine->stats.timeouts++;
              transport_failure = true;
            }
          else
            {
              engine->stats.cancelled++;
            }

          ret = wait_result;
          nxmutex_unlock(&engine->state_lock);
          goto finish_command;
        }
      else
        {
          ret = engine->completion_result;
        }

      if (lock_ret >= 0)
        {
          nxmutex_unlock(&engine->state_lock);
        }
    }

  if (ret >= 0)
    {
      ret = nxmutex_lock(&engine->state_lock);
      if (ret >= 0)
        {
          if (response_length != NULL)
            {
              *response_length = engine->response_length;
            }

          ret = engine->completion_result < 0 ? engine->completion_result :
                engine->firmware_status == 0 ? 0 :
                -(int)engine->firmware_status;
          nxmutex_unlock(&engine->state_lock);
        }
    }

  goto finish_command;

cancel_command:
  sv6621_command_cancel(engine, ret);
  nxsem_wait_uninterruptible(&engine->completion);

finish_command:
  nxmutex_unlock(&engine->execute_lock);
  goto free_buffers;

unlock_execute:
  nxmutex_unlock(&engine->execute_lock);

free_buffers:
  if (ret < 0)
    {
      syslog(LOG_ERR,
             "SV6621 command failed: instance=%u id=%u sequence=%u ret=%d\n",
             instance, id, header.sequence, ret);
    }

  if (transport_failure && engine->error != NULL)
    {
      engine->error(ret, engine->error_arg);
    }

  kmm_free(packet);
  kmm_free(message);
  return ret;
}

/****************************************************************************
 * Name: sv6621_command_send_noack
 ****************************************************************************/

int sv6621_command_send_noack(FAR struct sv6621_command_engine_s *engine,
                              uint8_t instance, uint8_t id,
                              FAR const void *payload,
                              size_t payload_length)
{
  struct sv6621_message_header_s header;
  FAR uint8_t *message;
  FAR uint8_t *packet;
  size_t message_length;
  size_t packet_capacity;
  size_t packet_length;
  int ret;

  if (engine == NULL || instance > 0x0f ||
      (payload == NULL && payload_length != 0))
    {
      return -EINVAL;
    }

  message_length = SV6621_COMMAND_HEADER_SIZE + payload_length;
  if (message_length > SV6621_COMMAND_MAX_MESSAGE_SIZE)
    {
      return -E2BIG;
    }

  packet_capacity = SV6621_PACKET_HEADER_SIZE + message_length +
                    SV6621_PACKET_HEADER_SIZE + SV6621_SDIO_BLOCK_SIZE;
  message = kmm_malloc(message_length);
  packet = kmm_malloc(packet_capacity);
  if (message == NULL || packet == NULL)
    {
      kmm_free(packet);
      kmm_free(message);
      return -ENOMEM;
    }

  ret = nxmutex_lock(&engine->execute_lock);
  if (ret < 0)
    {
      goto free_buffers;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      goto unlock_execute;
    }

  if (engine->shutting_down)
    {
      ret = -ESHUTDOWN;
      nxmutex_unlock(&engine->state_lock);
      goto unlock_execute;
    }

  if (engine->dispatching_event || engine->pending)
    {
      ret = engine->dispatching_event ? -EDEADLK : -EBUSY;
      nxmutex_unlock(&engine->state_lock);
      goto unlock_execute;
    }

  engine->next_sequence++;
  header.instance = instance;
  header.type = SV6621_MESSAGE_COMMAND;
  header.id = id;
  header.sequence = engine->next_sequence;
  header.total_length = (uint16_t)message_length;
  engine->stats.commands++;
  nxmutex_unlock(&engine->state_lock);

  ret = sv6621_command_encode_header(&header, message);
  if (ret < 0)
    {
      goto unlock_execute;
    }

  if (payload_length > 0)
    {
      memcpy(message + SV6621_COMMAND_HEADER_SIZE, payload, payload_length);
    }

  ret = sv6621_packet_build(SV6621_CHANNEL_WIFI_COMMAND, message,
                            message_length, packet, packet_capacity,
                            &packet_length);
  if (ret >= 0)
    {
      ret = engine->sender(packet, packet_length, engine->sender_arg);
    }

unlock_execute:
  nxmutex_unlock(&engine->execute_lock);

free_buffers:
  kmm_free(packet);
  kmm_free(message);
  return ret;
}

int sv6621_command_cancel(FAR struct sv6621_command_engine_s *engine,
                          int result)
{
  int ret;

  if (engine == NULL || result >= 0)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!engine->pending)
    {
      nxmutex_unlock(&engine->state_lock);
      return -ENOENT;
    }

  engine->pending = false;
  engine->completion_result = result;
  engine->response = NULL;
  engine->response_capacity = 0;
  engine->stats.cancelled++;
  nxsem_post(&engine->completion);
  nxmutex_unlock(&engine->state_lock);
  return 0;
}

int sv6621_command_receive(FAR struct sv6621_command_engine_s *engine,
                           FAR const uint8_t *message, size_t length)
{
  struct sv6621_message_header_s header;
  FAR const uint8_t *payload;
  size_t payload_length;
  int ret;

  if (engine == NULL || message == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&engine->receive_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&engine->state_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&engine->receive_lock);
      return ret;
    }

  if (engine->shutting_down)
    {
      nxmutex_unlock(&engine->state_lock);
      nxmutex_unlock(&engine->receive_lock);
      return -ESHUTDOWN;
    }

  nxmutex_unlock(&engine->state_lock);
  ret = sv6621_command_decode_header(message, length, &header);
  if (ret < 0)
    {
      engine->stats.malformed++;
      goto unlock_receive;
    }

  payload = message + SV6621_COMMAND_HEADER_SIZE;
  payload_length = header.total_length - SV6621_COMMAND_HEADER_SIZE;
  switch (header.type)
    {
      case SV6621_MESSAGE_COMMAND_ACK:
        ret = sv6621_command_complete_ack(engine, &header, payload,
                                          payload_length);
        break;

      case SV6621_MESSAGE_EVENT:
        ret = sv6621_command_dispatch_event(engine, &header, payload,
                                            payload_length);
        break;

      default:
        ret = -EPROTO;
        break;
    }

  if (ret == -EPROTO)
    {
      engine->stats.malformed++;
    }

unlock_receive:
  nxmutex_unlock(&engine->receive_lock);
  return ret;
}

void sv6621_command_channel_consumer(uint8_t channel,
                                     FAR const uint8_t encoded[4],
                                     FAR const uint8_t *payload, size_t length,
                                     FAR void *arg)
{
  FAR struct sv6621_command_engine_s *engine = arg;

  (void)encoded;

  if (engine == NULL || payload == NULL ||
      channel != SV6621_CHANNEL_WIFI_COMMAND ||
      length < SV6621_RX_LINK_HEADER_SIZE + SV6621_COMMAND_HEADER_SIZE)
    {
      if (engine != NULL)
        {
          engine->stats.malformed++;
        }

      return;
    }

  sv6621_command_receive(engine, payload + SV6621_RX_LINK_HEADER_SIZE,
                         length - SV6621_RX_LINK_HEADER_SIZE);
}
