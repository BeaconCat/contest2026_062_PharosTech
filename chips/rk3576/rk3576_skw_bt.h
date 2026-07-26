/****************************************************************************
 * chips/rk3576/rk3576_skw_bt.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_BT_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_BT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_SKW_BT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* SeekWave SV6621 (SWT6621-S) SDIO virtual channel map.
 *
 * The combo chip multiplexes every subsystem over the same SDIO function-1
 * data window; the channel number lives in the outer 32-bit packet header
 * (len:16 | pad:7 | eof:1 | channel:8).  Bluetooth HCI is *not* carried on
 * a UART: the wireless-bluetooth UART4 node found in the vendor device tree
 * is AP6256 template leftover and is not connected to this part.
 */

#define RK3576_SKW_CH_ATC        0 /* AT command console          */
#define RK3576_SKW_CH_LOOPCHECK  1 /* CP liveness / boot banners  */
#define RK3576_SKW_CH_BT_CMD     2 /* HCI command + HCI event     */
#define RK3576_SKW_CH_BT_AUDIO   3 /* HCI SCO (voice)             */
#define RK3576_SKW_CH_BT_ISOC    4 /* HCI ISO (LE audio)          */
#define RK3576_SKW_CH_BT_DATA    5 /* HCI ACL                     */
#define RK3576_SKW_CH_WIFI_CMD   6
#define RK3576_SKW_CH_WIFI_DATA  7
#define RK3576_SKW_CH_WIFI_DATA1 8
#define RK3576_SKW_CH_BSP_LOG    9
#define RK3576_SKW_CH_BT_LOG     10 /* CP-side Bluetooth firmware log */
#define RK3576_SKW_CH_BSP_UPDATE 11
#define RK3576_SKW_CH_MAX        12 /* SDIO2_MAX_CH_NUM on V20 parts */

/* Function-0 mailbox registers used by the Bluetooth bring-up.  The full
 * set is owned by the Wi-Fi core driver; only the AP->CP doorbell is
 * needed here.
 */

#define RK3576_SKW_REG_AP2CP_DOORBELL 0x1b0

/* Doorbell command written to RK3576_SKW_REG_AP2CP_DOORBELL to ask the CP
 * to bring the Bluetooth stack up.  The CP answers with a "BTREADY" banner
 * on the loopcheck channel.
 */

#define RK3576_SKW_DOORBELL_BT_START 0x04

/* Every virtual channel carries a 12-byte inner link header in front of the
 * real payload (the Wi-Fi command channel finds its struct skw_msg at +12,
 * the Wi-Fi data channel finds its Ethernet frame at +12).  The Bluetooth
 * channels follow the same rule, so an H4 frame starts at +12 as well.
 */

#define RK3576_SKW_LINK_HDR_LEN 12

/* H4 packet indicators (Bluetooth Core spec, UART transport), reused as the
 * first byte of the SDIO channel payload.
 */

#define RK3576_SKW_BT_H4_CMD 0x01
#define RK3576_SKW_BT_H4_ACL 0x02
#define RK3576_SKW_BT_H4_SCO 0x03
#define RK3576_SKW_BT_H4_EVT 0x04
#define RK3576_SKW_BT_H4_ISO 0x05

/* Number of bytes this driver needs in front of the caller payload:
 * inner link header plus the H4 packet indicator.  Exported through
 * struct bt_driver_s::head_reserve so the Bluetooth stack allocates room
 * for it and no bounce buffer is required on the transmit path.
 */

#define RK3576_SKW_BT_HEAD_RESERVE (RK3576_SKW_LINK_HDR_LEN + 1)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Receive callback installed on a virtual channel.  It is invoked by the
 * SeekWave SDIO core receive thread once per reassembled channel packet.
 *
 *   channel - Virtual channel the packet arrived on.
 *   data    - Start of the channel payload, i.e. the 12-byte inner link
 *             header (the outer 32-bit packet header is already stripped
 *             by the core driver).
 *   len     - Number of valid bytes at data, link header included.
 *   arg     - Opaque value handed to skw_sdio_register_channel_handler().
 */

