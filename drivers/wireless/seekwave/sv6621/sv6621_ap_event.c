/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_event.c
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

#include "sv6621_ap_event.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool sv6621_ap_event_supported(uint8_t id);
static void sv6621_ap_event_worker(FAR void *arg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool sv6621_ap_event_supported(uint8_t id)
{
  return id == SV6621_AP_EVENT_RX_MGMT ||
         id == SV6621_AP_EVENT_DEL_STA ||
         id == SV6621_AP_EVENT_MGMT_TX_STATUS;
}

static void sv6621_ap_event_worker(FAR void *arg)
{
  FAR struct sv6621_ap_event_queue_s *queue = arg;

  for (;;)
    {
      FAR struct sv6621_ap_event_slot_s *slot;
      int ret;

      ret = nxmutex_lock(&queue->lock);
      if (ret < 0)
        {
          if (queue->error != NULL)
            {
              queue->error(ret, queue->arg);
            }

          return;
        }

      if (queue->stopping || queue->count == 0)
        {
          queue->work_scheduled = false;
          nxmutex_unlock(&queue->lock);
          return;
        }

      slot = &queue->slots[queue->tail];
      nxmutex_unlock(&queue->lock);

      ret = queue->handler(slot->instance, slot->id, slot->payload,
                           slot->length, queue->arg);

      if (nxmutex_lock(&queue->lock) < 0)
        {
          if (queue->error != NULL)
            {
              queue->error(-EDEADLK, queue->arg);
            }

          return;
        }

      queue->tail = (queue->tail + 1) % SV6621_AP_EVENT_QUEUE_DEPTH;
      queue->count--;
      nxmutex_unlock(&queue->lock);

      if (ret < 0 && queue->error != NULL)
        {
          queue->error(ret, queue->arg);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_event_queue_init(FAR struct sv6621_ap_event_queue_s *queue,
                               sv6621_ap_event_handler_t handler,
                               sv6621_ap_event_error_t error,
                               FAR void *arg)
{
  int ret;

  if (queue == NULL || handler == NULL)
    {
      return -EINVAL;
    }

  memset(queue, 0, sizeof(*queue));
  ret = nxmutex_init(&queue->lock);
  if (ret < 0)
    {
      return ret;
    }

  queue->handler = handler;
  queue->error = error;
  queue->arg = arg;
  return 0;
}

void sv6621_ap_event_queue_deinit(FAR struct sv6621_ap_event_queue_s *queue)
{
  if (queue == NULL)
    {
      return;
    }

  if (nxmutex_lock(&queue->lock) >= 0)
    {
      queue->stopping = true;
      nxmutex_unlock(&queue->lock);
    }

  work_cancel_sync(LPWORK, &queue->work);
  nxmutex_destroy(&queue->lock);
  memset(queue, 0, sizeof(*queue));
}

int sv6621_ap_event_queue_reset(FAR struct sv6621_ap_event_queue_s *queue)
{
  int ret;

  if (queue == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&queue->lock);
  if (ret < 0)
    {
      return ret;
    }

  queue->stopping = true;
  nxmutex_unlock(&queue->lock);
  work_cancel_sync(LPWORK, &queue->work);

  ret = nxmutex_lock(&queue->lock);
  if (ret < 0)
    {
      return ret;
    }

  queue->head = 0;
  queue->tail = 0;
  queue->count = 0;
  queue->work_scheduled = false;
  queue->stopping = false;
  nxmutex_unlock(&queue->lock);
  return 0;
}

int sv6621_ap_event_queue_submit(FAR struct sv6621_ap_event_queue_s *queue,
                                 uint8_t instance, uint8_t id,
                                 FAR const uint8_t *payload, size_t length)
{
  FAR struct sv6621_ap_event_slot_s *slot;
  bool schedule = false;
  int ret;

  if (queue == NULL || !sv6621_ap_event_supported(id) ||
      (payload == NULL && length != 0) ||
      length > SV6621_AP_EVENT_MAX_PAYLOAD)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&queue->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (queue->stopping)
    {
      nxmutex_unlock(&queue->lock);
      return -ESHUTDOWN;
    }

  if (queue->count == SV6621_AP_EVENT_QUEUE_DEPTH)
    {
      nxmutex_unlock(&queue->lock);
      return -ENOBUFS;
    }

  slot = &queue->slots[queue->head];
  slot->instance = instance;
  slot->id = id;
  slot->length = length;
  if (length != 0)
    {
      memcpy(slot->payload, payload, length);
    }

  queue->head = (queue->head + 1) % SV6621_AP_EVENT_QUEUE_DEPTH;
  queue->count++;
  if (!queue->work_scheduled)
    {
      queue->work_scheduled = true;
      schedule = true;
    }

  nxmutex_unlock(&queue->lock);
  if (!schedule)
    {
      return 0;
    }

  ret = work_queue(LPWORK, &queue->work, sv6621_ap_event_worker, queue, 0);
  if (ret < 0 && nxmutex_lock(&queue->lock) >= 0)
    {
      queue->work_scheduled = false;
      nxmutex_unlock(&queue->lock);
    }

  return ret;
}
