/****************************************************************************
 * chips/rk3576/rk3576_spi.c
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
 * RK3576 SPI master driver (Rockchip SPI block, "rockchip,rk3066-spi"
 * compatible), implementing the NuttX struct spi_ops_s lower half.
 *
 * The transfer engine is programmed I/O: each chunk of up to
 * RK3576_SPI_MAX_FRAMES frames is pushed through the 32-entry TX FIFO while
 * the RX FIFO is drained in the same loop, with the number of frames in
 * flight capped at the FIFO depth so the receiver can never overflow.  The
 * controller interrupt is therefore not used; a bounded no-progress spin
 * count turns a wedged controller into -ETIMEDOUT instead of a hang.  This
 * mirrors the polled rk3576_i2c driver and keeps the driver usable from
 * board bring-up context before the scheduler is fully up.
 *
 * Pin muxing and any GPIO chip select are the board's responsibility (there
 * is no pinctrl framework yet); the board overrides the weak
 * rk3576_spi_bus_select() / _status() / _cmddata() hooks at the bottom of
 * this file.  Only SPI4 is routed on the KICKPI-K7, but all five instances
 * are selectable by port index.
 *
 * This IP is single-lane only.  Dual/quad (QSPI) peripherals must use the
 * separate RK3576 FSPI controller; see RK3576_SPI_LANES_MAX.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_spi.h"
#include "rk3576_spi.h"

#ifdef CONFIG_RK3576_SPI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Functional clock feeding the BAUDR divider, in Hz.
 *
 * TODO: query the CRU once it exposes a clk_spi rate/gate API
 * (rk3576_cru_set_spi_clock_gate() / _selection() do not exist yet).  The
 * bootloader leaves clk_spi on its reset parent, which is the 200 MHz
 * gpll_div6 branch used by the vendor kernel for every SPI instance.
 */

#define RK3576_SPI_CLKIN 200000000

/* Default bus frequency used until the first SPI_SETFREQUENCY(). */

#define RK3576_SPI_DEFAULT_HZ 1000000

/* Number of consecutive polls that may make no forward progress (no FIFO
 * slot freed, no word received) before a transfer is abandoned.  At the
 * lowest supported bit rate one frame still completes far inside this
 * budget, so it only ever fires on genuinely stuck hardware.
 */

#define RK3576_SPI_NOPROGRESS_LIMIT 1000000

/* Dummy frame clocked out during a receive-only software transfer that the
 * hardware still runs in TX-and-RX mode (SPI_EXCHANGE with txbuffer NULL).
 */

#define RK3576_SPI_TXDUMMY 0xffff

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static per-controller description. */

struct rk3576_spi_desc_s
{
  uintptr_t base; /* Controller register base */
};

/* Runtime state of one controller. */

struct rk3576_spi_priv_s
{
  struct spi_dev_s dev;  /* Base class (must be first)               */
  uintptr_t base;        /* Controller register base                 */
  int port;              /* Controller index (0..4)                  */
  uint32_t clkin;        /* Functional clock into BAUDR (Hz)         */
  uint32_t frequency;    /* Requested bus frequency (Hz)             */
  uint32_t actual;       /* Frequency the divider actually yields    */
  uint8_t nbits;         /* Frame width in bits (4, 8 or 16)         */
  uint8_t mode;          /* enum spi_mode_e currently programmed     */
  bool initialized;      /* True once the port has been set up       */
  mutex_t lock;          /* Bus arbitration for SPI_LOCK()           */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_spi_lock(struct spi_dev_s *dev, bool lock);
static void rk3576_spi_select(struct spi_dev_s *dev, uint32_t devid,
                              bool selected);
static uint32_t rk3576_spi_setfrequency(struct spi_dev_s *dev,
                                        uint32_t frequency);
static void rk3576_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode);
static void rk3576_spi_setbits(struct spi_dev_s *dev, int nbits);
static uint8_t rk3576_spi_status(struct spi_dev_s *dev, uint32_t devid);
#ifdef CONFIG_SPI_CMDDATA
static int rk3576_spi_cmddata(struct spi_dev_s *dev, uint32_t devid,
                              bool cmd);
