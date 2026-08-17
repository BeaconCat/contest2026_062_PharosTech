/****************************************************************************
 * app/nyabula/src/nyabula_main.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "../include/nyabula_eye_engine.h"
#include "../include/nyabula_core.h"
#include "../include/nyabula_gateway.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#define NYABULA_NEEDS_BOARD_INIT 1
#endif

#define NYABULA_DEMO_EXPRESSION_MS 3000
#define NYABULA_DEMO_SCENE_MS      4500

#ifndef CONFIG_CONTEST2026_062_NYABULA_GATEWAY_PORT
#define CONFIG_CONTEST2026_062_NYABULA_GATEWAY_PORT 8080
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const enum nyabula_eye_expression_e g_nyabula_demo_expressions[] = {
  NYABULA_EYE_EXPRESSION_IDLE,     NYABULA_EYE_EXPRESSION_CURIOUS,
  NYABULA_EYE_EXPRESSION_HAPPY,    NYABULA_EYE_EXPRESSION_PROCESSING,
  NYABULA_EYE_EXPRESSION_STAR,     NYABULA_EYE_EXPRESSION_HEART,
  NYABULA_EYE_EXPRESSION_SLEEPY,   NYABULA_EYE_EXPRESSION_SLEEP,
  NYABULA_EYE_EXPRESSION_ANGRY,    NYABULA_EYE_EXPRESSION_SAD,
  NYABULA_EYE_EXPRESSION_SURPRISE, NYABULA_EYE_EXPRESSION_DIZZY,
  NYABULA_EYE_EXPRESSION_DERP,
};

static const enum nyabula_eye_scene_e g_nyabula_demo_scenes[] = {
  NYABULA_EYE_SCENE_MUSIC,      NYABULA_EYE_SCENE_TIMER,
  NYABULA_EYE_SCENE_WEATHER,    NYABULA_EYE_SCENE_BATTERY,
  NYABULA_EYE_SCENE_ALARM,      NYABULA_EYE_SCENE_CALL,
  NYABULA_EYE_SCENE_TASK,       NYABULA_EYE_SCENE_STOPWATCH,
  NYABULA_EYE_SCENE_CALENDAR,   NYABULA_EYE_SCENE_SLEEP_TIMER,
  NYABULA_EYE_SCENE_NETWORK,    NYABULA_EYE_SCENE_AUDIO,
  NYABULA_EYE_SCENE_EQ,         NYABULA_EYE_SCENE_CAPTION,
  NYABULA_EYE_SCENE_BRIEFING,   NYABULA_EYE_SCENE_PRIVACY,
  NYABULA_EYE_SCENE_IDENTITY,   NYABULA_EYE_SCENE_MEMORY,
  NYABULA_EYE_SCENE_DEVICES,    NYABULA_EYE_SCENE_SYSTEM,
  NYABULA_EYE_SCENE_HEALTH,     NYABULA_EYE_SCENE_PRESENCE,
  NYABULA_EYE_SCENE_COMPANION,  NYABULA_EYE_SCENE_HOME,
  NYABULA_EYE_SCENE_SUBWOOFER,
};

enum nyabula_demo_mode_e
{
  NYABULA_DEMO_IDLE = 0,
  NYABULA_DEMO_EXPRESSIONS,
  NYABULA_DEMO_SCENES
};

struct nyabula_demo_s
{
  struct nyabula_eye_engine_s *engine;
  size_t index;
  enum nyabula_demo_mode_e mode;
};

static void nyabula_demo_scene_request(
    enum nyabula_eye_scene_e scene,
    struct nyabula_eye_scene_request_s *request)
{
  memset(request, 0, sizeof(*request));
  request->scene = scene;
  request->style = NYABULA_EYE_SCENE_STYLE_FULL;
  request->payload.duration_ms = 300000;
  request->payload.remaining_ms = 300000;
  request->payload.weather = NYABULA_EYE_WEATHER_RAIN;
  request->payload.music_view = NYABULA_EYE_MUSIC_SPECTRUM;
  request->payload.battery_state = NYABULA_EYE_BATTERY_CHARGING;
  request->payload.alarm_copy = NYABULA_EYE_ALARM_COPY_NAME;
  request->payload.call_state = NYABULA_EYE_CALL_INCOMING;
  request->payload.task_state = NYABULA_EYE_TASK_RUNNING;
  request->payload.network_state = NYABULA_EYE_NETWORK_WIFI;
  request->payload.audio_route = NYABULA_EYE_AUDIO_SPEAKER;
  request->payload.eq_view = NYABULA_EYE_EQ_PROFILE;
  request->payload.hour = 7;
  request->payload.minute = 30;
  request->payload.percent = 68;
  request->payload.year = 2026;
  request->payload.month = 8;
  request->payload.day = 16;
  request->payload.device_count = 2;
  request->payload.briefing_index = 1;
  request->payload.briefing_count = 3;
  request->payload.temperature_c = 23.0f;
  request->payload.feels_like_c = 22.0f;
  request->payload.humidity_percent = 78.0f;
  request->payload.wind_kph = 24.0f;
  request->payload.visibility_km = 0.8f;
  request->payload.distance_m = 0.8f;
  request->payload.heart_rate_bpm = 72.0f;
  request->payload.crossover_hz = 80.0f;
  request->payload.active = true;
  request->payload.playing = true;
  request->payload.privacy_camera = true;
  request->payload.privacy_microphone = true;
  request->payload.signal_good = true;

  if (scene == NYABULA_EYE_SCENE_MUSIC)
    {
      request->payload.duration_ms = 235000;
    }
  else if (scene == NYABULA_EYE_SCENE_CALENDAR)
    {
      request->payload.hour = 9;
      request->payload.minute = 30;
    }
  else if (scene == NYABULA_EYE_SCENE_SLEEP_TIMER)
    {
      request->payload.remaining_ms = 1800000;
    }
}

static void nyabula_demo_timer_cb(lv_timer_t *timer)
{
  struct nyabula_demo_s *demo = lv_timer_get_user_data(timer);

  if (demo->mode == NYABULA_DEMO_SCENES)
    {
      struct nyabula_eye_scene_request_s request;
      demo->index = (demo->index + 1) %
                    (sizeof(g_nyabula_demo_scenes) /
                     sizeof(g_nyabula_demo_scenes[0]));
      nyabula_demo_scene_request(g_nyabula_demo_scenes[demo->index],
                                 &request);
      nyabula_eye_engine_show_scene(demo->engine, &request);
    }
  else
    {
      demo->index =
          (demo->index + 1) % (sizeof(g_nyabula_demo_expressions) /
                               sizeof(g_nyabula_demo_expressions[0]));
      nyabula_eye_engine_set_expression(
          demo->engine, g_nyabula_demo_expressions[demo->index], 0);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct nyabula_eye_engine_s *eye_engine;
  lv_nuttx_dsc_t descriptor;
  lv_nuttx_result_t result;
  lv_obj_t *screen;
  struct nyabula_demo_s demo;
  struct nyabula_core_s *core = NULL;
  struct nyabula_gateway_s *gateway = NULL;
  lv_timer_t *demo_timer = NULL;
  enum nyabula_demo_mode_e demo_mode =
      argc > 1 && strcmp(argv[1], "demo") == 0
          ? NYABULA_DEMO_EXPRESSIONS
          : argc > 1 && strcmp(argv[1], "scenes") == 0
                ? NYABULA_DEMO_SCENES
                : NYABULA_DEMO_IDLE;
  bool core_mode = argc <= 1 || strcmp(argv[1], "core") == 0;

  if (lv_is_initialized())
    {
      fprintf(stderr, "nyabula: LVGL is already initialized\n");
      return EBUSY;
    }

#ifdef NYABULA_NEEDS_BOARD_INIT
  boardctl(BOARDIOC_INIT, 0);
#endif

  lv_init();
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.fb_path = "/dev/fb0";
  lv_nuttx_init(&descriptor, &result);
  if (result.disp == NULL)
    {
      fprintf(stderr, "nyabula: failed to initialize the display\n");
      lv_deinit();
      return ENODEV;
    }

  screen = lv_screen_active();
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x03070b), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  eye_engine = nyabula_eye_engine_create(screen);
  if (eye_engine == NULL)
    {
      fprintf(stderr, "nyabula: failed to create the eye engine\n");
      lv_nuttx_deinit(&result);
      lv_deinit();
      return ENOMEM;
    }

  demo.engine = eye_engine;
  demo.index = 0;
  demo.mode = demo_mode;
  if (demo_mode != NYABULA_DEMO_IDLE)
    {
      uint32_t period = demo_mode == NYABULA_DEMO_SCENES
                            ? NYABULA_DEMO_SCENE_MS
                            : NYABULA_DEMO_EXPRESSION_MS;
      if (demo_mode == NYABULA_DEMO_SCENES)
        {
          struct nyabula_eye_scene_request_s request;
          nyabula_demo_scene_request(g_nyabula_demo_scenes[0], &request);
          nyabula_eye_engine_show_scene(eye_engine, &request);
        }
      demo_timer = lv_timer_create(nyabula_demo_timer_cb,
                                   period, &demo);
      if (demo_timer == NULL)
        {
          fprintf(stderr, "nyabula: failed to create the demo timer\n");
          nyabula_eye_engine_destroy(eye_engine);
          lv_nuttx_deinit(&result);
          lv_deinit();
          return ENOMEM;
        }
    }

#ifdef CONFIG_CONTEST2026_062_NYABULA_GATEWAY
  if (core_mode)
    {
      core = nyabula_core_create(eye_engine);
      if (core == NULL)
        {
          fprintf(stderr, "nyabula: failed to create Nyabula Core\n");
          nyabula_eye_engine_destroy(eye_engine);
          lv_nuttx_deinit(&result);
          lv_deinit();
          return ENOMEM;
        }

      gateway = nyabula_gateway_start(
          core, CONFIG_CONTEST2026_062_NYABULA_GATEWAY_PORT);
      if (gateway == NULL)
        {
          fprintf(stderr, "nyabula: failed to start the control gateway\n");
          nyabula_core_destroy(core);
          nyabula_eye_engine_destroy(eye_engine);
          lv_nuttx_deinit(&result);
          lv_deinit();
          return EIO;
        }
    }
#else
  if (core_mode)
    {
      fprintf(stderr, "nyabula: control gateway is not enabled\n");
    }
#endif

  while (1)
    {
      if (core != NULL)
        {
          nyabula_core_tick(core);
        }

      uint32_t idle = lv_timer_handler();

      usleep((idle == 0 ? 1 : idle) * 1000);
    }

#ifdef CONFIG_CONTEST2026_062_NYABULA_GATEWAY
  nyabula_gateway_stop(gateway);
#endif
  nyabula_core_destroy(core);
  nyabula_eye_engine_destroy(eye_engine);
  lv_nuttx_deinit(&result);
  lv_deinit();
  return 0;
}
