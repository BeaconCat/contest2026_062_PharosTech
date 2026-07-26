/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_vicap.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VICAP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VICAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_VICAP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pixel formats supported by the VICAP bypass path.  The values are the
 * usual V4L2 FourCCs so that they can be handed to the NuttX video stack
 * unchanged.
 */

#define RK3576_VICAP_FOURCC(a, b, c, d) \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
   ((uint32_t)(d) << 24))

#define RK3576_VICAP_FMT_NV12  RK3576_VICAP_FOURCC('N', 'V', '1', '2')
#define RK3576_VICAP_FMT_NV16  RK3576_VICAP_FOURCC('N', 'V', '1', '6')
#define RK3576_VICAP_FMT_UYVY  RK3576_VICAP_FOURCC('U', 'Y', 'V', 'Y')
#define RK3576_VICAP_FMT_YUYV  RK3576_VICAP_FOURCC('Y', 'U', 'Y', 'V')
#define RK3576_VICAP_FMT_SRGGB8  RK3576_VICAP_FOURCC('R', 'G', 'G', 'B')
#define RK3576_VICAP_FMT_SRGGB10 RK3576_VICAP_FOURCC('R', 'G', '1', '0')
#define RK3576_VICAP_FMT_SRGGB12 RK3576_VICAP_FOURCC('R', 'G', '1', '2')

/* Frame geometry limits enforced by rk3576_vicap_set_format() */

#define RK3576_VICAP_MIN_WIDTH   64
#define RK3576_VICAP_MIN_HEIGHT  64
#define RK3576_VICAP_MAX_WIDTH   4672
#define RK3576_VICAP_MAX_HEIGHT  3504

/* Number of ping-pong DMA buffers owned by the driver (FRM0 / FRM1) */

#define RK3576_VICAP_NBUFFERS    2

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Input source feeding the capture DMA. */

enum rk3576_vicap_input_e
{
  RK3576_VICAP_INPUT_DVP = 0, /* Parallel (DVP) sensor interface        */
  RK3576_VICAP_INPUT_CSI2 = 1 /* MIPI CSI-2 host, one virtual channel   */
};

/* Capture configuration handed to rk3576_vicap_set_format(). */

struct rk3576_vicap_format_s
{
  uint16_t width;             /* Active pixels per line                 */
  uint16_t height;            /* Active lines per frame                 */
  uint32_t pixelformat;       /* One of RK3576_VICAP_FMT_*              */
  uint8_t input;              /* enum rk3576_vicap_input_e              */
  uint8_t vc;                 /* CSI-2 virtual channel (0..3)           */
  uint8_t datatype;           /* CSI-2 data type, 0 to disable the filter */
  bool vsync_active_high;     /* DVP VSYNC polarity                     */
  bool hsync_active_low;      /* DVP HSYNC polarity                     */
};

/* Frame-complete callback.  Invoked from the VICAP interrupt handler, so
 * it must not block.  The buffer stays owned by the driver and is handed
 * back to the hardware as soon as the callback returns.
 *
 * Input Parameters:
 *   arg    - Opaque value supplied to rk3576_vicap_start_streaming()
 *   buf    - Virtual address of the completed frame (D-cache invalidated)
 *   size   - Number of valid bytes in buf
 *   seq    - Monotonic frame sequence number, starting at 0
 */

typedef void (*rk3576_vicap_frame_cb_t)(void *arg, void *buf, size_t size,
                                        uint32_t seq);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vicap_initialize
 *
 * Description:
 *   Bring up the VICAP block: enable its clocks, release it from reset,
 *   mask all interrupts and attach the interrupt handler.  Must be called
 *   after rk3576_clk_tree_initialize(), i.e. from board_late_initialize().
 *   Calling it more than once is harmless.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_initialize(void);

/****************************************************************************
 * Name: rk3576_vicap_set_format
 *
 * Description:
 *   Program the capture geometry and pixel format and allocate the
 *   ping-pong DMA buffers.  Cannot be called while streaming.
 *
 * Input Parameters:
 *   fmt - Requested capture configuration
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_set_format(const struct rk3576_vicap_format_s *fmt);

/****************************************************************************
 * Name: rk3576_vicap_get_framesize
 *
 * Description:
 *   Return the number of bytes one frame occupies with the format that is
 *   currently programmed.
 *
 * Returned Value:
 *   Frame size in bytes, or 0 if no format has been set yet.
 *
 ****************************************************************************/

size_t rk3576_vicap_get_framesize(void);

/****************************************************************************
 * Name: rk3576_vicap_start_streaming
 *
 * Description:
 *   Arm both ping-pong buffers and start the capture DMA.  The callback is
 *   invoked from interrupt context once per completed frame.
 *
 * Input Parameters:
 *   callback - Frame-complete callback, must not be NULL
 *   arg      - Opaque value passed back to the callback
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_start_streaming(rk3576_vicap_frame_cb_t callback,
                                 void *arg);

/****************************************************************************
 * Name: rk3576_vicap_stop_streaming
 *
 * Description:
 *   Stop the capture DMA and mask the frame interrupt.  Safe to call when
 *   the driver is already idle.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vicap_stop_streaming(void);

#ifdef CONFIG_RK3576_VICAP_VIDEO
/****************************************************************************
 * Name: rk3576_vicap_imgdata_get
 *
 * Description:
 *   Return the NuttX video-stack image data (capture DMA) lower half for
 *   the VICAP block.  Board logic passes it, together with the image
 *   sensor lower half of the camera actually fitted, to video_register()
 *   to obtain a standard V4L2-style /dev/videoN node.
 *
 *   In this mode the video stack owns the frame buffers, so the capture
 *   runs one frame at a time (single-shot) instead of using the internal
 *   ping-pong pair.
 *
 * Returned Value:
 *   The image data lower half (struct imgdata_s *), never NULL.
 *
 ****************************************************************************/

struct imgdata_s;

struct imgdata_s *rk3576_vicap_imgdata_get(void);
#endif /* CONFIG_RK3576_VICAP_VIDEO */

#endif /* CONFIG_RK3576_VICAP */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VICAP_H */