#endif
static uint32_t rk3576_spi_send(struct spi_dev_s *dev, uint32_t wd);
#ifdef CONFIG_SPI_EXCHANGE
static void rk3576_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                                void *rxbuffer, size_t nwords);
#else
static void rk3576_spi_sndblock(struct spi_dev_s *dev, const void *txbuffer,
                                size_t nwords);
static void rk3576_spi_recvblock(struct spi_dev_s *dev, void *rxbuffer,
                                 size_t nwords);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_rk3576_spi_ops =
{
  .lock         = rk3576_spi_lock,
  .select       = rk3576_spi_select,
  .setfrequency = rk3576_spi_setfrequency,
  .setmode      = rk3576_spi_setmode,
  .setbits      = rk3576_spi_setbits,
  .status       = rk3576_spi_status,
#ifdef CONFIG_SPI_CMDDATA
  .cmddata      = rk3576_spi_cmddata,
#endif
  .send         = rk3576_spi_send,
#ifdef CONFIG_SPI_EXCHANGE
  .exchange     = rk3576_spi_exchange,
#else
  .sndblock     = rk3576_spi_sndblock,
  .recvblock    = rk3576_spi_recvblock,
#endif
};

static const struct rk3576_spi_desc_s g_rk3576_spi_desc[RK3576_SPI_NPORTS] =
{
  {
    .base = RK3576_SPI0_ADDR,
  },
  {
    .base = RK3576_SPI1_ADDR,
  },
  {
    .base = RK3576_SPI2_ADDR,
  },
  {
    .base = RK3576_SPI3_ADDR,
  },
  {
    .base = RK3576_SPI4_ADDR,
  },
};

