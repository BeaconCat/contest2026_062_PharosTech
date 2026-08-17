/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_wifi.c
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

#include <sys/random.h>

#include <errno.h>
#include <string.h>
#include <time.h>

#include "sv6621_wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WIFI_INSTANCE                0
#define SV6621_WIFI_COMMAND_GET_INFO        1
#define SV6621_WIFI_COMMAND_SYNC_VERSION    2
#define SV6621_WIFI_COMMAND_PHY_BB_CONFIG   50
#define SV6621_WIFI_COMMAND_VERSION         1
#define SV6621_WIFI_LAST_SUPPORTED_COMMAND  61
#define SV6621_WIFI_VERSION_TABLE_SIZE      256
#define SV6621_WIFI_VERSION_RESPONSE_SIZE   512
#define SV6621_WIFI_COMMAND_TIMEOUT_MS      5000

#define SV6621_WIFI_CHIP_INFO_SIZE          234
#define SV6621_WIFI_CHIP_INFO_MIN_SIZE      70
#define SV6621_WIFI_INFO_ENCRYPTION_OFFSET  0
#define SV6621_WIFI_INFO_MODEL_OFFSET       2
#define SV6621_WIFI_INFO_VERSION_OFFSET     6
#define SV6621_WIFI_INFO_FIRMWARE_OFFSET    10
#define SV6621_WIFI_INFO_CAPABILITY_OFFSET  14
#define SV6621_WIFI_INFO_MAX_STA_OFFSET     18
#define SV6621_WIFI_INFO_MAX_MC_OFFSET      19
#define SV6621_WIFI_INFO_MAX_SCAN_OFFSET    42
#define SV6621_WIFI_INFO_MAC_OFFSET         64
#define SV6621_WIFI_INFO_BANDWIDTH_OFFSET   86
#define SV6621_WIFI_INFO_PRIVATE_OFFSET     90

#define SV6621_WIFI_CALIBRATION_HEADER_SIZE 4
#define SV6621_WIFI_CALIBRATION_CHUNK_SIZE  512
#define SV6621_WIFI_CALIBRATION_MAX_SIZE    (256 * 512)

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint16_t sv6621_wifi_get_le16(FAR const uint8_t *value);
static uint32_t sv6621_wifi_get_le32(FAR const uint8_t *value);
static void sv6621_wifi_put_le64(uint8_t value[8], uint64_t number);
static bool
sv6621_wifi_address_valid(FAR const uint8_t address[SV6621_MAC_LENGTH]);
static int sv6621_wifi_select_address(
    FAR const struct sv6621_board_ops_s *board_ops, FAR void *board_arg,
    FAR const uint8_t firmware_address[SV6621_MAC_LENGTH],
    uint8_t selected[SV6621_MAC_LENGTH]);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t sv6621_wifi_get_le16(FAR const uint8_t *value)
{
  return value[0] | ((uint16_t)value[1] << 8);
}

static uint32_t sv6621_wifi_get_le32(FAR const uint8_t *value)
{
  return value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) |
         ((uint32_t)value[3] << 24);
}

static void sv6621_wifi_put_le64(uint8_t value[8], uint64_t number)
{
  unsigned int index;

  for (index = 0; index < 8; index++)
    {
      value[index] = number >> (index * 8);
    }
}

static bool
sv6621_wifi_address_valid(FAR const uint8_t address[SV6621_MAC_LENGTH])
{
  bool all_zero = true;
  bool all_ones = true;
  unsigned int index;

  if ((address[0] & 1) != 0)
    {
      return false;
    }

  for (index = 0; index < SV6621_MAC_LENGTH; index++)
    {
      all_zero = all_zero && address[index] == 0;
      all_ones = all_ones && address[index] == 0xff;
    }

  return !all_zero && !all_ones;
}

