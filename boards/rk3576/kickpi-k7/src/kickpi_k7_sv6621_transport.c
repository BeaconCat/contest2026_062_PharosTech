/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_sv6621_transport.c
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

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "kickpi_k7_sv6621_transport.h"
#include "rk3576_sdmmc.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_SV6621_MSHC_BASE      0x2a320000
#define RK3576_SV6621_CTRL           (RK3576_SV6621_MSHC_BASE + 0x000)
#define RK3576_SV6621_CLKDIV         (RK3576_SV6621_MSHC_BASE + 0x008)
#define RK3576_SV6621_CLKENA         (RK3576_SV6621_MSHC_BASE + 0x010)
#define RK3576_SV6621_CTYPE          (RK3576_SV6621_MSHC_BASE + 0x018)
#define RK3576_SV6621_BLKSIZ         (RK3576_SV6621_MSHC_BASE + 0x01c)
#define RK3576_SV6621_BYTCNT         (RK3576_SV6621_MSHC_BASE + 0x020)
#define RK3576_SV6621_INTMASK        (RK3576_SV6621_MSHC_BASE + 0x024)
#define RK3576_SV6621_CMDARG         (RK3576_SV6621_MSHC_BASE + 0x028)
#define RK3576_SV6621_CMD            (RK3576_SV6621_MSHC_BASE + 0x02c)
#define RK3576_SV6621_RESP0          (RK3576_SV6621_MSHC_BASE + 0x030)
#define RK3576_SV6621_RINTSTS        (RK3576_SV6621_MSHC_BASE + 0x044)
#define RK3576_SV6621_STATUS         (RK3576_SV6621_MSHC_BASE + 0x048)
#define RK3576_SV6621_UHS            (RK3576_SV6621_MSHC_BASE + 0x074)
#define RK3576_SV6621_TIMING0        (RK3576_SV6621_MSHC_BASE + 0x130)
#define RK3576_SV6621_TIMING1        (RK3576_SV6621_MSHC_BASE + 0x134)
#define RK3576_SV6621_FIFO           (RK3576_SV6621_MSHC_BASE + 0x200)

#define RK3576_SV6621_CRU_SDIO_SEL   0x272004a0

#define RK3576_SV6621_CMD_START      (1u << 31)
#define RK3576_SV6621_CMD_STOP_ABORT (1u << 14)
#define RK3576_SV6621_CMD3           0xa0000143
#define RK3576_SV6621_CMD5           0xa0000045
#define RK3576_SV6621_CMD7           0xa0000147
#define RK3576_SV6621_CMD11          0xb000014b
#define RK3576_SV6621_CMD19          0xa0002353
#define RK3576_SV6621_CMD52          0xa0000174
#define RK3576_SV6621_CMD53_READ     0xa0002375
#define RK3576_SV6621_CMD53_WRITE    0xa0002775

#define RK3576_SV6621_INT_CMDDONE    (1u << 2)
#define RK3576_SV6621_INT_DTO        (1u << 3)
#define RK3576_SV6621_INT_RTO        (1u << 8)
#define RK3576_SV6621_INT_DRTO       (1u << 9)
#define RK3576_SV6621_INT_HTO        (1u << 10)
#define RK3576_SV6621_INT_VOLTSW     (1u << 10)
#define RK3576_SV6621_INT_SDIO       (1u << 24)
#define RK3576_SV6621_INT_SYNC_ALL   (UINT32_MAX & ~RK3576_SV6621_INT_SDIO)
#define RK3576_SV6621_INT_CMDERR     0x00001142
#define RK3576_SV6621_INT_DATAERR    0x0000ae80
#define RK3576_SV6621_INT_TIMEOUT \
  (RK3576_SV6621_INT_RTO | RK3576_SV6621_INT_DRTO | RK3576_SV6621_INT_HTO)

#define RK3576_SV6621_CLK_UPDATE   0x80202000
#define RK3576_SV6621_CLK_UPD_VOLT 0x90202000
#define RK3576_SV6621_SRC_396M     0x2f02
#define RK3576_SV6621_TCON(raw) \
  (((uint32_t)0x7ff << 1 << 16) | ((uint32_t)(raw) << 1))
#define RK3576_SV6621_TCON_180          RK3576_SV6621_TCON(0x2)
#define RK3576_SV6621_PHASE_COUNT       4
#define RK3576_SV6621_PHASE_STEP        90
#define RK3576_SV6621_POLL_LIMIT        200000
#define RK3576_SV6621_FUNCTION_MAX      7
#define RK3576_SV6621_ADDRESS_MAX       0x1ffff
#define RK3576_SV6621_BLOCK_SIZE        512
#define RK3576_SV6621_BYTE_COUNT_MAX    512
#define RK3576_SV6621_BLOCK_COUNT_MAX   512
#define RK3576_SV6621_TUNING_BLOCK_SIZE 64
#define RK3576_SV6621_DMA_BOUNCE_SIZE   (16 * 1024)
#define RK3576_SV6621_CMD53_TIMEOUT_MS  1000

