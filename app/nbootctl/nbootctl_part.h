/****************************************************************************
 * apps/system/nbootctl/nbootctl_part.h
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

#ifndef __APPS_SYSTEM_NBOOTCTL_NBOOTCTL_PART_H
#define __APPS_SYSTEM_NBOOTCTL_NBOOTCTL_PART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NBOOTCTL_SHA256_SIZE 32
#define NBOOTCTL_SHA256_HEX  64

/* Room for /dev/mmcsdNpMM and a terminator. */
#define NBOOTCTL_FORMAT_PATH_MAX 32

/* Print the SHA-256 of a file as hex.  Used by the web UI's upload flow to
 * compare the browser's digest against what the board received.
 */

int nbootctl_part_digest(const char *path);

/* Compare a file against an expected lowercase hex digest, without writing
 * anything.  Returns 0 when they match, -EKEYREJECTED when they do not.
 */

int nbootctl_part_verify(const char *path, const char *hex);

/* Write a file into a named GPT partition.
 *
 * The digest is checked against the source before a single sector is
 * touched, then the whole partition region is read back and compared.
 * `partition` is one of uboot, trust, bootctrl, nuttx_a, nuttx_b, amp_a,
 * amp_b, data; the device node is derived from its index in the layout,
 * not from the name, because NuttX numbers them (/dev/mmcsdNpM).
 */

int nbootctl_part_write(unsigned int medium, const char *partition,
                        const char *path, const char *hex);

/* Write a file at an absolute LBA, for regions no partition covers -- the
 * MiniLoader at sector 64 being the one that matters.  `sectors` caps the
 * write so a mistaken size cannot run off the end of the layout.
 */

int nbootctl_part_write_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *path,
                            const char *hex);

/* Replace the partition table itself.  Writes at LBA 0, so it covers the
 * protective MBR, the primary header and the entry array in one pass.
 */

int nbootctl_part_write_gpt(unsigned int medium, const char *path,
                            const char *hex);

/* Compute the SHA-256 of a region of the boot medium and compare it with a
 * hex digest.  Read-only.
 */

int nbootctl_part_check_raw(unsigned int medium, uint64_t lba,
                            uint64_t sectors, const char *hex);

/* Resolve a partition name to its device node.  Exposed because the
 * formatter needs the same name-to-node mapping the writer uses, and two
 * tables would drift.
 */

int nbootctl_part_device_path(unsigned int medium, const char *partition,
                              char *out, size_t size);

/* Create a filesystem on a named partition. */
int nbootctl_format_partition(unsigned int medium, const char *partition);

#endif /* __APPS_SYSTEM_NBOOTCTL_NBOOTCTL_PART_H */
