/****************************************************************************
 * chips/rk3576/rk3576_crypto.c
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
 * RK3576 hardware crypto accelerator ("rockchip,crypto-v4") driver.
 *
 * The block moves data with an internal DMA engine that walks a linked list
 * of descriptors.  A single descriptor is enough for every operation this
 * driver performs, because the payload is copied through a pair of bounce
 * buffers taken from the DMA-safe heap: caller buffers may live anywhere in
 * the 4GB address space, may be unaligned and may be uncacheable, while the
 * engine needs 32-bit, word aligned, physically contiguous memory.
 *
 * Two engines are wired up:
 *
 *   AES  - ECB / CBC / CTR with 128, 192 and 256 bit keys, exported both as
 *          rk3576_crypto_aes() and as the NuttX arch hook aes_cypher().
 *   SHA  - SHA-256, exported as a streaming init/update/final triplet and a
 *          one-shot helper.  The hash engine keeps its intermediate state
 *          in hardware, so a streaming digest owns the block for its whole
 *          lifetime.
 *
 * DES / TDES / SM4 / HMAC / SM3 are register-compatible with the AES and
 * SHA paths below and only need extra mode constants.
 * TODO: the RSA/ECC public key accelerator (PKA) is a separate register
 * bank that is not described here and is not implemented.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/crypto/crypto.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_crypto.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_addrenv.h"
#include "rk3576_crypto.h"
#include "rk3576_dma_alloc.h"

#ifdef CONFIG_RK3576_CRYPTO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Size of each bounce buffer.  A multiple of both the AES block size and
 * the SHA-256 block size, so a chunk boundary never splits a block.
 */

#define RK3576_CRYPTO_BOUNCE_SIZE 4096

/* Busy-wait budget for the soft reset handshake, in loop iterations. */

#define RK3576_CRYPTO_RESET_LIMIT 10000

/* Upper bound on one DMA descriptor, in milliseconds.  A 4KB chunk needs a
 * few microseconds; this only bounds a hardware failure.
 */

#define RK3576_CRYPTO_DMA_TIMEOUT_MS 1000

/* Enable both byte swappers: the engine is big-endian internally while the
 * key, IV and digest registers are written and read as little-endian words.
 */

