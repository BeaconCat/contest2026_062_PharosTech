/****************************************************************************
 * app/nyabula/host/include/nyabula_host/runtime.hxx
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

#ifndef __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_RUNTIME_HXX
#define __APP_NYABULA_HOST_INCLUDE_NYABULA_HOST_RUNTIME_HXX

#include "nyabula_host/json.hxx"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace nyabula
{

class Clock
{
public:
  virtual ~Clock() = default;
  virtual uint64_t monotonic_ms() const = 0;
  virtual uint64_t unix_ms() const = 0;
};

class SystemClock final : public Clock
{
public:
  SystemClock();
  uint64_t monotonic_ms() const override;
  uint64_t unix_ms() const override;

private:
  std::chrono::steady_clock::time_point started_;
};

struct RuntimeEvent
{
  std::string channel;
  uint64_t revision;
  Json envelope;
};

class Runtime
{
public:
  using Listener = std::function<void(const RuntimeEvent &)>;

  struct Owner
  {
    std::string source;
    uint8_t priority = 0;
    uint64_t sequence = 0;
    uint64_t expires_at_ms = 0;
    Json value;
  };

  explicit Runtime(Clock &clock);

  Json execute(const Json &command);
  Json snapshot(const std::string &channel) const;
  Json capabilities() const;
  Json persistent_state() const;
  void restore(const Json &state);
  void tick();

  uint64_t subscribe(Listener listener);
  void unsubscribe(uint64_t token);

private:
  Json execute_locked(const Json &command, std::vector<RuntimeEvent> &events);
  Json eye_state_locked() const;
  Json core_state_locked() const;
  Json agent_state_locked() const;
  Json media_state_locked() const;
  Json telemetry_state_locked() const;
  Json snapshot_locked(const std::string &channel) const;
  void reconcile_eyes_locked(std::vector<RuntimeEvent> &events);
  bool present_services_locked();
  void publish_locked(const std::string &channel,
                      std::vector<RuntimeEvent> &events);
  void dispatch(const std::vector<RuntimeEvent> &events);
  static Json error(const std::string &code, const std::string &message);

  Clock &clock_;
  mutable std::mutex mutex_;
  uint64_t revision_ = 0;
  std::map<std::string, uint64_t> channel_revisions_;
  uint64_t sequence_ = 0;
  uint64_t next_listener_ = 1;
  uint64_t next_object_ = 1;
  std::map<uint64_t, Listener> listeners_;
  std::map<std::string, Owner> expression_owners_;
  std::map<std::string, Owner> scene_owners_;
  Json eye_state_;
  std::map<std::string, Json> timers_;
  std::map<std::string, Json> alarms_;
  std::map<std::string, Json> tasks_;
  std::map<std::string, Json> agent_runs_;
  Json media_state_;
  Json call_state_;
  Json device_state_;
};

} // namespace nyabula

#endif
