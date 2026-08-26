/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_scheduler.c
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
#include <sched.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ny_permission.h"
#include "ny_runtime.h"
#include "ny_scheduler.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define NY_PLUGIN_ID_SIZE 64

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum ny_scheduler_event_type_e
{
  NY_SCHEDULER_EVENT_USER = 0,
  NY_SCHEDULER_EVENT_STOP
};

struct ny_scheduler_event_s
{
  enum ny_scheduler_event_type_e type;
  char payload[CONFIG_NYABULA_CORE_EVENT_SIZE];
};

struct ny_scheduler_slot_s
{
  bool occupied;
  bool stopping;
  bool task_stopped;
  pid_t pid;
  int start_result;
  int exit_result;
  enum ny_plugin_state_e state;
  struct ny_plugin_config_s config;
  char id[NY_PLUGIN_ID_SIZE];
  char path[PATH_MAX];
  char index_arg[12];
  sem_t ready;
  sem_t pending;
  sem_t stopped;
  size_t head;
  size_t tail;
  size_t count;
  struct ny_scheduler_event_s events[CONFIG_NYABULA_CORE_EVENT_DEPTH];
  struct ny_plugin_s plugin;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static mutex_t g_scheduler_lock = NXMUTEX_INITIALIZER;
static struct ny_scheduler_slot_s
    g_scheduler_slots[CONFIG_NYABULA_CORE_MAX_PLUGINS];

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int ny_scheduler_wait(sem_t *sem);
static const char *ny_scheduler_state_name(enum ny_plugin_state_e state);
static struct ny_scheduler_slot_s *ny_scheduler_find_locked(const char *id);
static struct ny_scheduler_slot_s *ny_scheduler_empty_locked(void);
static int ny_scheduler_enqueue_locked(struct ny_scheduler_slot_s *slot,
                                       enum ny_scheduler_event_type_e type,
                                       const char *payload);
static int ny_scheduler_worker(int argc, char *argv[]);
static void ny_scheduler_clear_locked(struct ny_scheduler_slot_s *slot);
static int ny_scheduler_start_config(const struct ny_plugin_config_s *config);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int ny_scheduler_wait(sem_t *sem)
{
  int ret;

  do
    {
      ret = sem_wait(sem);
    }
  while (ret < 0 && errno == EINTR);

  return ret < 0 ? -errno : 0;
}

static const char *ny_scheduler_state_name(enum ny_plugin_state_e state)
{
  switch (state)
    {
      case NY_PLUGIN_EMPTY:
        return "empty";
      case NY_PLUGIN_LOADED:
        return "loaded";
      case NY_PLUGIN_RUNNING:
        return "running";
      case NY_PLUGIN_STOPPED:
        return "stopped";
      case NY_PLUGIN_FAILED:
        return "failed";
      default:
        return "unknown";
    }
}

static struct ny_scheduler_slot_s *ny_scheduler_find_locked(const char *id)
{
  int index;

  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_scheduler_slots[index].occupied &&
          strcmp(g_scheduler_slots[index].id, id) == 0)
        {
          return &g_scheduler_slots[index];
        }
    }

  return NULL;
}

static struct ny_scheduler_slot_s *ny_scheduler_empty_locked(void)
{
  int index;

  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (!g_scheduler_slots[index].occupied)
        {
          return &g_scheduler_slots[index];
        }
    }

  return NULL;
}

static int ny_scheduler_enqueue_locked(struct ny_scheduler_slot_s *slot,
                                       enum ny_scheduler_event_type_e type,
                                       const char *payload)
{
  struct ny_scheduler_event_s *event;

  if (slot->count >= CONFIG_NYABULA_CORE_EVENT_DEPTH)
    {
      return -EAGAIN;
    }

  event = &slot->events[slot->tail];
  memset(event, 0, sizeof(*event));
  event->type = type;
  if (payload != NULL)
    {
      strlcpy(event->payload, payload, sizeof(event->payload));
    }

  slot->tail = (slot->tail + 1) % CONFIG_NYABULA_CORE_EVENT_DEPTH;
  slot->count++;
  sem_post(&slot->pending);
  return 0;
}

