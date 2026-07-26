/****************************************************************************
 * chips/rk3576/rk3576_i3c.c
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
 * RK3576 I3C master driver (Synopsys DesignWare MIPI I3C host controller,
 * "rockchip,i3c-master": I3C0 @0x2abe0000, I3C1 @0x2abf0000).
 *
 * NuttX has no I3C subsystem, so the controller is exposed in two ways:
 *
 *   1. As a standard struct i2c_master_s.  I3C is backwards compatible with
 *      legacy I2C targets, so every existing NuttX I2C device driver can be
 *      used unmodified on an I3C bus.  This is the primary interface and is
 *      fully functional.
 *
 *   2. Through the native rk3576_i3c_* helpers declared in rk3576_i3c.h:
 *      CCC transfers, ENTDAA dynamic address assignment, SDR private
 *      transfers and in-band interrupts.
 *
 * Data movement is programmed I/O through the RX_TX_DATA_PORT.  The block
 * also supports DMA through the PL330 (DTS lists rx/tx request lines) but
 * PIO keeps the driver self-contained and is more than fast enough for the
 * register-access traffic these buses carry.
 *
 * Transfers are polled; the interrupt is only used to service in-band
 * interrupts (IBI), which are asynchronous by nature.
 *
 * Pin muxing is the board's responsibility.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_i3c.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_i3c.h"

#ifdef CONFIG_RK3576_I3C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* NuttX has no I3C subsystem and therefore no dedicated debug channel.
 * The controller is published as an I2C master, so reuse the I2C channel
 * for its messages.
 */

#define i3cerr  i2cerr
#define i3cinfo i2cinfo

/* Busy-wait bounds.  The controller either completes a queued command or
 * flags an error; these only guard against a wedged bus.
 */

#define RK3576_I3C_POLL_LIMIT     1000000

/* Bus timing targets (MIPI I3C Basic v1.1). */

#define RK3576_I3C_PP_FREQ_HZ     12500000 /* Push-pull SCL */
#define RK3576_I3C_OD_FREQ_HZ     4000000  /* Open-drain SCL (<= 4.17MHz) */
#define RK3576_I3C_I2C_FM_HZ      400000   /* I2C fast mode */
#define RK3576_I3C_I2C_FMP_HZ     1000000  /* I2C fast mode plus */

#define RK3576_I3C_TLOW_OD_MIN_NS 200 /* Min open-drain SCL low period */
#define RK3576_I3C_THIGH_MAX_NS   41  /* Max push-pull SCL high period */
#define RK3576_I3C_TBUS_FREE_NS   1300
#define RK3576_I3C_TBUS_IDLE_NS   200000

/* I2C fast mode: 1.3us low of a 2.5us period.  Fast mode plus: 0.5us low
 * of a 1.0us period.  Expressed as numerator/denominator of the period so
 * the split can be computed in integer arithmetic.
 */

#define RK3576_I3C_FM_LOW_NUM     13
#define RK3576_I3C_FM_LOW_DEN     25
#define RK3576_I3C_FMP_LOW_NUM    1
#define RK3576_I3C_FMP_LOW_DEN    2

/* Minimum counter value the controller accepts in a timing register. */

#define RK3576_I3C_CNT_MIN        2
#define RK3576_I3C_CNT_MAX        0xffff
#define RK3576_I3C_EXT_LCNT_MAX   0xff

/* Dynamic addresses handed out by ENTDAA start here (0x08 is the lowest
 * address usable by an I3C target).
 */

#define RK3576_I3C_DYNADDR_BASE   0x08

/* Transaction ID used for driver-issued commands. */

#define RK3576_I3C_TID_XFER       0x1
#define RK3576_I3C_TID_DAA        0x2

/* Device characteristics table entry layout (4 words per device). */

#define RK3576_I3C_DCT_ENTRY_SIZE 16
#define RK3576_I3C_DCT_PID_MSB    0 /* PID[47:16] */
#define RK3576_I3C_DCT_PID_LSB    4 /* PID[15:0] in bits [15:0] */
#define RK3576_I3C_DCT_CHAR       8 /* BCR[15:8] | DCR[7:0] */

#define RK3576_I3C_PID_LSB_BITS   16

/* Number of nanoseconds in one second, for count computations. */

#define RK3576_I3C_NSEC_PER_SEC   1000000000ULL

/* Controller base addresses (see DTS i3c-master@2abe0000/@2abf0000). */

static const uintptr_t g_rk3576_i3c_base[RK3576_I3C_NUM] = {
  RK3576_I3C0_ADDR,
  RK3576_I3C1_ADDR,
};