static int sv6621_wifi_select_address(
    FAR const struct sv6621_board_ops_s *board_ops, FAR void *board_arg,
    FAR const uint8_t firmware_address[SV6621_MAC_LENGTH],
    uint8_t selected[SV6621_MAC_LENGTH])
{
  ssize_t random_length;
  int ret;

  if (sv6621_wifi_address_valid(firmware_address))
    {
      memcpy(selected, firmware_address, SV6621_MAC_LENGTH);
      return 0;
    }

  ret = board_ops->load_address(board_arg, selected);
  if (ret >= 0 && sv6621_wifi_address_valid(selected))
    {
      return 0;
    }

  selected[0] = 0xfe;
  selected[1] = 0xfd;
  selected[2] = 0xfc;
  random_length = getrandom(selected + 3, 3, 0);
  if (random_length < 0)
    {
      return -errno;
    }

  if (random_length != 3)
    {
      return -EIO;
    }

  ret = board_ops->store_address(board_arg, selected);
  return ret < 0 ? ret : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_wifi_sync_versions(FAR struct sv6621_command_engine_s *command)
{
  FAR uint8_t *versions;
  size_t response_length = SV6621_WIFI_VERSION_RESPONSE_SIZE;
  unsigned int id;
  int ret;

  if (command == NULL)
    {
      return -EINVAL;
    }

  versions = kmm_malloc(SV6621_WIFI_VERSION_RESPONSE_SIZE);
  if (versions == NULL)
    {
      return -ENOMEM;
    }

  ret = sv6621_command_execute(
      command, SV6621_WIFI_INSTANCE, SV6621_WIFI_COMMAND_SYNC_VERSION, NULL, 0,
      versions, &response_length, SV6621_WIFI_COMMAND_TIMEOUT_MS);
  if (ret < 0)
    {
      goto free_versions;
    }

  if (response_length != SV6621_WIFI_VERSION_RESPONSE_SIZE)
    {
      ret = -EPROTO;
      goto free_versions;
    }

  for (id = 0; id <= SV6621_WIFI_LAST_SUPPORTED_COMMAND; id++)
    {
      if (versions[id] != 0 && versions[id] != SV6621_WIFI_COMMAND_VERSION)
        {
          ret = -EPROTONOSUPPORT;
          goto free_versions;
        }
    }

  ret = 0;

free_versions:
  kmm_free(versions);
  return ret;
}

int sv6621_wifi_get_info(FAR struct sv6621_command_engine_s *command,
                         FAR const struct sv6621_board_ops_s *board_ops,
                         FAR void *board_arg,
                         FAR struct sv6621_wifi_info_s *info)
{
  struct timespec now;
  uint8_t request[8];
  uint8_t response[SV6621_WIFI_CHIP_INFO_SIZE];
  uint64_t timestamp_ms;
  size_t response_length = sizeof(response);
  int ret;

  if (command == NULL || board_ops == NULL ||
      board_ops->load_address == NULL || board_ops->store_address == NULL ||
      info == NULL)
    {
      return -EINVAL;
    }

  ret = clock_gettime(CLOCK_MONOTONIC, &now);
  if (ret < 0)
    {
      return -errno;
    }

  timestamp_ms = (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
  sv6621_wifi_put_le64(request, timestamp_ms);
  ret = sv6621_command_execute(command, SV6621_WIFI_INSTANCE,
                               SV6621_WIFI_COMMAND_GET_INFO, request,
                               sizeof(request), response, &response_length,
                               SV6621_WIFI_COMMAND_TIMEOUT_MS);
  if (ret < 0)
    {
      return ret;
    }

  if (response_length < SV6621_WIFI_CHIP_INFO_MIN_SIZE)
    {
      return -EPROTO;
    }

  memset(info, 0, sizeof(*info));
  info->encryption_capabilities =
      sv6621_wifi_get_le16(response + SV6621_WIFI_INFO_ENCRYPTION_OFFSET);
  info->chip_model =
      sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_MODEL_OFFSET);
  info->chip_version =
      sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_VERSION_OFFSET);
  info->firmware_version =
      sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_FIRMWARE_OFFSET);
  info->firmware_capabilities =
      sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_CAPABILITY_OFFSET);
  info->max_stations = response[SV6621_WIFI_INFO_MAX_STA_OFFSET];
  info->max_multicast_addresses = response[SV6621_WIFI_INFO_MAX_MC_OFFSET];
  info->max_scan_ssids = response[SV6621_WIFI_INFO_MAX_SCAN_OFFSET];
  if (response_length >= SV6621_WIFI_INFO_PRIVATE_OFFSET + 4)
    {
      info->bandwidth_capabilities =
          sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_BANDWIDTH_OFFSET);
      info->private_capabilities =
          sv6621_wifi_get_le32(response + SV6621_WIFI_INFO_PRIVATE_OFFSET);
    }

  return sv6621_wifi_select_address(
      board_ops, board_arg, response + SV6621_WIFI_INFO_MAC_OFFSET, info->mac);
}

int sv6621_wifi_download_calibration(
    FAR struct sv6621_command_engine_s *command, FAR const uint8_t *data,
    size_t length)
{
  uint8_t payload[SV6621_WIFI_CALIBRATION_HEADER_SIZE +
                  SV6621_WIFI_CALIBRATION_CHUNK_SIZE];
  size_t offset = 0;
  size_t chunk_length;
  uint8_t sequence = 0;
  int ret;

  if (command == NULL || data == NULL || length == 0)
    {
      return -EINVAL;
    }

  if (length > SV6621_WIFI_CALIBRATION_MAX_SIZE)
    {
      return -EFBIG;
    }

  while (offset < length)
    {
      chunk_length = length - offset;
      if (chunk_length > SV6621_WIFI_CALIBRATION_CHUNK_SIZE)
        {
          chunk_length = SV6621_WIFI_CALIBRATION_CHUNK_SIZE;
        }

      payload[0] = sequence;
      payload[1] = offset + chunk_length == length;
      payload[2] = chunk_length & 0xff;
      payload[3] = chunk_length >> 8;
      memcpy(payload + SV6621_WIFI_CALIBRATION_HEADER_SIZE, data + offset,
             chunk_length);

      ret = sv6621_command_execute(
          command, SV6621_WIFI_INSTANCE, SV6621_WIFI_COMMAND_PHY_BB_CONFIG,
          payload, SV6621_WIFI_CALIBRATION_HEADER_SIZE + chunk_length, NULL,
          NULL, SV6621_WIFI_COMMAND_TIMEOUT_MS);
      if (ret != 0)
        {
          return ret < 0 ? ret : -EREMOTEIO;
        }

      offset += chunk_length;
      sequence++;
    }

  return 0;
}
