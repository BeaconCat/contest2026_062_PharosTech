/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_transport.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TRANSPORT_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TRANSPORT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_transport_s;

typedef void (*sv6621_transport_irq_t)(FAR void *arg);

struct sv6621_transport_ops_s
{
  int (*open)(FAR struct sv6621_transport_s *transport);
  void (*close)(FAR struct sv6621_transport_s *transport);
  int (*read_byte)(FAR struct sv6621_transport_s *transport, uint8_t function,
                   uint32_t address, FAR uint8_t *value);
  int (*write_byte)(FAR struct sv6621_transport_s *transport, uint8_t function,
                    uint32_t address, uint8_t value);
  int (*read)(FAR struct sv6621_transport_s *transport, uint8_t function,
              uint32_t address, bool increment, FAR void *buffer,
              size_t length);
  int (*write)(FAR struct sv6621_transport_s *transport, uint8_t function,
               uint32_t address, bool increment, FAR const void *buffer,
               size_t length);
  int (*attach_irq)(FAR struct sv6621_transport_s *transport,
                    sv6621_transport_irq_t handler, FAR void *arg);
  int (*enable_irq)(FAR struct sv6621_transport_s *transport, bool enable);
  int (*recover)(FAR struct sv6621_transport_s *transport);
};

struct sv6621_transport_s
{
  FAR const struct sv6621_transport_ops_s *ops;
  FAR void *priv;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_transport_validate(FAR const struct sv6621_transport_s *transport);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_TRANSPORT_H */
