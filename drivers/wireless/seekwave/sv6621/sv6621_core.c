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
#include "sv6621_firmware.h"
#include "sv6621_regulatory.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_CORE_BSP_TIMEOUT_MS  2000
#define SV6621_CORE_WIFI_TIMEOUT_MS 2000
#define SV6621_CORE_SCAN_TIMEOUT_MS 10000
#define SV6621_CORE_CONNECT_TIMEOUT_MS 5000
#define SV6621_CORE_HANDSHAKE_TIMEOUT_MS 10000
#define SV6621_CORE_EVENT_CREDIT_UPDATE 16
#define SV6621_CORE_EVENT_THERMAL_WARN  18

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_core_service_event(enum sv6621_service_event_e event,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg);
static void sv6621_core_rx_error(int error, FAR void *arg);
static void sv6621_core_command_event(uint8_t instance, uint8_t id,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg);
static void sv6621_core_scan_complete(int result, FAR void *arg);
static void sv6621_core_scan_worker(FAR void *arg);
static void sv6621_core_station_event(bool connected, uint16_t reason,
                                      FAR void *arg);
static void sv6621_core_station_worker(FAR void *arg);
static void sv6621_core_data_input(FAR const struct sv6621_data_rx_s *rx,
                                   FAR void *arg);
static void sv6621_core_report(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length);
static void sv6621_core_event_worker(FAR void *arg);
static void sv6621_core_queue_fatal(FAR struct sv6621_dev_s *dev);
static void sv6621_core_queue_recovery(FAR struct sv6621_dev_s *dev,
                                       int error);
static void sv6621_core_recovery_worker(FAR void *arg);
static void sv6621_core_thermal_worker(FAR void *arg);
static int sv6621_core_set_state(FAR struct sv6621_dev_s *dev,
                                 enum sv6621_state_e state, int error);

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
 * Name: sv6621_core_set_state
 ****************************************************************************/

static int sv6621_core_set_state(FAR struct sv6621_dev_s *dev,
                                 enum sv6621_state_e state, int error)
{
  int ret;

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  dev->status.state = state;
  dev->status.last_error = error;
  nxmutex_unlock(&dev->status_lock);
  return 0;
}

/****************************************************************************
 * Name: sv6621_core_event_worker
 ****************************************************************************/

static void sv6621_core_event_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  int error = -EIO;

  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      error = dev->status.last_error;
      nxmutex_unlock(&dev->status_lock);
    }

  sv6621_core_report(dev, SV6621_EVENT_FATAL, &error, sizeof(error));
}

/****************************************************************************
 * Name: sv6621_core_queue_fatal
 ****************************************************************************/

static void sv6621_core_queue_fatal(FAR struct sv6621_dev_s *dev)
{
  if (work_available(&dev->event_work))
    {
      work_queue(LPWORK, &dev->event_work, sv6621_core_event_worker, dev, 0);
    }
}

/****************************************************************************
 * Name: sv6621_core_queue_recovery
 ****************************************************************************/

static void sv6621_core_queue_recovery(FAR struct sv6621_dev_s *dev,
                                       int error)
{
  bool queue = false;
  int ret;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  if (!dev->recovery_pending &&
      dev->status.state != SV6621_STATE_OFF &&
      dev->status.state != SV6621_STATE_STOPPING &&
      dev->status.state != SV6621_STATE_FAILED)
    {
      dev->recovery_pending = true;
      dev->status.last_error = error;
      queue = true;
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = work_queue(LPWORK, &dev->recovery_work,
                   sv6621_core_recovery_worker, dev, 0);
  if (ret < 0)
    {
      if (nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->recovery_pending = false;
          dev->status.state = SV6621_STATE_FAILED;
          dev->status.last_error = ret;
          nxmutex_unlock(&dev->status_lock);
        }

      sv6621_core_queue_fatal(dev);
    }
}

/****************************************************************************
 * Name: sv6621_core_thermal_worker
 ****************************************************************************/

static void sv6621_core_thermal_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  struct sv6621_thermal_s thermal;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  thermal.transmit_blocked = dev->thermal_blocked;
  nxmutex_unlock(&dev->status_lock);
  sv6621_core_report(dev, SV6621_EVENT_THERMAL_CHANGED, &thermal,
                     sizeof(thermal));
}

