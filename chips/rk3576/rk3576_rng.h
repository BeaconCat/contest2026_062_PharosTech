/****************************************************************************
 * chips/rk3576/rk3576_rng.h
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
 * Public API of the RK3576 hardware true random number generator driver.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_RNG_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_RNG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_rng_initialize
 *
 * Description:
 *   Bring the RKRNG block out of reset, ungate its clock and enable the
 *   ring oscillator entropy source.  Idempotent: repeated calls are
 *   harmless.  up_rnginitialize() calls this automatically, so a board only
 *   needs to call it directly when CONFIG_DEV_RANDOM is not selected but
 *   rk3576_rng_read() is still wanted (for example to seed a WPA supplicant
 *   nonce or to randomise a MAC address).
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rng_initialize(void);

/****************************************************************************
 * Name: rk3576_rng_read
 *
 * Description:
 *   Fill a caller supplied buffer with hardware generated random bytes.
 *   The call blocks (it polls the generator) until the whole buffer has
 *   been filled or the hardware times out.  It is safe to call from
 *   multiple threads; access to the block is serialised internally.
 *
 *   This function must not be called from interrupt context because it
 *   takes a mutex.
 *
 * Input Parameters:
 *   buf - Destination buffer, must be able to hold len bytes.
 *   len - Number of random bytes requested.  Zero is a no-op.
 *
 * Returned Value:
 *   OK (0) when len bytes were written to buf; a negated errno value on
 *   failure (-EINVAL for a NULL buffer, -ETIMEDOUT when the generator did
 *   not produce data in time).
 *
 ****************************************************************************/

int rk3576_rng_read(uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_RNG_H */
