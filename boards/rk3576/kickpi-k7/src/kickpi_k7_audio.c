/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_audio.c
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

/****************************************************************************
 * On-board audio bring-up: wire the RK3576 SAI1 I2S controller
 * (i2s_dev_s, PL330 DMA data path) to the on-board Everest ES8388 codec
 * (I2C3, 7-bit address 0x10) and register the combined PCM device as
 * /dev/audio/pcm0.  The SAI1 signal group (MCLK/SCLK/LRCK/SDO0/SDI0) is
 * muxed here since there is no pinctrl framework yet.  SAI1 is the I2S
 * master and provides MCLK to the codec.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <errno.h>
#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8388.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/wqueue.h>

#include <arch/board/board.h>

#include "arm64_internal.h"
#include "kickpi_k7.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"
#include "rk3576_sai.h"

#ifdef CONFIG_KICKPI_K7_AUDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define KICKPI_K7_ES8388_I2C_BUS    3      /* ES8388 on I2C3 (2ac60000) */
#define KICKPI_K7_ES8388_I2C_ADDR   0x10   /* DTS es8388@10             */
#define KICKPI_K7_ES8388_I2C_FREQ   100000 /* 100 kHz control clock     */
#define KICKPI_K7_SAI_BUS           1      /* SAI1                      */
#define KICKPI_K7_PCM_DEVNAME       "pcm0"

#define KICKPI_K7_HP_DETECT_POLL_MS 100
#define KICKPI_K7_HP_DEBOUNCE_COUNT 2
#define KICKPI_K7_SPK_ENABLE_MS     30
#define KICKPI_K7_SPK_DISABLE_MS    40

/* GPIO0_D3 is high when a headphone plug is present.  GPIO4_B1 enables the
 * external speaker amplifier when driven high.
 */

#define KICKPI_K7_HP_DETECT \
  (GPIO_PORT0 | GPIO_PIN_D3 | GPIO_INPUT | GPIO_PULLUP)
#define KICKPI_K7_SPK_ENABLE \
  (GPIO_PORT4 | GPIO_PIN_B1 | GPIO_OUTPUT | GPIO_PULLDOWN)

/* SYS_GRF_SOC_CON18 bit 1 routes SAI1 MCLK to the selected IO pin. */

#define KICKPI_K7_SYS_GRF_SOC_CON18 0x26046400
#define KICKPI_K7_SAI1_MCLKOUT_BIT  (1u << 1)

/* SAI1 default signal group (DTS sai1m0, all GPIO4 bank, alt func 1):
 * MCLK=GPIO4_A2, SCLK=GPIO4_A3, LRCK=GPIO4_A5, SDO0=GPIO4_A7, SDI0=GPIO4_B3.
 */

#define KICKPI_K7_SAI1_MCLK (GPIO_PORT4 | GPIO_PIN_A2 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SCLK (GPIO_PORT4 | GPIO_PIN_A3 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_LRCK (GPIO_PORT4 | GPIO_PIN_A5 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SDO0 (GPIO_PORT4 | GPIO_PIN_A7 | GPIO_ALT | GPIO_AF1)
#define KICKPI_K7_SAI1_SDI0 (GPIO_PORT4 | GPIO_PIN_B3 | GPIO_ALT | GPIO_AF1)

/* ES8388 control bus: I2C3 default group (DTS i2c3m0-xfer, GPIO4 bank, alt
 * func 11): SCL=GPIO4_B4, SDA=GPIO4_B5.
 */

#define KICKPI_K7_I2C3_SCL (GPIO_PORT4 | GPIO_PIN_B4 | GPIO_ALT | GPIO_AF11)
#define KICKPI_K7_I2C3_SDA (GPIO_PORT4 | GPIO_PIN_B5 | GPIO_ALT | GPIO_AF11)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static void kickpi_k7_audio_set_output_power(enum es8388_output_route_e route,
                                             bool enable);

static const struct es8388_lower_s g_es8388_lower = {
  .frequency = KICKPI_K7_ES8388_I2C_FREQ,
  .address = KICKPI_K7_ES8388_I2C_ADDR,
  .set_output = kickpi_k7_audio_set_output_power,
};

