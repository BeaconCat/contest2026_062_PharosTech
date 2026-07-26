/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_vdec.h
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
 * RK3576 RKVDEC video decoder: public API.
 *
 * NuttX has no video-decoder subsystem, so the decoder is exported as a
 * character device (/dev/vdec0) that accepts one ioctl per decode task.
 * The caller (a user-space H.264 parser, or in-kernel code) is responsible
 * for slicing the elementary stream into pictures and for building the
 * SPS/PPS and reference-picture-set buffers; the hardware only performs
 * slice-level decoding.
 *
 * Every buffer address handed over in struct rk3576_vdec_task_s must come
 * from rk3576_dma_alloc() (physically contiguous, below 4GB) and must obey
 * the alignment constraints documented in hardware/rk3576_vdec.h.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VDEC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VDEC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/fs/ioctl.h>

#include "hardware/rk3576_vdec.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Character device path. */

#define RK3576_VDEC_DEVPATH   "/dev/vdec0"

/* ioctl commands.  They live at the top of the NuttX video ioctl range,
 * well clear of the command numbers used by drivers/video.
 */

#define RK3576_VDEC_IOC_DECODE  _VIDIOC(0x0081) /* arg: struct
                                                 * rk3576_vdec_task_s * */
#define RK3576_VDEC_IOC_RESET   _VIDIOC(0x0082) /* arg: none            */
#define RK3576_VDEC_IOC_GETVER  _VIDIOC(0x0083) /* arg: uint32_t *      */

/* Supported codecs.  Only H.264 is fully programmed today. */

#define RK3576_VDEC_CODEC_H264  0
#define RK3576_VDEC_CODEC_HEVC  1
#define RK3576_VDEC_CODEC_VP9   2

/* Output chroma format. */

#define RK3576_VDEC_YUV420      0
#define RK3576_VDEC_YUV422      1
#define RK3576_VDEC_MONO        3

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One reference picture slot. */

struct rk3576_vdec_ref_s
{
  uintptr_t frame;  /* Physical address of the reference frame, 0 = unused */
  uintptr_t colmv;  /* Physical address of its collocated MV buffer        */
  int32_t poc;      /* Picture order count                                 */
  bool field;       /* Reference is a single field                         */
};

/* A complete decode task: one picture. */

struct rk3576_vdec_task_s
{
  uint8_t codec;              /* RK3576_VDEC_CODEC_*                      */
  uint8_t yuv_format;         /* RK3576_VDEC_YUV420 / _YUV422 / _MONO     */
  uint8_t bitdepth;           /* 8 or 10                                  */
  uint8_t strm_start_bit;     /* First valid bit in the first stream word */

  uint16_t width;             /* Picture width in pixels                  */
  uint16_t height;            /* Picture height in pixels                 */

  bool field;                 /* Picture is a field, not a frame          */
  bool topfield;              /* ... and it is the top field              */
  bool conceal;               /* Enable hardware error concealment        */

  uintptr_t stream;           /* Bitstream buffer, 16B aligned            */
  size_t stream_len;          /* Bitstream length in bytes                */

  uintptr_t output;           /* Decoded frame buffer, 256B aligned       */
  uint32_t luma_stride;       /* Luma line stride in bytes                */
  uint32_t frame_size;        /* Total luma+chroma size in bytes          */

  uintptr_t pps;              /* Packed SPS/PPS parameter buffer          */
  uintptr_t rps;              /* Packed reference-picture-set buffer      */
  uintptr_t cabac_table;      /* CABAC init table (H.264/H.265), or 0     */
  uintptr_t colmv;            /* Collocated MV buffer of the current pic  */
  uintptr_t errinfo;          /* Per-MB error info output, or 0           */

  uint8_t nrefs;              /* Number of valid entries in refs[]        */
  struct rk3576_vdec_ref_s refs[RKVDEC_MAX_REFS];
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: rk3576_vdec_initialize
 *
 * Description:
 *   Power up the VPU domain, enable the decoder clocks, reset the core,
 *   put its IOMMU into bypass and register /dev/vdec0.  Must be called
 *   from board_late_initialize(), after rk3576_clk_tree_initialize().
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vdec_initialize(void);

/****************************************************************************
 * Name: rk3576_vdec_decode_frame
 *
 * Description:
 *   Submit one picture to the decoder and block until the hardware raises
 *   its completion interrupt.  Only one task runs at a time; concurrent
 *   callers are serialised.
 *
 * Input Parameters:
 *   task - Fully populated task descriptor.
 *
 * Returned Value:
 *   OK when the picture decoded cleanly, -EIO on a hardware/stream error,
 *   -ETIMEDOUT if the core did not finish in time, or another negated
 *   errno for an invalid request.
 *
 ****************************************************************************/

int rk3576_vdec_decode_frame(const struct rk3576_vdec_task_s *task);

/****************************************************************************
 * Name: rk3576_vdec_reset
 *
 * Description:
 *   Soft reset the decoder core.  Used to recover from a hardware timeout
 *   or bus error before submitting the next task.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_vdec_reset(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_VDEC_H */
