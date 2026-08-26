/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_wasm.h
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

#ifndef __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H
#define __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "ny_manifest.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct ny_wasm_plugin_s
{
  void *module;
  void *instance;
  void *environment;
  uint8_t *binary;
  uint32_t binary_size;
  uint32_t event_timeout_ms;
  atomic_uint_fast64_t permissions;
  bool thread_initialized;
  char id[NY_PLUGIN_ID_SIZE];
  char storage_root[PATH_MAX];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int ny_wasm_run(const char *path, const char *id, uint64_t permissions);
int ny_wasm_run_config(const struct ny_plugin_config_s *config);
int ny_wasm_load(struct ny_wasm_plugin_s *plugin,
                 const struct ny_plugin_config_s *config);
int ny_wasm_start(struct ny_wasm_plugin_s *plugin);
int ny_wasm_dispatch(struct ny_wasm_plugin_s *plugin, const char *event);
int ny_wasm_stop(struct ny_wasm_plugin_s *plugin);
void ny_wasm_set_permissions(struct ny_wasm_plugin_s *plugin,
                             uint64_t permissions);
void ny_wasm_destroy(struct ny_wasm_plugin_s *plugin);

#endif /* __PACKAGES_DEMOS_CONTEST2026_062_NYABULA_CORE_NY_WASM_H */
