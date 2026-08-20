/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap.c
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
#include <limits.h>
#include <string.h>

#include "sv6621_ap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_COMMAND_START              19
#define SV6621_AP_COMMAND_STOP               20
#define SV6621_AP_COMMAND_TIMEOUT_MS        5000
#define SV6621_AP_FIXED_SIZE                  64
#define SV6621_AP_RESPONSE_SIZE                3
#define SV6621_AP_MAX_PAYLOAD \
  (SV6621_COMMAND_MAX_MESSAGE_SIZE - SV6621_COMMAND_HEADER_SIZE)

#define SV6621_AP_BEACON_INTERVAL_OFFSET      0
#define SV6621_AP_DTIM_OFFSET                  4
#define SV6621_AP_FLAGS_OFFSET                 5
#define SV6621_AP_CHANNEL_OFFSET               6
#define SV6621_AP_WIDTH_OFFSET                 7
#define SV6621_AP_CENTER1_OFFSET               8
#define SV6621_AP_CENTER2_OFFSET               9
#define SV6621_AP_BAND_OFFSET                 10
#define SV6621_AP_SSID_LENGTH_OFFSET          11
#define SV6621_AP_SSID_OFFSET                 12
#define SV6621_AP_BLOB_TABLE_OFFSET           44
#define SV6621_AP_BLOB_COUNT                   5

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sv6621_ap_blob_field_s
{
  FAR const struct sv6621_ap_blob_s *blob;
  size_t table_offset;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_put_le16(FAR uint8_t *value, uint16_t number);
static void sv6621_ap_put_le32(FAR uint8_t *value, uint32_t number);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_ap_put_le16(FAR uint8_t *value, uint16_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
}

static void sv6621_ap_put_le32(FAR uint8_t *value, uint32_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
  value[2] = number >> 16;
  value[3] = number >> 24;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_encode_start(FAR const struct sv6621_ap_start_s *config,
                           FAR uint8_t *payload, size_t capacity,
                           FAR size_t *written)
{
  struct sv6621_ap_blob_field_s fields[SV6621_AP_BLOB_COUNT];
  size_t total = SV6621_AP_FIXED_SIZE;
  size_t offset;
  size_t index;

  if (config == NULL || payload == NULL || written == NULL ||
      config->beacon_interval == 0 || config->beacon_interval > INT_MAX ||
      config->dtim_period == 0 ||
      config->hidden_ssid > 2 || config->channel == 0 ||
      config->channel_width > SV6621_AP_CHANNEL_WIDTH_160 ||
      config->band > SV6621_BAND_5GHZ ||
      config->ssid_length > SV6621_SSID_MAX_LENGTH)
    {
      return -EINVAL;
    }

  fields[0].blob = &config->beacon_head;
  fields[1].blob = &config->beacon_tail;
  fields[2].blob = &config->beacon_ies;
  fields[3].blob = &config->probe_response_ies;
  fields[4].blob = &config->association_response_ies;
  for (index = 0; index < SV6621_AP_BLOB_COUNT; index++)
    {
      fields[index].table_offset =
          SV6621_AP_BLOB_TABLE_OFFSET + index * 4;
      if ((fields[index].blob->length != 0 &&
           fields[index].blob->data == NULL) ||
          fields[index].blob->length > UINT16_MAX ||
          fields[index].blob->length > SV6621_AP_MAX_PAYLOAD - total)
        {
          return -EINVAL;
        }

      total += fields[index].blob->length;
    }

  if (capacity < total)
    {
      return -ENOSPC;
    }

  memset(payload, 0, total);
  sv6621_ap_put_le32(payload + SV6621_AP_BEACON_INTERVAL_OFFSET,
                     config->beacon_interval);
  payload[SV6621_AP_DTIM_OFFSET] = config->dtim_period;
  payload[SV6621_AP_FLAGS_OFFSET] = config->hidden_ssid;
  payload[SV6621_AP_CHANNEL_OFFSET] = config->channel;
  payload[SV6621_AP_WIDTH_OFFSET] = config->channel_width;
  payload[SV6621_AP_CENTER1_OFFSET] = config->center_channel1;
  payload[SV6621_AP_CENTER2_OFFSET] = config->center_channel2;
  payload[SV6621_AP_BAND_OFFSET] = config->band;
  payload[SV6621_AP_SSID_LENGTH_OFFSET] = config->ssid_length;
  memcpy(payload + SV6621_AP_SSID_OFFSET, config->ssid,
         config->ssid_length);

  offset = SV6621_AP_FIXED_SIZE;
  for (index = 0; index < SV6621_AP_BLOB_COUNT; index++)
    {
      if (fields[index].blob->length != 0)
        {
          sv6621_ap_put_le16(payload + fields[index].table_offset,
                             offset);
          sv6621_ap_put_le16(payload + fields[index].table_offset + 2,
                             fields[index].blob->length);
          memcpy(payload + offset, fields[index].blob->data,
                 fields[index].blob->length);
          offset += fields[index].blob->length;
        }
    }

  *written = offset;
  return 0;
}

int sv6621_ap_start(FAR struct sv6621_command_engine_s *command,
                    uint8_t instance,
                    FAR const struct sv6621_ap_start_s *config,
                    FAR struct sv6621_ap_context_s *context)
{
  FAR uint8_t *payload;
  uint8_t response[SV6621_AP_RESPONSE_SIZE];
  size_t payload_length;
  size_t response_length = sizeof(response);
  int ret;

  if (command == NULL || config == NULL || context == NULL)
    {
      return -EINVAL;
    }

  payload = kmm_malloc(SV6621_AP_MAX_PAYLOAD);
  if (payload == NULL)
    {
      return -ENOMEM;
    }

  ret = sv6621_ap_encode_start(config, payload, SV6621_AP_MAX_PAYLOAD,
                               &payload_length);
  if (ret == 0)
    {
      ret = sv6621_command_execute(command, instance,
                                   SV6621_AP_COMMAND_START, payload,
                                   payload_length, response,
                                   &response_length,
                                   SV6621_AP_COMMAND_TIMEOUT_MS);
    }

  kmm_free(payload);
  if (ret < 0)
    {
      return ret;
    }

  if (response_length != sizeof(response))
    {
      return -EPROTO;
    }

  context->lmac_id = response[0];
  context->instance = response[1];
  context->multicast_index = response[2];
  return 0;
}

int sv6621_ap_stop(FAR struct sv6621_command_engine_s *command,
                   uint8_t instance)
{
  if (command == NULL)
    {
      return -EINVAL;
    }

  return sv6621_command_execute(command, instance, SV6621_AP_COMMAND_STOP,
                                NULL, 0, NULL, NULL,
                                SV6621_AP_COMMAND_TIMEOUT_MS);
}

int sv6621_ap_init(FAR struct sv6621_ap_s *ap)
{
  if (ap == NULL)
    {
      return -EINVAL;
    }

  memset(ap, 0, sizeof(*ap));
  return nxmutex_init(&ap->lock);
}

void sv6621_ap_deinit(FAR struct sv6621_ap_s *ap)
{
  if (ap == NULL)
    {
      return;
    }

  if (nxmutex_lock(&ap->lock) >= 0)
    {
      DEBUGASSERT(!ap->active);
      nxmutex_unlock(&ap->lock);
    }

  nxmutex_destroy(&ap->lock);
  memset(ap, 0, sizeof(*ap));
}

int sv6621_ap_enable(FAR struct sv6621_ap_s *ap,
                     FAR struct sv6621_command_engine_s *command,
                     uint8_t instance,
                     FAR const struct sv6621_ap_start_s *config)
{
  struct sv6621_ap_context_s context;
  int ret;

  if (ap == NULL || command == NULL || config == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return -EBUSY;
    }

  ret = sv6621_ap_start(command, instance, config, &context);
  if (ret == 0)
    {
      ap->context = context;
      ap->instance = instance;
      ap->active = true;
    }

  nxmutex_unlock(&ap->lock);
  return ret;
}

int sv6621_ap_disable(FAR struct sv6621_ap_s *ap,
                      FAR struct sv6621_command_engine_s *command)
{
  int ret;

  if (ap == NULL || command == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ap->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!ap->active)
    {
      nxmutex_unlock(&ap->lock);
      return 0;
    }

  ret = sv6621_ap_stop(command, ap->instance);
  if (ret == 0)
    {
      memset(&ap->context, 0, sizeof(ap->context));
      ap->instance = 0;
      ap->active = false;
    }

  nxmutex_unlock(&ap->lock);
  return ret;
}

bool sv6621_ap_is_active(FAR struct sv6621_ap_s *ap)
{
  bool active = false;

  if (ap != NULL && nxmutex_lock(&ap->lock) >= 0)
    {
      active = ap->active;
      nxmutex_unlock(&ap->lock);
    }

  return active;
}