/****************************************************************************
 * Name: sv6621_core_recovery_worker
 ****************************************************************************/

static void sv6621_core_recovery_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  enum sv6621_state_e state = SV6621_STATE_RECOVERING;
  int error = -EIO;
  int ret;

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return;
    }

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return;
    }

  if (!dev->recovery_pending || dev->status.state == SV6621_STATE_OFF ||
      dev->status.state == SV6621_STATE_STOPPING)
    {
      dev->recovery_pending = false;
      nxmutex_unlock(&dev->status_lock);
      nxmutex_unlock(&dev->lifecycle_lock);
      return;
    }

  error = dev->status.last_error;
  dev->recovery_pending = false;
  dev->status.state = SV6621_STATE_RECOVERING;
  dev->status.recovery_count++;
  nxmutex_unlock(&dev->status_lock);
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  sv6621_core_report(dev, SV6621_EVENT_RECOVERY_STARTED, &error,
                     sizeof(error));

#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  sv6621_scan_controller_cancel(&dev->scan);
  sv6621_wpa_cancel(&dev->wpa, error);
  sv6621_station_reset(&dev->station, error);
  sv6621_command_cancel(&dev->command, error);
  sv6621_data_reset_credits(&dev->data);
  sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
  sv6621_rx_stop(&dev->rx);
  dev->suspended = false;
  dev->station_open = false;

  if (dev->transport_open)
    {
      dev->config.transport->ops->close(dev->config.transport);
      dev->transport_open = false;
    }

  if (dev->powered)
    {
      dev->config.board_ops->power_off(dev->config.board_arg);
      dev->powered = false;
    }

  sv6621_service_reset(&dev->service);
  sv6621_core_set_state(dev, SV6621_STATE_FAILED, error);
  nxmutex_unlock(&dev->lifecycle_lock);

  ret = sv6621_start(dev);
  if (ret == 0)
    {
      sv6621_core_report(dev, SV6621_EVENT_RECOVERY_COMPLETE, NULL, 0);
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

  sv6621_core_queue_recovery(dev, -EIO);
  (void)payload;
  (void)length;
}

/****************************************************************************
 * Name: sv6621_core_rx_error
 ****************************************************************************/

static void sv6621_core_rx_error(int error, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_core_queue_recovery(dev, error);
}

/****************************************************************************
 * Name: sv6621_core_command_event
 ****************************************************************************/

static void sv6621_core_command_event(uint8_t instance, uint8_t id,
                                      FAR const uint8_t *payload,
                                      size_t length, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (id == SV6621_CORE_EVENT_CREDIT_UPDATE && length >= 4)
    {
      uint16_t lmac0 = payload[0] | ((uint16_t)payload[1] << 8);
      uint16_t lmac1 = payload[2] | ((uint16_t)payload[3] << 8);

      sv6621_data_add_credits(&dev->data, lmac0, lmac1);
#ifdef CONFIG_NET
      sv6621_network_credit_available(&dev->network);
#endif
      return;
    }

  if (id == SV6621_CORE_EVENT_THERMAL_WARN)
    {
      bool blocked;

      if (length != 1)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      blocked = payload[0] == 0;
      if (sv6621_data_set_tx_block(&dev->data,
                                   SV6621_DATA_TX_BLOCK_THERMAL,
                                   blocked) < 0)
        {
          sv6621_core_queue_recovery(dev, -EIO);
          return;
        }

      if (nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->thermal_blocked = blocked;
          nxmutex_unlock(&dev->status_lock);
        }

#ifdef CONFIG_NET
      if (!blocked)
        {
          sv6621_network_credit_available(&dev->network);
        }
#endif
      if (work_available(&dev->thermal_work))
        {
          work_queue(LPWORK, &dev->thermal_work,
                     sv6621_core_thermal_worker, dev, 0);
        }

      return;
    }

  sv6621_scan_command_event(instance, id, payload, length, &dev->scan);
  sv6621_station_command_event(instance, id, payload, length, &dev->station);
}

/****************************************************************************
 * Name: sv6621_core_station_event
 ****************************************************************************/

