/****************************************************************************
 * apps/system/nbootctl/nbootctl_part.c
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

/* Partition-level writes driven from a host download.
 *
 * The host computes a SHA-256 of the file it is about to send and the board
 * compares that against what it actually received before anything is
 * written.  Only on a match does the payload go to the medium, and the
 * region is read back and compared afterwards.  A transfer that is cut
 * short, reordered or corrupted therefore cannot reach the flash: it fails
 * the comparison while the image is still sitting in a file.
 *
 * Two digest checks guard every write, and they catch different things:
 *
 *   - source vs expected: did the bytes arrive intact?
 *   - medium vs source:   did the bytes land intact?
 *
 * Neither is a substitute for the other -- a bad cable explains the first,
 * a bad sector the second.
 */

#include <nuttx/config.h>

#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <nuttx/fs/fs.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nbootctl_part.h"

#define NBOOTCTL_SECTOR_SIZE    512
#define NBOOTCTL_CHUNK_SECTORS  128

/* Partition index within the GPT, which is what NuttX uses to name the
 * device node: the layout in parameter.txt lists uboot first, so it becomes
 * /dev/mmcsdNp1.  Names are matched here and translated to an index; the
 * names are never used as node names directly.
 *
 * config sits between the AMP slots and data.  It holds provisioning and
 * identity, which a factory reset must preserve or deliberately clear,
 * while data holds models that an OTA can replace.  Keeping them in
 * separate partitions is what lets one be wiped without the other.
 */

struct nbootctl_partition_s
{
  const char *name;
  unsigned int index;
  uint64_t blocks;          /* 0 = grow to the end, size not enforced */
};

static const struct nbootctl_partition_s nbootctl_partitions[] =
{
  { "uboot",    1, 8192     },
  { "trust",    2, 8192     },
  { "bootctrl", 3, 2048     },
  { "nuttx_a",  4, 131072   },
  { "nuttx_b",  5, 131072   },
  { "amp_a",    6, 1048576  },
  { "amp_b",    7, 1048576  },
  { "config",   8, 65536    },
  { "data",     9, 0        },
};

/* Private Function Prototypes */

static const char *nbootctl_disk_path(unsigned int medium);
static int nbootctl_parse_hex(const char *text, uint8_t *out, size_t size);
static int nbootctl_hash_file(const char *path, uint8_t *digest,
                              uint64_t *size_out);
static int nbootctl_stream_file(int fd, uint64_t offset, size_t bytes,
                                uint8_t *buffer, SHA2_CTX *hash);
static int nbootctl_write_region(int fd, uint64_t lba, const char *path,
                                 uint64_t size, uint8_t *buffer,
                                 const uint8_t *expected);
static int nbootctl_verify_region(int fd, uint64_t lba, uint64_t sectors,
                                  uint8_t *buffer, const uint8_t *expected);
static int nbootctl_part_write_node(const char *node, const char *path,
                                    const char *hex, uint64_t capacity);
static int nbootctl_part_write_at(unsigned int medium, uint64_t lba,
                                  uint64_t capacity, const char *path,
                                  const char *hex);
static void nbootctl_print_hex(const uint8_t *digest);

/****************************************************************************
 * Name: nbootctl_disk_path
 ****************************************************************************/

static const char *nbootctl_disk_path(unsigned int medium)
{
  return medium == 1 ? "/dev/mmcsd0" : medium == 2 ? "/dev/mmcsd1" : NULL;
}

/****************************************************************************
 * Name: nbootctl_parse_hex
 *
 * Description:
 *   Turn a 64-character lowercase hex digest into 32 bytes.  Uppercase is
 *   accepted too; the host may format either way.
 *
 ****************************************************************************/

static int nbootctl_parse_hex(const char *text, uint8_t *out, size_t size)
{
  size_t i;

  if (text == NULL || strlen(text) != size * 2)
    {
      return -EINVAL;
    }

  for (i = 0; i < size; i++)
    {
      unsigned int hi;
      unsigned int lo;
      int j;

      for (j = 0; j < 2; j++)
        {
          char c = text[i * 2 + j];

          if (c >= '0' && c <= '9')
            {
              if (j == 0)
                {
                  hi = c - '0';
                }
              else
                {
                  lo = c - '0';
                }
            }
          else if (c >= 'a' && c <= 'f')
            {
              if (j == 0)
                {
                  hi = c - 'a' + 10;
                }
              else
                {
                  lo = c - 'a' + 10;
                }
            }
          else if (c >= 'A' && c <= 'F')
            {
              if (j == 0)
                {
                  hi = c - 'A' + 10;
                }
              else
                {
                  lo = c - 'A' + 10;
                }
            }
          else
            {
              return -EINVAL;
            }
        }

      out[i] = (uint8_t)((hi << 4) | lo);
    }

  return 0;
}

