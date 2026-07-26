/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_hdmi.h
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
 * RK3576 HDMI TX hardware register definitions.
 *
 * Two independent hardware blocks are described here:
 *
 *   1. Synopsys DesignWare HDMI TX "QP" controller
 *      (vendor DTS node hdmi@27da0000, compatible
 *      "rockchip,rk3576-dw-hdmi").  Two register windows:
 *
 *        0x27DA0000 + 0x10000   main controller register file
 *        0x27DB0000 + 0x10000   QP extension window (FRL / eARC / CEC)
 *
 *      The QP controller is a different register map from the classic
 *      DesignWare HDMI TX found on RK3288/RK3399: registers are 32-bit
 *      word accesses (DTS "reg-io-width = <4>") instead of the byte-wide
 *      map of the older core.  The RK3576 instance is register compatible
 *      with the RK3588 one.
 *
 *   2. Rockchip/Samsung HDPTX PHY
 *      (vendor DTS node hdmiphy@2b000000, compatible
 *      "rockchip,rk3576-hdptx-phy-hdmi", "rockchip,rk3588-hdptx-phy-hdmi")
 *
 *        0x2B000000 + 0x2000    PHY register file (2048 32-bit registers)
 *        0x26032000 + 0x100     HDPTXPHY GRF ("rockchip,rk3576-hdptxphy-grf")
 *
 *      The PHY register file is addressed by "register number": the
 *      documentation numbers the registers 0x0000..0x07ff and the byte
 *      offset is the number multiplied by four.  Blocks:
 *
 *        0x0000..0x00ff  CMN    common block, reference + ROPLL / LCPLL
 *        0x0100..0x01ff  SB     sideband (AUX / HPD helper logic)
 *        0x0200..0x02ff  LNTOP  lane top: protocol select, bus width
 *        0x0300..0x07ff  LANE   per-lane serializer / driver settings
 *
 * TODO: the RK3576 TRM chapters for "HDMITX" and "HDPTX PHY" were not
 * available while writing this header.  Offsets and bit positions below
 * follow the public DesignWare HDMI TX QP / Samsung HDPTX programming
 * model as used on RK3588, which the RK3576 blocks are documented (in the
 * vendor DTS compatible string) to be identical to.  Every definition
 * marked "TODO: verify" must be cross-checked against the RK3576 TRM
 * during hardware bring-up.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_HDMI_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_HDMI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register bases -----------------------------------------------------------
 *
 * Kept local to this header so that hardware/rk3576_memorymap.h (a shared
 * file) does not have to be touched by every new peripheral.  The main
 * bases are also exported through rk3576_memorymap.h once integrated.
 */

#ifndef RK3576_HDMITX0_ADDR
#  define RK3576_HDMITX0_ADDR      0x27DA0000 /* DW HDMI TX QP controller  */
#endif
#ifndef RK3576_HDMITX0_QP_ADDR
#  define RK3576_HDMITX0_QP_ADDR   0x27DB0000 /* QP extension window       */
#endif
#ifndef RK3576_HDPTXPHY_ADDR
#  define RK3576_HDPTXPHY_ADDR     0x2B000000 /* HDPTX PHY register file   */
#endif
#ifndef RK3576_HDPTXPHY_GRF_ADDR
#  define RK3576_HDPTXPHY_GRF_ADDR 0x26032000 /* HDPTX PHY GRF             */
#endif
#ifndef RK3576_VO0_GRF_ADDR
#  define RK3576_VO0_GRF_ADDR      0x2601A000 /* VO0 GRF                   */
#endif
#ifndef RK3576_VO1_GRF_ADDR
#  define RK3576_VO1_GRF_ADDR      0x26036000 /* VO1 GRF (HDMI TX side)    */
#endif

#define RK3576_HDMITX0_SIZE        0x10000
#define RK3576_HDPTXPHY_SIZE       0x2000

/* ==========================================================================
 * DesignWare HDMI TX QP controller
 * ========================================================================*/

/* Identification / configuration (read only) */

