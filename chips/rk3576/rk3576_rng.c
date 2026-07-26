/****************************************************************************
 * chips/rk3576/rk3576_rng.c
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
 * RK3576 hardware true random number generator (RKRNG) driver.
 *
 * The block produces 256 bits per conversion.  A conversion takes only a
 * few microseconds, far less than the cost of an interrupt round trip, so
 * the driver polls RNG_STATE instead of attaching RK3576_IRQ_RKRNG_NS.
 *
 * The driver exports two things:
 *
 *   1. rk3576_rng_read(), a direct blocking entropy call for in-kernel
 *      users such as the WPA supplicant nonce generator and MAC address
 *      randomisation.
 *   2. up_rnginitialize() / devurandom_register(), the arch hooks that the
 *      NuttX devrandom framework (drivers/misc) uses to publish
 *      /dev/random and /dev/urandom.
 *
 * Both /dev/random and /dev/urandom are backed by the same hardware source;
 * the block never blocks waiting for entropy, so the usual distinction
 * between the two devices does not apply here.
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
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_rng.h"
#include "rk3576_rng.h"

#ifdef CONFIG_RK3576_RNG

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Busy-wait budget for one 256-bit conversion.  The generator needs a few
 * microseconds; this is several orders of magnitude more and only bounds a
 * hardware failure.
 */

#define RK3576_RNG_POLL_LIMIT 100000

/* Automatic reseed request interval, in conversions.  The block reseeds the
 * post-processor from the ring oscillator every N reads.
 */

#define RK3576_RNG_RESEED_INTERVAL 100

/* Ring oscillator speed and output width used for every conversion. */

#define RK3576_RNG_CTL_CONFIG                          \
  (RK3576_RNG_CTL_ENABLE | RK3576_RNG_CTL_LEN_256BIT | \
   RK3576_RNG_CTL_RING_FASTEST)

/* CLK framework node name of the AHB interface clock. */

#define RK3576_RNG_HCLK_NAME "hclk_trng_ns_en"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_rng_reset_release(void);
static int rk3576_rng_clk_init(void);
static int rk3576_rng_convert(uint8_t *dest, size_t len);
static ssize_t rk3576_rng_fops_read(struct file *filep, char *buffer,
                                    size_t buflen);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Serialises access to the single hardware instance. */

static mutex_t g_rk3576_rng_lock = NXMUTEX_INITIALIZER;

/* Set once the block has been taken out of reset and enabled. */

static bool g_rk3576_rng_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_rng_getreg(unsigned int off)
{
  return getreg32(RK3576_RKRNG_ADDR + off);
}

static inline void rk3576_rng_putreg(unsigned int off, uint32_t val)
{
  putreg32(val, RK3576_RKRNG_ADDR + off);
}

/****************************************************************************
 * Name: rk3576_rng_reset_release
 *
 * Description:
 *   Take the TRNG out of reset.  SECURECRU_SOFTRST_CON00 is HIWORD write-
 *   masked and a set bit holds the block in reset, so releasing it means
 *   writing the bit's mask with a zero data bit.  Only the TRNG bit is
 *   touched; the other blocks sharing this register keep their state.
 *
 *   Reset control has no representation in the NuttX CLK framework, so it
 *   stays open coded here.
 *
 ****************************************************************************/

static void rk3576_rng_reset_release(void)
{
  putreg32((uint32_t)RK3576_SECURE_RST_HRESETN_TRNG_NS << 16,
           RK3576_SECURE_CRU_ADDR + RK3576_SECURE_CRU_SOFTRST_CON00);
}

/****************************************************************************
 * Name: rk3576_rng_clk_init
 *
 * Description:
 *   Ungate hclk_trng_ns through the NuttX CLK framework.  All clock
 *   handling of this driver lives here.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_rng_clk_init(void)
{
  struct clk_s *hclk;
  int ret;

  hclk = clk_get(RK3576_RNG_HCLK_NAME);
  if (hclk == NULL)
    {
      _err("ERROR: RKRNG: failed to get %s\n", RK3576_RNG_HCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      _err("ERROR: RKRNG: failed to enable %s: %d\n", RK3576_RNG_HCLK_NAME,
           ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rng_convert
 *
 * Description:
 *   Run one 256-bit conversion and copy up to RK3576_RNG_DOUT_NBYTES bytes
 *   of the result to the caller.  The generator must already be enabled and
 *   the caller must hold g_rk3576_rng_lock.
 *
 * Input Parameters:
 *   dest - Destination buffer.
 *   len  - Number of bytes to copy, 1 .. RK3576_RNG_DOUT_NBYTES.
 *
 * Returned Value:
 *   OK on success, -ETIMEDOUT if the generator never signalled completion.
 *
 ****************************************************************************/

