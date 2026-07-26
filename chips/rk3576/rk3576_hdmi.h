/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_hdmi.h
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
 * RK3576 HDMI TX encoder: public interface.
 *
 * The HDMI transmitter is the downstream encoder of the VOP: the VOP owns
 * the timing generator and the pixel stream, the HDMI block turns that
 * stream into TMDS.  There is no NuttX "HDMI" upper half to bind to, so
 * this module exposes a plain chip-level API which the framebuffer /
 * board bring-up code calls, in this order:
 *
 *   rk3576_hdmi_initialize()          -- clocks, PHY bias, IRQ, DDC
 *   rk3576_hdmi_hpd_status()          -- is a sink attached?
 *   rk3576_hdmi_read_edid()           -- optional, to pick a mode
 *   rk3576_vop_set_timing()           -- program the pixel source
 *   rk3576_hdmi_set_mode()            -- program PHY PLL + encoder
 *
 * The timing structure is owned by the VOP driver (rk3576_vop.h) so that
 * the pixel source and the encoder cannot disagree about the mode.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_HDMI_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_HDMI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "rk3576_vop.h"

#ifdef CONFIG_RK3576_HDMI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* One EDID block is 128 bytes; a sink may expose up to 256 blocks but two
 * (base block + one CEA extension) covers every mode this driver can use.
 */

#define RK3576_HDMI_EDID_BLOCK_SIZE 128
#define RK3576_HDMI_EDID_MAX_BYTES  (RK3576_HDMI_EDID_BLOCK_SIZE * 2)

/* Highest TMDS character rate supported without FRL / SCDC scrambling.
 * Above this the sink must be switched to 1/40 clock ratio via SCDC.
 */

#define RK3576_HDMI_TMDS_SCDC_THRESHOLD 340000000u

/* Absolute pixel clock limits of the HDPTX PHY in TMDS mode. */

#define RK3576_HDMI_PIXCLK_MIN 25000000u
#define RK3576_HDMI_PIXCLK_MAX 600000000u

/* NOTE: the display timing type is owned by the VOP driver and is named
 * struct rk3576_vop_timing_s (see rk3576_vop.h).  It is used verbatim by
 * the encoder API below so that the pixel source and the encoder are
 * always programmed from the same description.
 */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hdmi_initialize
 *
 * Description:
 *   Bring up the HDMI TX controller and the HDPTX PHY far enough that the
 *   hot-plug detect input and the DDC (EDID) channel work.  No video is
 *   emitted until rk3576_hdmi_set_mode() is called.
 *
 *   Must be called after rk3576_clk_tree_initialize(), i.e. from
 *   board_late_initialize().  Calling it more than once is harmless.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_hdmi_initialize(void);

/****************************************************************************
 * Name: rk3576_hdmi_set_mode
 *
 * Description:
 *   Program the HDPTX PHY PLL for the requested pixel clock and configure
 *   the encoder (video mapping, sync polarity, AVI InfoFrame) for the
 *   requested timing, then enable the TMDS output.
 *
 *   The VOP must already be producing this exact timing on the video port
 *   routed to HDMI0 (see rk3576_vop_set_timing() and
 *   rk3576_vop_set_interface()).
 *
 * Input Parameters:
 *   timing - Mode to output.  The structure is copied.
 *
 * Returned Value:
 *   OK on success; -EINVAL for a timing the PHY cannot generate;
 *   -ETIMEDOUT if the PHY PLL does not lock; -EPERM if not initialised.
 *
 ****************************************************************************/

int rk3576_hdmi_set_mode(const struct rk3576_vop_timing_s *timing);

/****************************************************************************
 * Name: rk3576_hdmi_disable
 *
 * Description:
 *   Stop the TMDS output and power the PHY PLL down.  Hot-plug detect and
 *   DDC stay alive so a later rk3576_hdmi_set_mode() can restart output.
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_hdmi_disable(void);

/****************************************************************************
 * Name: rk3576_hdmi_read_edid
 *
 * Description:
 *   Read the sink's EDID over the DDC channel, starting at block 0.
 *   Reads that cross a 256-byte boundary use the E-DDC segment pointer.
 *
 * Input Parameters:
 *   buf - Destination buffer
 *   len - Number of bytes to read (rounded down to a multiple of 128 is
 *         not required; any length up to RK3576_HDMI_EDID_MAX_BYTES works)
 *
 * Returned Value:
 *   Number of bytes read on success, a negated errno value on failure.
 *   -ENODEV if no sink is attached.
 *
 ****************************************************************************/

ssize_t rk3576_hdmi_read_edid(uint8_t *buf, size_t len);

/****************************************************************************
 * Name: rk3576_hdmi_hpd_status
 *
 * Description:
 *   Report the current hot-plug detect level.
 *
 * Returned Value:
 *   true if a sink is attached, false otherwise.
 *
 ****************************************************************************/

bool rk3576_hdmi_hpd_status(void);

/****************************************************************************
 * Name: rk3576_hdmi_audio_config
 *
 * Description:
 *   Configure the HDMI audio path.  The samples arrive over the I2S link
 *   from SAI6; this call sets the audio InfoFrame, the sample packet
 *   layout and the audio clock regeneration (N/CTS) values derived from
 *   the pixel clock of the mode set by rk3576_hdmi_set_mode().
 *
 * Input Parameters:
 *   samplerate - Sample rate in Hz (32000, 44100, 48000 and their 2x/4x
 *                multiples)
 *   channels   - Number of channels (2..8)
 *   bits       - Sample width in bits (16, 20, 24)
 *
 * Returned Value:
 *   OK on success; -EINVAL for unsupported parameters; -EPERM if no video
 *   mode has been set yet (CTS depends on the pixel clock).
 *
 ****************************************************************************/

int rk3576_hdmi_audio_config(uint32_t samplerate, uint8_t channels,
                             uint8_t bits);

#endif /* CONFIG_RK3576_HDMI */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_HDMI_H */
