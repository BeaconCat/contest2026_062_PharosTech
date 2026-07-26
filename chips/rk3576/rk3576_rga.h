/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_rga.h
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
 * Public interface of the RK3576 RGA2 2D acceleration driver.
 *
 * NuttX has no generic 2D blitter subsystem, so the driver exposes two
 * equivalent entry points:
 *
 *   1. A character device (/dev/rga0, /dev/rga1) with the RGAIOC_* ioctls
 *      declared below, for user space.
 *   2. Direct in-kernel calls (rk3576_rga_blit() and friends) for other
 *      drivers such as the frame buffer / dual-display compositor.
 *
 * Both paths funnel into the same command-list builder and are serialised
 * per core by a mutex, so they may be mixed freely.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_RGA_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_RGA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <sys/ioctl.h>

#ifdef CONFIG_RK3576_RGA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pixel formats accepted by rk3576_rga_surface_s.format.  The values are
 * the raw RGA2 format codes so that no translation table is needed; see
 * hardware/rk3576_rga.h for their definition.
 */

#define RK3576_RGA_FORMAT_RGBA8888 0x0
#define RK3576_RGA_FORMAT_RGBX8888 0x1
#define RK3576_RGA_FORMAT_RGB888   0x2
#define RK3576_RGA_FORMAT_BGRA8888 0x3
#define RK3576_RGA_FORMAT_RGB565   0x4
#define RK3576_RGA_FORMAT_RGBA5551 0x5
#define RK3576_RGA_FORMAT_RGBA4444 0x6
#define RK3576_RGA_FORMAT_BGR888   0x7
#define RK3576_RGA_FORMAT_YUV422SP 0x8
#define RK3576_RGA_FORMAT_YUV422P  0x9
#define RK3576_RGA_FORMAT_YUV420SP 0xa
#define RK3576_RGA_FORMAT_YUV420P  0xb
#define RK3576_RGA_FORMAT_YUYV422  0xc
#define RK3576_RGA_FORMAT_YUYV420  0xd
#define RK3576_RGA_FORMAT_UYVY422  0xe
#define RK3576_RGA_FORMAT_UYVY420  0xf

/* Transform flags for rk3576_rga_blit().  Rotation values are mutually
 * exclusive and occupy the low two bits; mirroring and blending are
 * independent bit flags applied on top.
 */

#define RK3576_RGA_ROTATE_MASK   0x00000003
#define RK3576_RGA_ROTATE_0      0x00000000
#define RK3576_RGA_ROTATE_90     0x00000001
#define RK3576_RGA_ROTATE_180    0x00000002
#define RK3576_RGA_ROTATE_270    0x00000003

#define RK3576_RGA_MIRROR_X      0x00000004 /* Flip horizontally      */
#define RK3576_RGA_MIRROR_Y      0x00000008 /* Flip vertically        */
#define RK3576_RGA_BLEND_SRCOVER 0x00000010 /* Straight alpha blend   */
#define RK3576_RGA_BLEND_PREMUL  0x00000020 /* Premultiplied blend    */
#define RK3576_RGA_GLOBAL_ALPHA  0x00000040 /* Apply .galpha below    */
#define RK3576_RGA_ROP_ENABLE    0x00000080 /* Apply .rop below       */

#define RK3576_RGA_FLAGS_ALL     0x000000ff

/* Colour-space conversion selection for rk3576_rga_csc() and for the
 * .csc_mode field of a surface.
 */

#define RK3576_RGA_CSC_AUTO   0 /* Pick from the format pair */
#define RK3576_RGA_CSC_BT601L 1
#define RK3576_RGA_CSC_BT601F 2
#define RK3576_RGA_CSC_BT709L 3

/* ioctl commands.  _GRAPHIOCBASE is not appropriate here (that range is
 * owned by the frame buffer), so the private RGA range is used.
 */

#define RK3576_RGAIOC_BLIT    _IOC(0x1f00, 1) /* struct rk3576_rga_op_s * */
#define RK3576_RGAIOC_FILL    _IOC(0x1f00, 2) /* struct rk3576_rga_fill_s * */
#define RK3576_RGAIOC_SCALE   _IOC(0x1f00, 3) /* struct rk3576_rga_op_s * */
#define RK3576_RGAIOC_ROTATE  _IOC(0x1f00, 4) /* struct rk3576_rga_op_s * */
#define RK3576_RGAIOC_CSC     _IOC(0x1f00, 5) /* struct rk3576_rga_op_s * */
#define RK3576_RGAIOC_SYNC    _IOC(0x1f00, 6) /* void — wait for idle     */
#define RK3576_RGAIOC_VERSION _IOC(0x1f00, 7) /* uint32_t *              */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One 2D surface.  Plane addresses are *physical* addresses in the low
 * 4GB of the address space; the internal IOMMU is bypassed, so callers
 * must supply memory obtained from rk3576_dma_alloc() (or another
 * physically contiguous, sub-4GB region such as a frame buffer).
 *
 * The active rectangle is (xoffset, yoffset, width, height) inside a
 * virtual surface whose line pitch is 'stride' bytes.  For planar and
 * semi-planar YUV, cb/cr hold the chroma plane addresses; they are
 * ignored for packed formats.
 */

