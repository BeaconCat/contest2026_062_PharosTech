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
static int nbootctl_parse_slot(const char *value, unsigned int *slot);
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
 * Name: nbootctl_parse_slot
 ****************************************************************************/

static int nbootctl_parse_slot(const char *value, unsigned int *slot)
{
  if (strcmp(value, "a") == 0)
    {
      *slot = 0;
      return 0;
    }

  if (strcmp(value, "b") == 0)
    {
      *slot = 1;
      return 0;
    }

  return -1;
}

/****************************************************************************
 * Name: nbootctl_usage
 ****************************************************************************/

static void nbootctl_usage(void)
{
  fprintf(stderr, "usage: nbootctl status\n"
                  "       nbootctl verify nuttx|amp a|b\n"
                  "       nbootctl set-active nuttx|amp a|b\n"
                  "       nbootctl mark-successful nuttx|amp a|b\n"
                  "       nbootctl stage nuttx|amp IMAGE\n"
                  "       nbootctl clone nuttx|amp a|b a|b\n");
}

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  unsigned int medium;
  unsigned int running_slot;
  unsigned int slot;
  int ret;

  if (argc == 2 && strcmp(argv[1], "status") == 0)
    {
      return nbootctl_status();
    }

  if (argc == 4 &&
      (strcmp(argv[1], "verify") == 0 || strcmp(argv[1], "set-active") == 0 ||
       strcmp(argv[1], "mark-successful") == 0))
    {
      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0 || nbootctl_parse_slot(argv[3], &slot) != 0)
        {
          nbootctl_usage();
          return 1;
        }

      if (strcmp(argv[1], "verify") == 0)
        {
          ret = nbootctl_bootctrl_verify(medium, argv[2], slot);
        }
      else if (strcmp(argv[1], "set-active") == 0)
        {
          ret = nbootctl_bootctrl_set_active(medium, argv[2], slot);
        }
      else
        {
          ret = nbootctl_bootctrl_mark_successful(medium, argv[2], slot);
        }
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: %s failed: %d\n", argv[1], ret);
          return 1;
        }
      printf("%s %s %c: OK\n", argv[1], argv[2], slot ? 'b' : 'a');
      return 0;
    }

  if (argc == 4 && strcmp(argv[1], "stage") == 0)
    {
      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0)
        {
          return 1;
        }

      ret = nbootctl_bootctrl_stage(medium, argv[2], running_slot, argv[3]);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: stage failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

  if (argc == 5 && strcmp(argv[1], "clone") == 0)
    {
      unsigned int target_slot;

      ret = nbootctl_handoff(&medium, &running_slot);
      if (ret != 0 || nbootctl_parse_slot(argv[3], &slot) != 0 ||
          nbootctl_parse_slot(argv[4], &target_slot) != 0)
        {
          nbootctl_usage();
          return 1;
        }

      ret = nbootctl_bootctrl_clone(medium, argv[2], slot, target_slot);
      if (ret < 0)
        {
          fprintf(stderr, "nbootctl: clone failed: %d\n", ret);
          return 1;
        }

      return 0;
    }

  nbootctl_usage();
  return 1;
}
