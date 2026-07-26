/****************************************************************************
 * chips/rk3576/hardware/rk3576_pdm.h
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
 * Register description of the RK3576 PDM (pulse-density-modulation) audio
 * receiver.  The block clocks out a PDM bit clock to an external MEMS
 * microphone array, samples the returned 1-bit streams on both clock edges
 * (one data line carries two microphones, left on one edge, right on the
 * other) and performs the PDM to PCM decimation in hardware.  Up to four
 * data lines ("paths") are supported, i.e. up to eight microphones.
 *
 * The SoC instantiates two identical controllers:
 *
 *   PDM0 (0x273B0000) – PMU/"always-on" bus domain, SPI 202 (INTID 234)
 *   PDM1 (0x2A6E0000) – main bus domain,            SPI 203 (INTID 235)
 *
 * Both nodes are "disabled" in the vendor Linux device tree, so the field
 * layout below is taken from the Rockchip PDM IP as used across the RK3xxx
 * family.  Offsets are confirmed against the TRM register summary; the bit
 * positions marked "verify" still need a TRM cross-check on hardware.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PDM_H
#define __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PDM_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base addresses.
 *
 * TODO: move these two definitions to hardware/rk3576_memorymap.h once the
 * chip-wide address map is next touched; they are kept local so that this
 * driver does not conflict with other in-flight work on that header.
 */

/* GIC interrupt IDs.  The device tree carries the GIC SPI number; the NuttX
 * IRQ number (== GIC INTID) is SPI + 32.
 *
 *   PDM0: interrupts = <0 0xca 4>  -> SPI 202 -> INTID 234
 *   PDM1: interrupts = <0 0xcb 4>  -> SPI 203 -> INTID 235
 *
 * TODO: move to chips/rk3576/include/irq.h as RK3576_IRQ_PDM0 / _PDM1.
 */

/* PL330 peripheral request line feeding the PDM receive FIFO.  Both device
 * tree nodes declare "dmas = <&dmacN 4>, dma-names = rx".
 */

#define RK3576_PDM_DMA_RX_DRQ 4

/* Register offsets ********************************************************/

#define RK3576_PDM_SYSCONFIG  0x0000 /* Reset / start-stop control     */
#define RK3576_PDM_CTRL0      0x0004 /* Path enables, valid data width */
#define RK3576_PDM_CTRL1      0x0008 /* Filter / gain control          */
#define RK3576_PDM_CLK_CTRL   0x000c /* PDM clock: rate, polarity      */
#define RK3576_PDM_HPF_CTRL   0x0010 /* High-pass filter control       */
#define RK3576_PDM_FIFO_CTRL  0x0014 /* Receive FIFO thresholds        */
#define RK3576_PDM_DMA_CTRL   0x0018 /* DMA enable + watermark         */
#define RK3576_PDM_INT_EN     0x001c /* Interrupt enable               */
#define RK3576_PDM_INT_CLR    0x0020 /* Interrupt clear (W1C)          */
#define RK3576_PDM_INT_ST     0x0024 /* Interrupt status (RO)          */
#define RK3576_PDM_RXFIFO     0x0030 /* Receive FIFO data (RO)         */
#define RK3576_PDM_DATA_VALID 0x0038 /* Per-path data-valid enables    */
#define RK3576_PDM_VERSION    0x0044 /* Version ID (RO)                */

/* PDM_SYSCONFIG (0x00) ****************************************************/

#define PDM_SYSCONFIG_RX_CLR   (1 << 0) /* Flush receive path (W1T)     */
#define PDM_SYSCONFIG_RX_START (1 << 2) /* 1 = run, 0 = stop            */
#define PDM_SYSCONFIG_RST_MASK (PDM_SYSCONFIG_RX_CLR)

/* PDM_CTRL0 (0x04) ********************************************************/

#define PDM_CTRL0_VDW_MASK  (0x1f << 0) /* Valid data width - 1      */
#define PDM_CTRL0_VDW_SHIFT 0
#define PDM_CTRL0_VDW(n) \
  ((((n)-1) << PDM_CTRL0_VDW_SHIFT) & PDM_CTRL0_VDW_MASK)

#define PDM_CTRL0_MODE_MASK  (0x3 << 8) /* Sampling mode             */
#define PDM_CTRL0_MODE_SHIFT 8
#define PDM_CTRL0_MODE_LJ    (0 << PDM_CTRL0_MODE_SHIFT) /* Left just */
#define PDM_CTRL0_MODE_RJ    (1 << PDM_CTRL0_MODE_SHIFT) /* Right just*/

#define PDM_CTRL0_HWT_EN     (1 << 21) /* Hardware trigger enable   */

#define PDM_CTRL0_PATH0_EN   (1 << 27) /* Data line 0 (mic 0/1)     */
#define PDM_CTRL0_PATH1_EN   (1 << 28) /* Data line 1 (mic 2/3)     */
#define PDM_CTRL0_PATH2_EN   (1 << 29) /* Data line 2 (mic 4/5)     */
#define PDM_CTRL0_PATH3_EN   (1 << 30) /* Data line 3 (mic 6/7)     */
#define PDM_CTRL0_PATH_MASK  (0xfu << 27)
#define PDM_CTRL0_PATH_SHIFT 27

