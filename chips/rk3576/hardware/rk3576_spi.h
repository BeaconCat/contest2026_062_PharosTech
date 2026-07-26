/****************************************************************************
 * chips/rk3576/hardware/rk3576_spi.h
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
 * RK3576 SPI controller register definitions.
 *
 * The block is the classic Rockchip SPI master/slave controller (device
 * tree compatible "rockchip,rk3066-spi"), five instances SPI0..SPI4, each
 * with a 0x1000 register window and a 32-entry TX/RX FIFO.
 *
 * Unlike the CRU/GRF/PWM blocks these registers are NOT hiword-masked: a
 * plain read-modify-write is used.
 *
 * NOTE: this controller drives a single data lane per direction (standard
 * 4-wire Motorola SPI, plus TI-SSP and National Microwire frame formats).
 * Dual/quad (QSPI) line widths are provided by the separate RK3576 FSPI
 * (flexible SPI) controllers, not by this IP - see the comment on
 * RK3576_SPI_LANES_MAX below.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base addresses (RK3576 TRM, "SPI" chapter).
 *
 * TODO: move these to hardware/rk3576_memorymap.h together with the other
 * peripheral bases once the integration branch picks the driver up.  They
 * are defined here (guarded) so the driver builds stand-alone.
 */

#ifndef RK3576_SPI0_ADDR
#  define RK3576_SPI0_ADDR 0x2acf0000
#endif
#ifndef RK3576_SPI1_ADDR
#  define RK3576_SPI1_ADDR 0x2ad00000
#endif
#ifndef RK3576_SPI2_ADDR
#  define RK3576_SPI2_ADDR 0x2ad10000
#endif
#ifndef RK3576_SPI3_ADDR
#  define RK3576_SPI3_ADDR 0x2ad20000
#endif
#ifndef RK3576_SPI4_ADDR
#  define RK3576_SPI4_ADDR 0x2ad30000
#endif

/* Number of controller instances and per-controller hardware limits. */

#define RK3576_SPI_NPORTS    5  /* SPI0 .. SPI4                          */
#define RK3576_SPI_NCS       2  /* Native chip selects per controller    */
#define RK3576_SPI_FIFO_LEN  32 /* TX and RX FIFO depth, in frames       */

/* Maximum number of frames in one hardware transfer.  CTRLR1 holds
 * (frames - 1) in a 16-bit field, so 0xffff frames is the ceiling; longer
 * software transfers are split into chunks of this size.
 */

#define RK3576_SPI_MAX_FRAMES 0xffff

/* Data lanes supported by this IP.  The Rockchip "rk3066-spi" block is
 * single-lane only (one MOSI, one MISO).  Dual/quad-lane displays such as
 * the ST77916 QSPI round panels must be driven by the RK3576 FSPI
 * controller instead.  The constant exists so higher layers can query the
 * capability instead of hard-coding the assumption.
 */

#define RK3576_SPI_LANES_MAX 1

/* Register offsets *********************************************************/

#define RK3576_SPI_CTRLR0  0x0000 /* Control register 0                   */
#define RK3576_SPI_CTRLR1  0x0004 /* Control register 1 (frame count - 1) */
#define RK3576_SPI_ENR     0x0008 /* Enable register                      */
#define RK3576_SPI_SER     0x000c /* Slave (chip) enable register         */
#define RK3576_SPI_BAUDR   0x0010 /* Baud rate divider (even values only) */
#define RK3576_SPI_TXFTLR  0x0014 /* TX FIFO threshold level              */
#define RK3576_SPI_RXFTLR  0x0018 /* RX FIFO threshold level              */
#define RK3576_SPI_TXFLR   0x001c /* TX FIFO level (RO)                   */
#define RK3576_SPI_RXFLR   0x0020 /* RX FIFO level (RO)                   */
#define RK3576_SPI_SR      0x0024 /* Status register (RO)                 */
#define RK3576_SPI_IPR     0x0028 /* Interrupt polarity register          */
#define RK3576_SPI_IMR     0x002c /* Interrupt mask register              */
#define RK3576_SPI_ISR     0x0030 /* Interrupt status (masked, RO)        */
#define RK3576_SPI_RISR    0x0034 /* Raw interrupt status (RO)            */
#define RK3576_SPI_ICR     0x0038 /* Interrupt clear register             */
#define RK3576_SPI_DMACR   0x003c /* DMA control register                 */
#define RK3576_SPI_DMATDLR 0x0040 /* DMA TX data level (burst watermark)  */
#define RK3576_SPI_DMARDLR 0x0044 /* DMA RX data level (burst watermark)  */
#define RK3576_SPI_TXDR    0x0400 /* TX FIFO data entry (WO)              */
#define RK3576_SPI_RXDR    0x0800 /* RX FIFO data entry (RO)              */

/* CTRLR0 (0x00) ************************************************************/

#define SPI_CTRLR0_DFS_SHIFT    0 /* [1:0] Data frame size            */
#define SPI_CTRLR0_DFS_MASK     (0x3 << SPI_CTRLR0_DFS_SHIFT)
#define SPI_CTRLR0_DFS_4BIT     (0x0 << SPI_CTRLR0_DFS_SHIFT)
#define SPI_CTRLR0_DFS_8BIT     (0x1 << SPI_CTRLR0_DFS_SHIFT)
#define SPI_CTRLR0_DFS_16BIT    (0x2 << SPI_CTRLR0_DFS_SHIFT)