/****************************************************************************
 * Name: nbootctl_print_hex
 ****************************************************************************/

static void nbootctl_print_hex(const uint8_t *digest)
{
  int i;

  for (i = 0; i < NBOOTCTL_SHA256_SIZE; i++)
    {
      printf("%02x", digest[i]);
    }
}

/****************************************************************************
 * Name: nbootctl_hash_file
 ****************************************************************************/

static int nbootctl_hash_file(const char *path, uint8_t *digest,
                              uint64_t *size_out)
{
  struct stat file_info;
  SHA2_CTX hash;
  uint8_t *buffer;
  uint64_t offset;
  int fd;
  int ret = 0;

  if (stat(path, &file_info) < 0 || !S_ISREG(file_info.st_mode) ||
      file_info.st_size <= 0)
    {
      return -EINVAL;
    }

  buffer = memalign(64, NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      free(buffer);
      return -errno;
    }

  sha256init(&hash);
  for (offset = 0; offset < (uint64_t)file_info.st_size; )
    {
      size_t bytes = (uint64_t)file_info.st_size - offset >
                             NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE
                         ? NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE
                         : (size_t)((uint64_t)file_info.st_size - offset);

      if (read(fd, buffer, bytes) != (ssize_t)bytes)
        {
          ret = -EIO;
          break;
        }

      sha256update(&hash, buffer, bytes);
      offset += bytes;
    }

  if (ret == 0)
    {
      sha256final(digest, &hash);
      *size_out = (uint64_t)file_info.st_size;
    }

  close(fd);
  free(buffer);
  return ret;
}

/****************************************************************************
 * Name: nbootctl_stream_file
 *
 * Description:
 *   Read `bytes` from the file at `offset` into the buffer and feed them to
 *   the hash.  Short reads are an error: the caller sized the buffer from
 *   the file, so anything less means the file changed underneath us.
 *
 ****************************************************************************/

static int nbootctl_stream_file(int fd, uint64_t offset, size_t bytes,
                                uint8_t *buffer, SHA2_CTX *hash)
{
  memset(buffer, 0, bytes);
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0 ||
      read(fd, buffer, bytes) != (ssize_t)bytes)
    {
      return -EIO;
    }

  sha256update(hash, buffer, bytes);
  return 0;
}

/****************************************************************************
 * Name: nbootctl_verify_region
 *
 * Description:
 *   Hash a region of the medium and compare it with the expected digest.
 *   Read-only; used both after a write and on its own.
 *
 ****************************************************************************/

static int nbootctl_verify_region(int fd, uint64_t lba, uint64_t sectors,
                                  uint8_t *buffer, const uint8_t *expected)
{
  SHA2_CTX hash;
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint64_t done = 0;

  sha256init(&hash);
  while (done < sectors)
    {
      size_t take = sectors - done > NBOOTCTL_CHUNK_SECTORS
                    ? NBOOTCTL_CHUNK_SECTORS
                    : (size_t)(sectors - done);

      if (pread(fd, buffer, take * NBOOTCTL_SECTOR_SIZE,
                (off_t)((lba + done) * NBOOTCTL_SECTOR_SIZE)) !=
          (ssize_t)(take * NBOOTCTL_SECTOR_SIZE))
        {
          return -EIO;
        }

      sha256update(&hash, buffer, take * NBOOTCTL_SECTOR_SIZE);
      done += take;
    }

  sha256final(actual, &hash);
  return memcmp(actual, expected, NBOOTCTL_SHA256_SIZE) == 0
         ? 0 : -EKEYREJECTED;
}

/****************************************************************************
 * Name: nbootctl_write_region
 *
 * Description:
 *   Write the file into the medium at `lba`, then read the region back and
 *   compare it against the digest computed from the source.  The tail of
 *   the last sector is zero-padded, so the read-back covers whole sectors
 *   while the comparison is over the whole region.
 *
 ****************************************************************************/

