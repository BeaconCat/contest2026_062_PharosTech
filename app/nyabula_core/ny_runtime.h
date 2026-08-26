/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_runtime.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <quickjs/quickjs.h>

#include "ny_manifest.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

enum ny_plugin_state_e
{
  NY_PLUGIN_EMPTY = 0,
  NY_PLUGIN_LOADED,
  NY_PLUGIN_RUNNING,
  NY_PLUGIN_STOPPED,
  NY_PLUGIN_FAILED
};

struct ny_plugin_s
{
  JSRuntime *runtime;
  JSContext *context;
  JSModuleDef *module;
  enum ny_plugin_state_e state;
  uint64_t deadline_ns;
  uint64_t permissions;
  size_t memory_limit;
  size_t stack_limit;
  uint32_t event_timeout_ms;
  bool module_entry;
  atomic_bool cancelled;
  char id[NY_PLUGIN_ID_SIZE];
  char version[NY_PLUGIN_VERSION_SIZE];
  char root[PATH_MAX];
  char path[PATH_MAX];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_plugin_load(struct ny_plugin_s *plugin, const char *path);
int ny_plugin_load_config(struct ny_plugin_s *plugin,
                          const struct ny_plugin_config_s *config);
int ny_plugin_start(struct ny_plugin_s *plugin);
int ny_plugin_dispatch(struct ny_plugin_s *plugin, const char *event);
int ny_plugin_stop(struct ny_plugin_s *plugin);
void ny_plugin_destroy(struct ny_plugin_s *plugin);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_RUNTIME_H */