static bool g_audio_initialized;
static bool g_headphones_connected;
static bool g_headphones_sample;
static uint8_t g_headphones_debounce;
static enum kickpi_k7_audio_output_e g_audio_output;
static enum kickpi_k7_audio_channel_e g_audio_channel;
static bool g_audio_swap;
static bool g_audio_invert_left;
static bool g_audio_invert_right;
static struct audio_lowerhalf_s *g_es8388;
static struct audio_lowerhalf_s *g_pcm;
static struct work_s g_headphone_work;
static mutex_t g_audio_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool kickpi_k7_audio_read_headphones(void)
{
  return rk3576_gpio_read(KICKPI_K7_HP_DETECT);
}

static void kickpi_k7_audio_set_output_power(enum es8388_output_route_e route,
                                             bool enable)
{
  bool speaker = enable && (route == ES8388_OUTPUT_ROUTE_LINE2 ||
                            route == ES8388_OUTPUT_ROUTE_BOTH);

  if (speaker)
    {
      usleep(KICKPI_K7_SPK_ENABLE_MS * USEC_PER_MSEC);
    }

  rk3576_gpio_write(KICKPI_K7_SPK_ENABLE, speaker);

  if (!speaker)
    {
      usleep(KICKPI_K7_SPK_DISABLE_MS * USEC_PER_MSEC);
    }
}

static enum kickpi_k7_audio_output_e kickpi_k7_audio_default_output(void)
{
#if defined(CONFIG_KICKPI_K7_AUDIO_OUTPUT_HEADPHONES)
  return KICKPI_K7_AUDIO_OUTPUT_HEADPHONES;
#elif defined(CONFIG_KICKPI_K7_AUDIO_OUTPUT_SPEAKER)
  return KICKPI_K7_AUDIO_OUTPUT_SPEAKER;
#elif defined(CONFIG_KICKPI_K7_AUDIO_OUTPUT_BOTH)
  return KICKPI_K7_AUDIO_OUTPUT_BOTH;
#elif defined(CONFIG_KICKPI_K7_AUDIO_OUTPUT_OFF)
  return KICKPI_K7_AUDIO_OUTPUT_OFF;
#else
  return KICKPI_K7_AUDIO_OUTPUT_AUTO;
#endif
}

static int kickpi_k7_audio_apply_output(void)
{
  enum es8388_output_route_e route;
  int ret;

  switch (g_audio_output)
    {
      case KICKPI_K7_AUDIO_OUTPUT_AUTO:
        route = g_headphones_connected ? ES8388_OUTPUT_ROUTE_LINE1
                                       : ES8388_OUTPUT_ROUTE_LINE2;
        break;

      case KICKPI_K7_AUDIO_OUTPUT_HEADPHONES:
        route = ES8388_OUTPUT_ROUTE_LINE1;
        break;

      case KICKPI_K7_AUDIO_OUTPUT_SPEAKER:
        route = ES8388_OUTPUT_ROUTE_LINE2;
        break;

      case KICKPI_K7_AUDIO_OUTPUT_BOTH:
        route = ES8388_OUTPUT_ROUTE_BOTH;
        break;

      case KICKPI_K7_AUDIO_OUTPUT_OFF:
        route = ES8388_OUTPUT_ROUTE_NONE;
        break;

      default:
        return -EINVAL;
    }

  ret = es8388_set_output_route(g_es8388, route);
  if (ret < 0)
    {
      return ret;
    }

  return OK;
}

