/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/nycore_main.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ny_runtime.h"
#include "ny_scheduler.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void nycore_usage(void);
static int nycore_run(const char *path, const char *event);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void nycore_usage(void)
{
  fprintf(stderr, "Usage:\n"
                  "  nycore run <plugin.js> [-e <event>]\n"
                  "  nycore start <id> <plugin.js>\n"
                  "  nycore event <id> <event>\n"
                  "  nycore stop <id>\n"
                  "  nycore stop-all\n"
                  "  nycore list\n");
}

static int nycore_run(const char *path, const char *event)
{
  struct ny_plugin_s plugin;
  int ret;

  ret = ny_plugin_load(&plugin, path);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: load failed: %d\n", ret);
      ny_plugin_destroy(&plugin);
      return ret;
    }

  ret = ny_plugin_start(&plugin);
  if (ret >= 0 && event != NULL)
    {
      ret = ny_plugin_dispatch(&plugin, event);
    }

  if (ret >= 0)
    {
      ret = ny_plugin_stop(&plugin);
    }

  ny_plugin_destroy(&plugin);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  int ret;

  if (argc < 2)
    {
      nycore_usage();
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "run") == 0)
    {
      const char *event = NULL;

      if (argc != 3 && argc != 5)
        {
          nycore_usage();
          return EXIT_FAILURE;
        }

      if (argc == 5)
        {
          if (strcmp(argv[3], "-e") != 0)
            {
              nycore_usage();
              return EXIT_FAILURE;
            }

          event = argv[4];
        }

      ret = nycore_run(argv[2], event);
    }
  else if (strcmp(argv[1], "start") == 0 && argc == 4)
    {
      ret = ny_scheduler_start(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "event") == 0 && argc == 4)
    {
      ret = ny_scheduler_dispatch(argv[2], argv[3]);
    }
  else if (strcmp(argv[1], "stop") == 0 && argc == 3)
    {
      ret = ny_scheduler_stop(argv[2]);
    }
  else if (strcmp(argv[1], "stop-all") == 0 && argc == 2)
    {
      ret = ny_scheduler_stop_all();
    }
  else if (strcmp(argv[1], "list") == 0 && argc == 2)
    {
      ny_scheduler_list();
      ret = 0;
    }
  else
    {
      nycore_usage();
      return EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "nycore: command failed: %d\n", ret);
    }

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
