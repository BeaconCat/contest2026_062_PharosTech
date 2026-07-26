/****************************************************************************
 * chips/rk3576/hardware/rk3576_memorymap.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO banks (TRM) */

#define RK3576_GPIO0_ADDR 0x27320000
#define RK3576_GPIO1_ADDR 0x2AE10000
#define RK3576_GPIO2_ADDR 0x2AE20000
#define RK3576_GPIO3_ADDR 0x2AE30000
#define RK3576_GPIO4_ADDR 0x2AE40000
#define RK3576_PIO_ADDR   RK3576_GPIO0_ADDR

/* DesignWare 16550 UARTs (TRM). UART0 = debug console (vendor DTS earlycon).
 */

#define RK3576_UART0_ADDR  0x2AD40000
#define RK3576_UART1_ADDR  0x27310000
#define RK3576_UART2_ADDR  0x2AD50000
#define RK3576_UART3_ADDR  0x2AD60000
#define RK3576_UART4_ADDR  0x2AD70000
#define RK3576_UART5_ADDR  0x2AD80000
#define RK3576_UART6_ADDR  0x2AD90000
#define RK3576_UART7_ADDR  0x2ADA0000
#define RK3576_UART8_ADDR  0x2ADB0000
#define RK3576_UART9_ADDR  0x2ADC0000
#define RK3576_UART10_ADDR 0x2AFC0000
#define RK3576_UART11_ADDR 0x2AFD0000

/* PWM (Rockchip PWM v4) */
#define RK3576_PWM0_ADDR 0x27330000
#define RK3576_PWM1_ADDR 0x2ADD0000
#define RK3576_PWM2_ADDR 0x2ADE0000

/* Synopsys DesignWare MSHC (dw_mmc, same IP as rk3288/rk3399) */

#define RK3576_SDMMC_ADDR 0x2A310000 /* SD/MMC host (dw-mshc) */
#define RK3576_SDIO_ADDR  0x2A320000 /* SDIO host (dw-mshc)   */
#define RK3576_EMMC_ADDR  0x2A330000 /* eMMC host (dwcmshc)   */

/* USB OTG (Synopsys DesignWare USB3 / DWC3) */

#define RK3576_USB0_ADDR 0x23000000 /* USB OTG0 (DWC3)       */

/* I2C controller (Synopsys/Rockchip RK I2C, "rk3399-i2c" compatible). */

#define RK3576_I2C0_ADDR 0x27300000
#define RK3576_I2C1_ADDR 0x2ac40000
#define RK3576_I2C2_ADDR 0x2ac50000
#define RK3576_I2C3_ADDR 0x2ac60000
#define RK3576_I2C4_ADDR 0x2ac70000
#define RK3576_I2C5_ADDR 0x2ac80000
#define RK3576_I2C6_ADDR 0x2ac90000
#define RK3576_I2C7_ADDR 0x2aca0000
#define RK3576_I2C8_ADDR 0x2acb0000
#define RK3576_I2C9_ADDR 0x2ae80000

/* Clock & Reset Unit */

#define RK3576_CRU_ADDR         0x27200000
#define RK3576_PPLL_CRU_ADDR    0x27208000
#define RK3576_SECURE_CRU_ADDR  0x27210000
#define RK3576_PMU1_CRU_ADDR    0x27220000
#define RK3576_DDR0_CRU_ADDR    0x27228000
#define RK3576_DDR1_CRU_ADDR    0x27230000
#define RK3576_BIGCORE_CRU_ADDR 0x27238000
#define RK3576_LITCORE_CRU_ADDR 0x27240000
#define RK3576_CCI_CRU_ADDR     0x27248000

/* IOMUX */
#define RK3576_IOC_ADDR 0x26040000

/* DMA controller */

#define RK3576_DMAC0_ADDR 0x2ab90000

/* SPI controllers (Synopsys DesignWare SSI) */

#define RK3576_SPI4_ADDR 0x2ad30000

/* I3C masters (Synopsys DesignWare MIPI I3C host controller) */

#define RK3576_I3C0_ADDR 0x2abe0000
#define RK3576_I3C1_ADDR 0x2abf0000

