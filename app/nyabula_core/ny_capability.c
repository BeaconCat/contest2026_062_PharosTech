/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_capability.c
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
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ny_capability.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ny_capability_binding_s
{
  const char *name;
  JSCFunction *function;
  int length;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static struct ny_plugin_s *ny_capability_plugin(JSContext *context);
static bool ny_capability_has(struct ny_plugin_s *plugin, uint64_t permission);
static JSValue ny_capability_denied(JSContext *context,
                                    const char *permission);
static bool ny_capability_valid_key(const char *key);
static int ny_capability_data_path(struct ny_plugin_s *plugin, const char *key,
                                   char *path, size_t size);
static int ny_capability_write_all(int fd, const char *buffer, size_t length);
static JSValue ny_capability_core_info(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv);
static JSValue ny_capability_core_log(JSContext *context,
                                      JSValueConst this_value, int argc,
                                      JSValueConst *argv);
static JSValue ny_capability_storage_get(JSContext *context,
                                         JSValueConst this_value, int argc,
                                         JSValueConst *argv);
static JSValue ny_capability_storage_put(JSContext *context,
                                         JSValueConst this_value, int argc,
                                         JSValueConst *argv);
static JSValue ny_capability_network_request(JSContext *context,
                                             JSValueConst this_value, int argc,
                                             JSValueConst *argv);
static JSValue ny_capability_ui_notify(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv);
static JSValue ny_capability_ai_invoke(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv);
static int
ny_capability_add_namespace(JSContext *context, JSValue root, const char *name,
                            const struct ny_capability_binding_s *bindings,
                            size_t count);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static struct ny_plugin_s *ny_capability_plugin(JSContext *context)
{
  return JS_GetContextOpaque(context);
}

static bool ny_capability_has(struct ny_plugin_s *plugin, uint64_t permission)
{
  return plugin != NULL && (plugin->permissions & permission) == permission;
}

static JSValue ny_capability_denied(JSContext *context, const char *permission)
{
  return JS_ThrowTypeError(context, "permission denied: %s", permission);
}

static bool ny_capability_valid_key(const char *key)
{
  size_t index;
  size_t length = strlen(key);

  if (length == 0 || length >= 64 || strcmp(key, ".") == 0 ||
      strcmp(key, "..") == 0)
    {
      return false;
    }

  for (index = 0; index < length; index++)
    {
      char value = key[index];

      if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '.' || value == '_' ||
            value == '-'))
        {
          return false;
        }
    }

  return true;
}

static int ny_capability_data_path(struct ny_plugin_s *plugin, const char *key,
                                   char *path, size_t size)
{
  struct stat status;
  char directory[PATH_MAX];
  int ret;

  if (plugin->root[0] == '\0' || !ny_capability_valid_key(key))
    {
      return -EINVAL;
    }

  ret = snprintf(directory, sizeof(directory), "%s/data", plugin->root);
  if (ret < 0 || ret >= (int)sizeof(directory))
    {
      return -ENAMETOOLONG;
    }

  if (mkdir(directory, 0770) < 0 && errno != EEXIST)
    {
      return -errno;
    }

  if (lstat(directory, &status) < 0 || !S_ISDIR(status.st_mode))
    {
      return -ENOTDIR;
    }

  ret = snprintf(path, size, "%s/%s", directory, key);
  return ret < 0 || ret >= (int)size ? -ENAMETOOLONG : 0;
}

static int ny_capability_write_all(int fd, const char *buffer, size_t length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t count = write(fd, buffer + offset, length - offset);

      if (count < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (count == 0)
        {
          return -EIO;
        }

      offset += (size_t)count;
    }

  return 0;
}

static JSValue ny_capability_core_info(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);
  JSValue result = JS_NewObject(context);

  if (JS_IsException(result))
    {
      return result;
    }

  JS_SetPropertyStr(context, result, "id", JS_NewString(context, plugin->id));
  JS_SetPropertyStr(context, result, "version",
                    JS_NewString(context, plugin->version));
  JS_SetPropertyStr(context, result, "apiVersion",
                    JS_NewInt32(context, NY_PLUGIN_API_VERSION));
  return result;
}

static JSValue ny_capability_core_log(JSContext *context,
                                      JSValueConst this_value, int argc,
                                      JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);
  int index;

  if (!ny_capability_has(plugin, NY_PERMISSION_CORE_LOG))
    {
      return ny_capability_denied(context, "core.log");
    }

  printf("nyplugin[%s]:", plugin->id);
  for (index = 0; index < argc; index++)
    {
      const char *text = JS_ToCString(context, argv[index]);

      if (text == NULL)
        {
          return JS_EXCEPTION;
        }

      printf(" %s", text);
      JS_FreeCString(context, text);
    }

  putchar('\n');
  return JS_UNDEFINED;
}

