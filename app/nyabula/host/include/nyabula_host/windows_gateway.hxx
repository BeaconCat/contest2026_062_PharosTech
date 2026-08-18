/****************************************************************************
 * app/nyabula/host/include/nyabula_host/windows_gateway.hxx
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

#ifndef __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_WINDOWS_GATEWAY_HXX
#define __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_WINDOWS_GATEWAY_HXX

#include "nyabula_host/runtime.hxx"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace nyabula
{

class WindowsGateway
{
public:
  WindowsGateway(Runtime &runtime, uint16_t port, std::string web_root,
                 std::string app_root);
  ~WindowsGateway();

  bool start();
  void stop();
  uint16_t port() const;

private:
  void accept_loop();
  void handle_client(uintptr_t socket_value);

  Runtime &runtime_;
  uint16_t port_;
  std::string web_root_;
  std::string app_root_;
  std::atomic<bool> running_{ false };
  uintptr_t listen_socket_ = ~uintptr_t(0);
  std::thread accept_thread_;
};

} // namespace nyabula

#endif