static void sv6621_core_station_event(bool connected, uint16_t reason,
                                      FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  dev->station_connected = connected;
  dev->station_reason = reason;
  nxmutex_unlock(&dev->status_lock);
  if (work_available(&dev->station_work))
    {
      work_queue(LPWORK, &dev->station_work, sv6621_core_station_worker, dev,
                 0);
    }
}

/****************************************************************************
 * Name: sv6621_core_station_worker
 ****************************************************************************/

static void sv6621_core_station_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  struct sv6621_status_s status;
#ifdef CONFIG_NET
  struct sv6621_data_tx_context_s context;
  bool link_ready = false;
#endif
  uint16_t reason;
  bool connected;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  connected = dev->station_connected;
  reason = dev->station_reason;
  if (connected && nxmutex_lock(&dev->station.lock) >= 0)
    {
      memcpy(dev->status.bssid, dev->station.target.bss.bssid,
             SV6621_MAC_LENGTH);
      dev->status.channel = dev->station.target.bss.channel;
      dev->status.band = dev->station.target.bss.band;
      dev->status.signal_dbm = dev->station.target.bss.signal_dbm;
#ifdef CONFIG_NET
      context.peer_index = dev->station.peer.peer_index;
      context.multicast_index = dev->station.peer.multicast_index;
      context.instance = dev->station.peer.instance;
      context.lmac_id = dev->station.peer.lmac_id;
      context.tid = 0;
      link_ready = true;
#endif
      nxmutex_unlock(&dev->station.lock);
    }
  else if (!connected)
    {
      memset(dev->status.bssid, 0, sizeof(dev->status.bssid));
      dev->status.channel = 0;
      dev->status.signal_dbm = 0;
    }

  status = dev->status;
  nxmutex_unlock(&dev->status_lock);
#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, connected && link_ready,
                          link_ready ? &context : NULL);
#endif
  if (!connected)
    {
      sv6621_wpa_cancel(&dev->wpa, -ECONNRESET);
    }

  if (connected)
    {
      sv6621_core_report(dev, SV6621_EVENT_CONNECTED, &status,
                         sizeof(status));
    }
  else
    {
      sv6621_core_report(dev, SV6621_EVENT_DISCONNECTED, &reason,
                         sizeof(reason));
    }
}

/****************************************************************************
 * Name: sv6621_core_data_input
 ****************************************************************************/

static void sv6621_core_data_input(FAR const struct sv6621_data_rx_s *rx,
                                   FAR void *arg)
{
#ifdef CONFIG_NET
  FAR struct sv6621_dev_s *dev = arg;

  sv6621_network_input(rx, &dev->network);
#else
  (void)rx;
  (void)arg;
#endif
}

/****************************************************************************
 * Name: sv6621_core_scan_complete
 ****************************************************************************/

