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

#include <errno.h>

#include "sv6621_wifi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_WIFI_INSTANCE               0
#define SV6621_WIFI_COMMAND_SYNC_VERSION   2
#define SV6621_WIFI_COMMAND_VERSION        1
#define SV6621_WIFI_LAST_SUPPORTED_COMMAND 61
#define SV6621_WIFI_VERSION_TABLE_SIZE     256
#define SV6621_WIFI_VERSION_RESPONSE_SIZE  512
#define SV6621_WIFI_COMMAND_TIMEOUT_MS     5000

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
