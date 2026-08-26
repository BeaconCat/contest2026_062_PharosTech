/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_wasm.c
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
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wasm_export.h>

#include "ny_manifest.h"
#include "ny_wasm.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_WASM_ERROR_SIZE 192
#define NY_WASM_LOG_LIMIT  1024

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_wasm_lock = NXMUTEX_INITIALIZER;
static bool g_wasm_initialized;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int32_t ny_wasm_core_log(wasm_exec_env_t environment, int32_t offset,
                                int32_t length);
static int ny_wasm_initialize(void);
static int ny_wasm_read(const char *path, uint8_t **binary, uint32_t *size);
static int ny_wasm_call(wasm_module_inst_t instance,
                        wasm_exec_env_t environment, const char *name,
                        bool required);
static int ny_wasm_call_event(struct ny_wasm_plugin_s *plugin,
                              const char *event);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static NativeSymbol g_wasm_symbols[] = {
  { "core_log", (void *)ny_wasm_core_log, "(ii)i", NULL },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int32_t ny_wasm_core_log(wasm_exec_env_t environment, int32_t offset,
                                int32_t length)
{
  struct ny_wasm_plugin_s *plugin;
  wasm_module_inst_t instance;
  const char *message;

  plugin = wasm_runtime_get_user_data(environment);
  instance = wasm_runtime_get_module_inst(environment);
  if (plugin == NULL ||
      (atomic_load(&plugin->permissions) & NY_PERMISSION_CORE_LOG) == 0)
    {
      return -EACCES;
    }

  if (offset < 0 || length < 0 || length > NY_WASM_LOG_LIMIT ||
      !wasm_runtime_validate_app_addr(instance, (uint32_t)offset,
                                      (uint32_t)length))
    {
      return -EINVAL;
    }

  message = wasm_runtime_addr_app_to_native(instance, (uint32_t)offset);
  printf("nyplugin[%s]: %.*s\n", plugin->id, length, message);
  return 0;
}

static int ny_wasm_call_event(struct ny_wasm_plugin_s *plugin,
                              const char *event)
{
  wasm_module_inst_t instance = plugin->instance;
  wasm_exec_env_t environment = plugin->environment;
  wasm_function_inst_t function;
  wasm_valkind_t types[2];
  const char *exception;
  void *native;
  uint64_t offset;
  uint32_t arguments[2];
  size_t length = strlen(event);

  if (length >= CONFIG_NYABULA_CORE_EVENT_SIZE)
    {
      return -E2BIG;
    }

  function = wasm_runtime_lookup_function(instance, "ny_on_event");
  if (function == NULL)
    {
      return 0;
    }

  if (wasm_func_get_param_count(function, instance) != 2 ||
      wasm_func_get_result_count(function, instance) != 0)
    {
      return -EPROTO;
    }

  wasm_func_get_param_types(function, instance, types);
  if (types[0] != WASM_I32 || types[1] != WASM_I32)
    {
      return -EPROTO;
    }

  offset =
      wasm_runtime_module_malloc(instance, length == 0 ? 1 : length, &native);
  if (offset == 0 || offset > UINT32_MAX || native == NULL)
    {
      return -ENOMEM;
    }

  memcpy(native, event, length);
  arguments[0] = (uint32_t)offset;
  arguments[1] = (uint32_t)length;
  if (!wasm_runtime_call_wasm(environment, function, 2, arguments))
    {
      exception = wasm_runtime_get_exception(instance);
      fprintf(stderr, "nycore: wasm ny_on_event failed: %s\n",
              exception == NULL ? "unknown exception" : exception);
      wasm_runtime_module_free(instance, offset);
      return -EFAULT;
    }

  wasm_runtime_module_free(instance, offset);
  return 0;
}

static int ny_wasm_initialize(void)
{
  int ret = 0;

  nxmutex_lock(&g_wasm_lock);
  if (!g_wasm_initialized)
    {
      if (!wasm_runtime_init() ||
          !wasm_runtime_register_natives("nyabula", g_wasm_symbols,
                                         sizeof(g_wasm_symbols) /
                                             sizeof(g_wasm_symbols[0])))
        {
          ret = -EIO;
        }
      else
        {
          g_wasm_initialized = true;
        }
    }

  nxmutex_unlock(&g_wasm_lock);
  return ret;
}

static int ny_wasm_read(const char *path, uint8_t **binary, uint32_t *size)
{
  struct stat status;
  size_t offset = 0;
  int fd;

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      return -errno;
    }

  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || status.st_size > CONFIG_NYABULA_CORE_SOURCE_LIMIT)
    {
      close(fd);
      return -EFBIG;
    }

  *binary = malloc((size_t)status.st_size);
  if (*binary == NULL)
    {
      close(fd);
      return -ENOMEM;
    }

  while (offset < (size_t)status.st_size)
    {
      ssize_t count =
          read(fd, *binary + offset, (size_t)status.st_size - offset);

      if (count < 0 && errno == EINTR)
        {
          continue;
        }

      if (count <= 0)
        {
          free(*binary);
          *binary = NULL;
          close(fd);
          return count < 0 ? -errno : -EIO;
        }

      offset += (size_t)count;
    }

  close(fd);
  *size = (uint32_t)status.st_size;
  return 0;
}

