/****************************************************************************
 * chips/rk3576/hardware/rk3576_crypto.h
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
 * Register definitions for the RK3576 "rockchip,crypto-v4" hardware crypto
 * accelerator (vendor DTS node crypto@2a400000, 0x2000 bytes of registers,
 * GIC INTID 177 = SPI 145 + 32).
 *
 * The block contains a block-cipher engine (AES / DES / TDES / SM4), a hash
 * engine (MD5 / SHA1 / SHA2 / SM3, with an HMAC front end) and a public key
 * accelerator (RSA / ECC).  Data is moved in and out by an internal DMA
 * engine that walks a linked list of descriptors (LLIs) in memory; the CPU
 * only programs the key, the IV and the mode, then points the DMA at the
 * head of the list.
 *
 * Most control registers are HIWORD write-masked: bits [31:16] are a
 * per-bit write enable for bits [15:0].  Status registers are plain and are
 * write-one-to-clear.
 *
 * The RK3576 TRM does not document the security sub-system, so the bit
 * layout below follows the register naming published by Rockchip for the
 * same crypto IP generation.  It has not been verified on hardware yet.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRYPTO_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRYPTO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/compiler.h>

#include "rk3576_memorymap.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Controller base address.
 * TODO: move to rk3576_memorymap.h once the integration branch takes it.
 */


#define RK3576_CRYPTO_SIZE 0x2000

/* HIWORD write-enable mask covering all 16 writable bits. */

#define RK3576_CRYPTO_WRITE_MASK 0xffff0000u

/* Register offsets *********************************************************/

#define RK3576_CRYPTO_CLK_CTL       0x0000 /* Clock control (hiword)       */
#define RK3576_CRYPTO_RST_CTL       0x0004 /* Soft reset (hiword)          */
#define RK3576_CRYPTO_DMA_INT_EN    0x0008 /* DMA interrupt enable         */
#define RK3576_CRYPTO_DMA_INT_ST    0x000c /* DMA interrupt status (W1C)   */
#define RK3576_CRYPTO_DMA_CTL       0x0010 /* DMA control (hiword)         */
#define RK3576_CRYPTO_DMA_LLI_ADDR  0x0014 /* Physical address of LLI head */
#define RK3576_CRYPTO_DMA_ST        0x0018 /* DMA busy status              */
#define RK3576_CRYPTO_DMA_STATE     0x001c /* DMA state machine            */
#define RK3576_CRYPTO_DMA_LLI_RADDR 0x0020 /* LLI address being fetched    */
#define RK3576_CRYPTO_DMA_SRC_RADDR 0x0024 /* Source address in flight     */
#define RK3576_CRYPTO_DMA_DST_RADDR 0x0028 /* Destination address in fligh */
#define RK3576_CRYPTO_DMA_ITEM_ID   0x002c /* LLI index being processed    */
#define RK3576_CRYPTO_FIFO_CTL      0x0040 /* FIFO byte-swap control       */
#define RK3576_CRYPTO_BC_CTL        0x0044 /* Block cipher control (hiword)*/
#define RK3576_CRYPTO_HASH_CTL      0x0048 /* Hash control (hiword)        */
#define RK3576_CRYPTO_HASH_VALID    0x0054 /* Hash result valid (W1C)      */

#define RK3576_CRYPTO_CIPHER_KEY0   0x0080 /* Block cipher key, word 0     */
#define RK3576_CRYPTO_CIPHER_KEY(n) (RK3576_CRYPTO_CIPHER_KEY0 + ((n) << 2))

#define RK3576_CRYPTO_CIPHER_IV0    0x0100 /* Channel 0 IV / counter, w0   */
#define RK3576_CRYPTO_CIPHER_IV(n)  (RK3576_CRYPTO_CIPHER_IV0 + ((n) << 2))

#define RK3576_CRYPTO_HASH_DOUT0    0x0200 /* Hash digest out, word 0      */
#define RK3576_CRYPTO_HASH_DOUT(n)  (RK3576_CRYPTO_HASH_DOUT0 + ((n) << 2))

/* Number of key / IV / digest words the block exposes. */

