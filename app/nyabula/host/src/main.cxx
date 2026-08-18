/****************************************************************************
 * app/nyabula/host/src/main.cxx
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
#include "nyabula_host/runtime.hxx"
#include "nyabula_host/windows_gateway.hxx"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace
{
std::atomic<bool> g_running{ true };
void stop(int) { g_running = false; }
}

int main(int argc, char **argv)
{
  const uint16_t port =
      argc > 1 ? static_cast<uint16_t>(std::stoi(argv[1])) : 8090;
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  nyabula::SystemClock clock;
  nyabula::Runtime runtime(clock);
  const auto data_root = nyabula::windows_data_root();
  nyabula::JsonFileStore state_store(data_root / "state" / "core.json");
  runtime.restore(state_store.load());
  std::atomic<bool> state_dirty{ false };
  const uint64_t persistence_token =
      runtime.subscribe([&state_dirty](const nyabula::RuntimeEvent &event) {
      if (event.channel == "core" || event.channel == "agent-runs" ||
          event.channel == "media")
          state_dirty = true;
      });
  nyabula::WindowsGateway gateway(runtime, port, NYABULA_HOST_WEB_ROOT,
                                  NYABULA_APP_ROOT);
  if (!gateway.start())
    {
      std::cerr << "nyabula_host: failed to listen on port " << port << '\n';
      return 1;
    }
  std::cout << "Nyabula Windows Host\n"
            << "  Console:  http://127.0.0.1:" << port << "/\n"
            << "  Mock Eye: http://127.0.0.1:" << port << "/mock-eye\n"
            << "  MCP:      http://127.0.0.1:" << port << "/mcp/v1\n"
            << "  Data:     " << data_root.string() << '\n';
  uint64_t last_save = 0;
  while (g_running)
    {
      runtime.tick();
      const uint64_t now = clock.monotonic_ms();
      if (state_dirty && now - last_save >= 500)
        {
          if (state_store.save(runtime.persistent_state()))
            {
              state_dirty = false;
              last_save = now;
            }
        }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  if (state_dirty)
    state_store.save(runtime.persistent_state());
  runtime.unsubscribe(persistence_token);
  gateway.stop();
  return 0;
}