static int ny_scheduler_worker(int argc, char *argv[])
{
  struct ny_scheduler_event_s event;
  struct ny_scheduler_slot_s *slot;
  int index;
  int ret;

  if (argc < 2)
    {
      return EXIT_FAILURE;
    }

  index = atoi(argv[1]);
  if (index < 0 || index >= CONFIG_NYABULA_CORE_MAX_PLUGINS)
    {
      return EXIT_FAILURE;
    }

  slot = &g_scheduler_slots[index];
  ret = ny_plugin_load_config(&slot->plugin, &slot->config);
  if (ret >= 0)
    {
      ret = ny_plugin_start(&slot->plugin);
    }

  nxmutex_lock(&g_scheduler_lock);
  slot->start_result = ret;
  slot->state = slot->plugin.state;
  nxmutex_unlock(&g_scheduler_lock);
  sem_post(&slot->ready);

  while (ret >= 0)
    {
      ret = ny_scheduler_wait(&slot->pending);
      if (ret < 0)
        {
          break;
        }

      nxmutex_lock(&g_scheduler_lock);
      if (slot->count == 0)
        {
          nxmutex_unlock(&g_scheduler_lock);
          continue;
        }

      event = slot->events[slot->head];
      slot->head = (slot->head + 1) % CONFIG_NYABULA_CORE_EVENT_DEPTH;
      slot->count--;
      nxmutex_unlock(&g_scheduler_lock);

      if (event.type == NY_SCHEDULER_EVENT_STOP)
        {
          ret = ny_plugin_stop(&slot->plugin);
          break;
        }

      ret = ny_plugin_dispatch(&slot->plugin, event.payload);
      nxmutex_lock(&g_scheduler_lock);
      slot->state = slot->plugin.state;
      nxmutex_unlock(&g_scheduler_lock);
    }

  nxmutex_lock(&g_scheduler_lock);
  slot->exit_result = ret;
  slot->state = ret < 0 ? NY_PLUGIN_FAILED : slot->plugin.state;
  nxmutex_unlock(&g_scheduler_lock);

  ny_plugin_destroy(&slot->plugin);

  nxmutex_lock(&g_scheduler_lock);
  slot->task_stopped = true;
  nxmutex_unlock(&g_scheduler_lock);
  sem_post(&slot->stopped);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void ny_scheduler_clear_locked(struct ny_scheduler_slot_s *slot)
{
  sem_destroy(&slot->ready);
  sem_destroy(&slot->pending);
  sem_destroy(&slot->stopped);
  memset(slot, 0, sizeof(*slot));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ny_scheduler_start(const char *id, const char *path)
{
  struct ny_plugin_config_s config;

  if (id == NULL || path == NULL || strlen(id) >= sizeof(config.id))
    {
      return -EINVAL;
    }

  ny_manifest_default_config(&config, path);
  strlcpy(config.id, id, sizeof(config.id));
  return ny_scheduler_start_config(&config);
}

int ny_scheduler_start_package(const char *package_path)
{
  struct ny_plugin_config_s config;
  int ret;

  ret = ny_manifest_load(package_path, &config);
  return ret < 0 ? ret : ny_scheduler_start_config(&config);
}

int ny_scheduler_refresh_permissions(const char *id)
{
  struct ny_scheduler_slot_s *slot;
  struct ny_plugin_config_s config;
  pid_t pid;
  int ret;

  if (id == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return 0;
    }

  config = slot->config;
  pid = slot->pid;
  nxmutex_unlock(&g_scheduler_lock);

  ret = ny_permission_apply(&config);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot != NULL && slot->pid == pid)
    {
      slot->config.permissions = config.permissions;
      atomic_store(&slot->plugin.permissions, config.permissions);
    }

  nxmutex_unlock(&g_scheduler_lock);
  return 0;
}

/****************************************************************************

 * * Private Functions

 * ****************************************************************************/

static int ny_scheduler_start_config(const struct ny_plugin_config_s *config)
{
  struct ny_scheduler_slot_s *slot;
  char *worker_argv[2];
  int index;
  int ret;

  if (config == NULL || config->id[0] == '\0' || config->entry[0] == '\0' ||
      strlen(config->id) >= NY_PLUGIN_ID_SIZE ||
      strlen(config->entry) >= PATH_MAX)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  if (ny_scheduler_find_locked(config->id) != NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EEXIST;
    }

  slot = ny_scheduler_empty_locked();
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ENOSPC;
    }

  index = slot - g_scheduler_slots;
  memset(slot, 0, sizeof(*slot));
  slot->occupied = true;
  slot->state = NY_PLUGIN_EMPTY;
  slot->config = *config;
  strlcpy(slot->id, config->id, sizeof(slot->id));
  strlcpy(slot->path, config->entry, sizeof(slot->path));
  snprintf(slot->index_arg, sizeof(slot->index_arg), "%d", index);

  if (sem_init(&slot->ready, 0, 0) < 0)
    {
      ret = -errno;
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  if (sem_init(&slot->pending, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&slot->ready);
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  if (sem_init(&slot->stopped, 0, 0) < 0)
    {
      ret = -errno;
      sem_destroy(&slot->pending);
      sem_destroy(&slot->ready);
      memset(slot, 0, sizeof(*slot));
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  worker_argv[0] = slot->index_arg;
  worker_argv[1] = NULL;
  slot->pid = task_create(config->id, CONFIG_NYABULA_CORE_PRIORITY,
                          CONFIG_NYABULA_CORE_STACKSIZE, ny_scheduler_worker,
                          worker_argv);
  if (slot->pid < 0)
    {
      ret = -errno;
      ny_scheduler_clear_locked(slot);
      nxmutex_unlock(&g_scheduler_lock);
      return ret;
    }

  nxmutex_unlock(&g_scheduler_lock);
  ret = ny_scheduler_wait(&slot->ready);
  if (ret < 0)
    {
      return ret;
    }

  nxmutex_lock(&g_scheduler_lock);
  ret = slot->start_result;
  nxmutex_unlock(&g_scheduler_lock);
  if (ret < 0)
    {
      ny_scheduler_wait(&slot->stopped);
      nxmutex_lock(&g_scheduler_lock);
      ny_scheduler_clear_locked(slot);
      nxmutex_unlock(&g_scheduler_lock);
    }

  return ret;
}

/****************************************************************************

 * * Public Functions

 * ****************************************************************************/

int ny_scheduler_dispatch(const char *id, const char *event)
{
  struct ny_scheduler_slot_s *slot;
  int ret;

  if (id == NULL || event == NULL ||
      strlen(event) >= CONFIG_NYABULA_CORE_EVENT_SIZE)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      ret = -ENOENT;
    }
  else if (slot->state != NY_PLUGIN_RUNNING || slot->stopping)
    {
      ret = -EPIPE;
    }
  else
    {
      ret = ny_scheduler_enqueue_locked(slot, NY_SCHEDULER_EVENT_USER, event);
    }

  nxmutex_unlock(&g_scheduler_lock);
  return ret;
}

int ny_scheduler_stop(const char *id)
{
  struct ny_scheduler_slot_s *slot;
  bool already_stopped;
  int ret;

  if (id == NULL)
    {
      return -EINVAL;
    }

  nxmutex_lock(&g_scheduler_lock);
  slot = ny_scheduler_find_locked(id);
  if (slot == NULL)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -ENOENT;
    }

  if (slot->stopping)
    {
      nxmutex_unlock(&g_scheduler_lock);
      return -EBUSY;
    }

  already_stopped = slot->task_stopped;
  ret = already_stopped
            ? 0
            : ny_scheduler_enqueue_locked(slot, NY_SCHEDULER_EVENT_STOP, NULL);
  if (ret >= 0)
    {
      slot->stopping = true;
    }

  nxmutex_unlock(&g_scheduler_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!already_stopped)
    {
      ret = ny_scheduler_wait(&slot->stopped);
      if (ret < 0)
        {
          return ret;
        }
    }

  nxmutex_lock(&g_scheduler_lock);
  ny_scheduler_clear_locked(slot);
  nxmutex_unlock(&g_scheduler_lock);
  return 0;
}

int ny_scheduler_stop_all(void)
{
  char ids[CONFIG_NYABULA_CORE_MAX_PLUGINS][NY_PLUGIN_ID_SIZE];
  int count = 0;
  int index;
  int ret = 0;

  nxmutex_lock(&g_scheduler_lock);
  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      if (g_scheduler_slots[index].occupied)
        {
          strlcpy(ids[count++], g_scheduler_slots[index].id,
                  NY_PLUGIN_ID_SIZE);
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
  for (index = 0; index < count; index++)
    {
      int stop_ret = ny_scheduler_stop(ids[index]);

      if (stop_ret < 0 && ret == 0)
        {
          ret = stop_ret;
        }
    }

  return ret;
}

void ny_scheduler_list(void)
{
  int index;

  nxmutex_lock(&g_scheduler_lock);
  printf("ID\tSTATE\tPID\tQUEUED\tPATH\n");
  for (index = 0; index < CONFIG_NYABULA_CORE_MAX_PLUGINS; index++)
    {
      struct ny_scheduler_slot_s *slot = &g_scheduler_slots[index];

      if (slot->occupied)
        {
          printf("%s\t%s\t%d\t%zu\t%s\n", slot->id,
                 ny_scheduler_state_name(slot->state), slot->pid, slot->count,
                 slot->path);
        }
    }

  nxmutex_unlock(&g_scheduler_lock);
}