static const int g_rk3576_i3c_irq[RK3576_I3C_NUM] = {
  RK3576_IRQ_I3C0,
  RK3576_IRQ_I3C1,
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_i3c_priv_s
{
  struct i2c_master_s dev;  /* Base class (must be first) */
  uintptr_t base;           /* Controller base address */
  int port;                 /* Controller index */
  int irq;                  /* GIC interrupt number */
  struct clk_s *fclk;       /* Functional (core) clock */
  uint32_t fclk_hz;         /* Cached core clock rate */
  uint32_t i2c_freq;        /* Currently programmed legacy I2C rate */
  uint8_t datdepth;         /* Usable device address table entries */
  uintptr_t dat;            /* Device address table base address */
  uintptr_t dct;            /* Device characteristics table base address */
  mutex_t lock;             /* Serialize bus access */
  bool inited;              /* Controller brought up */

  /* Devices discovered by ENTDAA */

  int ndevs;
  struct rk3576_i3c_devinfo_s devs[RK3576_I3C_MAX_DEVS];

  /* Per-slot IBI callbacks */

  rk3576_i3c_ibi_cb_t ibicb[RK3576_I3C_MAX_DEVS];
  void *ibiarg[RK3576_I3C_MAX_DEVS];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t rk3576_i3c_getreg(struct rk3576_i3c_priv_s *priv,
                                         unsigned int off);
static inline void rk3576_i3c_putreg(struct rk3576_i3c_priv_s *priv,
                                     unsigned int off, uint32_t val);
static uint32_t rk3576_i3c_ns2cnt(struct rk3576_i3c_priv_s *priv,
                                  uint32_t ns);
static uint32_t rk3576_i3c_clamp(uint32_t value, uint32_t min,
                                 uint32_t max);
static uint8_t rk3576_i3c_addr_parity(uint8_t addr);
static void rk3576_i3c_set_dat(struct rk3576_i3c_priv_s *priv, uint8_t slot,
                               uint32_t value);
static void rk3576_i3c_i3c_timings(struct rk3576_i3c_priv_s *priv);
static void rk3576_i3c_i2c_timings(struct rk3576_i3c_priv_s *priv,
                                   uint32_t frequency);
static void rk3576_i3c_enable(struct rk3576_i3c_priv_s *priv, bool enable);
static int rk3576_i3c_resp_errno(uint32_t err);
static int rk3576_i3c_wait_resp(struct rk3576_i3c_priv_s *priv,
                                uint32_t *resp);
static int rk3576_i3c_push_tx(struct rk3576_i3c_priv_s *priv,
                              const uint8_t *buffer, uint16_t length);
static int rk3576_i3c_pop_rx(struct rk3576_i3c_priv_s *priv,
                             uint8_t *buffer, uint16_t length);
static int rk3576_i3c_run(struct rk3576_i3c_priv_s *priv, uint32_t arg,
                          uint32_t cmd, uint8_t *buffer, uint16_t length,
                          bool read, uint16_t *xferred);
static int rk3576_i3c_ccc_locked(struct rk3576_i3c_priv_s *priv,
                                 struct rk3576_i3c_ccc_s *ccc);
static void rk3576_i3c_service_ibi(struct rk3576_i3c_priv_s *priv);
static int rk3576_i3c_interrupt(int irq, void *context, void *arg);
static int rk3576_i3c_clk_init(struct rk3576_i3c_priv_s *priv);
static int rk3576_i3c_hw_init(struct rk3576_i3c_priv_s *priv);
static int rk3576_i3c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count);
#ifdef CONFIG_I2C_RESET
static int rk3576_i3c_reset(struct i2c_master_s *dev);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct i2c_ops_s g_rk3576_i3c_ops = {
  .transfer = rk3576_i3c_transfer,
#ifdef CONFIG_I2C_RESET
  .reset = rk3576_i3c_reset,
#endif
};

static struct rk3576_i3c_priv_s g_rk3576_i3c[RK3576_I3C_NUM];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_i3c_getreg(struct rk3576_i3c_priv_s *priv,
                                         unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void rk3576_i3c_putreg(struct rk3576_i3c_priv_s *priv,
                                     unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_i3c_ns2cnt
 *
 * Description:
 *   Convert a bus time in nanoseconds into core-clock cycles, rounding up.
 ****************************************************************************/

static uint32_t rk3576_i3c_ns2cnt(struct rk3576_i3c_priv_s *priv,
                                  uint32_t ns)
{
  uint64_t cycles;

  cycles = ((uint64_t)priv->fclk_hz * ns + RK3576_I3C_NSEC_PER_SEC - 1) /
           RK3576_I3C_NSEC_PER_SEC;

  return (uint32_t)cycles;
}

/****************************************************************************
 * Name: rk3576_i3c_clamp
 ****************************************************************************/

static uint32_t rk3576_i3c_clamp(uint32_t value, uint32_t min, uint32_t max)
{
  if (value < min)
    {
      return min;
    }

  if (value > max)
    {
      return max;
    }

  return value;
}

/****************************************************************************
 * Name: rk3576_i3c_addr_parity
 *
 * Description:
 *   Compute the odd-parity bit the device address table expects in bit 7 of
 *   the dynamic address field.
 ****************************************************************************/

static uint8_t rk3576_i3c_addr_parity(uint8_t addr)
{
  uint8_t parity = 1;
  uint8_t bit;

  for (bit = 0; bit < 7; bit++)
    {
      parity ^= (addr >> bit) & 1;
    }

  return parity;
}

/****************************************************************************
 * Name: rk3576_i3c_set_dat
 *
 * Description:
 *   Write one device address table entry.
 ****************************************************************************/

static void rk3576_i3c_set_dat(struct rk3576_i3c_priv_s *priv, uint8_t slot,
                               uint32_t value)
{
  putreg32(value, priv->dat + ((uintptr_t)slot * 4));
}

/****************************************************************************
 * Name: rk3576_i3c_i3c_timings
 *
 * Description:
 *   Program the I3C push-pull / open-drain SCL timings and the bus free /
 *   bus idle counters from the measured core clock rate.
 ****************************************************************************/

static void rk3576_i3c_i3c_timings(struct rk3576_i3c_priv_s *priv)
{
  uint32_t period;
  uint32_t hcnt;
  uint32_t lcnt;
  uint32_t odlcnt;
  uint32_t extlcnt;

  /* Push-pull: keep SCL high no longer than THIGH_MAX. */

  hcnt = rk3576_i3c_ns2cnt(priv, RK3576_I3C_THIGH_MAX_NS);
  period = priv->fclk_hz / RK3576_I3C_PP_FREQ_HZ;
  if (period <= hcnt)
    {
      hcnt = (period > RK3576_I3C_CNT_MIN * 2) ? period / 2 :
                                                 RK3576_I3C_CNT_MIN;
    }

  hcnt = rk3576_i3c_clamp(hcnt, RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX);
  lcnt = (period > hcnt) ? period - hcnt : RK3576_I3C_CNT_MIN;
  lcnt = rk3576_i3c_clamp(lcnt, RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX);

  rk3576_i3c_putreg(priv, RK3576_I3C_SCL_I3C_PP_TIMING,
                    RK3576_I3C_SCL_TIMING(hcnt, lcnt));

  /* Open drain: same high count, low count stretched to the slower rate
   * but never shorter than TLOW_OD_MIN.
   */

  period = priv->fclk_hz / RK3576_I3C_OD_FREQ_HZ;
  odlcnt = (period > hcnt) ? period - hcnt : RK3576_I3C_CNT_MIN;
  if (odlcnt < rk3576_i3c_ns2cnt(priv, RK3576_I3C_TLOW_OD_MIN_NS))
    {
      odlcnt = rk3576_i3c_ns2cnt(priv, RK3576_I3C_TLOW_OD_MIN_NS);
    }

  odlcnt = rk3576_i3c_clamp(odlcnt, RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX);

  rk3576_i3c_putreg(priv, RK3576_I3C_SCL_I3C_OD_TIMING,
                    RK3576_I3C_SCL_TIMING(hcnt, odlcnt));

  /* Extended low counts for the SDR1..SDR4 data rates.  Each step halves
   * the data rate relative to SDR0, which is expressed by doubling the low
   * count.  The field is only 8 bits wide, so saturate.
   */

  extlcnt = rk3576_i3c_clamp(lcnt * 2, RK3576_I3C_CNT_MIN,
                             RK3576_I3C_EXT_LCNT_MAX);
  rk3576_i3c_putreg(priv, RK3576_I3C_SCL_EXT_LCNT_TIMING,
                    RK3576_I3C_EXT_LCNT(extlcnt,
                                        rk3576_i3c_clamp(lcnt * 3,
                                            RK3576_I3C_CNT_MIN,
                                            RK3576_I3C_EXT_LCNT_MAX),
                                        rk3576_i3c_clamp(lcnt * 4,
                                            RK3576_I3C_CNT_MIN,
                                            RK3576_I3C_EXT_LCNT_MAX),
                                        rk3576_i3c_clamp(lcnt * 5,
                                            RK3576_I3C_CNT_MIN,
                                            RK3576_I3C_EXT_LCNT_MAX)));

  /* Bus free time gates repeated STARTs, bus idle time gates hot join
   * detection.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_BUS_FREE_TIMING,
                    rk3576_i3c_clamp(
                        rk3576_i3c_ns2cnt(priv, RK3576_I3C_TBUS_FREE_NS),
                        RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX));

  rk3576_i3c_putreg(priv, RK3576_I3C_BUS_IDLE_TIMING,
                    rk3576_i3c_clamp(
                        rk3576_i3c_ns2cnt(priv, RK3576_I3C_TBUS_IDLE_NS),
                        RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX));
}

/****************************************************************************
 * Name: rk3576_i3c_i2c_timings
 *
 * Description:
 *   Program the legacy I2C SCL timing registers for the requested bus
 *   frequency.  Frequencies above fast mode use the fast-mode-plus register
 *   pair; anything else is served by the fast-mode pair.
 ****************************************************************************/

static void rk3576_i3c_i2c_timings(struct rk3576_i3c_priv_s *priv,
                                   uint32_t frequency)
{
  uint32_t period;
  uint32_t hcnt;
  uint32_t lcnt;

  if (frequency == 0)
    {
      frequency = RK3576_I3C_I2C_FM_HZ;
    }

  if (frequency > RK3576_I3C_I2C_FMP_HZ)
    {
      frequency = RK3576_I3C_I2C_FMP_HZ;
    }

  if (priv->i2c_freq == frequency)
    {
      return;
    }

  period = priv->fclk_hz / frequency;

  if (frequency > RK3576_I3C_I2C_FM_HZ)
    {
      lcnt = period * RK3576_I3C_FMP_LOW_NUM / RK3576_I3C_FMP_LOW_DEN;
    }
  else
    {
      lcnt = period * RK3576_I3C_FM_LOW_NUM / RK3576_I3C_FM_LOW_DEN;
    }

  lcnt = rk3576_i3c_clamp(lcnt, RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX);
  hcnt = (period > lcnt) ? period - lcnt : RK3576_I3C_CNT_MIN;
  hcnt = rk3576_i3c_clamp(hcnt, RK3576_I3C_CNT_MIN, RK3576_I3C_CNT_MAX);

  if (frequency > RK3576_I3C_I2C_FM_HZ)
    {
      rk3576_i3c_putreg(priv, RK3576_I3C_SCL_I2C_FMP_TIMING,
                        RK3576_I3C_SCL_TIMING(hcnt, lcnt));
    }
  else
    {
      rk3576_i3c_putreg(priv, RK3576_I3C_SCL_I2C_FM_TIMING,
                        RK3576_I3C_SCL_TIMING(hcnt, lcnt));
    }

  priv->i2c_freq = frequency;
}

/****************************************************************************
 * Name: rk3576_i3c_enable
 *
 * Description:
 *   Set or clear the controller enable bit, preserving the other DEVICE_CTRL
 *   settings.
 ****************************************************************************/

static void rk3576_i3c_enable(struct rk3576_i3c_priv_s *priv, bool enable)
{
  uint32_t ctrl = rk3576_i3c_getreg(priv, RK3576_I3C_DEVICE_CTRL);

  if (enable)
    {
      ctrl |= RK3576_I3C_DEVCTRL_ENABLE;
    }
  else
    {
      ctrl &= ~RK3576_I3C_DEVCTRL_ENABLE;
    }

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_CTRL, ctrl);
}

/****************************************************************************
 * Name: rk3576_i3c_resp_errno
 *
 * Description:
 *   Translate a response queue error code into a negated errno.
 ****************************************************************************/

static int rk3576_i3c_resp_errno(uint32_t err)
{
  switch (err)
    {
      case RK3576_I3C_RESP_ERR_NONE:
        return OK;

      case RK3576_I3C_RESP_ERR_IBA_NACK:
      case RK3576_I3C_RESP_ERR_ADDR_NACK:
      case RK3576_I3C_RESP_ERR_I2C_NACK:
        return -ENXIO;

      case RK3576_I3C_RESP_ERR_OVER_UNDER:
        return -EIO;

      case RK3576_I3C_RESP_ERR_XFER_ABORT:
        return -ECANCELED;

      case RK3576_I3C_RESP_ERR_CRC:
      case RK3576_I3C_RESP_ERR_PARITY:
      case RK3576_I3C_RESP_ERR_FRAME:
      default:
        return -EIO;
    }
}

/****************************************************************************
 * Name: rk3576_i3c_wait_resp
 *
 * Description:
 *   Spin until the response queue holds at least one entry and pop it.
 ****************************************************************************/

static int rk3576_i3c_wait_resp(struct rk3576_i3c_priv_s *priv,
                                uint32_t *resp)
{
  uint32_t level;
  int t;

  for (t = 0; t < RK3576_I3C_POLL_LIMIT; t++)
    {
      level = rk3576_i3c_getreg(priv, RK3576_I3C_QUEUE_STATUS_LEVEL);
      if (((level & RK3576_I3C_QSTATUS_RESP_LEVEL_MASK) >>
           RK3576_I3C_QSTATUS_RESP_LEVEL_SHIFT) != 0)
        {
          *resp = rk3576_i3c_getreg(priv, RK3576_I3C_RESPONSE_QUEUE_PORT);
          return OK;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_i3c_push_tx
 *
 * Description:
 *   Push length bytes into the TX FIFO, little-endian packed, waiting for
 *   room when the FIFO fills up.
 ****************************************************************************/

static int rk3576_i3c_push_tx(struct rk3576_i3c_priv_s *priv,
                              const uint8_t *buffer, uint16_t length)
{
  uint16_t offset = 0;
  uint32_t word;
  uint32_t status;
  int t;
  int i;

  while (offset < length)
    {
      /* Wait for at least one free FIFO word. */

      for (t = 0; t < RK3576_I3C_POLL_LIMIT; t++)
        {
          status = rk3576_i3c_getreg(priv, RK3576_I3C_DATA_BUFFER_STATUS);
          if (((status & RK3576_I3C_DSTATUS_TX_EMPTY_MASK) >>
               RK3576_I3C_DSTATUS_TX_EMPTY_SHIFT) != 0)
            {
              break;
            }

          /* An error response aborts the transfer; stop pushing so the
           * caller can report it instead of spinning to the limit.
           */

          status = rk3576_i3c_getreg(priv, RK3576_I3C_QUEUE_STATUS_LEVEL);
          if (((status & RK3576_I3C_QSTATUS_RESP_LEVEL_MASK) >>
               RK3576_I3C_QSTATUS_RESP_LEVEL_SHIFT) != 0)
            {
              return -EIO;
            }
        }

      if (t >= RK3576_I3C_POLL_LIMIT)
        {
          return -ETIMEDOUT;
        }

      word = 0;
      for (i = 0; i < 4 && offset < length; i++, offset++)
        {
          word |= (uint32_t)buffer[offset] << (i * 8);
        }

      rk3576_i3c_putreg(priv, RK3576_I3C_RX_TX_DATA_PORT, word);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i3c_pop_rx
 *
 * Description:
 *   Drain length bytes out of the RX FIFO.  Returns the number of bytes
 *   actually read, which may be short if the target ended the transfer
 *   early, or a negated errno on timeout.
 ****************************************************************************/

static int rk3576_i3c_pop_rx(struct rk3576_i3c_priv_s *priv,
                             uint8_t *buffer, uint16_t length)
{
  uint16_t offset = 0;
  uint32_t word;
  uint32_t status;
  int t;
  int i;

  while (offset < length)
    {
      for (t = 0; t < RK3576_I3C_POLL_LIMIT; t++)
        {
          status = rk3576_i3c_getreg(priv, RK3576_I3C_DATA_BUFFER_STATUS);
          if (((status & RK3576_I3C_DSTATUS_RX_LEVEL_MASK) >>
               RK3576_I3C_DSTATUS_RX_LEVEL_SHIFT) != 0)
            {
              break;
            }

          /* The response arrives once the command is complete.  If it is
           * already queued and the FIFO is empty, no more data is coming.
           */

          status = rk3576_i3c_getreg(priv, RK3576_I3C_QUEUE_STATUS_LEVEL);
          if (((status & RK3576_I3C_QSTATUS_RESP_LEVEL_MASK) >>
               RK3576_I3C_QSTATUS_RESP_LEVEL_SHIFT) != 0)
            {
              return offset;
            }
        }

      if (t >= RK3576_I3C_POLL_LIMIT)
        {
          return -ETIMEDOUT;
        }

      word = rk3576_i3c_getreg(priv, RK3576_I3C_RX_TX_DATA_PORT);
      for (i = 0; i < 4 && offset < length; i++, offset++)
        {
          buffer[offset] = (word >> (i * 8)) & 0xff;
        }
    }

  return offset;
}

/****************************************************************************
 * Name: rk3576_i3c_run
 *
 * Description:
 *   Queue one command (argument word followed by command word), move the
 *   payload and collect the response.
 *
 * Input Parameters:
 *   priv    - Controller state
 *   arg     - Transfer argument word (CMD_ATTR 1) or 0 when not needed
 *   cmd     - Transfer / address-assignment command word
 *   buffer  - Payload buffer, may be NULL when length is 0
 *   length  - Payload length in bytes
 *   read    - true if the payload is received rather than sent
 *   xferred - Receives the number of payload bytes moved, may be NULL
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 ****************************************************************************/

static int rk3576_i3c_run(struct rk3576_i3c_priv_s *priv, uint32_t arg,
                          uint32_t cmd, uint8_t *buffer, uint16_t length,
                          bool read, uint16_t *xferred)
{
  uint32_t resp = 0;
  uint32_t err;
  uint16_t moved = 0;
  int ret;

  /* Drop anything stale left by a previous failure. */

  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS, RK3576_I3C_INTR_ALL);

  if (arg != 0)
    {
      rk3576_i3c_putreg(priv, RK3576_I3C_COMMAND_QUEUE_PORT, arg);
    }

  rk3576_i3c_putreg(priv, RK3576_I3C_COMMAND_QUEUE_PORT, cmd);

  if (length != 0 && buffer != NULL)
    {
      if (read)
        {
          ret = rk3576_i3c_pop_rx(priv, buffer, length);
          if (ret < 0)
            {
              goto errout;
            }

          moved = (uint16_t)ret;
        }
      else
        {
          ret = rk3576_i3c_push_tx(priv, buffer, length);
          if (ret < 0 && ret != -EIO)
            {
              goto errout;
            }

          moved = length;
        }
    }

  ret = rk3576_i3c_wait_resp(priv, &resp);
  if (ret < 0)
    {
      goto errout;
    }

  err = (resp & RK3576_I3C_RESP_ERR_MASK) >> RK3576_I3C_RESP_ERR_SHIFT;
  ret = rk3576_i3c_resp_errno(err);
  if (ret < 0)
    {
      i3cerr("ERROR: I3C%d: response error %" PRIu32 "\n", priv->port, err);
      goto errout;
    }

  /* For reads the DL field reports how many bytes were received; for writes
   * it reports how many were left unsent.
   */

  if (read)
    {
      moved = (uint16_t)((resp & RK3576_I3C_RESP_DL_MASK) >>
                         RK3576_I3C_RESP_DL_SHIFT);
      if (moved > length)
        {
          moved = length;
        }
    }
  else
    {
      uint16_t left = (uint16_t)((resp & RK3576_I3C_RESP_DL_MASK) >>
                                 RK3576_I3C_RESP_DL_SHIFT);
      moved = (left <= length) ? length - left : 0;
    }

  if (xferred != NULL)
    {
      *xferred = moved;
    }

  return OK;

errout:

  /* Flush the queues and resume the state machine so the next transfer
   * starts from a known state.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_RESET_CTRL,
                    RK3576_I3C_RESET_CMD_QUE | RK3576_I3C_RESET_RESP_QUE |
                        RK3576_I3C_RESET_TX_FIFO |
                        RK3576_I3C_RESET_RX_FIFO);

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_CTRL,
                    rk3576_i3c_getreg(priv, RK3576_I3C_DEVICE_CTRL) |
                        RK3576_I3C_DEVCTRL_RESUME);

  if (xferred != NULL)
    {
      *xferred = 0;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_ccc_locked
 *
 * Description:
 *   CCC transfer worker; the caller must already hold priv->lock.
 ****************************************************************************/

static int rk3576_i3c_ccc_locked(struct rk3576_i3c_priv_s *priv,
                                 struct rk3576_i3c_ccc_s *ccc)
{
  uint32_t arg;
  uint32_t cmd;
  uint16_t moved = 0;
  int ret;

  arg = RK3576_I3C_CMD_ATTR_XFER_ARG |
        ((uint32_t)ccc->length << RK3576_I3C_XFER_ARG_DL_SHIFT);

  cmd = RK3576_I3C_CMD_ATTR_XFER_CMD | RK3576_I3C_XFER_CP |
        RK3576_I3C_XFER_ROC | RK3576_I3C_XFER_TOC |
        ((uint32_t)RK3576_I3C_TID_XFER << RK3576_I3C_XFER_TID_SHIFT) |
        ((uint32_t)ccc->id << RK3576_I3C_XFER_CMD_SHIFT);

  if (ccc->has_defbyte)
    {
      arg |= (uint32_t)ccc->defbyte << RK3576_I3C_XFER_ARG_DB_SHIFT;
      cmd |= RK3576_I3C_XFER_DBP;
    }

  /* Direct CCCs address one device through its DAT slot; broadcast CCCs
   * always use slot 0 as the index is ignored.
   */

  if (ccc->id >= RK3576_I3C_CCC_ENEC_D)
    {
      cmd |= (uint32_t)ccc->slot << RK3576_I3C_XFER_DEV_INDEX_SHIFT;
      if (ccc->read)
        {
          cmd |= RK3576_I3C_XFER_RNW;
        }
    }

  ret = rk3576_i3c_run(priv, arg, cmd, ccc->buffer, ccc->length, ccc->read,
                       &moved);
  if (ret == OK && ccc->read)
    {
      ccc->length = moved;
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_service_ibi
 *
 * Description:
 *   Drain the IBI queue and dispatch each event to the registered callback.
 *   Called from interrupt context.
 ****************************************************************************/

static void rk3576_i3c_service_ibi(struct rk3576_i3c_priv_s *priv)
{
  uint8_t payload[RK3576_I3C_IBI_PAYLOAD_MAX];
  uint32_t status;
  uint32_t level;
  uint32_t word;
  uint8_t dynaddr;
  uint8_t length;
  uint8_t i;
  int slot;

  for (; ; )
    {
      level = rk3576_i3c_getreg(priv, RK3576_I3C_QUEUE_STATUS_LEVEL);
      if (((level & RK3576_I3C_QSTATUS_IBI_STAT_MASK) >>
           RK3576_I3C_QSTATUS_IBI_STAT_SHIFT) == 0)
        {
          break;
        }

      status = rk3576_i3c_getreg(priv, RK3576_I3C_IBI_QUEUE_STATUS);

      length = (uint8_t)((status & RK3576_I3C_IBI_STS_DL_MASK) >>
                         RK3576_I3C_IBI_STS_DL_SHIFT);

      /* The IBI ID is (address << 1) | RnW. */

      dynaddr = (uint8_t)(((status & RK3576_I3C_IBI_STS_ID_MASK) >>
                           RK3576_I3C_IBI_STS_ID_SHIFT) >> 1);

      /* Payload words follow the status word in the same queue. */

      for (i = 0; i < length; i += 4)
        {
          word = rk3576_i3c_getreg(priv, RK3576_I3C_IBI_QUEUE_DATA);
          if (i < RK3576_I3C_IBI_PAYLOAD_MAX)
            {
              uint8_t n;

              for (n = 0; n < 4 && (i + n) < length &&
                          (i + n) < RK3576_I3C_IBI_PAYLOAD_MAX;
                   n++)
                {
                  payload[i + n] = (word >> (n * 8)) & 0xff;
                }
            }
        }

      if (length > RK3576_I3C_IBI_PAYLOAD_MAX)
        {
          length = RK3576_I3C_IBI_PAYLOAD_MAX;
        }

      if ((status & RK3576_I3C_IBI_STS_ERROR) != 0)
        {
          continue;
        }

      for (slot = 0; slot < priv->ndevs; slot++)
        {
          if (priv->devs[slot].dynaddr == dynaddr)
            {
              uint8_t idx = priv->devs[slot].slot;

              if (idx < RK3576_I3C_MAX_DEVS && priv->ibicb[idx] != NULL)
                {
                  priv->ibicb[idx](priv->ibiarg[idx], dynaddr, payload,
                                   length);
                }

              break;
            }
        }
    }
}

/****************************************************************************
 * Name: rk3576_i3c_interrupt
 *
 * Description:
 *   Controller interrupt handler.  Transfers are polled, so only the
 *   asynchronous events (IBI, hot join) are handled here.
 ****************************************************************************/

static int rk3576_i3c_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)arg;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status = rk3576_i3c_getreg(priv, RK3576_I3C_INTR_STATUS);
  status &= rk3576_i3c_getreg(priv, RK3576_I3C_INTR_SIGNAL_EN);
  if (status == 0)
    {
      return OK;
    }

  /* Acknowledge first: the IBI queue is drained below and new events must
   * not be lost.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS, status);

  if ((status & (RK3576_I3C_INTR_IBI_THLD |
                 RK3576_I3C_INTR_IBI_UPDATED)) != 0)
    {
      rk3576_i3c_service_ibi(priv);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i3c_clk_init
 *
 * Description:
 *   Bring up every clock the controller needs.  All clock handling for this
 *   driver lives here.
 ****************************************************************************/

static int rk3576_i3c_clk_init(struct rk3576_i3c_priv_s *priv)
{
  char name[32];
  struct clk_s *hclk;
  int ret;

  /* AHB bus interface clock */

  snprintf(name, sizeof(name), "hclk_i3c%d_en", priv->port);
  hclk = clk_get(name);
  if (hclk == NULL)
    {
      i3cerr("ERROR: I3C%d: failed to get %s\n", priv->port, name);
      return -ENODEV;
    }

  ret = clk_enable(hclk);
  if (ret < 0)
    {
      i3cerr("ERROR: I3C%d: failed to enable %s: %d\n", priv->port, name,
             ret);
      return ret;
    }

  /* Functional (core) clock — drives the SCL counters */

  snprintf(name, sizeof(name), "clk_i3c%d_en", priv->port);
  priv->fclk = clk_get(name);
  if (priv->fclk == NULL)
    {
      i3cerr("ERROR: I3C%d: failed to get %s\n", priv->port, name);
      return -ENODEV;
    }

  ret = clk_enable(priv->fclk);
  if (ret < 0)
    {
      i3cerr("ERROR: I3C%d: failed to enable %s: %d\n", priv->port, name,
             ret);
      return ret;
    }

  priv->fclk_hz = clk_get_rate(priv->fclk);
  if (priv->fclk_hz == 0)
    {
      i3cerr("ERROR: I3C%d: core clock rate reads back as 0\n", priv->port);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_i3c_hw_init
 *
 * Description:
 *   Reset the controller, discover the table geometry, program the bus
 *   timings and enable it in master mode.
 ****************************************************************************/

static int rk3576_i3c_hw_init(struct rk3576_i3c_priv_s *priv)
{
  uint32_t ptr;
  uint32_t depth;

  /* Full soft reset, then leave the controller disabled while we set up. */

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_CTRL, 0);
  rk3576_i3c_putreg(priv, RK3576_I3C_RESET_CTRL, RK3576_I3C_RESET_ALL);
  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS, RK3576_I3C_INTR_ALL);
  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_SIGNAL_EN, 0);

  /* Master mode */

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_CTRL_EXTENDED,
                    RK3576_I3C_DEVCTRL_EXT_MODE_MASTER);

  /* Locate the device address / characteristics tables. */

  ptr = rk3576_i3c_getreg(priv, RK3576_I3C_DEV_ADDR_TABLE_PTR);
  priv->dat = priv->base +
              ((ptr & RK3576_I3C_DAT_PTR_ADDR_MASK) >>
               RK3576_I3C_DAT_PTR_ADDR_SHIFT);

  depth = (ptr & RK3576_I3C_DAT_PTR_DEPTH_MASK) >>
          RK3576_I3C_DAT_PTR_DEPTH_SHIFT;

  if (priv->dat == priv->base || depth == 0)
    {
      /* Pointer register not implemented — fall back to the reset default
       * documented for the DesignWare block.
       */

      priv->dat = priv->base + RK3576_I3C_DEV_ADDR_TABLE_LOC;
      depth = RK3576_I3C_MAX_DEVS;
    }

  if (depth > RK3576_I3C_MAX_DEVS)
    {
      depth = RK3576_I3C_MAX_DEVS;
    }

  priv->datdepth = (uint8_t)depth;

  ptr = rk3576_i3c_getreg(priv, RK3576_I3C_DEV_CHAR_TABLE_PTR);
  priv->dct = priv->base +
              ((ptr & RK3576_I3C_DAT_PTR_ADDR_MASK) >>
               RK3576_I3C_DAT_PTR_ADDR_SHIFT);

  /* The controller itself owns address 0x08 on the bus so that it can be
   * targeted by secondary masters.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_ADDR,
                    RK3576_I3C_DEVADDR_DYNAMIC_VALID |
                        ((uint32_t)RK3576_I3C_DYNADDR_BASE
                         << RK3576_I3C_DEVADDR_DYNAMIC_SHIFT));

  /* Interrupt every time a single entry lands in a queue, so the polling
   * loops observe progress as early as possible.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_QUEUE_THLD_CTRL, 0);
  rk3576_i3c_putreg(priv, RK3576_I3C_DATA_BUFFER_THLD_CTRL, 0);

  /* Reject master-request and slave-interrupt events until a device is
   * explicitly opted in through rk3576_i3c_ibi_enable().
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_IBI_MR_REQ_REJECT, 0xffffffff);
  rk3576_i3c_putreg(priv, RK3576_I3C_IBI_SIR_REQ_REJECT, 0xffffffff);

  rk3576_i3c_i3c_timings(priv);
  rk3576_i3c_i2c_timings(priv, RK3576_I3C_I2C_FM_HZ);

  /* Legacy I2C targets share the bus, so the controller must emit the
   * broadcast address in open drain and tolerate I2C-only devices.
   */

  rk3576_i3c_putreg(priv, RK3576_I3C_DEVICE_CTRL,
                    RK3576_I3C_DEVCTRL_IBA_INCLUDE |
                        RK3576_I3C_DEVCTRL_I2C_SLAVE_PRESENT |
                        RK3576_I3C_DEVCTRL_HOT_JOIN_NACK |
                        RK3576_I3C_DEVCTRL_ENABLE);

  return OK;
}

/****************************************************************************
 * Name: rk3576_i3c_transfer
 *
 * Description:
 *   struct i2c_ops_s transfer method: run a list of legacy I2C messages
 *   against targets on the I3C bus.
 ****************************************************************************/

static int rk3576_i3c_transfer(struct i2c_master_s *dev,
                               struct i2c_msg_s *msgs, int count)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  struct i2c_msg_s *msg;
  uint32_t arg;
  uint32_t cmd;
  uint32_t speed;
  int ret = OK;
  int i;

  DEBUGASSERT(priv != NULL && msgs != NULL);

  if (count <= 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  for (i = 0; i < count; i++)
    {
      msg = &msgs[i];

      if (msg->length == 0 || msg->buffer == NULL)
        {
          ret = -EINVAL;
          break;
        }

      if ((msg->flags & I2C_M_TEN) != 0)
        {
          /* The controller's device address table only holds 7-bit static
           * addresses.
           */

          ret = -ENOTSUP;
          break;
        }

      rk3576_i3c_i2c_timings(priv, msg->frequency);

      speed = (msg->frequency > RK3576_I3C_I2C_FM_HZ) ?
                  RK3576_I3C_XFER_SPEED_I2C_FMP :
                  RK3576_I3C_XFER_SPEED_I2C_FM;

      /* Point the legacy slot at this target. */

      rk3576_i3c_set_dat(priv, RK3576_I3C_I2C_DAT_SLOT,
                         RK3576_I3C_DAT_LEGACY_I2C_DEVICE |
                             (((uint32_t)msg->addr &
                               RK3576_I3C_DAT_STATIC_ADDR_MASK)
                              << RK3576_I3C_DAT_STATIC_ADDR_SHIFT));

      arg = RK3576_I3C_CMD_ATTR_XFER_ARG |
            ((uint32_t)msg->length << RK3576_I3C_XFER_ARG_DL_SHIFT);

      cmd = RK3576_I3C_CMD_ATTR_XFER_CMD | RK3576_I3C_XFER_ROC | speed |
            ((uint32_t)RK3576_I3C_TID_XFER << RK3576_I3C_XFER_TID_SHIFT) |
            ((uint32_t)RK3576_I3C_I2C_DAT_SLOT
             << RK3576_I3C_XFER_DEV_INDEX_SHIFT);

      if ((msg->flags & I2C_M_READ) != 0)
        {
          cmd |= RK3576_I3C_XFER_RNW;
        }

      /* Terminate with a STOP unless the caller asked for a repeated START
       * and another message follows.
       */

      if ((msg->flags & I2C_M_NOSTOP) == 0 || i == count - 1)
        {
          cmd |= RK3576_I3C_XFER_TOC;
        }

      ret = rk3576_i3c_run(priv, arg, cmd, msg->buffer, msg->length,
                           (msg->flags & I2C_M_READ) != 0, NULL);
      if (ret < 0)
        {
          break;
        }
    }

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_reset
 *
 * Description:
 *   struct i2c_ops_s reset method: flush the controller queues and restart
 *   the bus state machine after a stuck transfer.
 ****************************************************************************/

#ifdef CONFIG_I2C_RESET
static int rk3576_i3c_reset(struct i2c_master_s *dev)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;

  DEBUGASSERT(priv != NULL);

  nxmutex_lock(&priv->lock);

  rk3576_i3c_enable(priv, false);
  rk3576_i3c_putreg(priv, RK3576_I3C_RESET_CTRL, RK3576_I3C_RESET_ALL);
  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS, RK3576_I3C_INTR_ALL);
  priv->i2c_freq = 0;
  rk3576_i3c_i3c_timings(priv);
  rk3576_i3c_enable(priv, true);

  nxmutex_unlock(&priv->lock);
  return OK;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_i3c_initialize
 ****************************************************************************/

struct i2c_master_s *rk3576_i3c_initialize(int port)
{
  struct rk3576_i3c_priv_s *priv;
  int ret;

  if (port < 0 || port >= RK3576_I3C_NUM)
    {
      i3cerr("ERROR: unsupported I3C port %d\n", port);
      return NULL;
    }

  priv = &g_rk3576_i3c[port];
  if (priv->inited)
    {
      return &priv->dev;
    }

  priv->dev.ops = &g_rk3576_i3c_ops;
  priv->base = g_rk3576_i3c_base[port];
  priv->irq = g_rk3576_i3c_irq[port];
  priv->port = port;
  priv->ndevs = 0;
  priv->i2c_freq = 0;

  ret = rk3576_i3c_clk_init(priv);
  if (ret < 0)
    {
      return NULL;
    }

  ret = rk3576_i3c_hw_init(priv);
  if (ret < 0)
    {
      i3cerr("ERROR: I3C%d: controller init failed: %d\n", port, ret);
      return NULL;
    }

  nxmutex_init(&priv->lock);

  ret = irq_attach(priv->irq, rk3576_i3c_interrupt, priv);
  if (ret < 0)
    {
      i3cerr("ERROR: I3C%d: failed to attach IRQ %d: %d\n", port, priv->irq,
             ret);
      nxmutex_destroy(&priv->lock);
      rk3576_i3c_enable(priv, false);
      return NULL;
    }

  up_enable_irq(priv->irq);

  priv->inited = true;

  i3cinfo("I3C%d ready at 0x%" PRIxPTR ", core clock %" PRIu32 " Hz, "
          "%u DAT slots\n",
          port, priv->base, priv->fclk_hz, priv->datdepth);

  return &priv->dev;
}

/****************************************************************************
 * Name: rk3576_i3c_uninitialize
 ****************************************************************************/

int rk3576_i3c_uninitialize(struct i2c_master_s *dev)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;

  if (priv == NULL || !priv->inited)
    {
      return -EINVAL;
    }

  up_disable_irq(priv->irq);
  irq_detach(priv->irq);

  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_SIGNAL_EN, 0);
  rk3576_i3c_enable(priv, false);

  nxmutex_destroy(&priv->lock);
  priv->inited = false;

  return OK;
}

/****************************************************************************
 * Name: rk3576_i3c_send_ccc
 ****************************************************************************/

int rk3576_i3c_send_ccc(struct i2c_master_s *dev,
                        struct rk3576_i3c_ccc_s *ccc)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  int ret;

  if (priv == NULL || !priv->inited || ccc == NULL)
    {
      return -EINVAL;
    }

  if (ccc->length != 0 && ccc->buffer == NULL)
    {
      return -EINVAL;
    }

  if (ccc->id >= RK3576_I3C_CCC_ENEC_D && ccc->slot >= priv->datdepth)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);
  ret = rk3576_i3c_ccc_locked(priv, ccc);
  nxmutex_unlock(&priv->lock);

  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_do_daa
 ****************************************************************************/

int rk3576_i3c_do_daa(struct i2c_master_s *dev)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  struct rk3576_i3c_ccc_s ccc;
  uint32_t cmd;
  uint32_t resp = 0;
  uint32_t err;
  uint8_t slot;
  uint8_t nslots;
  uint8_t assigned;
  uint8_t left;
  int ret;
  int i;

  if (priv == NULL || !priv->inited)
    {
      return -EINVAL;
    }

  if (priv->datdepth <= RK3576_I3C_FIRST_I3C_SLOT)
    {
      return -ENOSPC;
    }

  nxmutex_lock(&priv->lock);

  /* Start from a clean slate: every target drops its dynamic address. */

  memset(&ccc, 0, sizeof(ccc));
  ccc.id = RK3576_I3C_CCC_RSTDAA_B;
  ret = rk3576_i3c_ccc_locked(priv, &ccc);
  if (ret < 0 && ret != -ENXIO)
    {
      /* -ENXIO simply means nobody answered the broadcast; anything else is
       * a real failure.
       */

      goto errout;
    }

  priv->ndevs = 0;
  memset(priv->devs, 0, sizeof(priv->devs));

  /* Pre-load the free DAT slots with the addresses we are willing to hand
   * out.  The controller consumes one slot per device that answers.
   */

  nslots = priv->datdepth - RK3576_I3C_FIRST_I3C_SLOT;

  for (i = 0; i < nslots; i++)
    {
      uint8_t addr = RK3576_I3C_DYNADDR_BASE + 1 + i;
      uint32_t entry;

      slot = RK3576_I3C_FIRST_I3C_SLOT + i;
      entry = ((uint32_t)(addr | (rk3576_i3c_addr_parity(addr) << 7))
               << RK3576_I3C_DAT_DYNAMIC_ADDR_SHIFT) |
              RK3576_I3C_DAT_SIR_REJECT | RK3576_I3C_DAT_MR_REJECT;

      rk3576_i3c_set_dat(priv, slot, entry);
    }

  cmd = RK3576_I3C_CMD_ATTR_ADDR_ASSGN | RK3576_I3C_ADDR_ASSGN_ROC |
        RK3576_I3C_ADDR_ASSGN_TOC |
        ((uint32_t)RK3576_I3C_TID_DAA << RK3576_I3C_ADDR_ASSGN_TID_SHIFT) |
        ((uint32_t)RK3576_I3C_CCC_ENTDAA << RK3576_I3C_ADDR_ASSGN_CMD_SHIFT) |
        ((uint32_t)RK3576_I3C_FIRST_I3C_SLOT
         << RK3576_I3C_ADDR_ASSGN_DEV_INDEX_SHIFT) |
        ((uint32_t)nslots << RK3576_I3C_ADDR_ASSGN_DEV_COUNT_SHIFT);

  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS, RK3576_I3C_INTR_ALL);
  rk3576_i3c_putreg(priv, RK3576_I3C_COMMAND_QUEUE_PORT, cmd);

  ret = rk3576_i3c_wait_resp(priv, &resp);
  if (ret < 0)
    {
      goto errout;
    }

  err = (resp & RK3576_I3C_RESP_ERR_MASK) >> RK3576_I3C_RESP_ERR_SHIFT;

  /* ENTDAA ends with a NACK once no device answers the broadcast any more,
   * which is the normal end of enumeration rather than a failure.
   */

  if (err != RK3576_I3C_RESP_ERR_NONE &&
      err != RK3576_I3C_RESP_ERR_ADDR_NACK)
    {
      ret = rk3576_i3c_resp_errno(err);
      goto errout;
    }

  /* The DL field reports how many of the offered slots were left unused. */

  left = (uint8_t)((resp & RK3576_I3C_RESP_DL_MASK) >>
                   RK3576_I3C_RESP_DL_SHIFT);
  assigned = (left <= nslots) ? nslots - left : 0;

  /* Read the provisional ID and characteristics the controller collected
   * into the device characteristics table.
   */

  for (i = 0; i < assigned && i < RK3576_I3C_MAX_DEVS; i++)
    {
      uintptr_t entry = priv->dct +
                        ((uintptr_t)i * RK3576_I3C_DCT_ENTRY_SIZE);
      uint32_t pidmsb;
      uint32_t pidlsb;
      uint32_t chars;
      uint8_t addr;

      slot = RK3576_I3C_FIRST_I3C_SLOT + i;
      addr = RK3576_I3C_DYNADDR_BASE + 1 + i;

      pidmsb = getreg32(entry + RK3576_I3C_DCT_PID_MSB);
      pidlsb = getreg32(entry + RK3576_I3C_DCT_PID_LSB);
      chars = getreg32(entry + RK3576_I3C_DCT_CHAR);

      priv->devs[i].slot = slot;
      priv->devs[i].dynaddr = addr;
      priv->devs[i].dcr = (uint8_t)(chars & 0xff);
      priv->devs[i].bcr = (uint8_t)((chars >> 8) & 0xff);
      priv->devs[i].pid = ((uint64_t)pidmsb << RK3576_I3C_PID_LSB_BITS) |
                          (pidlsb & 0xffff);

      i3cinfo("I3C%d: device %d at 0x%02x, BCR 0x%02x DCR 0x%02x\n",
              priv->port, i, addr, priv->devs[i].bcr, priv->devs[i].dcr);
    }

  priv->ndevs = i;
  ret = priv->ndevs;

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_get_devices
 ****************************************************************************/

int rk3576_i3c_get_devices(struct i2c_master_s *dev,
                           struct rk3576_i3c_devinfo_s *info, int ndevs)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  int count;

  if (priv == NULL || !priv->inited || info == NULL || ndevs <= 0)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  count = (priv->ndevs < ndevs) ? priv->ndevs : ndevs;
  memcpy(info, priv->devs, (size_t)count * sizeof(*info));

  nxmutex_unlock(&priv->lock);
  return count;
}

/****************************************************************************
 * Name: rk3576_i3c_ibi_enable
 ****************************************************************************/

int rk3576_i3c_ibi_enable(struct i2c_master_s *dev, uint8_t slot,
                          bool withdata, rk3576_i3c_ibi_cb_t cb, void *arg)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  struct rk3576_i3c_ccc_s ccc;
  uint8_t event = RK3576_I3C_EVENT_SIR;
  uint32_t entry;
  uint32_t reject;
  int ret;

  if (priv == NULL || !priv->inited || cb == NULL ||
      slot >= priv->datdepth || slot >= RK3576_I3C_MAX_DEVS)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  priv->ibicb[slot] = cb;
  priv->ibiarg[slot] = arg;

  /* Stop rejecting this device's slave interrupt requests. */

  reject = rk3576_i3c_getreg(priv, RK3576_I3C_IBI_SIR_REQ_REJECT);
  reject &= ~(1u << slot);
  rk3576_i3c_putreg(priv, RK3576_I3C_IBI_SIR_REQ_REJECT, reject);

  entry = getreg32(priv->dat + ((uintptr_t)slot * 4));
  entry &= ~RK3576_I3C_DAT_SIR_REJECT;
  if (withdata)
    {
      entry |= RK3576_I3C_DAT_IBI_WITH_DATA;
    }
  else
    {
      entry &= ~RK3576_I3C_DAT_IBI_WITH_DATA;
    }

  rk3576_i3c_set_dat(priv, slot, entry);

  /* Route the IBI queue interrupt to the CPU. */

  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_STATUS_EN,
                    rk3576_i3c_getreg(priv, RK3576_I3C_INTR_STATUS_EN) |
                        RK3576_I3C_INTR_IBI_THLD |
                        RK3576_I3C_INTR_IBI_UPDATED);
  rk3576_i3c_putreg(priv, RK3576_I3C_INTR_SIGNAL_EN,
                    rk3576_i3c_getreg(priv, RK3576_I3C_INTR_SIGNAL_EN) |
                        RK3576_I3C_INTR_IBI_THLD |
                        RK3576_I3C_INTR_IBI_UPDATED);

  /* Finally let the device know it may signal. */

  memset(&ccc, 0, sizeof(ccc));
  ccc.id = RK3576_I3C_CCC_ENEC_D;
  ccc.slot = slot;
  ccc.buffer = &event;
  ccc.length = 1;

  ret = rk3576_i3c_ccc_locked(priv, &ccc);

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_ibi_disable
 ****************************************************************************/

int rk3576_i3c_ibi_disable(struct i2c_master_s *dev, uint8_t slot)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  struct rk3576_i3c_ccc_s ccc;
  uint8_t event = RK3576_I3C_EVENT_SIR;
  uint32_t entry;
  int ret;

  if (priv == NULL || !priv->inited || slot >= priv->datdepth ||
      slot >= RK3576_I3C_MAX_DEVS)
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);

  memset(&ccc, 0, sizeof(ccc));
  ccc.id = RK3576_I3C_CCC_DISEC_D;
  ccc.slot = slot;
  ccc.buffer = &event;
  ccc.length = 1;

  ret = rk3576_i3c_ccc_locked(priv, &ccc);

  rk3576_i3c_putreg(priv, RK3576_I3C_IBI_SIR_REQ_REJECT,
                    rk3576_i3c_getreg(priv, RK3576_I3C_IBI_SIR_REQ_REJECT) |
                        (1u << slot));

  entry = getreg32(priv->dat + ((uintptr_t)slot * 4));
  entry |= RK3576_I3C_DAT_SIR_REJECT;
  rk3576_i3c_set_dat(priv, slot, entry);

  priv->ibicb[slot] = NULL;
  priv->ibiarg[slot] = NULL;

  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_i3c_priv_transfer
 ****************************************************************************/

