/****************************************************************************
 * vendor/rockchip/chips/rk3576/hardware/rk3576_gmac.h
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
 * Register definitions for the two RK3576 gigabit Ethernet controllers.
 * The IP is a Synopsys DesignWare MAC 4.20a ("dwmac4") with an integrated
 * DMA.  Its register file is split into three windows inside the same 64 KiB
 * region:
 *
 *   0x0000..0x0BFF  MAC   (core, MDIO, address filter, management counters)
 *   0x0C00..0x0FFF  MTL   (per-queue FIFO configuration)
 *   0x1000..0x11FF  DMA   (global bus mode plus per-channel descriptor rings)
 *
 * Two Rockchip glue register banks sit outside the controller:
 *
 *   SDGMAC_GRF  (0x26038000)  RGMII/RMII mode select, clock source/divider
 *   IOC_GRF     (0x26040000)  RGMII TX/RX clock delay lines
 *
 * Both GRF banks are HIWORD registers: the upper 16 bits are a per-bit write
 * enable for the lower 16 bits.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_GMAC_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_GMAC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller instances *****************************************************/

#define RK3576_GMAC_NIFACES 2 /* Number of GMAC instances on the SoC */

/* Controller register bases.  These belong in rk3576_memorymap.h; they are
 * defined here (conditionally) until they are moved there.
 */

#define RK3576_GMAC_REGION_SIZE 0x10000

/* Interrupts (vendor DTS "macirq"):
 *
 *   gmac0: GIC INTID 325 (SPI 293), gmac1: GIC INTID 333 (SPI 301)
 *   INTID = SPI + 32
 *
 * Both are already published by chips/rk3576/include/irq.h as
 * RK3576_IRQ_GMAC0_SBD / RK3576_IRQ_GMAC1_SBD, which is what the driver
 * uses.  The secondary "eth_wake_irq" (INTID 330 / 338, the PMT wake-up
 * line) is not used: wake-on-LAN is not supported by this driver.
 */

/* Rockchip glue registers **************************************************/

/* SDGMAC_GRF: mode and clock routing.  This bank belongs in
 * rk3576_memorymap.h as well.
 */

#define RK3576_GMAC_GRF_CON_OFFSET(n)  (0x0020 + ((n)*4))
#define RK3576_GMAC_GRF_CLK_CON_OFFSET (0x0070)

/* HIWORD helpers: build a value whose upper half enables the written bits. */

#define RK3576_GMAC_HIWORD_BIT(b)    ((1u << ((b) + 16)) | (1u << (b)))
#define RK3576_GMAC_HIWORD_CLRBIT(b) (1u << ((b) + 16))
#define RK3576_GMAC_HIWORD_FIELD(v, mask, shift) \
  (((mask) << ((shift) + 16)) | (((v) & (mask)) << (shift)))

/* SDGMAC_GRF GMAC_CONn bits */

#define RK3576_GMAC_GRF_RMII_MODE  RK3576_GMAC_HIWORD_BIT(3)
#define RK3576_GMAC_GRF_RGMII_MODE RK3576_GMAC_HIWORD_CLRBIT(3)

/* SDGMAC_GRF CLK_CON1 bits.  The gmac1 field set is the gmac0 set shifted
 * up by RK3576_GMAC_GRF_CLK_SHIFT bits.
 */

#define RK3576_GMAC_GRF_CLK_SHIFT     8
#define RK3576_GMAC_GRF_CLK_SEL_IO    7 /* 1: clock from IO pad (input)   */
#define RK3576_GMAC_GRF_CLK_RMII_GATE 4 /* 1: gate the RMII clock         */
#define RK3576_GMAC_GRF_CLK_DIV_HI    6 /* Divider selector, high bit     */
#define RK3576_GMAC_GRF_CLK_DIV_LO    5 /* Divider selector, low bit      */

/* Divider selector encodings for RGMII (DIV_HI:DIV_LO) */

#define RK3576_GMAC_GRF_DIV_RGMII_1  0 /* 1000 Mbps: 125 MHz            */
#define RK3576_GMAC_GRF_DIV_RGMII_5  3 /* 100 Mbps:  25 MHz             */
#define RK3576_GMAC_GRF_DIV_RGMII_50 2 /* 10 Mbps:   2.5 MHz            */

