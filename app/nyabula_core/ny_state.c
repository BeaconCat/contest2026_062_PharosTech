/****************************************************************************
 * packages/demos/contest2026_062_nyabula_core/ny_state.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to you under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/mutex.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include "ny_state.h"

#define NY_STATE_VALUE_LIMIT (1024 * 1024)
#define NY_STATE_BATCH_LIMIT 8

static mutex_t g_state_lock = NXMUTEX_INITIALIZER;

static int ny_state_error(int result);
static int ny_state_directory(void);
static int ny_state_open(sqlite3 **database);
static int ny_state_key(const char *key);
static int ny_state_update(sqlite3 *database,
                           const struct ny_state_update_s *updates,
                           size_t count);

static int ny_state_error(int result)
{
  switch (result & 0xff)
    {
      case SQLITE_OK:
      case SQLITE_DONE:
      case SQLITE_ROW:
        return 0;
      case SQLITE_NOMEM:
        return -ENOMEM;
      case SQLITE_FULL:
      case SQLITE_TOOBIG:
        return -ENOSPC;
      case SQLITE_BUSY:
      case SQLITE_LOCKED:
        return -EBUSY;
      case SQLITE_READONLY:
      case SQLITE_PERM:
      case SQLITE_AUTH:
        return -EACCES;
      case SQLITE_CORRUPT:
      case SQLITE_NOTADB:
        return -EBADMSG;
      default:
        return -EIO;
    }
}

static int ny_state_key(const char *key)
{
  return key != NULL && key[0] != '\0' && strlen(key) < PATH_MAX ? 0 : -EINVAL;
}

static int ny_state_directory(void)
{
  char path[PATH_MAX];
  struct stat status;
  char *cursor;
  int fd;

  if (strlcpy(path, CONFIG_NYABULA_CORE_STATE_DATABASE, sizeof(path)) >=
          sizeof(path) ||
      path[0] != '/')
    {
      return -EINVAL;
    }

  for (cursor = path + 1; *cursor != '\0'; cursor++)
    {
      if (*cursor != '/')
        {
          continue;
        }

      *cursor = '\0';
      if (mkdir(path, 0700) < 0 && errno != EEXIST)
        {
          return -errno;
        }

      if (lstat(path, &status) < 0 || !S_ISDIR(status.st_mode))
        {
          return -ENOTDIR;
        }

      *cursor = '/';
    }

  if (lstat(path, &status) == 0)
    {
      return S_ISREG(status.st_mode) ? 0 : -EINVAL;
    }

  if (errno != ENOENT)
    {
      return -errno;
    }

  fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    {
      return -errno;
    }

  return close(fd) < 0 ? -errno : 0;
}

static int ny_state_open(sqlite3 **database)
{
  sqlite3_stmt *version = NULL;
  int ret = ny_state_directory();
  int result;

  *database = NULL;
  if (ret < 0)
    {
      return ret;
    }

  /* unix-none is safe only under the single-owner broker contract and
   * g_state_lock. No other process may open this private database.
   */

  result = sqlite3_open_v2(CONFIG_NYABULA_CORE_STATE_DATABASE, database,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_NOFOLLOW,
                           "unix-none");
  if (result != SQLITE_OK)
    {
      return ny_state_error(result);
    }

  sqlite3_limit(*database, SQLITE_LIMIT_LENGTH, NY_STATE_VALUE_LIMIT + 8192);
  sqlite3_limit(*database, SQLITE_LIMIT_SQL_LENGTH, 4096);
  result =
      sqlite3_prepare_v2(*database, "PRAGMA user_version", -1, &version, NULL);
  if (result == SQLITE_OK)
    {
      result = sqlite3_step(version);
    }

  if (result != SQLITE_ROW || (sqlite3_column_int(version, 0) != 0 &&
                               sqlite3_column_int(version, 0) != 1))
    {
      sqlite3_finalize(version);
      return result == SQLITE_ROW ? -EPROTONOSUPPORT : ny_state_error(result);
    }

  sqlite3_finalize(version);
  result = sqlite3_exec(
      *database,
      "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
      "PRAGMA temp_store=MEMORY; PRAGMA max_page_count=1024;"
      "CREATE TABLE IF NOT EXISTS state(k TEXT PRIMARY KEY,v BLOB NOT NULL);"
      "PRAGMA user_version=1;",
      NULL, NULL, NULL);
  return ny_state_error(result);
}