static int ny_wasm_call(wasm_module_inst_t instance,
                        wasm_exec_env_t environment, const char *name,
                        bool required)
{
  wasm_function_inst_t function;
  const char *exception;

  function = wasm_runtime_lookup_function(instance, name);
  if (function == NULL)
    {
      return required ? -ENOEXEC : 0;
    }

  if (wasm_func_get_param_count(function, instance) != 0 ||
      wasm_func_get_result_count(function, instance) != 0)
    {
      return -EPROTO;
    }

  if (!wasm_runtime_call_wasm(environment, function, 0, NULL))
    {
      exception = wasm_runtime_get_exception(instance);
      fprintf(stderr, "nycore: wasm %s failed: %s\n", name,
              exception == NULL ? "unknown exception" : exception);
      return -EFAULT;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_wasm_run(const char *path, const char *id, uint64_t permissions)
{
  struct ny_plugin_config_s config;
  struct ny_wasm_plugin_s plugin;
  int ret;

  if (path == NULL || id == NULL || id[0] == '\0')
    {
      return -EINVAL;
    }

  memset(&config, 0, sizeof(config));
  strlcpy(config.id, id, sizeof(config.id));
  strlcpy(config.entry, path, sizeof(config.entry));
  config.permissions = permissions;
  config.memory_limit = CONFIG_NYABULA_CORE_PLUGIN_MEMORY;
  config.stack_limit = CONFIG_NYABULA_CORE_PLUGIN_STACK;
  ret = ny_wasm_load(&plugin, &config);
  if (ret >= 0)
    {
      ret = ny_wasm_start(&plugin);
    }

  if (ret >= 0)
    {
      ret = ny_wasm_stop(&plugin);
    }

  ny_wasm_destroy(&plugin);
  return ret;
}

int ny_wasm_load(struct ny_wasm_plugin_s *plugin,
                 const struct ny_plugin_config_s *config)
{
  InstantiationArgs arguments;
  char error[NY_WASM_ERROR_SIZE];
  int ret;

  if (plugin == NULL || config == NULL || config->id[0] == '\0' ||
      config->entry[0] == '\0' || config->stack_limit == 0 ||
      config->stack_limit > UINT32_MAX || config->memory_limit < 65536)
    {
      return -EINVAL;
    }

  memset(plugin, 0, sizeof(*plugin));
  strlcpy(plugin->id, config->id, sizeof(plugin->id));
  atomic_init(&plugin->permissions, config->permissions);

  ret = ny_wasm_initialize();
  if (ret < 0 || (ret = ny_wasm_read(config->entry, &plugin->binary,
                                     &plugin->binary_size)) < 0)
    {
      return ret;
    }

  if (!wasm_runtime_thread_env_inited())
    {
      if (!wasm_runtime_init_thread_env())
        {
          ny_wasm_destroy(plugin);
          return -EIO;
        }

      plugin->thread_initialized = true;
    }

  plugin->module = wasm_runtime_load(plugin->binary, plugin->binary_size,
                                     error, sizeof(error));
  if (plugin->module == NULL)
    {
      fprintf(stderr, "nycore: wasm load failed: %s\n", error);
      ny_wasm_destroy(plugin);
      return -ENOEXEC;
    }

  memset(&arguments, 0, sizeof(arguments));
  arguments.default_stack_size = (uint32_t)config->stack_limit;
  arguments.host_managed_heap_size = 65536;
  arguments.max_memory_pages = (uint32_t)(config->memory_limit / (64 * 1024));
  plugin->instance = wasm_runtime_instantiate_ex(plugin->module, &arguments,
                                                 error, sizeof(error));
  if (plugin->instance == NULL)
    {
      fprintf(stderr, "nycore: wasm instantiate failed: %s\n", error);
      ny_wasm_destroy(plugin);
      return -ENOMEM;
    }

  plugin->environment = wasm_runtime_create_exec_env(
      plugin->instance, (uint32_t)config->stack_limit);
  if (plugin->environment == NULL)
    {
      ny_wasm_destroy(plugin);
      return -ENOMEM;
    }

  wasm_runtime_set_user_data(plugin->environment, plugin);
  return 0;
}

int ny_wasm_start(struct ny_wasm_plugin_s *plugin)
{
  return plugin == NULL || plugin->environment == NULL
             ? -EINVAL
             : ny_wasm_call(plugin->instance, plugin->environment,
                            "ny_on_start", true);
}

int ny_wasm_dispatch(struct ny_wasm_plugin_s *plugin, const char *event)
{
  return plugin == NULL || plugin->environment == NULL || event == NULL
             ? -EINVAL
             : ny_wasm_call_event(plugin, event);
}

int ny_wasm_stop(struct ny_wasm_plugin_s *plugin)
{
  return plugin == NULL || plugin->environment == NULL
             ? -EINVAL
             : ny_wasm_call(plugin->instance, plugin->environment,
                            "ny_on_stop", false);
}

void ny_wasm_set_permissions(struct ny_wasm_plugin_s *plugin,
                             uint64_t permissions)
{
  if (plugin != NULL)
    {
      atomic_store(&plugin->permissions, permissions);
    }
}

void ny_wasm_destroy(struct ny_wasm_plugin_s *plugin)
{
  if (plugin == NULL)
    {
      return;
    }

  if (plugin->environment != NULL)
    {
      wasm_runtime_destroy_exec_env(plugin->environment);
    }

  if (plugin->instance != NULL)
    {
      wasm_runtime_deinstantiate(plugin->instance);
    }

  if (plugin->module != NULL)
    {
      wasm_runtime_unload(plugin->module);
    }

  if (plugin->thread_initialized)
    {
      wasm_runtime_destroy_thread_env();
    }

  free(plugin->binary);
  memset(plugin, 0, sizeof(*plugin));
}