/* IOC_GRF: RGMII delay lines.  One control word per instance. */

#define RK3576_GMAC_IOC_CON_OFFSET(n) (0x0020 + ((n)*4))

#define RK3576_GMAC_IOC_RXDLY_EN      RK3576_GMAC_HIWORD_BIT(15)
#define RK3576_GMAC_IOC_RXDLY_DIS     RK3576_GMAC_HIWORD_CLRBIT(15)
#define RK3576_GMAC_IOC_TXDLY_EN      RK3576_GMAC_HIWORD_BIT(7)
#define RK3576_GMAC_IOC_TXDLY_DIS     RK3576_GMAC_HIWORD_CLRBIT(7)
#define RK3576_GMAC_IOC_RXDLY_CFG(v)  RK3576_GMAC_HIWORD_FIELD(v, 0x7f, 8)
#define RK3576_GMAC_IOC_TXDLY_CFG(v)  RK3576_GMAC_HIWORD_FIELD(v, 0x7f, 0)

/* Board delay values taken from the vendor DTS.  Both ports are
 * "rgmii-rxid": the receive delay is produced inside the PHY, so only the
 * transmit delay line is programmed.
 */

#define RK3576_GMAC0_TX_DELAY 0x21
#define RK3576_GMAC1_TX_DELAY 0x20

/* MAC register offsets *****************************************************/

#define RK3576_GMAC_MAC_CONFIG        0x0000
#define RK3576_GMAC_MAC_EXT_CONFIG    0x0004
#define RK3576_GMAC_MAC_PKT_FILTER    0x0008
#define RK3576_GMAC_MAC_HASH_TABLE(n) (0x0010 + ((n)*4))
#define RK3576_GMAC_MAC_Q0_TX_FLOW    0x0070
#define RK3576_GMAC_MAC_RX_FLOW       0x0090
#define RK3576_GMAC_MAC_RXQ_CTRL0     0x00a0
#define RK3576_GMAC_MAC_INT_STATUS    0x00b0
#define RK3576_GMAC_MAC_INT_ENABLE    0x00b4
#define RK3576_GMAC_MAC_VERSION       0x0110
#define RK3576_GMAC_MAC_HW_FEATURE0   0x011c
#define RK3576_GMAC_MAC_HW_FEATURE1   0x0120
#define RK3576_GMAC_MAC_HW_FEATURE2   0x0124
#define RK3576_GMAC_MAC_HW_FEATURE3   0x0128
#define RK3576_GMAC_MAC_MDIO_ADDR     0x0200
#define RK3576_GMAC_MAC_MDIO_DATA     0x0204
#define RK3576_GMAC_MAC_ADDR_HIGH(n)  (0x0300 + ((n)*8))
#define RK3576_GMAC_MAC_ADDR_LOW(n)   (0x0304 + ((n)*8))

/* MAC_CONFIG bits */

#define RK3576_GMAC_CFG_RE   (1u << 0)  /* Receiver enable              */
#define RK3576_GMAC_CFG_TE   (1u << 1)  /* Transmitter enable           */
#define RK3576_GMAC_CFG_DC   (1u << 4)  /* Deferral check               */
#define RK3576_GMAC_CFG_DCRS (1u << 9)  /* Disable carrier sense        */
#define RK3576_GMAC_CFG_DM   (1u << 13) /* Full duplex                  */
#define RK3576_GMAC_CFG_FES  (1u << 14) /* Speed: 1 = 100, 0 = 10/1000  */
#define RK3576_GMAC_CFG_PS   (1u << 15) /* Port select: 1 = MII (10/100)*/
#define RK3576_GMAC_CFG_JE   (1u << 16) /* Jumbo frame enable           */
#define RK3576_GMAC_CFG_JD   (1u << 17) /* Jabber disable               */
#define RK3576_GMAC_CFG_ACS  (1u << 20) /* Auto pad/CRC strip           */
#define RK3576_GMAC_CFG_CST  (1u << 21) /* CRC strip for type packets   */
#define RK3576_GMAC_CFG_IPC  (1u << 27) /* Checksum offload (RX)        */

