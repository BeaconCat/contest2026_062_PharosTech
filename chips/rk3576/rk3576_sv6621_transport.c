/****************************************************************************
 * chips/rk3576/rk3576_sv6621_transport.c
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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "rk3576_sdmmc.h"
#include "rk3576_sv6621_transport.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_SV6621_MSHC_BASE    0x2a320000
#define RK3576_SV6621_CTRL         (RK3576_SV6621_MSHC_BASE + 0x000)
#define RK3576_SV6621_CLKDIV       (RK3576_SV6621_MSHC_BASE + 0x008)
#define RK3576_SV6621_CLKENA       (RK3576_SV6621_MSHC_BASE + 0x010)
#define RK3576_SV6621_CTYPE        (RK3576_SV6621_MSHC_BASE + 0x018)
#define RK3576_SV6621_BLKSIZ       (RK3576_SV6621_MSHC_BASE + 0x01c)
#define RK3576_SV6621_BYTCNT       (RK3576_SV6621_MSHC_BASE + 0x020)
#define RK3576_SV6621_INTMASK      (RK3576_SV6621_MSHC_BASE + 0x024)
#define RK3576_SV6621_CMDARG       (RK3576_SV6621_MSHC_BASE + 0x028)
#define RK3576_SV6621_CMD          (RK3576_SV6621_MSHC_BASE + 0x02c)
#define RK3576_SV6621_RESP0        (RK3576_SV6621_MSHC_BASE + 0x030)
#define RK3576_SV6621_RINTSTS      (RK3576_SV6621_MSHC_BASE + 0x044)
#define RK3576_SV6621_STATUS       (RK3576_SV6621_MSHC_BASE + 0x048)
#define RK3576_SV6621_UHS          (RK3576_SV6621_MSHC_BASE + 0x074)
#define RK3576_SV6621_TIMING0      (RK3576_SV6621_MSHC_BASE + 0x130)
#define RK3576_SV6621_TIMING1      (RK3576_SV6621_MSHC_BASE + 0x134)
#define RK3576_SV6621_FIFO         (RK3576_SV6621_MSHC_BASE + 0x200)

#define RK3576_SV6621_CRU_SDIO_SEL 0x272004a0

#define RK3576_SV6621_CMD_START    (1u << 31)
#define RK3576_SV6621_CMD3         0xa0000143
#define RK3576_SV6621_CMD5         0xa0000045
#define RK3576_SV6621_CMD7         0xa0000147
#define RK3576_SV6621_CMD11        0xb000014b
#define RK3576_SV6621_CMD19        0xa0002353
#define RK3576_SV6621_CMD52        0xa0000174
#define RK3576_SV6621_CMD53_READ   0xa0002375
#define RK3576_SV6621_CMD53_WRITE  0xa0002775

#define RK3576_SV6621_INT_CMDDONE  (1u << 2)
#define RK3576_SV6621_INT_DTO      (1u << 3)
#define RK3576_SV6621_INT_RTO      (1u << 8)
#define RK3576_SV6621_INT_VOLTSW   (1u << 12)
#define RK3576_SV6621_INT_DATAERR  0x0000ae80

#define RK3576_SV6621_CLK_UPDATE   0x80202000
#define RK3576_SV6621_CLK_UPD_VOLT 0x90202000
#define RK3576_SV6621_SRC_396M     0x2f02
#define RK3576_SV6621_TCON(raw) \
  (((uint32_t)0x7ff << 1 << 16) | ((uint32_t)(raw) << 1))
#define RK3576_SV6621_TCON_180   RK3576_SV6621_TCON(0x2)
#define RK3576_SV6621_POLL_LIMIT 200000
#define RK3576_SV6621_FUNCTION_MAX 7
#define RK3576_SV6621_ADDRESS_MAX  0x1ffff
#define RK3576_SV6621_BLOCK_SIZE   512
#define RK3576_SV6621_BYTE_COUNT_MAX 511
#define RK3576_SV6621_BLOCK_COUNT_MAX 511

#define RK3576_SV6621_CCCR_IO_ENABLE  0x02
#define RK3576_SV6621_CCCR_IO_READY   0x03
#define RK3576_SV6621_CCCR_INTERRUPT  0x04
#define RK3576_SV6621_CCCR_BUS_IF      0x07
#define RK3576_SV6621_CCCR_SPEED       0x13
#define RK3576_SV6621_FBR1_BLOCK_LOW   0x110
#define RK3576_SV6621_FBR1_BLOCK_HIGH  0x111

#define RK3576_SV6621_FUNCTION1_BIT    (1 << 1)
#define RK3576_SV6621_INTERRUPT_MASTER (1 << 0)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_sv6621_transport_priv_s
{
  FAR struct sdio_dev_s *sdio;
  mutex_t lock;
  sv6621_transport_irq_t handler;
  FAR void *handler_arg;
  bool opened;
  bool irq_enabled;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_sv6621_ciu_update(uint32_t command);
static void rk3576_sv6621_set_clock(uint32_t source, uint32_t divider);
static uint32_t rk3576_sv6621_command(uint32_t command, uint32_t argument,
                                      FAR uint32_t *response);
static int rk3576_sv6621_direct(bool write, uint8_t function, uint32_t address,
                                uint8_t value, FAR uint8_t *result);
static void rk3576_sv6621_voltage_switch(void);
static void rk3576_sv6621_tune_sdr104(void);
static int rk3576_sv6621_open(FAR struct sv6621_transport_s *transport);
static void rk3576_sv6621_close(FAR struct sv6621_transport_s *transport);
static int rk3576_sv6621_read_byte(FAR struct sv6621_transport_s *transport,
                                   uint8_t function, uint32_t address,
                                   FAR uint8_t *value);
static int rk3576_sv6621_write_byte(FAR struct sv6621_transport_s *transport,
                                    uint8_t function, uint32_t address,
                                    uint8_t value);
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
static int rk3576_sv6621_recover(FAR struct sv6621_transport_s *transport);
static void rk3576_sv6621_host_interrupt(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_sv6621_transport_priv_s g_rk3576_sv6621_priv = {
  .lock = NXMUTEX_INITIALIZER,
};

static const struct sv6621_transport_ops_s g_rk3576_sv6621_ops = {
  .open = rk3576_sv6621_open,
  .close = rk3576_sv6621_close,
  .read_byte = rk3576_sv6621_read_byte,
  .write_byte = rk3576_sv6621_write_byte,
  .read = rk3576_sv6621_read,
  .write = rk3576_sv6621_write,
  .attach_irq = rk3576_sv6621_attach_irq,
  .enable_irq = rk3576_sv6621_enable_irq,
  .recover = rk3576_sv6621_recover,
};

static struct sv6621_transport_s g_rk3576_sv6621_transport = {
  .ops = &g_rk3576_sv6621_ops,
  .priv = &g_rk3576_sv6621_priv,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void rk3576_sv6621_ciu_update(uint32_t command)
{
  int index;

  putreg32(command, RK3576_SV6621_CMD);
  for (index = 0;
       (getreg32(RK3576_SV6621_CMD) & RK3576_SV6621_CMD_START) != 0 &&
       index < RK3576_SV6621_POLL_LIMIT;
       index++)
    ;
}

static void rk3576_sv6621_set_clock(uint32_t source, uint32_t divider)
{
  putreg32(0, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  putreg32((0x3fffu << 16) | (source & 0x3fff), RK3576_SV6621_CRU_SDIO_SEL);
  putreg32(divider, RK3576_SV6621_CLKDIV);
  putreg32(1, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  up_mdelay(2);
}

static uint32_t rk3576_sv6621_command(uint32_t command, uint32_t argument,
                                      FAR uint32_t *response)
{
  uint32_t status;
  int index;

  for (index = 0; (getreg32(RK3576_SV6621_STATUS) & (1u << 9)) != 0 &&
                  index < RK3576_SV6621_POLL_LIMIT;
       index++)
    {
      up_udelay(5);
    }

  putreg32(UINT32_MAX, RK3576_SV6621_RINTSTS);
  putreg32(argument, RK3576_SV6621_CMDARG);
  putreg32(command, RK3576_SV6621_CMD);

  for (index = 0;
       (getreg32(RK3576_SV6621_CMD) & RK3576_SV6621_CMD_START) != 0 &&
       index < RK3576_SV6621_POLL_LIMIT;
       index++)
    ;

  for (index = 0; index < RK3576_SV6621_POLL_LIMIT; index++)
    {
      status = getreg32(RK3576_SV6621_RINTSTS);
      if ((status & (RK3576_SV6621_INT_CMDDONE | RK3576_SV6621_INT_RTO)) != 0)
        {
          break;
        }

      up_udelay(5);
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

  argument = (write ? (1u << 31) : 0) | ((function & 7) << 28) |
             (write ? (1u << 27) : 0) | ((address & 0x1ffff) << 9) |
             (write ? value : 0);

  nxmutex_lock(&g_rk3576_sv6621_priv.lock);
  status = rk3576_sv6621_command(RK3576_SV6621_CMD52, argument, &response);
  nxmutex_unlock(&g_rk3576_sv6621_priv.lock);

  if ((status & RK3576_SV6621_INT_RTO) != 0)
    {
      return -ETIMEDOUT;
    }

  if (result != NULL)
    {
      *result = response & 0xff;
    }

  return ((response >> 8) & 0xcb) != 0 ? -EIO : OK;
}

static void rk3576_sv6621_voltage_switch(void)
{
  uint32_t response;
  int index;

  rk3576_sv6621_command(RK3576_SV6621_CMD11, 0, &response);
  for (index = 0; index < 100000; index++)
    {
      if ((getreg32(RK3576_SV6621_RINTSTS) & RK3576_SV6621_INT_VOLTSW) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  putreg32(RK3576_SV6621_INT_VOLTSW, RK3576_SV6621_RINTSTS);
  putreg32(0, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPD_VOLT);
  modifyreg32(RK3576_SV6621_UHS, 0, 1);
  up_mdelay(10);
  putreg32(1, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPD_VOLT);

  for (index = 0; index < 200000; index++)
    {
      if ((getreg32(RK3576_SV6621_RINTSTS) & RK3576_SV6621_INT_VOLTSW) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  putreg32(RK3576_SV6621_INT_VOLTSW, RK3576_SV6621_RINTSTS);
  up_mdelay(5);
}

static void rk3576_sv6621_tune_sdr104(void)
{
  uint32_t response;
  uint32_t fifo_count;
  uint8_t value;
  int received = 0;
  int index;

  rk3576_sv6621_direct(false, 0, 0x07, 0, &value);
  rk3576_sv6621_direct(true, 0, 0x07, 0x02, NULL);
  putreg32(1, RK3576_SV6621_CTYPE);
  rk3576_sv6621_direct(false, 0, 0x13, 0, &value);
  rk3576_sv6621_direct(true, 0, 0x13, 0x07, NULL);

  rk3576_sv6621_set_clock(RK3576_SV6621_SRC_396M, 0);
  putreg32(RK3576_SV6621_TCON_180, RK3576_SV6621_TIMING0);
  putreg32(RK3576_SV6621_TCON_180, RK3576_SV6621_TIMING1);

  modifyreg32(RK3576_SV6621_CTRL, 0, 1u << 1);
  for (index = 0;
       (getreg32(RK3576_SV6621_CTRL) & (1u << 1)) != 0 && index < 100000;
       index++)
    ;

  putreg32(64, RK3576_SV6621_BLKSIZ);
  putreg32(64, RK3576_SV6621_BYTCNT);
  rk3576_sv6621_command(RK3576_SV6621_CMD19, 0, &response);

  for (index = 0; index < 200000 && received < 16; index++)
    {
      fifo_count = (getreg32(RK3576_SV6621_STATUS) >> 17) & 0x1fff;
      while (fifo_count-- > 0 && received < 16)
        {
          (void)getreg32(RK3576_SV6621_FIFO);
          received++;
        }

      up_udelay(2);
    }
}

static int rk3576_sv6621_open(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  uint32_t response = 0;
  uint32_t status;
  uint32_t rca;
  uint8_t value = 0;
  int index;
  int ret;

  if (priv->opened)
    {
      return -EBUSY;
    }

  priv->sdio = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (priv->sdio == NULL)
    {
      wlerr("ERROR: RK3576 SDIO host initialization failed\n");
      return -ENODEV;
    }

  SDIO_CLOCK(priv->sdio, CLOCK_SDIO_DISABLED);

  putreg32(0, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  putreg32(0, RK3576_SV6621_CLKDIV);
  putreg32(0x0ffe0002, RK3576_SV6621_TIMING0);
  putreg32((0x3fffu << 16) | 0x2f9d, RK3576_SV6621_CRU_SDIO_SEL);

  putreg32(1, RK3576_SV6621_CLKENA);
  rk3576_sv6621_ciu_update(RK3576_SV6621_CLK_UPDATE);
  up_mdelay(10);

  putreg32(0, RK3576_SV6621_INTMASK);
  modifyreg32(RK3576_SV6621_CTRL, 0, 1u << 4);

  status = rk3576_sv6621_command(RK3576_SV6621_CMD5, 0x01300000, &response);
  if ((status & RK3576_SV6621_INT_RTO) != 0)
    {
      wlerr("ERROR: SV6621 CMD5 response timed out\n");
      return -ENODEV;
    }

  if ((response & (1u << 24)) != 0)
    {
      rk3576_sv6621_voltage_switch();
    }

  status = rk3576_sv6621_command(RK3576_SV6621_CMD3, 0, &response);
  if ((status & RK3576_SV6621_INT_RTO) != 0)
    {
      return -EIO;
    }

  rca = response >> 16;
  status = rk3576_sv6621_command(RK3576_SV6621_CMD7, rca << 16, &response);
  if ((status & RK3576_SV6621_INT_RTO) != 0)
    {
      return -EIO;
    }

  ret = rk3576_sv6621_direct(true, 0, 0x16, 0x03, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_INTERRUPT, 0,
                             NULL);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_sv6621_tune_sdr104();

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_FBR1_BLOCK_LOW,
                             RK3576_SV6621_BLOCK_SIZE & 0xff, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_FBR1_BLOCK_HIGH,
                             RK3576_SV6621_BLOCK_SIZE >> 8, NULL);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_IO_ENABLE,
                             RK3576_SV6621_FUNCTION1_BIT, NULL);
  if (ret < 0)
    {
      return ret;
    }

  for (index = 0; index < 100; index++)
    {
      ret = rk3576_sv6621_direct(false, 0,
                                 RK3576_SV6621_CCCR_IO_READY, 0, &value);
      if (ret < 0)
        {
          return ret;
        }

      if ((value & RK3576_SV6621_FUNCTION1_BIT) != 0)
        {
          priv->opened = true;
          return OK;
        }

      up_mdelay(10);
    }

  wlerr("ERROR: SV6621 function 1 did not become ready\n");
  return -EIO;
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

static int rk3576_sv6621_read(FAR struct sv6621_transport_s *transport,
                              uint8_t function, uint32_t address,
                              bool increment, FAR void *buffer, size_t length)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  FAR uint8_t *bytes = buffer;
  uint32_t argument;
  uint32_t status;
  uint32_t fifo_count;
  size_t words = (length + 3) / 4;
  int received = 0;
  int index;
  bool block = length >= RK3576_SV6621_BLOCK_SIZE &&
               (length % RK3576_SV6621_BLOCK_SIZE) == 0;

  if (!priv->opened || buffer == NULL || length == 0 ||
      function > RK3576_SV6621_FUNCTION_MAX ||
      address > RK3576_SV6621_ADDRESS_MAX ||
      (!block && length > RK3576_SV6621_BYTE_COUNT_MAX) ||
      (block && length / RK3576_SV6621_BLOCK_SIZE >
                    RK3576_SV6621_BLOCK_COUNT_MAX))
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);
  modifyreg32(RK3576_SV6621_CTRL, 0, 1u << 1);
  for (index = 0;
       (getreg32(RK3576_SV6621_CTRL) & (1u << 1)) != 0 && index < 100000;
       index++)
    ;

  putreg32(block ? RK3576_SV6621_BLOCK_SIZE : (uint32_t)length,
           RK3576_SV6621_BLKSIZ);
  putreg32(length, RK3576_SV6621_BYTCNT);
  argument = ((function & 7) << 28) | (increment ? (1u << 26) : 0) |
             ((address & 0x1ffff) << 9) |
             (block ? ((1u << 27) |
                       ((uint32_t)(length / RK3576_SV6621_BLOCK_SIZE) &
                        0x1ff))
                    : ((uint32_t)length & 0x1ff));

  status = rk3576_sv6621_command(RK3576_SV6621_CMD53_READ, argument, NULL);
  if ((status & RK3576_SV6621_INT_RTO) != 0)
    {
      nxmutex_unlock(&priv->lock);
      return -ETIMEDOUT;
    }

  for (index = 0; index < 400000 && received < words; index++)
    {
      fifo_count = (getreg32(RK3576_SV6621_STATUS) >> 17) & 0x1fff;
      while (fifo_count-- > 0 && received < words)
        {
          uint32_t word = getreg32(RK3576_SV6621_FIFO);
          int byte;

          for (byte = 0; byte < 4 && received * 4 + byte < length; byte++)
            {
              bytes[received * 4 + byte] = (word >> (8 * byte)) & 0xff;
            }

          received++;
        }

      up_udelay(5);
    }

  status = getreg32(RK3576_SV6621_RINTSTS);
  nxmutex_unlock(&priv->lock);

  return (status & RK3576_SV6621_INT_DATAERR) != 0
             ? -EIO
             : ((size_t)received == words ? OK : -ETIMEDOUT);
}

static int rk3576_sv6621_write(FAR struct sv6621_transport_s *transport,
                               uint8_t function, uint32_t address,
                               bool increment, FAR const void *buffer,
                               size_t length)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  FAR const uint8_t *bytes = buffer;
  uint32_t argument;
  uint32_t status;
  size_t words = (length + 3) / 4;
  int sent = 0;
  int index;
  bool block = length >= RK3576_SV6621_BLOCK_SIZE &&
               (length % RK3576_SV6621_BLOCK_SIZE) == 0;

  if (!priv->opened || buffer == NULL || length == 0 ||
      function > RK3576_SV6621_FUNCTION_MAX ||
      address > RK3576_SV6621_ADDRESS_MAX ||
      (!block && length > RK3576_SV6621_BYTE_COUNT_MAX) ||
      (block && length / RK3576_SV6621_BLOCK_SIZE >
                    RK3576_SV6621_BLOCK_COUNT_MAX))
    {
      return -EINVAL;
    }

  nxmutex_lock(&priv->lock);
  modifyreg32(RK3576_SV6621_CTRL, 0, 1u << 1);
  for (index = 0;
       (getreg32(RK3576_SV6621_CTRL) & (1u << 1)) != 0 && index < 100000;
       index++)
    ;

  putreg32(block ? RK3576_SV6621_BLOCK_SIZE : (uint32_t)length,
           RK3576_SV6621_BLKSIZ);
  putreg32(length, RK3576_SV6621_BYTCNT);
  argument = (1u << 31) | ((function & 7) << 28) |
             (increment ? (1u << 26) : 0) | ((address & 0x1ffff) << 9) |
             (block ? ((1u << 27) |
                       ((uint32_t)(length / RK3576_SV6621_BLOCK_SIZE) &
                        0x1ff))
                    : ((uint32_t)length & 0x1ff));

  for (index = 0; (getreg32(RK3576_SV6621_STATUS) & (1u << 9)) != 0 &&
                  index < RK3576_SV6621_POLL_LIMIT;
       index++)
    {
      up_udelay(5);
    }

  putreg32(UINT32_MAX, RK3576_SV6621_RINTSTS);
  putreg32(argument, RK3576_SV6621_CMDARG);
  putreg32(RK3576_SV6621_CMD53_WRITE, RK3576_SV6621_CMD);
  for (index = 0;
       (getreg32(RK3576_SV6621_CMD) & RK3576_SV6621_CMD_START) != 0 &&
       index < RK3576_SV6621_POLL_LIMIT;
       index++)
    ;

  for (index = 0; index < 2000000 && sent < words; index++)
    {
      if ((getreg32(RK3576_SV6621_STATUS) & (1u << 3)) == 0)
        {
          uint32_t word = 0;
          int byte;

          for (byte = 0; byte < 4 && sent * 4 + byte < length; byte++)
            {
              word |= ((uint32_t)bytes[sent * 4 + byte]) << (8 * byte);
            }

          putreg32(word, RK3576_SV6621_FIFO);
          sent++;
        }
    }

  for (index = 0; index < 400000; index++)
    {
      status = getreg32(RK3576_SV6621_RINTSTS);
      if ((status & (RK3576_SV6621_INT_DTO | RK3576_SV6621_INT_RTO)) != 0)
        {
          break;
        }

      up_udelay(5);
    }

  status = getreg32(RK3576_SV6621_RINTSTS);
  nxmutex_unlock(&priv->lock);
  if ((status & RK3576_SV6621_INT_RTO) != 0 || (size_t)sent < words)
    {
      return -ETIMEDOUT;
    }

  return (status & RK3576_SV6621_INT_DATAERR) != 0 ? -EIO : OK;
}

static void rk3576_sv6621_close(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;

  if (!priv->opened)
    {
      return;
    }

  rk3576_sv6621_enable_irq(transport, false);
  rk3576_sdmmc_register_sdio_callback(priv->sdio, NULL, NULL);
  SDIO_CLOCK(priv->sdio, CLOCK_SDIO_DISABLED);
  priv->opened = false;
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
          RK3576_SV6621_INTERRUPT_MASTER | RK3576_SV6621_FUNCTION1_BIT,
          NULL);
      if (ret < 0)
        {
          return ret;
        }

      ret = rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, true);
      if (ret < 0)
        {
          rk3576_sv6621_direct(true, 0, RK3576_SV6621_CCCR_INTERRUPT, 0,
                               NULL);
          return ret;
        }
    }
  else
    {
      ret = rk3576_sdmmc_enable_sdio_interrupt(priv->sdio, false);
      if (ret < 0)
        {
          return ret;
        }

      ret = rk3576_sv6621_direct(false, 0,
                                 RK3576_SV6621_CCCR_INTERRUPT, 0, &value);
      if (ret >= 0)
        {
          value &= ~(RK3576_SV6621_INTERRUPT_MASTER |
                     RK3576_SV6621_FUNCTION1_BIT);
          ret = rk3576_sv6621_direct(true, 0,
                                     RK3576_SV6621_CCCR_INTERRUPT, value,
                                     NULL);
        }

      if (ret < 0)
        {
          return ret;
        }
    }

  priv->irq_enabled = enable;
  return 0;
}

static int rk3576_sv6621_recover(FAR struct sv6621_transport_s *transport)
{
  FAR struct rk3576_sv6621_transport_priv_s *priv = transport->priv;
  bool restore_irq;
  int ret;

  if (!priv->opened)
    {
      return -ENODEV;
    }

  restore_irq = priv->irq_enabled;
  rk3576_sv6621_close(transport);
  ret = rk3576_sv6621_open(transport);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->handler != NULL)
    {
      ret = rk3576_sdmmc_register_sdio_callback(
          priv->sdio, rk3576_sv6621_host_interrupt, priv);
      if (ret < 0)
        {
          rk3576_sv6621_close(transport);
          return ret;
        }
    }

  if (restore_irq)
    {
      ret = rk3576_sv6621_enable_irq(transport, true);
      if (ret < 0)
        {
          rk3576_sv6621_close(transport);
          return ret;
        }
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct sv6621_transport_s *rk3576_sv6621_transport(void)
{
  return &g_rk3576_sv6621_transport;
}