#define RK3576_CRYPTO_FIFO_CONFIG \
  (RK3576_CRYPTO_DIN_BYTESWAP | RK3576_CRYPTO_DOUT_BYTESWAP)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_crypto_dev_s
{
  mutex_t lock;    /* Serialises access to the single hardware instance   */
  sem_t done;      /* Posted by the ISR when the LLI list completes       */
  bool ready;      /* Set once the block has been brought up              */
  uint32_t status; /* DMA_INT_ST latched by the ISR                       */

  struct rk3576_crypto_lli_s *lli; /* DMA descriptor, DMA-safe memory      */
  uint8_t *src;                    /* Input bounce buffer                  */
  uint8_t *dst;                    /* Output bounce buffer                 */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_crypto_getreg(unsigned int off);
static inline void rk3576_crypto_putreg(unsigned int off, uint32_t val);
static inline void rk3576_crypto_putmasked(unsigned int off, uint32_t val);
static int rk3576_crypto_clk_init(void);
static int rk3576_crypto_reset(void);
static int rk3576_crypto_interrupt(int irq, void *context, void *arg);
static int rk3576_crypto_run(size_t srclen, size_t dstlen,
                             uint32_t user_define);
static void rk3576_crypto_write_regs(unsigned int off, const uint8_t *data,
                                     size_t len);
static void rk3576_crypto_read_regs(unsigned int off, uint8_t *data,
                                    size_t len);
static int rk3576_crypto_aes_setup(const uint8_t *key, size_t keylen,
                                   const uint8_t *iv, int mode, bool encrypt);
static int rk3576_crypto_hash_submit(struct rk3576_crypto_sha256_s *ctx,
                                     const uint8_t *in, size_t len, bool last);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_crypto_dev_s g_rk3576_crypto = {
  .lock = NXMUTEX_INITIALIZER,
  .done = SEM_INITIALIZER(0),
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_crypto_getreg(unsigned int off)
{
  return getreg32(RK3576_CRYPTO_ADDR + off);
}

static inline void rk3576_crypto_putreg(unsigned int off, uint32_t val)
{
  putreg32(val, RK3576_CRYPTO_ADDR + off);
}

/****************************************************************************
 * Name: rk3576_crypto_putmasked
 *
 * Description:
 *   Write a HIWORD write-masked register: the value goes to bits [15:0] and
 *   every writable bit is enabled in [31:16], so the register ends up
 *   holding exactly val.
 *
 ****************************************************************************/

static inline void rk3576_crypto_putmasked(unsigned int off, uint32_t val)
{
  putreg32(RK3576_CRYPTO_WRITE_MASK | (val & 0xffff),
           RK3576_CRYPTO_ADDR + off);
}

/****************************************************************************
 * Name: rk3576_crypto_clk_init
 *
 * Description:
 *   Ungate every clock the crypto block needs.  All clock handling in this
 *   driver lives here so that a change in the CLK framework only touches
 *   one function.
 *
 *   The vendor DTS names three clocks for crypto@2a400000: "aclk" (AXI data
 *   path, 300MHz), "hclk" (AHB register interface) and "pka" (public key
 *   accelerator core clock, 300MHz).
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_crypto_clk_init(void)
{
  static const char *const names[] = {
    "aclk_crypto_ns_en",
    "hclk_crypto_ns_en",
    "clk_pka_crypto_ns_en",
  };

  struct clk_s *clk;
  unsigned int i;
  int ret;

  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
      clk = clk_get(names[i]);
      if (clk == NULL)
        {
          _err("ERROR: failed to get %s\n", names[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          _err("ERROR: failed to enable %s: %d\n", names[i], ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_crypto_reset
 *
 * Description:
 *   Pulse the software reset of the cipher/hash core and of the public key
 *   core.  The reset bits are self clearing; the block is ready again once
 *   they read back as zero.
 *
 * Returned Value:
 *   OK on success, -ETIMEDOUT if the reset never completed.
 *
 ****************************************************************************/

static int rk3576_crypto_reset(void)
{
  unsigned int i;

  rk3576_crypto_putmasked(RK3576_CRYPTO_RST_CTL,
                          RK3576_CRYPTO_SW_CC_RESET |
                              RK3576_CRYPTO_SW_PKA_RESET);

  for (i = 0; i < RK3576_CRYPTO_RESET_LIMIT; i++)
    {
      if (rk3576_crypto_getreg(RK3576_CRYPTO_RST_CTL) == 0)
        {
          return OK;
        }
    }

  _err("ERROR: crypto reset timed out, rst_ctl=0x%08" PRIx32 "\n",
       rk3576_crypto_getreg(RK3576_CRYPTO_RST_CTL));
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_crypto_interrupt
 *
 * Description:
 *   DMA completion handler.  Latches and acknowledges the status, masks
 *   further interrupts and wakes the submitting thread.
 *
 ****************************************************************************/

static int rk3576_crypto_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_crypto_dev_s *priv = arg;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_crypto_getreg(RK3576_CRYPTO_DMA_INT_ST);
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_ST, status);
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_EN, 0);

  priv->status = status;
  nxsem_post(&priv->done);
  return OK;
}