/* Watchdog (Synopsys DesignWare WDT, non-secure instance) */

#define RK3576_WDT_ADDR 0x2ace0000

/* Analog / thermal converters */

#define RK3576_SARADC_ADDR 0x2ae00000 /* Rockchip SAR-ADC v2        */
#define RK3576_TSADC_ADDR  0x2ae70000 /* On-die temperature sensor  */

/* Security block: crypto engine, TRNG and OTP */

#define RK3576_CRYPTO_ADDR 0x2a400000 /* rockchip,crypto-v4         */
#define RK3576_RNG_ADDR    0x2a410000 /* rockchip,rkrng             */
#define RK3576_OTP_ADDR    0x2a580000 /* eFuse / OTP (no interrupt) */

/* Audio: PDM microphone-array receivers */

#define RK3576_PDM0_ADDR 0x273b0000 /* PMU bus domain  */
#define RK3576_PDM1_ADDR 0x2a6e0000 /* main bus domain */

/* Mailboxes: 14 identical 4KB windows, AP<->BB inter-core doorbells.
 * NINSTANCES counts the register windows; the number of doorbell channels
 * inside one window is RK3576_MAILBOX_NCHANNELS in hardware/rk3576_mailbox.h.
 */

#define RK3576_MAILBOX_ADDR       0x2ae50000
#define RK3576_MAILBOX_STRIDE     0x1000
#define RK3576_MAILBOX_NINSTANCES 14
#define RK3576_MAILBOXN_ADDR(n) \
  (RK3576_MAILBOX_ADDR + (n)*RK3576_MAILBOX_STRIDE)

/* Power management unit (power-management@27380000).  The register window
 * used by software starts at 0x27380000 and is 0x800 bytes long.
 */

#define RK3576_PMU_ADDR 0x27380000

/* Gigabit Ethernet controllers (Synopsys dwmac-4.20a) */

#define RK3576_GMAC0_ADDR 0x2a220000
#define RK3576_GMAC1_ADDR 0x2a230000

/* PCIe 2.1 root complexes (Synopsys DesignWare + Rockchip client glue).
 * Windows taken from the vendor DTS "reg" and "ranges" properties.
 */

#define RK3576_PCIE0_APB_ADDR 0x2a200000 /* client/glue registers, 64KB */
#define RK3576_PCIE0_DBI_ADDR 0x22000000 /* DesignWare DBI + iATU, 4MB  */
#define RK3576_PCIE0_CFG_ADDR 0x20000000 /* config window base          */
#define RK3576_PCIE0_CFG_SIZE 0x00100000
#define RK3576_PCIE0_IO_ADDR  0x20100000 /* PCI I/O window              */
#define RK3576_PCIE0_IO_SIZE  0x00100000
#define RK3576_PCIE0_MEM_ADDR 0x20200000 /* PCI 32-bit MMIO window      */
#define RK3576_PCIE0_MEM_SIZE 0x00e00000

#define RK3576_PCIE1_APB_ADDR 0x2a210000
#define RK3576_PCIE1_DBI_ADDR 0x22400000
#define RK3576_PCIE1_CFG_ADDR 0x21000000
#define RK3576_PCIE1_CFG_SIZE 0x00100000
#define RK3576_PCIE1_IO_ADDR  0x21100000
#define RK3576_PCIE1_IO_SIZE  0x00100000
#define RK3576_PCIE1_MEM_ADDR 0x21200000
#define RK3576_PCIE1_MEM_SIZE 0x00e00000

/* Naneng combo SerDes PHYs (PCIe / USB3 / SATA / SGMII) */

#define RK3576_COMBPHY0_ADDR 0x2b050000 /* feeds PCIe port 0 */
#define RK3576_COMBPHY1_ADDR 0x2b060000 /* feeds PCIe port 1 */

/* Display: VOP2 video output processor and its IOMMU */

