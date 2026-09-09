/****************************************************************************
 * apps/system/nbootctl/nbootctl_bootctrl.c
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

#include <nuttx/config.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <nuttx/crc32.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/ioctl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nbootctl_bootctrl.h"

#define NBOOTCTL_MAGIC          "K7ABCTRL"
#define NBOOTCTL_FORMAT_VERSION 1
#define NBOOTCTL_RECORD_SIZE    4096
#define NBOOTCTL_RECORD_SECTORS 8
#define NBOOTCTL_COPY_COUNT     2
#define NBOOTCTL_SHA256_SIZE    32
#define NBOOTCTL_VERIFY_SECTORS 128
#define NBOOTCTL_SECTOR_SIZE    512
#define NBOOTCTL_UBOOT_START    16384
#define NBOOTCTL_UBOOT_SECTORS  8192
#define NBOOTCTL_FIT_MAGIC      0xd00dfeedu
#define NBOOTCTL_REBOOT_MAGIC   0x4e425200u

struct nbootctl_slot_s
{
  uint8_t priority;
  uint8_t tries_remaining;
  uint8_t successful;
  uint8_t reserved;
  uint64_t image_size;
  uint64_t image_version;
  uint8_t sha256[NBOOTCTL_SHA256_SIZE];
} __attribute__((packed));

struct nbootctl_domain_s
{
  uint8_t active_slot;
  uint8_t reserved[3];
  struct nbootctl_slot_s slots[2];
} __attribute__((packed));

struct nbootctl_record_s
{
  uint8_t magic[8];
  uint16_t format_version;
  uint16_t header_size;
  uint64_t generation;
  struct nbootctl_domain_s domains[2];
  uint8_t padding[NBOOTCTL_RECORD_SIZE - 4 - 20 -
                  sizeof(struct nbootctl_domain_s) * 2];
  uint32_t crc32;
} __attribute__((packed));

_Static_assert(sizeof(struct nbootctl_record_s) == NBOOTCTL_RECORD_SIZE,
               "bootctrl record size changed");
_Static_assert(sizeof(struct nbootctl_slot_s) == 52,
               "bootctrl slot size changed");
_Static_assert(sizeof(struct nbootctl_domain_s) == 108,
               "bootctrl domain size changed");
_Static_assert(offsetof(struct nbootctl_record_s, generation) == 12,
               "bootctrl generation offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, domains) == 20,
               "bootctrl domain offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, crc32) == 4092,
               "bootctrl CRC offset changed");
_Static_assert(offsetof(struct nbootctl_record_s, padding) == 236,
               "bootctrl request offset changed");

/* Private Function Prototypes */

static const char *nbootctl_bootctrl_path(unsigned int medium);
static uint32_t nbootctl_crc32(const void *data, size_t size);
static bool nbootctl_record_valid(const struct nbootctl_record_s *record);
static int nbootctl_read_records(unsigned int medium, struct inode **inode,
                                 struct nbootctl_record_s *records,
                                 int *selected);

/****************************************************************************
 * Name: nbootctl_bootctrl_path
 ****************************************************************************/

static const char *nbootctl_bootctrl_path(unsigned int medium)
{
  return medium == 1 ? "/dev/mmcsd0p3" : medium == 2 ? "/dev/mmcsd1p3" : NULL;
}

/****************************************************************************
 * Name: nbootctl_crc32
 ****************************************************************************/

static uint32_t nbootctl_crc32(const void *data, size_t size)
{
  return crc32part(data, size, UINT32_MAX) ^ UINT32_MAX;
}

/****************************************************************************
 * Name: nbootctl_record_valid
 ****************************************************************************/

static bool nbootctl_record_valid(const struct nbootctl_record_s *record)
{
  uint32_t checksum;

  if (memcmp(record->magic, NBOOTCTL_MAGIC, sizeof(record->magic)) != 0 ||
      record->format_version != NBOOTCTL_FORMAT_VERSION ||
      record->header_size != 20)
    {
      return false;
    }

  checksum = nbootctl_crc32(record, offsetof(struct nbootctl_record_s, crc32));
  return checksum == record->crc32;
}

/****************************************************************************
 * Name: nbootctl_read_records
 ****************************************************************************/

static int nbootctl_read_records(unsigned int medium, struct inode **inode,
                                 struct nbootctl_record_s *records,
                                 int *selected)
{
  const char *path = nbootctl_bootctrl_path(medium);
  int best = -1;
  int index;
  int ret;

  if (path == NULL)
    {
      return -EINVAL;
    }

  ret = open_blockdriver(path, 0, inode);
  if (ret < 0 || (*inode)->u.i_bops->read == NULL ||
      (*inode)->u.i_bops->write == NULL)
    {
      if (ret >= 0)
        {
          close_blockdriver(*inode);
          *inode = NULL;
        }

      return ret < 0 ? ret : -ENOSYS;
    }

  for (index = 0; index < NBOOTCTL_COPY_COUNT; index++)
    {
      if ((*inode)->u.i_bops->read(*inode, (uint8_t *)&records[index],
                                   index * NBOOTCTL_RECORD_SECTORS,
                                   NBOOTCTL_RECORD_SECTORS) !=
              NBOOTCTL_RECORD_SECTORS ||
          !nbootctl_record_valid(&records[index]))
        {
          continue;
        }

      if (best < 0 || records[index].generation > records[best].generation)
        {
          best = index;
        }
    }

  if (best < 0)
    {
      close_blockdriver(*inode);
      *inode = NULL;
      return -EBADMSG;
    }

  *selected = best;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_bootctrl_status
 ****************************************************************************/

int nbootctl_bootctrl_status(unsigned int medium)
{
  struct nbootctl_record_s *records;
  struct nbootctl_record_s *record;
  struct inode *inode = NULL;
  int selected;
  int domain;
  int slot;
  int ret;

  records = memalign(64, sizeof(*records) * NBOOTCTL_COPY_COUNT);
  if (records == NULL)
    {
      return -ENOMEM;
    }

  ret = nbootctl_read_records(medium, &inode, records, &selected);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: bootctrl read failed: %d\n", ret);
      free(records);
      return ret;
    }

  record = &records[selected];
  printf("bootctrl_generation=%llu\n", (unsigned long long)record->generation);
  for (domain = 0; domain < 2; domain++)
    {
      const char *name = domain == 0 ? "nuttx" : "amp";

      printf("%s_active=%c\n", name,
             record->domains[domain].active_slot ? 'b' : 'a');
      for (slot = 0; slot < 2; slot++)
        {
          const struct nbootctl_slot_s *entry =
              &record->domains[domain].slots[slot];

          printf("%s_%c priority=%u successful=%u size=%llu version=%llu\n",
                 name, slot ? 'b' : 'a', entry->priority, entry->successful,
                 (unsigned long long)entry->image_size,
                 (unsigned long long)entry->image_version);
        }
    }

  close_blockdriver(inode);
  free(records);
  return 0;
}