/* MAC_PKT_FILTER bits */

#define RK3576_GMAC_FILTER_PR  (1u << 0)  /* Promiscuous                  */
#define RK3576_GMAC_FILTER_HMC (1u << 2)  /* Hash multicast               */
#define RK3576_GMAC_FILTER_PM  (1u << 4)  /* Pass all multicast           */
#define RK3576_GMAC_FILTER_RA  (1u << 31) /* Receive all                  */

/* MAC_RXQ_CTRL0: two bits per queue, 2 = enabled for DCB traffic */

#define RK3576_GMAC_RXQ_EN_DCB (2u << 0)

/* MAC_ADDR_HIGH bits */

#define RK3576_GMAC_ADDRHI_AE (1u << 31) /* Address enable               */

/* MAC_MDIO_ADDR bits */

#define RK3576_GMAC_MDIO_GB        (1u << 0) /* Busy / start              */
#define RK3576_GMAC_MDIO_C45E      (1u << 1) /* Clause 45 enable          */
#define RK3576_GMAC_MDIO_GOC_SHIFT 2
#define RK3576_GMAC_MDIO_GOC_MASK  (3u << 2)
#define RK3576_GMAC_MDIO_GOC_WR    (1u << 2) /* Write operation           */
#define RK3576_GMAC_MDIO_GOC_RD    (3u << 2) /* Read operation            */
#define RK3576_GMAC_MDIO_CR_SHIFT  8
#define RK3576_GMAC_MDIO_CR_MASK   (0xfu << 8)
#define RK3576_GMAC_MDIO_RDA_SHIFT 16
#define RK3576_GMAC_MDIO_RDA_MASK  (0x1fu << 16)
#define RK3576_GMAC_MDIO_PA_SHIFT  21
#define RK3576_GMAC_MDIO_PA_MASK   (0x1fu << 21)

/* CSR clock range encoding.  The field selects the divider applied to the
 * CSR (application) clock to produce MDC, which clause 22 caps at 2.5 MHz.
 * The driver picks the entry matching the measured aclk_mac rate.
 */

#define RK3576_GMAC_MDIO_CR_DIV42  0x0 /*  60 ..  100 MHz */
#define RK3576_GMAC_MDIO_CR_DIV62  0x1 /* 100 ..  150 MHz */
#define RK3576_GMAC_MDIO_CR_DIV16  0x2 /*  20 ..   35 MHz */
#define RK3576_GMAC_MDIO_CR_DIV26  0x3 /*  35 ..   60 MHz */
#define RK3576_GMAC_MDIO_CR_DIV102 0x4 /* 150 ..  250 MHz */
#define RK3576_GMAC_MDIO_CR_DIV124 0x5 /* 250 ..  300 MHz */
#define RK3576_GMAC_MDIO_CR_DIV204 0x6 /* 300 ..  500 MHz */
#define RK3576_GMAC_MDIO_CR_DIV324 0x7 /* 500 ..  800 MHz */

#define RK3576_GMAC_MDIO_DATA_MASK 0xffffu

/* MTL register offsets *****************************************************/

#define RK3576_GMAC_MTL_OPMODE      0x0c00
#define RK3576_GMAC_MTL_TXQ0_OPMODE 0x0d00
#define RK3576_GMAC_MTL_TXQ0_DEBUG  0x0d08
#define RK3576_GMAC_MTL_RXQ0_OPMODE 0x0d30
#define RK3576_GMAC_MTL_RXQ0_DEBUG  0x0d38

/* MTL_TXQ0_OPMODE bits */

#define RK3576_GMAC_TXQ_FTQ        (1u << 0) /* Flush queue              */
#define RK3576_GMAC_TXQ_TSF        (1u << 1) /* Store and forward        */
#define RK3576_GMAC_TXQ_EN_SHIFT   2
#define RK3576_GMAC_TXQ_EN_MASK    (3u << 2)
#define RK3576_GMAC_TXQ_EN_ENABLED (2u << 2)
#define RK3576_GMAC_TXQ_TQS_SHIFT  16
#define RK3576_GMAC_TXQ_TQS_MASK   (0x1ffu << 16)