#define RK3576_VOP_ADDR       0x27d00000 /* VOP main register file, 12KB  */
#define RK3576_VOP_GAMMA_ADDR 0x27d05000 /* Gamma LUT, 4KB                */
#define RK3576_VOP_ACM_ADDR   0x27d06400 /* ACM colour management, 2KB    */
#define RK3576_VOP_SHARP_ADDR 0x27d06c00 /* Sharpness, 768B               */
#define RK3576_VOP_IOMMU_ADDR 0x27d07e00 /* rockchip,iommu-v2 for the VOP */

/* Display: HDMI TX (DW HDMI TX QP) and the Rockchip/Samsung HDPTX PHY */

#define RK3576_HDMITX0_ADDR    0x27da0000 /* DW HDMI TX QP controller */
#define RK3576_HDMITX0_QP_ADDR 0x27db0000 /* QP extension window      */
#define RK3576_HDPTXPHY_ADDR   0x2b000000 /* HDPTX PHY register file  */

/* Video decoder (RKVDEC v383) and its IOMMU */

#define RK3576_VDEC_LINK_ADDR  0x27b00000
#define RK3576_VDEC_ADDR       0x27b00100
#define RK3576_VDEC_IOMMU_ADDR 0x27b00800

/* RGA2 2D acceleration engine (two identical cores, 4KB each) */

#define RK3576_RGA0_ADDR 0x27920000
#define RK3576_RGA1_ADDR 0x27930000

/* Camera: MIPI CSI-2 host controllers */

#define RK3576_CSI2HOST0_ADDR 0x27c80000
#define RK3576_CSI2HOST1_ADDR 0x27c90000
#define RK3576_CSI2HOST2_ADDR 0x27ca0000
#define RK3576_CSI2HOST3_ADDR 0x27cb0000
#define RK3576_CSI2HOST4_ADDR 0x27cc0000

/* Camera: MIPI RX D-PHY / combo DC-PHY hardware blocks */

#define RK3576_CSI2DPHY0_ADDR  0x2b030000
#define RK3576_CSI2DPHY1_ADDR  0x2b070000
#define RK3576_MIPI_DCPHY_ADDR 0x2b020000

/* Camera: ISP, VICAP (rkcif), VPSS and their IOMMUs */

#define RK3576_ISP_ADDR         0x27c00000
#define RK3576_ISP_IOMMU_ADDR   0x27c07f00
#define RK3576_VICAP_ADDR       0x27c10000
#define RK3576_VICAP_IOMMU_ADDR 0x27c10800
#define RK3576_VPSS_ADDR        0x27c30000
#define RK3576_VPSS_IOMMU_ADDR  0x27c33f00

/* NPU (RKNPU, 6 TOPS): two RKNN cores with a 32KB window each.  The IOMMU
 * v2 register banks live inside those windows at +0x2000 / +0x2100.
 */

#define RK3576_RKNPU0_ADDR      0x27700000 /* RKNN core0 */
#define RK3576_RKNPU1_ADDR      0x27708000 /* RKNN core1 */
#define RK3576_RKNPU_IOMMU_ADDR 0x27702000

/* GPU (ARM Mali-G52 MC3, Bifrost), 128KB window */

#define RK3576_MALI_ADDR 0x27800000

/* General register file (GRF) syscon slices */

#define RK3576_SYS_GRF_ADDR        0x2600a000
#define RK3576_GPU_GRF_ADDR        0x26016000
#define RK3576_VO0_GRF_ADDR        0x2601a000
#define RK3576_PHP_GRF_ADDR        0x26020000
#define RK3576_PIPE_PHY_GRF0_ADDR  0x26028000
#define RK3576_PIPE_PHY_GRF1_ADDR  0x2602a000
#define RK3576_HDPTXPHY_GRF_ADDR   0x26032000
#define RK3576_MIPI_DCPHY_GRF_ADDR 0x26034000
#define RK3576_VO1_GRF_ADDR        0x26036000
#define RK3576_SDGMAC_GRF_ADDR     0x26038000
#define RK3576_CSI2DPHY0_GRF_ADDR  0x2603a000
#define RK3576_CSI2DPHY1_GRF_ADDR  0x2604c000

/* Miscellaneous syscon */

#define RK3576_GPU_PVTPLL_ADDR 0x27268000

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H */
