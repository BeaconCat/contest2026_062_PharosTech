/****************************************************************************
 * app/nyabula/host/tests/runtime_tests.cxx
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

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

class FakeClock final : public nyabula::Clock
{
public:
  uint64_t monotonic_ms() const override { return monotonic_; }
  uint64_t unix_ms() const override { return 1700000000000ULL + monotonic_; }
  void advance(uint64_t milliseconds) { monotonic_ += milliseconds; }

private:
  uint64_t monotonic_ = 0;
};

void require(bool condition, const char *message)
{
  if (!condition)
    {
      std::cerr << "FAILED: " << message << '\n';
      std::exit(1);
    }
}

nyabula::Json command(const char *action, const char *source = "test")
{
  nyabula::Json result = nyabula::Json::object();
  result["action"] = action;
  result["source"] = source;
  return result;
}

} // namespace

int main()
{
  const std::string unicode = u8"猫猫";
  nyabula::Json parsed = nyabula::Json::parse(
      std::string("{\"text\":\"") + unicode + "\",\"items\":[1,true,null]}");
  require(parsed.at("text").string() == unicode,
          "JSON preserves UTF-8 strings");
  require(parsed.at("items").array_items().size() == 3, "JSON parses arrays");

  FakeClock clock;
  nyabula::Runtime runtime(clock);
  nyabula::Json low = command("eyes.expression", "idle-service");
  low["expression"] = "happy";
  low["priority"] = 10;
  require(runtime.execute(low).at("ok").boolean(),
          "low-priority expression accepted");

  nyabula::Json high = command("eyes.expression", "assistant");
  high["expression"] = "processing";
  high["priority"] = 50;
  high["lease_ms"] = 1000;
  require(runtime.execute(high).at("ok").boolean(),
          "leased expression accepted");
  require(runtime.snapshot("eyes").at("state").at("expression").string() ==
              "processing",
          "higher-priority expression wins");
  clock.advance(1001);
  runtime.tick();
  require(runtime.snapshot("eyes").at("state").at("expression").string() ==
              "happy",
          "expired expression restores previous owner");

  nyabula::Json left_blink = command("eyes.blink");
  left_blink["eyes"] = "left";
  require(runtime.execute(left_blink).at("ok").boolean(),
          "left-eye blink is accepted");
  nyabula::Json blink_snapshot = runtime.snapshot("eyes");
  require(blink_snapshot.at("state").at("blink_eyes").string() == "left",
          "blink target survives the authoritative state protocol");
  nyabula::Json right_blink = command("eyes.blink");
  right_blink["eyes"] = "right";
  require(runtime.execute(right_blink).at("ok").boolean(),
          "right-eye blink is accepted");
  blink_snapshot = runtime.snapshot("eyes");
  require(blink_snapshot.at("state").at("blink_eyes").string() == "right",
          "right-eye blink target survives the authoritative state protocol");

  nyabula::Json capabilities = runtime.capabilities();
  require(capabilities.at("renderer_contract").at("version").integer() == 2,
          "renderer motion contract version is discoverable");
  require(capabilities.at("renderer_contract")
              .at("state_transition_ms")
              .integer() == 420,
          "renderer semantic transition duration is discoverable");
  require(capabilities.at("renderer_contract")
              .at("numeric_transition_ms")
              .integer() == 520,
          "renderer numeric transition duration is discoverable");
  require(capabilities.at("renderer_contract")
              .at("continuous_fields")
              .array_items()
              .size() == 23,
          "renderer continuous field contract remains complete");
  require(capabilities.at("renderer_contract")
              .at("clock_fields")
              .array_items()
              .size() == 3,
          "clock anchors remain separate from visual transitions");
  require(capabilities.at("renderer_contract")
              .at("semantic_fallback")
              .string() == "center_scale_crossfade_all_other_fields",
          "semantic transitions advertise center-scale crossfades");

  nyabula::Json timer = command("timer.start");
  timer["id"] = "tea";
  timer["duration_ms"] = 2500;
  runtime.execute(timer);
  require(runtime.snapshot("eyes").at("state").at("scene").string() == "timer",
          "timer presenter owns the timer scene");
  clock.advance(2600);
  runtime.tick();
  nyabula::Json core_snapshot = runtime.snapshot("core");
  const auto &timers = core_snapshot.at("state").at("timers").array_items();
  require(timers.size() == 1, "timer is listed");
  require(timers[0].at("status").string() == "completed",
          "timer completes under fake clock");

  nyabula::Json first_task = command("task.create");
  first_task["id"] = "task-a";
  first_task["title"] = u8"查天气";
  runtime.execute(first_task);
  nyabula::Json second_task = command("task.create");
  second_task["id"] = "task-b";
  second_task["title"] = u8"播放音乐";
  runtime.execute(second_task);
  core_snapshot = runtime.snapshot("core");
  require(core_snapshot.at("state").at("tasks").array_items().size() == 2,
          "multiple tasks coexist");

  nyabula::Json run = command("agent.run.start");
  run["id"] = "run-a";
  run["task_id"] = "task-a";
  runtime.execute(run);
  nyabula::Json presented_run = runtime.snapshot("eyes");
  require(presented_run.at("state").at("expression").string() == "processing",
          "active agent run drives the processing expression");
  require(presented_run.at("state").at("scene").string() == "task",
          "active agent run drives the task scene");
  nyabula::Json tool = command("agent.run.tool");
  tool["id"] = "run-a";
  tool["name"] = "weather.lookup";
  runtime.execute(tool);
  nyabula::Json agent_snapshot = runtime.snapshot("agent-runs");
  const nyabula::Json &saved_run =
      agent_snapshot.at("state").at("runs").array_items().front();
  require(saved_run.at("tool_calls").array_items().size() == 1,
          "agent run records tool activity");

  nyabula::Json long_timer = command("timer.start");
  long_timer["id"] = "persistent-timer";
  long_timer["duration_ms"] = 60000;
  runtime.execute(long_timer);
  nyabula::Json persisted = runtime.persistent_state();
  nyabula::Runtime restored(clock);
  restored.restore(persisted);
  nyabula::Json restored_agents = restored.snapshot("agent-runs");
  require(restored_agents.at("state")
                  .at("runs")
                  .array_items()
                  .front()
                  .at("status")
                  .string() == "interrupted",
          "running agent is marked interrupted after restart");
  nyabula::Json restored_core = restored.snapshot("core");
  require(restored_core.at("state").at("timers").array_items().size() == 2,
          "timers survive restart");

  FakeClock service_clock;
  nyabula::Runtime services(service_clock);

  nyabula::Json alarm = command("alarm.create");
  alarm["id"] = "wake-up";
  alarm["label"] = u8"早安";
  alarm["detail"] = u8"该喝水了";
  alarm["trigger_unix_ms"] = service_clock.unix_ms() + 1000;
  require(services.execute(alarm).at("ok").boolean(),
          "alarm creation succeeds");
  service_clock.advance(1001);
  services.tick();
  nyabula::Json alarm_core = services.snapshot("core");
  require(alarm_core.at("state")
              .at("alarms")
              .array_items()
              .front()
              .at("status")
              .string() == "ringing",
          "due alarm starts ringing");
  nyabula::Json alarm_eyes = services.snapshot("eyes");
  require(alarm_eyes.at("state").at("scene").string() == "alarm",
          "ringing alarm owns the alarm scene");
  require(alarm_eyes.at("state")
              .at("scene_owner")
              .at("source")
              .string() == "presenter.alarm",
          "alarm presenter is identified as scene owner");

  nyabula::Json dismiss = command("alarm.dismiss");
  dismiss["id"] = "wake-up";
  services.execute(dismiss);

  nyabula::Json load = command("media.load");
  load["title"] = u8"猫猫进行曲";
  load["artist"] = "Nyabula";
  load["duration_ms"] = 10000;
  load["position_ms"] = 1000;
  services.execute(load);
  services.execute(command("media.play"));
  nyabula::Json lyrics = command("media.lyrics");
  lyrics["previous_line"] = u8"灯火落进夜里";
  lyrics["current_line"] = u8"我听见你";
  lyrics["next_line"] = u8"轻轻回应";
  require(services.execute(lyrics).at("ok").boolean(),
          "media accepts an atomic three-line lyric window");
  nyabula::Json lyric_eyes = services.snapshot("eyes");
  require(lyric_eyes.at("state")
              .at("scene_payload")
              .at("current_line")
              .string() == u8"我听见你",
          "three-line lyric window reaches the eye presenter");
  service_clock.advance(2100);
  services.tick();
  nyabula::Json media = services.snapshot("media");
  require(media.at("state").at("session").at("position_ms").number() >=
              3000,
          "playing media advances under the monotonic clock");
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "music",
          "playing media owns the music scene");
  services.execute(command("media.pause"));
  const double paused_position = services.snapshot("media")
                                     .at("state")
                                     .at("session")
                                     .at("position_ms")
                                     .number();
  service_clock.advance(2100);
  services.tick();
  require(services.snapshot("media")
              .at("state")
              .at("session")
              .at("position_ms")
              .number() == paused_position,
          "paused media position remains fixed");

  nyabula::Json incoming = command("call.incoming");
  incoming["name"] = "Beacon";
  incoming["number"] = "+86 138 0013 8000";
  services.execute(incoming);
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "call",
          "incoming call overrides media");
  require(services.execute(command("call.answer")).at("ok").boolean(),
          "incoming call can be answered");
  require(services.snapshot("media")
              .at("state")
              .at("call")
              .at("status")
              .string() == "active",
          "answered call becomes active");
  services.execute(command("call.end"));
  service_clock.advance(2501);
  services.tick();
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "music",
          "media scene is restored after ended call clears");

  nyabula::Json device = command("device.update");
  device["focus"] = "network";
  device["network_state"] = "wifi";
  device["present_ms"] = 1000;
  services.execute(device);
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "music",
          "lower-priority device status does not cover media");
  services.execute(command("media.stop"));
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "music",
          "stopping media remains visible while its progress returns to zero");
  require(services.snapshot("eyes")
              .at("state")
              .at("scene_payload")
              .at("media_position_ms")
              .integer() == 0,
          "stopping media publishes a zero progress target");
  service_clock.advance(521);
  services.tick();
  require(services.snapshot("eyes").at("state").at("scene").string() ==
              "network",
          "device status becomes visible after media stops");
  service_clock.advance(1001);
  services.tick();
  require(services.snapshot("eyes").at("state").at("scene").is_null(),
          "device scene is released after its presentation window");

  FakeClock persistence_clock;
  nyabula::Runtime persistence_source(persistence_clock);
  nyabula::Json future_alarm = command("alarm.create");
  future_alarm["id"] = "persistent-alarm";
  future_alarm["trigger_unix_ms"] = persistence_clock.unix_ms() + 60000;
  persistence_source.execute(future_alarm);
  nyabula::Json persistent_media = command("media.load");
  persistent_media["title"] = "Persistent track";
  persistent_media["duration_ms"] = 60000;
  persistence_source.execute(persistent_media);
  persistence_source.execute(command("media.play"));
  nyabula::Runtime persistence_restored(persistence_clock);
  persistence_restored.restore(persistence_source.persistent_state());
  require(persistence_restored.snapshot("core")
              .at("state")
              .at("alarms")
              .array_items()
              .size() == 1,
          "alarms survive restart");
  require(persistence_restored.snapshot("media")
              .at("state")
              .at("session")
              .at("status")
              .string() == "paused",
          "playing media recovers safely as paused");

  std::cout << "nyabula_runtime_tests: all checks passed\n";
  return 0;
}
