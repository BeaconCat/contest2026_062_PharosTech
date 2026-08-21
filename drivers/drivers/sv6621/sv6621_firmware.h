/****************************************************************************
 * drivers/drivers/sv6621/sv6621_firmware.h
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

#ifndef __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_FIRMWARE_H
#define __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_FIRMWARE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct sv6621_transport_s;

struct sv6621_firmware_layout_s
{
  uint32_t iram_address;
  uint32_t dram_address;
  size_t header_offset;
  size_t table_end_offset;
  size_t nv_offset;
  size_t nv_capacity;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int sv6621_firmware_parse_iram(FAR const uint8_t *image, size_t length,
                               FAR struct sv6621_firmware_layout_s *layout);
int sv6621_firmware_verify_device(FAR struct sv6621_transport_s *transport);
int sv6621_firmware_download(FAR struct sv6621_transport_s *transport,
                             FAR const uint8_t *iram, size_t iram_length,
                             FAR const uint8_t *dram, size_t dram_length,
                             FAR const uint8_t *nvram, size_t nvram_length);
int sv6621_firmware_prepare_iram(FAR const uint8_t *image, size_t image_length,
                                 FAR const uint8_t *nvram, size_t nvram_length,
                                 FAR uint8_t **prepared_image);
void sv6621_firmware_release(FAR uint8_t *prepared_image);

#endif /* __DRIVERS_WIRELESS_SEEKWAVE_SV6621_SV6621_FIRMWARE_H */
