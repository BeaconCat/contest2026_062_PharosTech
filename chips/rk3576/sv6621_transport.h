/****************************************************************************
 * chips/rk3576/sv6621_transport.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_TRANSPORT_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_TRANSPORT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_transport_s;

typedef void (*sv6621_power_t)(FAR void *arg, bool on);

struct sv6621_transport_ops_s
{
  int (*initialize)(FAR struct sv6621_transport_s *transport,
                    sv6621_power_t power, FAR void *power_arg);
  int (*readb)(FAR struct sv6621_transport_s *transport, uint8_t function,
               uint32_t address, FAR uint8_t *value);
  int (*writeb)(FAR struct sv6621_transport_s *transport, uint8_t function,
                uint32_t address, uint8_t value);
  int (*read)(FAR struct sv6621_transport_s *transport, uint8_t function,
              uint32_t address, bool increment, FAR uint8_t *buffer,
              int length);
  int (*write)(FAR struct sv6621_transport_s *transport, uint8_t function,
               uint32_t address, bool increment, FAR const uint8_t *buffer,
               int length);
  void (*ack_interrupt)(FAR struct sv6621_transport_s *transport);
};

struct sv6621_transport_s
{
  FAR const struct sv6621_transport_ops_s *ops;
  FAR void *priv;
};

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_TRANSPORT_H */
