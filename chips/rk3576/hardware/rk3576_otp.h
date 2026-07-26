/****************************************************************************
 * chips/rk3576/hardware/rk3576_otp.h
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
 * RK3576 OTP (eFuse) controller register definitions.
 *
 * The block is a Rockchip OTP v2 controller ("rockchip,rk3576-otp",
 * otp@2a580000, 0x400 register window).  Only the "user mode" read path is
 * described here: the driver never programs a fuse, so none of the
 * programming (SBPI / burn) registers are defined on purpose.
 *
 * Several control registers are HIWORD write-masked: the upper 16 bits are
 * a per-bit write-enable for the lower 16 bits.  A write therefore always
 * carries both the value and the mask of the bits it wants to change.
 *
 * The OTP array is addressed in 16-bit words; a "word address" is the byte
 * offset of a fuse cell divided by RK3576_OTP_NBYTES.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_OTP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_OTP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.
 *
 * TODO: move this definition to hardware/rk3576_memorymap.h together with
 * the other peripheral bases once the integration commit lands; it is kept
 * local for now so that this driver is self-contained.
 */

#ifndef RK3576_OTP_ADDR
#  define RK3576_OTP_ADDR 0x2a580000 /* otp@2a580000, 0x400 window     */
#endif

/* Geometry of the fuse array as seen through the user-mode read path. */

#define RK3576_OTP_NBYTES 2     /* One OTP word is 16 bits            */
#define RK3576_OTP_SIZE   0x400 /* Readable user area, in bytes       */

/* Register offsets *********************************************************/

#define RK3576_OTP_USER_CTRL   0x0100 /* User mode control (HIWORD)    */
#define RK3576_OTP_USER_ADDR   0x0104 /* User mode word address(HIWORD)*/
#define RK3576_OTP_USER_ENABLE 0x0108 /* User mode FSM start (HIWORD)  */
#define RK3576_OTP_USER_QP     0x0120 /* Read ECC / parity status      */
#define RK3576_OTP_USER_Q      0x0124 /* Read data (16 bits valid)     */
#define RK3576_OTP_INT_STATUS  0x0304 /* Interrupt status (W1C)        */

/* USER_CTRL (0x100) ********************************************************/

#define OTP_USER_CTRL_USE_USER      (1 << 0)  /* Take the array into user
                                               * mode                     */
#define OTP_USER_CTRL_USE_USER_MASK (1 << 16) /* Write-enable for bit 0   */

/* USER_ADDR (0x104) ********************************************************/

#define OTP_USER_ADDR_MASK      0xffff0000 /* Write-enable for [15:0]     */
#define OTP_USER_ADDR_VAL_MASK  0x0000ffff /* Word address field          */

/* USER_ENABLE (0x108) ******************************************************/

#define OTP_USER_ENABLE_FSM      (1 << 0)  /* Start one read transaction  */
#define OTP_USER_ENABLE_FSM_MASK (1 << 16) /* Write-enable for bit 0      */

/* USER_QP (0x120) **********************************************************/

/* The two ECC status bits are only meaningful when they agree: both clear
 * means "no ECC applied", both set means "ECC checked and correct".  Any
 * other combination flags an uncorrectable read.
 */

#define OTP_USER_QP_ECC_MASK 0x000000c0

/* INT_STATUS (0x304) - write 1 to clear ************************************/

#define OTP_INT_STATUS_SBPI_DONE (1 << 1) /* Programming FSM done         */
#define OTP_INT_STATUS_USER_DONE (1 << 2) /* User mode read done          */

#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_HARDWARE_RK3576_OTP_H */
