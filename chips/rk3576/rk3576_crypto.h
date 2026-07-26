/****************************************************************************
 * chips/rk3576/rk3576_crypto.h
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
 * Public API of the RK3576 hardware crypto accelerator driver.
 *
 * The AES engine is reachable through the standard NuttX arch hook
 * aes_cypher() declared in <nuttx/crypto/crypto.h>; the entry points below
 * are the chip level API used by that hook and by in-kernel users that need
 * a digest (firmware image verification, WPA supplicant PBKDF2, ...).
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CRYPTO_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CRYPTO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* AES block size and the digest size of SHA-256, in bytes. */

#define RK3576_CRYPTO_AES_BLOCKLEN 16
#define RK3576_CRYPTO_SHA256_DIGESTLEN 32
#define RK3576_CRYPTO_SHA256_BLOCKLEN 64

/* Cipher chaining modes supported by rk3576_crypto_aes(). */

#define RK3576_CRYPTO_AES_ECB 0
#define RK3576_CRYPTO_AES_CBC 1
#define RK3576_CRYPTO_AES_CTR 2

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Streaming SHA-256 context.
 *
 * The hash engine keeps the intermediate state in hardware, so a context
 * owns the whole crypto block from rk3576_crypto_sha256_init() until
 * rk3576_crypto_sha256_final().  The block is a single shared resource:
 * the driver takes its lock in init() and releases it in final(), so a
 * caller must always pair the two and must not start a second digest, an
 * AES operation, or block indefinitely in between.
 */

struct rk3576_crypto_sha256_s
{
  uint8_t  block[RK3576_CRYPTO_SHA256_BLOCKLEN]; /* Partial input block   */
  size_t   nblock;   /* Valid bytes in block[]                            */
  bool     started;  /* At least one descriptor has been submitted        */
  bool     locked;   /* This context currently owns the hardware          */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_crypto_initialize
 *
 * Description:
 *   Ungate the crypto clocks, reset the block, allocate the DMA descriptor
 *   and bounce buffers and attach the completion interrupt.  Idempotent:
 *   repeated calls are harmless.  up_cryptoinitialize() calls this
 *   automatically; a board only needs to call it directly when
 *   CONFIG_CRYPTO is not selected but the chip level API below is still
 *   wanted.
 *
 *   Must be called after rk3576_clk_tree_initialize(), that is from
 *   board_late_initialize() or later, because it uses clk_get().
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_crypto_initialize(void);

/****************************************************************************
 * Name: rk3576_crypto_aes
 *
 * Description:
 *   Encrypt or decrypt a buffer with the hardware AES engine.
 *
 * Input Parameters:
 *   key     - AES key, 16, 24 or 32 bytes.
 *   keylen  - Key length in bytes.
 *   iv      - Initial vector, RK3576_CRYPTO_AES_BLOCKLEN bytes.  Ignored
 *             (may be NULL) for ECB.  On return it holds the chaining
 *             value left by the hardware, so consecutive calls can
 *             continue a single CBC/CTR stream.
 *   mode    - RK3576_CRYPTO_AES_ECB, _CBC or _CTR.
 *   encrypt - true to encrypt, false to decrypt.
 *   in      - Input buffer.
 *   out     - Output buffer; may alias in.
 *   len     - Number of bytes, must be a multiple of
 *             RK3576_CRYPTO_AES_BLOCKLEN.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_crypto_aes(const uint8_t *key, size_t keylen, uint8_t *iv,
                      int mode, bool encrypt, const uint8_t *in,
                      uint8_t *out, size_t len);

/****************************************************************************
 * Name: rk3576_crypto_sha256_init
 *
 * Description:
 *   Start a streaming SHA-256 digest.  Takes the hardware lock, which is
 *   held until rk3576_crypto_sha256_final() returns.
 *
 * Input Parameters:
 *   ctx - Caller supplied context, zeroed by this call.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_crypto_sha256_init(struct rk3576_crypto_sha256_s *ctx);

/****************************************************************************
 * Name: rk3576_crypto_sha256_update
 *
 * Description:
 *   Feed more data into a digest started by rk3576_crypto_sha256_init().
 *   Data is buffered internally so that every descriptor but the last is a
 *   whole number of 64 byte hash blocks, as the engine requires.
 *
 * Input Parameters:
 *   ctx - Context from rk3576_crypto_sha256_init().
 *   in  - Input buffer.
 *   len - Number of bytes; zero is a no-op.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.  On failure the
 *   context is released and must not be passed to final().
 *
 ****************************************************************************/

int rk3576_crypto_sha256_update(struct rk3576_crypto_sha256_s *ctx,
                                const uint8_t *in, size_t len);

/****************************************************************************
 * Name: rk3576_crypto_sha256_final
 *
 * Description:
 *   Flush the remaining bytes, let the engine pad the message and read the
 *   digest out.  Releases the hardware lock taken by init().
 *
 * Input Parameters:
 *   ctx - Context from rk3576_crypto_sha256_init().
 *   out - Receives RK3576_CRYPTO_SHA256_DIGESTLEN bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_crypto_sha256_final(struct rk3576_crypto_sha256_s *ctx,
                               uint8_t *out);

/****************************************************************************
 * Name: rk3576_crypto_sha256
 *
 * Description:
 *   One-shot SHA-256 of a single contiguous buffer.
 *
 * Input Parameters:
 *   in  - Input buffer.
 *   len - Number of bytes; zero produces the digest of the empty message.
 *   out - Receives RK3576_CRYPTO_SHA256_DIGESTLEN bytes.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_crypto_sha256(const uint8_t *in, size_t len, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CRYPTO_H */