/* MTL_RXQ0_OPMODE bits */

#define RK3576_GMAC_RXQ_RSF       (1u << 5) /* Store and forward         */
#define RK3576_GMAC_RXQ_FEP       (1u << 4) /* Forward error packets     */
#define RK3576_GMAC_RXQ_FUP       (1u << 3) /* Forward undersized good   */
#define RK3576_GMAC_RXQ_RQS_SHIFT 20
#define RK3576_GMAC_RXQ_RQS_MASK  (0x3ffu << 20)

/* MAC_HW_FEATURE1 FIFO size fields: size = 128 << field */

#define RK3576_GMAC_HWF1_RXFIFO_SHIFT 0
#define RK3576_GMAC_HWF1_RXFIFO_MASK  (0x1fu << 0)
#define RK3576_GMAC_HWF1_TXFIFO_SHIFT 6
#define RK3576_GMAC_HWF1_TXFIFO_MASK  (0x1fu << 6)
#define RK3576_GMAC_FIFO_BASE_BYTES   128

/* DMA register offsets *****************************************************/

#define RK3576_GMAC_DMA_MODE           0x1000
#define RK3576_GMAC_DMA_SYSBUS_MODE    0x1004
#define RK3576_GMAC_DMA_INT_STATUS     0x1008
#define RK3576_GMAC_DMA_CH0_CONTROL    0x1100
#define RK3576_GMAC_DMA_CH0_TX_CONTROL 0x1104
#define RK3576_GMAC_DMA_CH0_RX_CONTROL 0x1108
#define RK3576_GMAC_DMA_CH0_TXDESC_HI  0x1110
#define RK3576_GMAC_DMA_CH0_TXDESC_LO  0x1114
#define RK3576_GMAC_DMA_CH0_RXDESC_HI  0x1118
#define RK3576_GMAC_DMA_CH0_RXDESC_LO  0x111c
#define RK3576_GMAC_DMA_CH0_TXTAIL     0x1120
#define RK3576_GMAC_DMA_CH0_RXTAIL     0x1128
#define RK3576_GMAC_DMA_CH0_TXRING_LEN 0x112c
#define RK3576_GMAC_DMA_CH0_RXRING_LEN 0x1130
#define RK3576_GMAC_DMA_CH0_INT_ENABLE 0x1134
#define RK3576_GMAC_DMA_CH0_CUR_TXDESC 0x1144
#define RK3576_GMAC_DMA_CH0_CUR_RXDESC 0x114c
#define RK3576_GMAC_DMA_CH0_STATUS     0x1160

/* DMA_MODE bits */

#define RK3576_GMAC_DMAMODE_SWR (1u << 0) /* Software reset (self clear)  */

/* DMA_SYSBUS_MODE bits */

#define RK3576_GMAC_SYSBUS_FB           (1u << 0) /* Fixed burst          */
#define RK3576_GMAC_SYSBUS_BLEN4        (1u << 1)
#define RK3576_GMAC_SYSBUS_BLEN8        (1u << 2)
#define RK3576_GMAC_SYSBUS_BLEN16       (1u << 3)
#define RK3576_GMAC_SYSBUS_AAL          (1u << 12) /* Address aligned beats*/
#define RK3576_GMAC_SYSBUS_MB           (1u << 14) /* Mixed burst          */
#define RK3576_GMAC_SYSBUS_RD_OSR_SHIFT 16
#define RK3576_GMAC_SYSBUS_RD_OSR_MASK  (0x3fu << 16)
#define RK3576_GMAC_SYSBUS_WR_OSR_SHIFT 24
#define RK3576_GMAC_SYSBUS_WR_OSR_MASK  (0x3fu << 24)

/* Outstanding request limits from the vendor DTS stmmac-axi-config
 * (snps,rd_osr_lmt = 8, snps,wr_osr_lmt = 4).  The register holds
 * "limit - 1".
 */

#define RK3576_GMAC_RD_OSR_LMT 8
#define RK3576_GMAC_WR_OSR_LMT 4

/* DMA_CH0_CONTROL bits */

