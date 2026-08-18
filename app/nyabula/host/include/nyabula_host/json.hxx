/****************************************************************************
 * app/nyabula/host/include/nyabula_host/json.hxx
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

#ifndef __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_JSON_HXX
#define __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_JSON_HXX

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nyabula
{

class Json
{
public:
  enum class Type
  {
    Null,
    Boolean,
    Number,
    String,
    Array,
    Object
  };

  using Array = std::vector<Json>;
  using Object = std::map<std::string, Json>;

  Json();
  Json(std::nullptr_t);
  Json(bool value);
  Json(int value);
  Json(int64_t value);
  Json(uint64_t value);
  Json(double value);
  Json(const char *value);
  Json(std::string value);
  Json(Array value);
  Json(Object value);

  static Json parse(const std::string &text);
  static Json object();
  static Json array();

  Type type() const;
  bool is_null() const;
  bool is_boolean() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  bool boolean(bool fallback = false) const;
  double number(double fallback = 0.0) const;
  int64_t integer(int64_t fallback = 0) const;
  const std::string &string() const;
  const Array &array_items() const;
  Array &array_items();
  const Object &object_items() const;
  Object &object_items();

  bool contains(const std::string &key) const;
  const Json &at(const std::string &key) const;
  Json &operator[](const std::string &key);
  void push_back(Json value);
  std::string dump() const;

private:
  Type type_;
  bool boolean_;
  double number_;
  std::string string_;
  Array array_;
  Object object_;
};

} // namespace nyabula

#endif