#define RK3576_SV6621_R4_FUNCTIONS_MASK (7u << 28)
#define RK3576_SV6621_R6_ERROR_MASK     (7u << 13)
#define RK3576_SV6621_R1_ERROR_MASK     0xfff9a088

#define RK3576_SV6621_CCCR_IO_ENABLE    0x02
#define RK3576_SV6621_CCCR_IO_READY     0x03
#define RK3576_SV6621_CCCR_INTERRUPT    0x04
#define RK3576_SV6621_CCCR_INT_PENDING  0x05
#define RK3576_SV6621_CCCR_ABORT        0x06
#define RK3576_SV6621_CCCR_BUS_IF       0x07
#define RK3576_SV6621_CCCR_SPEED        0x13
#define RK3576_SV6621_FBR1_BLOCK_LOW    0x110
#define RK3576_SV6621_FBR1_BLOCK_HIGH   0x111

#define RK3576_SV6621_FUNCTION1_BIT     (1 << 1)
#define RK3576_SV6621_INTERRUPT_MASTER  (1 << 0)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_sv6621_transport_priv_s
{
  FAR struct sdio_dev_s *sdio;
  mutex_t lock;
  sv6621_transport_irq_t handler;
  FAR void *handler_arg;
  bool prepared;
  bool opened;
  bool host_irq_attached;
  bool irq_enabled;
  uint8_t sample_phase;
  bool sample_phase_valid;
};