#define RK3576_HDMI_CORE_ID                 0x0000
#define RK3576_HDMI_VER_NUMBER              0x0004
#define RK3576_HDMI_VER_TYPE                0x0008
#define RK3576_HDMI_CONFIG_REG              0x000c
#define RK3576_HDMI_CONFIG_CEC              0x0010
#define RK3576_HDMI_CORE_TIMESTAMP_HHMM     0x0014
#define RK3576_HDMI_CORE_TIMESTAMP_MMDD     0x0018
#define RK3576_HDMI_CORE_TIMESTAMP_YYYY     0x001c

/* Reset manager */

#define RK3576_HDMI_GLOBAL_SWRESET_REQUEST  0x0040
#define RK3576_HDMI_GLOBAL_SWDISABLE        0x0044
#define RK3576_HDMI_RESET_MANAGER_CONFIG0   0x0048

/* GLOBAL_SWRESET_REQUEST / GLOBAL_SWDISABLE share the same bit layout. */

#define RK3576_HDMI_SWRST_EARCRX_TRIGGER    (1 << 0)
#define RK3576_HDMI_SWRST_CEC               (1 << 1)
#define RK3576_HDMI_SWRST_I2CM              (1 << 12)
#define RK3576_HDMI_SWRST_AVP               (1 << 6)
#define RK3576_HDMI_SWDIS_AVP_DATAPATH      (1 << 6)
#define RK3576_HDMI_SWDIS_CEC               (1 << 1)

/* Timer base -- must be programmed with the pclk frequency in MHz so that
 * the core's internal microsecond timers are accurate.
 */

#define RK3576_HDMI_TIMER_BASE_CONFIG0      0x0080
#define RK3576_HDMI_TIMER_BASE_MASK         0xffff

/* CMU (clock monitoring unit) */

#define RK3576_HDMI_CMU_CONFIG0             0x00a0
#define RK3576_HDMI_CMU_CONFIG1             0x00a4
#define RK3576_HDMI_CMU_CONFIG2             0x00a8
#define RK3576_HDMI_CMU_CONFIG3             0x00ac
#define RK3576_HDMI_CMU_STATUS              0x00b0

#define RK3576_HDMI_CMU_DISPLAY_CLK_MONITOR 0x3f
#define RK3576_HDMI_CMU_DISPLAY_CLK_LOCKED  0x15

/* I2C master (DDC / EDID / SCDC).  "SM" = standard mode 100 kHz,
 * "FM" = fast mode 400 kHz.
 */

#define RK3576_HDMI_I2CM_SM_SCL_CONFIG0     0x00fc
#define RK3576_HDMI_I2CM_FM_SCL_CONFIG0     0x0100
#define RK3576_HDMI_I2CM_CONFIG0            0x0104
#define RK3576_HDMI_I2CM_CONTROL0           0x0108
#define RK3576_HDMI_I2CM_STATUS0            0x010c
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL0 0x0110
#define RK3576_HDMI_I2CM_INTERFACE_CONTROL1 0x0114
#define RK3576_HDMI_I2CM_INTERFACE_WRDATA0  0x0118
#define RK3576_HDMI_I2CM_INTERFACE_WRDATA1  0x011c
#define RK3576_HDMI_I2CM_INTERFACE_WRDATA2  0x0120
#define RK3576_HDMI_I2CM_INTERFACE_WRDATA3  0x0124
#define RK3576_HDMI_I2CM_INTERFACE_RDDATA0  0x0128
#define RK3576_HDMI_I2CM_INTERFACE_RDDATA1  0x012c
#define RK3576_HDMI_I2CM_INTERFACE_RDDATA2  0x0130
#define RK3576_HDMI_I2CM_INTERFACE_RDDATA3  0x0134

/* I2CM_INTERFACE_CONTROL0 */

#define RK3576_HDMI_I2CM_WR_MASK            (0x3 << 12)
#define RK3576_HDMI_I2CM_WR_READ            (0x0 << 12)
#define RK3576_HDMI_I2CM_WR_WRITE           (0x1 << 12)
#define RK3576_HDMI_I2CM_WR_EXT_READ        (0x2 << 12)
#define RK3576_HDMI_I2CM_FM_EN              (1 << 16)
#define RK3576_HDMI_I2CM_NBYTES_SHIFT       0
#define RK3576_HDMI_I2CM_NBYTES_MASK        (0xf << 0)

/* I2CM_INTERFACE_CONTROL1: slave / segment / offset pointers */

