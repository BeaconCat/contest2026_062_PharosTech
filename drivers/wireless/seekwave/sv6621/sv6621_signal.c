/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_signal.c
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

#include "sv6621_signal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_SIGNAL_COMMAND_SET_CQM     34
#define SV6621_SIGNAL_COMMAND_TIMEOUT_MS  2000
#define SV6621_SIGNAL_PAYLOAD_SIZE        5

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_signal_configure
 ****************************************************************************/

int sv6621_signal_configure(FAR struct sv6621_command_engine_s *command,
                            uint8_t instance, int32_t threshold_dbm,
                            uint8_t hysteresis_db)
{
  uint32_t threshold;
  uint8_t payload[SV6621_SIGNAL_PAYLOAD_SIZE];

  if (command == NULL || instance > 0x0f || threshold_dbm < INT8_MIN ||
      threshold_dbm > 0)
    {
      return -EINVAL;
    }

  threshold = (uint32_t)threshold_dbm;
  payload[0] = threshold & 0xff;
  payload[1] = (threshold >> 8) & 0xff;
  payload[2] = (threshold >> 16) & 0xff;
  payload[3] = threshold >> 24;
  payload[4] = hysteresis_db;
  return sv6621_command_execute(command, instance,
                                SV6621_SIGNAL_COMMAND_SET_CQM, payload,
                                sizeof(payload), NULL, NULL,
                                SV6621_SIGNAL_COMMAND_TIMEOUT_MS);
}