static const uint8_t g_rk3576_sv6621_tuning_pattern[] = {
  0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc, 0xc3, 0x3c, 0xcc, 0xff, 0xfe,
  0xff, 0xfe, 0xef, 0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb, 0xbf, 0xff,
  0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef, 0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc,
  0x3c, 0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee, 0xff, 0xfd, 0xff, 0xfd,
  0xdf, 0xff, 0xbf, 0xff, 0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde,
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_sv6621_ciu_update(uint32_t command);
static int rk3576_sv6621_set_clock(uint32_t source, uint32_t divider);
static uint32_t rk3576_sv6621_command(uint32_t command, uint32_t argument,
                                      FAR uint32_t *response);
static int rk3576_sv6621_direct(bool write, uint8_t function, uint32_t address,
                                uint8_t value, FAR uint8_t *result);
static int rk3576_sv6621_voltage_switch(void);
static int rk3576_sv6621_execute_tuning(void);
static int
rk3576_sv6621_tune_sdr104(FAR struct rk3576_sv6621_transport_priv_s *priv);
static int
rk3576_sv6621_open_failed(FAR struct rk3576_sv6621_transport_priv_s *priv,
                          int error);
static int rk3576_sv6621_open(FAR struct sv6621_transport_s *transport);
static int rk3576_sv6621_enumerate(FAR struct sv6621_transport_s *transport);
static void rk3576_sv6621_close(FAR struct sv6621_transport_s *transport);
static int rk3576_sv6621_read_byte(FAR struct sv6621_transport_s *transport,
                                   uint8_t function, uint32_t address,
                                   FAR uint8_t *value);
static int rk3576_sv6621_write_byte(FAR struct sv6621_transport_s *transport,
                                    uint8_t function, uint32_t address,
                                    uint8_t value);
static int
rk3576_sv6621_extended(FAR struct rk3576_sv6621_transport_priv_s *priv,
                       bool write, uint8_t function, uint32_t address,
                       bool increment, FAR void *buffer, size_t length);
static int rk3576_sv6621_read(FAR struct sv6621_transport_s *transport,
                              uint8_t function, uint32_t address,
                              bool increment, FAR void *buffer, size_t length);
static int rk3576_sv6621_write(FAR struct sv6621_transport_s *transport,
                               uint8_t function, uint32_t address,
                               bool increment, FAR const void *buffer,
                               size_t length);
static int rk3576_sv6621_attach_irq(FAR struct sv6621_transport_s *transport,
                                    sv6621_transport_irq_t handler,
                                    FAR void *arg);
static int rk3576_sv6621_enable_irq(FAR struct sv6621_transport_s *transport,
                                    bool enable);
static int rk3576_sv6621_ack_irq(FAR struct sv6621_transport_s *transport);
static void rk3576_sv6621_host_interrupt(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_sv6621_transport_priv_s g_rk3576_sv6621_priv = {
  .lock = NXMUTEX_INITIALIZER,
};

static uint8_t
    g_rk3576_sv6621_dma_bounce[RK3576_SV6621_DMA_BOUNCE_SIZE] aligned_data(64);

static const struct sv6621_transport_ops_s g_rk3576_sv6621_ops = {
  .open = rk3576_sv6621_open,
  .enumerate = rk3576_sv6621_enumerate,
  .close = rk3576_sv6621_close,
  .read_byte = rk3576_sv6621_read_byte,
  .write_byte = rk3576_sv6621_write_byte,
  .read = rk3576_sv6621_read,
  .write = rk3576_sv6621_write,
  .attach_irq = rk3576_sv6621_attach_irq,
  .enable_irq = rk3576_sv6621_enable_irq,
  .ack_irq = rk3576_sv6621_ack_irq,
};

static struct sv6621_transport_s g_rk3576_sv6621_transport = {
  .ops = &g_rk3576_sv6621_ops,
  .priv = &g_rk3576_sv6621_priv,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int rk3576_sv6621_ciu_update(uint32_t command)
{
  int index;

  putreg32(command, RK3576_SV6621_CMD);
  for (index = 0;
       (getreg32(RK3576_SV6621_CMD) & RK3576_SV6621_CMD_START) != 0 &&
       index < RK3576_SV6621_POLL_LIMIT;
       index++)
    ;

  return index == RK3576_SV6621_POLL_LIMIT ? -ETIMEDOUT : 0;
}

static int rk3576_sv6621_set_clock(uint32_t source, uint32_t divider)
{
  int ret;

  putreg32(0, RK3576_SV6621_CLKENA);
  ret = rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  if (ret < 0)
    {
      return ret;
    }

  putreg32((0x3fffu << 16) | (source & 0x3fff), RK3576_SV6621_CRU_SDIO_SEL);
  putreg32(divider, RK3576_SV6621_CLKDIV);
  putreg32(1, RK3576_SV6621_CLKENA);
  ret = rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(2);
  return 0;
}

static uint32_t rk3576_sv6621_command(uint32_t command, uint32_t argument,
                                      FAR uint32_t *response)
{
  uint32_t completion = RK3576_SV6621_INT_CMDDONE | RK3576_SV6621_INT_CMDERR;
  uint32_t status;
  int index;

  if (command == RK3576_SV6621_CMD11)
    {
      completion |= RK3576_SV6621_INT_VOLTSW;
    }

  for (index = 0; (getreg32(RK3576_SV6621_STATUS) & (1u << 9)) != 0 &&
                  index < RK3576_SV6621_POLL_LIMIT;
       index++)
    {
      up_udelay(5);
    }

  if (index == RK3576_SV6621_POLL_LIMIT)
    {
      return RK3576_SV6621_INT_RTO;
    }

  putreg32(RK3576_SV6621_INT_SYNC_ALL, RK3576_SV6621_RINTSTS);
  putreg32(argument, RK3576_SV6621_CMDARG);
  putreg32(command, RK3576_SV6621_CMD);

  for (index = 0;
       (getreg32(RK3576_SV6621_CMD) & RK3576_SV6621_CMD_START) != 0 &&
       index < RK3576_SV6621_POLL_LIMIT;
       index++)
    ;

  if (index == RK3576_SV6621_POLL_LIMIT)
    {
      return RK3576_SV6621_INT_RTO;
    }

  for (index = 0; index < RK3576_SV6621_POLL_LIMIT; index++)
    {
      status = getreg32(RK3576_SV6621_RINTSTS);
      if ((status & completion) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  if (index == RK3576_SV6621_POLL_LIMIT)
    {
      return RK3576_SV6621_INT_RTO;
    }

  if (response != NULL)
    {
      *response = getreg32(RK3576_SV6621_RESP0);
    }

  return getreg32(RK3576_SV6621_RINTSTS);
}

static int rk3576_sv6621_direct(bool write, uint8_t function, uint32_t address,
                                uint8_t value, FAR uint8_t *result)
{
  uint32_t argument;
  uint32_t response = 0;
  uint32_t status;
  int ret;

  argument = (write ? (1u << 31) : 0) | ((function & 7) << 28) |
             (write ? (1u << 27) : 0) | ((address & 0x1ffff) << 9) |
             (write ? value : 0);

  ret = nxmutex_lock(&g_rk3576_sv6621_priv.lock);
  if (ret < 0)
    {
      return ret;
    }

  status = rk3576_sv6621_command(RK3576_SV6621_CMD52, argument, &response);
  nxmutex_unlock(&g_rk3576_sv6621_priv.lock);

  if ((status & RK3576_SV6621_INT_CMDERR) != 0)
    {
      return (status & RK3576_SV6621_INT_RTO) != 0 ? -ETIMEDOUT : -EIO;
    }

  if (result != NULL)
    {
      *result = response & 0xff;
    }

  return ((response >> 8) & 0xcb) != 0 ? -EIO : OK;
}

static int rk3576_sv6621_voltage_switch(void)
{
  uint32_t response;
  uint32_t status;
  int index;

  status = rk3576_sv6621_command(RK3576_SV6621_CMD11, 0, &response);
  if ((status & RK3576_SV6621_INT_CMDERR) != 0)
    {
      return (status & RK3576_SV6621_INT_RTO) != 0 ? -ETIMEDOUT : -EIO;
    }

  for (index = 0; index < 100000; index++)
    {
      if ((getreg32(RK3576_SV6621_RINTSTS) & RK3576_SV6621_INT_VOLTSW) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  if (index == 100000)
    {
      return -ETIMEDOUT;
    }

  putreg32(RK3576_SV6621_INT_VOLTSW, RK3576_SV6621_RINTSTS);
  putreg32(0, RK3576_SV6621_CLKENA);
  if (rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPD_VOLT) < 0)
    {
      return -ETIMEDOUT;
    }

  putreg32(getreg32(RK3576_SV6621_UHS) | 1, RK3576_SV6621_UHS);
  up_mdelay(10);
  putreg32(1, RK3576_SV6621_CLKENA);
  if (rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPD_VOLT) < 0)
    {
      return -ETIMEDOUT;
    }

  for (index = 0; index < 200000; index++)
    {
      if ((getreg32(RK3576_SV6621_RINTSTS) & RK3576_SV6621_INT_VOLTSW) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  if (index == 200000)
    {
      return -ETIMEDOUT;
    }

  putreg32(RK3576_SV6621_INT_VOLTSW, RK3576_SV6621_RINTSTS);
  up_mdelay(5);
  return 0;
}

static int rk3576_sv6621_execute_tuning(void)
{
  uint8_t tuning_block[RK3576_SV6621_TUNING_BLOCK_SIZE];
  uint32_t response;
  uint32_t fifo_count;
  uint32_t status;
  uint32_t word;
  size_t received = 0;
  int index;

  putreg32(getreg32(RK3576_SV6621_CTRL) | (1u << 1), RK3576_SV6621_CTRL);
  for (index = 0;
       (getreg32(RK3576_SV6621_CTRL) & (1u << 1)) != 0 && index < 100000;
       index++)
    ;

  if (index == 100000)
    {
      return -ETIMEDOUT;
    }

  putreg32(RK3576_SV6621_TUNING_BLOCK_SIZE, RK3576_SV6621_BLKSIZ);
  putreg32(RK3576_SV6621_TUNING_BLOCK_SIZE, RK3576_SV6621_BYTCNT);
  status = rk3576_sv6621_command(RK3576_SV6621_CMD19, 0, &response);
  if ((status & (RK3576_SV6621_INT_CMDERR | RK3576_SV6621_INT_DATAERR)) != 0)
    {
      return (status & RK3576_SV6621_INT_TIMEOUT) != 0 ? -ETIMEDOUT : -EIO;
    }

  for (index = 0; index < 200000 && received < RK3576_SV6621_TUNING_BLOCK_SIZE;
       index++)
    {
      fifo_count = (getreg32(RK3576_SV6621_STATUS) >> 17) & 0x1fff;
      while (fifo_count-- > 0 && received < RK3576_SV6621_TUNING_BLOCK_SIZE)
        {
          word = getreg32(RK3576_SV6621_FIFO);
          tuning_block[received++] = word;
          tuning_block[received++] = word >> 8;
          tuning_block[received++] = word >> 16;
          tuning_block[received++] = word >> 24;
        }

      up_udelay(2);
    }

  for (index = 0; index < 200000; index++)
    {
      status = getreg32(RK3576_SV6621_RINTSTS);
      if ((status & (RK3576_SV6621_INT_DTO | RK3576_SV6621_INT_DATAERR)) != 0)
        {
          break;
        }

      up_udelay(2);
    }

  if (received != RK3576_SV6621_TUNING_BLOCK_SIZE || index == 200000)
    {
      return -ETIMEDOUT;
    }

  if ((status & RK3576_SV6621_INT_DATAERR) != 0)
    {
      return (status & RK3576_SV6621_INT_TIMEOUT) != 0 ? -ETIMEDOUT : -EIO;
    }

  if (memcmp(tuning_block, g_rk3576_sv6621_tuning_pattern,
             RK3576_SV6621_TUNING_BLOCK_SIZE) != 0)
    {
      return -EILSEQ;
    }

  return 0;
}

static int
rk3576_sv6621_tune_sdr104(FAR struct rk3576_sv6621_transport_priv_s *priv)
{
  static const uint8_t phases[RK3576_SV6621_PHASE_COUNT] = { 2, 3, 0, 1 };

  uint8_t value;
  uint8_t cached_phase = priv->sample_phase;
  bool cached = priv->sample_phase_valid;
  int last_ret = -EIO;
  int index;
  int ret;

  ret = rk3576_sv6621_direct(false, 0, RK3576_SV6621_CCCR_BUS_IF, 0, &value);
  if (ret < 0)
    {
      return ret;
    }

  value = (value & ~0x03) | 0x02;
  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_BUS_IF, value, NULL);
  if (ret < 0)
    {
      return ret;
    }

  putreg32(1, RK3576_SV6621_CTYPE);
  ret = rk3576_sv6621_direct(false, 0, RK3576_SV6621_CCCR_SPEED, 0, &value);
  if (ret < 0)
    {
      return ret;
    }

  value = (value & ~0x0e) | 0x07;
  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_SPEED, value, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_sv6621_set_clock(RK3576_SV6621_SRC_396M, 0);
  if (ret < 0)
    {
      return ret;
    }

  putreg32(RK3576_SV6621_TCON_180, RK3576_SV6621_TIMING0);
  if (cached)
    {
      putreg32(RK3576_SV6621_TCON(cached_phase), RK3576_SV6621_TIMING1);
      last_ret = rk3576_sv6621_execute_tuning();
      if (last_ret == 0)
        {
          return 0;
        }

      priv->sample_phase_valid = false;
    }

  for (index = 0; index < RK3576_SV6621_PHASE_COUNT; index++)
    {
      if (cached && phases[index] == cached_phase)
        {
          continue;
        }

      putreg32(RK3576_SV6621_TCON(phases[index]), RK3576_SV6621_TIMING1);
      last_ret = rk3576_sv6621_execute_tuning();
      if (last_ret == 0)
        {
          priv->sample_phase = phases[index];
          priv->sample_phase_valid = true;
          wlinfo("RK3576 SDIO sample phase tuned to %u degrees\n",
                 (unsigned int)phases[index] * RK3576_SV6621_PHASE_STEP);
          return 0;
        }
    }

  wlerr("ERROR: RK3576 SDIO has no valid sample phase\n");
  return last_ret;
}

/****************************************************************************
 * Name: rk3576_sv6621_open_failed
 ****************************************************************************/

static int
rk3576_sv6621_open_failed(FAR struct rk3576_sv6621_transport_priv_s *priv,
                          int error)
{
  irqstate_t flags;

  rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, false);
  rk3576_sdmmc_register_sdio_callback(priv->sdio, NULL, NULL);
  SDIO_CLOCK(priv->sdio, CLOCK_SDIO_DISABLED);
  flags = enter_critical_section();
  priv->handler = NULL;
  priv->handler_arg = NULL;
  leave_critical_section(flags);
  priv->irq_enabled = false;
  priv->opened = false;
  priv->prepared = false;
  return error;
}

static int rk3576_sv6621_open(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  int ret;

  if (priv->prepared || priv->opened)
    {
      return -EBUSY;
    }

  if (priv->sdio == NULL)
    {
      priv->sdio = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
      if (priv->sdio == NULL)
        {
          wlerr("ERROR: RK3576 SDIO host initialization failed\n");
          return -ENODEV;
        }
    }
  else
    {
      SDIO_RESET(priv->sdio);
    }

  if (!priv->host_irq_attached)
    {
      ret = SDIO_ATTACH(priv->sdio);
      if (ret < 0)
        {
          wlerr("ERROR: RK3576 SDIO interrupt attach failed: %d\n", ret);
          return rk3576_sv6621_open_failed(priv, ret);
        }

      priv->host_irq_attached = true;
    }

  SDIO_CLOCK(priv->sdio, CLOCK_SDIO_DISABLED);

  putreg32(0, RK3576_SV6621_CLKENA);
  ret = rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  putreg32(0, RK3576_SV6621_CLKDIV);
  putreg32(0x0ffe0002, RK3576_SV6621_TIMING0);
  putreg32((0x3fffu << 16) | 0x2f9d, RK3576_SV6621_CRU_SDIO_SEL);
  putreg32(0, RK3576_SV6621_INTMASK);
  putreg32(getreg32(RK3576_SV6621_CTRL) | (1u << 4), RK3576_SV6621_CTRL);

  priv->prepared = true;
  return 0;
}

/****************************************************************************
 * Name: rk3576_sv6621_enumerate
 ****************************************************************************/

static int rk3576_sv6621_enumerate(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  uint32_t response = 0;
  uint32_t status;
  uint32_t rca;
  uint8_t value = 0;
  int index;
  int ret;

  if (!priv->prepared || priv->opened)
    {
      return -EINVAL;
    }

  /* Match the last known-good networking build: release WL_REG_ON into a
   * quiet bus, let the combo ROM settle, then start the identification clock.
   */

  putreg32(1, RK3576_SV6621_CLKENA);
  ret = rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  up_mdelay(10);
  for (index = 0; index < 3; index++)
    {
      status =
          rk3576_sv6621_command(RK3576_SV6621_CMD5, 0x01300000, &response);
      if ((status & RK3576_SV6621_INT_CMDERR) != 0)
        {
          if ((status & RK3576_SV6621_INT_RTO) != 0)
            {
              up_mdelay(10);
              continue;
            }

          ret = -EIO;
          wlerr("ERROR: SV6621 CMD5 failed: %d\n", ret);
          return rk3576_sv6621_open_failed(priv, ret);
        }

      if ((response & (1u << 31)) != 0)
        {
          break;
        }

      up_mdelay(10);
    }

  if (index == 3)
    {
      wlerr("ERROR: SV6621 did not become ready after CMD5 retries\n");
      return rk3576_sv6621_open_failed(priv, -ETIMEDOUT);
    }

  if ((response & (1u << 24)) == 0)
    {
      return rk3576_sv6621_open_failed(priv, -EOPNOTSUPP);
    }

  if ((response & RK3576_SV6621_R4_FUNCTIONS_MASK) == 0)
    {
      wlerr("ERROR: SV6621 reported no SDIO functions: 0x%08" PRIx32 "\n",
            response);
      return rk3576_sv6621_open_failed(priv, -ENODEV);
    }

  ret = rk3576_sv6621_voltage_switch();
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  status = rk3576_sv6621_command(RK3576_SV6621_CMD3, 0, &response);
  if ((status & RK3576_SV6621_INT_CMDERR) != 0)
    {
      ret = (status & RK3576_SV6621_INT_RTO) != 0 ? -ETIMEDOUT : -EIO;
      return rk3576_sv6621_open_failed(priv, ret);
    }

  if ((response & RK3576_SV6621_R6_ERROR_MASK) != 0 || (response >> 16) == 0)
    {
      wlerr("ERROR: SV6621 CMD3 rejected RCA assignment: 0x%08" PRIx32 "\n",
            response);
      return rk3576_sv6621_open_failed(priv, -EIO);
    }

  rca = response >> 16;
  status = rk3576_sv6621_command(RK3576_SV6621_CMD7, rca << 16, &response);
  if ((status & RK3576_SV6621_INT_CMDERR) != 0)
    {
      ret = (status & RK3576_SV6621_INT_RTO) != 0 ? -ETIMEDOUT : -EIO;
      return rk3576_sv6621_open_failed(priv, ret);
    }

  if ((response & RK3576_SV6621_R1_ERROR_MASK) != 0)
    {
      wlerr("ERROR: SV6621 CMD7 selection failed: 0x%08" PRIx32 "\n",
            response);
      return rk3576_sv6621_open_failed(priv, -EIO);
    }

  ret = rk3576_sv6621_direct(true, 0, 0x16, 0x03, NULL);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  /* Keep the card-side interrupt path enabled while changing the bus
   * timing.  The verified SV6621 enumeration trace enables IEN before
   * entering SDR104; the host callback remains masked until attach_irq.
   */

  ret = rk3576_sv6621_direct(
      true, 0, RK3576_SV6621_CCCR_INTERRUPT,
      RK3576_SV6621_INTERRUPT_MASTER | RK3576_SV6621_FUNCTION1_BIT, NULL);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  ret = rk3576_sv6621_tune_sdr104(priv);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_FBR1_BLOCK_LOW,
                             RK3576_SV6621_BLOCK_SIZE & 0xff, NULL);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_FBR1_BLOCK_HIGH,
                             RK3576_SV6621_BLOCK_SIZE >> 8, NULL);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_IO_ENABLE,
                             RK3576_SV6621_FUNCTION1_BIT, NULL);
  if (ret < 0)
    {
      return rk3576_sv6621_open_failed(priv, ret);
    }

  for (index = 0; index < 100; index++)
    {
      ret = rk3576_sv6621_direct(false, 0, RK3576_SV6621_CCCR_IO_READY, 0,
                                 &value);
      if (ret < 0)
        {
          return rk3576_sv6621_open_failed(priv, ret);
        }

      if ((value & RK3576_SV6621_FUNCTION1_BIT) != 0)
        {
          priv->opened = true;
          return OK;
        }

      up_mdelay(10);
    }

  wlerr("ERROR: SV6621 function 1 did not become ready\n");
  return rk3576_sv6621_open_failed(priv, -ETIMEDOUT);
}

static int rk3576_sv6621_read_byte(FAR struct sv6621_transport_s *transport,
                                   uint8_t function, uint32_t address,
                                   FAR uint8_t *value)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;

  if (!priv->opened || value == NULL ||
      function > RK3576_SV6621_FUNCTION_MAX ||
      address > RK3576_SV6621_ADDRESS_MAX)
    {
      return -EINVAL;
    }

  return rk3576_sv6621_direct(false, function, address, 0, value);
}

static int rk3576_sv6621_write_byte(FAR struct sv6621_transport_s *transport,
                                    uint8_t function, uint32_t address,
                                    uint8_t value)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;

  if (!priv->opened || function > RK3576_SV6621_FUNCTION_MAX ||
      address > RK3576_SV6621_ADDRESS_MAX)
    {
      return -EINVAL;
    }

  return rk3576_sv6621_direct(true, function, address, value, NULL);
}