#define RK3576_HDMI_I2CM_SLVADDR_SHIFT      0
#define RK3576_HDMI_I2CM_SLVADDR_MASK       (0x7f << 0)
#define RK3576_HDMI_I2CM_ADDR_SHIFT         8
#define RK3576_HDMI_I2CM_ADDR_MASK          (0xff << 8)
#define RK3576_HDMI_I2CM_SEGADDR_SHIFT      16
#define RK3576_HDMI_I2CM_SEGADDR_MASK       (0x7f << 16)
#define RK3576_HDMI_I2CM_SEGPTR_SHIFT       24
#define RK3576_HDMI_I2CM_SEGPTR_MASK        (0xffu << 24)

/* I2CM_CONTROL0 */

#define RK3576_HDMI_I2CM_EXECUTE            (1 << 0)

/* Standard DDC addresses */

#define RK3576_HDMI_DDC_ADDR                0x50 /* EDID                   */
#define RK3576_HDMI_DDC_SEGADDR             0x30 /* E-DDC segment pointer  */
#define RK3576_HDMI_SCDC_ADDR               0x54 /* SCDC                   */

/* SCDC (status and control data channel), needed above 340 MHz TMDS */

#define RK3576_HDMI_SCDC_CONFIG0            0x0180
#define RK3576_HDMI_SCDC_I2C_FM_EN          (1 << 12)

/* Scrambling / TMDS */

#define RK3576_HDMI_SCRAMB_CONFIG0          0x01a0
#define RK3576_HDMI_SCRAMB_EN               (1 << 0)

/* Link (TMDS vs FRL, DVI vs HDMI) */

#define RK3576_HDMI_LINK_CONFIG0            0x01b0
#define RK3576_HDMI_LINK_OPMODE_DVI         (1 << 0)

/* Video interface -- input side (from the VOP) */

#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG0 0x0800
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG1 0x0804
#define RK3576_HDMI_VIDEO_INTERFACE_CONFIG2 0x0808

/* VIDEO_INTERFACE_CONFIG0 */

#define RK3576_HDMI_VID_MAP_CFG_SHIFT       0
#define RK3576_HDMI_VID_MAP_CFG_MASK        (0x1f << 0)
#define RK3576_HDMI_VID_MAP_RGB_8BIT        0x01
#define RK3576_HDMI_VID_MAP_RGB_10BIT       0x03
#define RK3576_HDMI_VID_MAP_RGB_12BIT       0x05
#define RK3576_HDMI_VID_MAP_YUV444_8BIT     0x09
#define RK3576_HDMI_VID_MAP_YUV422_12BIT    0x16
#define RK3576_HDMI_VID_MAP_YUV420_8BIT     0x1c
#define RK3576_HDMI_VID_CONV_EN             (1 << 5)
#define RK3576_HDMI_VID_PACKING_MODE_SHIFT  8
#define RK3576_HDMI_VID_PACKING_MODE_MASK   (0x3 << 8)
#define RK3576_HDMI_VID_HSYNC_POL_LOW       (1 << 16)
#define RK3576_HDMI_VID_VSYNC_POL_LOW       (1 << 17)
#define RK3576_HDMI_VID_DE_POL_LOW          (1 << 18)

/* VIDEO_INTERFACE_CONFIG1: active pixel / line counts (informational for
 * the packet scheduler; the real timing generator lives in the VOP).
 */

#define RK3576_HDMI_VID_HACTIVE_SHIFT       0
#define RK3576_HDMI_VID_HACTIVE_MASK        (0xffff << 0)
#define RK3576_HDMI_VID_VACTIVE_SHIFT       16
#define RK3576_HDMI_VID_VACTIVE_MASK        (0xffffu << 16)

/* VIDEO_INTERFACE_CONFIG2: blanking */

#define RK3576_HDMI_VID_HBLANK_SHIFT        0
#define RK3576_HDMI_VID_HBLANK_MASK         (0xffff << 0)
#define RK3576_HDMI_VID_VBLANK_SHIFT        16
#define RK3576_HDMI_VID_VBLANK_MASK         (0xffffu << 16)

/* Video packing */

#define RK3576_HDMI_VIDEO_PACKING_CONFIG0   0x081c

/* Audio interface (I2S input from SAI6) */