/****************************************************************************
 * Name: rk3576_crypto_run
 *
 * Description:
 *   Submit one descriptor covering the bounce buffers and wait for the DMA
 *   engine to finish it.  The caller must have filled priv->src with srclen
 *   bytes and must hold the hardware lock.
 *
 * Input Parameters:
 *   srclen      - Bytes to feed the engine from the input bounce buffer.
 *   dstlen      - Bytes the engine writes to the output bounce buffer, zero
 *                 for a hash where the data is consumed only.
 *   user_define - Engine routing flags for the descriptor.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_crypto_run(size_t srclen, size_t dstlen,
                             uint32_t user_define)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  struct rk3576_crypto_lli_s *lli = priv->lli;
  int ret;

  lli->src_addr = (uint32_t)up_addrenv_va_to_pa(priv->src);
  lli->src_len = (uint32_t)srclen;
  lli->dst_addr = dstlen ? (uint32_t)up_addrenv_va_to_pa(priv->dst) : 0;
  lli->dst_len = (uint32_t)dstlen;
  lli->user_define = user_define;
  lli->reserve = 0;
  lli->dma_ctrl = RK3576_CRYPTO_LLI_LIST_DONE;
  lli->next_addr = 0;

  /* Push the payload and the descriptor out of the D-cache before handing
   * them to the engine.
   */

  if (srclen > 0)
    {
      up_clean_dcache((uintptr_t)priv->src, (uintptr_t)priv->src + srclen);
    }

  up_clean_dcache((uintptr_t)lli, (uintptr_t)lli + sizeof(*lli));

  /* Arm the completion interrupt.  Only the terminal conditions are
   * enabled; per item interrupts would fire once per descriptor and this
   * driver submits one descriptor at a time.
   */

  priv->status = 0;
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_ST, RK3576_CRYPTO_INT_ALL);
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_EN,
                       RK3576_CRYPTO_INT_LIST_DONE |
                           RK3576_CRYPTO_INT_ERR_MASK);

  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_LLI_ADDR,
                       (uint32_t)up_addrenv_va_to_pa(lli));
  rk3576_crypto_putmasked(RK3576_CRYPTO_DMA_CTL, RK3576_CRYPTO_DMA_START);

  ret = nxsem_tickwait_uninterruptible(
      &priv->done, MSEC2TICK(RK3576_CRYPTO_DMA_TIMEOUT_MS));
  if (ret < 0)
    {
      rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_EN, 0);
      _err("ERROR: DMA timed out, st=0x%08" PRIx32 " state=0x%08" PRIx32 "\n",
           rk3576_crypto_getreg(RK3576_CRYPTO_DMA_ST),
           rk3576_crypto_getreg(RK3576_CRYPTO_DMA_STATE));
      return -ETIMEDOUT;
    }

  if ((priv->status & RK3576_CRYPTO_INT_ERR_MASK) != 0)
    {
      _err("ERROR: DMA fault, int_st=0x%08" PRIx32 "\n", priv->status);
      return -EIO;
    }

  /* Drop any stale cache lines covering the result before the CPU reads
   * what the engine wrote.
   */

  if (dstlen > 0)
    {
      up_invalidate_dcache((uintptr_t)priv->dst,
                           (uintptr_t)priv->dst + dstlen);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_crypto_write_regs
 *
 * Description:
 *   Copy a byte string into a bank of consecutive 32-bit registers.  The
 *   bytes are packed little-endian, which together with the FIFO byte
 *   swappers gives the natural big-endian order the algorithms define.
 *
 * Input Parameters:
 *   off  - Offset of the first register.
 *   data - Source bytes, need not be aligned.
 *   len  - Number of bytes, rounded up to a whole register.
 *
 ****************************************************************************/

static void rk3576_crypto_write_regs(unsigned int off, const uint8_t *data,
                                     size_t len)
{
  uint32_t word;
  size_t i;

  for (i = 0; i < len; i += 4)
    {
      size_t chunk = len - i < 4 ? len - i : 4;

      word = 0;
      memcpy(&word, data + i, chunk);
      rk3576_crypto_putreg(off + i, word);
    }
}

/****************************************************************************
 * Name: rk3576_crypto_read_regs
 *
 * Description:
 *   Inverse of rk3576_crypto_write_regs().
 *
 ****************************************************************************/

static void rk3576_crypto_read_regs(unsigned int off, uint8_t *data,
                                    size_t len)
{
  uint32_t word;
  size_t i;

  for (i = 0; i < len; i += 4)
    {
      size_t chunk = len - i < 4 ? len - i : 4;

      word = rk3576_crypto_getreg(off + i);
      memcpy(data + i, &word, chunk);
    }
}

/****************************************************************************
 * Name: rk3576_crypto_aes_setup
 *
 * Description:
 *   Program key, IV and mode into the block cipher engine and enable it.
 *   The engine must be disabled while the key registers are written, so
 *   BC_CTL is cleared first.
 *
 * Returned Value:
 *   OK on success, -EINVAL for an unsupported key length or mode.
 *
 ****************************************************************************/

static int rk3576_crypto_aes_setup(const uint8_t *key, size_t keylen,
                                   const uint8_t *iv, int mode, bool encrypt)
{
  uint32_t ctl = RK3576_CRYPTO_BC_ALG_AES | RK3576_CRYPTO_BC_ENABLE;

  switch (keylen)
    {
      case 16:
        ctl |= RK3576_CRYPTO_BC_KEY_128BIT;
        break;

      case 24:
        ctl |= RK3576_CRYPTO_BC_KEY_192BIT;
        break;

      case 32:
        ctl |= RK3576_CRYPTO_BC_KEY_256BIT;
        break;

      default:
        _err("ERROR: unsupported AES key length %zu\n", keylen);
        return -EINVAL;
    }

  switch (mode)
    {
      case RK3576_CRYPTO_AES_ECB:
        ctl |= RK3576_CRYPTO_BC_MODE_ECB;
        break;

      case RK3576_CRYPTO_AES_CBC:
        ctl |= RK3576_CRYPTO_BC_MODE_CBC;
        break;

      case RK3576_CRYPTO_AES_CTR:
        ctl |= RK3576_CRYPTO_BC_MODE_CTR;
        break;

      default:
        _err("ERROR: unsupported AES mode %d\n", mode);
        return -EINVAL;
    }

  /* CTR turns the block cipher into a key stream generator, so the engine
   * always runs it in the encrypt direction.
   */

  if (!encrypt && mode != RK3576_CRYPTO_AES_CTR)
    {
      ctl |= RK3576_CRYPTO_BC_DECRYPT;
    }

  rk3576_crypto_putmasked(RK3576_CRYPTO_BC_CTL, 0);
  rk3576_crypto_putmasked(RK3576_CRYPTO_HASH_CTL, 0);
  rk3576_crypto_putmasked(RK3576_CRYPTO_FIFO_CTL, RK3576_CRYPTO_FIFO_CONFIG);

  rk3576_crypto_write_regs(RK3576_CRYPTO_CIPHER_KEY0, key, keylen);

  if (mode != RK3576_CRYPTO_AES_ECB && iv != NULL)
    {
      rk3576_crypto_write_regs(RK3576_CRYPTO_CIPHER_IV0, iv,
                               RK3576_CRYPTO_AES_BLOCKLEN);
    }

  rk3576_crypto_putmasked(RK3576_CRYPTO_BC_CTL, ctl);
  return OK;
}

/****************************************************************************
 * Name: rk3576_crypto_hash_submit
 *
 * Description:
 *   Feed one chunk of an in-flight digest to the engine.  len must be a
 *   multiple of RK3576_CRYPTO_SHA256_BLOCKLEN unless last is true, and must
 *   not exceed RK3576_CRYPTO_BOUNCE_SIZE.  The engine performs the message
 *   padding itself when the descriptor is marked as the last one of the
 *   string.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_crypto_hash_submit(struct rk3576_crypto_sha256_s *ctx,
                                     const uint8_t *in, size_t len, bool last)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  uint32_t user_define = 0;

  if (!ctx->started)
    {
      user_define |= RK3576_CRYPTO_LLI_STR_START;
    }

  if (last)
    {
      user_define |= RK3576_CRYPTO_LLI_STR_LAST;
    }

  if (len > 0)
    {
      memcpy(priv->src, in, len);
    }

  ctx->started = true;
  return rk3576_crypto_run(len, 0, user_define);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_crypto_initialize
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_initialize(void)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  int ret;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->ready)
    {
      nxmutex_unlock(&priv->lock);
      return OK;
    }

  ret = rk3576_crypto_clk_init();
  if (ret < 0)
    {
      goto errout;
    }

  /* The descriptor and both bounce buffers must be physically contiguous
   * and addressable by the engine's 32-bit DMA.
   */

  priv->lli = rk3576_dma_alloc(sizeof(struct rk3576_crypto_lli_s));
  priv->src = rk3576_dma_alloc(RK3576_CRYPTO_BOUNCE_SIZE);
  priv->dst = rk3576_dma_alloc(RK3576_CRYPTO_BOUNCE_SIZE);

  if (priv->lli == NULL || priv->src == NULL || priv->dst == NULL)
    {
      _err("ERROR: out of DMA memory\n");
      ret = -ENOMEM;
      goto errout_with_mem;
    }

  ret = rk3576_crypto_reset();
  if (ret < 0)
    {
      goto errout_with_mem;
    }

  /* Let the hardware gate the sub-blocks it is not using, keep the data
   * path byte swappers on and leave both engines disabled.
   */

  rk3576_crypto_putmasked(RK3576_CRYPTO_CLK_CTL,
                          RK3576_CRYPTO_AUTO_CLKGATE_EN);
  rk3576_crypto_putmasked(RK3576_CRYPTO_FIFO_CTL, RK3576_CRYPTO_FIFO_CONFIG);
  rk3576_crypto_putmasked(RK3576_CRYPTO_BC_CTL, 0);
  rk3576_crypto_putmasked(RK3576_CRYPTO_HASH_CTL, 0);
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_EN, 0);
  rk3576_crypto_putreg(RK3576_CRYPTO_DMA_INT_ST, RK3576_CRYPTO_INT_ALL);

  ret = irq_attach(RK3576_IRQ_NSCRYPTO, rk3576_crypto_interrupt, priv);
  if (ret < 0)
    {
      _err("ERROR: failed to attach IRQ %d: %d\n", RK3576_IRQ_NSCRYPTO, ret);
      goto errout_with_mem;
    }

  up_enable_irq(RK3576_IRQ_NSCRYPTO);

  priv->ready = true;
  nxmutex_unlock(&priv->lock);
  _info("crypto engine ready\n");
  return OK;

