/****************************************************************************
 * app/nyabula/host/web/mock_eye_transport.js
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

"use strict";

(() => {
  let socket = null;
  let revision = 0;
  let reconnectDelay = 250;
  let blinkNonce = null;
  let requestSequence = 1;

  function setConnectionState(state, detail) {
    document.documentElement.dataset.coreConnection = state;
    console.info(`MockEyeRenderer · ${state}${detail ? ` · ${detail}` : ""}`);
  }

  function requestId() {
    return `mock-eye-${Date.now()}-${requestSequence++}`;
  }

  function applyExpression(expression) {
    if (expression === mode) return;
    const previousMode = mode;
    if (expression === "sleep" && previousMode !== "sleep") {
      sleepStartTop = cur.lidTop;
      sleepStartBot = cur.lidBot;
      sleepEndBot = Math.max(.14, sleepStartBot);
      sleepEndTop = Math.max(sleepStartTop, 1 - sleepEndBot);
    }
    mode = expression;
    modeT = 0;
    if (mode === "sleep" && previousMode !== "sleep") zNext = 0;
    if (mode === "surprise") {
      blinkPhase = -1;
      nextBlink = 3;
    }
    document.querySelectorAll("[data-mode]").forEach(button =>
      button.classList.toggle("on", button.dataset.mode === expression));
  }

  function applyPayload(payload = {}) {
    let optionChanged = false;
    const markOptionChanged = () => { optionChanged = true; };
    const clone = value => JSON.parse(JSON.stringify(value ?? {}));
    const changed = (left, right) => JSON.stringify(left) !== JSON.stringify(right);
    let semanticPayloadChanged = false;
    let numericPayloadChanged = false;
    for (const [key, value] of Object.entries(payload)) {
      if (SCENE_CLOCK_FIELDS.has(key)) {
        continue;
      } else if (SCENE_NUMERIC_FIELDS.has(key) || key === "eq_bands") {
        numericPayloadChanged ||= changed(scenePayload[key], value);
      } else {
        semanticPayloadChanged ||= changed(scenePayload[key], value);
      }
    }
    if (semanticPayloadChanged && Object.keys(scenePayload).length > 0) {
      scenePrevPayload = clone(scenePayloadDisplay);
      markOptionChanged();
    }
    if (numericPayloadChanged) {
      scenePayloadFrom = clone(scenePayloadDisplay);
      scenePayloadFade = 0;
    }
    scenePayload = {...scenePayload, ...clone(payload)};
    if (Object.keys(scenePayloadDisplay).length === 0) {
      scenePayloadDisplay = clone(scenePayload);
    }
    const mediaChanged =
      (typeof payload.media_title === "string" && payload.media_title !== mediaTitle) ||
      (typeof payload.media_artist === "string" && payload.media_artist !== mediaArtist) ||
      (typeof payload.media_status === "string" && payload.media_status !== mediaStatus) ||
      (Number.isFinite(payload.media_duration_ms) && payload.media_duration_ms !== mediaDurationMs);
    if (mediaChanged) {
      mediaPrev = currentMediaState();
      mediaPrevStatus = mediaStatus;
      markOptionChanged();
    }
    const timerChanged =
      (typeof payload.timer_status === "string" && payload.timer_status !== timerStatus) ||
      (Number.isFinite(payload.timer_duration_ms) && payload.timer_duration_ms !== timerDurationMs);
    if (timerChanged) {
      timerPrev = currentTimerState();
      markOptionChanged();
    }
    const alarmChanged =
      (typeof payload.alarm_copy === "string" && payload.alarm_copy !== alarmCopy) ||
      (typeof payload.alarm_label === "string" && payload.alarm_label !== alarmLabel) ||
      (typeof payload.alarm_detail === "string" && payload.alarm_detail !== alarmDetail) ||
      (Number.isFinite(payload.alarm_trigger_unix_ms) &&
       payload.alarm_trigger_unix_ms !== alarmTriggerUnixMs);
    if (alarmChanged) {
      alarmPrev = {copy: alarmCopy, label: alarmLabel, detail: alarmDetail,
                   trigger: alarmTriggerUnixMs};
      alarmPrevCopy = alarmCopy;
      markOptionChanged();
    }
    const callChanged =
      (typeof payload.call_state === "string" && payload.call_state !== callState) ||
      (typeof payload.call_name === "string" && payload.call_name !== callName) ||
      (typeof payload.call_number === "string" && payload.call_number !== callNumber);
    if (callChanged) {
      callPrev = {state: callState, name: callName, number: callNumber};
      callPrevState = callState;
      markOptionChanged();
    }
    if (payload.condition && payload.condition !== weatherKind) {
      weatherPrevKind = weatherKind;
      weatherKind = payload.condition;
      weatherFade = 0;
    }
    if (payload.music_view && payload.music_view !== musicView) {
      musicPrevView = musicView;
      musicView = payload.music_view;
      musicViewFade = 0;
    }
    if (payload.alarm_copy && payload.alarm_copy !== alarmCopy) {
      alarmCopy = payload.alarm_copy;
    }
    if (typeof payload.alarm_label === "string") alarmLabel = payload.alarm_label;
    if (typeof payload.alarm_detail === "string") alarmDetail = payload.alarm_detail;
    if (Number.isFinite(payload.alarm_trigger_unix_ms)) {
      alarmTriggerUnixMs = payload.alarm_trigger_unix_ms;
    }
    if (typeof payload.media_title === "string" && payload.media_title !== mediaTitle) {
      mediaTitle = payload.media_title;
    }
    if (typeof payload.media_artist === "string" && payload.media_artist !== mediaArtist) {
      mediaArtist = payload.media_artist;
    }
    if (Number.isFinite(payload.media_duration_ms)) {
      mediaDurationMs = payload.media_duration_ms;
    }
    if (Number.isFinite(payload.media_position_ms)) {
      mediaPositionMs = payload.media_position_ms;
      mediaSyncedAt = tNow;
    }
    if (typeof payload.media_status === "string" && payload.media_status !== mediaStatus) {
      mediaStatus = payload.media_status;
    }
    if (typeof payload.call_name === "string" && payload.call_name !== callName) {
      callName = payload.call_name;
    }
    if (typeof payload.call_number === "string" && payload.call_number !== callNumber) {
      callNumber = payload.call_number;
    }
    if (Number.isFinite(payload.timer_duration_ms)) timerDurationMs = payload.timer_duration_ms;
    if (Number.isFinite(payload.timer_remaining_ms)) {
      timerRemainingMs = payload.timer_remaining_ms;
      timerSyncedAt = tNow;
    }
    if (payload.timer_status) timerStatus = payload.timer_status;
    if (Number.isFinite(payload.task_progress)) {
      const nextProgress = Math.max(0, Math.min(1, payload.task_progress));
      if (taskProgressDisplay === null) taskProgressDisplay = nextProgress;
      if (taskProgressTarget !== nextProgress) {
        taskProgressFrom = taskProgressDisplay;
        taskProgressTarget = nextProgress;
        taskProgressFade = 0;
      }
      taskProgress = nextProgress;
    }
    if (typeof payload.task_title === "string" && payload.task_title !== taskTitle) {
      taskPrevState = taskState;
      taskPrevProgress = taskProgressDisplay;
      taskPrevTitle = taskTitle;
      taskTitle = payload.task_title;
      markOptionChanged();
    }
    const changes = [
      ["battery_state", () => batteryState, value => { batteryPrevState = batteryState; batteryState = value; }],
      ["call_state", () => callState, value => { callState = value; }],
      ["task_state", () => taskState, value => { if (!taskPrevState) { taskPrevState = taskState; taskPrevProgress = taskProgressDisplay; taskPrevTitle = taskTitle; } taskState = value; }],
      ["network_state", () => networkState, value => { networkPrevState = networkState; networkState = value; }],
      ["audio_route", () => audioRoute, value => { audioPrevRoute = audioRoute; audioRoute = value; }],
      ["eq_view", () => eqView, value => { eqPrevView = eqView; eqView = value; }],
    ];
    for (const [field, current, update] of changes) {
      if (payload[field] !== undefined && payload[field] !== current()) {
        update(payload[field]);
        markOptionChanged();
      }
    }
    if (optionChanged) {
      optionFade = 0;
      optionChangedAt = tNow;
    }
  }

  function applyScene(nextScene, style, payload) {
    if (style && style !== sceneStyle) setSceneStyle(style);
    if (nextScene && nextScene !== sceneType && nextScene !== scenePending) {
      scenePayload = {};
      scenePayloadDisplay = {};
      scenePayloadFrom = {};
      scenePrevPayload = null;
      scenePayloadFade = 1;
    }
    applyPayload(payload);
    if (!nextScene) {
      if (sceneType || scenePending) hideScene();
      return;
    }
    if (nextScene !== sceneType && nextScene !== scenePending) showScene(nextScene);
  }

  function applyState(state) {
    applyExpression(state.expression ?? "idle");
    applyScene(state.scene, state.scene_style ?? "full", state.scene_payload ?? {});
    if (typeof state.ambient_light === "number") {
      lightLvl = Math.max(0, Math.min(1, state.ambient_light));
      const value = Math.round(lightLvl * 100);
    }
    if (state.iris_left) irisHexL = state.iris_left;
    if (state.iris_right) irisHexR = state.iris_right;
    if (state.gaze && Number.isFinite(state.gaze.x) && Number.isFinite(state.gaze.y)) {
      lookTarget = {x: state.gaze.x, y: state.gaze.y};
      lookHold = Math.max(.05, (state.gaze.hold_ms ?? 2200) / 1000);
      saccade.x = 0;
      saccade.y = 0;
    }
    if (state.blink_nonce !== blinkNonce) {
      if (blinkNonce !== null) {
        blinkEyes = state.blink_eyes ?? "both";
        blinkPhase = 0;
      }
      blinkNonce = state.blink_nonce;
    }
  }

  function handleEnvelope(envelope) {
    if (typeof envelope.revision === "number" && envelope.revision < revision) return;
    if (typeof envelope.revision === "number") revision = envelope.revision;
    if (envelope.type === "state.snapshot") applyState(envelope.state);
    else if (envelope.type === "state.patch" && envelope.patch?.op === "replace") {
      applyState(envelope.patch.value);
    }
  }

  function connect() {
    const scheme = location.protocol === "https:" ? "wss" : "ws";
    socket = new WebSocket(`${scheme}://${location.host}/ws/v1/eyes`);
    setConnectionState("连接中");
    socket.addEventListener("open", () => {
      reconnectDelay = 250;
      setConnectionState("已连接", `revision ${revision}`);
      socket.send(JSON.stringify({
        protocol: "nyabula.v1",
        type: "state.resume",
        revision,
        request_id: requestId(),
      }));
      socket.send(JSON.stringify({
        protocol: "nyabula.v1",
        type: "clock.sync",
        client_time_ms: Date.now(),
        request_id: requestId(),
      }));
    });
    socket.addEventListener("message", event => {
      try {
        const envelope = JSON.parse(event.data);
        handleEnvelope(envelope);
        if (envelope.type === "state.snapshot" || envelope.type === "state.patch") {
          setConnectionState("已连接", `revision ${revision}`);
        }
      } catch (error) {
        setConnectionState("协议错误", error.message);
      }
    });
    socket.addEventListener("close", () => {
      setConnectionState("已断线", `${reconnectDelay}ms 后重连`);
      setTimeout(connect, reconnectDelay);
      reconnectDelay = Math.min(5000, reconnectDelay * 2);
    });
    socket.addEventListener("error", () => socket.close());
  }

  window.NyabulaMockEye = {get revision() { return revision; }};
  connect();
})();