static void kickpi_k7_audio_headphone_worker(void *arg)
{
  bool sample = kickpi_k7_audio_read_headphones();
  int ret;

  (void)arg;

  if (sample != g_headphones_sample)
    {
      g_headphones_sample = sample;
      g_headphones_debounce = 1;
    }
  else if (g_headphones_debounce < KICKPI_K7_HP_DEBOUNCE_COUNT)
    {
      g_headphones_debounce++;
    }

  if (g_headphones_debounce == KICKPI_K7_HP_DEBOUNCE_COUNT &&
      sample != g_headphones_connected)
    {
      ret = nxmutex_lock(&g_audio_lock);
      if (ret < 0)
        {
          goto reschedule;
        }

      g_headphones_connected = sample;
      if (g_audio_initialized && g_audio_output == KICKPI_K7_AUDIO_OUTPUT_AUTO)
        {
          kickpi_k7_audio_apply_output();
        }

      nxmutex_unlock(&g_audio_lock);
    }

reschedule:
  work_queue(LPWORK, &g_headphone_work, kickpi_k7_audio_headphone_worker, NULL,
             MSEC2TICK(KICKPI_K7_HP_DETECT_POLL_MS));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_audio_initialize
 *
 * Description:
 *   Mux the SAI1 pins, bring up the SAI1 I2S master and the ES8388 codec
 *   over I2C3, and register the PCM device (/dev/audio/pcm0).
 *
 * Returned Value:
 *   OK on success; a negated errno on failure.
 *
 ****************************************************************************/

int kickpi_k7_audio_initialize(void)
{
  struct audio_lowerhalf_s *pcm;
  struct i2c_master_s *i2c;
  struct i2s_dev_s *i2s;
  int ret;

  if (g_audio_initialized)
    {
      return OK;
    }

  /* Keep the amplifier disabled before touching clocks or the codec. */

  rk3576_config_gpio(KICKPI_K7_SPK_ENABLE);
  rk3576_gpio_write(KICKPI_K7_SPK_ENABLE, false);
  rk3576_config_gpio(KICKPI_K7_HP_DETECT);
  g_headphones_connected = kickpi_k7_audio_read_headphones();
  g_headphones_sample = g_headphones_connected;
  g_headphones_debounce = KICKPI_K7_HP_DEBOUNCE_COUNT;
  g_audio_output = kickpi_k7_audio_default_output();
  g_audio_channel = KICKPI_K7_AUDIO_CHANNEL_STEREO;

  /* Mux the SAI1 signal group. */

  rk3576_config_gpio(KICKPI_K7_SAI1_MCLK);
  rk3576_config_gpio(KICKPI_K7_SAI1_SCLK);
  rk3576_config_gpio(KICKPI_K7_SAI1_LRCK);
  rk3576_config_gpio(KICKPI_K7_SAI1_SDO0);
  rk3576_config_gpio(KICKPI_K7_SAI1_SDI0);

  /* This K7-specific hiword-masked route is board policy, not a property of

   * * the SAI controller.  Clearing the bit enables MCLK output; hardware
   *
   * readback is 0x1d when the route is active.
   */

  putreg32(KICKPI_K7_SAI1_MCLKOUT_BIT << 16, KICKPI_K7_SYS_GRF_SOC_CON18);

  /* Bring up the I2C3 control bus and the SAI1 I2S data interface. */

  /* Mux the I2C3 control bus (SCL/SDA). */

  rk3576_config_gpio(KICKPI_K7_I2C3_SCL);
  rk3576_config_gpio(KICKPI_K7_I2C3_SDA);

  i2c = rk3576_i2c_initialize(KICKPI_K7_ES8388_I2C_BUS);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: I2C%d init failed\n", KICKPI_K7_ES8388_I2C_BUS);
      return -ENODEV;
    }

  i2s = rk3576_sai_initialize(KICKPI_K7_SAI_BUS);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: SAI%d init failed\n", KICKPI_K7_SAI_BUS);
      return -ENODEV;
    }

  /* Bind the codec to the I2C/I2S interfaces, wrap it in the PCM decoder
   * (so it accepts WAV/PCM streams) and register the audio device.
   */

  g_es8388 = es8388_initialize(i2c, i2s, &g_es8388_lower);
  if (g_es8388 == NULL)
    {
      syslog(LOG_ERR, "ERROR: es8388_initialize failed\n");
      return -ENODEV;
    }

  ret = kickpi_k7_audio_apply_output();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio route init failed: %d\n", ret);
      return ret;
    }

  pcm = pcm_decode_initialize(g_es8388);
  if (pcm == NULL)
    {
      syslog(LOG_ERR, "ERROR: pcm_decode_initialize failed\n");
      return -ENODEV;
    }

  g_pcm = pcm;

  ret = audio_register(KICKPI_K7_PCM_DEVNAME, pcm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: audio_register(%s) failed: %d\n",
             KICKPI_K7_PCM_DEVNAME, ret);
      return ret;
    }

  syslog(LOG_INFO, "INFO: audio ready: /dev/audio/%s (ES8388 on SAI1)\n",
         KICKPI_K7_PCM_DEVNAME);
  g_audio_initialized = true;
  work_queue(LPWORK, &g_headphone_work, kickpi_k7_audio_headphone_worker, NULL,
             MSEC2TICK(KICKPI_K7_HP_DETECT_POLL_MS));
  return OK;
}