errout_with_mem:
  if (priv->lli != NULL)
    {
      rk3576_dma_free(priv->lli, sizeof(struct rk3576_crypto_lli_s));
      priv->lli = NULL;
    }

  if (priv->src != NULL)
    {
      rk3576_dma_free(priv->src, RK3576_CRYPTO_BOUNCE_SIZE);
      priv->src = NULL;
    }

  if (priv->dst != NULL)
    {
      rk3576_dma_free(priv->dst, RK3576_CRYPTO_BOUNCE_SIZE);
      priv->dst = NULL;
    }

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_crypto_aes
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_aes(const uint8_t *key, size_t keylen, uint8_t *iv, int mode,
                      bool encrypt, const uint8_t *in, uint8_t *out,
                      size_t len)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  int ret;

  if (key == NULL || in == NULL || out == NULL)
    {
      return -EINVAL;
    }

  if ((len % RK3576_CRYPTO_AES_BLOCKLEN) != 0)
    {
      _err("ERROR: length %zu is not a whole number of AES blocks\n", len);
      return -EINVAL;
    }

  if (len == 0)
    {
      return OK;
    }

  ret = rk3576_crypto_initialize();
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_crypto_aes_setup(key, keylen, iv, mode, encrypt);
  if (ret < 0)
    {
      goto errout;
    }

  while (len > 0)
    {
      size_t chunk =
          len > RK3576_CRYPTO_BOUNCE_SIZE ? RK3576_CRYPTO_BOUNCE_SIZE : len;

      memcpy(priv->src, in, chunk);

      /* The engine keeps the chaining value in its IV registers between
       * descriptors, so every chunk but the first continues the stream.
       */

      ret = rk3576_crypto_run(chunk, chunk,
                              RK3576_CRYPTO_LLI_CIPHER_EN |
                                  RK3576_CRYPTO_LLI_STR_START |
                                  RK3576_CRYPTO_LLI_STR_LAST);
      if (ret < 0)
        {
          goto errout;
        }

      memcpy(out, priv->dst, chunk);

      in += chunk;
      out += chunk;
      len -= chunk;
    }

  /* Hand the chaining value back so the caller can continue the stream in
   * a later call.
   */

  if (iv != NULL && mode != RK3576_CRYPTO_AES_ECB)
    {
      rk3576_crypto_read_regs(RK3576_CRYPTO_CIPHER_IV0, iv,
                              RK3576_CRYPTO_AES_BLOCKLEN);
    }

