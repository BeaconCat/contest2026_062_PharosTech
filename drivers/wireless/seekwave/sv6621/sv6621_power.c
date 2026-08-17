/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_power.c
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

#include "sv6621_power.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_POWER_INSTANCE          0
#define SV6621_POWER_COMMAND_RESUME    27
#define SV6621_POWER_COMMAND_SUSPEND   28
#define SV6621_POWER_SUSPEND_SIZE      4
#define SV6621_POWER_COMMAND_TIMEOUT_MS 2000

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_power_suspend
 ****************************************************************************/

int sv6621_power_suspend(FAR struct sv6621_command_engine_s *command,
                         bool wake_enabled, uint16_t wake_flags)
{
  uint8_t payload[SV6621_POWER_SUSPEND_SIZE];

  if (command == NULL || (!wake_enabled && wake_flags != 0))
    {
      return -EINVAL;
    }

  payload[0] = wake_enabled ? 1 : 0;
  payload[1] = 0;
  payload[2] = wake_flags & 0xff;
  payload[3] = wake_flags >> 8;
  return sv6621_command_send_noack(command, SV6621_POWER_INSTANCE,
                                   SV6621_POWER_COMMAND_SUSPEND, payload,
                                   sizeof(payload));
}

/****************************************************************************
 * Name: sv6621_power_resume
 ****************************************************************************/

int sv6621_power_resume(FAR struct sv6621_command_engine_s *command)
{
  if (command == NULL)
    {
      return -EINVAL;
    }

  return sv6621_command_execute(command, SV6621_POWER_INSTANCE,
                                SV6621_POWER_COMMAND_RESUME, NULL, 0, NULL,
                                NULL, SV6621_POWER_COMMAND_TIMEOUT_MS);
}