static int
rk3576_sv6621_extended(FAR struct rk3576_sv6621_transport_priv_s *priv,
                       bool write, uint8_t function, uint32_t address,
                       bool increment, FAR void *buffer, size_t length)
{
  FAR uint8_t *transfer = buffer;
  bool block = !increment && length >= RK3576_SV6621_BLOCK_SIZE &&
               (length % RK3576_SV6621_BLOCK_SIZE) == 0;
  sdio_eventset_t event = 0;
  uint32_t argument;
  uint32_t response = 0;
  uint32_t command;
  unsigned int block_length;
  unsigned int block_count;
  int ret;

  if (!priv->opened || buffer == NULL || length == 0 ||
      function > RK3576_SV6621_FUNCTION_MAX ||
      address > RK3576_SV6621_ADDRESS_MAX ||
      (increment && length - 1 > RK3576_SV6621_ADDRESS_MAX - address) ||
      (!block && length > RK3576_SV6621_BYTE_COUNT_MAX) ||
      (block &&
       length / RK3576_SV6621_BLOCK_SIZE > RK3576_SV6621_BLOCK_COUNT_MAX))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (length <= sizeof(g_rk3576_sv6621_dma_bounce))
    {
      transfer = g_rk3576_sv6621_dma_bounce;
      if (write)
        {
          memcpy(transfer, buffer, length);
        }
    }

  block_length = block ? RK3576_SV6621_BLOCK_SIZE : (unsigned int)length;
  block_count = block ? (unsigned int)(length / RK3576_SV6621_BLOCK_SIZE) : 0;
  argument =
      (write ? (1u << 31) : 0) | ((function & 7) << 28) |
      (block ? (1u << 27) : 0) | (increment ? (1u << 26) : 0) |
      ((address & 0x1ffff) << 9) |
      (block ? (block_count & 0x1ff)
             : (block_length == RK3576_SV6621_BYTE_COUNT_MAX ? 0
                                                             : block_length));
  command = write ? SD_ACMD53WR : SD_ACMD53RD;

  ret = nxmutex_lock(&priv->sdio->mutex);
  if (ret < 0)
    {
      goto unlock_transport;
    }

#ifdef CONFIG_SDIO_MUXBUS
  ret = SDIO_LOCK(priv->sdio, true);
  if (ret < 0)
    {
      goto unlock_host;
    }
#endif

  SDIO_BLOCKSETUP(priv->sdio, block_length, block ? block_count : 1);
  SDIO_WAITENABLE(priv->sdio,
                  SDIOWAIT_TRANSFERDONE | SDIOWAIT_TIMEOUT | SDIOWAIT_ERROR,
                  RK3576_SV6621_CMD53_TIMEOUT_MS);
  if (write)
    {
      ret = SDIO_DMASENDSETUP(priv->sdio, transfer, length);
    }
  else
    {
      ret = SDIO_DMARECVSETUP(priv->sdio, transfer, length);
    }

  if (ret < 0)
    {
      SDIO_CANCEL(priv->sdio);
      goto unlock_bus;
    }

  ret = SDIO_SENDCMD(priv->sdio, command, argument);
  if (ret < 0)
    {
      SDIO_CANCEL(priv->sdio);
      goto unlock_bus;
    }

  event = SDIO_EVENTWAIT(priv->sdio);
  ret = SDIO_RECVR5(priv->sdio, command, &response);
  if (ret >= 0 && (event & SDIOWAIT_TIMEOUT) != 0)
    {
      ret = -ETIMEDOUT;
    }
  else if (ret >= 0 &&
           ((event & SDIOWAIT_ERROR) != 0 || ((response >> 8) & 0xcb) != 0))
    {
      ret = -EIO;
    }

unlock_bus:
#ifdef CONFIG_SDIO_MUXBUS
  SDIO_LOCK(priv->sdio, false);
unlock_host:
#endif
  nxmutex_unlock(&priv->sdio->mutex);

  if (ret >= 0 && !write && transfer != buffer)
    {
      memcpy(buffer, transfer, length);
    }

unlock_transport:
  nxmutex_unlock(&priv->lock);
  return ret;
}