errout:
  rk3576_crypto_putmasked(RK3576_CRYPTO_BC_CTL, 0);
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_crypto_sha256_init
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_sha256_init(struct rk3576_crypto_sha256_s *ctx)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  int ret;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  ret = rk3576_crypto_initialize();
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  memset(ctx, 0, sizeof(*ctx));
  ctx->locked = true;

  rk3576_crypto_putmasked(RK3576_CRYPTO_BC_CTL, 0);
  rk3576_crypto_putmasked(RK3576_CRYPTO_FIFO_CTL, RK3576_CRYPTO_FIFO_CONFIG);
  rk3576_crypto_putreg(RK3576_CRYPTO_HASH_VALID, RK3576_CRYPTO_HASH_IS_VALID);
  rk3576_crypto_putmasked(RK3576_CRYPTO_HASH_CTL,
                          RK3576_CRYPTO_HASH_ALG_SHA256 |
                              RK3576_CRYPTO_HASH_ENABLE);
  return OK;
}

/****************************************************************************
 * Name: rk3576_crypto_sha256_update
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_sha256_update(struct rk3576_crypto_sha256_s *ctx,
                                const uint8_t *in, size_t len)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  int ret = OK;

  if (ctx == NULL || !ctx->locked || (in == NULL && len > 0))
    {
      return -EINVAL;
    }

  /* Top up the partial block first; it is only submitted once it is full,
   * because a non-final descriptor must carry whole hash blocks.
   */

  if (ctx->nblock > 0)
    {
      size_t room = RK3576_CRYPTO_SHA256_BLOCKLEN - ctx->nblock;
      size_t fill = len < room ? len : room;

      memcpy(ctx->block + ctx->nblock, in, fill);
      ctx->nblock += fill;
      in += fill;
      len -= fill;

      if (ctx->nblock < RK3576_CRYPTO_SHA256_BLOCKLEN || len == 0)
        {
          /* Either still partial, or exactly full but possibly final: keep
           * it buffered so final() can pad it.
           */

          return OK;
        }

      ret = rk3576_crypto_hash_submit(ctx, ctx->block, ctx->nblock, false);
      if (ret < 0)
        {
          goto errout;
        }

      ctx->nblock = 0;
    }

  /* Submit as many whole-block chunks as possible, always keeping at least
   * one byte back so that final() has something to mark as the last
   * descriptor.
   */

  while (len > RK3576_CRYPTO_SHA256_BLOCKLEN)
    {
      size_t chunk = len > RK3576_CRYPTO_BOUNCE_SIZE
                         ? RK3576_CRYPTO_BOUNCE_SIZE
                         : len - 1;

      chunk -= chunk % RK3576_CRYPTO_SHA256_BLOCKLEN;
      if (chunk == 0)
        {
          break;
        }

      ret = rk3576_crypto_hash_submit(ctx, in, chunk, false);
      if (ret < 0)
        {
          goto errout;
        }

      in += chunk;
      len -= chunk;
    }

  if (len > 0)
    {
      memcpy(ctx->block, in, len);
      ctx->nblock = len;
    }

  return OK;