static JSValue ny_capability_storage_get(JSContext *context,
                                         JSValueConst this_value, int argc,
                                         JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);
  char path[PATH_MAX];
  char *buffer;
  const char *key;
  JSValue result;
  ssize_t count;
  int fd;
  int ret;

  if (!ny_capability_has(plugin, NY_PERMISSION_STORAGE_READ))
    {
      return ny_capability_denied(context, "storage.read");
    }

  if (argc != 1 || (key = JS_ToCString(context, argv[0])) == NULL)
    {
      return JS_ThrowTypeError(context, "storage.get requires one key");
    }

  ret = ny_capability_data_path(plugin, key, path, sizeof(path));
  JS_FreeCString(context, key);
  if (ret < 0)
    {
      return JS_ThrowTypeError(context, "invalid storage key");
    }

  fd = open(path, O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    {
      return errno == ENOENT
                 ? JS_NULL
                 : JS_ThrowInternalError(context, "storage read failed");
    }

  buffer = malloc(CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT + 1);
  if (buffer == NULL)
    {
      close(fd);
      return JS_ThrowOutOfMemory(context);
    }

  count = read(fd, buffer, CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT + 1);
  close(fd);
  if (count < 0 || count > CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT)
    {
      free(buffer);
      return JS_ThrowInternalError(context, "storage value invalid");
    }

  buffer[count] = '\0';
  result = JS_NewStringLen(context, buffer, (size_t)count);
  free(buffer);
  return result;
}

static JSValue ny_capability_storage_put(JSContext *context,
                                         JSValueConst this_value, int argc,
                                         JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);
  char path[PATH_MAX];
  const char *key;
  const char *value;
  size_t length;
  int fd;
  int ret;

  if (!ny_capability_has(plugin, NY_PERMISSION_STORAGE_WRITE))
    {
      return ny_capability_denied(context, "storage.write");
    }

  if (argc != 2 || (key = JS_ToCString(context, argv[0])) == NULL)
    {
      return JS_ThrowTypeError(context, "storage.put requires key and value");
    }

  value = JS_ToCStringLen(context, &length, argv[1]);
  if (value == NULL)
    {
      JS_FreeCString(context, key);
      return JS_EXCEPTION;
    }

  ret = ny_capability_data_path(plugin, key, path, sizeof(path));
  JS_FreeCString(context, key);
  if (ret < 0)
    {
      JS_FreeCString(context, value);
      return JS_ThrowTypeError(context, "invalid storage key");
    }

  if (length > CONFIG_NYABULA_CORE_STORAGE_VALUE_LIMIT)
    {
      JS_FreeCString(context, value);
      return JS_ThrowRangeError(context, "storage value too large");
    }

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0660);
  if (fd < 0)
    {
      JS_FreeCString(context, value);
      return JS_ThrowInternalError(context, "storage write failed");
    }

  ret = ny_capability_write_all(fd, value, length);
  close(fd);
  JS_FreeCString(context, value);
  return ret < 0 ? JS_ThrowInternalError(context, "storage write failed")
                 : JS_UNDEFINED;
}

static JSValue ny_capability_network_request(JSContext *context,
                                             JSValueConst this_value, int argc,
                                             JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);

  if (!ny_capability_has(plugin, NY_PERMISSION_NETWORK_REQUEST))
    {
      return ny_capability_denied(context, "network.request");
    }

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  if (argc == 1)
    {
      return JS_DupValue(context, argv[0]);
    }

  return JS_ThrowTypeError(context, "network.request requires one request");
#else
  return JS_ThrowInternalError(context, "network broker unavailable");
#endif
}

static JSValue ny_capability_ui_notify(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);
  const char *message;

  if (!ny_capability_has(plugin, NY_PERMISSION_UI_NOTIFY))
    {
      return ny_capability_denied(context, "ui.notify");
    }

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  if (argc != 1 || (message = JS_ToCString(context, argv[0])) == NULL)
    {
      return JS_ThrowTypeError(context, "ui.notify requires one message");
    }

  printf("nymock-ui[%s]: %s\n", plugin->id, message);
  JS_FreeCString(context, message);
  return JS_UNDEFINED;
#else
  return JS_ThrowInternalError(context, "UI broker unavailable");