static struct rk3576_spi_priv_s g_rk3576_spi[RK3576_SPI_NPORTS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_spi_getreg(struct rk3576_spi_priv_s *priv,
                                         unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void rk3576_spi_putreg(struct rk3576_spi_priv_s *priv,
                                     unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_spi_enable
 *
 * Description:
 *   Enable or disable the controller.  CTRLR0/CTRLR1/BAUDR may only be
 *   written while the controller is disabled.
 ****************************************************************************/

static void rk3576_spi_enable(struct rk3576_spi_priv_s *priv, bool enable)
{
  rk3576_spi_putreg(priv, RK3576_SPI_ENR, enable ? SPI_ENR_ENABLE : 0);
}

/****************************************************************************
 * Name: rk3576_spi_modifyctrlr0
 *
 * Description:
 *   Read-modify-write CTRLR0.  The controller must already be disabled.
 ****************************************************************************/

static void rk3576_spi_modifyctrlr0(struct rk3576_spi_priv_s *priv,
                                    uint32_t clrbits, uint32_t setbits)
{
  uint32_t regval = rk3576_spi_getreg(priv, RK3576_SPI_CTRLR0);

  regval &= ~clrbits;
  regval |= setbits;
  rk3576_spi_putreg(priv, RK3576_SPI_CTRLR0, regval);
}

/****************************************************************************
 * Name: rk3576_spi_dfs
 *
 * Description:
 *   Translate a frame width in bits into the CTRLR0.DFS encoding.
 *
 * Returned Value:
 *   The DFS field value, or a negated errno value for an unsupported width.
 ****************************************************************************/

static int rk3576_spi_dfs(int nbits)
{
  switch (nbits)
    {
      case 4:
        return SPI_CTRLR0_DFS_4BIT;

      case 8:
        return SPI_CTRLR0_DFS_8BIT;

      case 16:
        return SPI_CTRLR0_DFS_16BIT;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: rk3576_spi_lock
 ****************************************************************************/

static int rk3576_spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: rk3576_spi_select
 *
 * Description:
 *   Assert or de-assert the chip select for the addressed device.  The
 *   actual pin is board business, so this defers to the weak
 *   rk3576_spi_bus_select() hook.
 ****************************************************************************/

static void rk3576_spi_select(struct spi_dev_s *dev, uint32_t devid,
                              bool selected)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  rk3576_spi_bus_select(priv->port, devid, selected);
}

/****************************************************************************
 * Name: rk3576_spi_setfrequency
 *
 * Description:
 *   Program BAUDR for the requested SCLK frequency.  sclk = clkin / BAUDR
 *   and BAUDR must be an even number in [2, 0xfffe], so the divider is
 *   rounded up to the next even value; the resulting frequency is therefore
 *   always less than or equal to the request.
 *
 * Returned Value:
 *   The frequency the divider actually produces, in Hz.
 ****************************************************************************/

static uint32_t rk3576_spi_setfrequency(struct spi_dev_s *dev,
                                        uint32_t frequency)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint32_t div;

  if (frequency == 0)
    {
      frequency = RK3576_SPI_DEFAULT_HZ;
    }

  if (frequency == priv->frequency)
    {
      return priv->actual;
    }

  /* Round the divider up so the bus never runs faster than requested, then
   * round up again to the next even value.
   */

  div = (priv->clkin + frequency - 1) / frequency;
  div = (div + 1) & ~1u;

  if (div < SPI_BAUDR_MIN)
    {
      div = SPI_BAUDR_MIN;
    }
  else if (div > SPI_BAUDR_MAX)
    {
      div = SPI_BAUDR_MAX;
    }

  rk3576_spi_enable(priv, false);
  rk3576_spi_putreg(priv, RK3576_SPI_BAUDR, div);

  priv->frequency = frequency;
  priv->actual    = priv->clkin / div;

  spiinfo("SPI%d frequency %" PRIu32 " Hz -> div %" PRIu32
          " actual %" PRIu32 " Hz\n",
          priv->port, frequency, div, priv->actual);

  return priv->actual;
}

/****************************************************************************
 * Name: rk3576_spi_setmode
 *
 * Description:
 *   Program CTRLR0.SCPOL / CTRLR0.SCPH for the requested SPI mode.
 ****************************************************************************/

static void rk3576_spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint32_t setbits;

  if (mode == (enum spi_mode_e)priv->mode)
    {
      return;
    }

  switch (mode)
    {
      case SPIDEV_MODE0: /* CPOL = 0, CPHA = 0 */
        setbits = 0;
        break;

      case SPIDEV_MODE1: /* CPOL = 0, CPHA = 1 */
        setbits = SPI_CTRLR0_SCPH;
        break;

      case SPIDEV_MODE2: /* CPOL = 1, CPHA = 0 */
        setbits = SPI_CTRLR0_SCPOL;
        break;

      case SPIDEV_MODE3: /* CPOL = 1, CPHA = 1 */
        setbits = SPI_CTRLR0_SCPOL | SPI_CTRLR0_SCPH;
        break;

      default:
        spierr("ERROR: SPI%d unsupported mode %d\n", priv->port, (int)mode);
        return;
    }

  rk3576_spi_enable(priv, false);
  rk3576_spi_modifyctrlr0(priv, SPI_CTRLR0_SCPOL | SPI_CTRLR0_SCPH, setbits);

  priv->mode = (uint8_t)mode;
}

/****************************************************************************
 * Name: rk3576_spi_setbits
 *
 * Description:
 *   Program the frame width.  The hardware supports 4, 8 and 16 bits per
 *   frame; anything else is rejected and the previous width is kept.
 ****************************************************************************/

static void rk3576_spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  int dfs;

  if (nbits == priv->nbits)
    {
      return;
    }

  dfs = rk3576_spi_dfs(nbits);
  if (dfs < 0)
    {
      spierr("ERROR: SPI%d unsupported frame width %d\n", priv->port, nbits);
      return;
    }

  rk3576_spi_enable(priv, false);

  /* CTRLR0.BHT selects the APB access width of the FIFO data registers:
   * 8-bit accesses for frames of 8 bits or less, 16-bit for wider frames.
   */

  rk3576_spi_modifyctrlr0(priv, SPI_CTRLR0_DFS_MASK | SPI_CTRLR0_BHT_8BIT,
                          (uint32_t)dfs |
                          (nbits <= 8 ? SPI_CTRLR0_BHT_8BIT : 0));

  priv->nbits = (uint8_t)nbits;
}

/****************************************************************************
 * Name: rk3576_spi_status
 ****************************************************************************/

static uint8_t rk3576_spi_status(struct spi_dev_s *dev, uint32_t devid)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  return rk3576_spi_bus_status(priv->port, devid);
}

