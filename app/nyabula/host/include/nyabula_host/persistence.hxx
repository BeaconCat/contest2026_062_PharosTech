/****************************************************************************
 * app/nyabula/host/include/nyabula_host/persistence.hxx
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

#ifndef __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_PERSISTENCE_HXX
#define __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_PERSISTENCE_HXX

#include "nyabula_host/json.hxx"

#include <filesystem>

namespace nyabula
{

class JsonFileStore
{
public:
  explicit JsonFileStore(std::filesystem::path path);
  Json load() const;
  bool save(const Json &value) const;
  const std::filesystem::path &path() const;

private:
  std::filesystem::path path_;
};

std::filesystem::path windows_data_root();

} // namespace nyabula

#endif
