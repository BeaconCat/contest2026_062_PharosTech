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
 * Private Types
 ****************************************************************************/

struct ny_wasm_context_s
{
  const char *id;
  uint64_t permissions;
};

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
  struct ny_wasm_context_s *context;
  wasm_module_inst_t instance;
  const char *message;

  context = wasm_runtime_get_user_data(environment);
  instance = wasm_runtime_get_module_inst(environment);
  if (context == NULL || (context->permissions & NY_PERMISSION_CORE_LOG) == 0)
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
  printf("nyplugin[%s]: %.*s\n", context->id, length, message);
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
  struct ny_wasm_context_s context;
  wasm_module_inst_t instance = NULL;
  wasm_exec_env_t environment = NULL;
  wasm_module_t module = NULL;
  uint8_t *binary = NULL;
  uint32_t size;
  char error[NY_WASM_ERROR_SIZE];
  bool thread_initialized = false;
  int ret;

  if (path == NULL || id == NULL || id[0] == '\0')
    {
      return -EINVAL;
    }

  ret = ny_wasm_initialize();
  if (ret < 0 || (ret = ny_wasm_read(path, &binary, &size)) < 0)
    {
      return ret;
    }

  if (!wasm_runtime_thread_env_inited())
    {
      if (!wasm_runtime_init_thread_env())
        {
          ret = -EIO;
          goto out;
        }

      thread_initialized = true;
    }

  module = wasm_runtime_load(binary, size, error, sizeof(error));
  if (module == NULL)
    {
      fprintf(stderr, "nycore: wasm load failed: %s\n", error);
      ret = -ENOEXEC;
      goto out;
    }

  instance = wasm_runtime_instantiate(module, CONFIG_NYABULA_CORE_PLUGIN_STACK,
                                      65536, error, sizeof(error));
  if (instance == NULL)
    {
      fprintf(stderr, "nycore: wasm instantiate failed: %s\n", error);
      ret = -ENOMEM;
      goto out;
    }

  environment =
      wasm_runtime_create_exec_env(instance, CONFIG_NYABULA_CORE_PLUGIN_STACK);
  if (environment == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  context.id = id;
  context.permissions = permissions;
  wasm_runtime_set_user_data(environment, &context);
  ret = ny_wasm_call(instance, environment, "ny_on_start", true);
  if (ret >= 0)
    {
      ret = ny_wasm_call(instance, environment, "ny_on_stop", false);
    }

out:
  if (environment != NULL)
    {
      wasm_runtime_destroy_exec_env(environment);
    }

  if (instance != NULL)
    {
      wasm_runtime_deinstantiate(instance);
    }

  if (module != NULL)
    {
      wasm_runtime_unload(module);
    }

  if (thread_initialized)
    {
      wasm_runtime_destroy_thread_env();
    }

  free(binary);
  return ret;
}