#define RK3576_HDMI_AUDIO_INTERFACE_CONFIG0 0x0820
#define RK3576_HDMI_AUDIO_INTERFACE_CONFIG1 0x0824
#define RK3576_HDMI_AUDIO_INTERFACE_CONFIG2 0x0828
#define RK3576_HDMI_AUDIO_INTERFACE_CONFIG3 0x082c

#define RK3576_HDMI_AUD_IFACE_I2S           (0 << 0)
#define RK3576_HDMI_AUD_IFACE_SPDIF         (1 << 0)
#define RK3576_HDMI_AUD_IFACE_MASK          (0x3 << 0)
#define RK3576_HDMI_AUD_I2S_MODE_STD        (0 << 4)
#define RK3576_HDMI_AUD_WIDTH_SHIFT         8
#define RK3576_HDMI_AUD_WIDTH_MASK          (0x1f << 8)
#define RK3576_HDMI_AUD_CHANNELS_SHIFT      16
#define RK3576_HDMI_AUD_CHANNELS_MASK       (0x1f << 16)
#define RK3576_HDMI_AUD_PACKET_SAMPFIT_MASK (0x1f << 0)

/* Audio clock regeneration (ACR): N and CTS */

#define RK3576_HDMI_AUDPKT_ACR_CONFIG0      0x0900
#define RK3576_HDMI_AUDPKT_ACR_CONFIG1      0x0904
#define RK3576_HDMI_AUDPKT_ACR_CONTROL0     0x0908
#define RK3576_HDMI_AUDPKT_ACR_N_SHIFT      12
#define RK3576_HDMI_AUDPKT_ACR_N_MASK       (0xfffffu << 12)
#define RK3576_HDMI_AUDPKT_ACR_CTS_SHIFT    12
#define RK3576_HDMI_AUDPKT_ACR_CTS_MASK     (0xfffffu << 12)
#define RK3576_HDMI_AUDPKT_ACR_CTS_SW_SEL   (1 << 0)

/* Packet scheduler */

#define RK3576_HDMI_PKTSCHED_CONFIG0        0x04b0
#define RK3576_HDMI_PKTSCHED_PKT_CONFIG0    0x04b4
#define RK3576_HDMI_PKTSCHED_PKT_CONFIG1    0x04b8
#define RK3576_HDMI_PKTSCHED_PKT_CONFIG2    0x04bc
#define RK3576_HDMI_PKTSCHED_PKT_CONFIG3    0x04c0
#define RK3576_HDMI_PKTSCHED_PKT_CONTROL0   0x04c4
#define RK3576_HDMI_PKTSCHED_PKT_SEND_AUTO  0x04c8
#define RK3576_HDMI_PKTSCHED_PKT_SEND_MAN   0x04cc
#define RK3576_HDMI_PKTSCHED_PKT_EN         0x04d0

#define RK3576_HDMI_PKTSCHED_NULL_TX_EN     (1 << 0)
#define RK3576_HDMI_PKTSCHED_ACR_TX_EN      (1 << 1)
#define RK3576_HDMI_PKTSCHED_AUDS_TX_EN     (1 << 2)
#define RK3576_HDMI_PKTSCHED_AVI_TX_EN      (1 << 16)
#define RK3576_HDMI_PKTSCHED_AUDI_TX_EN     (1 << 20)
#define RK3576_HDMI_PKTSCHED_DRMI_TX_EN     (1 << 17)
#define RK3576_HDMI_PKTSCHED_VSI_TX_EN      (1 << 21)

/* InfoFrame payload windows.  Each packet occupies seven 32-bit words:
 * CONTENTS0 holds the three header bytes in [7:0], [15:8], [23:16]; the
 * following words hold the packet body bytes, four per word.
 */

#define RK3576_HDMI_PKT_AVI_CONTENTS0       0x0540
#define RK3576_HDMI_PKT_AUDI_CONTENTS0      0x0560
#define RK3576_HDMI_PKT_DRMI_CONTENTS0      0x0580
#define RK3576_HDMI_PKT_CONTENTS_WORDS      7

/* Interrupt blocks.  Each has STATUS / MASK_N / CLEAR / FORCE at +0/+4/
 * +8/+0xc.  MASK_N is active low (write 1 to unmask).
 */