/****************************************************************************
 * Name: rk3576_spi_cmddata
 ****************************************************************************/

#ifdef CONFIG_SPI_CMDDATA
static int rk3576_spi_cmddata(struct spi_dev_s *dev, uint32_t devid,
                              bool cmd)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  return rk3576_spi_bus_cmddata(priv->port, devid, cmd);
}
#endif

/****************************************************************************
 * Name: rk3576_spi_transfer
 *
 * Description:
 *   Run one hardware transfer of up to RK3576_SPI_MAX_FRAMES frames.  The
 *   controller always runs in TX-and-RX mode when a receive buffer is
 *   supplied, so a receive-only software request still clocks out dummy
 *   frames; a transmit-only request uses TX-only mode and discards the
 *   incoming data in hardware.
 *
 * Input Parameters:
 *   priv     - Controller state.
 *   txbuffer - Frames to transmit, or NULL to clock out dummy frames.
 *   rxbuffer - Where to store received frames, or NULL to discard them.
 *   nwords   - Number of frames, at most RK3576_SPI_MAX_FRAMES.
 *
 * Returned Value:
 *   OK on success; -ETIMEDOUT if the controller stops making progress.
 ****************************************************************************/

static int rk3576_spi_transfer(struct rk3576_spi_priv_s *priv,
                               const void *txbuffer, void *rxbuffer,
                               size_t nwords)
{
  const uint8_t *tx8   = (const uint8_t *)txbuffer;
  const uint16_t *tx16 = (const uint16_t *)txbuffer;
  uint8_t *rx8         = (uint8_t *)rxbuffer;
  uint16_t *rx16       = (uint16_t *)rxbuffer;
  bool wide            = (priv->nbits > 8);
  size_t txcount       = 0;
  size_t rxcount       = 0;
  uint32_t stall       = 0;
  uint32_t regval;
  int ret              = OK;

  DEBUGASSERT(nwords > 0 && nwords <= RK3576_SPI_MAX_FRAMES);

  /* Program the transfer mode and frame count with the controller off. */

  rk3576_spi_enable(priv, false);

  rk3576_spi_modifyctrlr0(priv, SPI_CTRLR0_XFM_MASK,
                          rxbuffer != NULL ? SPI_CTRLR0_XFM_TR
                                           : SPI_CTRLR0_XFM_TO);

  rk3576_spi_putreg(priv, RK3576_SPI_CTRLR1,
                    (uint32_t)(nwords - 1) & SPI_CTRLR1_NDF_MASK);

  /* Interrupts stay masked - this is a polled driver - but any latched
   * status from a previous (aborted) transfer is cleared so the overflow
   * checks below only see this transfer's errors.
   */

  rk3576_spi_putreg(priv, RK3576_SPI_IMR, 0);
  rk3576_spi_putreg(priv, RK3576_SPI_ICR, SPI_INT_ALL);

  rk3576_spi_enable(priv, true);

  while (rxcount < nwords)
    {
      bool progress = false;

      /* Push as many frames as the TX FIFO accepts, keeping the number of
       * frames in flight below the RX FIFO depth so the receiver cannot
       * overflow while we are busy filling the transmitter.
       */

      while (txcount < nwords &&
             (txcount - rxcount) < RK3576_SPI_FIFO_LEN &&
             (rk3576_spi_getreg(priv, RK3576_SPI_SR) & SPI_SR_TF_FULL) == 0)
        {
          if (txbuffer == NULL)
            {
              regval = RK3576_SPI_TXDUMMY;
            }
          else if (wide)
            {
              regval = tx16[txcount];
            }
          else
            {
              regval = tx8[txcount];
            }

          rk3576_spi_putreg(priv, RK3576_SPI_TXDR, regval);
          txcount++;
          progress = true;
        }

      /* In TX-only mode the RX FIFO stays empty; completion is signalled by
       * the TX FIFO draining and the master leaving the busy state.
       */

      if (rxbuffer == NULL)
        {
          if (txcount >= nwords)
            {
              regval = rk3576_spi_getreg(priv, RK3576_SPI_SR);
              if ((regval & SPI_SR_TF_EMPTY) != 0 &&
                  (regval & SPI_SR_BUSY) == 0)
                {
                  rxcount = nwords;
                  break;
                }
            }
        }
      else
        {
          while (rxcount < txcount &&
                 (rk3576_spi_getreg(priv, RK3576_SPI_SR) &
                  SPI_SR_RF_EMPTY) == 0)
            {
              regval = rk3576_spi_getreg(priv, RK3576_SPI_RXDR);
              if (wide)
                {
                  rx16[rxcount] = (uint16_t)regval;
                }
              else
                {
                  rx8[rxcount] = (uint8_t)regval;
                }

              rxcount++;
              progress = true;
            }
        }

      if (progress)
        {
          stall = 0;
        }
      else if (++stall >= RK3576_SPI_NOPROGRESS_LIMIT)
        {
          spierr("ERROR: SPI%d stalled, sr=%08" PRIx32
                 " risr=%08" PRIx32 " tx=%zu rx=%zu of %zu\n",
                 priv->port, rk3576_spi_getreg(priv, RK3576_SPI_SR),
                 rk3576_spi_getreg(priv, RK3576_SPI_RISR),
                 txcount, rxcount, nwords);
          ret = -ETIMEDOUT;
          break;
        }
    }

  /* Wait for the last frame to leave the shift register before dropping the
   * enable, otherwise the tail of the transfer is truncated on the wire.
   */

  if (ret == OK)
    {
      for (stall = 0; stall < RK3576_SPI_NOPROGRESS_LIMIT; stall++)
        {
          if ((rk3576_spi_getreg(priv, RK3576_SPI_SR) & SPI_SR_BUSY) == 0)
            {
              break;
            }
        }

      if (stall >= RK3576_SPI_NOPROGRESS_LIMIT)
        {
          spierr("ERROR: SPI%d still busy at end of transfer\n", priv->port);
          ret = -ETIMEDOUT;
        }
    }

  regval = rk3576_spi_getreg(priv, RK3576_SPI_RISR);
  if ((regval & (SPI_INT_RXOI | SPI_INT_TXOI)) != 0)
    {
      spierr("ERROR: SPI%d FIFO overflow, risr=%08" PRIx32 "\n",
             priv->port, regval);
      ret = -EIO;
    }

  rk3576_spi_putreg(priv, RK3576_SPI_ICR, SPI_INT_ALL);
  rk3576_spi_enable(priv, false);

  return ret;
}

