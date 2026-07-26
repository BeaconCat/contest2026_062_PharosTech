/****************************************************************************
 * chips/rk3576/rk3576_wdt.h
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
 * RK3576 Watchdog Timer (Synopsys DesignWare) driver public API.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_WDT_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_WDT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_wdt_initialize
 *
 * Description:
 *   Initialise the RK3576 non-secure watchdog (WDT_NS) and register it with
 *   the NuttX watchdog upper half as /dev/watchdogN.
 *
 *   The watchdog is left disabled and configured for the "reset the SoC on
 *   timeout" response mode (WDT_CR.RMOD = 0).  An application arms it with
 *   the WDIOC_SETTIMEOUT / WDIOC_START ioctls and keeps it happy with
 *   WDIOC_KEEPALIVE.
 *
 * Input Parameters:
 *   minor - The device minor number, i.e. N in /dev/watchdogN.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_wdt_initialize(int minor);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_WDT_H */
