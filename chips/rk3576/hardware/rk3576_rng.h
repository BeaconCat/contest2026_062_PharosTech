/****************************************************************************
 * chips/rk3576/hardware/rk3576_rng.h
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
 * Register definitions for the RK3576 "rockchip,rkrng" true random number
 * generator (vendor DTS node rng@2a410000, 0x200 bytes of registers,
 * GIC INTID 213 = SPI 181 + 32).
 *
 * The entropy source is a set of free running ring oscillators; the block
 * post-processes the raw bits and presents 256 bits of output at a time in
 * RNG_DOUT0..7.
 *
 * The control register is HIWORD write-masked: the upper 16 bits are a
 * per-bit write enable for the lower 16 bits.
 *
 * The RK3576 TRM does not document the security sub-system, so the bit
 * layout below follows the register naming published by the Rockchip
 * hardware RNG driver for the same IP block.  It has not been verified on
 * hardware yet.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_RNG_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_RNG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.
 * TODO: move to rk3576_memorymap.h once the integration branch takes it.
 */

#ifndef RK3576_RKRNG_ADDR
#  define RK3576_RKRNG_ADDR 0x2a410000
#endif

#define RK3576_RKRNG_SIZE 0x200

/* Register offsets *********************************************************/

#define RK3576_RNG_CTL         0x0400 /* Control (HIWORD write-masked)   */
#define RK3576_RNG_STATE       0x0404 /* Status, write 1 to clear        */
#define RK3576_RNG_AUTO_RQSTS  0x0408 /* Automatic reseed request count  */
#define RK3576_RNG_DOUT0       0x0410 /* Random data out, word 0         */
#define RK3576_RNG_DOUT(n)     (RK3576_RNG_DOUT0 + ((n) << 2))

/* Number of 32-bit words in one RNG_DOUT burst (256 bits). */

#define RK3576_RNG_DOUT_NWORDS 8
#define RK3576_RNG_DOUT_NBYTES (RK3576_RNG_DOUT_NWORDS * 4)

/* HIWORD write-enable mask covering all 16 writable bits. */

#define RK3576_RNG_WRITE_MASK 0xffff0000u

/* RNG_CTL (0x0400), HIWORD write-masked ************************************/

#define RK3576_RNG_CTL_START       (1 << 0) /* Start one conversion       */
#define RK3576_RNG_CTL_ENABLE      (1 << 1) /* Enable the ring oscillator */

#define RK3576_RNG_CTL_RING_SHIFT  2        /* Ring oscillator speed      */
#define RK3576_RNG_CTL_RING_MASK   (3 << RK3576_RNG_CTL_RING_SHIFT)
#define RK3576_RNG_CTL_RING_SLOWEST (0 << RK3576_RNG_CTL_RING_SHIFT)
#define RK3576_RNG_CTL_RING_SLOW    (1 << RK3576_RNG_CTL_RING_SHIFT)
#define RK3576_RNG_CTL_RING_FAST    (2 << RK3576_RNG_CTL_RING_SHIFT)
#define RK3576_RNG_CTL_RING_FASTEST (3 << RK3576_RNG_CTL_RING_SHIFT)

#define RK3576_RNG_CTL_LEN_SHIFT   4        /* Output length per start    */
#define RK3576_RNG_CTL_LEN_MASK    (3 << RK3576_RNG_CTL_LEN_SHIFT)
#define RK3576_RNG_CTL_LEN_64BIT   (0 << RK3576_RNG_CTL_LEN_SHIFT)
#define RK3576_RNG_CTL_LEN_128BIT  (1 << RK3576_RNG_CTL_LEN_SHIFT)
#define RK3576_RNG_CTL_LEN_192BIT  (2 << RK3576_RNG_CTL_LEN_SHIFT)
#define RK3576_RNG_CTL_LEN_256BIT  (3 << RK3576_RNG_CTL_LEN_SHIFT)

/* RNG_STATE (0x0404), write 1 to clear *************************************/

#define RK3576_RNG_STATE_DONE  (1 << 0) /* Random data ready in DOUTn     */

/* Secure CRU bits owning the TRNG (SECURECRU_GATE_CON00 / SOFTRST_CON00,
 * RK3576 TRM Part1 "SECURECRU").  A gate bit set to 1 disables the clock.
 */

#define RK3576_SECURE_CRU_GATE_CON00    0x0800
#define RK3576_SECURE_CRU_SOFTRST_CON00 0x0a00
#define RK3576_SECURE_CRU_WRITE_MASK    0xffff0000u

#define RK3576_SECURE_GATE_HCLK_TRNG_NS (1 << 4)
#define RK3576_SECURE_RST_HRESETN_TRNG_NS (1 << 4)

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_RNG_H */