/****************************************************************************
 * Name: rk3576_spi_exchangeall
 *
 * Description:
 *   Split a software transfer of arbitrary length into hardware chunks of
 *   at most RK3576_SPI_MAX_FRAMES frames each.
 ****************************************************************************/

static void rk3576_spi_exchangeall(struct rk3576_spi_priv_s *priv,
                                   const void *txbuffer, void *rxbuffer,
                                   size_t nwords)
{
  size_t framesize = (priv->nbits > 8) ? sizeof(uint16_t) : sizeof(uint8_t);
  const uint8_t *tx = (const uint8_t *)txbuffer;
  uint8_t *rx = (uint8_t *)rxbuffer;
  size_t remaining = nwords;

  while (remaining > 0)
    {
      size_t chunk = remaining;

      if (chunk > RK3576_SPI_MAX_FRAMES)
        {
          chunk = RK3576_SPI_MAX_FRAMES;
        }

      if (rk3576_spi_transfer(priv, tx, rx, chunk) < 0)
        {
          return;
        }

      if (tx != NULL)
        {
          tx += chunk * framesize;
        }

      if (rx != NULL)
        {
          rx += chunk * framesize;
        }

      remaining -= chunk;
    }
}

/****************************************************************************
 * Name: rk3576_spi_send
 *
 * Description:
 *   Exchange one frame: shift out wd and return the frame shifted in.
 ****************************************************************************/