#define RK3576_GMAC_CHCTRL_PBLX8     (1u << 16) /* Multiply PBL by 8         */
#define RK3576_GMAC_CHCTRL_DSL_SHIFT 18         /* Descriptor skip length    */
#define RK3576_GMAC_CHCTRL_DSL_MASK  (7u << 18)

/* DMA_CH0_TX_CONTROL bits */

#define RK3576_GMAC_TXCTRL_ST        (1u << 0)  /* Start transmission      */
#define RK3576_GMAC_TXCTRL_OSF       (1u << 4)  /* Operate on second frame */
#define RK3576_GMAC_TXCTRL_TSE       (1u << 12) /* TCP segmentation        */
#define RK3576_GMAC_TXCTRL_PBL_SHIFT 16
#define RK3576_GMAC_TXCTRL_PBL_MASK  (0x3fu << 16)

/* DMA_CH0_RX_CONTROL bits */

#define RK3576_GMAC_RXCTRL_SR         (1u << 0) /* Start receive           */
#define RK3576_GMAC_RXCTRL_RBSZ_SHIFT 1         /* Buffer size, bits 14:1  */
#define RK3576_GMAC_RXCTRL_RBSZ_MASK  (0x3fffu << 1)
#define RK3576_GMAC_RXCTRL_PBL_SHIFT  16
#define RK3576_GMAC_RXCTRL_PBL_MASK   (0x3fu << 16)

/* Programmable burst length used for both directions */

#define RK3576_GMAC_DMA_PBL 8

/* DMA_CH0_STATUS / DMA_CH0_INT_ENABLE bits (identical layout) */

#define RK3576_GMAC_DMAINT_TI  (1u << 0)  /* Transmit complete            */
#define RK3576_GMAC_DMAINT_TPS (1u << 1)  /* Transmit process stopped     */
#define RK3576_GMAC_DMAINT_TBU (1u << 2)  /* TX buffer unavailable        */
#define RK3576_GMAC_DMAINT_RI  (1u << 6)  /* Receive complete             */
#define RK3576_GMAC_DMAINT_RBU (1u << 7)  /* RX buffer unavailable        */
#define RK3576_GMAC_DMAINT_RPS (1u << 8)  /* Receive process stopped      */
#define RK3576_GMAC_DMAINT_RWT (1u << 9)  /* Receive watchdog timeout     */
#define RK3576_GMAC_DMAINT_ETI (1u << 10) /* Early transmit               */
#define RK3576_GMAC_DMAINT_ERI (1u << 11) /* Early receive                */
#define RK3576_GMAC_DMAINT_FBE (1u << 12) /* Fatal bus error              */
#define RK3576_GMAC_DMAINT_CDE (1u << 13) /* Context descriptor error     */
#define RK3576_GMAC_DMAINT_AIS (1u << 14) /* Abnormal interrupt summary   */
#define RK3576_GMAC_DMAINT_NIS (1u << 15) /* Normal interrupt summary     */

#define RK3576_GMAC_DMAINT_ALL 0xffffu

/* Descriptors **************************************************************/

/* dwmac4 uses four 32-bit words per descriptor in ring mode. */

struct rk3576_gmac_desc_s
{
  volatile uint32_t des0;
  volatile uint32_t des1;
  volatile uint32_t des2;
  volatile uint32_t des3;
};

/* Transmit descriptor, read (host owned) format.
 *
 *   TDES0  buffer 1 address, low 32 bits
 *   TDES1  buffer 2 address (unused, single buffer per frame)
 *   TDES2  lengths and interrupt control
 *   TDES3  frame length, first/last flags, ownership
 */

#define RK3576_GMAC_TDES2_B1L_MASK  (0x3fffu << 0)
#define RK3576_GMAC_TDES2_IOC       (1u << 31) /* Interrupt on completion  */

#define RK3576_GMAC_TDES3_FL_MASK   (0x7fffu << 0) /* Frame length         */
#define RK3576_GMAC_TDES3_CIC_SHIFT 16             /* Checksum insertion   */
#define RK3576_GMAC_TDES3_CIC_FULL  (3u << 16)
#define RK3576_GMAC_TDES3_LD        (1u << 28) /* Last descriptor      */
#define RK3576_GMAC_TDES3_FD        (1u << 29) /* First descriptor     */
#define RK3576_GMAC_TDES3_CTXT      (1u << 30) /* Context descriptor   */
#define RK3576_GMAC_TDES3_OWN       (1u << 31) /* Owned by DMA         */