static void sv6621_core_scan_complete(int result, FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  int ret;

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return;
    }

  if (dev->scan_reporting)
    {
      nxmutex_unlock(&dev->status_lock);
      return;
    }

  dev->scan_reporting = true;
  dev->scan_result = result;
  nxmutex_unlock(&dev->status_lock);
  ret = work_queue(LPWORK, &dev->scan_work, sv6621_core_scan_worker, dev, 0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->scan_reporting = false;
      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_scan_worker
 ****************************************************************************/

static void sv6621_core_scan_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  FAR struct sv6621_bss_s *entries;
  size_t count = SV6621_SCAN_CACHE_CAPACITY;
  size_t index;
  int result;
  int ret;

  entries = kmm_malloc(sizeof(*entries) * SV6621_SCAN_CACHE_CAPACITY);
  if (entries == NULL)
    {
      count = 0;
      result = -ENOMEM;
    }
  else
    {
      ret = sv6621_scan_cache_snapshot(&dev->scan.cache, entries, &count);
      result = ret < 0 ? ret : dev->scan_result;
    }

  for (index = 0; index < count; index++)
    {
      sv6621_core_report(dev, SV6621_EVENT_SCAN_RESULT, &entries[index],
                         sizeof(entries[index]));
    }

  kmm_free(entries);
  if (nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->scan_reporting = false;
      nxmutex_unlock(&dev->status_lock);
    }

  sv6621_core_report(dev, SV6621_EVENT_SCAN_COMPLETE, &result, sizeof(result));
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
      config->board_ops->power_off == NULL ||
      config->board_ops->load_address == NULL ||
      config->board_ops->store_address == NULL || config->iram.data == NULL ||
      config->iram.length == 0 || config->dram.data == NULL ||
      config->dram.length == 0 || config->nvram.data == NULL ||
      config->nvram.length == 0 || config->calibration.data == NULL ||
      config->calibration.length == 0 || config->regulatory == NULL)
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
  ret = sv6621_regulatory_scan_channels(
      config->regulatory, dev->scan_channels,
      SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY, &dev->scan_channel_count);
  if (ret < 0)
    {
      goto free_device;
    }

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

  ret = sv6621_data_init(&dev->data, &dev->router, &dev->tx,
                         sv6621_core_data_input, dev);
  if (ret < 0)
    {
      goto deinit_tx;
    }

  ret = sv6621_scan_controller_init(&dev->scan, &dev->command,
                                    SV6621_CORE_SCAN_TIMEOUT_MS,
                                    sv6621_core_scan_complete, dev);
  if (ret < 0)
    {
      goto deinit_data;
    }

  ret = sv6621_command_engine_init(&dev->command, sv6621_tx_command_sender,
                                   &dev->tx, sv6621_core_command_event, dev,
                                   sv6621_core_rx_error, dev);
  if (ret < 0)
    {
      goto deinit_scan;
    }

  ret = sv6621_station_init(&dev->station, &dev->command, &dev->scan,
                            sv6621_core_station_event, dev);
  if (ret < 0)
    {
      goto deinit_command;
    }

  ret = sv6621_wpa_init(&dev->wpa, &dev->command, &dev->station);
  if (ret < 0)
    {
      goto deinit_station;
    }

  sv6621_data_set_eapol_input(&dev->data, sv6621_wpa_input, &dev->wpa);
  ret = sv6621_service_init(&dev->service, sv6621_core_service_event, dev);
  if (ret < 0)
    {
      goto deinit_wpa;
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
deinit_wpa:
  sv6621_data_set_eapol_input(&dev->data, NULL, NULL);
  sv6621_wpa_deinit(&dev->wpa);
deinit_station:
  sv6621_station_deinit(&dev->station);
deinit_command:
  sv6621_command_engine_deinit(&dev->command);
deinit_scan:
  sv6621_scan_controller_deinit(&dev->scan);
deinit_data:
  sv6621_data_deinit(&dev->data);
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

  dev->config.event = NULL;
  sv6621_stop(dev);
  work_cancel_sync(LPWORK, &dev->recovery_work);
  work_cancel_sync(LPWORK, &dev->thermal_work);
  work_cancel_sync(LPWORK, &dev->event_work);
  work_cancel_sync(LPWORK, &dev->scan_work);
  work_cancel_sync(LPWORK, &dev->station_work);
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                            sv6621_command_channel_consumer, &dev->command);
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_LOOPCHECK,
                            sv6621_service_channel_consumer, &dev->service);
  sv6621_rx_deinit(&dev->rx);
#ifdef CONFIG_NET
  sv6621_network_deinit(&dev->network);
#endif
  sv6621_service_deinit(&dev->service);
  sv6621_data_set_eapol_input(&dev->data, NULL, NULL);
  sv6621_wpa_deinit(&dev->wpa);
  sv6621_station_deinit(&dev->station);
  sv6621_scan_controller_deinit(&dev->scan);
  sv6621_command_engine_deinit(&dev->command);
  sv6621_data_deinit(&dev->data);
  sv6621_tx_deinit(&dev->tx);
  sv6621_packet_router_deinit(&dev->router);
  nxmutex_destroy(&dev->status_lock);
  nxmutex_destroy(&dev->lifecycle_lock);
  kmm_free(dev);
}

