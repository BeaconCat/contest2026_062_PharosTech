/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_vop.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VOP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VOP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_VOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Display interface a video port can be routed to. */

#define RK3576_VOP_IF_HDMI0   0
#define RK3576_VOP_IF_MIPI0   1
#define RK3576_VOP_IF_MIPI1   2
#define RK3576_VOP_IF_LVDS0   3
#define RK3576_VOP_IF_EDP0    4

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Display timing for one video port.
 *
 * Horizontal and vertical parameters use the usual CRTC convention:
 *
 *   htotal = hact + hfp + hsync + hbp
 *   vtotal = vact + vfp + vsync + vbp
 *
 * pixclk is the required dclk frequency in Hz:
 *
 *   pixclk = htotal * vtotal * refresh
 */

struct rk3576_vop_timing_s
{
  uint32_t pixclk;   /* Pixel (dclk) frequency in Hz                     */

  uint16_t hact;     /* Horizontal active pixels                         */
  uint16_t hfp;      /* Horizontal front porch                           */
  uint16_t hsync;    /* Horizontal sync width                            */
  uint16_t hbp;      /* Horizontal back porch                            */

  uint16_t vact;     /* Vertical active lines                            */
  uint16_t vfp;      /* Vertical front porch                             */
  uint16_t vsync;    /* Vertical sync width                              */
  uint16_t vbp;      /* Vertical back porch                              */

  bool hsync_active_high; /* HSYNC polarity on the display interface     */
  bool vsync_active_high; /* VSYNC polarity on the display interface     */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_vop_timing_1080p60
 *
 * Description:
 *   Return the CEA-861 1920x1080p60 timing (148.5 MHz dclk).
 *
 ****************************************************************************/

const struct rk3576_vop_timing_s *rk3576_vop_timing_1080p60(void);

/****************************************************************************
 * Name: rk3576_vop_timing_720p60
 *
 * Description:
 *   Return the CEA-861 1280x720p60 timing (74.25 MHz dclk).
 *
 ****************************************************************************/

const struct rk3576_vop_timing_s *rk3576_vop_timing_720p60(void);

/****************************************************************************
 * Name: rk3576_vop_set_timing
 *
 * Description:
 *   Select the display timing used by the framebuffer video port.  Must be
 *   called before up_fbinitialize(); afterwards the mode is fixed because
 *   the framebuffer geometry reported to the FB upper half would otherwise
 *   change underneath the application.
 *
 * Input Parameters:
 *   timing - Timing to use.  The structure is copied.
 *
 * Returned Value:
 *   OK on success; -EBUSY if the VOP has already been initialised,
 *   -EINVAL if the timing does not fit the compiled framebuffer.
 *
 ****************************************************************************/

int rk3576_vop_set_timing(const struct rk3576_vop_timing_s *timing);

/****************************************************************************
 * Name: rk3576_vop_set_interface
 *
 * Description:
 *   Select which display interface the framebuffer video port drives.  The
 *   default is RK3576_VOP_IF_HDMI0.  Must be called before
 *   up_fbinitialize().
 *
 * Input Parameters:
 *   iface - One of the RK3576_VOP_IF_* interface identifiers.
 *
 * Returned Value:
 *   OK on success; -EBUSY if the VOP is already initialised; -EINVAL for
 *   an unsupported interface.
 *
 ****************************************************************************/

int rk3576_vop_set_interface(int iface);

/****************************************************************************
 * Name: rk3576_vop_get_fbmem
 *
 * Description:
 *   Return the physical address and size of the framebuffer the VOP scans
 *   out.  Useful for the board to hand the buffer to another producer.
 *
 * Input Parameters:
 *   fbsize - If non-NULL, receives the framebuffer size in bytes.
 *
 * Returned Value:
 *   Physical address of the framebuffer.
 *
 ****************************************************************************/

uintptr_t rk3576_vop_get_fbmem(size_t *fbsize);

#endif /* CONFIG_RK3576_VOP */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VOP_H */