#define RK3576_CRYPTO_KEY_NWORDS   8  /* 256-bit maximum cipher key       */
#define RK3576_CRYPTO_IV_NWORDS    4  /* 128-bit IV / counter             */
#define RK3576_CRYPTO_DOUT_NWORDS 16  /* 512-bit maximum digest           */

/* CLK_CTL (0x0000), HIWORD write-masked ************************************/

#define RK3576_CRYPTO_AUTO_CLKGATE_EN (1 << 0) /* Gate idle sub-blocks     */

/* RST_CTL (0x0004), HIWORD write-masked ************************************/

#define RK3576_CRYPTO_SW_CC_RESET  (1 << 0) /* Reset cipher/hash core      */
#define RK3576_CRYPTO_SW_PKA_RESET (1 << 1) /* Reset public key core       */

/* DMA_INT_EN (0x0008) / DMA_INT_ST (0x000c), plain, status is W1C *********/

#define RK3576_CRYPTO_INT_LIST_DONE (1 << 0) /* Whole LLI list finished    */
#define RK3576_CRYPTO_INT_DST_DONE  (1 << 1) /* One dst item finished      */
#define RK3576_CRYPTO_INT_SRC_DONE  (1 << 2) /* One src item finished      */
#define RK3576_CRYPTO_INT_LIST_ERR  (1 << 3) /* LLI fetch error            */
#define RK3576_CRYPTO_INT_DST_ERR   (1 << 4) /* Destination write error    */
#define RK3576_CRYPTO_INT_SRC_ERR   (1 << 5) /* Source read error          */

#define RK3576_CRYPTO_INT_ERR_MASK                                        \
  (RK3576_CRYPTO_INT_LIST_ERR | RK3576_CRYPTO_INT_DST_ERR |               \
   RK3576_CRYPTO_INT_SRC_ERR)

#define RK3576_CRYPTO_INT_ALL                                             \
  (RK3576_CRYPTO_INT_LIST_DONE | RK3576_CRYPTO_INT_DST_DONE |             \
   RK3576_CRYPTO_INT_SRC_DONE | RK3576_CRYPTO_INT_ERR_MASK)

/* DMA_CTL (0x0010), HIWORD write-masked ************************************/

#define RK3576_CRYPTO_DMA_START   (1 << 0) /* Start walking the LLI list   */
#define RK3576_CRYPTO_DMA_RESTART (1 << 1) /* Resume after a pause         */

/* DMA_ST (0x0018) **********************************************************/

#define RK3576_CRYPTO_DMA_BUSY (1 << 0) /* DMA engine is running           */

/* FIFO_CTL (0x0040), HIWORD write-masked ***********************************/

#define RK3576_CRYPTO_DOUT_BYTESWAP (1 << 0) /* Byte-swap data out         */
#define RK3576_CRYPTO_DIN_BYTESWAP  (1 << 1) /* Byte-swap data in          */

/* BC_CTL (0x0044), HIWORD write-masked *************************************/

#define RK3576_CRYPTO_BC_ENABLE  (1 << 0) /* Enable the block cipher       */
#define RK3576_CRYPTO_BC_DECRYPT (1 << 1) /* 0 = encrypt, 1 = decrypt      */

#define RK3576_CRYPTO_BC_KEYSIZE_SHIFT 2
#define RK3576_CRYPTO_BC_KEYSIZE_MASK  (3 << RK3576_CRYPTO_BC_KEYSIZE_SHIFT)
#define RK3576_CRYPTO_BC_KEY_128BIT    (0 << RK3576_CRYPTO_BC_KEYSIZE_SHIFT)
#define RK3576_CRYPTO_BC_KEY_192BIT    (1 << RK3576_CRYPTO_BC_KEYSIZE_SHIFT)
#define RK3576_CRYPTO_BC_KEY_256BIT    (2 << RK3576_CRYPTO_BC_KEYSIZE_SHIFT)

#define RK3576_CRYPTO_BC_MODE_SHIFT 4
#define RK3576_CRYPTO_BC_MODE_MASK  (15 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_ECB   (0 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_CBC   (1 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_CTS   (2 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_CTR   (3 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_CFB   (4 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_OFB   (5 << RK3576_CRYPTO_BC_MODE_SHIFT)
#define RK3576_CRYPTO_BC_MODE_XTS   (6 << RK3576_CRYPTO_BC_MODE_SHIFT)