int sv6621_start(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  bool rx_started = false;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_WIFI_READY)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_OFF && state != SV6621_STATE_FAILED)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_STARTING, 0);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto fail;
    }

  dev->thermal_blocked = false;
  nxmutex_unlock(&dev->status_lock);

  ret = dev->config.transport->ops->open(dev->config.transport);
  if (ret < 0)
    {
      goto fail;
    }

  dev->transport_open = true;
  ret = dev->config.board_ops->power_on(dev->config.board_arg);
  if (ret < 0)
    {
      goto fail;
    }

  dev->powered = true;
  ret = dev->config.transport->ops->enumerate(dev->config.transport);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_service_reset(&dev->service);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_command_reset(&dev->command);
  if (ret < 0)
    {
      goto fail;
    }

  sv6621_data_reset_credits(&dev->data);
  ret = sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_rx_start(&dev->rx);
  if (ret < 0)
    {
      goto fail;
    }

  rx_started = true;
  ret = sv6621_firmware_download(
      dev->config.transport, dev->config.iram.data, dev->config.iram.length,
      dev->config.dram.data, dev->config.dram.length, dev->config.nvram.data,
      dev->config.nvram.length);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_service_wait_bsp(&dev->service, SV6621_CORE_BSP_TIMEOUT_MS);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_BSP_READY, 0);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_STARTING, 0);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_service_start_wifi(&dev->service, dev->config.transport,
                                  SV6621_CORE_WIFI_TIMEOUT_MS);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_wifi_sync_versions(&dev->command);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

  ret = sv6621_wifi_get_info(&dev->command, dev->config.board_ops,
                             dev->config.board_arg, &dev->wifi_info);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

#ifdef CONFIG_NET
  if (!dev->network.registered)
    {
      ret = sv6621_network_init(&dev->network, &dev->data, &dev->command,
                                dev->wifi_info.max_multicast_addresses,
                                dev->wifi_info.mac);
      if (ret < 0)
        {
          goto fail;
        }
    }
#endif

  ret = sv6621_wifi_configure_baseline(&dev->command);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_wifi_download_calibration(&dev->command,
                                         dev->config.calibration.data,
                                         dev->config.calibration.length);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      goto fail;
    }

  ret = sv6621_regulatory_set_domain(&dev->command,
                                     dev->config.regulatory);
  if (ret < 0)
    {
      goto fail;
    }

  ret = sv6621_wifi_open_station(&dev->command, dev->wifi_info.mac);
  if (ret < 0)
    {
      goto fail;
    }

  dev->station_open = true;

#ifdef CONFIG_NET
  ret = sv6621_network_sync_multicast(&dev->network);
  if (ret < 0)
    {
      goto fail;
    }
#endif

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto fail;
    }

  memcpy(dev->status.mac, dev->wifi_info.mac, SV6621_MAC_LENGTH);
  nxmutex_unlock(&dev->status_lock);

  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_READY, 0);
  if (ret < 0)
    {
      goto fail;
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  state = SV6621_STATE_WIFI_READY;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));

  return 0;

fail:
#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  if (dev->station_open)
    {
      sv6621_wifi_close_station(&dev->command);
      dev->station_open = false;
    }

  if (rx_started)
    {
      sv6621_rx_stop(&dev->rx);
    }

  if (dev->transport_open)
    {
      dev->config.transport->ops->close(dev->config.transport);
      dev->transport_open = false;
    }

  if (dev->powered)
    {
      dev->config.board_ops->power_off(dev->config.board_arg);
      dev->powered = false;
    }

  sv6621_core_set_state(dev, SV6621_STATE_FAILED, ret);
  nxmutex_unlock(&dev->lifecycle_lock);
  state = SV6621_STATE_FAILED;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  sv6621_core_report(dev, SV6621_EVENT_FATAL, &ret, sizeof(ret));
  return ret;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_stop(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  uint32_t recovery_count;
  int scan_ret = 0;
  int close_ret = 0;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return ret;
    }

  state = dev->status.state;
  recovery_count = dev->status.recovery_count;
  dev->recovery_pending = false;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_OFF)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return 0;
    }

  sv6621_core_set_state(dev, SV6621_STATE_STOPPING, 0);
#ifdef CONFIG_NET
  sv6621_network_set_link(&dev->network, false, NULL);