static int nbootctl_write_region(int fd, uint64_t lba, const char *path,
                                 uint64_t size, uint8_t *buffer,
                                 const uint8_t *expected)
{
  SHA2_CTX source_hash;
  uint64_t sectors = (size + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
  uint64_t offset;
  int source;
  int ret = 0;

  source = open(path, O_RDONLY);
  if (source < 0)
    {
      return -errno;
    }

  sha256init(&source_hash);
  for (offset = 0; offset < size; )
    {
      size_t bytes = size - offset > NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE
                     ? NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE
                     : (size_t)(size - offset);
      size_t whole = (bytes + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;

      ret = nbootctl_stream_file(source, offset, bytes, buffer, &source_hash);
      if (ret < 0)
        {
          break;
        }

      if (pwrite(fd, buffer, whole * NBOOTCTL_SECTOR_SIZE,
                 (off_t)((lba + offset / NBOOTCTL_SECTOR_SIZE) *
                         NBOOTCTL_SECTOR_SIZE)) !=
          (ssize_t)(whole * NBOOTCTL_SECTOR_SIZE))
        {
          ret = -EIO;
          break;
        }

      offset += bytes;
    }

  close(source);
  if (ret < 0)
    {
      return ret;
    }

  {
    uint8_t computed[NBOOTCTL_SHA256_SIZE];

    sha256final(computed, &source_hash);
    if (memcmp(computed, expected, NBOOTCTL_SHA256_SIZE) != 0)
      {
        return -EKEYREJECTED;
      }
  }

  return nbootctl_verify_region(fd, lba, sectors, buffer, expected);
}

/****************************************************************************
 * Name: nbootctl_part_write_node
 *
 * Description:
 *   Write a file into a block device node, whose LBA 0 is the start of
 *   that device.  Used for partitions, where the node carries its own
 *   bounds, and for the raw window used by the loader.
 *
 ****************************************************************************/

static int nbootctl_part_write_node(const char *node, const char *path,
                                    const char *hex, uint64_t capacity)
{
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  uint64_t file_size;
  uint64_t sectors;
  int fd;
  int ret;

  if (nbootctl_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_hash_file(path, actual, &file_size);
  if (ret < 0)
    {
      return ret;
    }

  /* Refuse before opening the medium: a mismatched digest means the host
   * and the board disagree about what is being written, and nothing about
   * that is safe to proceed with.
   */

  if (memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: source digest mismatch\n");
      return -EKEYREJECTED;
    }

  sectors = (file_size + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
  if (sectors == 0 || (capacity != 0 && sectors > capacity))
    {
      fprintf(stderr, "nbootctl: payload does not fit (%llu > %llu blocks)\n",
              (unsigned long long)sectors, (unsigned long long)capacity);
      return -EFBIG;
    }

  buffer = memalign(64, NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  fd = open(node, O_RDWR);
  if (fd < 0)
    {
      free(buffer);
      return -errno;
    }

  ret = nbootctl_write_region(fd, 0, path, file_size, buffer, expected);

  close(fd);
  free(buffer);

  if (ret == 0)
    {
      printf("write: %llu bytes to %s, digest ",
             (unsigned long long)file_size, node);
      nbootctl_print_hex(expected);
      printf(", readback OK\n");
    }

  return ret;
}

/****************************************************************************
 * Name: nbootctl_part_write_at
 *
 * Description:
 *   Write a file at an absolute LBA of the whole disk.  Needed for regions
 *   no partition covers -- the MiniLoader sits at sector 64, between the
 *   protective MBR and the first partition.
 *
 ****************************************************************************/

static int nbootctl_part_write_at(unsigned int medium, uint64_t lba,
                                  uint64_t capacity, const char *path,
                                  const char *hex)
{
  const char *disk_path = nbootctl_disk_path(medium);
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  uint64_t file_size;
  uint64_t sectors;
  int fd;
  int ret;

  if (disk_path == NULL || nbootctl_parse_hex(hex, expected,
                                              sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_hash_file(path, actual, &file_size);
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: source digest mismatch\n");
      return -EKEYREJECTED;
    }

  sectors = (file_size + NBOOTCTL_SECTOR_SIZE - 1) / NBOOTCTL_SECTOR_SIZE;
  if (sectors == 0 || (capacity != 0 && sectors > capacity))
    {
      fprintf(stderr, "nbootctl: payload does not fit (%llu > %llu blocks)\n",
              (unsigned long long)sectors, (unsigned long long)capacity);
      return -EFBIG;
    }

  buffer = memalign(64, NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  fd = open(disk_path, O_RDWR);
  if (fd < 0)
    {
      free(buffer);
      return -errno;
    }

  ret = nbootctl_write_region(fd, lba, path, file_size, buffer, expected);

  close(fd);
  free(buffer);

  if (ret == 0)
    {
      printf("write: %llu bytes at lba %llu, digest ",
             (unsigned long long)file_size, (unsigned long long)lba);
      nbootctl_print_hex(expected);
      printf(", readback OK\n");
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int nbootctl_part_digest(const char *path)
{
  uint8_t digest[NBOOTCTL_SHA256_SIZE];
  uint64_t size;
  int ret;

  ret = nbootctl_hash_file(path, digest, &size);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: cannot hash %s: %d\n", path, ret);
      return ret;
    }

  printf("%llu ", (unsigned long long)size);
  nbootctl_print_hex(digest);
  printf("\n");
  return 0;
}

int nbootctl_part_verify(const char *path, const char *hex)
{
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t actual[NBOOTCTL_SHA256_SIZE];
  uint64_t size;
  int ret;

  if (nbootctl_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  ret = nbootctl_hash_file(path, actual, &size);
  if (ret < 0)
    {
      return ret;
    }

  if (memcmp(actual, expected, sizeof(actual)) != 0)
    {
      fprintf(stderr, "nbootctl: digest mismatch: got ");
      nbootctl_print_hex(actual);
      fprintf(stderr, "\n");
      return -EKEYREJECTED;
    }

  printf("verify: %llu bytes match\n", (unsigned long long)size);
  return 0;
}

int nbootctl_part_device_path(unsigned int medium, const char *partition,
                              char *out, size_t size)
{
  const char *disk_path = nbootctl_disk_path(medium);
  size_t i;

  if (disk_path == NULL || partition == NULL || out == NULL || size == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < sizeof(nbootctl_partitions) /
                  sizeof(nbootctl_partitions[0]); i++)
    {
      if (strcmp(partition, nbootctl_partitions[i].name) == 0)
        {
          snprintf(out, size, "%sp%u", disk_path,
                   nbootctl_partitions[i].index);
          return 0;
        }
    }

  return -ENOENT;
}

int nbootctl_part_write(unsigned int medium, const char *partition,
                        const char *path, const char *hex)
{
  const struct nbootctl_partition_s *entry = NULL;
  char node[32];
  size_t i;
  int ret;

  for (i = 0; i < sizeof(nbootctl_partitions) /
                  sizeof(nbootctl_partitions[0]); i++)
    {
      if (partition != NULL &&
          strcmp(partition, nbootctl_partitions[i].name) == 0)
        {
          entry = &nbootctl_partitions[i];
          break;
        }
    }

  if (entry == NULL)
    {
      fprintf(stderr, "nbootctl: unknown partition %s\n",
              partition != NULL ? partition : "(null)");
      return -EINVAL;
    }

  /* Write through the partition's own device node.  NuttX names block
   * partitions by their index in the table, so /dev/mmcsdNp1 is uboot and
   * /dev/mmcsdNp3 is bootctrl -- the name the user typed never reaches the
   * filesystem.  Using the partition node rather than the whole disk at an
   * offset means NuttX itself bounds the write to the partition.
   */

  ret = nbootctl_part_device_path(medium, partition, node, sizeof(node));
  if (ret < 0)
    {
      return ret;
    }

  return nbootctl_part_write_node(node, path, hex, entry->blocks);
}

int nbootctl_part_write_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *path,
                            const char *hex)
{
  return nbootctl_part_write_at(medium, lba, sectors, path, hex);
}

int nbootctl_part_write_gpt(unsigned int medium, const char *path,
                            const char *hex)
{
  /* The table occupies LBA 0 through the end of the entry array.  With the
   * standard 128 entries of 128 bytes that is 34 sectors; the file is
   * expected to carry a whole number of them and is allowed to be shorter.
   */

  return nbootctl_part_write_at(medium, 0, 34, path, hex);
}

int nbootctl_part_check_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *hex)
{
  const char *disk_path = nbootctl_disk_path(medium);
  uint8_t expected[NBOOTCTL_SHA256_SIZE];
  uint8_t *buffer;
  int fd;
  int ret;

  if (disk_path == NULL || sectors == 0 ||
      nbootctl_parse_hex(hex, expected, sizeof(expected)) < 0)
    {
      return -EINVAL;
    }

  buffer = memalign(64, NBOOTCTL_CHUNK_SECTORS * NBOOTCTL_SECTOR_SIZE);
  if (buffer == NULL)
    {
      return -ENOMEM;
    }

  fd = open(disk_path, O_RDONLY);
  if (fd < 0)
    {
      free(buffer);
      return -errno;
    }

  ret = nbootctl_verify_region(fd, lba, sectors, buffer, expected);

  close(fd);
  free(buffer);

  if (ret == 0)
    {
      printf("check: lba %llu..%llu matches\n", (unsigned long long)lba,
             (unsigned long long)(lba + sectors - 1));
    }
  else
    {
      fprintf(stderr, "nbootctl: region digest mismatch\n");
    }

  return ret;
}
