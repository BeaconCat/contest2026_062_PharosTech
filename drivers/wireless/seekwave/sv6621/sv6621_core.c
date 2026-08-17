/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_core.c
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

#include <errno.h>
#include <string.h>

#include "sv6621_core.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_core_service_event(enum sv6621_service_event_e event,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg);
static void sv6621_core_rx_error(int error, FAR void *arg);
static void sv6621_core_report(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_core_report
 ****************************************************************************/

static void sv6621_core_report(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length)
{
  if (dev->config.event != NULL)
    {
      dev->config.event(dev, event, data, length, dev->config.event_arg);
    }
}

/****************************************************************************
 * Name: sv6621_core_service_event
 ****************************************************************************/

static void sv6621_core_service_event(enum sv6621_service_event_e event,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (event != SV6621_SERVICE_EVENT_ASSERT &&
      event != SV6621_SERVICE_EVENT_DUMP_COMPLETE)
    {
      return;
    }

  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->status.state = SV6621_STATE_FAILED;
      dev->status.last_error = -EIO;
      nxmutex_unlock(&dev->status_lock);
    }

  sv6621_core_report(dev, SV6621_EVENT_FATAL, payload, length);
}

/****************************************************************************
 * Name: sv6621_core_rx_error
 ****************************************************************************/

static void sv6621_core_rx_error(int error, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->status.state = SV6621_STATE_FAILED;
      dev->status.last_error = error;
      nxmutex_unlock(&dev->status_lock);
    }

  sv6621_core_report(dev, SV6621_EVENT_FATAL, &error, sizeof(error));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_create(FAR const struct sv6621_config_s *config,
                  FAR struct sv6621_dev_s **dev_out)
{
  FAR struct sv6621_dev_s *dev;
  int ret;

  if (config == NULL || dev_out == NULL || config->transport == NULL ||
      config->board_ops == NULL || config->board_ops->power_on == NULL ||
      config->board_ops->power_off == NULL || config->iram.data == NULL ||
      config->iram.length == 0 || config->dram.data == NULL ||
      config->dram.length == 0 || config->nvram.data == NULL ||
      config->nvram.length == 0)
    {
      return -EINVAL;
    }

  ret = sv6621_transport_validate(config->transport);
  if (ret < 0)
    {
      return ret;
    }

  *dev_out = NULL;
  dev = kmm_zalloc(sizeof(*dev));
  if (dev == NULL)
    {
      return -ENOMEM;
    }

  dev->config = *config;
  dev->status.state = SV6621_STATE_OFF;
  ret = nxmutex_init(&dev->lifecycle_lock);
  if (ret < 0)
    {
      goto free_device;
    }

  ret = nxmutex_init(&dev->status_lock);
  if (ret < 0)
    {
      goto destroy_lifecycle_lock;
    }

  ret = sv6621_packet_router_init(&dev->router);
  if (ret < 0)
    {
      goto destroy_status_lock;
    }

  ret = sv6621_tx_init(&dev->tx, config->transport);
  if (ret < 0)
    {
      goto deinit_router;
    }

  ret = sv6621_command_engine_init(&dev->command, sv6621_tx_command_sender,
                                   &dev->tx, NULL, NULL);
  if (ret < 0)
    {
      goto deinit_tx;
    }

  ret = sv6621_service_init(&dev->service, sv6621_core_service_event, dev);
  if (ret < 0)
    {
      goto deinit_command;
    }

  ret = sv6621_rx_init(&dev->rx, config->transport, &dev->router,
                       sv6621_core_rx_error, dev);
  if (ret < 0)
    {
      goto deinit_service;
    }

  ret =
      sv6621_packet_subscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                              sv6621_service_channel_consumer, &dev->service);
  if (ret < 0)
    {
      goto deinit_rx;
    }

  ret =
      sv6621_packet_subscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                              sv6621_command_channel_consumer, &dev->command);
  if (ret < 0)
    {
      goto unsubscribe_service;
    }

  *dev_out = dev;
  return 0;

unsubscribe_service:
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                            sv6621_service_channel_consumer, &dev->service);
deinit_rx:
  sv6621_rx_deinit(&dev->rx);
deinit_service:
  sv6621_service_deinit(&dev->service);
deinit_command:
  sv6621_command_engine_deinit(&dev->command);
deinit_tx:
  sv6621_tx_deinit(&dev->tx);
deinit_router:
  sv6621_packet_router_deinit(&dev->router);
destroy_status_lock:
  nxmutex_destroy(&dev->status_lock);
destroy_lifecycle_lock:
  nxmutex_destroy(&dev->lifecycle_lock);
free_device:
  kmm_free(dev);
  return ret;
}

void sv6621_destroy(FAR struct sv6621_dev_s *dev)
{
  if (dev == NULL)
    {
      return;
    }

  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                            sv6621_command_channel_consumer, &dev->command);
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                            sv6621_service_channel_consumer, &dev->service);
  sv6621_rx_deinit(&dev->rx);
  sv6621_service_deinit(&dev->service);
  sv6621_command_engine_deinit(&dev->command);
  sv6621_tx_deinit(&dev->tx);
  sv6621_packet_router_deinit(&dev->router);
  nxmutex_destroy(&dev->status_lock);
  nxmutex_destroy(&dev->lifecycle_lock);
  kmm_free(dev);
}

int sv6621_get_status(FAR struct sv6621_dev_s *dev,
                      FAR struct sv6621_status_s *status)
{
  int ret;

  if (dev == NULL || status == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  *status = dev->status;
  nxmutex_unlock(&dev->status_lock);
  return 0;
}
