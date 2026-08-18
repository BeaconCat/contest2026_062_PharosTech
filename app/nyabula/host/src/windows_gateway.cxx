/****************************************************************************
 * app/nyabula/host/src/windows_gateway.cxx
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

#include "nyabula_host/windows_gateway.hxx"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace nyabula
{
namespace
{

struct HttpRequest
{
  std::string method;
  std::string target;
  std::map<std::string, std::string> headers;
  std::string body;
};

struct WebSocketSession
{
  SOCKET socket = INVALID_SOCKET;
  std::mutex write_mutex;
  std::atomic<bool> alive{ true };

  bool send_text(const std::string &text)
  {
    std::lock_guard<std::mutex> guard(write_mutex);
    if (!alive)
      return false;
    std::vector<unsigned char> frame;
    frame.push_back(0x81);
    const uint64_t size = text.size();
    if (size < 126)
      frame.push_back(static_cast<unsigned char>(size));
    else if (size <= 0xffff)
      {
        frame.push_back(126);
        frame.push_back(static_cast<unsigned char>((size >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(size & 0xff));
      }
    else
      {
        frame.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8)
          frame.push_back(static_cast<unsigned char>((size >> shift) & 0xff));
      }
    frame.insert(frame.end(), text.begin(), text.end());
    size_t sent = 0;
    while (sent < frame.size())
      {
        const int count =
            send(socket, reinterpret_cast<const char *>(frame.data() + sent),
                 static_cast<int>(frame.size() - sent), 0);
        if (count <= 0)
          {
            alive = false;
            return false;
          }
        sent += static_cast<size_t>(count);
      }
    return true;
  }
};

std::string lower(std::string value)
{
  for (char &character : value)
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  return value;
}

bool receive_exact(SOCKET socket, void *data, size_t size)
{
  size_t received = 0;
  while (received < size)
    {
      const int count = recv(socket, static_cast<char *>(data) + received,
                             static_cast<int>(size - received), 0);
      if (count <= 0)
        return false;
      received += static_cast<size_t>(count);
    }
  return true;
}

bool receive_request(SOCKET socket, HttpRequest &request)
{
  std::string data;
  std::array<char, 4096> buffer{};
  size_t header_end = std::string::npos;
  while (data.size() < 1024 * 1024)
    {
      const int count =
          recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (count <= 0)
        return false;
      data.append(buffer.data(), static_cast<size_t>(count));
      header_end = data.find("\r\n\r\n");
      if (header_end != std::string::npos)
        break;
    }
  if (header_end == std::string::npos)
    return false;

  std::istringstream headers(data.substr(0, header_end));
  std::string line;
  if (!std::getline(headers, line))
    return false;
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  std::istringstream first(line);
  std::string version;
  if (!(first >> request.method >> request.target >> version))
    return false;
  while (std::getline(headers, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const size_t colon = line.find(':');
      if (colon == std::string::npos)
        continue;
      std::string key = lower(line.substr(0, colon));
      size_t start = colon + 1;
      while (start < line.size() && line[start] == ' ')
        ++start;
      request.headers[key] = line.substr(start);
    }
  const size_t content_length =
      request.headers.count("content-length")
          ? static_cast<size_t>(std::stoull(request.headers["content-length"]))
          : 0;
  request.body = data.substr(header_end + 4);
  while (request.body.size() < content_length)
    {
      const int count =
          recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (count <= 0)
        return false;
      request.body.append(buffer.data(), static_cast<size_t>(count));
    }
  if (request.body.size() > content_length)
    request.body.resize(content_length);
  return true;
}

void send_all(SOCKET socket, const std::string &data)
{
  size_t sent = 0;
  while (sent < data.size())
    {
      const int count = send(socket, data.data() + sent,
                             static_cast<int>(data.size() - sent), 0);
      if (count <= 0)
        return;
      sent += static_cast<size_t>(count);
    }
}

void http_response(
    SOCKET socket, int status, const char *reason, const char *content_type,
    const std::string &body,
    const std::vector<std::pair<std::string, std::string> > &extra = {})
{
  std::ostringstream response;
  response << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
           << "Content-Type: " << content_type << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Access-Control-Allow-Origin: *\r\n"
           << "Access-Control-Allow-Headers: content-type, authorization\r\n"
           << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           << "Cache-Control: no-store\r\n";
  for (const auto &header : extra)
    response << header.first << ": " << header.second << "\r\n";
  response << "Connection: close\r\n\r\n" << body;
  send_all(socket, response.str());
}

std::array<uint32_t, 5> sha1(const std::string &input)
{
  std::vector<unsigned char> message(input.begin(), input.end());
  const uint64_t bit_length = static_cast<uint64_t>(message.size()) * 8;
  message.push_back(0x80);
  while ((message.size() % 64) != 56)
    message.push_back(0);
  for (int shift = 56; shift >= 0; shift -= 8)
    message.push_back(
        static_cast<unsigned char>((bit_length >> shift) & 0xff));
  std::array<uint32_t, 5> hash = { 0x67452301, 0xefcdab89, 0x98badcfe,
                                   0x10325476, 0xc3d2e1f0 };
  for (size_t block = 0; block < message.size(); block += 64)
    {
      uint32_t words[80]{};
      for (int index = 0; index < 16; ++index)
        words[index] =
            (static_cast<uint32_t>(message[block + index * 4]) << 24) |
            (static_cast<uint32_t>(message[block + index * 4 + 1]) << 16) |
            (static_cast<uint32_t>(message[block + index * 4 + 2]) << 8) |
            message[block + index * 4 + 3];
      for (int index = 16; index < 80; ++index)
        {
          const uint32_t value = words[index - 3] ^ words[index - 8] ^
                                 words[index - 14] ^ words[index - 16];
          words[index] = (value << 1) | (value >> 31);
        }
      uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
      for (int index = 0; index < 80; ++index)
        {
          uint32_t function, constant;
          if (index < 20)
            {
              function = (b & c) | ((~b) & d);
              constant = 0x5a827999;
            }
          else if (index < 40)
            {
              function = b ^ c ^ d;
              constant = 0x6ed9eba1;
            }
          else if (index < 60)
            {
              function = (b & c) | (b & d) | (c & d);
              constant = 0x8f1bbcdc;
            }
          else
            {
              function = b ^ c ^ d;
              constant = 0xca62c1d6;
            }
          const uint32_t rotated = (a << 5) | (a >> 27);
          const uint32_t next =
              rotated + function + e + constant + words[index];
          e = d;
          d = c;
          c = (b << 30) | (b >> 2);
          b = a;
          a = next;
        }
      hash[0] += a;
      hash[1] += b;
      hash[2] += c;
      hash[3] += d;
      hash[4] += e;
    }
  return hash;
}

std::string base64(const unsigned char *data, size_t size)
{
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  for (size_t index = 0; index < size; index += 3)
    {
      const uint32_t value =
          static_cast<uint32_t>(data[index]) << 16 |
          (index + 1 < size ? static_cast<uint32_t>(data[index + 1]) << 8
                            : 0) |
          (index + 2 < size ? data[index + 2] : 0);
      output.push_back(alphabet[(value >> 18) & 63]);
      output.push_back(alphabet[(value >> 12) & 63]);
      output.push_back(index + 1 < size ? alphabet[(value >> 6) & 63] : '=');
      output.push_back(index + 2 < size ? alphabet[value & 63] : '=');
    }
  return output;
}

std::string websocket_accept(const std::string &key)
{
  const auto hash = sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  std::array<unsigned char, 20> bytes{};
  for (size_t index = 0; index < hash.size(); ++index)
    for (int byte = 0; byte < 4; ++byte)
      bytes[index * 4 + byte] =
          static_cast<unsigned char>(hash[index] >> (24 - byte * 8));
  return base64(bytes.data(), bytes.size());
}

bool receive_websocket_text(SOCKET socket, std::string &text, uint8_t &opcode)
{
  unsigned char header[2];
  if (!receive_exact(socket, header, 2))
    return false;
  opcode = header[0] & 0x0f;
  const bool masked = (header[1] & 0x80) != 0;
  uint64_t size = header[1] & 0x7f;
  if (size == 126)
    {
      unsigned char extended[2];
      if (!receive_exact(socket, extended, 2))
        return false;
      size = static_cast<uint64_t>(extended[0]) << 8 | extended[1];
    }
  else if (size == 127)
    {
      unsigned char extended[8];
      if (!receive_exact(socket, extended, 8))
        return false;
      size = 0;
      for (unsigned char byte : extended)
        size = (size << 8) | byte;
    }
  if (size > 1024 * 1024)
    return false;
  unsigned char mask[4]{};
  if (masked && !receive_exact(socket, mask, 4))
    return false;
  text.resize(static_cast<size_t>(size));
  if (size != 0 &&
      !receive_exact(socket, text.data(), static_cast<size_t>(size)))
    return false;
  if (masked)
    for (size_t index = 0; index < text.size(); ++index)
      text[index] ^= mask[index % 4];
  return true;
}

std::string read_file(const std::filesystem::path &path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return {};
  return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string content_type(const std::filesystem::path &path)
{
  const std::string extension = lower(path.extension().string());
  if (extension == ".html")
    return "text/html; charset=utf-8";
  if (extension == ".js")
    return "text/javascript; charset=utf-8";
  if (extension == ".ttf")
    return "font/ttf";
  if (extension == ".svg")
    return "image/svg+xml";
  return "application/octet-stream";
}

Json outbound_mcp_call(const Json &input)
{
  const std::string endpoint = input.at("endpoint").string();
  if (endpoint.rfind("http://", 0) != 0)
    throw std::runtime_error(
        "only http:// MCP endpoints are supported by the Windows mock");
  const size_t authority_start = 7;
  const size_t path_start = endpoint.find('/', authority_start);
  const std::string authority =
      endpoint.substr(authority_start, path_start == std::string::npos
                                           ? std::string::npos
                                           : path_start - authority_start);
  const size_t colon = authority.rfind(':');
  const std::string host =
      colon == std::string::npos ? authority : authority.substr(0, colon);
  const std::string port =
      colon == std::string::npos ? "80" : authority.substr(colon + 1);
  const std::string path =
      path_start == std::string::npos ? "/" : endpoint.substr(path_start);
  if (host.empty())
    throw std::runtime_error("MCP endpoint has no host");

  Json request = Json::object();
  request["jsonrpc"] = "2.0";
  request["id"] = input.at("id").is_null() ? Json(1) : input.at("id");
  request["method"] =
      input.at("method").is_string() ? input.at("method") : Json("tools/call");
  request["params"] =
      input.at("params").is_object() ? input.at("params") : Json::object();
  if (request.at("method").string() == "tools/call" &&
      input.at("name").is_string())
    {
      request["params"]["name"] = input.at("name");
      request["params"]["arguments"] = input.at("arguments").is_object()
                                           ? input.at("arguments")
                                           : Json::object();
    }
  const std::string body = request.dump();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *addresses = nullptr;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0)
    throw std::runtime_error("failed to resolve MCP endpoint");
  SOCKET client = INVALID_SOCKET;
  for (addrinfo *address = addresses; address != nullptr;
       address = address->ai_next)
    {
      client = socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
      if (client != INVALID_SOCKET &&
          connect(client, address->ai_addr,
                  static_cast<int>(address->ai_addrlen)) == 0)
        break;
      if (client != INVALID_SOCKET)
        closesocket(client);
      client = INVALID_SOCKET;
    }
  freeaddrinfo(addresses);
  if (client == INVALID_SOCKET)
    throw std::runtime_error("failed to connect to MCP endpoint");
  std::ostringstream wire;
  wire << "POST " << path << " HTTP/1.1\r\nHost: " << authority
       << "\r\nContent-Type: application/json\r\nContent-Length: "
       << body.size() << "\r\nConnection: close\r\n\r\n"
       << body;
  send_all(client, wire.str());
  std::string response;
  std::array<char, 4096> buffer{};
  for (;;)
    {
      const int count =
          recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (count <= 0)
        break;
      response.append(buffer.data(), static_cast<size_t>(count));
    }
  closesocket(client);
  const size_t body_start = response.find("\r\n\r\n");
  if (body_start == std::string::npos)
    throw std::runtime_error("invalid MCP HTTP response");
  std::istringstream status_line(response.substr(0, response.find("\r\n")));
  std::string version;
  int status = 0;
  status_line >> version >> status;
  if (status < 200 || status >= 300)
    throw std::runtime_error("MCP endpoint returned HTTP " +
                             std::to_string(status));
  return Json::parse(response.substr(body_start + 4));
}

Json mcp_response(Runtime &runtime, const Json &request)
{
  Json response = Json::object();
  response["jsonrpc"] = "2.0";
  response["id"] = request.at("id");
  const std::string method = request.at("method").string();
  if (method == "initialize")
    {
      response["result"] = Json::object();
      response["result"]["protocolVersion"] = "2025-03-26";
      response["result"]["capabilities"] = Json::object();
      response["result"]["capabilities"]["tools"] = Json::object();
      response["result"]["serverInfo"] = Json::object();
      response["result"]["serverInfo"]["name"] = "nyabula-core";
      response["result"]["serverInfo"]["version"] = "0.1.0";
    }
  else if (method == "tools/list")
    {
      response["result"] = Json::object();
      response["result"]["tools"] = Json::array();
      for (const auto &definition :
           std::vector<std::pair<const char *, const char *> >{
               { "nyabula_command",
                 "Submit a semantic command to Nyabula Core" },
               { "nyabula_set_expression",
                 "Set Nyabula's semantic eye expression" },
               { "nyabula_show_scene", "Show a typed Nyabula eye scene" },
               { "nyabula_create_task",
                 "Create a task in Nyabula's task service" } })
        {
          Json tool = Json::object();
          tool["name"] = definition.first;
          tool["description"] = definition.second;
          tool["inputSchema"] = Json::object();
          tool["inputSchema"]["type"] = "object";
          tool["inputSchema"]["additionalProperties"] = true;
          response["result"]["tools"].push_back(tool);
        }
    }
  else if (method == "tools/call")
    {
      const std::string name = request.at("params").at("name").string();
      Json command = request.at("params").at("arguments");
      if (name == "nyabula_set_expression")
        command["action"] = "eyes.expression";
      else if (name == "nyabula_show_scene")
        command["action"] = "eyes.scene.show";
      else if (name == "nyabula_create_task")
        command["action"] = "task.create";
      else if (name != "nyabula_command")
        {
          response["error"] = Json::object();
          response["error"]["code"] = -32602;
          response["error"]["message"] = "Unknown tool";
          return response;
        }
      if (!command.contains("source"))
        command["source"] = "mcp";
      Json executed = runtime.execute(command);
      response["result"] = Json::object();
      response["result"]["isError"] = !executed.at("ok").boolean();
      response["result"]["content"] = Json::array();
      Json content = Json::object();
      content["type"] = "text";
      content["text"] = executed.dump();
      response["result"]["content"].push_back(content);
    }
  else
    {
      response["error"] = Json::object();
      response["error"]["code"] = -32601;
      response["error"]["message"] = "Method not found";
    }
  return response;
}

} // namespace

WindowsGateway::WindowsGateway(Runtime &runtime, uint16_t port,
                               std::string web_root, std::string app_root)
    : runtime_(runtime), port_(port), web_root_(std::move(web_root)),
      app_root_(std::move(app_root))
{
}

WindowsGateway::~WindowsGateway() { stop(); }

bool WindowsGateway::start()
{
  if (running_)
    return true;
  WSADATA data;
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    return false;
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET)
    return false;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(port_);
  if (bind(listener, reinterpret_cast<sockaddr *>(&address),
           sizeof(address)) == SOCKET_ERROR ||
      listen(listener, SOMAXCONN) == SOCKET_ERROR)
    {
      closesocket(listener);
      return false;
    }
  listen_socket_ = static_cast<uintptr_t>(listener);
  running_ = true;
  accept_thread_ = std::thread(&WindowsGateway::accept_loop, this);
  return true;
}

void WindowsGateway::stop()
{
  if (!running_.exchange(false))
    return;
  SOCKET listener = static_cast<SOCKET>(listen_socket_);
  shutdown(listener, SD_BOTH);
  closesocket(listener);
  if (accept_thread_.joinable())
    accept_thread_.join();
  listen_socket_ = ~uintptr_t(0);
  WSACleanup();
}

uint16_t WindowsGateway::port() const { return port_; }

void WindowsGateway::accept_loop()
{
  const SOCKET listener = static_cast<SOCKET>(listen_socket_);
  while (running_)
    {
      SOCKET client = accept(listener, nullptr, nullptr);
      if (client == INVALID_SOCKET)
        continue;
      std::thread(&WindowsGateway::handle_client, this,
                  static_cast<uintptr_t>(client))
          .detach();
    }
}

void WindowsGateway::handle_client(uintptr_t socket_value)
{
  const SOCKET socket = static_cast<SOCKET>(socket_value);
  HttpRequest request;
  if (!receive_request(socket, request))
    {
      closesocket(socket);
      return;
    }

  const std::string route = request.target.substr(0, request.target.find('?'));

  const bool websocket = lower(request.headers["upgrade"]) == "websocket" &&
                         route.rfind("/ws/v1/", 0) == 0;
  if (websocket)
    {
      const std::string channel = route.substr(std::string("/ws/v1/").size());
      if (channel != "eyes" && channel != "core" && channel != "agent-runs" &&
          channel != "media" && channel != "telemetry")
        {
          http_response(socket, 404, "Not Found", "text/plain",
                        "unknown channel");
          closesocket(socket);
          return;
        }
      const std::string accept =
          websocket_accept(request.headers["sec-websocket-key"]);
      std::ostringstream response;
      response << "HTTP/1.1 101 Switching Protocols\r\n"
               << "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
      send_all(socket, response.str());
      auto session = std::make_shared<WebSocketSession>();
      session->socket = socket;
      session->send_text(runtime_.snapshot(channel).dump());
      std::weak_ptr<WebSocketSession> weak = session;
      const uint64_t token =
          runtime_.subscribe([weak, channel](const RuntimeEvent &event) {
            if (event.channel != channel)
              return;
            if (auto locked = weak.lock())
              locked->send_text(event.envelope.dump());
          });
      while (session->alive)
        {
          std::string text;
          uint8_t opcode = 0;
          if (!receive_websocket_text(socket, text, opcode) || opcode == 0x8)
            break;
          if (opcode == 0x9)
            continue;
          if (opcode != 0x1)
            continue;
          try
            {
              Json message = Json::parse(text);
              Json reply;
              if (message.at("type").string() == "clock.sync")
                {
                  reply = Json::object();
                  reply["protocol"] = "nyabula.v1";
                  reply["type"] = "clock.sync.ack";
                  reply["request_id"] = message.at("request_id");
                  reply["client_time_ms"] = message.at("client_time_ms");
                  reply["server_time_ms"] =
                      runtime_.snapshot(channel).at("server_time_ms");
                }
              else if (message.at("type").string() == "state.resume")
                {
                  reply = runtime_.snapshot(channel);
                }
              else
                {
                  Json command = message.at("command").is_object()
                                     ? message.at("command")
                                     : message;
                  Json result = runtime_.execute(command);
                  reply = Json::object();
                  reply["protocol"] = "nyabula.v1";
                  reply["type"] = "command.ack";
                  reply["request_id"] = message.at("request_id");
                  reply["result"] = result;
                }
              session->send_text(reply.dump());
            }
          catch (const std::exception &exception)
            {
              Json reply = Json::object();
              reply["protocol"] = "nyabula.v1";
              reply["type"] = "command.ack";
              reply["ok"] = false;
              reply["error"] = exception.what();
              session->send_text(reply.dump());
            }
        }
      session->alive = false;
      runtime_.unsubscribe(token);
      shutdown(socket, SD_BOTH);
      closesocket(socket);
      return;
    }

  if (request.method == "OPTIONS")
    http_response(socket, 204, "No Content", "text/plain", "");
  else if (request.method == "GET" && route == "/api/v1/health")
    http_response(socket, 200, "OK", "application/json",
                  "{\"ok\":true,\"name\":\"Nyabula Windows Host\"}");
  else if (request.method == "GET" && route == "/api/v1/capabilities")
    http_response(socket, 200, "OK", "application/json",
                  runtime_.capabilities().dump());
  else if (request.method == "GET" && route.rfind("/api/v1/state/", 0) == 0)
    http_response(socket, 200, "OK", "application/json",
                  runtime_.snapshot(route.substr(14)).dump());
  else if (request.method == "POST" && route == "/api/v1/command")
    {
      try
        {
          http_response(socket, 200, "OK", "application/json",
                        runtime_.execute(Json::parse(request.body)).dump());
        }
      catch (const std::exception &exception)
        {
          Json result = Json::object();
          result["ok"] = false;
          result["error"] = exception.what();
          http_response(socket, 400, "Bad Request", "application/json",
                        result.dump());
        }
    }
  else if (request.method == "POST" && route == "/mcp/v1")
    {
      try
        {
          http_response(
              socket, 200, "OK", "application/json",
              mcp_response(runtime_, Json::parse(request.body)).dump());
        }
      catch (const std::exception &exception)
        {
          Json result = Json::object();
          result["jsonrpc"] = "2.0";
          result["id"] = Json();
          result["error"] = Json::object();
          result["error"]["code"] = -32700;
          result["error"]["message"] = exception.what();
          http_response(socket, 400, "Bad Request", "application/json",
                        result.dump());
        }
    }
  else if (request.method == "POST" && route == "/api/v1/mcp/call")
    {
      try
        {
          http_response(socket, 200, "OK", "application/json",
                        outbound_mcp_call(Json::parse(request.body)).dump());
        }
      catch (const std::exception &exception)
        {
          Json result = Json::object();
          result["ok"] = false;
          result["error"] = exception.what();
          http_response(socket, 502, "Bad Gateway", "application/json",
                        result.dump());
        }
    }
  else
    {
      std::filesystem::path path;
      if (route == "/" || route == "/console")
        path = std::filesystem::path(web_root_) / "console.html";
      else if (route == "/mock-eye")
        path = std::filesystem::path(web_root_) / "mock_eye.html";
      else if (route == "/mock_eye_transport.js")
        path = std::filesystem::path(web_root_) / "mock_eye_transport.js";
      else if (route.rfind("/fonts/", 0) == 0)
        path = std::filesystem::path(app_root_) / "res" / route.substr(1);
      else if (route == "/icons/material_icon_paths.js")
        path = std::filesystem::path(app_root_) / "res" / "icons" /
               "material_icon_paths.js";
      if (!path.empty())
        {
          const std::string body = read_file(path);
          if (!body.empty())
            http_response(socket, 200, "OK", content_type(path).c_str(), body);
          else
            http_response(socket, 404, "Not Found", "text/plain",
                          "asset not found");
        }
      else
        http_response(socket, 404, "Not Found", "text/plain",
                      "route not found");
    }
  closesocket(socket);
}

} // namespace nyabula
