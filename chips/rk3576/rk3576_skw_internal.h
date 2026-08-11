/****************************************************************************
 * chips/rk3576/rk3576_skw_internal.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_INTERNAL_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

void rk3576_skw_get_mac(uint8_t mac[6]);
void rk3576_skw_get_bssid(uint8_t bssid[6]);
int rk3576_skw_send_control(uint8_t id, const uint8_t *payload, int length);
int rk3576_skw_data_tx(const uint8_t *frame, int length);
int rk3576_skw_add_key(uint8_t key_type, uint8_t cipher,
                       const uint8_t *mac, uint8_t key_id,
                       const uint8_t *key, int key_len, const uint8_t *pn);

#ifdef CONFIG_NET
int rk3576_skw_netdev_register(void);
void rk3576_skw_net_input(const uint8_t *frame, int length);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_INTERNAL_H */
