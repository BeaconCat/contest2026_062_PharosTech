/****************************************************************************
 * apps/system/audioctl/audioctl_main.c
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
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/board/board.h>

struct audioctl_mode_s
{
  FAR const char *name;
  enum kickpi_k7_audio_output_e output;
};

static const struct audioctl_mode_s g_audioctl_modes[] = {
  { "auto", KICKPI_K7_AUDIO_OUTPUT_AUTO },
  { "headphones", KICKPI_K7_AUDIO_OUTPUT_HEADPHONES },
  { "speaker", KICKPI_K7_AUDIO_OUTPUT_SPEAKER },
  { "both", KICKPI_K7_AUDIO_OUTPUT_BOTH },
  { "off", KICKPI_K7_AUDIO_OUTPUT_OFF },
};

#define AUDIOCTL_NMODES \
  ((int)(sizeof(g_audioctl_modes) / sizeof(g_audioctl_modes[0])))

int main(int argc, FAR char *argv[])
{
  enum kickpi_k7_audio_output_e output;
  enum kickpi_k7_audio_channel_e channel;
  bool invert_left;
  bool invert_right;
  FAR const char *name = "unknown";
  int ret;
  int i;

  if (argc > 4)
    {
      fprintf(stderr, "usage: audioctl [auto|headphones|speaker|both|off]\n"
                      "       audioctl mono [on|off]\n"
                      "       audioctl swap [on|off]\n"
                      "       audioctl polarity [left|right] "
                      "[normal|invert]\n");
      return EXIT_FAILURE;
    }
  else if (argc >= 2 && strcmp(argv[1], "swap") == 0)
    {
      if (argc != 3 ||
          (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0))
        {
          fprintf(stderr, "usage: audioctl swap [on|off]\n");
          return EXIT_FAILURE;
        }

      ret = kickpi_k7_audio_set_swap(strcmp(argv[2], "on") == 0);
      if (ret < 0)
        {
          fprintf(stderr, "audioctl: set swap failed: %d\n", ret);
          return EXIT_FAILURE;
        }
    }
  else if (argc >= 2 && strcmp(argv[1], "polarity") == 0)
    {
      if (argc != 4 ||
          (strcmp(argv[2], "left") != 0 && strcmp(argv[2], "right") != 0) ||
          (strcmp(argv[3], "normal") != 0 && strcmp(argv[3], "invert") != 0))
        {
          fprintf(stderr, "usage: audioctl polarity [left|right] "
                          "[normal|invert]\n");
          return EXIT_FAILURE;
        }

      kickpi_k7_audio_get_polarity(&invert_left, &invert_right);
      if (strcmp(argv[2], "left") == 0)
        {
          invert_left = strcmp(argv[3], "invert") == 0;
        }
      else
        {
          invert_right = strcmp(argv[3], "invert") == 0;
        }

      ret = kickpi_k7_audio_set_polarity(invert_left, invert_right);
      if (ret < 0)
        {
          fprintf(stderr, "audioctl: set polarity failed: %d\n", ret);
          return EXIT_FAILURE;
        }
    }

  else if (argc >= 2 && strcmp(argv[1], "mono") == 0)
    {
      if (argc != 3 ||
          (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0))
        {
          fprintf(stderr, "usage: audioctl mono [on|off]\n");
          return EXIT_FAILURE;
        }

      ret = kickpi_k7_audio_set_channel(strcmp(argv[2], "on") == 0
                                            ? KICKPI_K7_AUDIO_CHANNEL_MONO
                                            : KICKPI_K7_AUDIO_CHANNEL_STEREO);
      if (ret < 0)
        {
          fprintf(stderr, "audioctl: set mono failed: %d\n", ret);
          return EXIT_FAILURE;
        }
    }
  else if (argc == 2)
    {
      for (i = 0; i < AUDIOCTL_NMODES; i++)
        {
          if (strcmp(argv[1], g_audioctl_modes[i].name) == 0)
            {
              ret = kickpi_k7_audio_set_output(g_audioctl_modes[i].output);
              if (ret < 0)
                {
                  fprintf(stderr, "audioctl: set failed: %d\n", ret);
                  return EXIT_FAILURE;
                }

              break;
            }
        }

      if (i == AUDIOCTL_NMODES)
        {
          fprintf(stderr, "audioctl: unknown mode: %s\n", argv[1]);
          return EXIT_FAILURE;
        }
    }

  output = kickpi_k7_audio_get_output();
  for (i = 0; i < AUDIOCTL_NMODES; i++)
    {
      if (output == g_audioctl_modes[i].output)
        {
          name = g_audioctl_modes[i].name;
          break;
        }
    }

  channel = kickpi_k7_audio_get_channel();
  kickpi_k7_audio_get_polarity(&invert_left, &invert_right);
  printf("mode=%s channel=%s swap=%s polarity_l=%s polarity_r=%s "
         "headphones=%s hp_level=%s\n",
         name, channel == KICKPI_K7_AUDIO_CHANNEL_MONO ? "mono" : "stereo",
         kickpi_k7_audio_get_swap() ? "on" : "off",
         invert_left ? "invert" : "normal", invert_right ? "invert" : "normal",
         kickpi_k7_audio_headphones_connected() ? "connected" : "disconnected",
         kickpi_k7_audio_headphone_detect_level() ? "high" : "low");
  return EXIT_SUCCESS;
}