int ny_state_read(const char *key, char **value, size_t *length, size_t limit)
{
  sqlite3 *database = NULL;
  sqlite3_stmt *statement = NULL;
  const void *data;
  int size;
  int result;
  int ret;

  if (value == NULL || length == NULL)
    {
      return -EINVAL;
    }

  *value = NULL;
  *length = 0;
  if (ny_state_key(key) < 0 || limit > NY_STATE_VALUE_LIMIT)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_state_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_state_open(&database);
  if (ret < 0)
    {
      goto out;
    }

  result = sqlite3_prepare_v2(database, "SELECT v FROM state WHERE k=?1", -1,
                              &statement, NULL);
  if (result == SQLITE_OK)
    {
      result = sqlite3_bind_text(statement, 1, key, -1, SQLITE_STATIC);
    }

  if (result == SQLITE_OK)
    {
      result = sqlite3_step(statement);
    }

  if (result != SQLITE_ROW)
    {
      ret = result == SQLITE_DONE ? -ENOENT : ny_state_error(result);
      goto out;
    }

  size = sqlite3_column_bytes(statement, 0);
  if (size < 0 || (size_t)size > limit)
    {
      ret = -EFBIG;
      goto out;
    }

  data = sqlite3_column_blob(statement, 0);
  if (size != 0 && data == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  *value = malloc((size_t)size + 1);
  if (*value == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (size != 0)
    {
      memcpy(*value, data, size);
    }

  (*value)[size] = '\0';
  *length = size;
  ret = 0;
out:
  sqlite3_finalize(statement);
  sqlite3_close(database);
  nxmutex_unlock(&g_state_lock);
  return ret;
}

int ny_state_write(const char *key, const void *value, size_t length)
{
  const struct ny_state_update_s update = { key, value, length };
  return ny_state_write_many(&update, 1);
}

static int ny_state_update(sqlite3 *database,
                           const struct ny_state_update_s *updates,
                           size_t count)
{
  sqlite3_stmt *statement = NULL;
  size_t index;
  int result;

  result = sqlite3_prepare_v2(database,
                              "INSERT INTO state(k,v) VALUES(?1,?2) "
                              "ON CONFLICT(k) DO UPDATE SET v=excluded.v",
                              -1, &statement, NULL);
  for (index = 0; result == SQLITE_OK && index < count; index++)
    {
      result = sqlite3_bind_text(statement, 1, updates[index].key, -1,
                                 SQLITE_STATIC);
      if (result == SQLITE_OK)
        {
          result = sqlite3_bind_blob(
              statement, 2,
              updates[index].value == NULL ? "" : updates[index].value,
              (int)updates[index].length, SQLITE_STATIC);
        }

      if (result == SQLITE_OK)
        {
          result = sqlite3_step(statement);
        }

      if (result == SQLITE_DONE)
        {
          result = sqlite3_reset(statement);
        }
    }

  sqlite3_finalize(statement);
  return ny_state_error(result);
}

int ny_state_write_many(const struct ny_state_update_s *updates, size_t count)
{
  sqlite3 *database = NULL;
  size_t index;
  int result;
  int ret;

  if (updates == NULL || count == 0 || count > NY_STATE_BATCH_LIMIT)
    {
      return -EINVAL;
    }

  for (index = 0; index < count; index++)
    {
      if (ny_state_key(updates[index].key) < 0 ||
          (updates[index].value == NULL && updates[index].length != 0) ||
          updates[index].length > NY_STATE_VALUE_LIMIT)
        {
          return -EINVAL;
        }
    }

  ret = nxmutex_lock(&g_state_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = ny_state_open(&database);
  if (ret < 0)
    {
      goto out;
    }

  result = sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  if (result != SQLITE_OK)
    {
      ret = ny_state_error(result);
      goto out;
    }

  ret = ny_state_update(database, updates, count);
  if (ret == 0)
    {
      result = sqlite3_exec(database, "COMMIT", NULL, NULL, NULL);
      ret = ny_state_error(result);
    }

  if (ret < 0)
    {
      sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
    }

out:
  sqlite3_close(database);
  nxmutex_unlock(&g_state_lock);
  return ret;
}
