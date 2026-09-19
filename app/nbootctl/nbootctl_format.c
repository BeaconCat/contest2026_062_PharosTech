/****************************************************************************
 * apps/system/nbootctl/nbootctl_format.c
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

/* Filesystem creation for the store partitions.
 *
 * Two callers, one implementation.  The board's storage driver reaches
 * here through kickpi_k7_storage_format_hook() when it finds a partition
 * that has never been formatted, and an operator can ask directly with
 * `nbootctl format <partition>`.  Both want the same thing: a fresh FAT
 * filesystem on a named partition.
 *
 * The formatter itself is the mkfatfs utility's, whose header is public in
 * the application tree.  Calling it from here rather than from the driver
 * keeps the dependency pointing the way it should -- an application may
 * use a board service, not the other way round.
 */

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fsutils/mkfatfs.h>
#include <nuttx/fs/fs.h>

#include "nbootctl_part.h"

/* Private Function Prototypes */

static int nbootctl_format_device(FAR const char *path);

/****************************************************************************
 * Name: nbootctl_format_device
 *
 * Description:
 *   Put a FAT filesystem on a block device node.
 *
 *   A device that is already mounted is left alone: mkfatfs writes over
 *   the structures the mounted filesystem is using, and the result is not
 *   merely wrong but undefined.  The caller is expected to know whether
 *   the partition is in use, so this only refuses and reports.
 *
 ****************************************************************************/

static int nbootctl_format_device(FAR const char *path)
{
  struct fat_format_s fmt;
  int ret;

  if (path == NULL)
    {
      return -EINVAL;
    }

  /* Leaving the fields at their autoselect values lets mkfatfs size the
   * FAT and the cluster from the device; only the label is set, so the two
   * store partitions are distinguishable from a host.  ff_volumelabel is a
   * fixed 11-byte field, not a pointer.
   */

  memset(&fmt, 0, sizeof(fmt));
  fmt.ff_clustshift = 0xff;
  memcpy(fmt.ff_volumelabel, "NYABULA    ", sizeof(fmt.ff_volumelabel));

  ret = mkfatfs(path, &fmt);
  if (ret < 0)
    {
      return -errno;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int nbootctl_format_partition(unsigned int medium, FAR const char *partition)
{
  char path[NBOOTCTL_FORMAT_PATH_MAX];
  int ret;

  ret = nbootctl_part_device_path(medium, partition, path, sizeof(path));
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: cannot format %s: %d\n",
              partition != NULL ? partition : "(null)", ret);
      return ret;
    }

  printf("nbootctl: formatting %s\n", path);
  ret = nbootctl_format_device(path);
  if (ret < 0)
    {
      fprintf(stderr, "nbootctl: format %s failed: %d\n", path, ret);
      return ret;
    }

  printf("nbootctl: %s formatted\n", path);
  return 0;
}

/****************************************************************************
 * Name: kickpi_k7_storage_format_hook
 *
 * Description:
 *   Strong definition of the weak hook the board declares.
 *
 *   The board source is compiled into every configuration of this board,
 *   including ones that do not include nbootctl, so it can only carry a
 *   weak reference.  When this application is present it takes over and a
 *   partition found unformatted at boot is prepared automatically.
 *
 ****************************************************************************/

int kickpi_k7_storage_format_hook(FAR const char *path)
{
  if (path == NULL)
    {
      return -EINVAL;
    }

  return nbootctl_format_device(path);
}