int kickpi_k7_audio_set_output(enum kickpi_k7_audio_output_e output)
{
  enum kickpi_k7_audio_output_e previous;
  int ret;

  if (output > KICKPI_K7_AUDIO_OUTPUT_OFF)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_audio_initialized)
    {
      ret = -EAGAIN;
    }
  else
    {
      previous = g_audio_output;
      g_audio_output = output;
      ret = kickpi_k7_audio_apply_output();
      if (ret < 0)
        {
          g_audio_output = previous;
        }
    }

  nxmutex_unlock(&g_audio_lock);
  return ret;
}

enum kickpi_k7_audio_output_e kickpi_k7_audio_get_output(void)
{
  enum kickpi_k7_audio_output_e output;
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return KICKPI_K7_AUDIO_OUTPUT_OFF;
    }

  output = g_audio_output;
  nxmutex_unlock(&g_audio_lock);
  return output;
}

bool kickpi_k7_audio_headphones_connected(void)
{
  bool connected;
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return false;
    }

  connected = g_headphones_connected;
  nxmutex_unlock(&g_audio_lock);
  return connected;
}

bool kickpi_k7_audio_headphone_detect_level(void)
{
  return rk3576_gpio_read(KICKPI_K7_HP_DETECT);
}

int kickpi_k7_audio_set_channel(enum kickpi_k7_audio_channel_e channel)
{
  enum kickpi_k7_audio_channel_e previous;
  int ret;

  if (channel > KICKPI_K7_AUDIO_CHANNEL_MONO)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_audio_initialized)
    {
      ret = -EAGAIN;
    }
  else
    {
      previous = g_audio_channel;
      ret =
          pcm_decode_set_mono(g_pcm, channel == KICKPI_K7_AUDIO_CHANNEL_MONO);
      if (ret >= 0)
        {
          g_audio_channel = channel;
        }
      else
        {
          g_audio_channel = previous;
        }
    }

  nxmutex_unlock(&g_audio_lock);
  return ret;
}

enum kickpi_k7_audio_channel_e kickpi_k7_audio_get_channel(void)
{
  enum kickpi_k7_audio_channel_e channel;
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return KICKPI_K7_AUDIO_CHANNEL_STEREO;
    }

  channel = g_audio_channel;
  nxmutex_unlock(&g_audio_lock);
  return channel;
}

int kickpi_k7_audio_set_swap(bool enable)
{
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = g_audio_initialized ? pcm_decode_set_swap(g_pcm, enable) : -EAGAIN;
  if (ret >= 0)
    {
      g_audio_swap = enable;
    }

  nxmutex_unlock(&g_audio_lock);
  return ret;
}

bool kickpi_k7_audio_get_swap(void)
{
  bool enable;
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return false;
    }

  enable = g_audio_swap;
  nxmutex_unlock(&g_audio_lock);
  return enable;
}

int kickpi_k7_audio_set_polarity(bool invert_left, bool invert_right)
{
  int ret;

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = g_audio_initialized
            ? pcm_decode_set_polarity(g_pcm, invert_left, invert_right)
            : -EAGAIN;
  if (ret >= 0)
    {
      g_audio_invert_left = invert_left;
      g_audio_invert_right = invert_right;
    }

  nxmutex_unlock(&g_audio_lock);
  return ret;
}

void kickpi_k7_audio_get_polarity(bool *invert_left, bool *invert_right)
{
  int ret;

  if (invert_left != NULL)
    {
      *invert_left = false;
    }

  if (invert_right != NULL)
    {
      *invert_right = false;
    }

  ret = nxmutex_lock(&g_audio_lock);
  if (ret < 0)
    {
      return;
    }

  if (invert_left != NULL)
    {
      *invert_left = g_audio_invert_left;
    }

  if (invert_right != NULL)
    {
      *invert_right = g_audio_invert_right;
    }

  nxmutex_unlock(&g_audio_lock);
}

#endif /* CONFIG_KICKPI_K7_AUDIO */
