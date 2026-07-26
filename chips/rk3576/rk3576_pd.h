/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pd.h
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
 * RK3576 PMU power-domain (PD) public API.
 *
 * NuttX has no generic power-domain framework (drivers/power hosts the
 * regulator, battery and PM subsystems only), so this module exports a
 * small explicit API instead.  A peripheral driver has to bring its power
 * domain up *before* touching any of its registers - a gated domain reads
 * back as all zeroes and swallows writes.
 *
 * Typical use from a peripheral driver:
 *
 *   ret = rk3576_pd_on(RK3576_PD_GPU);
 *   if (ret < 0)
 *     {
 *       return ret;
 *     }
 *
 * Domain identifiers match the "reg" property of the power-domain nodes
 * below power-management@27380000/power-controller in the vendor device
 * tree, i.e. they are the same numbers Linux uses in
 * <dt-bindings/power/rockchip,rk3576-power.h>.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_PD_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_PD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Power domain identifiers.
 *
 * The first block mirrors the device tree / Linux binding numbering.  The
 * parent relations encoded by the device tree hierarchy are:
 *
 *   PD_NPU  -> PD_NPUTOP -> PD_NPU0, PD_NPU1
 *   PD_NVM  -> PD_SDGMAC
 *   PD_PHP  -> PD_SUBPHP
 *   PD_VI   -> PD_VEPU0
 *   PD_VOP  -> PD_USB, PD_VO0, PD_VO1
 *
 * rk3576_pd_on() walks that chain automatically.
 */

#define RK3576_PD_NPU    0  /* VD_NPU voltage domain (NPU root)      */
#define RK3576_PD_NPUTOP 1  /* RKNN_TOP, NPU MCU/WDT/timer, BIU      */
#define RK3576_PD_NPU0   2  /* RKNN_CORE0                            */
#define RK3576_PD_NPU1   3  /* RKNN_CORE1                            */
#define RK3576_PD_GPU    4  /* VD_GPU: Mali, GPU PVTPLL, GPU_GRF     */
#define RK3576_PD_NVM    5  /* eMMC, FSPI0                           */
#define RK3576_PD_SDGMAC 6  /* SDMMC, SDIO, GMAC0/1, FSPI1, DSMC     */
#define RK3576_PD_USB    7  /* USB0 (DWC3), UFSHC, MMU2              */
#define RK3576_PD_PHP    8  /* USB1, PCIE0, SATA*, MMU0/1            */
#define RK3576_PD_SUBPHP 9  /* PCIE1, SATA0/1                        */
#define RK3576_PD_AUDIO  10 /* SAI0-4, SPDIF, ASRC, PDM1             */
#define RK3576_PD_VEPU0  11 /* Video encoder core 0                  */
#define RK3576_PD_VEPU1  12 /* Video encoder core 1                  */
#define RK3576_PD_VPU    13 /* JPEG, EBC, VDPP, RGA0/1               */
#define RK3576_PD_VDEC   14 /* VDPU (rkvdec)                         */
#define RK3576_PD_VI     15 /* ISP, VICAP, VPSS, CSI hosts           */
#define RK3576_PD_VO0    16 /* HDMITX, EDP, DSI host, HDCP0          */
#define RK3576_PD_VO1    17 /* DP, HDCP1, SAI7-9                     */
#define RK3576_PD_VOP    18 /* VOP core + VOP_GRF                    */

/* SoC-internal sub-domains of PD_VOP.  These have their own power gate
 * bits but no device tree node, because Linux folds them into the VOP
 * driver.  They must be powered together with RK3576_PD_VOP before the
 * corresponding VOP window types can be used.
 */

#define RK3576_PD_VOP_ESMART  19 /* VOP_ESMART0-3 windows                 */
#define RK3576_PD_VOP_CLUSTER 20 /* VOP_CLUSTER0-1 windows                */

#define RK3576_PD_NDOMAINS    21

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_pd_on
 *
 * Description:
 *   Power a domain up.  Parent domains are powered up first, recursively.
 *   The sequence performed for each domain is:
 *
 *     1. Release the software power-down request (PMU_PWR_GATE_SFTCONx).
 *     2. Wait for PMU_PWR_GATE_STS to report the domain as powered up.
 *     3. Release the SRAM retention request (PMU_MEM_PWR_GATE_SFTCONx).
 *     4. Release the bus idle request for every BIU of the domain and wait
 *        for both the acknowledge and the idle state to clear.
 *
 *   For the voltage domains (VD_NPU, VD_GPU) the corresponding
 *   PMU_VOL_GATE_CONx off-request bit is cleared before the power gate is
 *   released, as required by TRM section 6.5.7.
 *
 *   The call is idempotent: powering an already powered domain up is a
 *   no-op that returns OK.  Peripheral resets are *not* touched - those
 *   live in the CRU and belong to the individual peripheral drivers.
 *
 * Input Parameters:
 *   domain - One of the RK3576_PD_* identifiers.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure:
 *   -EINVAL for an unknown domain, -ETIMEDOUT if the power or bus idle
 *   handshake did not complete.
 *
 ****************************************************************************/

int rk3576_pd_on(int domain);

/****************************************************************************
 * Name: rk3576_pd_off
 *
 * Description:
 *   Power a domain down.  The reverse sequence is used: request bus idle
 *   for every BIU of the domain, wait for the acknowledge, enable SRAM
 *   retention, then assert the power gate and wait for the status.  For
 *   the voltage domains the PMU_VOL_GATE_CONx off request is asserted last.
 *
 *   Child domains are NOT powered down implicitly - power them down first,
 *   otherwise the hardware handshake for the parent will time out.  The
 *   caller is responsible for having quiesced the peripherals in the
 *   domain (no outstanding bus transactions, DMA stopped, IRQs masked).
 *
 * Input Parameters:
 *   domain - One of the RK3576_PD_* identifiers.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_pd_off(int domain);

/****************************************************************************
 * Name: rk3576_pd_is_on
 *
 * Description:
 *   Read the current power state of a domain from PMU_PWR_GATE_STS.
 *
 * Input Parameters:
 *   domain - One of the RK3576_PD_* identifiers.
 *
 * Returned Value:
 *   true if the domain is powered up, false if it is powered down or the
 *   domain identifier is invalid.
 *
 ****************************************************************************/

bool rk3576_pd_is_on(int domain);

/****************************************************************************
 * Name: rk3576_pd_name
 *
 * Description:
 *   Return the human readable name of a domain, for logging.
 *
 * Input Parameters:
 *   domain - One of the RK3576_PD_* identifiers.
 *
 * Returned Value:
 *   A pointer to a constant string; "unknown" for an invalid identifier.
 *
 ****************************************************************************/

const char *rk3576_pd_name(int domain);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_PD_H */