#define RK3576_HDMI_MAINUNIT_0_INT_STATUS   0x0ce0
#define RK3576_HDMI_MAINUNIT_0_INT_MASK_N   0x0ce4
#define RK3576_HDMI_MAINUNIT_0_INT_CLEAR    0x0ce8
#define RK3576_HDMI_MAINUNIT_0_INT_FORCE    0x0cec
#define RK3576_HDMI_MAINUNIT_1_INT_STATUS   0x0cf0
#define RK3576_HDMI_MAINUNIT_1_INT_MASK_N   0x0cf4
#define RK3576_HDMI_MAINUNIT_1_INT_CLEAR    0x0cf8
#define RK3576_HDMI_MAINUNIT_1_INT_FORCE    0x0cfc
#define RK3576_HDMI_AVP_0_INT_STATUS        0x0d00
#define RK3576_HDMI_AVP_0_INT_MASK_N        0x0d04
#define RK3576_HDMI_AVP_0_INT_CLEAR         0x0d08
#define RK3576_HDMI_AVP_1_INT_STATUS        0x0d10
#define RK3576_HDMI_AVP_1_INT_MASK_N        0x0d14
#define RK3576_HDMI_AVP_1_INT_CLEAR         0x0d18
#define RK3576_HDMI_CEC_INT_STATUS          0x0d60
#define RK3576_HDMI_EARCRX_0_INT_STATUS     0x0d80

/* MAINUNIT_1 carries the I2C master completion / error events. */

#define RK3576_HDMI_I2CM_OP_DONE_IRQ        (1 << 12)
#define RK3576_HDMI_I2CM_READ_REQ_IRQ       (1 << 13)
#define RK3576_HDMI_I2CM_NACK_RCVD_IRQ      (1 << 14)
#define RK3576_HDMI_I2CM_ARB_LOST_IRQ       (1 << 15)
#define RK3576_HDMI_I2CM_IRQ_ALL                                              \
  (RK3576_HDMI_I2CM_OP_DONE_IRQ | RK3576_HDMI_I2CM_READ_REQ_IRQ |             \
   RK3576_HDMI_I2CM_NACK_RCVD_IRQ | RK3576_HDMI_I2CM_ARB_LOST_IRQ)

/* ==========================================================================
 * HDPTX PHY
 * ========================================================================*/

/* Register "number" to byte offset.  See the block comment above. */

#define RK3576_HDPTX_REG(n)                 ((n) * 4)

#define RK3576_HDPTX_CMN(n)                 RK3576_HDPTX_REG(0x0000 + (n))
#define RK3576_HDPTX_SB(n)                  RK3576_HDPTX_REG(0x0100 + (n))
#define RK3576_HDPTX_LNTOP(n)               RK3576_HDPTX_REG(0x0200 + (n))
#define RK3576_HDPTX_LANE(n)                RK3576_HDPTX_REG(0x0300 + (n))

/* CMN block: reference clock and ROPLL.
 *
 * TODO: verify against the RK3576 TRM.  Taken from the public Samsung
 * HDPTX programming model used on RK3588.
 */

#define RK3576_HDPTX_CMN_REG0008            RK3576_HDPTX_CMN(0x08)
#define RK3576_HDPTX_CMN_LCPLL_EN           (1 << 6)
#define RK3576_HDPTX_CMN_ROPLL_EN           (1 << 5)

/* ROPLL PMS dividers */

