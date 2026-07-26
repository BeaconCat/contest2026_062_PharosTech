/****************************************************************************
 * chips/rk3576/rk3576_otp.h
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

#ifndef __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_OTP_H
#define __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_OTP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef CONFIG_RK3576_OTP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Byte offset and length of the fuse cells published by the vendor device
 * tree (otp@2a580000).  Offsets are byte offsets inside the fuse array.
 */

#define RK3576_OTP_CELL_CPU_CODE_OFFSET     0x02
#define RK3576_OTP_CELL_CPU_CODE_SIZE       0x02
#define RK3576_OTP_CELL_CPU_VERSION_OFFSET  0x05
#define RK3576_OTP_CELL_CPU_VERSION_SIZE    0x01
#define RK3576_OTP_CELL_CPU_ID_OFFSET       0x0a
#define RK3576_OTP_CELL_CPU_ID_SIZE         0x10
#define RK3576_OTP_CELL_CPUB_LEAKAGE_OFFSET 0x1e
#define RK3576_OTP_CELL_CPUL_LEAKAGE_OFFSET 0x1f
#define RK3576_OTP_CELL_NPU_LEAKAGE_OFFSET  0x20
#define RK3576_OTP_CELL_GPU_LEAKAGE_OFFSET  0x21
#define RK3576_OTP_CELL_LOG_LEAKAGE_OFFSET  0x22
#define RK3576_OTP_CELL_LEAKAGE_SIZE        0x01
#define RK3576_OTP_CELL_CPUB_OPP_OFFSET     0x30
#define RK3576_OTP_CELL_CPUL_OPP_OFFSET     0x36
#define RK3576_OTP_CELL_NPU_OPP_OFFSET      0x42
#define RK3576_OTP_CELL_GPU_OPP_OFFSET      0x48
#define RK3576_OTP_CELL_LOGIC_OPP_OFFSET    0x4e
#define RK3576_OTP_CELL_OPP_SIZE            0x06

/* The cpu-version cell only carries 3 bits starting at bit 3
 * (device tree "bits = <3 3>").
 */

#define RK3576_OTP_CPU_VERSION_SHIFT 3
#define RK3576_OTP_CPU_VERSION_MASK  0x07

/* Length of an Ethernet/WLAN station address derived from the chip ID. */

#define RK3576_OTP_MAC_SIZE 6

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_otp_initialize
 *
 * Description:
 *   Initialise the OTP controller driver and register the read-only
 *   character device /dev/otp.  The device supports pread()/read() and
 *   lseek(); it can never be opened for writing.
 *
 *   Safe to call more than once; subsequent calls are no-ops.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_otp_initialize(void);

/****************************************************************************
 * Name: rk3576_otp_read
 *
 * Description:
 *   Read a range of raw bytes out of the fuse array.  Arbitrary byte
 *   offsets and lengths are supported; the driver internally reads whole
 *   16-bit OTP words and trims the result.
 *
 * Input Parameters:
 *   offset - Byte offset inside the fuse array.
 *   buf    - Destination buffer.
 *   len    - Number of bytes to read.
 *
 * Returned Value:
 *   The number of bytes read on success; a negated errno value on failure.
 *
 ****************************************************************************/

ssize_t rk3576_otp_read(uint32_t offset, void *buf, size_t len);

/****************************************************************************
 * Name: rk3576_otp_read_cpu_id
 *
 * Description:
 *   Read the 16-byte factory chip ID (device tree cell "id@a").  The value
 *   is unique per die and is the recommended seed for deriving MAC
 *   addresses and serial numbers.
 *
 * Input Parameters:
 *   id - Buffer receiving RK3576_OTP_CELL_CPU_ID_SIZE bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_id(uint8_t id[RK3576_OTP_CELL_CPU_ID_SIZE]);

/****************************************************************************
 * Name: rk3576_otp_read_cpu_code
 *
 * Description:
 *   Read the 16-bit CPU code (device tree cell "cpu-code@2"), e.g. 0x3576.
 *
 * Input Parameters:
 *   code - Receives the CPU code.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_code(uint16_t *code);

/****************************************************************************
 * Name: rk3576_otp_read_cpu_version
 *
 * Description:
 *   Read the 3-bit silicon revision (device tree cell "cpu-version@5",
 *   bits [5:3] of the fuse byte).
 *
 * Input Parameters:
 *   version - Receives the silicon revision.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_version(uint8_t *version);

/****************************************************************************
 * Name: rk3576_otp_get_mac
 *
 * Description:
 *   Derive a stable, per-board station address from the factory chip ID.
 *   The result is a locally administered unicast address, so it never
 *   collides with a vendor OUI, and it is stable across reboots because it
 *   only depends on fuses.  Different interfaces on the same board must
 *   pass different index values to obtain different addresses.
 *
 * Input Parameters:
 *   index - Interface index (0 for the first interface).
 *   mac   - Buffer receiving RK3576_OTP_MAC_SIZE bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_otp_get_mac(uint8_t index, uint8_t mac[RK3576_OTP_MAC_SIZE]);

#endif /* CONFIG_RK3576_OTP */
#endif /* __VENDOR_ROCKCHIP_CHIPS_RK3576_RK3576_OTP_H */
