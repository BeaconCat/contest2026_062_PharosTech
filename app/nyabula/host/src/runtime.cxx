/****************************************************************************
 * app/nyabula/host/src/runtime.cxx
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

#include "nyabula_host/runtime.hxx"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace nyabula
{
namespace
{

const std::set<std::string> kExpressions = {
  "idle",  "curious", "happy", "processing", "star",  "heart", "sleepy",
  "sleep", "angry",   "sad",   "surprise",   "dizzy", "derp"
};

const std::set<std::string> kScenes = {
  "music",   "timer",    "weather",   "battery",  "alarm",
  "call",    "task",     "stopwatch", "calendar", "sleep-timer",
  "network", "audio",    "eq",        "caption",  "briefing",
  "privacy", "identity", "memory",    "devices",  "system",
  "health",  "presence", "companion", "home",     "subwoofer"
};

std::string field_string(const Json &object, const std::string &key,
                         const std::string &fallback = "")
{
  const Json &value = object.at(key);
  return value.is_string() ? value.string() : fallback;
}

uint64_t field_u64(const Json &object, const std::string &key,
                   uint64_t fallback = 0)
{
  const Json &value = object.at(key);
  return value.is_number() && value.number() >= 0
             ? static_cast<uint64_t>(value.number())
             : fallback;
}

bool expired(const Runtime::Owner &owner, uint64_t now)
{
  return owner.expires_at_ms != 0 && owner.expires_at_ms <= now;
}

Json owner_json(const Runtime::Owner *owner, uint64_t now)
{
  Json result = Json::object();
  result["active"] = owner != nullptr;
  if (owner != nullptr)
    {
      result["source"] = owner->source;
      result["priority"] = static_cast<int>(owner->priority);
      result["sequence"] = owner->sequence;
      result["lease_remaining_ms"] =
          owner->expires_at_ms > now ? owner->expires_at_ms - now : 0;
    }
  return result;
}

template <typename Map> Json values_json(const Map &values)
{
  Json result = Json::array();
  for (const auto &item : values)
    result.push_back(item.second);
  return result;
}

} // namespace

SystemClock::SystemClock() : started_(std::chrono::steady_clock::now()) {}

uint64_t SystemClock::monotonic_ms() const
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_)
          .count());
}

uint64_t SystemClock::unix_ms() const
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

Runtime::Runtime(Clock &clock) : clock_(clock), eye_state_(Json::object())
{
  eye_state_["expression"] = "idle";
  eye_state_["expression_owner"] = owner_json(nullptr, 0);
  eye_state_["scene"] = Json();
  eye_state_["scene_style"] = "full";
  eye_state_["scene_payload"] = Json::object();
  eye_state_["scene_owner"] = owner_json(nullptr, 0);
  eye_state_["auto_blink"] = true;
  eye_state_["ambient_light"] = 0.55;
  eye_state_["iris_left"] = "#38e06e";
  eye_state_["iris_right"] = "#38e06e";
  eye_state_["gaze"] = Json::object();
  eye_state_["gaze"]["x"] = 0.0;
  eye_state_["gaze"]["y"] = 0.0;
  eye_state_["blink_nonce"] = 0;
  eye_state_["blink_eyes"] = "both";

  media_state_ = Json::object();
  media_state_["status"] = "stopped";
  media_state_["title"] = "";
  media_state_["artist"] = "";
  media_state_["duration_ms"] = 0;
  media_state_["position_ms"] = 0;
  media_state_["synced_at_ms"] = 0;
  media_state_["view"] = "spectrum";
  media_state_["previous_line"] = "";
  media_state_["current_line"] = "";
  media_state_["next_line"] = "";

  call_state_ = Json::object();
  call_state_["status"] = "idle";
  call_state_["name"] = "";
  call_state_["number"] = "";

  device_state_ = Json::object();
  device_state_["network_state"] = "offline";
  device_state_["audio_route"] = "speaker";
  device_state_["battery_state"] = "charging";
  device_state_["battery_percent"] = 68;
  device_state_["privacy_camera"] = false;
  device_state_["privacy_microphone"] = false;
  device_state_["focus"] = "";
  device_state_["present_until_ms"] = 0;
}

Json Runtime::error(const std::string &code, const std::string &message)
{
  Json result = Json::object();
  result["ok"] = false;
  result["error"] = Json::object();
  result["error"]["code"] = code;
  result["error"]["message"] = message;
  return result;
}

Json Runtime::execute(const Json &command)
{
  std::vector<RuntimeEvent> events;
  Json result;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    result = execute_locked(command, events);
  }
  dispatch(events);
  return result;
}

Json Runtime::execute_locked(const Json &command,
                             std::vector<RuntimeEvent> &events)
{
  if (!command.is_object())
    return error("invalid_command", "command must be an object");
  const std::string action = field_string(command, "action");
  const std::string source = field_string(command, "source", "console");
  const std::string request_id = field_string(command, "request_id");
  const uint64_t now = clock_.monotonic_ms();
  bool eyes_changed = false;
  bool core_changed = false;
  bool agents_changed = false;
  bool media_changed = false;
  bool telemetry_changed = false;

  if (action == "eyes.expression")
    {
      const std::string expression = field_string(command, "expression");
      if (kExpressions.count(expression) == 0)
        return error("invalid_expression", "unknown eye expression");
      Owner &owner = expression_owners_[source];
      owner.source = source;
      owner.priority = static_cast<uint8_t>(
          std::min<uint64_t>(255, field_u64(command, "priority", 40)));
      owner.sequence = ++sequence_;
      const uint64_t lease = field_u64(command, "lease_ms");
      owner.expires_at_ms = lease == 0 ? 0 : now + lease;
      owner.value = command;
      eyes_changed = true;
    }
  else if (action == "eyes.scene.show")
    {
      const std::string scene = field_string(command, "scene");
      if (kScenes.count(scene) == 0)
        return error("invalid_scene", "unknown eye scene");
      Owner &owner = scene_owners_[source];
      owner.source = source;
      owner.priority = static_cast<uint8_t>(
          std::min<uint64_t>(255, field_u64(command, "priority", 40)));
      owner.sequence = ++sequence_;
      const uint64_t lease = field_u64(command, "lease_ms");
      owner.expires_at_ms = lease == 0 ? 0 : now + lease;
      owner.value = command;
      eyes_changed = true;
    }
  else if (action == "eyes.scene.hide")
    {
      eyes_changed = scene_owners_.erase(source) != 0;
    }
  else if (action == "core.release")
    {
      const std::string domain = field_string(command, "domain", "all");
      if (domain == "all" || domain == "expression")
        eyes_changed = expression_owners_.erase(source) != 0 || eyes_changed;
      if (domain == "all" || domain == "scene")
        eyes_changed = scene_owners_.erase(source) != 0 || eyes_changed;
    }
  else if (action == "core.reset")
    {
      expression_owners_.clear();
      scene_owners_.clear();
      timers_.clear();
      alarms_.clear();
      tasks_.clear();
      agent_runs_.clear();
      eye_state_["auto_blink"] = true;
      eye_state_["ambient_light"] = 0.55;
      eye_state_["iris_left"] = "#38e06e";
      eye_state_["iris_right"] = "#38e06e";
      eye_state_["blink_eyes"] = "both";
      media_state_["status"] = "stopped";
      media_state_["position_ms"] = 0;
      call_state_["status"] = "idle";
      device_state_["focus"] = "";
      device_state_["present_until_ms"] = 0;
      eyes_changed = true;
      core_changed = true;
      agents_changed = true;
      media_changed = true;
      telemetry_changed = true;
    }
  else if (action == "eyes.blink")
    {
      const std::string eyes = field_string(command, "eyes", "both");
      if (eyes != "left" && eyes != "right" && eyes != "both")
        return error("invalid_eyes", "eyes must be left, right or both");
      eye_state_["blink_eyes"] = eyes;
      eye_state_["blink_nonce"] = eye_state_.at("blink_nonce").integer() + 1;
      eyes_changed = true;
    }
  else if (action == "eyes.gaze")
    {
      eye_state_["gaze"]["x"] = command.at("x").number();
      eye_state_["gaze"]["y"] = command.at("y").number();
      eye_state_["gaze"]["hold_ms"] = field_u64(command, "hold_ms", 2200);
      eyes_changed = true;
    }
  else if (action == "eyes.auto_blink")
    {
      eye_state_["auto_blink"] = command.at("enabled").boolean(true);
      eyes_changed = true;
    }
  else if (action == "eyes.ambient")
    {
      eye_state_["ambient_light"] =
          std::max(0.0, std::min(1.0, command.at("level").number(0.55)));
      eyes_changed = true;
    }
  else if (action == "eyes.iris")
    {
      const std::string color = field_string(command, "color", "#38e06e");
      const std::string target = field_string(command, "target", "both");
      if (target == "both" || target == "left")
        eye_state_["iris_left"] = color;
      if (target == "both" || target == "right")
        eye_state_["iris_right"] = color;
      eyes_changed = true;
    }
  else if (action == "timer.start")
    {
      const std::string id = field_string(
          command, "id", "timer-" + std::to_string(next_object_++));
      const uint64_t duration = field_u64(command, "duration_ms");
      if (duration == 0)
        return error("invalid_duration", "duration_ms must be positive");
      Json timer = Json::object();
      timer["id"] = id;
      timer["label"] = field_string(command, "label", "倒计时");
      timer["status"] = "running";
      timer["duration_ms"] = duration;
      timer["remaining_ms"] = duration;
      timer["deadline_ms"] = now + duration;
      timer["deadline_unix_ms"] = clock_.unix_ms() + duration;
      timers_[id] = timer;
      core_changed = true;
    }
  else if (action == "timer.pause" || action == "timer.resume" ||
           action == "timer.cancel")
    {
      const std::string id = field_string(command, "id");
      auto iterator = timers_.find(id);
      if (iterator == timers_.end())
        return error("not_found", "timer does not exist");
      Json &timer = iterator->second;
      if (action == "timer.pause" &&
          field_string(timer, "status") == "running")
        {
          const uint64_t deadline = field_u64(timer, "deadline_ms");
          timer["remaining_ms"] = deadline > now ? deadline - now : 0;
          timer["status"] = "paused";
        }
      else if (action == "timer.resume" &&
               field_string(timer, "status") == "paused")
        {
          timer["deadline_ms"] = now + field_u64(timer, "remaining_ms");
          timer["deadline_unix_ms"] =
              clock_.unix_ms() + field_u64(timer, "remaining_ms");
          timer["status"] = "running";
        }
      else if (action == "timer.cancel")
        timer["status"] = "cancelled";
      core_changed = true;
    }
  else if (action == "alarm.create")
    {
      const std::string id = field_string(
          command, "id", "alarm-" + std::to_string(next_object_++));
      const uint64_t trigger = field_u64(command, "trigger_unix_ms");
      if (trigger == 0)
        return error("invalid_trigger", "trigger_unix_ms must be positive");
      Json alarm = Json::object();
      alarm["id"] = id;
      alarm["label"] = field_string(command, "label", "提醒");
      alarm["detail"] = field_string(command, "detail");
      alarm["trigger_unix_ms"] = trigger;
      alarm["enabled"] = command.at("enabled").boolean(true);
      alarm["status"] = "scheduled";
      alarm["created_at_ms"] = clock_.unix_ms();
      alarms_[id] = alarm;
      core_changed = true;
    }
  else if (action == "alarm.enable" || action == "alarm.dismiss" ||
           action == "alarm.snooze" || action == "alarm.delete")
    {
      const std::string id = field_string(command, "id");
      auto iterator = alarms_.find(id);
      if (iterator == alarms_.end())
        return error("not_found", "alarm does not exist");
      if (action == "alarm.delete")
        alarms_.erase(iterator);
      else if (action == "alarm.enable")
        {
          iterator->second["enabled"] = command.at("enabled").boolean(true);
          iterator->second["status"] = command.at("enabled").boolean(true)
                                             ? "scheduled"
                                             : "disabled";
        }
      else if (action == "alarm.dismiss")
        {
          iterator->second["enabled"] = false;
          iterator->second["status"] = "dismissed";
          iterator->second["dismissed_at_ms"] = clock_.unix_ms();
        }
      else
        {
          const uint64_t duration = field_u64(command, "duration_ms", 300000);
          iterator->second["enabled"] = true;
          iterator->second["status"] = "scheduled";
          iterator->second["trigger_unix_ms"] = clock_.unix_ms() + duration;
        }
      core_changed = true;
    }
  else if (action == "media.load")
    {
      const uint64_t duration = field_u64(command, "duration_ms");
      if (duration == 0)
        return error("invalid_duration", "duration_ms must be positive");
      media_state_["status"] = "paused";
      media_state_["title"] = field_string(command, "title", "未命名音频");
      media_state_["artist"] = field_string(command, "artist");
      media_state_["album"] = field_string(command, "album");
      media_state_["duration_ms"] = duration;
      media_state_["position_ms"] = std::min(
          duration, field_u64(command, "position_ms"));
      media_state_["synced_at_ms"] = now;
      media_state_["clear_at_ms"] = 0;
      media_state_["view"] = field_string(command, "view", "spectrum");
      media_state_["previous_line"] = field_string(command, "previous_line");
      media_state_["current_line"] = field_string(command, "current_line");
      media_state_["next_line"] = field_string(command, "next_line");
      media_changed = true;
    }
  else if (action == "media.lyrics")
    {
      if (field_u64(media_state_, "duration_ms") == 0)
        return error("no_media", "no media session is loaded");
      media_state_["previous_line"] = field_string(command, "previous_line");
      media_state_["current_line"] = field_string(command, "current_line");
      media_state_["next_line"] = field_string(command, "next_line");
      media_state_["view"] = "lyrics";
      media_changed = true;
    }
  else if (action == "media.play" || action == "media.pause" ||
           action == "media.seek" || action == "media.stop" ||
           action == "media.view")
    {
      const uint64_t duration = field_u64(media_state_, "duration_ms");
      if (duration == 0 && action != "media.view")
        return error("no_media", "no media session is loaded");
      uint64_t position = field_u64(media_state_, "position_ms");
      if (field_string(media_state_, "status") == "playing")
        position = std::min(
            duration, position + now - field_u64(media_state_, "synced_at_ms"));
      if (action == "media.play")
        {
          media_state_["status"] = "playing";
          media_state_["clear_at_ms"] = 0;
        }
      else if (action == "media.pause")
        media_state_["status"] = "paused";
      else if (action == "media.seek")
        position = std::min(duration, field_u64(command, "position_ms"));
      else if (action == "media.stop")
        {
          media_state_["status"] = "stopping";
          position = 0;
          media_state_["clear_at_ms"] = now + 520;
        }
      else
        media_state_["view"] = field_string(command, "view", "spectrum");
      media_state_["position_ms"] = position;
      media_state_["synced_at_ms"] = now;
      media_changed = true;
    }
  else if (action == "call.incoming")
    {
      call_state_["status"] = "incoming";
      call_state_["name"] = field_string(command, "name", "未知联系人");
      call_state_["number"] = field_string(command, "number");
      call_state_["started_at_ms"] = clock_.unix_ms();
      media_changed = true;
    }
  else if (action == "call.answer" || action == "call.end" ||
           action == "call.clear")
    {
      const std::string current = field_string(call_state_, "status");
      if (action == "call.answer")
        {
          if (current != "incoming")
            return error("invalid_state", "no incoming call to answer");
          call_state_["status"] = "active";
          call_state_["answered_at_ms"] = clock_.unix_ms();
        }
      else if (action == "call.end")
        {
          if (current == "idle")
            return error("invalid_state", "no call to end");
          call_state_["status"] = "ended";
          call_state_["ended_at_ms"] = clock_.unix_ms();
          call_state_["clear_at_ms"] = now + 2500;
        }
      else
        call_state_["status"] = "idle";
      media_changed = true;
    }
  else if (action == "device.update")
    {
      for (const auto &field : command.object_items())
        if (field.first != "action" && field.first != "source" &&
            field.first != "request_id" && field.first != "priority" &&
            field.first != "lease_ms")
          device_state_[field.first] = field.second;
      const uint64_t present_for = field_u64(command, "present_ms", 3000);
      if (!field_string(command, "focus").empty())
        device_state_["present_until_ms"] = now + present_for;
      telemetry_changed = true;
    }
  else if (action == "task.create")
    {
      const std::string id = field_string(
          command, "id", "task-" + std::to_string(next_object_++));
      Json task = Json::object();
      task["id"] = id;
      task["title"] = field_string(command, "title", "未命名任务");
      task["status"] = "queued";
      task["progress"] = 0.0;
      task["created_at_ms"] = clock_.unix_ms();
      task["updated_at_ms"] = clock_.unix_ms();
      tasks_[id] = task;
      core_changed = true;
    }
  else if (action == "task.update")
    {
      const std::string id = field_string(command, "id");
      auto iterator = tasks_.find(id);
      if (iterator == tasks_.end())
        return error("not_found", "task does not exist");
      if (command.at("status").is_string())
        iterator->second["status"] = command.at("status");
      if (command.at("progress").is_number())
        iterator->second["progress"] = command.at("progress");
      iterator->second["updated_at_ms"] = clock_.unix_ms();
      core_changed = true;
    }
  else if (action == "task.delete")
    {
      const std::string id = field_string(command, "id");
      if (tasks_.erase(id) == 0)
        return error("not_found", "task does not exist");
      core_changed = true;
    }
  else if (action == "agent.run.start")
    {
      const std::string id =
          field_string(command, "id", "run-" + std::to_string(next_object_++));
      Json run = Json::object();
      run["id"] = id;
      run["task_id"] = field_string(command, "task_id");
      run["agent"] = field_string(command, "agent", "mock-agent");
      run["status"] = "running";
      run["phase"] = "thinking";
      run["message"] = field_string(command, "message", "正在思考");
      run["progress"] = 0.0;
      run["tool_calls"] = Json::array();
      run["started_at_ms"] = clock_.unix_ms();
      run["updated_at_ms"] = clock_.unix_ms();
      agent_runs_[id] = run;
      agents_changed = true;
    }
  else if (action == "agent.run.progress" || action == "agent.run.tool" ||
           action == "agent.run.complete" || action == "agent.run.fail")
    {
      const std::string id = field_string(command, "id");
      auto iterator = agent_runs_.find(id);
      if (iterator == agent_runs_.end())
        return error("not_found", "agent run does not exist");
      Json &run = iterator->second;
      if (action == "agent.run.progress")
        {
          if (command.at("phase").is_string())
            run["phase"] = command.at("phase");
          if (command.at("message").is_string())
            run["message"] = command.at("message");
          if (command.at("progress").is_number())
            run["progress"] = command.at("progress");
        }
      else if (action == "agent.run.tool")
        {
          Json call = Json::object();
          call["name"] = field_string(command, "name");
          call["status"] = field_string(command, "status", "running");
          call["at_ms"] = clock_.unix_ms();
          run["tool_calls"].push_back(call);
          run["phase"] = "tool";
        }
      else
        {
          run["status"] =
              action == "agent.run.complete" ? "completed" : "failed";
          run["phase"] = run.at("status");
          run["progress"] =
              action == "agent.run.complete" ? 1.0 : run.at("progress");
          run["message"] = field_string(
              command, "message",
              action == "agent.run.complete" ? "已完成" : "执行失败");
          run["finished_at_ms"] = clock_.unix_ms();
        }
      run["updated_at_ms"] = clock_.unix_ms();
      agents_changed = true;
    }
  else
    return error("unknown_action", "action is not supported");

  if (core_changed || agents_changed || media_changed || telemetry_changed)
    eyes_changed = present_services_locked() || eyes_changed;

  if (eyes_changed)
    {
      reconcile_eyes_locked(events);
      publish_locked("eyes", events);
    }
  if (core_changed)
    publish_locked("core", events);
  if (agents_changed)
    publish_locked("agent-runs", events);
  if (media_changed)
    publish_locked("media", events);
  if (telemetry_changed)
    publish_locked("telemetry", events);

  Json result = Json::object();
  result["ok"] = true;
  result["request_id"] = request_id;
  result["revision"] = revision_;
  return result;
}

bool Runtime::present_services_locked()
{
  bool changed = false;
  const uint64_t now = clock_.monotonic_ms();
  auto update_owner = [this, now,
                       &changed](auto &owners, const std::string &source,
                                 uint8_t priority, const Json *value) {
    auto iterator = owners.find(source);
    if (value == nullptr)
      {
        if (iterator != owners.end())
          {
            owners.erase(iterator);
            changed = true;
          }
        return;
      }
    if (iterator != owners.end() &&
        iterator->second.value.dump() == value->dump())
      return;
    Owner owner;
    owner.source = source;
    owner.priority = priority;
    owner.sequence = ++sequence_;
    owner.expires_at_ms = 0;
    owner.value = *value;
    owners[source] = std::move(owner);
    changed = true;
  };

  const std::string call_status = field_string(call_state_, "status", "idle");
  if (call_status != "idle")
    {
      Json scene = Json::object();
      scene["scene"] = "call";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["call_state"] = call_status;
      scene["payload"]["call_name"] = call_state_.at("name");
      scene["payload"]["call_number"] = call_state_.at("number");
      update_owner(scene_owners_, "presenter.call", 70, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.call", 70, nullptr);

  const Json *ringing_alarm = nullptr;
  for (auto iterator = alarms_.rbegin(); iterator != alarms_.rend(); ++iterator)
    if (field_string(iterator->second, "status") == "ringing")
      {
        ringing_alarm = &iterator->second;
        break;
      }
  if (ringing_alarm != nullptr)
    {
      Json scene = Json::object();
      scene["scene"] = "alarm";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["alarm_id"] = ringing_alarm->at("id");
      scene["payload"]["alarm_label"] = ringing_alarm->at("label");
      scene["payload"]["alarm_detail"] = ringing_alarm->at("detail");
      scene["payload"]["alarm_copy"] =
          field_string(*ringing_alarm, "detail").empty() ? "name" : "reminder";
      scene["payload"]["alarm_trigger_unix_ms"] =
          ringing_alarm->at("trigger_unix_ms");
      update_owner(scene_owners_, "presenter.alarm", 60, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.alarm", 60, nullptr);

  const Json *active_run = nullptr;
  for (auto iterator = agent_runs_.rbegin(); iterator != agent_runs_.rend();
       ++iterator)
    if (field_string(iterator->second, "status") == "running")
      {
        active_run = &iterator->second;
        break;
      }
  if (active_run != nullptr)
    {
      Json expression = Json::object();
      expression["expression"] = "processing";
      update_owner(expression_owners_, "presenter.agent", 45, &expression);
      Json scene = Json::object();
      scene["scene"] = "task";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["task_state"] =
          field_string(*active_run, "phase") == "tool" ? "running" : "queued";
      scene["payload"]["task_progress"] = active_run->at("progress");
      scene["payload"]["task_title"] = active_run->at("message");
      scene["payload"]["run_id"] = active_run->at("id");
      update_owner(scene_owners_, "presenter.agent", 45, &scene);
    }
  else
    {
      update_owner(expression_owners_, "presenter.agent", 45, nullptr);
      update_owner(scene_owners_, "presenter.agent", 45, nullptr);
    }

  const Json *active_task = nullptr;
  for (auto iterator = tasks_.rbegin(); iterator != tasks_.rend(); ++iterator)
    {
      const std::string status = field_string(iterator->second, "status");
      if (status == "queued" || status == "running" || status == "confirm")
        {
          active_task = &iterator->second;
          break;
        }
    }
  if (active_task != nullptr)
    {
      Json scene = Json::object();
      scene["scene"] = "task";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["task_state"] = active_task->at("status");
      scene["payload"]["task_progress"] = active_task->at("progress");
      scene["payload"]["task_title"] = active_task->at("title");
      scene["payload"]["task_id"] = active_task->at("id");
      update_owner(scene_owners_, "presenter.task", 30, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.task", 30, nullptr);

  const Json *active_timer = nullptr;
  for (auto iterator = timers_.rbegin(); iterator != timers_.rend();
       ++iterator)
    {
      const std::string status = field_string(iterator->second, "status");
      if (status == "running" || status == "paused")
        {
          active_timer = &iterator->second;
          break;
        }
    }
  if (active_timer != nullptr)
    {
      Json scene = Json::object();
      scene["scene"] = "timer";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["timer_id"] = active_timer->at("id");
      scene["payload"]["timer_label"] = active_timer->at("label");
      scene["payload"]["timer_status"] = active_timer->at("status");
      scene["payload"]["timer_duration_ms"] = active_timer->at("duration_ms");
      scene["payload"]["timer_remaining_ms"] =
          active_timer->at("remaining_ms");
      scene["payload"]["timer_presented_at_ms"] = now;
      update_owner(scene_owners_, "presenter.timer", 25, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.timer", 25, nullptr);

  const std::string media_status =
      field_string(media_state_, "status", "stopped");
  if (media_status == "playing" || media_status == "paused" ||
      media_status == "stopping")
    {
      Json scene = Json::object();
      scene["scene"] = "music";
      scene["style"] = "full";
      scene["payload"] = Json::object();
      scene["payload"]["media_status"] = media_status;
      scene["payload"]["media_title"] = media_state_.at("title");
      scene["payload"]["media_artist"] = media_state_.at("artist");
      scene["payload"]["media_duration_ms"] =
          media_state_.at("duration_ms");
      scene["payload"]["media_position_ms"] =
          media_state_.at("position_ms");
      scene["payload"]["media_synced_at_ms"] = now;
      scene["payload"]["music_view"] = media_state_.at("view");
      scene["payload"]["previous_line"] = media_state_.at("previous_line");
      scene["payload"]["current_line"] = media_state_.at("current_line");
      scene["payload"]["next_line"] = media_state_.at("next_line");
      update_owner(scene_owners_, "presenter.media", 20, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.media", 20, nullptr);

  const uint64_t present_until = field_u64(device_state_, "present_until_ms");
  const std::string focus = field_string(device_state_, "focus");
  if (!focus.empty() && present_until > now && kScenes.count(focus) != 0)
    {
      Json scene = Json::object();
      scene["scene"] = focus;
      scene["style"] = "full";
      scene["payload"] = device_state_;
      update_owner(scene_owners_, "presenter.device", 15, &scene);
    }
  else
    update_owner(scene_owners_, "presenter.device", 15, nullptr);

  return changed;
}

void Runtime::reconcile_eyes_locked(std::vector<RuntimeEvent> &)
{
  const uint64_t now = clock_.monotonic_ms();
  auto choose = [now](auto &owners) -> Owner * {
    Owner *winner = nullptr;
    for (auto iterator = owners.begin(); iterator != owners.end();)
      {
        if (expired(iterator->second, now))
          iterator = owners.erase(iterator);
        else
          {
            Owner &candidate = iterator++->second;
            if (winner == nullptr || candidate.priority > winner->priority ||
                (candidate.priority == winner->priority &&
                 candidate.sequence > winner->sequence))
              winner = &candidate;
          }
      }
    return winner;
  };

  Owner *expression = choose(expression_owners_);
  Owner *scene = choose(scene_owners_);
  eye_state_["expression"] =
      expression == nullptr
          ? "idle"
          : field_string(expression->value, "expression", "idle");
  eye_state_["expression_owner"] = owner_json(expression, now);
  eye_state_["scene"] = scene == nullptr ? Json() : scene->value.at("scene");
  eye_state_["scene_style"] =
      scene == nullptr ? "full" : field_string(scene->value, "style", "full");
  eye_state_["scene_payload"] =
      scene == nullptr ? Json::object() : scene->value.at("payload");
  eye_state_["scene_owner"] = owner_json(scene, now);
}

void Runtime::publish_locked(const std::string &channel,
                             std::vector<RuntimeEvent> &events)
{
  ++revision_;
  const uint64_t channel_revision = ++channel_revisions_[channel];
  Json envelope = Json::object();
  envelope["protocol"] = "nyabula.v1";
  envelope["type"] = "state.patch";
  envelope["channel"] = channel;
  envelope["revision"] = channel_revision;
  envelope["event_sequence"] = revision_;
  envelope["server_time_ms"] = clock_.unix_ms();
  envelope["patch"] = Json::object();
  envelope["patch"]["op"] = "replace";
  envelope["patch"]["path"] = "/";
  envelope["patch"]["value"] = snapshot_locked(channel).at("state");
  events.push_back({ channel, channel_revision, envelope });
}

Json Runtime::eye_state_locked() const { return eye_state_; }

Json Runtime::core_state_locked() const
{
  Json result = Json::object();
  result["timers"] = values_json(timers_);
  result["alarms"] = values_json(alarms_);
  result["tasks"] = values_json(tasks_);
  return result;
}

Json Runtime::agent_state_locked() const
{
  Json result = Json::object();
  result["runs"] = values_json(agent_runs_);
  return result;
}

Json Runtime::media_state_locked() const
{
  Json result = Json::object();
  result["session"] = media_state_;
  result["call"] = call_state_;
  return result;
}

Json Runtime::telemetry_state_locked() const
{
  Json result = Json::object();
  result["device"] = device_state_;
  return result;
}

Json Runtime::snapshot_locked(const std::string &channel) const
{
  Json envelope = Json::object();
  envelope["protocol"] = "nyabula.v1";
  envelope["type"] = "state.snapshot";
  envelope["channel"] = channel;
  const auto revision = channel_revisions_.find(channel);
  envelope["revision"] =
      revision == channel_revisions_.end() ? 0 : revision->second;
  envelope["event_sequence"] = revision_;
  envelope["server_time_ms"] = clock_.unix_ms();
  if (channel == "eyes")
    envelope["state"] = eye_state_locked();
  else if (channel == "core")
    envelope["state"] = core_state_locked();
  else if (channel == "agent-runs")
    envelope["state"] = agent_state_locked();
  else if (channel == "media")
    envelope["state"] = media_state_locked();
  else if (channel == "telemetry")
    envelope["state"] = telemetry_state_locked();
  else
    envelope["state"] = Json::object();
  return envelope;
}

Json Runtime::snapshot(const std::string &channel) const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return snapshot_locked(channel);
}

Json Runtime::capabilities() const
{
  Json result = Json::object();
  result["protocol"] = "nyabula.v1";
  result["websocket_paths"] = Json::array();
  for (const char *path : { "/ws/v1/eyes", "/ws/v1/core", "/ws/v1/agent-runs",
                            "/ws/v1/media", "/ws/v1/telemetry" })
    result["websocket_paths"].push_back(path);
  result["actions"] = Json::array();
  for (const char *action :
       { "eyes.expression", "eyes.blink",         "eyes.gaze",
         "eyes.auto_blink", "eyes.ambient",       "eyes.iris",
         "eyes.scene.show", "eyes.scene.hide",    "core.release",
         "timer.start",     "timer.pause",        "timer.resume",
         "timer.cancel",    "alarm.create",       "alarm.enable",
         "alarm.dismiss",   "alarm.snooze",       "alarm.delete",
         "task.create",     "task.update",        "task.delete",
         "media.load",      "media.play",         "media.pause",
         "media.seek",      "media.stop",         "media.view",
         "media.lyrics",
         "call.incoming",   "call.answer",        "call.end",
         "call.clear",      "device.update",      "agent.run.start",
         "agent.run.progress", "agent.run.tool",  "agent.run.complete",
         "agent.run.fail" })
    result["actions"].push_back(action);
  result["renderer_contract"] = Json::object();
  result["renderer_contract"]["version"] = 2;
  result["renderer_contract"]["state_transition_ms"] = 420;
  result["renderer_contract"]["numeric_transition_ms"] = 520;
  result["renderer_contract"]["continuous_fields"] = Json::array();
  for (const char *field :
       { "duration_ms", "position_ms", "remaining_ms", "elapsed_ms",
         "percent", "device_count", "briefing_index", "briefing_count",
         "temperature_c", "feels_like_c", "humidity_percent", "wind_kph",
         "visibility_km", "distance_m", "heart_rate_bpm", "crossover_hz",
         "progress", "eq_bands", "media_duration_ms", "media_position_ms",
         "timer_duration_ms", "timer_remaining_ms", "task_progress" })
    result["renderer_contract"]["continuous_fields"].push_back(field);
  result["renderer_contract"]["clock_fields"] = Json::array();
  for (const char *field :
       { "media_synced_at_ms", "timer_presented_at_ms", "present_until_ms" })
    result["renderer_contract"]["clock_fields"].push_back(field);
  result["renderer_contract"]["semantic_fallback"] =
      "center_scale_crossfade_all_other_fields";
  return result;
}

Json Runtime::persistent_state() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  Json state = Json::object();
  state["schema_version"] = 1;
  state["saved_at_ms"] = clock_.unix_ms();
  state["next_object"] = next_object_;
  state["timers"] = values_json(timers_);
  state["alarms"] = values_json(alarms_);
  state["tasks"] = values_json(tasks_);
  state["agent_runs"] = values_json(agent_runs_);
  state["media"] = media_state_;
  return state;
}

void Runtime::restore(const Json &state)
{
  if (!state.is_object() || state.at("schema_version").integer() != 1)
    return;
  std::lock_guard<std::mutex> guard(mutex_);
  timers_.clear();
  alarms_.clear();
  tasks_.clear();
  agent_runs_.clear();
  next_object_ = std::max<uint64_t>(1, field_u64(state, "next_object", 1));
  const uint64_t monotonic = clock_.monotonic_ms();
  const uint64_t wall = clock_.unix_ms();
  for (Json timer : state.at("timers").array_items())
    {
      const std::string id = field_string(timer, "id");
      if (id.empty())
        continue;
      if (field_string(timer, "status") == "running")
        {
          const uint64_t wall_deadline = field_u64(timer, "deadline_unix_ms");
          const uint64_t remaining =
              wall_deadline > wall ? wall_deadline - wall : 0;
          timer["remaining_ms"] = remaining;
          timer["deadline_ms"] = monotonic + remaining;
          if (remaining == 0)
            timer["status"] = "completed";
        }
      timers_[id] = std::move(timer);
    }
  for (Json alarm : state.at("alarms").array_items())
    {
      const std::string id = field_string(alarm, "id");
      if (!id.empty())
        alarms_[id] = std::move(alarm);
    }
  for (Json task : state.at("tasks").array_items())
    {
      const std::string id = field_string(task, "id");
      if (id.empty())
        continue;
      if (field_string(task, "status") == "running")
        {
          task["status"] = "queued";
          task["recovery_reason"] = "host_restarted";
          task["updated_at_ms"] = wall;
        }
      tasks_[id] = std::move(task);
    }
  for (Json run : state.at("agent_runs").array_items())
    {
      const std::string id = field_string(run, "id");
      if (id.empty())
        continue;
      if (field_string(run, "status") == "running")
        {
          run["status"] = "interrupted";
          run["phase"] = "interrupted";
          run["message"] = "Host restarted before the run completed";
          run["finished_at_ms"] = wall;
          run["updated_at_ms"] = wall;
        }
      agent_runs_[id] = std::move(run);
    }
  if (state.at("media").is_object())
    {
      media_state_ = state.at("media");
      if (field_string(media_state_, "status") == "playing")
        {
          media_state_["status"] = "paused";
          media_state_["recovery_reason"] = "host_restarted";
        }
      else if (field_string(media_state_, "status") == "stopping")
        {
          media_state_["status"] = "stopped";
          media_state_["position_ms"] = 0;
          media_state_["clear_at_ms"] = 0;
        }
      media_state_["synced_at_ms"] = monotonic;
    }
  call_state_["status"] = "idle";
  present_services_locked();
  std::vector<RuntimeEvent> ignored;
  reconcile_eyes_locked(ignored);
}

void Runtime::tick()
{
  std::vector<RuntimeEvent> events;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    const uint64_t now = clock_.monotonic_ms();
    bool core_changed = false;
    bool media_changed = false;
    bool telemetry_changed = false;
    for (auto &item : timers_)
      {
        Json &timer = item.second;
        if (field_string(timer, "status") != "running")
          continue;
        const uint64_t old_remaining = field_u64(timer, "remaining_ms");
        const uint64_t deadline = field_u64(timer, "deadline_ms");
        const uint64_t remaining = deadline > now ? deadline - now : 0;
        if (remaining / 1000 != old_remaining / 1000 || remaining == 0)
          {
            timer["remaining_ms"] = remaining;
            if (remaining == 0)
              timer["status"] = "completed";
            core_changed = true;
          }
      }

    const uint64_t wall = clock_.unix_ms();
    for (auto &item : alarms_)
      {
        Json &alarm = item.second;
        if (alarm.at("enabled").boolean() &&
            field_string(alarm, "status") == "scheduled" &&
            field_u64(alarm, "trigger_unix_ms") <= wall)
          {
            alarm["status"] = "ringing";
            alarm["enabled"] = false;
            alarm["triggered_at_ms"] = wall;
            core_changed = true;
          }
      }

    if (field_string(media_state_, "status") == "playing")
      {
        const uint64_t duration = field_u64(media_state_, "duration_ms");
        const uint64_t old_position = field_u64(media_state_, "position_ms");
        const uint64_t position = std::min(
            duration, old_position + now -
                          field_u64(media_state_, "synced_at_ms"));
        if (position / 1000 != old_position / 1000 || position == duration)
          {
            media_state_["position_ms"] = position;
            media_state_["synced_at_ms"] = now;
            if (position == duration)
              {
                media_state_["status"] = "stopping";
                media_state_["position_ms"] = 0;
                media_state_["clear_at_ms"] = now + 520;
              }
            media_changed = true;
          }
      }

    if (field_string(media_state_, "status") == "stopping" &&
        field_u64(media_state_, "clear_at_ms") <= now)
      {
        media_state_["status"] = "stopped";
        media_state_["position_ms"] = 0;
        media_state_["clear_at_ms"] = 0;
        media_changed = true;
      }

    if (field_string(call_state_, "status") == "ended" &&
        field_u64(call_state_, "clear_at_ms") <= now)
      {
        call_state_["status"] = "idle";
        media_changed = true;
      }

    if (field_u64(device_state_, "present_until_ms") != 0 &&
        field_u64(device_state_, "present_until_ms") <= now)
      {
        device_state_["focus"] = "";
        device_state_["present_until_ms"] = 0;
        telemetry_changed = true;
      }

    const std::string old_expression = field_string(eye_state_, "expression");
    const std::string old_scene = eye_state_.at("scene").string();
    const std::string old_payload = eye_state_.at("scene_payload").dump();
    const bool presenter_changed =
        (core_changed || media_changed || telemetry_changed) &&
        present_services_locked();
    reconcile_eyes_locked(events);
    if (presenter_changed ||
        old_expression != field_string(eye_state_, "expression") ||
        old_scene != eye_state_.at("scene").string() ||
        old_payload != eye_state_.at("scene_payload").dump())
      publish_locked("eyes", events);
    if (core_changed)
      publish_locked("core", events);
    if (media_changed)
      publish_locked("media", events);
    if (telemetry_changed)
      publish_locked("telemetry", events);
  }
  dispatch(events);
}

uint64_t Runtime::subscribe(Listener listener)
{
  std::lock_guard<std::mutex> guard(mutex_);
  const uint64_t token = next_listener_++;
  listeners_[token] = std::move(listener);
  return token;
}

void Runtime::unsubscribe(uint64_t token)
{
  std::lock_guard<std::mutex> guard(mutex_);
  listeners_.erase(token);
}

void Runtime::dispatch(const std::vector<RuntimeEvent> &events)
{
  std::vector<Listener> listeners;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto &listener : listeners_)
      listeners.push_back(listener.second);
  }
  for (const RuntimeEvent &event : events)
    for (const Listener &listener : listeners)
      listener(event);
}

} // namespace nyabula