#endif
}

static JSValue ny_capability_ai_invoke(JSContext *context,
                                       JSValueConst this_value, int argc,
                                       JSValueConst *argv)
{
  struct ny_plugin_s *plugin = ny_capability_plugin(context);

  if (!ny_capability_has(plugin, NY_PERMISSION_AI_INVOKE))
    {
      return ny_capability_denied(context, "ai.invoke");
    }

#ifdef CONFIG_NYABULA_CORE_MOCK_CAPABILITIES
  if (argc == 1)
    {
      return JS_DupValue(context, argv[0]);
    }

  return JS_ThrowTypeError(context, "ai.invoke requires one prompt");
#else
  return JS_ThrowInternalError(context, "AI broker unavailable");
#endif
}

static int
ny_capability_add_namespace(JSContext *context, JSValue root, const char *name,
                            const struct ny_capability_binding_s *bindings,
                            size_t count)
{
  JSValue object = JS_NewObject(context);
  size_t index;

  if (JS_IsException(object))
    {
      return -ENOMEM;
    }

  for (index = 0; index < count; index++)
    {
      JSValue function =
          JS_NewCFunction(context, bindings[index].function,
                          bindings[index].name, bindings[index].length);

      if (JS_IsException(function))
        {
          JS_FreeValue(context, object);
          return -ENOMEM;
        }

      if (JS_SetPropertyStr(context, object, bindings[index].name, function) <
          0)
        {
          JS_FreeValue(context, object);
          return -EFAULT;
        }
    }

  if (JS_SetPropertyStr(context, root, name, object) < 0)
    {
      return -EFAULT;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_capability_register(struct ny_plugin_s *plugin)
{
  static const struct ny_capability_binding_s core_bindings[] = {
    { "info", ny_capability_core_info, 0 },
    { "log", ny_capability_core_log, 1 },
  };
  static const struct ny_capability_binding_s storage_bindings[] = {
    { "get", ny_capability_storage_get, 1 },
    { "put", ny_capability_storage_put, 2 },
  };
  static const struct ny_capability_binding_s network_bindings[] = {
    { "request", ny_capability_network_request, 1 },
  };
  static const struct ny_capability_binding_s ui_bindings[] = {
    { "notify", ny_capability_ui_notify, 1 },
  };
  static const struct ny_capability_binding_s ai_bindings[] = {
    { "invoke", ny_capability_ai_invoke, 1 },
  };
  JSContext *context;
  JSValue global;
  JSValue root;
  JSValue legacy_log;

  if (plugin == NULL || plugin->context == NULL)
    {
      return -EINVAL;
    }

  context = plugin->context;
  root = JS_NewObject(context);
  if (JS_IsException(root))
    {
      return -ENOMEM;
    }

  if (ny_capability_add_namespace(context, root, "core", core_bindings,
                                  sizeof(core_bindings) /
                                      sizeof(core_bindings[0])) < 0 ||
      ny_capability_add_namespace(context, root, "storage", storage_bindings,
                                  sizeof(storage_bindings) /
                                      sizeof(storage_bindings[0])) < 0 ||
      ny_capability_add_namespace(context, root, "network", network_bindings,
                                  sizeof(network_bindings) /
                                      sizeof(network_bindings[0])) < 0 ||
      ny_capability_add_namespace(context, root, "ui", ui_bindings,
                                  sizeof(ui_bindings) /
                                      sizeof(ui_bindings[0])) < 0 ||
      ny_capability_add_namespace(context, root, "ai", ai_bindings,
                                  sizeof(ai_bindings) /
                                      sizeof(ai_bindings[0])) < 0)
    {
      JS_FreeValue(context, root);
      return -EFAULT;
    }

  global = JS_GetGlobalObject(context);
  legacy_log = JS_NewCFunction(context, ny_capability_core_log, "nyLog", 1);
  if (JS_IsException(global) || JS_IsException(legacy_log))
    {
      JS_FreeValue(context, global);
      JS_FreeValue(context, legacy_log);
      JS_FreeValue(context, root);
      return -ENOMEM;
    }

  if (JS_SetPropertyStr(context, global, "nyLog", legacy_log) < 0)
    {
      JS_FreeValue(context, root);
      JS_FreeValue(context, global);
      return -EFAULT;
    }

  if (JS_SetPropertyStr(context, global, "ny", root) < 0)
    {
      JS_FreeValue(context, global);
      return -EFAULT;
    }

  JS_FreeValue(context, global);
  return 0;
}