errout:
  rk3576_crypto_putmasked(RK3576_CRYPTO_HASH_CTL, 0);
  ctx->locked = false;
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_crypto_sha256_final
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_sha256_final(struct rk3576_crypto_sha256_s *ctx,
                               uint8_t *out)
{
  struct rk3576_crypto_dev_s *priv = &g_rk3576_crypto;
  int ret;

  if (ctx == NULL || out == NULL || !ctx->locked)
    {
      return -EINVAL;
    }

  ret = rk3576_crypto_hash_submit(ctx, ctx->block, ctx->nblock, true);
  if (ret == OK)
    {
      rk3576_crypto_read_regs(RK3576_CRYPTO_HASH_DOUT0, out,
                              RK3576_CRYPTO_SHA256_DIGESTLEN);
      rk3576_crypto_putreg(RK3576_CRYPTO_HASH_VALID,
                           RK3576_CRYPTO_HASH_IS_VALID);
    }

  rk3576_crypto_putmasked(RK3576_CRYPTO_HASH_CTL, 0);

  /* Do not leave the tail of the message on the caller's stack. */

  memset(ctx, 0, sizeof(*ctx));
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_crypto_sha256
 *
 * Description:
 *   See rk3576_crypto.h.
 *
 ****************************************************************************/

int rk3576_crypto_sha256(const uint8_t *in, size_t len, uint8_t *out)
{
  struct rk3576_crypto_sha256_s ctx;
  int ret;

  if (out == NULL || (in == NULL && len > 0))
    {
      return -EINVAL;
    }

  ret = rk3576_crypto_sha256_init(&ctx);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_crypto_sha256_update(&ctx, in, len);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_crypto_sha256_final(&ctx, out);
}

/****************************************************************************
 * Name: up_cryptoinitialize
 *
 * Description:
 *   Arch hook called by the NuttX crypto framework (see
 *   include/nuttx/crypto/crypto.h) to bring up the hardware accelerator
 *   that backs aes_cypher().
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int up_cryptoinitialize(void) { return rk3576_crypto_initialize(); }

#if defined(CONFIG_CRYPTO_AES)

/****************************************************************************
 * Name: aes_cypher
 *
 * Description:
 *   NuttX arch AES hook.  Translates the framework's mode constants to the
 *   chip level API.  CFB and the CMAC variant (AES_MODE_MAC) are not
 *   implemented; the software implementation in crypto/ handles them.
 *
 * Input Parameters:
 *   out     - Output buffer, size bytes.
 *   in      - Input buffer, size bytes.  May alias out.
 *   size    - Payload length, a multiple of the AES block size.
 *   iv      - Initial vector, 16 bytes, unused for ECB.
 *   key     - AES key.
 *   keysize - Key length in bytes: 16, 24 or 32.
 *   mode    - AES_MODE_ECB, AES_MODE_CBC or AES_MODE_CTR.
 *   encrypt - CYPHER_ENCRYPT or CYPHER_DECRYPT.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int aes_cypher(void *out, const void *in, size_t size, const void *iv,
               const void *key, size_t keysize, int mode, int encrypt)
{
  uint8_t ivbuf[RK3576_CRYPTO_AES_BLOCKLEN];
  int hwmode;

  switch (mode & AES_MODE_MASK)
    {
      case AES_MODE_ECB:
        hwmode = RK3576_CRYPTO_AES_ECB;
        break;

      case AES_MODE_CBC:
        hwmode = RK3576_CRYPTO_AES_CBC;
        break;

      case AES_MODE_CTR:
        hwmode = RK3576_CRYPTO_AES_CTR;
        break;

      default:
        return -EINVAL;
    }

  if ((mode & AES_MODE_MAC) != 0)
    {
      return -ENOSYS;
    }

  /* The framework passes a const IV, so work on a copy: the hardware hands
   * the updated chaining value back through this buffer.
   */

  if (hwmode != RK3576_CRYPTO_AES_ECB)
    {
      if (iv == NULL)
        {
          return -EINVAL;
        }

      memcpy(ivbuf, iv, sizeof(ivbuf));
    }
  else
    {
      memset(ivbuf, 0, sizeof(ivbuf));
    }

  return rk3576_crypto_aes(key, keysize, ivbuf, hwmode,
                           encrypt == CYPHER_ENCRYPT, in, out, size);
}
#endif /* CONFIG_CRYPTO_AES */

#endif /* CONFIG_RK3576_CRYPTO */
