/****************************************************************************
 * chips/rk3576/rk3576_clk_tree_periph.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_PERIPH_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_PERIPH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_CLK

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_clk_tree_periph_initialize
 *
 * Description:
 *   Register the clock nodes of the peripheral blocks that are not covered
 *   by rk3576_clk_tree_initialize() (watchdog, SPI, SARADC, TSADC, crypto,
 *   OTP, RNG, mailbox, GMAC, PCIe, VOP, HDMI, RGA, VDEC, CSI, ISP, VICAP,
 *   NPU, GPU, PDM, I3C and the infrared receiver).
 *
 *   Must be called once during board init, right after
 *   rk3576_clk_tree_initialize() and before any peripheral driver calls
 *   clk_get().  Every node registered here may reference the PLLs and the
 *   fixed-factor dividers produced by that function.
 *
 ****************************************************************************/

void rk3576_clk_tree_periph_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CLK */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CLK_TREE_PERIPH_H */