#endif
  scan_ret = sv6621_scan_controller_cancel(&dev->scan);
  sv6621_wpa_cancel(&dev->wpa, -ESHUTDOWN);
  sv6621_station_disconnect(&dev->station, 3);
  sv6621_station_reset(&dev->station, -ESHUTDOWN);
  if (dev->station_open)
    {
      close_ret = sv6621_wifi_close_station(&dev->command);
      dev->station_open = false;
    }

  sv6621_command_cancel(&dev->command, -ESHUTDOWN);
  sv6621_data_reset_credits(&dev->data);
  sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
  sv6621_rx_stop(&dev->rx);
  dev->suspended = false;
  if (dev->transport_open)
    {
      dev->config.transport->ops->close(dev->config.transport);
      dev->transport_open = false;
    }

  if (dev->powered)
    {
      dev->config.board_ops->power_off(dev->config.board_arg);
      dev->powered = false;
    }

  sv6621_service_reset(&dev->service);
  ret = nxmutex_lock(&dev->status_lock);
  if (ret >= 0)
    {
      memset(&dev->status, 0, sizeof(dev->status));
      dev->status.state = SV6621_STATE_OFF;
      dev->status.recovery_count = recovery_count;
      dev->thermal_blocked = false;
      nxmutex_unlock(&dev->status_lock);
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  state = SV6621_STATE_OFF;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  if (scan_ret < 0)
    {
      return scan_ret;
    }

  return close_ret < 0 ? close_ret : 0;
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

/****************************************************************************
 * Name: sv6621_get_link_stats
 ****************************************************************************/

int sv6621_get_link_stats(FAR struct sv6621_dev_s *dev,
                          FAR struct sv6621_link_stats_s *stats)
{
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t instance;
  int ret;

  if (dev == NULL || stats == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY ||
      !dev->station_connected)
    {
      ret = -ENOTCONN;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  memcpy(bssid, dev->status.bssid, sizeof(bssid));
  nxmutex_unlock(&dev->status_lock);

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  instance = dev->station.peer.instance;
  nxmutex_unlock(&dev->station.lock);
  ret = sv6621_stats_query(&dev->command, instance, bssid, stats);
  if (ret == 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->status.signal_dbm = stats->signal_dbm;
      nxmutex_unlock(&dev->status_lock);
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_scan(FAR struct sv6621_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }
  else if (dev->scan_reporting)
    {
      ret = -EBUSY;
    }
  else
    {
      ret = 0;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_scan_controller_begin(&dev->scan, dev->scan_channels,
                                     dev->scan_channel_count);

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_connect(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_connect_s *request)
{
  FAR struct sv6621_scan_entry_s *target = NULL;
  bool wpa_prepared = false;
  int ret;

  if (dev == NULL || request == NULL)
    {
      return -EINVAL;
    }

  if (request->security != SV6621_SECURITY_OPEN &&
      request->security != SV6621_SECURITY_WPA2_PSK)
    {
      return -EOPNOTSUPP;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
      goto unlock_lifecycle;
    }

  ret = sv6621_scan_controller_cancel(&dev->scan);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (dev->station.state != SV6621_STATION_IDLE)
    {
      ret = -EBUSY;
    }

  nxmutex_unlock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  if (request->security == SV6621_SECURITY_WPA2_PSK)
    {
      target = kmm_malloc(sizeof(*target));
      if (target == NULL)
        {
          ret = -ENOMEM;
          goto unlock_lifecycle;
        }

      ret = sv6621_scan_cache_find(&dev->scan.cache, request, target);
      if (ret < 0)
        {
          goto free_target;
        }

      ret = sv6621_wpa_prepare(&dev->wpa, request, dev->wifi_info.mac,
                               target->bss.bssid);
      if (ret < 0)
        {
          goto free_target;
        }

      wpa_prepared = true;
      kmm_free(target);
      target = NULL;
    }

  ret = sv6621_station_connect(&dev->station, request,
                               SV6621_CORE_CONNECT_TIMEOUT_MS);
  if (ret < 0)
    {
      goto cancel_wpa;
    }

  if (wpa_prepared)
    {
      ret = sv6621_wpa_run(&dev->wpa, &dev->station.peer,
                           SV6621_CORE_HANDSHAKE_TIMEOUT_MS);
      if (ret < 0)
        {
          sv6621_station_disconnect(&dev->station, 1);
          goto cancel_wpa;
        }
    }

  goto unlock_lifecycle;

free_target:
  kmm_free(target);
cancel_wpa:
  if (wpa_prepared)
    {
      sv6621_wpa_cancel(&dev->wpa, ret);
    }

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_disconnect(FAR struct sv6621_dev_s *dev, uint16_t reason)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }
  else
    {
      ret = sv6621_station_disconnect(&dev->station, reason);
      if (ret == 0)
        {
          sv6621_wpa_cancel(&dev->wpa, -ENOTCONN);
        }
    }

  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_suspend
 ****************************************************************************/

int sv6621_suspend(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_suspend_s *config)
{
  enum sv6621_wpa_state_e wpa_state;
  enum sv6621_station_state_e station_state;
  enum sv6621_state_e state;
  bool scan_active;
  int ret;

  if (dev == NULL || config == NULL ||
      (config->wake_flags & ~SV6621_WAKE_ALL) != 0 ||
      (!config->wake_enabled && config->wake_flags != 0))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_SUSPENDED)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_WIFI_READY)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = nxmutex_lock(&dev->scan.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  scan_active = dev->scan.active;
  nxmutex_unlock(&dev->scan.lock);

  ret = nxmutex_lock(&dev->wpa.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  wpa_state = dev->wpa.state;
  nxmutex_unlock(&dev->wpa.lock);

  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  station_state = dev->station.state;
  nxmutex_unlock(&dev->station.lock);
  if (scan_active ||
      (wpa_state != SV6621_WPA_IDLE && wpa_state != SV6621_WPA_COMPLETE) ||
      (station_state != SV6621_STATION_IDLE &&
       station_state != SV6621_STATION_CONNECTED))
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

#ifdef CONFIG_NET
  if (dev->station_connected)
    {
      ret = sv6621_network_sync_addresses(&dev->network);
      if (ret != 0)
        {
          ret = ret < 0 ? ret : -EREMOTEIO;
          goto unlock_lifecycle;
        }
    }
#endif

  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP,
                                 true);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  ret = sv6621_rx_suspend(&dev->rx);
  if (ret < 0)
    {
      sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP,
                               false);
      goto unlock_lifecycle;
    }

  ret = sv6621_power_suspend(&dev->command, config->wake_enabled,
                             config->wake_flags);
  if (ret < 0)
    {
      sv6621_rx_resume(&dev->rx);
      sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP,
                               false);
      goto unlock_lifecycle;
    }

  dev->suspended = true;
  ret = sv6621_core_set_state(dev, SV6621_STATE_SUSPENDED, 0);
  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  state = SV6621_STATE_SUSPENDED;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  return 0;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

/****************************************************************************
 * Name: sv6621_resume
 ****************************************************************************/

int sv6621_resume(FAR struct sv6621_dev_s *dev)
{
  enum sv6621_state_e state;
  bool recover = false;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  state = dev->status.state;
  nxmutex_unlock(&dev->status_lock);
  if (state == SV6621_STATE_WIFI_READY)
    {
      ret = 0;
      goto unlock_lifecycle;
    }

  if (state != SV6621_STATE_SUSPENDED || !dev->suspended)
    {
      ret = -EBUSY;
      goto unlock_lifecycle;
    }

  ret = sv6621_rx_resume(&dev->rx);
  if (ret < 0)
    {
      recover = true;
      goto unlock_lifecycle;
    }

  ret = sv6621_power_resume(&dev->command);
  if (ret != 0)
    {
      ret = ret < 0 ? ret : -EREMOTEIO;
      recover = true;
      goto unlock_lifecycle;
    }

  ret = sv6621_data_set_tx_block(&dev->data, SV6621_DATA_TX_BLOCK_SLEEP,
                                 false);
  if (ret < 0)
    {
      recover = true;
      goto unlock_lifecycle;
    }

  dev->suspended = false;
  ret = sv6621_core_set_state(dev, SV6621_STATE_WIFI_READY, 0);
  nxmutex_unlock(&dev->lifecycle_lock);
  if (ret < 0)
    {
      return ret;
    }

#ifdef CONFIG_NET
  sv6621_network_credit_available(&dev->network);
#endif
  state = SV6621_STATE_WIFI_READY;
  sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state, sizeof(state));
  return 0;

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  if (recover)
    {
      sv6621_core_queue_recovery(dev, ret);
    }

  return ret;
}