typedef void (*skw_sdio_channel_handler_t)(uint8_t channel, const void *data,
                                           size_t len, void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/* Transport primitives implemented by the SeekWave SDIO core driver
 * (chips/rk3576/rk3576_skw.c).  The Bluetooth driver deliberately does not
 * duplicate any SDIO handling: it only borrows the channel multiplexer that
 * the Wi-Fi bring-up already established.
 */

/****************************************************************************
 * Name: skw_sdio_send_channel
 *
 * Description:
 *   Send one packet on a virtual channel.  The core driver prepends the
 *   outer 32-bit packet header and appends the trailing end-of-frame
 *   header, then issues the CMD53 write.
 *
 * Input Parameters:
 *   channel - Virtual channel number (RK3576_SKW_CH_*).
 *   data    - Channel payload, starting at the inner link header.
 *   len     - Payload length in bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int skw_sdio_send_channel(uint8_t channel, const void *data, size_t len);

/****************************************************************************
 * Name: skw_sdio_register_channel_handler
 *
 * Description:
 *   Install (handler != NULL) or remove (handler == NULL) the receive
 *   callback of a virtual channel.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int skw_sdio_register_channel_handler(uint8_t channel,
                                      skw_sdio_channel_handler_t handler,
                                      void *arg);

/****************************************************************************
 * Name: skw_sdio_func0_writeb
 *
 * Description:
 *   Write one byte to an SDIO function-0 mailbox register (CMD52).
 *
 ****************************************************************************/

int skw_sdio_func0_writeb(uint32_t addr, uint8_t value);

/****************************************************************************
 * Name: skw_sdio_func0_readb
 *
 * Description:
 *   Read one byte from an SDIO function-0 mailbox register (CMD52).
 *
 ****************************************************************************/

int skw_sdio_func0_readb(uint32_t addr, uint8_t *value);

/****************************************************************************
 * Name: skw_sdio_cp_ready
 *
 * Description:
 *   True once the CP firmware has been downloaded and has reported that it
 *   is running.  The Bluetooth bring-up must not touch the doorbell before
 *   that point.
 *
 ****************************************************************************/

bool skw_sdio_cp_ready(void);

/****************************************************************************
 * Name: rk3576_skw_bt_initialize
 *
 * Description:
 *   Bring the Bluetooth side of the SeekWave combo chip up and register it
 *   with the NuttX Bluetooth stack.  The CP firmware must already be
 *   running (the Wi-Fi core driver downloads and boots it).
 *
 *   The sequence is: hook the Bluetooth virtual channels, ring the AP->CP
 *   doorbell with RK3576_SKW_DOORBELL_BT_START, wait for the CP "BTREADY"
 *   banner, then register a struct bt_driver_s so that the HCI core can
 *   open /dev/ttyBT<id>.
 *
 * Input Parameters:
 *   id - Bluetooth device minor number (0 gives /dev/ttyBT0).
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.  -ETIMEDOUT means
 *   the CP never answered the BT_START doorbell.
 *
 ****************************************************************************/

int rk3576_skw_bt_initialize(int id);

/****************************************************************************
 * Name: rk3576_skw_bt_send_hci
 *
 * Description:
 *   Send a raw HCI packet.  Intended for callers that do not go through the
 *   Bluetooth stack (vendor configuration, factory test).  Unlike the
 *   struct bt_driver_s send method this one copies the payload, so no head
 *   room has to be reserved by the caller.
 *
 * Input Parameters:
 *   h4type - RK3576_SKW_BT_H4_CMD / _ACL / _SCO / _ISO.
 *   data   - HCI packet without the H4 indicator byte.
 *   len    - Length of data in bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_skw_bt_send_hci(uint8_t h4type, const void *data, size_t len);

/****************************************************************************
 * Name: rk3576_skw_bt_is_ready
 *
 * Description:
 *   True once the CP has reported BTREADY and the HCI transport is usable.
 *
 ****************************************************************************/

bool rk3576_skw_bt_is_ready(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_RK3576_SKW_BT */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_SKW_BT_H */
