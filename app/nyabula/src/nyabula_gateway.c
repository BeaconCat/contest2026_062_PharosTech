/****************************************************************************
 * app/nyabula/src/nyabula_gateway.c
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
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <netutils/cJSON.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../include/nyabula_gateway.h"
#include "generated/nyabula_console_html.h"

#define NYABULA_GATEWAY_REQUEST_MAX 32768
#define NYABULA_GATEWAY_BACKLOG     4

#ifndef CONFIG_CONTEST2026_062_NYABULA_GATEWAY_STACKSIZE
#define CONFIG_CONTEST2026_062_NYABULA_GATEWAY_STACKSIZE 16384
#endif

struct nyabula_gateway_name_s
{
  const char *name;
  int value;
};

struct nyabula_gateway_s
{
  struct nyabula_core_s *core;
  pthread_t thread;
  int listen_fd;
  uint16_t port;
  bool running;
};

static const struct nyabula_gateway_name_s g_nyabula_expressions[] = {
  {"idle", NYABULA_EYE_EXPRESSION_IDLE},
  {"curious", NYABULA_EYE_EXPRESSION_CURIOUS},
  {"happy", NYABULA_EYE_EXPRESSION_HAPPY},
  {"processing", NYABULA_EYE_EXPRESSION_PROCESSING},
  {"star", NYABULA_EYE_EXPRESSION_STAR},
  {"heart", NYABULA_EYE_EXPRESSION_HEART},
  {"sleepy", NYABULA_EYE_EXPRESSION_SLEEPY},
  {"sleep", NYABULA_EYE_EXPRESSION_SLEEP},
  {"angry", NYABULA_EYE_EXPRESSION_ANGRY},
  {"sad", NYABULA_EYE_EXPRESSION_SAD},
  {"surprise", NYABULA_EYE_EXPRESSION_SURPRISE},
  {"dizzy", NYABULA_EYE_EXPRESSION_DIZZY},
  {"derp", NYABULA_EYE_EXPRESSION_DERP},
};

static const struct nyabula_gateway_name_s g_nyabula_scenes[] = {
  {"none", NYABULA_EYE_SCENE_NONE},
  {"music", NYABULA_EYE_SCENE_MUSIC},
  {"timer", NYABULA_EYE_SCENE_TIMER},
  {"weather", NYABULA_EYE_SCENE_WEATHER},
  {"battery", NYABULA_EYE_SCENE_BATTERY},
  {"alarm", NYABULA_EYE_SCENE_ALARM},
  {"call", NYABULA_EYE_SCENE_CALL},
  {"task", NYABULA_EYE_SCENE_TASK},
  {"stopwatch", NYABULA_EYE_SCENE_STOPWATCH},
  {"calendar", NYABULA_EYE_SCENE_CALENDAR},
  {"sleep_timer", NYABULA_EYE_SCENE_SLEEP_TIMER},
  {"network", NYABULA_EYE_SCENE_NETWORK},
  {"audio", NYABULA_EYE_SCENE_AUDIO},
  {"eq", NYABULA_EYE_SCENE_EQ},
  {"caption", NYABULA_EYE_SCENE_CAPTION},
  {"briefing", NYABULA_EYE_SCENE_BRIEFING},
  {"privacy", NYABULA_EYE_SCENE_PRIVACY},
  {"identity", NYABULA_EYE_SCENE_IDENTITY},
  {"memory", NYABULA_EYE_SCENE_MEMORY},
  {"devices", NYABULA_EYE_SCENE_DEVICES},
  {"system", NYABULA_EYE_SCENE_SYSTEM},
  {"health", NYABULA_EYE_SCENE_HEALTH},
  {"presence", NYABULA_EYE_SCENE_PRESENCE},
  {"companion", NYABULA_EYE_SCENE_COMPANION},
  {"home", NYABULA_EYE_SCENE_HOME},
  {"subwoofer", NYABULA_EYE_SCENE_SUBWOOFER},
};

static const struct nyabula_gateway_name_s g_nyabula_styles[] = {
  {"full", NYABULA_EYE_SCENE_STYLE_FULL},
  {"minimal", NYABULA_EYE_SCENE_STYLE_MINIMAL},
};

static const struct nyabula_gateway_name_s g_nyabula_weather[] = {
  {"sunny", NYABULA_EYE_WEATHER_SUNNY},
  {"cloudy", NYABULA_EYE_WEATHER_CLOUDY},
  {"rain", NYABULA_EYE_WEATHER_RAIN},
  {"storm", NYABULA_EYE_WEATHER_STORM},
  {"snow", NYABULA_EYE_WEATHER_SNOW},
  {"fog", NYABULA_EYE_WEATHER_FOG},
};

static const struct nyabula_gateway_name_s g_nyabula_music_views[] = {
  {"spectrum", NYABULA_EYE_MUSIC_SPECTRUM},
  {"lyrics", NYABULA_EYE_MUSIC_LYRICS},
};

static const struct nyabula_gateway_name_s g_nyabula_battery_states[] = {
  {"charging", NYABULA_EYE_BATTERY_CHARGING},
  {"low", NYABULA_EYE_BATTERY_LOW},
  {"full", NYABULA_EYE_BATTERY_FULL},
  {"hot", NYABULA_EYE_BATTERY_HOT},
  {"dock", NYABULA_EYE_BATTERY_DOCK},
};

static const struct nyabula_gateway_name_s g_nyabula_alarm_copies[] = {
  {"name", NYABULA_EYE_ALARM_COPY_NAME},
  {"reminder", NYABULA_EYE_ALARM_COPY_REMINDER},
  {"none", NYABULA_EYE_ALARM_COPY_NONE},
};

static const struct nyabula_gateway_name_s g_nyabula_call_states[] = {
  {"incoming", NYABULA_EYE_CALL_INCOMING},
  {"active", NYABULA_EYE_CALL_ACTIVE},
  {"ended", NYABULA_EYE_CALL_ENDED},
};

static const struct nyabula_gateway_name_s g_nyabula_task_states[] = {
  {"running", NYABULA_EYE_TASK_RUNNING},
  {"queued", NYABULA_EYE_TASK_QUEUED},
  {"confirm", NYABULA_EYE_TASK_CONFIRM},
  {"done", NYABULA_EYE_TASK_DONE},
  {"failed", NYABULA_EYE_TASK_FAILED},
};

static const struct nyabula_gateway_name_s g_nyabula_network_states[] = {
  {"wifi", NYABULA_EYE_NETWORK_WIFI},
  {"bluetooth", NYABULA_EYE_NETWORK_BLUETOOTH},
  {"offline", NYABULA_EYE_NETWORK_OFFLINE},
};

static const struct nyabula_gateway_name_s g_nyabula_audio_routes[] = {
  {"speaker", NYABULA_EYE_AUDIO_SPEAKER},
  {"headphones", NYABULA_EYE_AUDIO_HEADPHONES},
  {"both", NYABULA_EYE_AUDIO_BOTH},
  {"mute", NYABULA_EYE_AUDIO_MUTE},
};

static const struct nyabula_gateway_name_s g_nyabula_eq_views[] = {
  {"profile", NYABULA_EYE_EQ_PROFILE},
  {"calibrating", NYABULA_EYE_EQ_CALIBRATING},
};

#define NYABULA_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

static int nyabula_gateway_lookup(const struct nyabula_gateway_name_s *table,
                                  size_t count, const char *name)
{
  for (size_t i = 0; i < count; i++)
    {
      if (strcmp(table[i].name, name) == 0)
        {
          return table[i].value;
        }
    }

  return -1;
}

static const char *
nyabula_gateway_name(const struct nyabula_gateway_name_s *table,
                     size_t count, int value)
{
  for (size_t i = 0; i < count; i++)
    {
      if (table[i].value == value)
        {
          return table[i].name;
        }
    }

  return "unknown";
}

static cJSON *nyabula_gateway_object_item(cJSON *object, const char *name)
{
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return item;
}

static const char *nyabula_gateway_string(cJSON *object, const char *name,
                                          const char *fallback)
{
  cJSON *item = nyabula_gateway_object_item(object, name);
  return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring
                                                           : fallback;
}

static double nyabula_gateway_number(cJSON *object, const char *name,
                                     double fallback)
{
  cJSON *item = nyabula_gateway_object_item(object, name);
  return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool nyabula_gateway_boolean(cJSON *object, const char *name,
                                    bool fallback)
{
  cJSON *item = nyabula_gateway_object_item(object, name);
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void nyabula_gateway_copy(char *dest, size_t size, const char *source)
{
  snprintf(dest, size, "%s", source == NULL ? "" : source);
}

static int nyabula_gateway_enum(cJSON *object, const char *name,
                                const struct nyabula_gateway_name_s *table,
                                size_t count, int fallback)
{
  const char *value = nyabula_gateway_string(object, name, NULL);
  int result;

  if (value == NULL)
    {
      return fallback;
    }

  result = nyabula_gateway_lookup(table, count, value);
  return result < 0 ? fallback : result;
}

static void nyabula_gateway_parse_payload(
    cJSON *object, struct nyabula_eye_scene_payload_s *payload)
{
  cJSON *bands;

  memset(payload, 0, sizeof(*payload));
  payload->weather = nyabula_gateway_enum(
      object, "weather", g_nyabula_weather,
      NYABULA_ARRAY_SIZE(g_nyabula_weather), NYABULA_EYE_WEATHER_SUNNY);
  payload->music_view = nyabula_gateway_enum(
      object, "music_view", g_nyabula_music_views,
      NYABULA_ARRAY_SIZE(g_nyabula_music_views),
      NYABULA_EYE_MUSIC_SPECTRUM);
  payload->battery_state = nyabula_gateway_enum(
      object, "battery_state", g_nyabula_battery_states,
      NYABULA_ARRAY_SIZE(g_nyabula_battery_states),
      NYABULA_EYE_BATTERY_CHARGING);
  payload->alarm_copy = nyabula_gateway_enum(
      object, "alarm_copy", g_nyabula_alarm_copies,
      NYABULA_ARRAY_SIZE(g_nyabula_alarm_copies),
      NYABULA_EYE_ALARM_COPY_NAME);
  payload->call_state = nyabula_gateway_enum(
      object, "call_state", g_nyabula_call_states,
      NYABULA_ARRAY_SIZE(g_nyabula_call_states),
      NYABULA_EYE_CALL_INCOMING);
  payload->task_state = nyabula_gateway_enum(
      object, "task_state", g_nyabula_task_states,
      NYABULA_ARRAY_SIZE(g_nyabula_task_states),
      NYABULA_EYE_TASK_RUNNING);
  payload->network_state = nyabula_gateway_enum(
      object, "network_state", g_nyabula_network_states,
      NYABULA_ARRAY_SIZE(g_nyabula_network_states),
      NYABULA_EYE_NETWORK_WIFI);
  payload->audio_route = nyabula_gateway_enum(
      object, "audio_route", g_nyabula_audio_routes,
      NYABULA_ARRAY_SIZE(g_nyabula_audio_routes),
      NYABULA_EYE_AUDIO_SPEAKER);
  payload->eq_view = nyabula_gateway_enum(
      object, "eq_view", g_nyabula_eq_views,
      NYABULA_ARRAY_SIZE(g_nyabula_eq_views), NYABULA_EYE_EQ_PROFILE);

#define NYABULA_PAYLOAD_NUMBER(field) \
  payload->field = nyabula_gateway_number(object, #field, 0)
  NYABULA_PAYLOAD_NUMBER(duration_ms);
  NYABULA_PAYLOAD_NUMBER(position_ms);
  NYABULA_PAYLOAD_NUMBER(remaining_ms);
  NYABULA_PAYLOAD_NUMBER(elapsed_ms);
  NYABULA_PAYLOAD_NUMBER(year);
  NYABULA_PAYLOAD_NUMBER(month);
  NYABULA_PAYLOAD_NUMBER(day);
  NYABULA_PAYLOAD_NUMBER(hour);
  NYABULA_PAYLOAD_NUMBER(minute);
  NYABULA_PAYLOAD_NUMBER(percent);
  NYABULA_PAYLOAD_NUMBER(device_count);
  NYABULA_PAYLOAD_NUMBER(briefing_index);
  NYABULA_PAYLOAD_NUMBER(briefing_count);
  NYABULA_PAYLOAD_NUMBER(temperature_c);
  NYABULA_PAYLOAD_NUMBER(feels_like_c);
  NYABULA_PAYLOAD_NUMBER(humidity_percent);
  NYABULA_PAYLOAD_NUMBER(wind_kph);
  NYABULA_PAYLOAD_NUMBER(visibility_km);
  NYABULA_PAYLOAD_NUMBER(distance_m);
  NYABULA_PAYLOAD_NUMBER(heart_rate_bpm);
  NYABULA_PAYLOAD_NUMBER(crossover_hz);
  NYABULA_PAYLOAD_NUMBER(progress);
#undef NYABULA_PAYLOAD_NUMBER

  payload->active = nyabula_gateway_boolean(object, "active", false);
  payload->playing = nyabula_gateway_boolean(object, "playing", false);
  payload->privacy_camera =
      nyabula_gateway_boolean(object, "privacy_camera", false);
  payload->privacy_microphone =
      nyabula_gateway_boolean(object, "privacy_microphone", false);
  payload->signal_good =
      nyabula_gateway_boolean(object, "signal_good", false);

#define NYABULA_PAYLOAD_TEXT(field) \
  nyabula_gateway_copy(payload->field, sizeof(payload->field), \
                       nyabula_gateway_string(object, #field, ""))
  NYABULA_PAYLOAD_TEXT(title);
  NYABULA_PAYLOAD_TEXT(subtitle);
  NYABULA_PAYLOAD_TEXT(detail);
  NYABULA_PAYLOAD_TEXT(value);
  NYABULA_PAYLOAD_TEXT(previous_line);
  NYABULA_PAYLOAD_TEXT(current_line);
  NYABULA_PAYLOAD_TEXT(next_line);
#undef NYABULA_PAYLOAD_TEXT

  bands = nyabula_gateway_object_item(object, "eq_bands");
  if (cJSON_IsArray(bands))
    {
      for (int i = 0; i < NYABULA_EYE_EQ_BANDS; i++)
        {
          cJSON *band = cJSON_GetArrayItem(bands, i);
          payload->eq_bands[i] = cJSON_IsNumber(band) ? band->valuedouble : 0;
        }
    }
}

static cJSON *nyabula_gateway_payload_json(
    const struct nyabula_eye_scene_payload_s *payload)
{
  cJSON *json = cJSON_CreateObject();
  cJSON *bands = cJSON_AddArrayToObject(json, "eq_bands");

#define NYABULA_JSON_ENUM(field, table) \
  cJSON_AddStringToObject(json, #field, \
                          nyabula_gateway_name(table, \
                                               NYABULA_ARRAY_SIZE(table), \
                                               payload->field))
  NYABULA_JSON_ENUM(weather, g_nyabula_weather);
  NYABULA_JSON_ENUM(music_view, g_nyabula_music_views);
  NYABULA_JSON_ENUM(battery_state, g_nyabula_battery_states);
  NYABULA_JSON_ENUM(alarm_copy, g_nyabula_alarm_copies);
  NYABULA_JSON_ENUM(call_state, g_nyabula_call_states);
  NYABULA_JSON_ENUM(task_state, g_nyabula_task_states);
  NYABULA_JSON_ENUM(network_state, g_nyabula_network_states);
  NYABULA_JSON_ENUM(audio_route, g_nyabula_audio_routes);
  NYABULA_JSON_ENUM(eq_view, g_nyabula_eq_views);
#undef NYABULA_JSON_ENUM

#define NYABULA_JSON_NUMBER(field) \
  cJSON_AddNumberToObject(json, #field, payload->field)
  NYABULA_JSON_NUMBER(duration_ms);
  NYABULA_JSON_NUMBER(position_ms);
  NYABULA_JSON_NUMBER(remaining_ms);
  NYABULA_JSON_NUMBER(elapsed_ms);
  NYABULA_JSON_NUMBER(year);
  NYABULA_JSON_NUMBER(month);
  NYABULA_JSON_NUMBER(day);
  NYABULA_JSON_NUMBER(hour);
  NYABULA_JSON_NUMBER(minute);
  NYABULA_JSON_NUMBER(percent);
  NYABULA_JSON_NUMBER(device_count);
  NYABULA_JSON_NUMBER(briefing_index);
  NYABULA_JSON_NUMBER(briefing_count);
  NYABULA_JSON_NUMBER(temperature_c);
  NYABULA_JSON_NUMBER(feels_like_c);
  NYABULA_JSON_NUMBER(humidity_percent);
  NYABULA_JSON_NUMBER(wind_kph);
  NYABULA_JSON_NUMBER(visibility_km);
  NYABULA_JSON_NUMBER(distance_m);
  NYABULA_JSON_NUMBER(heart_rate_bpm);
  NYABULA_JSON_NUMBER(crossover_hz);
  NYABULA_JSON_NUMBER(progress);
#undef NYABULA_JSON_NUMBER

  cJSON_AddBoolToObject(json, "active", payload->active);
  cJSON_AddBoolToObject(json, "playing", payload->playing);
  cJSON_AddBoolToObject(json, "privacy_camera", payload->privacy_camera);
  cJSON_AddBoolToObject(json, "privacy_microphone",
                        payload->privacy_microphone);
  cJSON_AddBoolToObject(json, "signal_good", payload->signal_good);

#define NYABULA_JSON_TEXT(field) \
  cJSON_AddStringToObject(json, #field, payload->field)
  NYABULA_JSON_TEXT(title);
  NYABULA_JSON_TEXT(subtitle);
  NYABULA_JSON_TEXT(detail);
  NYABULA_JSON_TEXT(value);
  NYABULA_JSON_TEXT(previous_line);
  NYABULA_JSON_TEXT(current_line);
  NYABULA_JSON_TEXT(next_line);
#undef NYABULA_JSON_TEXT

  for (int i = 0; i < NYABULA_EYE_EQ_BANDS; i++)
    {
      cJSON_AddItemToArray(bands, cJSON_CreateNumber(payload->eq_bands[i]));
    }

  return json;
}

static int nyabula_gateway_parse_command(
    cJSON *json, struct nyabula_core_command_s *command, char *error,
    size_t error_size)
{
  const char *action;
  cJSON *params;
  int value;

  memset(command, 0, sizeof(*command));
  if (!cJSON_IsObject(json))
    {
      snprintf(error, error_size, "request body must be an object");
      return -EINVAL;
    }

  action = nyabula_gateway_string(json, "action", NULL);
  params = nyabula_gateway_object_item(json, "params");
  if (action == NULL || !cJSON_IsObject(params))
    {
      snprintf(error, error_size, "action and params object are required");
      return -EINVAL;
    }

  nyabula_gateway_copy(command->source, sizeof(command->source),
                       nyabula_gateway_string(json, "source", "web"));
  nyabula_gateway_copy(command->request_id, sizeof(command->request_id),
                       nyabula_gateway_string(json, "id", ""));
  value = nyabula_gateway_number(json, "priority", 50);
  if (value < 0 || value > 255)
    {
      snprintf(error, error_size, "priority must be between 0 and 255");
      return -ERANGE;
    }

  command->priority = value;
  double lease_ms = nyabula_gateway_number(json, "lease_ms", 0);
  if (command->source[0] == '\0' || lease_ms < 0 || lease_ms > UINT32_MAX)
    {
      snprintf(error, error_size,
               "source must be non-empty and lease_ms must be uint32");
      return -ERANGE;
    }

  command->lease_ms = lease_ms;

  if (strcmp(action, "eyes.expression") == 0)
    {
      const char *name = nyabula_gateway_string(params, "expression", NULL);
      value = name == NULL ? -1 : nyabula_gateway_lookup(
          g_nyabula_expressions, NYABULA_ARRAY_SIZE(g_nyabula_expressions),
          name);
      if (value < 0)
        {
          snprintf(error, error_size, "unknown expression");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_EXPRESSION;
      command->data.expression.expression = value;
      command->data.expression.transition_ms =
          nyabula_gateway_number(params, "transition_ms", 280);
    }
  else if (strcmp(action, "eyes.blink") == 0 ||
           strcmp(action, "eyes.iris") == 0)
    {
      const char *eyes = nyabula_gateway_string(params, "eyes", "both");
      if (strcmp(eyes, "left") != 0 && strcmp(eyes, "right") != 0 &&
          strcmp(eyes, "both") != 0)
        {
          snprintf(error, error_size, "eyes must be left, right or both");
          return -EINVAL;
        }

      enum nyabula_eye_mask_e mask = strcmp(eyes, "left") == 0
                                         ? NYABULA_EYE_MASK_LEFT
                                     : strcmp(eyes, "right") == 0
                                         ? NYABULA_EYE_MASK_RIGHT
                                         : NYABULA_EYE_MASK_BOTH;
      if (strcmp(action, "eyes.blink") == 0)
        {
          command->action = NYABULA_CORE_ACTION_BLINK;
          command->data.blink.eyes = mask;
        }
      else
        {
          const char *rgb = nyabula_gateway_string(params, "rgb", NULL);
          char *end;
          unsigned long color;
          if (rgb == NULL)
            {
              snprintf(error, error_size, "rgb hex string is required");
              return -EINVAL;
            }

          color = strtoul(rgb[0] == '#' ? rgb + 1 : rgb, &end, 16);
          if (*end != '\0' || color > 0xffffff)
            {
              snprintf(error, error_size, "rgb must be a 24-bit hex value");
              return -EINVAL;
            }

          command->action = NYABULA_CORE_ACTION_IRIS_COLOR;
          command->data.iris_color.eyes = mask;
          command->data.iris_color.rgb = color;
        }
    }
  else if (strcmp(action, "eyes.gaze") == 0)
    {
      command->action = NYABULA_CORE_ACTION_GAZE;
      command->data.gaze.x = nyabula_gateway_number(params, "x", 0);
      command->data.gaze.y = nyabula_gateway_number(params, "y", 0);
      command->data.gaze.hold_ms =
          nyabula_gateway_number(params, "hold_ms", 1000);
      if (command->data.gaze.x < -1 || command->data.gaze.x > 1 ||
          command->data.gaze.y < -1 || command->data.gaze.y > 1)
        {
          snprintf(error, error_size, "gaze x and y must be between -1 and 1");
          return -ERANGE;
        }
    }
  else if (strcmp(action, "eyes.auto_blink") == 0)
    {
      command->action = NYABULA_CORE_ACTION_AUTO_BLINK;
      command->data.auto_blink.enabled =
          nyabula_gateway_boolean(params, "enabled", true);
    }
  else if (strcmp(action, "eyes.ambient") == 0)
    {
      command->action = NYABULA_CORE_ACTION_AMBIENT_LIGHT;
      command->data.ambient_light.level =
          nyabula_gateway_number(params, "level", 1);
      if (command->data.ambient_light.level < 0 ||
          command->data.ambient_light.level > 1)
        {
          snprintf(error, error_size, "ambient level must be between 0 and 1");
          return -ERANGE;
        }
    }
  else if (strcmp(action, "eyes.scene.show") == 0)
    {
      const char *scene = nyabula_gateway_string(params, "scene", NULL);
      const char *style = nyabula_gateway_string(params, "style", "full");
      cJSON *payload = nyabula_gateway_object_item(params, "payload");
      int scene_value = scene == NULL ? -1 : nyabula_gateway_lookup(
          g_nyabula_scenes, NYABULA_ARRAY_SIZE(g_nyabula_scenes), scene);
      int style_value = nyabula_gateway_lookup(
          g_nyabula_styles, NYABULA_ARRAY_SIZE(g_nyabula_styles), style);
      if (scene_value <= NYABULA_EYE_SCENE_NONE || style_value < 0 ||
          !cJSON_IsObject(payload))
        {
          snprintf(error, error_size, "valid scene, style and payload required");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_SCENE_SHOW;
      command->data.scene_show.request.scene = scene_value;
      command->data.scene_show.request.style = style_value;
      nyabula_gateway_parse_payload(
          payload, &command->data.scene_show.request.payload);
    }
  else if (strcmp(action, "eyes.scene.update") == 0)
    {
      cJSON *payload = nyabula_gateway_object_item(params, "payload");
      if (!cJSON_IsObject(payload))
        {
          snprintf(error, error_size, "payload object is required");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_SCENE_UPDATE;
      nyabula_gateway_parse_payload(payload,
                                    &command->data.scene_update.payload);
    }
  else if (strcmp(action, "eyes.scene.hide") == 0)
    {
      command->action = NYABULA_CORE_ACTION_SCENE_HIDE;
    }
  else if (strcmp(action, "core.release") == 0)
    {
      const char *domain = nyabula_gateway_string(params, "domain", "all");
      if (strcmp(domain, "expression") != 0 &&
          strcmp(domain, "scene") != 0 && strcmp(domain, "all") != 0)
        {
          snprintf(error, error_size,
                   "domain must be expression, scene or all");
          return -EINVAL;
        }

      command->action = NYABULA_CORE_ACTION_RELEASE;
      command->data.release.domains =
          strcmp(domain, "expression") == 0
              ? NYABULA_CORE_DOMAIN_EXPRESSION
          : strcmp(domain, "scene") == 0 ? NYABULA_CORE_DOMAIN_SCENE
                                          : NYABULA_CORE_DOMAIN_ALL;
    }
  else if (strcmp(action, "core.reset") == 0)
    {
      command->action = NYABULA_CORE_ACTION_RESET;
    }
  else
    {
      snprintf(error, error_size, "unknown action: %s", action);
      return -EINVAL;
    }

  return 0;
}

static int nyabula_gateway_send_all(int fd, const void *buffer, size_t size)
{
  const uint8_t *cursor = buffer;
  while (size > 0)
    {
      ssize_t sent = send(fd, cursor, size, 0);
      if (sent <= 0)
        {
          return -errno;
        }

      cursor += sent;
      size -= sent;
    }

  return 0;
}

static void nyabula_gateway_response(int fd, int status, const char *reason,
                                     const char *content_type,
                                     const void *body, size_t body_size)
{
  char header[512];
  int length = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Access-Control-Allow-Headers: Content-Type\r\n"
      "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
      status, reason, content_type, body_size);
  nyabula_gateway_send_all(fd, header, length);
  if (body_size > 0)
    {
      nyabula_gateway_send_all(fd, body, body_size);
    }
}

static void nyabula_gateway_json_response(int fd, int status,
                                          const char *reason, cJSON *json)
{
  char *body = cJSON_PrintUnformatted(json);
  if (body == NULL)
    {
      static const char failure[] = "{\"error\":\"out of memory\"}";
      nyabula_gateway_response(fd, 500, "Internal Server Error",
                               "application/json", failure,
                               sizeof(failure) - 1);
      return;
    }

  nyabula_gateway_response(fd, status, reason,
                           "application/json; charset=utf-8", body,
                           strlen(body));
  free(body);
}

static void nyabula_gateway_error(int fd, int status, const char *reason,
                                  const char *message)
{
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "ok", false);
  cJSON_AddStringToObject(json, "error", message);
  nyabula_gateway_json_response(fd, status, reason, json);
  cJSON_Delete(json);
}

static cJSON *nyabula_gateway_owner_json(
    const struct nyabula_core_owner_s *owner)
{
  cJSON *json = cJSON_CreateObject();
  cJSON_AddBoolToObject(json, "active", owner->active);
  cJSON_AddStringToObject(json, "source", owner->source);
  cJSON_AddNumberToObject(json, "priority", owner->priority);
  cJSON_AddNumberToObject(json, "lease_remaining_ms",
                          owner->lease_remaining_ms);
  cJSON_AddNumberToObject(json, "sequence", owner->sequence);
  return json;
}

static void nyabula_gateway_state(int fd, struct nyabula_core_s *core)
{
  struct nyabula_core_snapshot_s snapshot;
  cJSON *json = cJSON_CreateObject();
  cJSON *expression = cJSON_AddObjectToObject(json, "expression");
  cJSON *scene = cJSON_AddObjectToObject(json, "scene");
  cJSON *blink = cJSON_AddObjectToObject(json, "blink");
  cJSON *settings = cJSON_AddObjectToObject(json, "settings");
  cJSON *last = cJSON_AddObjectToObject(json, "last_command");

  nyabula_core_get_snapshot(core, &snapshot);
  cJSON_AddNumberToObject(json, "revision", snapshot.revision);
  cJSON_AddNumberToObject(json, "uptime_ms", snapshot.uptime_ms);
  cJSON_AddNumberToObject(json, "queue_depth", snapshot.queue_depth);
  cJSON_AddStringToObject(
      expression, "value",
      nyabula_gateway_name(g_nyabula_expressions,
                           NYABULA_ARRAY_SIZE(g_nyabula_expressions),
                           snapshot.expression));
  cJSON_AddItemToObject(expression, "owner",
                        nyabula_gateway_owner_json(
                            &snapshot.expression_owner));
  cJSON_AddStringToObject(
      scene, "name",
      nyabula_gateway_name(g_nyabula_scenes,
                           NYABULA_ARRAY_SIZE(g_nyabula_scenes),
                           snapshot.scene));
  cJSON_AddStringToObject(
      scene, "style",
      nyabula_gateway_name(g_nyabula_styles,
                           NYABULA_ARRAY_SIZE(g_nyabula_styles),
                           snapshot.scene_style));
  cJSON_AddItemToObject(scene, "payload",
                        nyabula_gateway_payload_json(&snapshot.scene_payload));
  cJSON_AddItemToObject(scene, "owner",
                        nyabula_gateway_owner_json(&snapshot.scene_owner));
  cJSON_AddNumberToObject(blink, "nonce", snapshot.blink_nonce);
  cJSON_AddStringToObject(
      blink, "eyes",
      snapshot.blink_eyes == NYABULA_EYE_MASK_LEFT
          ? "left"
          : snapshot.blink_eyes == NYABULA_EYE_MASK_RIGHT ? "right" : "both");
  cJSON_AddBoolToObject(settings, "auto_blink", snapshot.auto_blink);
  cJSON_AddNumberToObject(settings, "ambient_light", snapshot.ambient_light);
  cJSON *iris = cJSON_AddArrayToObject(settings, "iris_rgb");
  cJSON_AddItemToArray(iris, cJSON_CreateNumber(snapshot.iris_rgb[0]));
  cJSON_AddItemToArray(iris, cJSON_CreateNumber(snapshot.iris_rgb[1]));
  cJSON_AddStringToObject(last, "id", snapshot.last_request_id);
  cJSON_AddNumberToObject(last, "status", snapshot.last_status);
  cJSON_AddStringToObject(last, "error", snapshot.last_error);
  nyabula_gateway_json_response(fd, 200, "OK", json);
  cJSON_Delete(json);
}

static void nyabula_gateway_command(int fd, struct nyabula_core_s *core,
                                    const char *body)
{
  struct nyabula_core_command_s command;
  char error[128];
  cJSON *json = cJSON_Parse(body);
  cJSON *response;
  int ret;

  if (json == NULL)
    {
      nyabula_gateway_error(fd, 400, "Bad Request", "invalid JSON body");
      return;
    }

  ret = nyabula_gateway_parse_command(json, &command, error, sizeof(error));
  cJSON_Delete(json);
  if (ret < 0)
    {
      nyabula_gateway_error(fd, 422, "Unprocessable Entity", error);
      return;
    }

  ret = nyabula_core_submit(core, &command);
  if (ret < 0)
    {
      nyabula_gateway_error(fd, ret == -EAGAIN ? 503 : 500,
                            ret == -EAGAIN ? "Service Unavailable"
                                           : "Internal Server Error",
                            ret == -EAGAIN ? "command queue is full"
                                           : "failed to submit command");
      return;
    }

  response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "ok", true);
  cJSON_AddBoolToObject(response, "accepted", true);
  cJSON_AddStringToObject(response, "id", command.request_id);
  nyabula_gateway_json_response(fd, 202, "Accepted", response);
  cJSON_Delete(response);
}

static size_t nyabula_gateway_content_length(const char *headers)
{
  const char *line = headers;
  while ((line = strstr(line, "\r\n")) != NULL)
    {
      line += 2;
      if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
          return strtoul(line + 15, NULL, 10);
        }
    }

  return 0;
}

static void nyabula_gateway_handle_client(int fd,
                                          struct nyabula_core_s *core)
{
  char *request = malloc(NYABULA_GATEWAY_REQUEST_MAX + 1);
  char method[12];
  char path[128];
  char *header_end = NULL;
  size_t received = 0;
  size_t expected = 0;

  if (request == NULL)
    {
      nyabula_gateway_error(fd, 500, "Internal Server Error",
                            "out of memory");
      return;
    }

  while (received < NYABULA_GATEWAY_REQUEST_MAX)
    {
      ssize_t count = recv(fd, request + received,
                           NYABULA_GATEWAY_REQUEST_MAX - received, 0);
      if (count <= 0)
        {
          break;
        }

      received += count;
      request[received] = '\0';
      if (header_end == NULL)
        {
          header_end = strstr(request, "\r\n\r\n");
          if (header_end != NULL)
            {
              expected = (header_end + 4 - request) +
                         nyabula_gateway_content_length(request);
            }
        }

      if (header_end != NULL && received >= expected)
        {
          break;
        }
    }

  if (received == 0 || sscanf(request, "%11s %127s", method, path) != 2)
    {
      free(request);
      return;
    }

  if (strcmp(method, "OPTIONS") == 0)
    {
      nyabula_gateway_response(fd, 204, "No Content", "text/plain", "", 0);
    }
  else if (strcmp(method, "GET") == 0 &&
           (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
    {
      nyabula_gateway_response(fd, 200, "OK", "text/html; charset=utf-8",
                               g_nyabula_console_html,
                               g_nyabula_console_html_size);
    }
  else if (strcmp(method, "GET") == 0 &&
           strcmp(path, "/api/v1/health") == 0)
    {
      static const char health[] =
          "{\"ok\":true,\"name\":\"Nyabula Core\",\"api_version\":\"v1\"}";
      nyabula_gateway_response(fd, 200, "OK",
                               "application/json; charset=utf-8", health,
                               sizeof(health) - 1);
    }
  else if (strcmp(method, "GET") == 0 &&
           strcmp(path, "/api/v1/capabilities") == 0)
    {
      static const char capabilities[] =
          "{\"api_version\":\"v1\",\"transport\":\"http-json\","
          "\"actions\":[\"eyes.expression\",\"eyes.blink\",\"eyes.gaze\","
          "\"eyes.auto_blink\",\"eyes.ambient\",\"eyes.iris\","
          "\"eyes.scene.show\",\"eyes.scene.update\",\"eyes.scene.hide\","
          "\"core.release\",\"core.reset\"],"
          "\"expressions\":[\"idle\",\"curious\",\"happy\",\"processing\","
          "\"star\",\"heart\",\"sleepy\",\"sleep\",\"angry\",\"sad\","
          "\"surprise\",\"dizzy\",\"derp\"],"
          "\"scenes\":[\"music\",\"timer\",\"weather\",\"battery\","
          "\"alarm\",\"call\",\"task\",\"stopwatch\",\"calendar\","
          "\"sleep_timer\",\"network\",\"audio\",\"eq\",\"caption\","
          "\"briefing\",\"privacy\",\"identity\",\"memory\",\"devices\","
          "\"system\",\"health\",\"presence\",\"companion\",\"home\","
          "\"subwoofer\"],\"styles\":[\"full\",\"minimal\"],"
          "\"renderer_contract\":{\"version\":2,"
          "\"state_transition_ms\":420,\"numeric_transition_ms\":520,"
          "\"continuous_fields\":[\"duration_ms\",\"position_ms\","
          "\"remaining_ms\",\"elapsed_ms\",\"percent\",\"device_count\","
          "\"briefing_index\",\"briefing_count\",\"temperature_c\","
          "\"feels_like_c\",\"humidity_percent\",\"wind_kph\","
          "\"visibility_km\",\"distance_m\",\"heart_rate_bpm\","
          "\"crossover_hz\",\"progress\",\"eq_bands\"],"
          "\"clock_fields\":[],"
          "\"semantic_fallback\":"
          "\"center_scale_crossfade_all_other_fields\"},"
          "\"limits\":{\"priority_min\":0,\"priority_max\":255,"
          "\"queue_depth\":32,\"source_length\":23}}";
      nyabula_gateway_response(fd, 200, "OK",
                               "application/json; charset=utf-8", capabilities,
                               sizeof(capabilities) - 1);
    }
  else if (strcmp(method, "GET") == 0 &&
           strcmp(path, "/api/v1/state") == 0)
    {
      nyabula_gateway_state(fd, core);
    }
  else if (strcmp(method, "POST") == 0 &&
           strcmp(path, "/api/v1/command") == 0)
    {
      if (header_end == NULL)
        {
          nyabula_gateway_error(fd, 400, "Bad Request",
                                "incomplete HTTP request");
        }
      else
        {
          nyabula_gateway_command(fd, core, header_end + 4);
        }
    }
  else
    {
      nyabula_gateway_error(fd, 404, "Not Found", "route not found");
    }

  free(request);
}

static void *nyabula_gateway_thread(void *argument)
{
  struct nyabula_gateway_s *gateway = argument;
  struct sockaddr_in address;

  gateway->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (gateway->listen_fd < 0)
    {
      fprintf(stderr, "nyabula_gateway: socket failed: %d\n", errno);
      gateway->running = false;
      return NULL;
    }

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(gateway->port);
  if (bind(gateway->listen_fd, (struct sockaddr *)&address,
           sizeof(address)) < 0 ||
      listen(gateway->listen_fd, NYABULA_GATEWAY_BACKLOG) < 0)
    {
      fprintf(stderr, "nyabula_gateway: bind/listen failed: %d\n", errno);
      close(gateway->listen_fd);
      gateway->listen_fd = -1;
      gateway->running = false;
      return NULL;
    }

  printf("nyabula_gateway: listening on 0.0.0.0:%u\n", gateway->port);
  while (gateway->running)
    {
      int client = accept(gateway->listen_fd, NULL, NULL);
      if (client < 0)
        {
          if (gateway->running)
            {
              fprintf(stderr, "nyabula_gateway: accept failed: %d\n", errno);
            }
          continue;
        }

      nyabula_gateway_handle_client(client, gateway->core);
      close(client);
    }

  return NULL;
}

struct nyabula_gateway_s *
nyabula_gateway_start(struct nyabula_core_s *core, uint16_t port)
{
  struct nyabula_gateway_s *gateway;
  pthread_attr_t attributes;

  if (core == NULL || port == 0)
    {
      return NULL;
    }

  gateway = calloc(1, sizeof(*gateway));
  if (gateway == NULL)
    {
      return NULL;
    }

  gateway->core = core;
  gateway->port = port;
  gateway->listen_fd = -1;
  gateway->running = true;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(
      &attributes, CONFIG_CONTEST2026_062_NYABULA_GATEWAY_STACKSIZE);
  if (pthread_create(&gateway->thread, &attributes, nyabula_gateway_thread,
                     gateway) != 0)
    {
      pthread_attr_destroy(&attributes);
      free(gateway);
      return NULL;
    }

  pthread_attr_destroy(&attributes);

  return gateway;
}

void nyabula_gateway_stop(struct nyabula_gateway_s *gateway)
{
  if (gateway == NULL)
    {
      return;
    }

  gateway->running = false;
  if (gateway->listen_fd >= 0)
    {
      shutdown(gateway->listen_fd, SHUT_RDWR);
      close(gateway->listen_fd);
    }

  pthread_join(gateway->thread, NULL);
  free(gateway);
}