static int rk3576_sv6621_read(FAR struct sv6621_transport_s *transport,
                              uint8_t function, uint32_t address,
                              bool increment, FAR void *buffer, size_t length)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;

  return rk3576_sv6621_extended(priv, false, function, address, increment,
                                buffer, length);
}

static int rk3576_sv6621_write(FAR struct sv6621_transport_s *transport,
                               uint8_t function, uint32_t address,
                               bool increment, FAR const void *buffer,
                               size_t length)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;

  return rk3576_sv6621_extended(priv, true, function, address, increment,
                                (FAR void *)buffer, length);
}

static void rk3576_sv6621_close(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  irqstate_t flags;

  if (!priv->prepared)
    {
      return;
    }

  if (priv->opened)
    {
      rk3576_sv6621_enable_irq(transport, false);
    }

  rk3576_sdmmc_register_sdio_callback(priv->sdio, NULL, NULL);
  SDIO_CLOCK(priv->sdio, CLOCK_SDIO_DISABLED);
  flags = enter_critical_section();
  priv->handler = NULL;
  priv->handler_arg = NULL;
  leave_critical_section(flags);
  priv->irq_enabled = false;
  priv->opened = false;
  priv->prepared = false;
}

static void rk3576_sv6621_host_interrupt(FAR void *arg)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = arg;
  sv6621_transport_irq_t handler = priv->handler;

  if (handler != NULL)
    {
      handler(priv->handler_arg);
    }
}

