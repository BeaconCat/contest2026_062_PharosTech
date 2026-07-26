/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_isp.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_ISP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_ISP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_ISP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Frame geometry limits accepted by rk3576_isp_set_format() */

#define RK3576_ISP_MIN_WIDTH   64
#define RK3576_ISP_MIN_HEIGHT  64
#define RK3576_ISP_MAX_WIDTH   4672
#define RK3576_ISP_MAX_HEIGHT  3504

/* Ping-pong output buffers owned by the driver */

#define RK3576_ISP_NBUFFERS    2

/* White balance gains are U4.8: 0x100 is unity, 0xfff the maximum */

#define RK3576_ISP_WB_UNITY    0x100
#define RK3576_ISP_WB_MAX      0xfff

/* Number of sample points in the output gamma curve */

#define RK3576_ISP_GAMMA_POINTS 17

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Bayer colour filter array phase of the incoming raw stream. */

enum rk3576_isp_bayer_e
{
  RK3576_ISP_BAYER_RGGB = 0,
  RK3576_ISP_BAYER_GRBG = 1,
  RK3576_ISP_BAYER_GBRG = 2,
  RK3576_ISP_BAYER_BGGR = 3
};

/* Raw input bit depth. */

enum rk3576_isp_rawdepth_e
{
  RK3576_ISP_RAW8 = 8,
  RK3576_ISP_RAW10 = 10,
  RK3576_ISP_RAW12 = 12
};

/* Pipeline configuration.  The output is always NV12 (semi-planar 4:2:0)
 * at the same resolution as the input window; scaling belongs to the VPSS
 * block, which this driver does not touch.
 */

struct rk3576_isp_format_s
{
  uint16_t width;   /* Active pixels per line                            */
  uint16_t height;  /* Active lines per frame                            */
  uint8_t bayer;    /* enum rk3576_isp_bayer_e                           */
  uint8_t depth;    /* enum rk3576_isp_rawdepth_e                        */
};

/* Static white balance gains, U4.8 (RK3576_ISP_WB_UNITY == 1.0x). */

struct rk3576_isp_wbgain_s
{
  uint16_t r;
  uint16_t gr;
  uint16_t gb;
  uint16_t b;
};

/* Frame-complete callback.  Runs in interrupt context and must not block.
 * The NV12 frame stays owned by the driver and returns to the hardware as
 * soon as the callback returns.
 */

typedef void (*rk3576_isp_frame_cb_t)(void *arg, void *buf, size_t size,
                                      uint32_t seq);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_isp_initialize
 *
 * Description:
 *   Power up the VI domain, enable the ISP clocks, park the pipeline and
 *   attach the ISP and memory-interface interrupt handlers.  Must run
 *   after rk3576_clk_tree_initialize().  Repeated calls are harmless.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_initialize(void);

/****************************************************************************
 * Name: rk3576_isp_set_format
 *
 * Description:
 *   Program the acquisition window, the Bayer phase and the raw bit depth,
 *   configure the minimum viable processing chain (black level, demosaic,
 *   white balance, gamma, YUV conversion) and allocate the NV12 output
 *   buffers.  Cannot be called while streaming.
 *
 * Input Parameters:
 *   fmt - Requested pipeline configuration
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_set_format(const struct rk3576_isp_format_s *fmt);

/****************************************************************************
 * Name: rk3576_isp_get_framesize
 *
 * Description:
 *   Return the size in bytes of one NV12 output frame for the format that
 *   is currently programmed.
 *
 * Returned Value:
 *   Frame size in bytes, or 0 if no format has been set yet.
 *
 ****************************************************************************/

size_t rk3576_isp_get_framesize(void);

/****************************************************************************
 * Name: rk3576_isp_set_black_level
 *
 * Description:
 *   Set the fixed black level subtracted from each of the four Bayer
 *   positions (A = first pixel of the first line, then B, C, D in raster
 *   order).  Values are in raw sensor counts.  Takes effect on the next
 *   shadow-register latch.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_set_black_level(uint16_t a, uint16_t b, uint16_t c,
                               uint16_t d);

/****************************************************************************
 * Name: rk3576_isp_set_wbgain
 *
 * Description:
 *   Apply static white balance gains.  There is no automatic white
 *   balance loop yet, so the caller supplies the gains (a fixed daylight
 *   preset is installed by rk3576_isp_set_format()).
 *
 * Input Parameters:
 *   gain - Per-channel U4.8 gains
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_set_wbgain(const struct rk3576_isp_wbgain_s *gain);

/****************************************************************************
 * Name: rk3576_isp_set_gamma
 *
 * Description:
 *   Load the output gamma curve.  The curve holds
 *   RK3576_ISP_GAMMA_POINTS equidistant 10-bit sample points; passing
 *   NULL restores the built-in approximation of the sRGB transfer curve.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_set_gamma(const uint16_t *curve);

/****************************************************************************
 * Name: rk3576_isp_start_streaming
 *
 * Description:
 *   Enable the pipeline and the memory interface.  The callback fires
 *   once per processed frame from interrupt context.
 *
 * Input Parameters:
 *   callback - Frame-complete callback, must not be NULL
 *   arg      - Opaque value passed back to the callback
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_start_streaming(rk3576_isp_frame_cb_t callback, void *arg);

/****************************************************************************
 * Name: rk3576_isp_stop_streaming
 *
 * Description:
 *   Disable the memory interface and the pipeline.  Safe to call when the
 *   driver is already idle.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_isp_stop_streaming(void);

#endif /* CONFIG_RK3576_ISP */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_ISP_H */
