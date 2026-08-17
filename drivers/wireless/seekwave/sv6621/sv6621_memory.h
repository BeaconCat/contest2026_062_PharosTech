/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_memory.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MEMORY_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MEMORY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "sv6621_transport.h"

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_memory_latch(FAR struct sv6621_transport_s *transport,
                        uint32_t address);
int sv6621_memory_read(FAR struct sv6621_transport_s *transport,
                       uint32_t address, FAR void *buffer, size_t length);
int sv6621_memory_write(FAR struct sv6621_transport_s *transport,
                        uint32_t address, FAR const void *buffer,
                        size_t length);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_MEMORY_H */