static uint32_t rk3576_spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;
  uint16_t tx16 = (uint16_t)wd;
  uint16_t rx16 = 0;
  uint8_t tx8   = (uint8_t)wd;
  uint8_t rx8   = 0;
  int ret;

  if (priv->nbits > 8)
    {
      ret = rk3576_spi_transfer(priv, &tx16, &rx16, 1);
      return ret < 0 ? 0 : (uint32_t)rx16;
    }

  ret = rk3576_spi_transfer(priv, &tx8, &rx8, 1);
  return ret < 0 ? 0 : (uint32_t)rx8;
}

/****************************************************************************
 * Name: rk3576_spi_exchange
 ****************************************************************************/

#ifdef CONFIG_SPI_EXCHANGE
static void rk3576_spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                                void *rxbuffer, size_t nwords)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nwords > 0)
    {
      rk3576_spi_exchangeall(priv, txbuffer, rxbuffer, nwords);
    }
}

#else
/****************************************************************************
 * Name: rk3576_spi_sndblock
 ****************************************************************************/

static void rk3576_spi_sndblock(struct spi_dev_s *dev, const void *txbuffer,
                                size_t nwords)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nwords > 0)
    {
      rk3576_spi_exchangeall(priv, txbuffer, NULL, nwords);
    }
}

/****************************************************************************
 * Name: rk3576_spi_recvblock
 ****************************************************************************/

static void rk3576_spi_recvblock(struct spi_dev_s *dev, void *rxbuffer,
                                 size_t nwords)
{
  struct rk3576_spi_priv_s *priv = (struct rk3576_spi_priv_s *)dev;

  if (nwords > 0)
    {
      rk3576_spi_exchangeall(priv, NULL, rxbuffer, nwords);
    }
}
#endif /* CONFIG_SPI_EXCHANGE */

/****************************************************************************
 * Name: rk3576_spi_hwinit
 *
 * Description:
 *   Bring one controller into a known master configuration: Motorola frame
 *   format, MSB first, no receive sample delay, one chip select idle cycle
 *   between frames, all interrupts masked and DMA off.  Frequency, mode and
 *   frame width are then applied through the normal ops so the cached state
 *   matches the hardware.
 ****************************************************************************/