static int rk3576_rng_convert(uint8_t *dest, size_t len)
{
  uint32_t words[RK3576_RNG_DOUT_NWORDS];
  unsigned int i;

  /* Clear any stale completion flag, then kick off a conversion. */

  rk3576_rng_putreg(RK3576_RNG_STATE, RK3576_RNG_STATE_DONE);
  rk3576_rng_putreg(RK3576_RNG_CTL, RK3576_RNG_WRITE_MASK |
                                        RK3576_RNG_CTL_CONFIG |
                                        RK3576_RNG_CTL_START);

  for (i = 0; i < RK3576_RNG_POLL_LIMIT; i++)
    {
      if ((rk3576_rng_getreg(RK3576_RNG_STATE) & RK3576_RNG_STATE_DONE) != 0)
        {
          break;
        }
    }

  if (i >= RK3576_RNG_POLL_LIMIT)
    {
      _err("ERROR: RKRNG conversion timed out, state=0x%08" PRIx32 "\n",
           rk3576_rng_getreg(RK3576_RNG_STATE));

      /* Leave START deasserted so the next attempt starts from a known
       * state.
       */

      rk3576_rng_putreg(RK3576_RNG_CTL,
                        RK3576_RNG_WRITE_MASK | RK3576_RNG_CTL_CONFIG);
      return -ETIMEDOUT;
    }

  for (i = 0; i < RK3576_RNG_DOUT_NWORDS; i++)
    {
      words[i] = rk3576_rng_getreg(RK3576_RNG_DOUT(i));
    }

  /* Acknowledge (write one to clear) and deassert START. */

  rk3576_rng_putreg(RK3576_RNG_STATE, RK3576_RNG_STATE_DONE);
  rk3576_rng_putreg(RK3576_RNG_CTL,
                    RK3576_RNG_WRITE_MASK | RK3576_RNG_CTL_CONFIG);

  memcpy(dest, words, len);

  /* Do not leave entropy sitting on the stack. */

  memset(words, 0, sizeof(words));
  return OK;
}

/****************************************************************************
 * Name: rk3576_rng_fops_read
 *
 * Description:
 *   read() handler shared by /dev/random and /dev/urandom.
 *
 ****************************************************************************/

static ssize_t rk3576_rng_fops_read(struct file *filep, char *buffer,
                                    size_t buflen)
{
  int ret;

  (void)filep;

  if (buflen == 0)
    {
      return 0;
    }

  ret = rk3576_rng_read((uint8_t *)buffer, buflen);
  if (ret < 0)
    {
      return ret;
    }

  return (ssize_t)buflen;
}

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct file_operations g_rk3576_rng_fops = {
  .read = rk3576_rng_fops_read,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rng_initialize
 *
 * Description:
 *   See rk3576_rng.h.
 *
 ****************************************************************************/

int rk3576_rng_initialize(void)
{
  int ret;

  ret = nxmutex_lock(&g_rk3576_rng_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_rk3576_rng_ready)
    {
      ret = rk3576_rng_clk_init();
      if (ret < 0)
        {
          nxmutex_unlock(&g_rk3576_rng_lock);
          return ret;
        }

      rk3576_rng_reset_release();

      /* Program the automatic reseed interval and enable the ring
       * oscillator.  START is left low; it is pulsed per conversion.
       */

      rk3576_rng_putreg(RK3576_RNG_AUTO_RQSTS, RK3576_RNG_RESEED_INTERVAL);
      rk3576_rng_putreg(RK3576_RNG_CTL,
                        RK3576_RNG_WRITE_MASK | RK3576_RNG_CTL_CONFIG);
      rk3576_rng_putreg(RK3576_RNG_STATE, RK3576_RNG_STATE_DONE);

      g_rk3576_rng_ready = true;
    }

  nxmutex_unlock(&g_rk3576_rng_lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rng_read
 *
 * Description:
 *   See rk3576_rng.h.
 *
 ****************************************************************************/

int rk3576_rng_read(uint8_t *buf, size_t len)
{
  int ret;

  if (buf == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return OK;
    }

  ret = rk3576_rng_initialize();
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&g_rk3576_rng_lock);
  if (ret < 0)
    {
      return ret;
    }

  while (len > 0)
    {
      size_t chunk = len;

      if (chunk > RK3576_RNG_DOUT_NBYTES)
        {
          chunk = RK3576_RNG_DOUT_NBYTES;
        }

      ret = rk3576_rng_convert(buf, chunk);
      if (ret < 0)
        {
          nxmutex_unlock(&g_rk3576_rng_lock);
          return ret;
        }

      buf += chunk;
      len -= chunk;
    }

  nxmutex_unlock(&g_rk3576_rng_lock);
  return OK;
}

/****************************************************************************
 * Name: up_rnginitialize
 *
 * Description:
 *   Arch hook called by up_initialize() when CONFIG_DEV_RANDOM is selected.
 *   Brings up the generator and registers /dev/random.
 *
 ****************************************************************************/

void up_rnginitialize(void)
{
  int ret;

  ret = rk3576_rng_initialize();
  if (ret < 0)
    {
      _err("ERROR: failed to initialize RKRNG: %d\n", ret);
      return;
    }

  ret = register_driver("/dev/random", &g_rk3576_rng_fops, 0444, NULL);
  if (ret < 0)
    {
      _err("ERROR: failed to register /dev/random: %d\n", ret);
    }
}

/****************************************************************************
 * Name: devurandom_register
 *
 * Description:
 *   Register /dev/urandom backed by the same hardware source.  Only built
 *   when the arch (rather than the software pool) owns /dev/urandom.
 *
 ****************************************************************************/

#ifdef CONFIG_DEV_URANDOM_ARCH
void devurandom_register(void)
{
  int ret;

  ret = rk3576_rng_initialize();
  if (ret < 0)
    {
      _err("ERROR: failed to initialize RKRNG: %d\n", ret);
      return;
    }

  ret = register_driver("/dev/urandom", &g_rk3576_rng_fops, 0444, NULL);
  if (ret < 0)
    {
      _err("ERROR: failed to register /dev/urandom: %d\n", ret);
    }
}
#endif /* CONFIG_DEV_URANDOM_ARCH */

#endif /* CONFIG_RK3576_RNG */
