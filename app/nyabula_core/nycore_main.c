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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct ny_plugin_s plugin;
  const char *event = NULL;
  int ret;

  if (argc != 2 && argc != 4)
    {
      fprintf(stderr, "Usage: nycore <plugin.js> [-e <event>]\n");
      return EXIT_FAILURE;
    }

  if (argc == 4)
    {
      if (strcmp(argv[2], "-e") != 0)
        {
          fprintf(stderr, "nycore: expected -e before event\n");
          return EXIT_FAILURE;
        }

      event = argv[3];
    }

  ret = ny_plugin_load(&plugin, argv[1]);
  if (ret < 0)
    {
      fprintf(stderr, "nycore: load failed: %d\n", ret);
      ny_plugin_destroy(&plugin);
      return EXIT_FAILURE;
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
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
