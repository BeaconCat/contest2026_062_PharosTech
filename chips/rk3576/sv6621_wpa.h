/****************************************************************************
 * chips/rk3576/sv6621_wpa.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_WPA_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_WPA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: sv6621_wpa_connect
 *
 * Description:
 *   Associate with a WPA2-PSK (CCMP) network and run the host-side EAPOL
 *   4-way handshake to completion, installing the derived PTK/GTK into the
 *   CP via ADD_KEY.  The SeekWave combo is a FullMAC that offloads
 *   auth/assoc but NOT the 4-way handshake, so the supplicant runs here.
 *
 * Input Parameters:
 *   ssid       - Target SSID (NUL-terminated).
 *   passphrase - WPA2-PSK ASCII passphrase (8..63 chars).
 *
 * Returned Value:
 *   OK once the 4-way handshake completes and keys are installed;
 *   a negated errno otherwise.
 *
 ****************************************************************************/

int sv6621_wpa_connect(FAR const char *ssid, FAR const char *passphrase);

/****************************************************************************
 * Name: sv6621_wpa_eapol_input
 *
 * Description:
 *   Feed a received EAPOL frame (EtherType 0x888e payload, starting at the
 *   802.1X header) to the supplicant state machine.  Called by the SKW
 *   core's channel-7 receive path.
 *
 ****************************************************************************/

void sv6621_wpa_eapol_input(FAR const uint8_t *data, int len);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_SV6621_WPA_H */
