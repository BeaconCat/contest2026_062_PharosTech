/****************************************************************************
 * drivers/wireless/seekwave/sv6621/include/sv6621.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_MAC_LENGTH      6
#define SV6621_SSID_MAX_LENGTH 32
#define SV6621_KEY_MAX_LENGTH  64

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s;
struct sv6621_transport_s;

enum sv6621_state_e
{
  SV6621_STATE_OFF = 0,
  SV6621_STATE_STARTING,
  SV6621_STATE_BSP_READY,
  SV6621_STATE_WIFI_STARTING,
  SV6621_STATE_WIFI_READY,
  SV6621_STATE_RECOVERING,
  SV6621_STATE_STOPPING,
  SV6621_STATE_FAILED
};

enum sv6621_security_e
{
  SV6621_SECURITY_OPEN = 0,
  SV6621_SECURITY_WPA2_PSK,
  SV6621_SECURITY_WPA3_SAE,
  SV6621_SECURITY_WPA2_WPA3_PSK
};

enum sv6621_band_e
{
  SV6621_BAND_2GHZ = 0,
  SV6621_BAND_5GHZ
};

enum sv6621_event_e
{
  SV6621_EVENT_STATE_CHANGED = 0,
  SV6621_EVENT_SCAN_RESULT,
  SV6621_EVENT_SCAN_COMPLETE,
  SV6621_EVENT_CONNECTED,
  SV6621_EVENT_DISCONNECTED,
  SV6621_EVENT_RECOVERY_STARTED,
  SV6621_EVENT_RECOVERY_COMPLETE,
  SV6621_EVENT_FATAL
};

struct sv6621_firmware_s
{
  FAR const uint8_t *data;
  size_t length;
};

struct sv6621_bss_s
{
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t channel;
  enum sv6621_band_e band;
  enum sv6621_security_e security;
  int16_t signal_dbm;
};

struct sv6621_connect_s
{
  uint8_t ssid[SV6621_SSID_MAX_LENGTH];
  uint8_t ssid_length;
  uint8_t bssid[SV6621_MAC_LENGTH];
  bool bssid_valid;
  enum sv6621_security_e security;
  uint8_t credential[SV6621_KEY_MAX_LENGTH];
  uint8_t credential_length;
};

struct sv6621_status_s
{
  enum sv6621_state_e state;
  uint8_t mac[SV6621_MAC_LENGTH];
  uint8_t bssid[SV6621_MAC_LENGTH];
  uint8_t channel;
  enum sv6621_band_e band;
  int16_t signal_dbm;
  uint32_t recovery_count;
  int last_error;
};

typedef void (*sv6621_event_t)(FAR struct sv6621_dev_s *dev,
                               enum sv6621_event_e event, FAR const void *data,
                               size_t length, FAR void *arg);

struct sv6621_board_ops_s
{
  int (*power_on)(FAR void *arg);
  void (*power_off)(FAR void *arg);
  int (*load_address)(FAR void *arg, uint8_t address[SV6621_MAC_LENGTH]);
  int (*store_address)(FAR void *arg,
                       FAR const uint8_t address[SV6621_MAC_LENGTH]);
};

struct sv6621_config_s
{
  FAR struct sv6621_transport_s *transport;
  FAR const struct sv6621_board_ops_s *board_ops;
  FAR void *board_arg;
  struct sv6621_firmware_s iram;
  struct sv6621_firmware_s dram;
  struct sv6621_firmware_s nvram;
  struct sv6621_firmware_s calibration;
  sv6621_event_t event;
  FAR void *event_arg;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
extern "C" {
#endif

int sv6621_create(FAR const struct sv6621_config_s *config,
                  FAR struct sv6621_dev_s **dev);
void sv6621_destroy(FAR struct sv6621_dev_s *dev);
int sv6621_start(FAR struct sv6621_dev_s *dev);
int sv6621_stop(FAR struct sv6621_dev_s *dev);
int sv6621_get_status(FAR struct sv6621_dev_s *dev,
                      FAR struct sv6621_status_s *status);
int sv6621_scan(FAR struct sv6621_dev_s *dev);
int sv6621_connect(FAR struct sv6621_dev_s *dev,
                   FAR const struct sv6621_connect_s *request);
int sv6621_disconnect(FAR struct sv6621_dev_s *dev, uint16_t reason);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_INCLUDE_SV6621_H */
