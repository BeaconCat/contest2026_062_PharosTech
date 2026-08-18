/****************************************************************************
 * app/nyabula/host/src/json.cxx
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

#include "nyabula_host/json.hxx"

#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace nyabula
{
namespace
{

class Parser
{
public:
  explicit Parser(const std::string &text) : text_(text), offset_(0) {}

  Json parse()
  {
    Json value = parse_value();
    whitespace();
    if (offset_ != text_.size())
      {
        fail("unexpected trailing JSON data");
      }
    return value;
  }

private:
  [[noreturn]] void fail(const char *message) const
  {
    throw std::runtime_error(std::string(message) + " at byte " +
                             std::to_string(offset_));
  }

  void whitespace()
  {
    while (offset_ < text_.size() &&
           (text_[offset_] == ' ' || text_[offset_] == '\n' ||
            text_[offset_] == '\r' || text_[offset_] == '\t'))
      {
        ++offset_;
      }
  }

  bool consume(char value)
  {
    whitespace();
    if (offset_ < text_.size() && text_[offset_] == value)
      {
        ++offset_;
        return true;
      }
    return false;
  }

  Json parse_value()
  {
    whitespace();
    if (offset_ >= text_.size())
      {
        fail("expected JSON value");
      }

    const char current = text_[offset_];
    if (current == '{')
      return parse_object();
    if (current == '[')
      return parse_array();
    if (current == '"')
      return Json(parse_string());
    if (current == '-' || (current >= '0' && current <= '9'))
      {
        return Json(parse_number());
      }
    if (text_.compare(offset_, 4, "true") == 0)
      {
        offset_ += 4;
        return Json(true);
      }
    if (text_.compare(offset_, 5, "false") == 0)
      {
        offset_ += 5;
        return Json(false);
      }
    if (text_.compare(offset_, 4, "null") == 0)
      {
        offset_ += 4;
        return Json();
      }
    fail("invalid JSON value");
  }

  Json parse_object()
  {
    Json result = Json::object();
    consume('{');
    if (consume('}'))
      return result;
    do
      {
        whitespace();
        if (offset_ >= text_.size() || text_[offset_] != '"')
          {
            fail("expected object key");
          }
        std::string key = parse_string();
        if (!consume(':'))
          fail("expected colon");
        result[key] = parse_value();
      }
    while (consume(','));
    if (!consume('}'))
      fail("expected closing brace");
    return result;
  }

  Json parse_array()
  {
    Json result = Json::array();
    consume('[');
    if (consume(']'))
      return result;
    do
      {
        result.push_back(parse_value());
      }
    while (consume(','));
    if (!consume(']'))
      fail("expected closing bracket");
    return result;
  }

  std::string parse_string()
  {
    if (!consume('"'))
      fail("expected string");
    std::string result;
    while (offset_ < text_.size())
      {
        char value = text_[offset_++];
        if (value == '"')
          return result;
        if (value != '\\')
          {
            result.push_back(value);
            continue;
          }
        if (offset_ >= text_.size())
          fail("invalid string escape");
        value = text_[offset_++];
        switch (value)
          {
            case '"':
              result.push_back('"');
              break;
            case '\\':
              result.push_back('\\');
              break;
            case '/':
              result.push_back('/');
              break;
            case 'b':
              result.push_back('\b');
              break;
            case 'f':
              result.push_back('\f');
              break;
            case 'n':
              result.push_back('\n');
              break;
            case 'r':
              result.push_back('\r');
              break;
            case 't':
              result.push_back('\t');
              break;
            case 'u':
              {
                if (offset_ + 4 > text_.size())
                  fail("invalid unicode escape");
                unsigned codepoint = 0;
                for (int index = 0; index < 4; ++index)
                  {
                    const char digit = text_[offset_++];
                    codepoint <<= 4;
                    if (digit >= '0' && digit <= '9')
                      codepoint += digit - '0';
                    else if (digit >= 'a' && digit <= 'f')
                      codepoint += digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F')
                      codepoint += digit - 'A' + 10;
                    else
                      fail("invalid unicode digit");
                  }
                if (codepoint <= 0x7f)
                  result.push_back(static_cast<char>(codepoint));
                else if (codepoint <= 0x7ff)
                  {
                    result.push_back(
                        static_cast<char>(0xc0 | (codepoint >> 6)));
                    result.push_back(
                        static_cast<char>(0x80 | (codepoint & 0x3f)));
                  }
                else
                  {
                    result.push_back(
                        static_cast<char>(0xe0 | (codepoint >> 12)));
                    result.push_back(
                        static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
                    result.push_back(
                        static_cast<char>(0x80 | (codepoint & 0x3f)));
                  }
                break;
              }
            default:
              fail("unsupported string escape");
          }
      }
    fail("unterminated string");
  }

  double parse_number()
  {
    whitespace();
    const size_t start = offset_;
    if (text_[offset_] == '-')
      ++offset_;
    while (offset_ < text_.size() && text_[offset_] >= '0' &&
           text_[offset_] <= '9')
      ++offset_;
    if (offset_ < text_.size() && text_[offset_] == '.')
      {
        ++offset_;
        while (offset_ < text_.size() && text_[offset_] >= '0' &&
               text_[offset_] <= '9')
          ++offset_;
      }
    if (offset_ < text_.size() &&
        (text_[offset_] == 'e' || text_[offset_] == 'E'))
      {
        ++offset_;
        if (offset_ < text_.size() &&
            (text_[offset_] == '+' || text_[offset_] == '-'))
          ++offset_;
        while (offset_ < text_.size() && text_[offset_] >= '0' &&
               text_[offset_] <= '9')
          ++offset_;
      }
    try
      {
        return std::stod(text_.substr(start, offset_ - start));
      }
    catch (...)
      {
        fail("invalid number");
      }
  }

  const std::string &text_;
  size_t offset_;
};

std::string escape(const std::string &value)
{
  std::ostringstream output;
  for (const unsigned char character : value)
    {
      switch (character)
        {
          case '"':
            output << "\\\"";
            break;
          case '\\':
            output << "\\\\";
            break;
          case '\b':
            output << "\\b";
            break;
          case '\f':
            output << "\\f";
            break;
          case '\n':
            output << "\\n";
            break;
          case '\r':
            output << "\\r";
            break;
          case '\t':
            output << "\\t";
            break;
          default:
            if (character < 0x20)
              {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(character);
              }
            else
              output << character;
        }
    }
  return output.str();
}

} // namespace

Json::Json() : type_(Type::Null), boolean_(false), number_(0) {}
Json::Json(std::nullptr_t) : Json() {}
Json::Json(bool value) : type_(Type::Boolean), boolean_(value), number_(0) {}
Json::Json(int value) : Json(static_cast<double>(value)) {}
Json::Json(int64_t value) : Json(static_cast<double>(value)) {}
Json::Json(uint64_t value) : Json(static_cast<double>(value)) {}
Json::Json(double value) : type_(Type::Number), boolean_(false), number_(value)
{
}
Json::Json(const char *value)
    : Json(std::string(value == nullptr ? "" : value))
{
}
Json::Json(std::string value)
    : type_(Type::String), boolean_(false), number_(0),
      string_(std::move(value))
{
}
Json::Json(Array value)
    : type_(Type::Array), boolean_(false), number_(0), array_(std::move(value))
{
}
Json::Json(Object value)
    : type_(Type::Object), boolean_(false), number_(0),
      object_(std::move(value))
{
}

Json Json::parse(const std::string &text) { return Parser(text).parse(); }
Json Json::object() { return Json(Object{}); }
Json Json::array() { return Json(Array{}); }
Json::Type Json::type() const { return type_; }
bool Json::is_null() const { return type_ == Type::Null; }
bool Json::is_boolean() const { return type_ == Type::Boolean; }
bool Json::is_number() const { return type_ == Type::Number; }
bool Json::is_string() const { return type_ == Type::String; }
bool Json::is_array() const { return type_ == Type::Array; }
bool Json::is_object() const { return type_ == Type::Object; }
bool Json::boolean(bool fallback) const
{
  return is_boolean() ? boolean_ : fallback;
}
double Json::number(double fallback) const
{
  return is_number() ? number_ : fallback;
}
int64_t Json::integer(int64_t fallback) const
{
  return is_number() ? static_cast<int64_t>(number_) : fallback;
}
const std::string &Json::string() const
{
  static const std::string empty;
  return is_string() ? string_ : empty;
}
const Json::Array &Json::array_items() const
{
  static const Array empty;
  return is_array() ? array_ : empty;
}
Json::Array &Json::array_items()
{
  if (!is_array())
    {
      *this = array();
    }
  return array_;
}
const Json::Object &Json::object_items() const
{
  static const Object empty;
  return is_object() ? object_ : empty;
}
Json::Object &Json::object_items()
{
  if (!is_object())
    {
      *this = object();
    }
  return object_;
}
bool Json::contains(const std::string &key) const
{
  return is_object() && object_.find(key) != object_.end();
}
const Json &Json::at(const std::string &key) const
{
  static const Json null;
  auto iterator = object_.find(key);
  return iterator == object_.end() ? null : iterator->second;
}
Json &Json::operator[](const std::string &key)
{
  if (!is_object())
    *this = object();
  return object_[key];
}
void Json::push_back(Json value) { array_items().push_back(std::move(value)); }

std::string Json::dump() const
{
  std::ostringstream output;
  switch (type_)
    {
      case Type::Null:
        output << "null";
        break;
      case Type::Boolean:
        output << (boolean_ ? "true" : "false");
        break;
      case Type::Number:
        if (std::isfinite(number_) && std::floor(number_) == number_)
          output << std::fixed << std::setprecision(0) << number_;
        else
          output << std::setprecision(15) << number_;
        break;
      case Type::String:
        output << '"' << escape(string_) << '"';
        break;
      case Type::Array:
        output << '[';
        for (size_t index = 0; index < array_.size(); ++index)
          {
            if (index != 0)
              output << ',';
            output << array_[index].dump();
          }
        output << ']';
        break;
      case Type::Object:
        output << '{';
        {
          bool first = true;
          for (const auto &item : object_)
            {
              if (!first)
                output << ',';
              first = false;
              output << '"' << escape(item.first)
                     << "\":" << item.second.dump();
            }
        }
        output << '}';
        break;
    }
  return output.str();
}

} // namespace nyabula
