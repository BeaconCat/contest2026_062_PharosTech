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

#include <nuttx/kmalloc.h>

#include <errno.h>
#include <string.h>

#include "sv6621_firmware.h"
#include "sv6621_memory.h"
#include "sv6621_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_FIRMWARE_METADATA_SIZE  0x200
#define SV6621_FIRMWARE_ALIGNMENT      4
#define SV6621_FIRMWARE_HEAD_SIZE      16
#define SV6621_FIRMWARE_MARKER_SIZE    8
#define SV6621_FIRMWARE_NV_MARK_SIZE   4
#define SV6621_FIRMWARE_NV_HEADER_SIZE 0x20
#define SV6621_FIRMWARE_NV_OFFSET      0x08
#define SV6621_FIRMWARE_NV_LENGTH      0x0c
#define SV6621_FIRMWARE_IDENTITY_SIZE  16

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_firmware_head[SV6621_FIRMWARE_MARKER_SIZE] = {
  'k', 'e', 'e', 's', '0', '6', '1', '6'
};

static const uint8_t g_sv6621_firmware_tail[SV6621_FIRMWARE_MARKER_SIZE] = {
  'e', 'v', 'a', 'w', '0', '6', '1', '6'
};

static const uint8_t
    g_sv6621_firmware_nv_start[SV6621_FIRMWARE_NV_MARK_SIZE] = { 'T', 'S', 'V',
                                                                 'N' };

static const uint8_t g_sv6621_firmware_nv_end[SV6621_FIRMWARE_NV_MARK_SIZE] = {
  'D', 'E', 'V', 'N'
};

static const uint8_t g_sv6621_firmware_identity[] = {
  'S', 'V', '6', '1', '6', '0', 'L', 'I', 'T', 'E'
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

int sv6621_firmware_verify_device(FAR struct sv6621_transport_s *transport)
{
  uint8_t identity[SV6621_FIRMWARE_IDENTITY_SIZE];
  int ret;

  ret = sv6621_memory_read(transport, SV6621_CP_CHIP_ID_ADDRESS, identity,
                           sizeof(identity));
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(identity, g_sv6621_firmware_identity,
             sizeof(g_sv6621_firmware_identity)) != 0)
    {
      return -ENODEV;
    }

  return 0;
}

int sv6621_firmware_download(FAR struct sv6621_transport_s *transport,
                             FAR const uint8_t *iram, size_t iram_length,
                             FAR const uint8_t *dram, size_t dram_length,
                             FAR const uint8_t *nvram, size_t nvram_length)
{
  struct sv6621_firmware_layout_s layout;
  FAR uint8_t *prepared_iram = NULL;
  int ret;

  if (transport == NULL || iram == NULL || iram_length == 0 || dram == NULL ||
      dram_length == 0 || nvram == NULL || nvram_length == 0)
    {
      return -EINVAL;
    }

  ret = sv6621_firmware_verify_device(transport);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_firmware_parse_iram(iram, iram_length, &layout);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_firmware_prepare_iram(iram, iram_length, nvram, nvram_length,
                                     &prepared_iram);
  if (ret < 0)
    {
      return ret;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_DMA_TYPE, 0x01);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_SLEEP_CONTROL, 0x01);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_RX_FLOW_LOW, 0x00);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_RX_FLOW_HIGH, 0x00);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = sv6621_memory_write(transport, layout.dram_address, dram, dram_length);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = sv6621_memory_write(transport, layout.iram_address, prepared_iram,
                            iram_length);
  if (ret < 0)
    {
      goto release_image;
    }

  ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                   SV6621_SDIO_DOWNLOAD_STATUS, 0x01);

release_image:
  sv6621_firmware_release(prepared_iram);
  return ret;
}

int sv6621_firmware_prepare_iram(FAR const uint8_t *image, size_t image_length,
                                 FAR const uint8_t *nvram, size_t nvram_length,
                                 FAR uint8_t **prepared_image)
{
  struct sv6621_firmware_layout_s layout;
  FAR uint8_t *copy;
  uint32_t source_offset;
  uint32_t source_length;
  int ret;

  if (image == NULL || nvram == NULL || prepared_image == NULL ||
      nvram_length < SV6621_FIRMWARE_NV_HEADER_SIZE)
    {
      return -EINVAL;
    }

  *prepared_image = NULL;
  ret = sv6621_firmware_parse_iram(image, image_length, &layout);
  if (ret < 0)
    {
      return ret;
    }

  if (layout.nv_offset == SIZE_MAX || layout.nv_capacity == 0)
    {
      return -ENOENT;
    }

  source_offset = sv6621_firmware_get_le32(nvram + SV6621_FIRMWARE_NV_OFFSET);
  source_length = sv6621_firmware_get_le32(nvram + SV6621_FIRMWARE_NV_LENGTH);
  if (source_offset < SV6621_FIRMWARE_NV_HEADER_SIZE || source_length == 0 ||
      source_offset > nvram_length ||
      source_length > nvram_length - source_offset)
    {
      return -EPROTO;
    }

  if (source_length > layout.nv_capacity || layout.nv_offset > image_length ||
      layout.nv_capacity > image_length - layout.nv_offset)
    {
      return -E2BIG;
    }

  copy = kmm_malloc(image_length);
  if (copy == NULL)
    {
      return -ENOMEM;
    }

  memcpy(copy, image, image_length);
  memset(copy + layout.nv_offset, 0, layout.nv_capacity);
  memcpy(copy + layout.nv_offset, nvram + source_offset, source_length);
  *prepared_image = copy;
  return 0;
}

void sv6621_firmware_release(FAR uint8_t *prepared_image)
{
  kmm_free(prepared_image);
}