/* Transmit descriptor, write-back format: TDES3 error summary */

#define RK3576_GMAC_TDES3_ES (1u << 15) /* Error summary        */

/* Receive descriptor, read (host owned) format.
 *
 *   RDES0  buffer 1 address, low 32 bits
 *   RDES1  reserved
 *   RDES2  buffer 2 address (unused)
 *   RDES3  ownership plus buffer valid flags
 */

#define RK3576_GMAC_RDES3_BUF1V (1u << 24) /* Buffer 1 address valid   */
#define RK3576_GMAC_RDES3_BUF2V (1u << 25) /* Buffer 2 address valid   */
#define RK3576_GMAC_RDES3_IOC   (1u << 30) /* Interrupt on completion  */
#define RK3576_GMAC_RDES3_OWN   (1u << 31) /* Owned by DMA             */

/* Receive descriptor, write-back format */

#define RK3576_GMAC_RDES3_PL_MASK (0x7fffu << 0) /* Packet length        */
#define RK3576_GMAC_RDES3_ES      (1u << 15)     /* Error summary        */
#define RK3576_GMAC_RDES3_LD      (1u << 28)     /* Last descriptor      */
#define RK3576_GMAC_RDES3_FD      (1u << 29)     /* First descriptor     */
#define RK3576_GMAC_RDES3_CTXT    (1u << 30)     /* Context descriptor   */

/* Generic clause-22 PHY registers ******************************************/

#define RK3576_GMAC_MII_BMCR            0x00 /* Basic mode control            */
#define RK3576_GMAC_MII_BMSR            0x01 /* Basic mode status             */
#define RK3576_GMAC_MII_PHYID1          0x02
#define RK3576_GMAC_MII_PHYID2          0x03
#define RK3576_GMAC_MII_ADVERTISE       0x04 /* Auto-negotiation advertisement*/
#define RK3576_GMAC_MII_LPA             0x05 /* Link partner ability          */
#define RK3576_GMAC_MII_CTRL1000        0x09 /* 1000BASE-T control            */
#define RK3576_GMAC_MII_STAT1000        0x0a /* 1000BASE-T status             */

#define RK3576_GMAC_BMCR_RESET          (1u << 15)
#define RK3576_GMAC_BMCR_LOOPBACK       (1u << 14)
#define RK3576_GMAC_BMCR_SPEED100       (1u << 13)
#define RK3576_GMAC_BMCR_ANENABLE       (1u << 12)
#define RK3576_GMAC_BMCR_PDOWN          (1u << 11)
#define RK3576_GMAC_BMCR_ANRESTART      (1u << 9)
#define RK3576_GMAC_BMCR_FULLDPLX       (1u << 8)
#define RK3576_GMAC_BMCR_SPEED1000      (1u << 6)

#define RK3576_GMAC_BMSR_LSTATUS        (1u << 2)
#define RK3576_GMAC_BMSR_ANEGCAPA       (1u << 3)
#define RK3576_GMAC_BMSR_ANEGDONE       (1u << 5)

#define RK3576_GMAC_ADV_10HALF          (1u << 5)
#define RK3576_GMAC_ADV_10FULL          (1u << 6)
#define RK3576_GMAC_ADV_100HALF         (1u << 7)
#define RK3576_GMAC_ADV_100FULL         (1u << 8)
#define RK3576_GMAC_ADV_PAUSE           (1u << 10)
#define RK3576_GMAC_ADV_SELECT_802_3    (1u << 0)

#define RK3576_GMAC_CTRL1000_1000FULL   (1u << 9)
#define RK3576_GMAC_CTRL1000_1000HALF   (1u << 8)

#define RK3576_GMAC_STAT1000_LP1000FULL (1u << 11)
#define RK3576_GMAC_STAT1000_LP1000HALF (1u << 10)

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_GMAC_H */