static int rk3576_sv6621_attach_irq(FAR struct sv6621_transport_s *transport,
                                    sv6621_transport_irq_t handler,
                                    FAR void *arg)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  irqstate_t flags;
  int ret;

  if (!priv->opened)
    {
      return -ENODEV;
    }

  if (handler == NULL && priv->irq_enabled)
    {
      return -EBUSY;
    }

  ret = rk3576_sdmmc_register_sdio_callback(
      priv->sdio, handler != NULL ? rk3576_sv6621_host_interrupt : NULL,
      handler != NULL ? priv : NULL);
  if (ret < 0)
    {
      return ret;
    }

  flags = enter_critical_section();
  priv->handler = handler;
  priv->handler_arg = handler != NULL ? arg : NULL;
  leave_critical_section(flags);
  return 0;
}

static int rk3576_sv6621_enable_irq(FAR struct sv6621_transport_s *transport,
                                    bool enable)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  uint8_t value;
  int ret;

  if (!priv->opened)
    {
      return -ENODEV;
    }

  if (enable && priv->handler == NULL)
    {
      return -EINVAL;
    }

  if (priv->irq_enabled == enable)
    {
      return 0;
    }

  if (enable)
    {
      ret = rk3576_sv6621_direct(
          true, 0, RK3576_SV6621_CCCR_INTERRUPT,
          RK3576_SV6621_INTERRUPT_MASTER | RK3576_SV6621_FUNCTION1_BIT, NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, true);
      if (ret < 0)
        {
          rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_INTERRUPT, 0, NULL);
          return ret;
        }
    }
  else
    {
      int rollback;

      ret = rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, false);
      if (ret < 0)
        {
          return ret;
        }

      ret = rk3576_sv6621_direct(false, 0, RK3576_SV6621_CCCR_INTERRUPT, 0,
                                 &value);
      if (ret >= 0)
        {
          value &=
              ~(RK3576_SV6621_INTERRUPT_MASTER | RK3576_SV6621_FUNCTION1_BIT);
          ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_INTERRUPT,
                                     value, NULL);
        }

      if (ret < 0)
        {
          rollback = rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, true);
          if (rollback < 0)
            {
              priv->irq_enabled = false;
              return rollback;
            }

          return ret;
        }
    }

  priv->irq_enabled = enable;
  return 0;
}

static int rk3576_sv6621_ack_irq(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  uint8_t pending;
  int ret;

  if (!priv->opened || !priv->irq_enabled)
    {
      return -ENODEV;
    }

  /* The SV6621 in-band interrupt contract clears the card-side source by
   * reading CCCR INTx before the receive window is drained.  The controller
   * latch can then be rearmed without racing an asserted DAT1 level.
   */

  ret = rk3576_sv6621_direct(false, 0, RK3576_SV6621_CCCR_INT_PENDING, 0,
                             &pending);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_sdmmc_ack_sdio_interrupt(priv->sdio);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct sv6621_transport_s *kickpi_k7_sv6621_transport(void)
{
  return &g_rk3576_sv6621_transport;
}