#define SPI_CTRLR0_CFS_SHIFT    2 /* [5:2] Microwire control frame size */
#define SPI_CTRLR0_CFS_MASK     (0xf << SPI_CTRLR0_CFS_SHIFT)

#define SPI_CTRLR0_SCPH         (1 << 6)  /* Serial clock phase      */
#define SPI_CTRLR0_SCPOL        (1 << 7)  /* Serial clock polarity   */

#define SPI_CTRLR0_CSM_SHIFT    8 /* [9:8] Idle cycles between frames */
#define SPI_CTRLR0_CSM_MASK     (0x3 << SPI_CTRLR0_CSM_SHIFT)
#define SPI_CTRLR0_CSM_0CYCLE   (0x0 << SPI_CTRLR0_CSM_SHIFT)

#define SPI_CTRLR0_SSD          (1 << 10) /* 1 = half-cycle ss_n hold  */
#define SPI_CTRLR0_EM_BIG       (1 << 11) /* 1 = big endian            */
#define SPI_CTRLR0_FBM_LSB      (1 << 12) /* 1 = LSB transmitted first */
#define SPI_CTRLR0_BHT_8BIT     (1 << 13) /* 1 = 8-bit APB FIFO access */

#define SPI_CTRLR0_RSD_SHIFT    14 /* [15:14] RX sample delay, in cycles */
#define SPI_CTRLR0_RSD_MASK     (0x3 << SPI_CTRLR0_RSD_SHIFT)

#define SPI_CTRLR0_FRF_SHIFT    16 /* [17:16] Frame format */
#define SPI_CTRLR0_FRF_MASK     (0x3 << SPI_CTRLR0_FRF_SHIFT)
#define SPI_CTRLR0_FRF_SPI      (0x0 << SPI_CTRLR0_FRF_SHIFT) /* Motorola  */
#define SPI_CTRLR0_FRF_SSP      (0x1 << SPI_CTRLR0_FRF_SHIFT) /* TI SSP    */
#define SPI_CTRLR0_FRF_MICROWIRE (0x2 << SPI_CTRLR0_FRF_SHIFT)

#define SPI_CTRLR0_XFM_SHIFT    18 /* [19:18] Transfer mode */
#define SPI_CTRLR0_XFM_MASK     (0x3 << SPI_CTRLR0_XFM_SHIFT)
#define SPI_CTRLR0_XFM_TR       (0x0 << SPI_CTRLR0_XFM_SHIFT) /* TX and RX */
#define SPI_CTRLR0_XFM_TO       (0x1 << SPI_CTRLR0_XFM_SHIFT) /* TX only   */
#define SPI_CTRLR0_XFM_RO       (0x2 << SPI_CTRLR0_XFM_SHIFT) /* RX only   */

#define SPI_CTRLR0_OPM_SLAVE    (1 << 20) /* 0 = master, 1 = slave      */
#define SPI_CTRLR0_MTM_3WIRE    (1 << 21) /* Microwire 3-wire transmit  */
#define SPI_CTRLR0_SOI_SHIFT    23        /* [24:23] Slave output index */
#define SPI_CTRLR0_SOI_MASK     (0x3 << SPI_CTRLR0_SOI_SHIFT)

/* CTRLR1 (0x04): number of frames to transfer, minus one. */

#define SPI_CTRLR1_NDF_MASK     0x0000ffff

/* ENR (0x08) ***************************************************************/

#define SPI_ENR_ENABLE          (1 << 0)

/* SER (0x0c): one bit per native chip select. */

#define SPI_SER_CS(n)           (1 << (n))
#define SPI_SER_MASK            0x00000003

/* BAUDR (0x10): sclk_out = spi_clk / BAUDR.  Only even values are legal. */

#define SPI_BAUDR_MIN           2
#define SPI_BAUDR_MAX           0xfffe

/* SR (0x24) ****************************************************************/

#define SPI_SR_BUSY             (1 << 0) /* Master transfer in progress */
#define SPI_SR_TF_FULL          (1 << 1) /* TX FIFO full                */
#define SPI_SR_TF_EMPTY         (1 << 2) /* TX FIFO empty               */
#define SPI_SR_RF_EMPTY         (1 << 3) /* RX FIFO empty               */
#define SPI_SR_RF_FULL          (1 << 4) /* RX FIFO full                */
#define SPI_SR_SLAVE_TX_BUSY    (1 << 5) /* Slave transfer in progress  */

/* IMR / ISR / RISR / ICR (0x2c .. 0x38) share the same bit layout. */

#define SPI_INT_TXEI            (1 << 0) /* TX FIFO empty               */
#define SPI_INT_TXOI            (1 << 1) /* TX FIFO overflow            */
#define SPI_INT_RXUI            (1 << 2) /* RX FIFO underflow           */
#define SPI_INT_RXOI            (1 << 3) /* RX FIFO overflow            */
#define SPI_INT_RXFI            (1 << 4) /* RX FIFO threshold reached   */
#define SPI_INT_CS_INACTIVE     (1 << 6) /* ss_n went inactive          */
#define SPI_INT_ALL             0x0000005f

/* DMACR (0x3c) *************************************************************/

#define SPI_DMACR_RDE           (1 << 0) /* RX DMA enable */
#define SPI_DMACR_TDE           (1 << 1) /* TX DMA enable */

/* PL330 peripheral request lines for SPI4, from the vendor device tree
 * (dmas = <&dmac0 12 &dmac0 13>, dma-names = "tx", "rx").
 */

#define RK3576_SPI4_DRQ_TX      12
#define RK3576_SPI4_DRQ_RX      13

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SPI_H */
