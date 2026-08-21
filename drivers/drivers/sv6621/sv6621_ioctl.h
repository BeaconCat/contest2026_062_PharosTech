/****************************************************************************
 * drivers/drivers/sv6621/sv6621_ioctl.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_IOCTL_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_IOCTL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include "sv6621.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_dev_s;

struct sv6621_ioctl_s
{
  mutex_t lock;
  FAR struct sv6621_dev_s *owner;
  struct sv6621_connect_s connection;
  struct sv6621_ap_config_s access_point;
  uint8_t mode;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_ioctl_init(FAR struct sv6621_ioctl_s *ioctl,
                      FAR struct sv6621_dev_s *owner);
void sv6621_ioctl_deinit(FAR struct sv6621_ioctl_s *ioctl);
int sv6621_ioctl_handle(FAR struct sv6621_ioctl_s *ioctl, int command,
                        unsigned long argument);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_IOCTL_H */