static void rk3576_spi_hwinit(struct rk3576_spi_priv_s *priv)
{
  rk3576_spi_enable(priv, false);

  rk3576_spi_putreg(priv, RK3576_SPI_CTRLR0,
                    SPI_CTRLR0_FRF_SPI | SPI_CTRLR0_XFM_TR |
                    SPI_CTRLR0_CSM_0CYCLE | SPI_CTRLR0_SSD);

  rk3576_spi_putreg(priv, RK3576_SPI_SER, 0);
  rk3576_spi_putreg(priv, RK3576_SPI_IMR, 0);
  rk3576_spi_putreg(priv, RK3576_SPI_ICR, SPI_INT_ALL);

  /* DMA is not wired up yet; make sure the requests stay de-asserted.
   *
   * TODO: drive the transfer through the PL330 (rk3576_dma_initialize() and
   * the generic DMA_GET_CHAN/DMA_CONFIG/DMA_START ops) for large blocks.
   * The request lines are RK3576_SPI4_DRQ_TX / _RX; DMACR.TDE/RDE plus the
   * DMATDLR/DMARDLR watermarks are the only extra controller state needed.
   */

  rk3576_spi_putreg(priv, RK3576_SPI_DMACR, 0);

  /* Interrupt-driven transfers are likewise not used: the driver polls the
   * status register, so the controller IRQ (RK3576_IRQ_SPI0 + port) is left
   * unattached.
   */

  rk3576_spi_putreg(priv, RK3576_SPI_TXFTLR, RK3576_SPI_FIFO_LEN / 2);
  rk3576_spi_putreg(priv, RK3576_SPI_RXFTLR, 0);

  /* Force the cached state to differ from the defaults being applied so the
   * early-exit checks in the setters do not skip the first programming.
   */

  priv->frequency = 0;
  priv->actual    = 0;
  priv->nbits     = 0;
  priv->mode      = (uint8_t)SPIDEV_MODE3;

  rk3576_spi_setfrequency(&priv->dev, RK3576_SPI_DEFAULT_HZ);
  rk3576_spi_setmode(&priv->dev, SPIDEV_MODE0);
  rk3576_spi_setbits(&priv->dev, 8);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_spi_initialize
 *
 * Description:
 *   Initialize one RK3576 SPI controller as a NuttX SPI master.  See
 *   rk3576_spi.h for the full description.
 ****************************************************************************/

struct spi_dev_s *rk3576_spi_initialize(int port)
{
  struct rk3576_spi_priv_s *priv;

  if (port < 0 || port >= RK3576_SPI_NPORTS)
    {
      spierr("ERROR: Invalid SPI port %d\n", port);
      return NULL;
    }

  priv = &g_rk3576_spi[port];
  if (priv->initialized)
    {
      return &priv->dev;
    }

  priv->dev.ops = &g_rk3576_spi_ops;
  priv->base    = g_rk3576_spi_desc[port].base;
  priv->port    = port;
  priv->clkin   = RK3576_SPI_CLKIN;

  nxmutex_init(&priv->lock);

  rk3576_spi_hwinit(priv);

  priv->initialized = true;

  spiinfo("SPI%d ready, base=%08" PRIxPTR " clkin=%" PRIu32 " Hz\n",
          port, priv->base, priv->clkin);

  return &priv->dev;
}

/****************************************************************************
 * Name: rk3576_spi_bus_select
 *
 * Description:
 *   Weak default chip select hook: drive the controller's native chip
 *   select.  A board with a GPIO chip select overrides this.
 ****************************************************************************/

void weak_function rk3576_spi_bus_select(int port, uint32_t devid,
                                         bool selected)
{
  struct rk3576_spi_priv_s *priv;
  uint32_t cs = SPIDEVID_INDEX(devid);

  if (port < 0 || port >= RK3576_SPI_NPORTS || cs >= (uint32_t)RK3576_SPI_NCS)
    {
      spierr("ERROR: SPI%d has no native chip select %" PRIu32 "\n",
             port, cs);
      return;
    }

  priv = &g_rk3576_spi[port];
  rk3576_spi_putreg(priv, RK3576_SPI_SER, selected ? SPI_SER_CS(cs) : 0);
}

/****************************************************************************
 * Name: rk3576_spi_bus_status
 *
 * Description:
 *   Weak default status hook: no card-detect or write-protect signals are
 *   known at chip level, so report an empty status.
 ****************************************************************************/

uint8_t weak_function rk3576_spi_bus_status(int port, uint32_t devid)
{
  UNUSED(port);
  UNUSED(devid);
  return 0;
}

/****************************************************************************
 * Name: rk3576_spi_bus_cmddata
 *
 * Description:
 *   Weak default command/data hook: the selector is a board-level GPIO, so
 *   without board support the operation is unsupported.
 ****************************************************************************/

#ifdef CONFIG_SPI_CMDDATA
int weak_function rk3576_spi_bus_cmddata(int port, uint32_t devid, bool cmd)
{
  UNUSED(port);
  UNUSED(devid);
  UNUSED(cmd);
  return -ENOSYS;
}
#endif

#endif /* CONFIG_RK3576_SPI */
