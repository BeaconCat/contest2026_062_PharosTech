/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pdm.h
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
 * Public interface of the RK3576 PDM microphone-array capture driver.  The
 * controller is exposed as a NuttX audio lower half (include/nuttx/audio),
 * so a board registers it with audio_register():
 *
 *   lower = rk3576_pdm_initialize(RK3576_PDM1);
 *   audio_register("pcm0c", lower);
 *
 * The board is responsible for muxing the PDM clock and data pins and for
 * providing the PDM output clock rate before the device is opened.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#ifdef CONFIG_RK3576_PDM

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller enumeration, used as the rk3576_pdm_initialize() argument and
 * as the index of the internal instance array.
 */

#define RK3576_PDM0      0 /* pdm@273b0000, PMU bus domain  */
#define RK3576_PDM1      1 /* pdm@2a6e0000, main bus domain */
#define RK3576_PDM_NCTRL 2

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct audio_lowerhalf_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pdm_initialize
 *
 * Description:
 *   Create the audio lower half for one PDM controller.  The hardware is
 *   not touched here; it is brought up when the upper half configures and
 *   starts a capture session.
 *
 * Input Parameters:
 *   controller - RK3576_PDM0 or RK3576_PDM1.
 *
 * Returned Value:
 *   An audio lower-half handle to hand to audio_register(), or NULL on an
 *   invalid controller index.
 *
 ****************************************************************************/

struct audio_lowerhalf_s *rk3576_pdm_initialize(int controller);

/****************************************************************************
 * Name: rk3576_pdm_set_clkout
 *
 * Description:
 *   Tell the driver the actual frequency, in Hz, of the controller's
 *   "pdm_clk_out" root clock.  The driver divides this clock down to the
 *   microphone bit clock and needs the real rate to pick the decimation
 *   ratio for a requested sample rate.  A board calls this after it has
 *   programmed the CRU, before the device is opened.  When it is never
 *   called the driver assumes the reset default documented in rk3576_pdm.c.
 *
 * Input Parameters:
 *   controller - RK3576_PDM0 or RK3576_PDM1.
 *   clkout_hz  - Root clock frequency in Hz.
 *
 * Returned Value:
 *   OK on success; -EINVAL on an invalid controller or zero frequency.
 *
 ****************************************************************************/

int rk3576_pdm_set_clkout(int controller, uint32_t clkout_hz);

#endif /* CONFIG_RK3576_PDM */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H */