/* Each data line carries two microphones (one per PDM clock edge). */

#define RK3576_PDM_MICS_PER_PATH 2
#define RK3576_PDM_NPATHS        4
#define RK3576_PDM_MAX_CHANNELS  (RK3576_PDM_NPATHS * RK3576_PDM_MICS_PER_PATH)

/* PDM_CTRL1 (0x08) ********************************************************/

#define PDM_CTRL1_FILTER_GAIN_MASK  (0x1f << 0) /* CIC output gain       */
#define PDM_CTRL1_FILTER_GAIN_SHIFT 0
#define PDM_CTRL1_FILTER_GAIN(n) \
  (((n) << PDM_CTRL1_FILTER_GAIN_SHIFT) & PDM_CTRL1_FILTER_GAIN_MASK)

/* Default CIC output gain.  The decimation filter has unity gain at this
 * setting for the 16-bit / OSR-128 configuration used by the voice path.
 */

#define RK3576_PDM_FILTER_GAIN_DEFAULT 0x08

/* PDM_CLK_CTRL (0x0c) *****************************************************/

#define PDM_CLK_CTRL_SAMPLERATE_MASK  (0x7 << 0) /* Decimation selector  */
#define PDM_CLK_CTRL_SAMPLERATE_SHIFT 0
#define PDM_CLK_CTRL_DS_RATIO_32      (0 << 0) /* clk_out = 32  x fs   */
#define PDM_CLK_CTRL_DS_RATIO_64      (1 << 0) /* clk_out = 64  x fs   */
#define PDM_CLK_CTRL_DS_RATIO_128     (2 << 0) /* clk_out = 128 x fs   */

#define PDM_CLK_CTRL_CKP_MASK         (1 << 4) /* Clock polarity       */
#define PDM_CLK_CTRL_CKP_NORMAL       (0 << 4)
#define PDM_CLK_CTRL_CKP_INVERTED     (1 << 4)

#define PDM_CLK_CTRL_CLK_EN           (1 << 5) /* PDM clock output on  */

#define PDM_CLK_CTRL_FD_RATIO_MASK    (1 << 6) /* Fractional divider   */
#define PDM_CLK_CTRL_FD_RATIO_40      (0 << 6) /* clk_out = clk / 4.0  */
#define PDM_CLK_CTRL_FD_RATIO_35      (1 << 6) /* clk_out = clk / 3.5  */

/* PDM_HPF_CTRL (0x10) *****************************************************/

#define PDM_HPF_CTRL_CF_MASK (0x3 << 0) /* Cut-off frequency select     */
#define PDM_HPF_CTRL_CF_3_79 (0 << 0)   /* 3.79 Hz                      */
#define PDM_HPF_CTRL_CF_60   (1 << 0)   /* 60 Hz                        */
#define PDM_HPF_CTRL_CF_243  (2 << 0)   /* 243 Hz                       */
#define PDM_HPF_CTRL_CF_493  (3 << 0)   /* 493 Hz                       */

#define PDM_HPF_CTRL_RXL_EN  (1 << 2) /* HPF on left  channels        */
#define PDM_HPF_CTRL_RXR_EN  (1 << 3) /* HPF on right channels        */

/* PDM_FIFO_CTRL (0x14) ****************************************************/

#define PDM_FIFO_CTRL_RFL_MASK  (0x7f << 0) /* Receive FIFO level (RO)  */
#define PDM_FIFO_CTRL_RFL_SHIFT 0

/* Depth of the receive FIFO in 32-bit words. */

#define RK3576_PDM_FIFO_DEPTH 32

/* PDM_DMA_CTRL (0x18) *****************************************************/

#define PDM_DMA_CTRL_RDL_MASK  (0x1f << 0) /* Request when level > RDL   */
#define PDM_DMA_CTRL_RDL_SHIFT 0
#define PDM_DMA_CTRL_RDL(n) \
  ((((n)-1) << PDM_DMA_CTRL_RDL_SHIFT) & PDM_DMA_CTRL_RDL_MASK)
#define PDM_DMA_CTRL_RDE (1 << 8) /* Receive DMA request enable */

/* Receive DMA watermark, in FIFO words.  Half the FIFO gives the PL330
 * enough slack to answer a request before the FIFO overruns.
 */

#define RK3576_PDM_DMA_RDL (RK3576_PDM_FIFO_DEPTH / 2)

/* PDM_INT_EN (0x1c) / PDM_INT_CLR (0x20) / PDM_INT_ST (0x24) **************/

#define PDM_INT_RXFI (1 << 0) /* Receive FIFO threshold reached         */
#define PDM_INT_RXOI (1 << 1) /* Receive FIFO overrun                   */
#define PDM_INT_ALL  (PDM_INT_RXFI | PDM_INT_RXOI)

/* PDM_DATA_VALID (0x38) ***************************************************/

#define PDM_DATA_VALID_PATH0 (1 << 0)
#define PDM_DATA_VALID_PATH1 (1 << 1)
#define PDM_DATA_VALID_PATH2 (1 << 2)
#define PDM_DATA_VALID_PATH3 (1 << 3)
#define PDM_DATA_VALID_MASK  (0xfu << 0)

#endif /* __VENDOR_ROCKCHIP_RK3576_HARDWARE_RK3576_PDM_H */
