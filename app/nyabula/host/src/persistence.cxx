/****************************************************************************
 * app/nyabula/host/src/persistence.cxx
 *
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
 ****************************************************************************/

#include "nyabula_host/persistence.hxx"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace nyabula
{

JsonFileStore::JsonFileStore(std::filesystem::path path)
    : path_(std::move(path))
{
}

Json JsonFileStore::load() const
{
  std::ifstream input(path_, std::ios::binary);
  if (!input)
    return Json();
  const std::string text(std::istreambuf_iterator<char>(input), {});
  try
    {
      return Json::parse(text);
    }
  catch (...)
    {
      return Json();
    }
}

bool JsonFileStore::save(const Json &value) const
{
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error)
    return false;
  std::filesystem::path temporary = path_;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
      return false;
    output << value.dump();
    output.flush();
    if (!output)
      return false;
  }
  return MoveFileExW(temporary.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

const std::filesystem::path &JsonFileStore::path() const { return path_; }

std::filesystem::path windows_data_root()
{
  if (const char *override_path = std::getenv("NYABULA_DATA_DIR"))
    if (*override_path != '\0')
      return std::filesystem::u8path(override_path);
  if (const char *local = std::getenv("LOCALAPPDATA"))
    if (*local != '\0')
      return std::filesystem::u8path(local) / "Nyabula";
  return std::filesystem::temp_directory_path() / "Nyabula";
}

} // namespace nyabula
