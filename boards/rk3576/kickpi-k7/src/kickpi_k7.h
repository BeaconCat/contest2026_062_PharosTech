/****************************************************************************
 * boards/arm64/rk3576/kickpi_k7/src/kickpi_k7.h
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

#ifndef __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H
#define __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Functions Definitions
 ****************************************************************************/

#ifdef CONFIG_IEEE80211_BROADCOM_FULLMAC_SDIO
/****************************************************************************
 * Name: kickpi_k7_wlan_initialize
 *
 * Description:
 *   Bring up the SDIO host (slot 1) and register the on-board AP6256 WiFi
 *   combo with the bcmf stack.
 *
 ****************************************************************************/

int kickpi_k7_wlan_initialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_ARM64_RK3576_KICKPI_K7_SRC_KICKPI_K7_H */
