/****************************************************************************
 * apps/system/nbootctl/nbootctl_main.c
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

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>

#include "nbootctl_bootctrl.h"

#define NBOOTCTL_HANDOFF_REG        0x26026234ul
#define NBOOTCTL_GENERATION_LO_REG  0x26026238ul
#define NBOOTCTL_GENERATION_HI_REG  0x2602623cul
#define NBOOTCTL_HANDOFF_MAGIC      0x4e480000u
#define NBOOTCTL_HANDOFF_MAGIC_MASK 0xffff0000u
#define NBOOTCTL_HANDOFF_VERSION    2u

/* Private Function Prototypes */

static uint32_t nbootctl_read(uintptr_t address);
static const char *nbootctl_medium_name(unsigned int medium);
static const char *nbootctl_reason_name(unsigned int reason);
static int nbootctl_handoff(unsigned int *medium_out, unsigned int *slot_out);
static int nbootctl_status(void);
static void nbootctl_usage(void);

/****************************************************************************
 * Name: nbootctl_read
 ****************************************************************************/

static uint32_t nbootctl_read(uintptr_t address)
{
  return *(volatile uint32_t *)address;
}

/****************************************************************************
 * Name: nbootctl_medium_name
 ****************************************************************************/

static const char *nbootctl_medium_name(unsigned int medium)
{
  return medium == 1 ? "sd" : medium == 2 ? "emmc" : "unknown";
}

/****************************************************************************
 * Name: nbootctl_reason_name
 ****************************************************************************/

static const char *nbootctl_reason_name(unsigned int reason)
{
  return reason == 0   ? "normal"
         : reason == 1 ? "requested-slot"
         : reason == 2 ? "fallback"
                       : "unknown";
}

/****************************************************************************
 * Name: nbootctl_handoff
 ****************************************************************************/

static int nbootctl_handoff(unsigned int *medium_out, unsigned int *slot_out)
{
  uint32_t header;
  uint32_t confirm;
  uint64_t generation;
  unsigned int version;
  unsigned int medium;
  unsigned int reason;
  unsigned int slot;

  header = nbootctl_read(NBOOTCTL_HANDOFF_REG);
  generation = nbootctl_read(NBOOTCTL_GENERATION_LO_REG);
  generation |= (uint64_t)nbootctl_read(NBOOTCTL_GENERATION_HI_REG) << 32;
  confirm = nbootctl_read(NBOOTCTL_HANDOFF_REG);

  version = (header >> 12) & 0xf;
  if (header != confirm ||
      (header & NBOOTCTL_HANDOFF_MAGIC_MASK) != NBOOTCTL_HANDOFF_MAGIC ||
      version != NBOOTCTL_HANDOFF_VERSION)
    {
      fprintf(stderr, "nbootctl: no valid N-Boot handoff\n");
      return 1;
    }

  reason = (header >> 8) & 0xf;
  medium = (header >> 4) & 0xf;
  slot = header & 0xf;
  if (medium < 1 || medium > 2 || reason > 2 || slot > 1)
    {
      fprintf(stderr, "nbootctl: invalid N-Boot handoff fields\n");
      return 1;
    }

  printf("medium=%s\nslot=%c\ngeneration=%llu\nreason=%s\n",
         nbootctl_medium_name(medium), slot ? 'b' : 'a',
         (unsigned long long)generation, nbootctl_reason_name(reason));
  *medium_out = medium;
  *slot_out = slot;
  return 0;
}

/****************************************************************************
 * Name: nbootctl_status
 ****************************************************************************/

static int nbootctl_status(void)
{
  unsigned int medium;
  unsigned int slot;
  int ret;

  ret = nbootctl_handoff(&medium, &slot);
  if (ret != 0)
    {
      return ret;
    }

  return nbootctl_bootctrl_status(medium);
}

/****************************************************************************
 * Name: nbootctl_usage
 ****************************************************************************/

static void nbootctl_usage(void)
{
  fprintf(stderr, "usage: nbootctl status\n");
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
      return nbootctl_status();
    }

  nbootctl_usage();
  return 1;
}
