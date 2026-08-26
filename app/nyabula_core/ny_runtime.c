/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_runtime.c
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
#include <time.h>

#include "ny_runtime.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t ny_runtime_now_ns(void);
static void ny_runtime_begin_event(struct ny_plugin_s *plugin);
static int ny_runtime_interrupt(JSRuntime *runtime, void *opaque);
static void ny_runtime_dump_exception(struct ny_plugin_s *plugin,
                                      const char *operation);
static JSValue ny_runtime_log(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv);
static int ny_runtime_call(struct ny_plugin_s *plugin, const char *name,
                           int argc, JSValueConst *argv);
static int ny_runtime_read_source(const char *path, char **source,
                                  size_t *length);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint64_t ny_runtime_now_ns(void)
{
  struct timespec now;

  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    {
      return 0;
    }

  return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void ny_runtime_begin_event(struct ny_plugin_s *plugin)
{
  atomic_store(&plugin->cancelled, false);
  plugin->deadline_ns =
      ny_runtime_now_ns() + (uint64_t)plugin->event_timeout_ms * 1000000ull;
}

static int ny_runtime_interrupt(JSRuntime *runtime, void *opaque)
{
  struct ny_plugin_s *plugin = opaque;

  if (atomic_load(&plugin->cancelled))
    {
      return 1;
    }

  return plugin->deadline_ns != 0 &&
         ny_runtime_now_ns() >= plugin->deadline_ns;
}

static void ny_runtime_dump_exception(struct ny_plugin_s *plugin,
                                      const char *operation)
{
  JSValue exception;
  const char *message;

  exception = JS_GetException(plugin->context);
  message = JS_ToCString(plugin->context, exception);
  fprintf(stderr, "nycore: %s failed: %s\n", operation,
          message == NULL ? "unknown exception" : message);

  if (message != NULL)
    {
      JS_FreeCString(plugin->context, message);
    }

  JS_FreeValue(plugin->context, exception);
}

static JSValue ny_runtime_log(JSContext *context, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
  int index;

  for (index = 0; index < argc; index++)
    {
      const char *text = JS_ToCString(context, argv[index]);

      if (text == NULL)
        {
          return JS_EXCEPTION;
        }

      printf("%s%s", index == 0 ? "nyplugin: " : " ", text);
      JS_FreeCString(context, text);
    }

  putchar('\n');
  return JS_UNDEFINED;
}

static int ny_runtime_call(struct ny_plugin_s *plugin, const char *name,
                           int argc, JSValueConst *argv)
{
  JSValue global;
  JSValue function;
  JSValue result;
  int ret = 0;

  global = JS_GetGlobalObject(plugin->context);
  function = JS_GetPropertyStr(plugin->context, global, name);
  if (JS_IsException(function))
    {
      ny_runtime_dump_exception(plugin, name);
      ret = -EFAULT;
      goto out;
    }

  if (JS_IsUndefined(function))
    {
      goto out;
    }

  if (!JS_IsFunction(plugin->context, function))
    {
      fprintf(stderr, "nycore: %s is not a function\n", name);
      ret = -EINVAL;
      goto out;
    }

  ny_runtime_begin_event(plugin);
  result = JS_Call(plugin->context, function, global, argc, argv);
  plugin->deadline_ns = 0;
  if (JS_IsException(result))
    {
      ny_runtime_dump_exception(plugin, name);
      ret = -EFAULT;
    }

  JS_FreeValue(plugin->context, result);

out:
  JS_FreeValue(plugin->context, function);
  JS_FreeValue(plugin->context, global);
  return ret;
}

static int ny_runtime_read_source(const char *path, char **source,
                                  size_t *length)
{
  FILE *stream;
  long size;
  char *buffer;
  size_t count;

  stream = fopen(path, "rb");
  if (stream == NULL)
    {
      return -errno;
    }

  if (fseek(stream, 0, SEEK_END) < 0 || (size = ftell(stream)) < 0 ||
      fseek(stream, 0, SEEK_SET) < 0)
    {
      int error = errno;
      fclose(stream);
      return -error;
    }

  if (size == 0 || size > CONFIG_NYABULA_CORE_SOURCE_LIMIT)
    {
      fclose(stream);
      return -EFBIG;
    }

  buffer = malloc((size_t)size + 1);
  if (buffer == NULL)
    {
      fclose(stream);
      return -ENOMEM;
    }

  count = fread(buffer, 1, (size_t)size, stream);
  fclose(stream);
  if (count != (size_t)size)
    {
      free(buffer);
      return -EIO;
    }

  buffer[count] = '\0';
  *source = buffer;
  *length = count;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_plugin_load(struct ny_plugin_s *plugin, const char *path)
{
  struct ny_plugin_config_s config;

  if (path == NULL)
    {
      return -EINVAL;
    }

  ny_manifest_default_config(&config, path);
  return ny_plugin_load_config(plugin, &config);
}

int ny_plugin_load_config(struct ny_plugin_s *plugin,
                          const struct ny_plugin_config_s *config)
{
  JSValue global;
  JSValue result;
  JSValue log_function;
  char *source;
  size_t length;
  int ret;

  if (plugin == NULL || config == NULL || config->entry[0] == '\0' ||
      config->memory_limit == 0 || config->stack_limit == 0 ||
      config->event_timeout_ms == 0)
    {
      return -EINVAL;
    }

  memset(plugin, 0, sizeof(*plugin));
  plugin->state = NY_PLUGIN_EMPTY;
  atomic_init(&plugin->cancelled, false);
  plugin->permissions = config->permissions;
  plugin->memory_limit = config->memory_limit;
  plugin->stack_limit = config->stack_limit;
  plugin->event_timeout_ms = config->event_timeout_ms;
  strlcpy(plugin->id, config->id, sizeof(plugin->id));
  strlcpy(plugin->root, config->root, sizeof(plugin->root));
  strlcpy(plugin->path, config->entry, sizeof(plugin->path));

  ret = ny_runtime_read_source(plugin->path, &source, &length);
  if (ret < 0)
    {
      return ret;
    }

  plugin->runtime = JS_NewRuntime();
  if (plugin->runtime == NULL)
    {
      free(source);
      return -ENOMEM;
    }

  JS_SetMemoryLimit(plugin->runtime, plugin->memory_limit);
  JS_SetMaxStackSize(plugin->runtime, plugin->stack_limit);
  JS_SetInterruptHandler(plugin->runtime, ny_runtime_interrupt, plugin);

  plugin->context = JS_NewContext(plugin->runtime);
  if (plugin->context == NULL)
    {
      free(source);
      ny_plugin_destroy(plugin);
      return -ENOMEM;
    }

  JS_SetContextOpaque(plugin->context, plugin);
  global = JS_GetGlobalObject(plugin->context);
  log_function = JS_NewCFunction(plugin->context, ny_runtime_log, "nyLog", 1);
  JS_SetPropertyStr(plugin->context, global, "nyLog", log_function);
  JS_FreeValue(plugin->context, global);

  ny_runtime_begin_event(plugin);
  result = JS_Eval(plugin->context, source, length, plugin->path,
                   JS_EVAL_TYPE_GLOBAL);
  plugin->deadline_ns = 0;
  free(source);
  if (JS_IsException(result))
    {
      ny_runtime_dump_exception(plugin, "load");
      JS_FreeValue(plugin->context, result);
      plugin->state = NY_PLUGIN_FAILED;
      return -EFAULT;
    }

  JS_FreeValue(plugin->context, result);
  plugin->state = NY_PLUGIN_LOADED;
  return 0;
}

int ny_plugin_start(struct ny_plugin_s *plugin)
{
  int ret;

  if (plugin == NULL || plugin->state != NY_PLUGIN_LOADED)
    {
      return -EINVAL;
    }

  ret = ny_runtime_call(plugin, "ny_on_start", 0, NULL);
  plugin->state = ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_RUNNING;
  return ret;
}

int ny_plugin_dispatch(struct ny_plugin_s *plugin, const char *event)
{
  JSValue argument;
  int ret;

  if (plugin == NULL || event == NULL || plugin->state != NY_PLUGIN_RUNNING)
    {
      return -EINVAL;
    }

  argument = JS_NewString(plugin->context, event);
  if (JS_IsException(argument))
    {
      ny_runtime_dump_exception(plugin, "event allocation");
      plugin->state = NY_PLUGIN_FAILED;
      return -ENOMEM;
    }

  ret = ny_runtime_call(plugin, "ny_on_event", 1, &argument);
  JS_FreeValue(plugin->context, argument);
  if (ret < 0)
    {
      plugin->state = NY_PLUGIN_FAILED;
    }

  return ret;
}

int ny_plugin_stop(struct ny_plugin_s *plugin)
{
  int ret = 0;

  if (plugin == NULL)
    {
      return -EINVAL;
    }

  if (plugin->state == NY_PLUGIN_RUNNING)
    {
      ret = ny_runtime_call(plugin, "ny_on_stop", 0, NULL);
    }

  atomic_store(&plugin->cancelled, true);
  plugin->deadline_ns = 0;
  plugin->state = ret < 0 ? NY_PLUGIN_FAILED : NY_PLUGIN_STOPPED;
  return ret;
}

void ny_plugin_destroy(struct ny_plugin_s *plugin)
{
  if (plugin == NULL)
    {
      return;
    }

  if (plugin->context != NULL)
    {
      JS_FreeContext(plugin->context);
    }

  if (plugin->runtime != NULL)
    {
      JS_FreeRuntime(plugin->runtime);
    }

  memset(plugin, 0, sizeof(*plugin));
  plugin->state = NY_PLUGIN_EMPTY;
}
