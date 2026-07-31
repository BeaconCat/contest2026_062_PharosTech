/****************************************************************************
 * boards/rk3576/kickpi-k7/include/board.h
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

#ifndef __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H
#define __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Types
 ****************************************************************************/

#ifdef CONFIG_KICKPI_K7_AUDIO
enum kickpi_k7_audio_output_e
{
  KICKPI_K7_AUDIO_OUTPUT_AUTO = 0,
  KICKPI_K7_AUDIO_OUTPUT_HEADPHONES,
  KICKPI_K7_AUDIO_OUTPUT_SPEAKER,
  KICKPI_K7_AUDIO_OUTPUT_BOTH,
  KICKPI_K7_AUDIO_OUTPUT_OFF,
};

enum kickpi_k7_audio_channel_e
{
  KICKPI_K7_AUDIO_CHANNEL_STEREO = 0,
  KICKPI_K7_AUDIO_CHANNEL_MONO,
};
#endif

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_KICKPI_K7_AUDIO
/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   Initialize the on-board ES8388 codec and SAI1 interface, then register
 *   /dev/audio/pcm0.  Repeated calls are safe.
 *
 * Returned Value:
 *   Zero on success; a negated errno value on failure.
 ****************************************************************************/

int kickpi_k7_audio_initialize(void);
int kickpi_k7_audio_set_output(enum kickpi_k7_audio_output_e output);
enum kickpi_k7_audio_output_e kickpi_k7_audio_get_output(void);
bool kickpi_k7_audio_headphones_connected(void);
bool kickpi_k7_audio_headphone_detect_level(void);
int kickpi_k7_audio_set_channel(enum kickpi_k7_audio_channel_e channel);
enum kickpi_k7_audio_channel_e kickpi_k7_audio_get_channel(void);
int kickpi_k7_audio_set_swap(bool enable);
bool kickpi_k7_audio_get_swap(void);
int kickpi_k7_audio_set_polarity(bool invert_left, bool invert_right);
void kickpi_k7_audio_get_polarity(bool *invert_left, bool *invert_right);
#endif

#endif /* __ASSEMBLY__ */

#endif /* __BOARDS_ARM64_RK3576_KICKPI_K7_INCLUDE_BOARD_H */
