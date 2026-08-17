/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_memory.c
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

#include <nuttx/arch.h>

#include <errno.h>

#include "sv6621_memory.h"
#include "sv6621_protocol.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_MEMORY_TRANSFER_SIZE  512
#define SV6621_MEMORY_RETRY_COUNT    4
#define SV6621_MEMORY_RETRY_DELAY_MS 2
#define SV6621_SDIO_CCCR_ABORT       0x006

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_memory_transfer(FAR struct sv6621_transport_s *transport,
                                  uint32_t address, FAR void *read_buffer,
                                  FAR const void *write_buffer, size_t length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_memory_transfer
 ****************************************************************************/

static int sv6621_memory_transfer(FAR struct sv6621_transport_s *transport,
                                  uint32_t address, FAR void *read_buffer,
                                  FAR const void *write_buffer, size_t length)
{
  FAR uint8_t *read_bytes = read_buffer;
  FAR const uint8_t *write_bytes = write_buffer;
  size_t offset;
  int ret;

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  if ((read_buffer == NULL) == (write_buffer == NULL) || length == 0 ||
      length > UINT32_MAX || address > UINT32_MAX - (uint32_t)(length - 1))
    {
      return -EINVAL;
    }

  ret = sv6621_memory_latch(transport, address);
  if (ret < 0)
    {
      return ret;
    }

  for (offset = 0; offset < length;)
    {
      size_t chunk = length - offset;
      unsigned int attempt;

      if (chunk > SV6621_MEMORY_TRANSFER_SIZE)
        {
          chunk = SV6621_MEMORY_TRANSFER_SIZE;
        }

      for (attempt = 0;; attempt++)
        {
          if (write_buffer != NULL)
            {
              ret = transport->ops->write(transport, SV6621_SDIO_FUNCTION_DATA,
                                          SV6621_SDIO_DT_WINDOW, true,
                                          write_bytes + offset, chunk);
            }
          else
            {
              ret = transport->ops->read(transport, SV6621_SDIO_FUNCTION_DATA,
                                         SV6621_SDIO_DT_WINDOW, true,
                                         read_bytes + offset, chunk);
            }

          if (ret >= 0 || attempt >= SV6621_MEMORY_RETRY_COUNT)
            {
              break;
            }

          transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                     SV6621_SDIO_CCCR_ABORT, 0x01);
          up_mdelay(SV6621_MEMORY_RETRY_DELAY_MS);
          ret = sv6621_memory_latch(transport, address + offset);
          if (ret < 0)
            {
              break;
            }
        }

      if (ret < 0)
        {
          return ret;
        }

      offset += chunk;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_memory_latch(FAR struct sv6621_transport_s *transport,
                        uint32_t address)
{
  unsigned int byte;
  int ret;

  ret = sv6621_transport_validate(transport);
  if (ret < 0)
    {
      return ret;
    }

  for (byte = 0; byte < sizeof(address); byte++)
    {
      ret = transport->ops->write_byte(transport, SV6621_SDIO_FUNCTION_CONTROL,
                                       SV6621_SDIO_DT_ADDRESS + byte,
                                       (address >> (byte * 8)) & 0xff);
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}

int sv6621_memory_read(FAR struct sv6621_transport_s *transport,
                       uint32_t address, FAR void *buffer, size_t length)
{
  return sv6621_memory_transfer(transport, address, buffer, NULL, length);
}

int sv6621_memory_write(FAR struct sv6621_transport_s *transport,
                        uint32_t address, FAR const void *buffer,
                        size_t length)
{
  return sv6621_memory_transfer(transport, address, NULL, buffer, length);
}
