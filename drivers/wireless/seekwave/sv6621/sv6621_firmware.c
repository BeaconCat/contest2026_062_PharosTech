/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_firmware.c
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
#include <string.h>

#include "sv6621_firmware.h"
#include "sv6621_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_FIRMWARE_METADATA_SIZE 0x200
#define SV6621_FIRMWARE_ALIGNMENT     4
#define SV6621_FIRMWARE_HEAD_SIZE     16
#define SV6621_FIRMWARE_MARKER_SIZE   8
#define SV6621_FIRMWARE_NV_MARK_SIZE  4

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_firmware_head[SV6621_FIRMWARE_MARKER_SIZE] =
{
  'k', 'e', 'e', 's', '0', '6', '1', '6'
};

static const uint8_t g_sv6621_firmware_tail[SV6621_FIRMWARE_MARKER_SIZE] =
{
  'e', 'v', 'a', 'w', '0', '6', '1', '6'
};

static const uint8_t g_sv6621_firmware_nv_start
    [SV6621_FIRMWARE_NV_MARK_SIZE] =
{
  'T', 'S', 'V', 'N'
};

static const uint8_t g_sv6621_firmware_nv_end[SV6621_FIRMWARE_NV_MARK_SIZE] =
{
  'D', 'E', 'V', 'N'
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t sv6621_firmware_get_le32(FAR const uint8_t *data);
static size_t sv6621_firmware_find(FAR const uint8_t *image, size_t limit,
                                   FAR const uint8_t *marker,
                                   size_t marker_length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_firmware_get_le32
 ****************************************************************************/

static uint32_t sv6621_firmware_get_le32(FAR const uint8_t *data)
{
  return data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
         ((uint32_t)data[3] << 24);
}

/****************************************************************************
 * Name: sv6621_firmware_find
 ****************************************************************************/

static size_t sv6621_firmware_find(FAR const uint8_t *image, size_t limit,
                                   FAR const uint8_t *marker,
                                   size_t marker_length)
{
  size_t offset;

  for (offset = 0; offset + marker_length <= limit;
       offset += SV6621_FIRMWARE_ALIGNMENT)
    {
      if (memcmp(image + offset, marker, marker_length) == 0)
        {
          return offset;
        }
    }

  return SIZE_MAX;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_firmware_parse_iram(FAR const uint8_t *image, size_t length,
                               FAR struct sv6621_firmware_layout_s *layout)
{
  size_t metadata_length;
  size_t header_offset;
  size_t tail_offset;
  size_t nv_start_offset;
  size_t nv_end_offset;

  if (image == NULL || layout == NULL || length < SV6621_FIRMWARE_HEAD_SIZE)
    {
      return -EINVAL;
    }

  metadata_length = length;
  if (metadata_length > SV6621_FIRMWARE_METADATA_SIZE)
    {
      metadata_length = SV6621_FIRMWARE_METADATA_SIZE;
    }

  header_offset =
      sv6621_firmware_find(image, metadata_length, g_sv6621_firmware_head,
                           sizeof(g_sv6621_firmware_head));
  tail_offset =
      sv6621_firmware_find(image, metadata_length, g_sv6621_firmware_tail,
                           sizeof(g_sv6621_firmware_tail));
  if (header_offset == SIZE_MAX || tail_offset == SIZE_MAX ||
      header_offset + SV6621_FIRMWARE_HEAD_SIZE > metadata_length ||
      tail_offset <= header_offset + SV6621_FIRMWARE_HEAD_SIZE)
    {
      return -EPROTO;
    }

  memset(layout, 0, sizeof(*layout));
  layout->iram_address = sv6621_firmware_get_le32(image + header_offset +
                                                  SV6621_FIRMWARE_MARKER_SIZE);
  layout->dram_address = sv6621_firmware_get_le32(
      image + header_offset + SV6621_FIRMWARE_MARKER_SIZE + sizeof(uint32_t));
  if (layout->iram_address != SV6621_CP_IRAM_ADDRESS ||
      layout->dram_address != SV6621_CP_DRAM_ADDRESS)
    {
      return -EPROTO;
    }

  layout->header_offset = header_offset;
  layout->table_end_offset = tail_offset;
  layout->nv_offset = SIZE_MAX;

  nv_start_offset =
      sv6621_firmware_find(image, header_offset, g_sv6621_firmware_nv_start,
                           sizeof(g_sv6621_firmware_nv_start));
  nv_end_offset =
      sv6621_firmware_find(image, header_offset, g_sv6621_firmware_nv_end,
                           sizeof(g_sv6621_firmware_nv_end));
  if (nv_start_offset != SIZE_MAX && nv_end_offset != SIZE_MAX &&
      nv_end_offset >= nv_start_offset + SV6621_FIRMWARE_NV_MARK_SIZE)
    {
      layout->nv_offset = nv_start_offset + SV6621_FIRMWARE_NV_MARK_SIZE;
      layout->nv_capacity = nv_end_offset - layout->nv_offset;
    }

  return 0;
}