struct rk3576_rga_surface_s
{
  uint32_t yrgb;    /* Physical address of the Y / RGB plane      */
  uint32_t cb;      /* Physical address of the Cb (or CbCr) plane */
  uint32_t cr;      /* Physical address of the Cr plane           */
  uint32_t stride;  /* Line pitch of the Y / RGB plane, in bytes  */
  uint16_t width;   /* Active rectangle width, in pixels          */
  uint16_t height;  /* Active rectangle height, in pixels         */
  uint16_t xoffset; /* Active rectangle X origin, in pixels       */
  uint16_t yoffset; /* Active rectangle Y origin, in pixels       */
  uint8_t format;   /* RK3576_RGA_FORMAT_*                        */
  uint8_t csc_mode; /* RK3576_RGA_CSC_*                           */
  uint8_t rb_swap;  /* Non-zero to swap the R and B components    */
  uint8_t uv_swap;  /* Non-zero to swap the U and V components    */
};

/* Generic two-surface operation descriptor, shared by the BLIT, SCALE,
 * ROTATE and CSC ioctls.
 */

struct rk3576_rga_op_s
{
  struct rk3576_rga_surface_s src; /* Source surface              */
  struct rk3576_rga_surface_s dst; /* Destination surface         */
  uint32_t flags;                  /* RK3576_RGA_ROTATE_* | ...   */
  uint8_t galpha;                  /* Global alpha, 0..255        */
  uint8_t rop;                     /* Raster operation code       */
};

/* Colour-fill descriptor. */

struct rk3576_rga_fill_s
{
  struct rk3576_rga_surface_s dst; /* Destination surface         */
  uint32_t color;                  /* Fill colour, ARGB8888       */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_rga_initialize
 *
 * Description:
 *   Bring up one RGA2 core: enable its clocks, reset the core, allocate the
 *   DMA-safe command buffer, attach the interrupt handler and register the
 *   /dev/rgaN character device.
 *
 *   Must be called from board_late_initialize(), i.e. after
 *   rk3576_clk_tree_initialize() has registered the clock tree.
 *
 * Input Parameters:
 *   core - Core index, 0 or 1.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_initialize(int core);

/****************************************************************************
 * Name: rk3576_rga_blit
 *
 * Description:
 *   Perform a BitBLT from src to dst.  Scaling is applied implicitly when
 *   the source and destination rectangles differ in size; rotation,
 *   mirroring, alpha blending, ROP and colour-space conversion are selected
 *   through 'flags' and through the surface descriptors.
 *
 *   The call blocks until the hardware signals completion.
 *
 * Input Parameters:
 *   src   - Source surface
 *   dst   - Destination surface
 *   flags - Bitwise OR of RK3576_RGA_ROTATE_* / MIRROR_* / BLEND_* flags
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_blit(const struct rk3576_rga_surface_s *src,
                    const struct rk3576_rga_surface_s *dst, uint32_t flags);

/****************************************************************************
 * Name: rk3576_rga_fill
 *
 * Description:
 *   Fill the active rectangle of dst with a solid colour.
 *
 * Input Parameters:
 *   dst   - Destination surface
 *   color - Fill colour in ARGB8888; the hardware converts it to the
 *           destination format.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_fill(const struct rk3576_rga_surface_s *dst, uint32_t color);

/****************************************************************************
 * Name: rk3576_rga_scale
 *
 * Description:
 *   Convenience wrapper around rk3576_rga_blit() with no transform: the
 *   source rectangle is resampled into the destination rectangle.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_scale(const struct rk3576_rga_surface_s *src,
                     const struct rk3576_rga_surface_s *dst);

/****************************************************************************
 * Name: rk3576_rga_rotate
 *
 * Description:
 *   Rotate src into dst by 0/90/180/270 degrees clockwise.  For the 90 and
 *   270 degree cases the caller is responsible for supplying a destination
 *   rectangle whose width and height are swapped with respect to the
 *   source.
 *
 * Input Parameters:
 *   src     - Source surface
 *   dst     - Destination surface
 *   degrees - 0, 90, 180 or 270
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_rotate(const struct rk3576_rga_surface_s *src,
                      const struct rk3576_rga_surface_s *dst, int degrees);

/****************************************************************************
 * Name: rk3576_rga_csc
 *
 * Description:
 *   Convert between RGB and YUV colour spaces (either direction), with
 *   optional rescaling if the rectangles differ in size.
 *
 * Input Parameters:
 *   src  - Source surface
 *   dst  - Destination surface
 *   mode - RK3576_RGA_CSC_AUTO or an explicit RK3576_RGA_CSC_* matrix
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rga_csc(const struct rk3576_rga_surface_s *src,
                   const struct rk3576_rga_surface_s *dst, int mode);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_RGA */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_RGA_H */