#define RK3576_CRYPTO_BC_ALG_SHIFT 8
#define RK3576_CRYPTO_BC_ALG_MASK  (3 << RK3576_CRYPTO_BC_ALG_SHIFT)
#define RK3576_CRYPTO_BC_ALG_AES   (0 << RK3576_CRYPTO_BC_ALG_SHIFT)
#define RK3576_CRYPTO_BC_ALG_SM4   (1 << RK3576_CRYPTO_BC_ALG_SHIFT)
#define RK3576_CRYPTO_BC_ALG_DES   (2 << RK3576_CRYPTO_BC_ALG_SHIFT)
#define RK3576_CRYPTO_BC_ALG_TDES  (3 << RK3576_CRYPTO_BC_ALG_SHIFT)

/* HASH_CTL (0x0048), HIWORD write-masked ***********************************/

#define RK3576_CRYPTO_HASH_ENABLE (1 << 0) /* Enable the hash engine       */
#define RK3576_CRYPTO_HMAC_ENABLE (1 << 1) /* Wrap the hash in an HMAC     */

#define RK3576_CRYPTO_HASH_ALG_SHIFT   4
#define RK3576_CRYPTO_HASH_ALG_MASK    (15 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SHA1    (0 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_MD5     (1 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SHA256  (2 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SHA224  (3 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SM3     (6 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SHA512  (8 << RK3576_CRYPTO_HASH_ALG_SHIFT)
#define RK3576_CRYPTO_HASH_ALG_SHA384  (9 << RK3576_CRYPTO_HASH_ALG_SHIFT)

/* HASH_VALID (0x0054), write one to clear **********************************/

#define RK3576_CRYPTO_HASH_IS_VALID (1 << 0) /* Digest ready in HASH_DOUT  */

/* DMA linked list item *****************************************************/

/* The DMA engine fetches 32 bytes per descriptor.  All addresses are
 * physical and must be 32-bit (the engine has no address extension) and
 * word aligned; the descriptor itself must be word aligned as well.
 */

begin_packed_struct struct rk3576_crypto_lli_s
{
  uint32_t src_addr;    /* Physical source address                        */
  uint32_t src_len;     /* Source length in bytes                         */
  uint32_t dst_addr;    /* Physical destination address, 0 if unused      */
  uint32_t dst_len;     /* Destination length in bytes, 0 if unused       */
  uint32_t user_define; /* Engine routing flags, see below                */
  uint32_t reserve;     /* Must be written as zero                        */
  uint32_t dma_ctrl;    /* Per item interrupt / list control, see below   */
  uint32_t next_addr;   /* Physical address of the next LLI, 0 to stop    */
} end_packed_struct;

/* LLI user_define flags */

#define RK3576_CRYPTO_LLI_CIPHER_EN   (1 << 7)  /* Route through BC engine */
#define RK3576_CRYPTO_LLI_STR_START   (1 << 8)  /* First item of a message */
#define RK3576_CRYPTO_LLI_STR_LAST    (1 << 9)  /* Last item of a message  */
#define RK3576_CRYPTO_LLI_ADDR_SAME   (1 << 10) /* dst == src, in place    */
#define RK3576_CRYPTO_LLI_PRIVACY_KEY (1 << 11) /* Use the OTP privacy key */
#define RK3576_CRYPTO_LLI_ROOT_KEY    (1 << 12) /* Use the OTP root key    */

/* LLI dma_ctrl flags */

#define RK3576_CRYPTO_LLI_LIST_DONE (1 << 0) /* Raise list-done here      */
#define RK3576_CRYPTO_LLI_PAUSE     (1 << 1) /* Pause after this item     */
#define RK3576_CRYPTO_LLI_SRC_DONE  (1 << 2) /* Raise src-done here       */
#define RK3576_CRYPTO_LLI_DST_DONE  (1 << 3) /* Raise dst-done here       */

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_CRYPTO_H */