int rk3576_i3c_priv_transfer(struct i2c_master_s *dev, uint8_t slot,
                             bool read, uint8_t *buffer, uint16_t length)
{
  struct rk3576_i3c_priv_s *priv = (struct rk3576_i3c_priv_s *)dev;
  uint32_t arg;
  uint32_t cmd;
  uint16_t moved = 0;
  int ret;

  if (priv == NULL || !priv->inited || buffer == NULL || length == 0 ||
      slot >= priv->datdepth)
    {
      return -EINVAL;
    }

  arg = RK3576_I3C_CMD_ATTR_XFER_ARG |
        ((uint32_t)length << RK3576_I3C_XFER_ARG_DL_SHIFT);

  cmd = RK3576_I3C_CMD_ATTR_XFER_CMD | RK3576_I3C_XFER_ROC |
        RK3576_I3C_XFER_TOC | RK3576_I3C_XFER_SPEED_I3C_SDR0 |
        ((uint32_t)RK3576_I3C_TID_XFER << RK3576_I3C_XFER_TID_SHIFT) |
        ((uint32_t)slot << RK3576_I3C_XFER_DEV_INDEX_SHIFT);

  if (read)
    {
      cmd |= RK3576_I3C_XFER_RNW;
    }

  nxmutex_lock(&priv->lock);
  ret = rk3576_i3c_run(priv, arg, cmd, buffer, length, read, &moved);
  nxmutex_unlock(&priv->lock);

  return (ret < 0) ? ret : (int)moved;
}

#endif /* CONFIG_RK3576_I3C */
