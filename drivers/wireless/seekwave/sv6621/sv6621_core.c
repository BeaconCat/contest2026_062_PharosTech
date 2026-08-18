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
#define SV6621_CORE_EVENT_BA_ACTION     13
#define SV6621_CORE_EVENT_CREDIT_UPDATE 16
#define SV6621_CORE_EVENT_MIC_FAILURE   17
#define SV6621_CORE_EVENT_THERMAL_WARN  18
#define SV6621_CORE_EVENT_CQM           20
#define SV6621_CORE_EVENT_UNPROTECTED_FRAME 21
#define SV6621_CORE_EVENT_CHANNEL_SWITCH 22
#define SV6621_CORE_EVENT_FW_RECOVERY   29
#define SV6621_CORE_MIC_FAILURE_SIZE    9
#define SV6621_CORE_CQM_EVENT_SIZE      11
#define SV6621_CORE_CHANNEL_EVENT_SIZE  11
#define SV6621_CORE_CQM_THRESHOLD_DBM  -70
#define SV6621_CORE_CQM_HYSTERESIS_DB   40

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
static bool sv6621_core_station_worker_stop(
    FAR struct sv6621_dev_s *dev, uint32_t generation);
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
static void sv6621_core_security_worker(FAR void *arg);
static void sv6621_core_signal_worker(FAR void *arg);
static int sv6621_core_channel_switch(FAR struct sv6621_dev_s *dev,
                                      FAR const uint8_t *payload,
                                      size_t length);
static int sv6621_core_set_state(FAR struct sv6621_dev_s *dev,
                                 enum sv6621_state_e state, int error);
#ifdef CONFIG_SV6621_PM
static int sv6621_core_pm_prepare(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state);
#endif

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

#ifdef CONFIG_SV6621_PM
/****************************************************************************
 * Name: sv6621_core_pm_prepare
 ****************************************************************************/

static int sv6621_core_pm_prepare(FAR struct pm_callback_s *callback,
                                  int domain, enum pm_state_e state)
{
  FAR struct sv6621_dev_s *dev =
      container_of(callback, struct sv6621_dev_s, pm_callback);
  struct sv6621_status_s status;
  int ret;

  if (domain != PM_IDLE_DOMAIN)
    {
      return 0;
    }

  if (state == PM_SLEEP)
    {
      if (dev->pm_suspended)
        {
          return 0;
        }

      ret = sv6621_get_status(dev, &status);
      if (ret < 0)
        {
          return ret;
        }

      if (status.state == SV6621_STATE_OFF ||
          status.state == SV6621_STATE_FAILED)
        {
          return 0;
        }

      if (status.state == SV6621_STATE_SUSPENDED)
        {
          return 0;
        }

      ret = sv6621_suspend(dev, &dev->config.system_suspend);
      if (ret == 0)
        {
          dev->pm_suspended = true;
        }

      return ret;
    }

  if (dev->pm_suspended)
    {
      ret = sv6621_resume(dev);
      if (ret == 0)
        {
          dev->pm_suspended = false;
        }
    }

  return 0;
}
#endif

/****************************************************************************
 * Name: sv6621_core_event_worker
 ****************************************************************************/