#define RK3576_HDPTX_CMN_ROPLL_PMS_MDIV     RK3576_HDPTX_CMN(0x51)
#define RK3576_HDPTX_CMN_ROPLL_PMS_MDIV_AFC RK3576_HDPTX_CMN(0x55)
#define RK3576_HDPTX_CMN_ROPLL_PMS_PDIV     RK3576_HDPTX_CMN(0x59)
#define RK3576_HDPTX_CMN_ROPLL_PDIV_SHIFT   4
#define RK3576_HDPTX_CMN_ROPLL_REFDIV_MASK  0x0f
#define RK3576_HDPTX_CMN_ROPLL_PMS_SDIV     RK3576_HDPTX_CMN(0x5a)
#define RK3576_HDPTX_CMN_ROPLL_SDIV_SHIFT   4
#define RK3576_HDPTX_CMN_ROPLL_SDM_EN       RK3576_HDPTX_CMN(0x5c)
#define RK3576_HDPTX_CMN_ROPLL_SDM_EN_BIT   (1 << 6)
#define RK3576_HDPTX_CMN_ROPLL_SDM_RSTN_BIT (1 << 5)
#define RK3576_HDPTX_CMN_ROPLL_SDM_DENO     RK3576_HDPTX_CMN(0x5e)
#define RK3576_HDPTX_CMN_ROPLL_SDM_NUM_SIGN RK3576_HDPTX_CMN(0x5f)
#define RK3576_HDPTX_CMN_ROPLL_SDM_NUM      RK3576_HDPTX_CMN(0x60)
#define RK3576_HDPTX_CMN_ROPLL_SDC_N        RK3576_HDPTX_CMN(0x64)
#define RK3576_HDPTX_CMN_ROPLL_SDC_NUM      RK3576_HDPTX_CMN(0x69)
#define RK3576_HDPTX_CMN_ROPLL_SDC_DENO     RK3576_HDPTX_CMN(0x6c)
#define RK3576_HDPTX_CMN_ROPLL_IQDIV_RSTN   RK3576_HDPTX_CMN(0x86)
#define RK3576_HDPTX_CMN_ROPLL_IQDIV_RSTN_B (1 << 5)

/* LNTOP block: protocol select and data bus width */

#define RK3576_HDPTX_LNTOP_REG0200          RK3576_HDPTX_LNTOP(0x00)
#define RK3576_HDPTX_LNTOP_PROT_TMDS        0x06
#define RK3576_HDPTX_LNTOP_REG0206          RK3576_HDPTX_LNTOP(0x06)
#define RK3576_HDPTX_LNTOP_WIDTH_40BIT      0x07
#define RK3576_HDPTX_LNTOP_REG0207          RK3576_HDPTX_LNTOP(0x07)

/* LANE block: per lane control.  Lane stride is 0x40 registers. */

#define RK3576_HDPTX_LANE_STRIDE            0x40
#define RK3576_HDPTX_LANE_NUM               4

#define RK3576_HDPTX_LANE_REG(lane, n)                                        \
  RK3576_HDPTX_LANE(((lane) * RK3576_HDPTX_LANE_STRIDE) + (n))

#define RK3576_HDPTX_LANE_DRV_LVL           0x12 /* TMDS drive strength    */
#define RK3576_HDPTX_LANE_DRV_PRE           0x13 /* pre-emphasis           */
#define RK3576_HDPTX_LANE_SER_EN            0x19 /* serializer enable      */
#define RK3576_HDPTX_LANE_SER_EN_BIT        (1 << 0)

/* HDPTX PHY GRF (hiword-mask writes: [31:16] = write enable mask) */

#define RK3576_HDPTXPHY_GRF_CON0            0x0000
#define RK3576_HDPTXPHY_GRF_CON1            0x0004
#define RK3576_HDPTXPHY_GRF_CON2            0x0008
#define RK3576_HDPTXPHY_GRF_STATUS0         0x0080

#define RK3576_HDPTXPHY_I_BGR_EN            (1 << 5)
#define RK3576_HDPTXPHY_I_BIAS_EN           (1 << 6)
#define RK3576_HDPTXPHY_I_PLL_EN            (1 << 7)

#define RK3576_HDPTXPHY_O_PLL_LOCK_DONE     (1 << 0)
#define RK3576_HDPTXPHY_O_PHY_CLK_RDY       (1 << 1)
#define RK3576_HDPTXPHY_O_PHY_RDY           (1 << 2)
#define RK3576_HDPTXPHY_O_SB_RDY            (1 << 3)

/* VO1 GRF: HDMI TX glue.  The hot-plug detect level is sampled here.
 *
 * TODO: verify the status register offset and the HPD bit position
 * against the RK3576 TRM "VO1 GRF" chapter.
 */

#define RK3576_VO1_GRF_SOC_CON0             0x0000
#define RK3576_VO1_GRF_SOC_STATUS0          0x0080
#define RK3576_VO1_GRF_HDMI_HPD_LEVEL       (1 << 1)

/* Hiword-mask helper: value in [15:0], write-enable mask in [31:16]. */

#define RK3576_HDMI_HIWORD(mask, value)                                       \
  ((uint32_t)((mask) << 16) | (uint32_t)((value) & (mask)))

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_HDMI_H */
