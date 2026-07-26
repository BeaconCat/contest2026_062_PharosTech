/****************************************************************************
 * chips/rk3576/rk3576_pdm.h
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
 * controller is exposed through the standard NuttX I2S interface
 * (include/nuttx/audio/i2s.h); only the receive half is implemented, since
 * the PDM block has no transmit path.  A board typically registers it with
 * the generic i2schar character driver:
 *
 *   struct i2s_dev_s *i2s = rk3576_pdm_initialize(RK3576_PDM1);
 *   i2schar_register(i2s, 0);
 *
 * The board is responsible for muxing the PDM clock and data pins before
 * the first capture is started.  All clocking is taken from the NuttX CLK
 * framework, so rk3576_clk_tree_initialize() must have run first.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/audio/i2s.h>

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
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pdm_initialize
 *
 * Description:
 *   Create the I2S (receive-only) interface for one PDM controller.  The
 *   controller clocks are enabled and the block is placed in a known idle
 *   state; capture parameters are applied later through the I2S
 *   rxsamplerate / rxdatawidth / rxchannels methods, and default to
 *   16 kHz / 16-bit / 2 channels (the voice wake-word configuration).
 *
 *   Must be called after rk3576_clk_tree_initialize(), i.e. from
 *   board_late_initialize().
 *
 * Input Parameters:
 *   controller - RK3576_PDM0 or RK3576_PDM1.
 *
 * Returned Value:
 *   An I2S device handle on success, or NULL on an invalid controller
 *   index or a clock/DMA bring-up failure.
 *
 ****************************************************************************/

struct i2s_dev_s *rk3576_pdm_initialize(int controller);

#endif /* CONFIG_RK3576_PDM */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_PDM_H */