static void sv6621_core_event_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  uint32_t generation;

  for (;;)
    {
      int error;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->fatal_generation;
      error = dev->status.last_error;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_FATAL, &error, sizeof(error));

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->fatal_generation)
        {
          dev->fatal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_queue_fatal
 ****************************************************************************/

static void sv6621_core_queue_fatal(FAR struct sv6621_dev_s *dev)
{
  bool queue = false;
  int ret;

  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  dev->fatal_generation++;
  if (!dev->fatal_work_scheduled)
    {
      dev->fatal_work_scheduled = true;
      queue = true;
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = work_queue(LPWORK, &dev->event_work, sv6621_core_event_worker, dev,
                   0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->fatal_work_scheduled = false;
      nxmutex_unlock(&dev->status_lock);
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

  if (dev->status.state != SV6621_STATE_OFF &&
      dev->status.state != SV6621_STATE_STOPPING &&
      dev->status.state != SV6621_STATE_FAILED)
    {
      if (!dev->recovery_pending)
        {
          dev->recovery_pending = true;
          dev->status.last_error = error;
        }

      if (!dev->recovery_running)
        {
          dev->recovery_running = true;
          queue = true;
        }
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
          dev->recovery_running = false;
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
  uint32_t generation;

  for (;;)
    {
      struct sv6621_thermal_s thermal;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->thermal_generation;
      thermal.transmit_blocked = dev->thermal_blocked;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_THERMAL_CHANGED, &thermal,
                         sizeof(thermal));

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->thermal_generation)
        {
          dev->thermal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_security_worker
 ****************************************************************************/

static void sv6621_core_security_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  for (;;)
    {
      struct sv6621_mic_failure_s failure;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (dev->security_tail == dev->security_head)
        {
          dev->security_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      failure = dev->security_events[dev->security_tail];
      dev->security_tail =
          (dev->security_tail + 1) % SV6621_CORE_SECURITY_EVENT_DEPTH;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_MIC_FAILURE, &failure,
                         sizeof(failure));
    }
}

/****************************************************************************
 * Name: sv6621_core_signal_worker
 ****************************************************************************/

static void sv6621_core_signal_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;

  for (;;)
    {
      struct sv6621_signal_event_s event;

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (dev->signal_tail == dev->signal_head)
        {
          dev->signal_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      event = dev->signal_events[dev->signal_tail];
      dev->signal_tail =
          (dev->signal_tail + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
      if (!dev->station_connected ||
          nxmutex_lock(&dev->station.lock) < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          continue;
        }

      if (memcmp(event.bssid, dev->station.target.bss.bssid,
                 SV6621_MAC_LENGTH) != 0)
        {
          nxmutex_unlock(&dev->station.lock);
          nxmutex_unlock(&dev->status_lock);
          continue;
        }

      nxmutex_unlock(&dev->station.lock);
      dev->status.signal_dbm = event.signal_dbm;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_SIGNAL_CHANGED, &event,
                         sizeof(event));
    }
}

/****************************************************************************
 * Name: sv6621_core_recovery_worker
 ****************************************************************************/

static void sv6621_core_recovery_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  int ret;

  for (;;)
    {
      enum sv6621_state_e state = SV6621_STATE_RECOVERING;
      uint16_t disconnect_reason = 0;
      bool report_disconnect = false;
      int error = -EIO;

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
          dev->status.state == SV6621_STATE_STOPPING ||
          dev->status.state == SV6621_STATE_FAILED)
        {
          dev->recovery_pending = false;
          dev->recovery_running = false;
          nxmutex_unlock(&dev->status_lock);
          nxmutex_unlock(&dev->lifecycle_lock);
          return;
        }

      error = dev->status.last_error;
      dev->recovery_pending = false;
      dev->status.state = SV6621_STATE_RECOVERING;
      dev->status.recovery_count++;
      if (dev->station_connected || dev->status.connected)
        {
          report_disconnect = !dev->station_work_scheduled;
          dev->station_connected = false;
          dev->station_reason = disconnect_reason;
          dev->station_generation++;
        }

      dev->status.connected = false;
      memset(dev->status.bssid, 0, sizeof(dev->status.bssid));
      memset(dev->status.ssid, 0, sizeof(dev->status.ssid));
      dev->status.ssid_length = 0;
      dev->status.channel = 0;
      dev->status.signal_dbm = 0;
      nxmutex_unlock(&dev->status_lock);
      sv6621_core_report(dev, SV6621_EVENT_STATE_CHANGED, &state,
                         sizeof(state));
      sv6621_core_report(dev, SV6621_EVENT_RECOVERY_STARTED, &error,
                         sizeof(error));

#ifdef CONFIG_NET
      sv6621_network_set_link(&dev->network, false, NULL);
#endif
      sv6621_scan_controller_cancel(&dev->scan);
      sv6621_wpa_cancel(&dev->wpa, error);
      sv6621_station_reset(&dev->station, error);
      if (report_disconnect)
        {
          sv6621_core_report(dev, SV6621_EVENT_DISCONNECTED,
                             &disconnect_reason,
                             sizeof(disconnect_reason));
        }

      sv6621_command_cancel(&dev->command, error);
      sv6621_data_reset_credits(&dev->data);
      sv6621_data_set_tx_block(&dev->data, UINT8_MAX, false);
      sv6621_rx_stop(&dev->rx);
      sv6621_data_reset_fragments(&dev->data);
      sv6621_data_reset_ba(&dev->data);
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

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (!dev->recovery_pending || dev->status.state == SV6621_STATE_OFF ||
          dev->status.state == SV6621_STATE_STOPPING ||
          dev->status.state == SV6621_STATE_FAILED)
        {
          dev->recovery_pending = false;
          dev->recovery_running = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
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
 * Name: sv6621_core_channel_switch
 ****************************************************************************/

static int sv6621_core_channel_switch(FAR struct sv6621_dev_s *dev,
                                      FAR const uint8_t *payload,
                                      size_t length)
{
#ifdef CONFIG_NET
  struct sv6621_data_tx_context_s context;
#endif
  uint8_t channel;
  uint8_t band;
  bool connected;
  int ret;

  if (length != SV6621_CORE_CHANNEL_EVENT_SIZE || payload[0] > 1 ||
      payload[2] == 0 || payload[6] > SV6621_BAND_5GHZ)
    {
      return -EPROTO;
    }

  if (payload[0] != 0)
    {
      ret = sv6621_data_set_tx_block(&dev->data,
                                     SV6621_DATA_TX_BLOCK_CHANNEL, true);
#ifdef CONFIG_NET
      if (ret >= 0)
        {
          sv6621_network_set_link(&dev->network, false, NULL);
        }
#endif
      return ret;
    }

  channel = payload[2];
  band = payload[6];
  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      return ret;
    }

  connected = dev->station_connected;
  if (connected)
    {
      ret = nxmutex_lock(&dev->station.lock);
      if (ret < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          return ret;
        }

      dev->status.channel = channel;
      dev->status.band = (enum sv6621_band_e)band;
      dev->station.target.bss.channel = channel;
      dev->station.target.bss.band = (enum sv6621_band_e)band;
#ifdef CONFIG_NET
      context.peer_index = dev->station.peer.peer_index;
      context.multicast_index = dev->station.peer.multicast_index;
      context.instance = dev->station.peer.instance;
      context.lmac_id = dev->station.peer.lmac_id;
      context.tid = 0;
#endif
      nxmutex_unlock(&dev->station.lock);
    }

  nxmutex_unlock(&dev->status_lock);
  ret = sv6621_data_set_tx_block(&dev->data,
                                 SV6621_DATA_TX_BLOCK_CHANNEL, false);
#ifdef CONFIG_NET
  if (ret >= 0 && connected)
    {
      sv6621_network_set_link(&dev->network, true, &context);
      sv6621_network_credit_available(&dev->network);
    }
#endif
  return ret;
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
      bool queue = false;
      bool blocked;
      int ret;

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

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      dev->thermal_blocked = blocked;
      dev->thermal_generation++;
      if (!dev->thermal_work_scheduled)
        {
          dev->thermal_work_scheduled = true;
          queue = true;
        }

      nxmutex_unlock(&dev->status_lock);

#ifdef CONFIG_NET
      if (!blocked)
        {
          sv6621_network_credit_available(&dev->network);
        }
#endif
      if (queue)
        {
          ret = work_queue(LPWORK, &dev->thermal_work,
                           sv6621_core_thermal_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              dev->thermal_work_scheduled = false;
              nxmutex_unlock(&dev->status_lock);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_MIC_FAILURE)
    {
      struct sv6621_mic_failure_s failure;
      uint8_t next;
      bool overflow = false;
      bool queue = false;
      int ret;

      if (length != SV6621_CORE_MIC_FAILURE_SIZE)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      failure.group_key = payload[0] != 0;
      failure.key_index = payload[1];
      failure.lmac_id = payload[2];
      memcpy(failure.address, payload + 3, SV6621_MAC_LENGTH);
      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      dev->status.mic_failures++;
      next = (dev->security_head + 1) % SV6621_CORE_SECURITY_EVENT_DEPTH;
      if (next == dev->security_tail)
        {
          dev->status.mic_failures_dropped++;
          overflow = true;
        }
      else
        {
          dev->security_events[dev->security_head] = failure;
          dev->security_head = next;
          if (!dev->security_work_scheduled)
            {
              dev->security_work_scheduled = true;
              queue = true;
            }
        }

      nxmutex_unlock(&dev->status_lock);
      if (overflow)
        {
          sv6621_core_queue_recovery(dev, -ENOBUFS);
          return;
        }

      if (queue)
        {
          ret = work_queue(LPWORK, &dev->security_work,
                           sv6621_core_security_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              uint8_t dropped =
                  dev->security_head >= dev->security_tail ?
                  dev->security_head - dev->security_tail :
                  SV6621_CORE_SECURITY_EVENT_DEPTH - dev->security_tail +
                      dev->security_head;

              dev->security_work_scheduled = false;
              dev->status.mic_failures_dropped += dropped;
              dev->security_tail = dev->security_head;
              nxmutex_unlock(&dev->status_lock);
              sv6621_core_queue_recovery(dev, ret);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_CQM)
    {
      struct sv6621_signal_event_s event;
      uint8_t next;
      bool queue = false;
      uint8_t status;
      int ret;

      if (length != SV6621_CORE_CQM_EVENT_SIZE)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      status = payload[0];
      if (status < SV6621_SIGNAL_LOW || status > SV6621_SIGNAL_TDLS_LOSS ||
          payload[10] > SV6621_BAND_5GHZ)
        {
          return;
        }

      event.status = (enum sv6621_signal_status_e)status;
      event.signal_dbm =
          (int16_t)(payload[1] | ((uint16_t)payload[2] << 8));
      memcpy(event.bssid, payload + 3, SV6621_MAC_LENGTH);
      event.channel = payload[9];
      event.band = (enum sv6621_band_e)payload[10];
      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (!dev->station_connected)
        {
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      ret = nxmutex_lock(&dev->station.lock);
      if (ret < 0)
        {
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      if (memcmp(event.bssid, dev->station.target.bss.bssid,
                 SV6621_MAC_LENGTH) != 0)
        {
          nxmutex_unlock(&dev->station.lock);
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->station.lock);
      next = (dev->signal_head + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
      if (next == dev->signal_tail)
        {
          dev->signal_tail =
              (dev->signal_tail + 1) % SV6621_CORE_SIGNAL_EVENT_DEPTH;
          dev->status.signal_events_dropped++;
        }

      dev->signal_events[dev->signal_head] = event;
      dev->signal_head = next;
      if (!dev->signal_work_scheduled)
        {
          dev->signal_work_scheduled = true;
          queue = true;
        }

      nxmutex_unlock(&dev->status_lock);
      if (queue)
        {
          ret = work_queue(LPWORK, &dev->signal_work,
                           sv6621_core_signal_worker, dev, 0);
          if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
            {
              uint8_t dropped =
                  dev->signal_head >= dev->signal_tail ?
                  dev->signal_head - dev->signal_tail :
                  SV6621_CORE_SIGNAL_EVENT_DEPTH - dev->signal_tail +
                      dev->signal_head;

              dev->signal_work_scheduled = false;
              dev->status.signal_events_dropped += dropped;
              dev->signal_tail = dev->signal_head;
              nxmutex_unlock(&dev->status_lock);
            }
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_UNPROTECTED_FRAME)
    {
      /* This port does not negotiate PMF or WAPI.  Do not reinject an
       * exceptional unprotected frame into either the network or station
       * state machine.
       */

      if (nxmutex_lock(&dev->status_lock) >= 0)
        {
          dev->status.unprotected_frames++;
          nxmutex_unlock(&dev->status_lock);
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_FW_RECOVERY)
    {
      bool blocked;

      if (length != 1)
        {
          sv6621_core_queue_recovery(dev, -EPROTO);
          return;
        }

      blocked = payload[0] == 0;
      if (sv6621_data_set_tx_block(&dev->data,
                                   SV6621_DATA_TX_BLOCK_RECOVERY,
                                   blocked) < 0)
        {
          sv6621_core_queue_recovery(dev, -EIO);
          return;
        }

#ifdef CONFIG_NET
      if (!blocked)
        {
          sv6621_network_credit_available(&dev->network);
        }
#endif
      return;
    }

  if (id == SV6621_CORE_EVENT_CHANNEL_SWITCH)
    {
      int ret = sv6621_core_channel_switch(dev, payload, length);

      if (ret < 0)
        {
          sv6621_core_queue_recovery(dev, ret);
        }

      return;
    }

  if (id == SV6621_CORE_EVENT_BA_ACTION)
    {
      (void)sv6621_data_ba_event(&dev->data, payload, length);
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
  bool queue = false;
  int ret;

  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return;
    }

  dev->station_connected = connected;
  dev->station_reason = reason;
  dev->station_generation++;
  if (!dev->station_work_scheduled)
    {
      dev->station_work_scheduled = true;
      queue = true;
    }

  nxmutex_unlock(&dev->status_lock);
  if (!queue)
    {
      return;
    }

  ret = work_queue(LPWORK, &dev->station_work,
                   sv6621_core_station_worker, dev, 0);
  if (ret < 0 && nxmutex_lock(&dev->status_lock) >= 0)
    {
      dev->station_work_scheduled = false;
      nxmutex_unlock(&dev->status_lock);
    }
}

/****************************************************************************
 * Name: sv6621_core_station_worker_stop
 ****************************************************************************/

static bool sv6621_core_station_worker_stop(
    FAR struct sv6621_dev_s *dev, uint32_t generation)
{
  if (nxmutex_lock(&dev->status_lock) < 0)
    {
      return true;
    }

  if (generation != dev->station_generation)
    {
      nxmutex_unlock(&dev->status_lock);
      return false;
    }

  dev->station_work_scheduled = false;
  nxmutex_unlock(&dev->status_lock);
  return true;
}

/****************************************************************************
 * Name: sv6621_core_station_worker
 ****************************************************************************/

static void sv6621_core_station_worker(FAR void *arg)
{
  FAR struct sv6621_dev_s *dev = arg;
  uint32_t generation;

  for (;;)
    {
      struct sv6621_status_s status;
      bool event_current = false;
#ifdef CONFIG_NET
      struct sv6621_data_tx_context_s context;
      struct sv6621_link_stats_s link_stats;
      bool link_ready = false;
#endif
      uint16_t reason;
      bool station_ready = false;
      bool connected;
      int station_ret = 0;
#ifdef CONFIG_NET
      int network_ret = 0;
#endif

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      generation = dev->station_generation;
      connected = dev->station_connected;
      reason = dev->station_reason;
      if (connected)
        {
          station_ret = nxmutex_lock(&dev->station.lock);
          if (station_ret >= 0)
            {
              memcpy(dev->status.bssid, dev->station.target.bss.bssid,
                     SV6621_MAC_LENGTH);
              dev->status.channel = dev->station.target.bss.channel;
              dev->status.band = dev->station.target.bss.band;
              dev->status.signal_dbm = dev->station.target.bss.signal_dbm;
              memcpy(dev->status.ssid, dev->station.target.bss.ssid,
                     dev->station.target.bss.ssid_length);
              dev->status.ssid_length = dev->station.target.bss.ssid_length;
              station_ready = true;
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
        }
      else if (!connected)
        {
          memset(dev->status.bssid, 0, sizeof(dev->status.bssid));
          memset(dev->status.ssid, 0, sizeof(dev->status.ssid));
          dev->status.ssid_length = 0;
          dev->status.channel = 0;
          dev->status.signal_dbm = 0;
        }

      dev->status.connected = false;
      status = dev->status;
      nxmutex_unlock(&dev->status_lock);
#ifdef CONFIG_NET
      if (connected && !station_ready)
        {
          sv6621_core_queue_recovery(
              dev, station_ret < 0 ? station_ret : -EIO);
          if (sv6621_core_station_worker_stop(dev, generation))
            {
              return;
            }

          continue;
        }
#endif
#ifdef CONFIG_NET
      if (connected && link_ready)
        {
          network_ret = sv6621_stats_query(
              &dev->command, context.instance, status.bssid, &link_stats);
          if (network_ret < 0)
            {
              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  event_current = generation == dev->station_generation &&
                                  dev->station_connected;
                  nxmutex_unlock(&dev->status_lock);
                }

              if (event_current)
                {
                  sv6621_core_queue_recovery(dev, network_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }
                }

              continue;
            }

          network_ret = sv6621_network_sync_multicast(&dev->network);
          if (network_ret == 0)
            {
              network_ret = sv6621_network_sync_link_addresses(
                  &dev->network, &context);
            }

          if (network_ret < 0)
            {
              if (nxmutex_lock(&dev->status_lock) >= 0)
                {
                  event_current = generation == dev->station_generation &&
                                  dev->station_connected;
                  nxmutex_unlock(&dev->status_lock);
                }

              if (event_current)
                {
                  sv6621_core_queue_recovery(dev, network_ret);
                  if (sv6621_core_station_worker_stop(dev, generation))
                    {
                      return;
                    }
                }

              continue;
            }

          if (nxmutex_lock(&dev->status_lock) < 0)
            {
              return;
            }

          event_current = generation == dev->station_generation &&
                          dev->station_connected;
          if (event_current)
            {
              dev->status.signal_dbm = link_stats.signal_dbm;
              dev->status.connected = true;
              status = dev->status;
              sv6621_network_set_link(&dev->network, true, &context);
            }

          nxmutex_unlock(&dev->status_lock);
          if (!event_current)
            {
              continue;
            }
        }
      else
        {
          sv6621_network_set_link(&dev->network, false, NULL);
        }
#endif
      if (!connected)
        {
          sv6621_wpa_cancel(&dev->wpa, -ECONNRESET);
          (void)sv6621_data_set_tx_block(
              &dev->data, SV6621_DATA_TX_BLOCK_CHANNEL, false);
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      event_current = generation == dev->station_generation &&
                      dev->station_connected == connected;
      status = dev->status;
      nxmutex_unlock(&dev->status_lock);
      if (!event_current)
        {
          continue;
        }

      if (connected)
        {
          (void)sv6621_set_signal_threshold(
              dev, SV6621_CORE_CQM_THRESHOLD_DBM,
              SV6621_CORE_CQM_HYSTERESIS_DB);
          sv6621_core_report(dev, SV6621_EVENT_CONNECTED, &status,
                             sizeof(status));
        }
      else
        {
          sv6621_core_report(dev, SV6621_EVENT_DISCONNECTED, &reason,
                             sizeof(reason));
        }

      if (nxmutex_lock(&dev->status_lock) < 0)
        {
          return;
        }

      if (generation == dev->station_generation)
        {
          dev->station_work_scheduled = false;
          nxmutex_unlock(&dev->status_lock);
          return;
        }

      nxmutex_unlock(&dev->status_lock);
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

#ifdef CONFIG_SV6621_PM
  if ((config->system_suspend.wake_flags & ~SV6621_WAKE_ALL) != 0 ||
      (!config->system_suspend.wake_enabled &&
       config->system_suspend.wake_flags != 0))
    {
      return -EINVAL;
    }
#endif

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

#ifdef CONFIG_SV6621_PM
  dev->pm_callback.prepare = sv6621_core_pm_prepare;
  ret = pm_register(&dev->pm_callback);
  if (ret < 0)
    {
      goto unsubscribe_command;
    }

  dev->pm_registered = true;
#endif

  *dev_out = dev;
  return 0;

#ifdef CONFIG_SV6621_PM
unsubscribe_command:
  sv6621_packet_unsubscribe(&dev->router, SV6621_CHANNEL_WIFI_COMMAND,
                            sv6621_command_channel_consumer, &dev->command);
#endif
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

#ifdef CONFIG_SV6621_PM
  if (dev->pm_registered)
    {
      pm_unregister(&dev->pm_callback);
      dev->pm_registered = false;
    }
#endif

  dev->config.event = NULL;
  sv6621_stop(dev);
  work_cancel_sync(LPWORK, &dev->recovery_work);
  work_cancel_sync(LPWORK, &dev->thermal_work);
  work_cancel_sync(LPWORK, &dev->security_work);
  work_cancel_sync(LPWORK, &dev->signal_work);
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
  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
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

  sv6621_data_set_pn_reuse(
      &dev->data, (dev->wifi_info.private_capabilities &
                   SV6621_WIFI_PRIVATE_PN_REUSE) != 0);

#ifdef CONFIG_NET
  if (!dev->network.registered)
    {
      ret = sv6621_network_init(&dev->network, dev, &dev->data, &dev->command,
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
  sv6621_data_reset_fragments(&dev->data);
  sv6621_data_reset_ba(&dev->data);
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

/****************************************************************************
 * Name: sv6621_scan
 ****************************************************************************/

int sv6621_scan(FAR struct sv6621_dev_s *dev)
{
  if (dev == NULL)
    {
      return -EINVAL;
    }

  return sv6621_scan_selected(dev, dev->scan_channels,
                              dev->scan_channel_count, NULL, 0);
}

/****************************************************************************
 * Name: sv6621_scan_selected
 ****************************************************************************/

int sv6621_scan_selected(
    FAR struct sv6621_dev_s *dev,
    FAR const struct sv6621_scan_channel_s *channels, size_t channel_count,
    FAR const uint8_t *ssid, size_t ssid_length)
{
  int ret;

  if (dev == NULL || channels == NULL || channel_count == 0 ||
      ssid_length > SV6621_SSID_MAX_LENGTH ||
      (ssid_length != 0 && ssid == NULL))
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

  ret = sv6621_scan_controller_begin(&dev->scan, channels, channel_count,
                                     ssid, ssid_length);

unlock_lifecycle:
  nxmutex_unlock(&dev->lifecycle_lock);
  return ret;
}

int sv6621_connect(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_connect_s *request)
{
  struct sv6621_connect_s resolved;
  FAR const struct sv6621_connect_s *connection = request;
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

  if (request->ssid_length == 0)
    {
      if (!request->bssid_valid)
        {
          return -EINVAL;
        }

      target = kmm_malloc(sizeof(*target));
      if (target == NULL)
        {
          return -ENOMEM;
        }

      ret = sv6621_scan_cache_find(&dev->scan.cache, request, target);
      if (ret < 0)
        {
          kmm_free(target);
          return ret;
        }

      if (target->bss.ssid_length == 0)
        {
          kmm_free(target);
          return -ENOENT;
        }

      resolved = *request;
      memcpy(resolved.ssid, target->bss.ssid, target->bss.ssid_length);
      resolved.ssid_length = target->bss.ssid_length;
      connection = &resolved;
      kmm_free(target);
      target = NULL;
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

  nxmutex_unlock(&dev->status_lock);
  if (ret < 0)
    {
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

  if (connection->security == SV6621_SECURITY_WPA2_PSK)
    {
      target = kmm_malloc(sizeof(*target));
      if (target == NULL)
        {
          ret = -ENOMEM;
          goto unlock_lifecycle;
        }

      ret = sv6621_scan_cache_find(&dev->scan.cache, connection, target);
      if (ret < 0)
        {
          goto free_target;
        }

      ret = sv6621_wpa_prepare(&dev->wpa, connection, dev->wifi_info.mac,
                               target->bss.bssid);
      if (ret < 0)
        {
          goto free_target;
        }

      wpa_prepared = true;
      kmm_free(target);
      target = NULL;
    }

  ret = sv6621_station_connect(&dev->station, connection,
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

  ret = nxmutex_lock(&dev->status_lock);
  if (ret < 0)
    {
      nxmutex_unlock(&dev->lifecycle_lock);
      return ret;
    }

  if (dev->status.state != SV6621_STATE_WIFI_READY)
    {
      ret = -ENETDOWN;
    }

  nxmutex_unlock(&dev->status_lock);
  if (ret >= 0)
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
 * Name: sv6621_set_signal_threshold
 ****************************************************************************/

int sv6621_set_signal_threshold(FAR struct sv6621_dev_s *dev,
                                int32_t threshold_dbm,
                                uint8_t hysteresis_db)
{
  uint8_t instance;
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

  if (dev->status.state != SV6621_STATE_WIFI_READY ||
      !dev->station_connected)
    {
      ret = -ENOTCONN;
      nxmutex_unlock(&dev->status_lock);
      goto unlock_lifecycle;
    }

  nxmutex_unlock(&dev->status_lock);
  ret = nxmutex_lock(&dev->station.lock);
  if (ret < 0)
    {
      goto unlock_lifecycle;
    }

  instance = dev->station.peer.instance;
  nxmutex_unlock(&dev->station.lock);
  ret = sv6621_signal_configure(&dev->command, instance, threshold_dbm,
                                hysteresis_db);

unlock_lifecycle:
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
  bool connected;
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
  connected = dev->station_connected;
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

  scan_active = dev->scan.active || dev->scan.stopping;
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
  if (connected)
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
