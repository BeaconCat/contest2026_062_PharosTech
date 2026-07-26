/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_imx415.c
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
 * Sony IMX415 CMOS image sensor driver for the KICKPI-K7 board.
 *
 * 1/2.8" 8.29 Mpixel sensor, MIPI CSI-2 output, RAW10/RAW12.  The board
 * carries three modules (CMK-OT2022-PX1) on I2C4/I2C5/I2C8, all at slave
 * address 0x37, all clocked by the same 37.125 MHz external oscillator
 * ("external-camera-37m-clock" in the vendor DTS).
 *
 * The register interface is a 16-bit big-endian address followed by 8-bit
 * data.  Multi-byte quantities (VMAX, HMAX, SHR0, gain) are little-endian
 * across consecutive addresses, low byte first.
 *
 * Note on I2C reads: the RK3576 I2C master driver terminates every message
 * with a STOP (repeated START is not implemented yet), so a register read is
 * issued as "write address, STOP, read data".  The IMX415 keeps its address
 * pointer across the STOP, so this works, but a repeated-START capable
 * master is preferable once rk3576_i2c.c supports I2C_M_NOSTOP.
 *
 * This file owns the sensor only.  The receiving side (D-PHY lane rate, CSI-2
 * host, VICAP) is programmed by the caller using the mode descriptor returned
 * by rk3576_imx415_get_modeinfo().
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
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/i2c/i2c_master.h>

#include "hardware/rk3576_csi.h"
#include "kickpi_k7_imx415.h"
#include "rk3576_csi.h"
#include "rk3576_gpio.h"
#include "rk3576_i2c.h"

#ifdef CONFIG_KICKPI_K7_IMX415

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IMX415_MAX_DEVICES        3       /* imx415_0, imx415_1, imx415_3   */
#define IMX415_I2C_FREQUENCY      400000  /* Fast mode, module supports 1M  */
#define IMX415_REGADDR_BYTES      2       /* 16-bit register address        */
#define IMX415_WRBUF_BYTES        3       /* addr_hi, addr_lo, data         */

/* Power-up timing (Sony IMX415 datasheet, power sequence).  Generous
 * margins: these are millisecond-scale one-off delays at probe time.
 */

#define IMX415_T_POWER_TO_CLK_MS  1  /* supplies stable -> XVCLK enabled    */
#define IMX415_T_CLK_TO_XCLR_MS   1  /* XVCLK running -> XCLR released      */
#define IMX415_T_XCLR_TO_I2C_MS   20 /* XCLR high -> first I2C access       */
#define IMX415_T_STANDBY_EXIT_MS  30 /* 0x3000 = 0 -> XMSTA release         */

/* Register map subset used here (Sony IMX415 register reference). */

#define IMX415_REG_MODE           0x3000 /* 1 = standby, 0 = operating     */
#define IMX415_REG_REGHOLD        0x3001 /* 1 = hold, 0 = release          */
#define IMX415_REG_XMSTA          0x3002 /* 0 = master mode start          */
#define IMX415_REG_BCWAIT_TIME    0x3008
#define IMX415_REG_CPWAIT_TIME    0x300a
#define IMX415_REG_WINMODE        0x301c
#define IMX415_REG_ADDMODE        0x3020
#define IMX415_REG_HADD           0x3021
#define IMX415_REG_VADD           0x3022
#define IMX415_REG_VMAX           0x3024 /* 20-bit, 0x3024..0x3026         */
#define IMX415_REG_HMAX           0x3028 /* 16-bit, 0x3028..0x3029         */
#define IMX415_REG_ADBIT          0x3031
#define IMX415_REG_MDBIT          0x3032
#define IMX415_REG_SYS_MODE       0x3033
#define IMX415_REG_PIX_HST        0x3040 /* 16-bit crop window origin, X   */
#define IMX415_REG_PIX_HWIDTH     0x3042 /* 16-bit crop window width       */
#define IMX415_REG_PIX_VST        0x3044 /* 16-bit crop window origin, Y   */
#define IMX415_REG_PIX_VWIDTH     0x3046 /* 16-bit crop window height      */
#define IMX415_REG_SHR0           0x3050 /* 20-bit shutter, 0x3050..0x3052 */
#define IMX415_REG_GAIN_PCG_0     0x3090 /* 11-bit, 0x3090..0x3091         */
#define IMX415_REG_INCKSEL1       0x3115
#define IMX415_REG_INCKSEL2       0x3116
#define IMX415_REG_INCKSEL3       0x3118 /* 16-bit, 0x3118..0x3119         */
#define IMX415_REG_INCKSEL4       0x311a /* 16-bit, 0x311a..0x311b         */
#define IMX415_REG_INCKSEL5       0x311e
#define IMX415_REG_TXCLKESC_FREQ  0x4004 /* 16-bit, 0x4004..0x4005         */

#define IMX415_MODE_STANDBY       0x01
#define IMX415_MODE_OPERATING     0x00
#define IMX415_XMSTA_STOP         0x01
#define IMX415_XMSTA_START        0x00
#define IMX415_REGHOLD_HOLD       0x01
#define IMX415_REGHOLD_RELEASE    0x00

/* Field widths of the multi-byte registers. */

#define IMX415_SHR0_MASK          0x000fffffu
#define IMX415_VMAX_MASK          0x000fffffu
#define IMX415_GAIN_MASK          0x07ffu

/* SHR0 must stay at least this many lines below VMAX (Sony constraint on
 * the minimum shutter sweep).
 */

#define IMX415_SHR0_MIN           8u
#define IMX415_SHR0_MARGIN        4u

/* Tolerance accepted on the measured XVCLK rate: +/- 1 %. */

#define IMX415_XVCLK_TOL_PCT      1u

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of a register initialisation table. */

struct imx415_regval_s
{
  uint16_t addr;
  uint8_t  val;
};

struct imx415_dev_s
{
  struct i2c_master_s *i2c;         /* I2C master this sensor hangs on    */
  const struct imx415_config_s *cfg; /* Board wiring                      */
  uint8_t  addr;                    /* 7-bit slave address                */
  bool     streaming;               /* true between stream_on/stream_off  */
  bool     mode_valid;              /* true once a mode table was loaded  */
  enum imx415_mode_e mode;          /* Current mode                       */
  uint32_t vmax;                    /* VMAX of the current mode           */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_imx415_clk_init(void);
static int rk3576_imx415_write(struct imx415_dev_s *dev, uint16_t addr,
                               uint8_t val);
static int rk3576_imx415_read(struct imx415_dev_s *dev, uint16_t addr,
                              uint8_t *val);
static int rk3576_imx415_write_bytes(struct imx415_dev_s *dev, uint16_t addr,
                                     uint32_t value, int nbytes);
static int rk3576_imx415_write_table(struct imx415_dev_s *dev,
                                     const struct imx415_regval_s *table,
                                     size_t nentries);
static int rk3576_imx415_power_up(const struct imx415_config_s *cfg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct imx415_dev_s g_imx415_dev[IMX415_MAX_DEVICES];
static int g_imx415_ndevs;
static struct clk_s *g_imx415_xvclk;
static uint32_t g_imx415_xvclk_hz;

/* Settings shared by every mode: INCK = 37.125 MHz, MIPI 4-lane, RAW10 and
 * the block of Sony "recommended, do not change" registers that accompany
 * the published mode tables.
 *
 * TODO: the 0x32xx..0x3Cxx entries are undocumented Sony recommended values
 *       taken from the public IMX415 mode tables; cross-check them against
 *       the module vendor's register set before relying on image quality.
 */

static const struct imx415_regval_s g_imx415_common_regs[] =
{
  /* Stay in standby while the table is loaded */

  { IMX415_REG_MODE,          IMX415_MODE_STANDBY },
  { IMX415_REG_XMSTA,         IMX415_XMSTA_STOP   },

  /* Bias / current-source settling times for INCK = 37.125 MHz */

  { IMX415_REG_BCWAIT_TIME,   0x7f },
  { IMX415_REG_CPWAIT_TIME,   0x5b },

  /* 10-bit AD, 10-bit MIPI output */

  { IMX415_REG_ADBIT,         0x00 },
  { IMX415_REG_MDBIT,         0x00 },
  { IMX415_REG_SYS_MODE,      0x08 },

  /* INCK selection block for 37.125 MHz / 891 Mbps per lane */

  { IMX415_REG_INCKSEL1,      0x00 },
  { IMX415_REG_INCKSEL2,      0x24 },
  { IMX415_REG_INCKSEL3,      0xc0 },
  { IMX415_REG_INCKSEL3 + 1,  0x00 },
  { IMX415_REG_INCKSEL4,      0xe7 },
  { IMX415_REG_INCKSEL4 + 1,  0x00 },
  { IMX415_REG_INCKSEL5,      0x24 },

  /* LP escape clock divider for a 891 Mbps link
   * TODO: confirm the 0x0948 divider against the datasheet TXCLKESC table.
   */

  { IMX415_REG_TXCLKESC_FREQ, 0x48 },
  { IMX415_REG_TXCLKESC_FREQ + 1, 0x09 },

  /* Sony recommended settings (undocumented) */

  { 0x32d4, 0x21 }, { 0x32ec, 0xa1 }, { 0x344c, 0x2b },
  { 0x344d, 0x01 }, { 0x344e, 0xed }, { 0x344f, 0x01 },
  { 0x3450, 0xf6 }, { 0x3451, 0x02 }, { 0x3452, 0x7f },
  { 0x3453, 0x03 }, { 0x358a, 0x04 }, { 0x35a1, 0x02 },
  { 0x35ec, 0x27 }, { 0x35ee, 0x8d }, { 0x35f0, 0x8d },
  { 0x35f2, 0x29 }, { 0x36bc, 0x0c }, { 0x36cc, 0x53 },
  { 0x36cd, 0x00 }, { 0x36ce, 0x3c }, { 0x36d0, 0x8c },
  { 0x36d1, 0x00 }, { 0x36d2, 0x71 }, { 0x36d4, 0x3c },
  { 0x36d6, 0x53 }, { 0x36d7, 0x00 }, { 0x36d8, 0x71 },
  { 0x36da, 0x8c }, { 0x36db, 0x00 }, { 0x3701, 0x00 },
  { 0x3720, 0x00 }, { 0x3724, 0x02 }, { 0x3726, 0x02 },
  { 0x3732, 0x02 }, { 0x3734, 0x03 }, { 0x3736, 0x03 },
  { 0x3742, 0x03 }, { 0x3862, 0xe0 }, { 0x38cc, 0x30 },
  { 0x38cd, 0x2f }, { 0x395c, 0x0c }, { 0x3a42, 0xd1 },
  { 0x3a4c, 0x77 }, { 0x3ae0, 0x02 }, { 0x3aec, 0x0c },
  { 0x3b00, 0x2e }, { 0x3b06, 0x29 }, { 0x3b98, 0x25 },
  { 0x3b99, 0x21 }, { 0x3b9b, 0x13 }, { 0x3b9c, 0x13 },
  { 0x3b9d, 0x13 }, { 0x3b9e, 0x13 }, { 0x3ba1, 0x00 },
  { 0x3ba2, 0x06 }, { 0x3ba3, 0x0b }, { 0x3ba4, 0x10 },
  { 0x3ba5, 0x14 }, { 0x3ba6, 0x18 }, { 0x3ba7, 0x1a },
  { 0x3ba8, 0x1a }, { 0x3ba9, 0x1a }, { 0x3bac, 0xed },
  { 0x3bad, 0x01 }, { 0x3bae, 0xf6 }, { 0x3baf, 0x02 },
  { 0x3bb0, 0xa2 }, { 0x3bb1, 0x03 }, { 0x3bb2, 0xe0 },
  { 0x3bb3, 0x03 }, { 0x3bb4, 0xe0 }, { 0x3bb5, 0x03 },
  { 0x3bb6, 0xe0 }, { 0x3bb7, 0x03 }, { 0x3bb8, 0xe0 },
  { 0x3bba, 0xe0 }, { 0x3bbc, 0xda }, { 0x3bbd, 0x00 },
  { 0x3bbe, 0x88 }, { 0x3bbf, 0x00 }, { 0x3bc0, 0x44 },
  { 0x3bc1, 0x00 }, { 0x3bc2, 0x7b }, { 0x3bc3, 0x00 },
  { 0x3bc4, 0xa2 }, { 0x3bc5, 0x00 }, { 0x3bc6, 0xcf },
  { 0x3bc7, 0x00 }, { 0x3bc8, 0x00 }, { 0x3bc9, 0x00 },
  { 0x3bca, 0xff }, { 0x3bcb, 0x03 },

  /* MIPI global timing for 891 Mbps per lane (TCLKPOST .. TLPX) */

  { 0x3a18, 0x8f }, { 0x3a19, 0x00 },
  { 0x3a1a, 0x4f }, { 0x3a1b, 0x00 },
  { 0x3a1c, 0x47 }, { 0x3a1d, 0x00 },
  { 0x3a1e, 0x37 }, { 0x3a1f, 0x00 },
  { 0x3a20, 0x4f }, { 0x3a21, 0x00 },
  { 0x3a22, 0x87 }, { 0x3a23, 0x00 },
  { 0x3a24, 0x4f }, { 0x3a25, 0x00 },
  { 0x3a26, 0x7f }, { 0x3a27, 0x00 },
  { 0x3a28, 0x3f }, { 0x3a29, 0x00 },
};

/* 3840x2160 @ 30 fps, all-pixel scan with a small crop, RAW10, 4 lanes.
 *
 * TODO: VMAX/HMAX below give 30 fps with the published 72 MHz internal
 *       system clock; confirm against the IMX415 datasheet frame-rate table
 *       once a scope/frame counter is available on the board.
 */

static const struct imx415_regval_s g_imx415_mode_3840x2160_regs[] =
{
  { IMX415_REG_WINMODE,        0x04 }, /* window cropping mode            */
  { IMX415_REG_ADDMODE,        0x00 }, /* no binning                      */
  { IMX415_REG_HADD,           0x00 },
  { IMX415_REG_VADD,           0x00 },

  { IMX415_REG_VMAX,           0xca }, /* VMAX = 0x008ca = 2250 lines     */
  { IMX415_REG_VMAX + 1,       0x08 },
  { IMX415_REG_VMAX + 2,       0x00 },

  { IMX415_REG_HMAX,           0x4c }, /* HMAX = 0x044c = 1100            */
  { IMX415_REG_HMAX + 1,       0x04 },

  { IMX415_REG_PIX_HST,        0x0c }, /* X origin = 12                   */
  { IMX415_REG_PIX_HST + 1,    0x00 },
  { IMX415_REG_PIX_HWIDTH,     0x00 }, /* width  = 0x0f00 = 3840          */
  { IMX415_REG_PIX_HWIDTH + 1, 0x0f },
  { IMX415_REG_PIX_VST,        0x10 }, /* Y origin = 16                   */
  { IMX415_REG_PIX_VST + 1,    0x00 },
  { IMX415_REG_PIX_VWIDTH,     0x70 }, /* height = 0x0870 = 2160          */
  { IMX415_REG_PIX_VWIDTH + 1, 0x08 },
};

/* 1920x1080 @ 30 fps, 2x2 binning of the 3864x2192 array then cropped,
 * RAW10, 4 lanes.
 *
 * TODO: same caveat as above for VMAX/HMAX, and the binning-mode crop
 *       origin may need a one-pixel adjustment to keep the Bayer phase.
 */

static const struct imx415_regval_s g_imx415_mode_1920x1080_regs[] =
{
  { IMX415_REG_WINMODE,        0x04 }, /* window cropping mode            */
  { IMX415_REG_ADDMODE,        0x01 }, /* 2x2 binning                     */
  { IMX415_REG_HADD,           0x01 },
  { IMX415_REG_VADD,           0x01 },

  { IMX415_REG_VMAX,           0x65 }, /* VMAX = 0x00465 = 1125 lines     */
  { IMX415_REG_VMAX + 1,       0x04 },
  { IMX415_REG_VMAX + 2,       0x00 },

  { IMX415_REG_HMAX,           0x98 }, /* HMAX = 0x0898 = 2200            */
  { IMX415_REG_HMAX + 1,       0x08 },

  { IMX415_REG_PIX_HST,        0x06 }, /* X origin = 6 (binned pixels)    */
  { IMX415_REG_PIX_HST + 1,    0x00 },
  { IMX415_REG_PIX_HWIDTH,     0x80 }, /* width  = 0x0780 = 1920          */
  { IMX415_REG_PIX_HWIDTH + 1, 0x07 },
  { IMX415_REG_PIX_VST,        0x08 }, /* Y origin = 8 (binned lines)     */
  { IMX415_REG_PIX_VST + 1,    0x00 },
  { IMX415_REG_PIX_VWIDTH,     0x38 }, /* height = 0x0438 = 1080          */
  { IMX415_REG_PIX_VWIDTH + 1, 0x04 },
};

static const struct imx415_modeinfo_s g_imx415_modeinfo[IMX415_MODE_COUNT] =
{
  [IMX415_MODE_1920X1080P30] =
  {
    .width          = 1920,
    .height         = 1080,
    .fps            = 30,
    .hmax           = 2200,
    .vmax           = 1125,
    .lane_rate_mbps = 891,
    .csi_dt         = RK3576_CSI2_DT_RAW10,
  },
  [IMX415_MODE_3840X2160P30] =
  {
    .width          = 3840,
    .height         = 2160,
    .fps            = 30,
    .hmax           = 1100,
    .vmax           = 2250,
    .lane_rate_mbps = 891,
    .csi_dt         = RK3576_CSI2_DT_RAW10,
  },
};

/* Mode register tables, indexed by enum imx415_mode_e. */

static const struct imx415_regval_s * const
g_imx415_mode_table[IMX415_MODE_COUNT] =
{
  [IMX415_MODE_1920X1080P30] = g_imx415_mode_1920x1080_regs,
  [IMX415_MODE_3840X2160P30] = g_imx415_mode_3840x2160_regs,
};

static const size_t g_imx415_mode_table_len[IMX415_MODE_COUNT] =
{
  [IMX415_MODE_1920X1080P30] = nitems(g_imx415_mode_1920x1080_regs),
  [IMX415_MODE_3840X2160P30] = nitems(g_imx415_mode_3840x2160_regs),
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_imx415_clk_init
 *
 * Description:
 *   Enable the external 37.125 MHz camera oscillator that feeds XVCLK of all
 *   three modules and record its measured rate.  All clock handling of this
 *   driver lives here; it is idempotent so that every sensor instance may
 *   call it.
 *
 ****************************************************************************/

static int rk3576_imx415_clk_init(void)
{
  uint32_t delta;
  int ret;

  if (g_imx415_xvclk != NULL)
    {
      return OK;
    }

  g_imx415_xvclk = clk_get(IMX415_XVCLK_NAME);
  if (g_imx415_xvclk == NULL)
    {
      verr("ERROR: failed to get %s\n", IMX415_XVCLK_NAME);
      return -ENODEV;
    }

  ret = clk_enable(g_imx415_xvclk);
  if (ret < 0)
    {
      verr("ERROR: failed to enable %s: %d\n", IMX415_XVCLK_NAME, ret);
      g_imx415_xvclk = NULL;
      return ret;
    }

  g_imx415_xvclk_hz = clk_get_rate(g_imx415_xvclk);

  /* The register tables below are computed for a 37.125 MHz INCK.  Refuse
   * to continue with a clock that is out of tolerance rather than produce
   * a silently mistimed link.
   */

  delta = g_imx415_xvclk_hz > IMX415_XVCLK_FREQ ?
          g_imx415_xvclk_hz - IMX415_XVCLK_FREQ :
          IMX415_XVCLK_FREQ - g_imx415_xvclk_hz;

  if (delta > IMX415_XVCLK_FREQ / 100 * IMX415_XVCLK_TOL_PCT)
    {
      verr("ERROR: XVCLK is %" PRIu32 " Hz, expected %u Hz\n",
           g_imx415_xvclk_hz, IMX415_XVCLK_FREQ);
      clk_disable(g_imx415_xvclk);
      g_imx415_xvclk = NULL;
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_imx415_write
 *
 * Description:
 *   Write one 8-bit register at a 16-bit big-endian address.
 *
 ****************************************************************************/

static int rk3576_imx415_write(struct imx415_dev_s *dev, uint16_t addr,
                               uint8_t val)
{
  uint8_t buf[IMX415_WRBUF_BYTES];
  struct i2c_msg_s msg;

  buf[0] = (uint8_t)(addr >> 8);
  buf[1] = (uint8_t)(addr & 0xff);
  buf[2] = val;

  msg.frequency = IMX415_I2C_FREQUENCY;
  msg.addr      = dev->addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = IMX415_WRBUF_BYTES;

  return I2C_TRANSFER(dev->i2c, &msg, 1);
}

/****************************************************************************
 * Name: rk3576_imx415_read
 *
 * Description:
 *   Read one 8-bit register.  Issued as two separate transactions because
 *   the RK3576 I2C master does not support repeated START yet.
 *
 ****************************************************************************/

static int rk3576_imx415_read(struct imx415_dev_s *dev, uint16_t addr,
                              uint8_t *val)
{
  uint8_t abuf[IMX415_REGADDR_BYTES];
  struct i2c_msg_s msg[2];
  int ret;

  abuf[0] = (uint8_t)(addr >> 8);
  abuf[1] = (uint8_t)(addr & 0xff);

  msg[0].frequency = IMX415_I2C_FREQUENCY;
  msg[0].addr      = dev->addr;
  msg[0].flags     = 0;
  msg[0].buffer    = abuf;
  msg[0].length    = IMX415_REGADDR_BYTES;

  msg[1].frequency = IMX415_I2C_FREQUENCY;
  msg[1].addr      = dev->addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = val;
  msg[1].length    = 1;

  ret = I2C_TRANSFER(dev->i2c, msg, 2);
  if (ret < 0)
    {
      verr("ERROR: read of 0x%04x failed: %d\n", addr, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_imx415_write_bytes
 *
 * Description:
 *   Write a little-endian multi-byte field starting at addr, low byte first.
 *
 ****************************************************************************/

static int rk3576_imx415_write_bytes(struct imx415_dev_s *dev, uint16_t addr,
                                     uint32_t value, int nbytes)
{
  int ret;
  int i;

  for (i = 0; i < nbytes; i++)
    {
      ret = rk3576_imx415_write(dev, (uint16_t)(addr + i),
                                (uint8_t)((value >> (8 * i)) & 0xff));
      if (ret < 0)
        {
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_imx415_write_table
 *
 * Description:
 *   Write a register initialisation table, aborting on the first error.
 *
 ****************************************************************************/

static int rk3576_imx415_write_table(struct imx415_dev_s *dev,
                                     const struct imx415_regval_s *table,
                                     size_t nentries)
{
  size_t i;
  int ret;

  for (i = 0; i < nentries; i++)
    {
      ret = rk3576_imx415_write(dev, table[i].addr, table[i].val);
      if (ret < 0)
        {
          verr("ERROR: write 0x%04x = 0x%02x failed: %d\n",
               table[i].addr, table[i].val, ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_imx415_power_up
 *
 * Description:
 *   Run the Sony power-up sequence for one module: assert power-down and
 *   reset (when wired), enable the master clock, then release power-down
 *   and reset with the required settling delays.  The analog supply itself
 *   is a fixed always-on regulator on this board (avdd-supply in the vendor
 *   DTS), so no rail has to be switched here.
 *
 ****************************************************************************/

static int rk3576_imx415_power_up(const struct imx415_config_s *cfg)
{
  int ret;

  /* Hold the sensor down while the clock starts */

  if (cfg->has_pwdn)
    {
      ret = rk3576_config_gpio(cfg->pwdn_pin);
      if (ret < 0)
        {
          return ret;
        }

      rk3576_gpio_write(cfg->pwdn_pin, false);
    }

  if (cfg->has_reset)
    {
      ret = rk3576_config_gpio(cfg->reset_pin);
      if (ret < 0)
        {
          return ret;
        }

      rk3576_gpio_write(cfg->reset_pin, false);
    }

  up_mdelay(IMX415_T_POWER_TO_CLK_MS);

  /* XVCLK must be running before XCLR is released */

  ret = rk3576_imx415_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(IMX415_T_CLK_TO_XCLR_MS);

  if (cfg->has_pwdn)
    {
      rk3576_gpio_write(cfg->pwdn_pin, true);
      up_mdelay(IMX415_T_CLK_TO_XCLR_MS);
    }

  if (cfg->has_reset)
    {
      rk3576_gpio_write(cfg->reset_pin, true);
    }

  /* Internal regulators and the register block need time before the first
   * I2C access is accepted.
   */

  up_mdelay(IMX415_T_XCLR_TO_I2C_MS);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_imx415_initialize
 ****************************************************************************/

struct imx415_dev_s *rk3576_imx415_initialize(
    int i2c_port, uint8_t i2c_addr, const struct imx415_config_s *cfg)
{
  struct imx415_dev_s *dev;
  uint8_t regval;
  int ret;

  if (cfg == NULL || cfg->num_lanes < 1 ||
      cfg->num_lanes > RK3576_CSI_MAX_LANES)
    {
      verr("ERROR: invalid IMX415 configuration\n");
      return NULL;
    }

  if (g_imx415_ndevs >= IMX415_MAX_DEVICES)
    {
      verr("ERROR: no free IMX415 slot (max %d)\n", IMX415_MAX_DEVICES);
      return NULL;
    }

  dev = &g_imx415_dev[g_imx415_ndevs];
  memset(dev, 0, sizeof(*dev));

  dev->cfg  = cfg;
  dev->addr = i2c_addr;

  ret = rk3576_imx415_power_up(cfg);
  if (ret < 0)
    {
      verr("ERROR: IMX415 power-up on i2c%d failed: %d\n", i2c_port, ret);
      return NULL;
    }

  dev->i2c = rk3576_i2c_initialize(i2c_port);
  if (dev->i2c == NULL)
    {
      verr("ERROR: failed to initialize i2c%d\n", i2c_port);
      return NULL;
    }

  /* Probe: after reset the sensor sits in standby, so MODE reads back as 1.
   * The IMX415 has no public chip-id register, so this doubles as the
   * identification step - it proves address 0x37 acknowledges and returns
   * the documented reset value.
   */

  ret = rk3576_imx415_read(dev, IMX415_REG_MODE, &regval);
  if (ret < 0)
    {
      verr("ERROR: IMX415 at i2c%d/0x%02x does not answer: %d\n",
           i2c_port, i2c_addr, ret);
      return NULL;
    }

  if (regval != IMX415_MODE_STANDBY)
    {
      vwarn("WARNING: IMX415 MODE reads 0x%02x, expected 0x%02x\n",
            regval, IMX415_MODE_STANDBY);
    }

  /* Load the settings shared by all modes while still in standby */

  ret = rk3576_imx415_write_table(dev, g_imx415_common_regs,
                                  nitems(g_imx415_common_regs));
  if (ret < 0)
    {
      return NULL;
    }

  g_imx415_ndevs++;

  vinfo("IMX415 ready on i2c%d addr 0x%02x, XVCLK %" PRIu32 " Hz, "
        "%d lanes -> csi%d/dphy%d\n",
        i2c_port, i2c_addr, g_imx415_xvclk_hz, cfg->num_lanes,
        cfg->csi_id, cfg->dphy_id);

  return dev;
}

/****************************************************************************
 * Name: rk3576_imx415_get_modeinfo
 ****************************************************************************/

const struct imx415_modeinfo_s *rk3576_imx415_get_modeinfo(
    enum imx415_mode_e mode)
{
  if ((unsigned int)mode >= IMX415_MODE_COUNT)
    {
      return NULL;
    }

  return &g_imx415_modeinfo[mode];
}

/****************************************************************************
 * Name: rk3576_imx415_set_mode
 ****************************************************************************/

int rk3576_imx415_set_mode(struct imx415_dev_s *dev, enum imx415_mode_e mode)
{
  int ret;

  if (dev == NULL || (unsigned int)mode >= IMX415_MODE_COUNT)
    {
      return -EINVAL;
    }

  if (dev->streaming)
    {
      verr("ERROR: cannot change mode while streaming\n");
      return -EBUSY;
    }

  ret = rk3576_imx415_write_table(dev, g_imx415_mode_table[mode],
                                  g_imx415_mode_table_len[mode]);
  if (ret < 0)
    {
      return ret;
    }

  dev->mode       = mode;
  dev->vmax       = g_imx415_modeinfo[mode].vmax;
  dev->mode_valid = true;

  vinfo("IMX415 mode %ux%u@%u, %" PRIu32 " Mbps/lane\n",
        g_imx415_modeinfo[mode].width, g_imx415_modeinfo[mode].height,
        g_imx415_modeinfo[mode].fps, g_imx415_modeinfo[mode].lane_rate_mbps);

  return OK;
}

/****************************************************************************
 * Name: rk3576_imx415_set_exposure
 ****************************************************************************/

int rk3576_imx415_set_exposure(struct imx415_dev_s *dev, uint32_t lines)
{
  uint32_t shr0;
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (!dev->mode_valid)
    {
      verr("ERROR: exposure requested before a mode was set\n");
      return -EPERM;
    }

  if (lines < IMX415_EXPOSURE_MIN ||
      lines > dev->vmax - IMX415_SHR0_MIN)
    {
      verr("ERROR: exposure %" PRIu32 " lines out of range (%u..%" PRIu32
           ")\n", lines, IMX415_EXPOSURE_MIN,
           dev->vmax - IMX415_SHR0_MIN);
      return -ERANGE;
    }

  /* Integration time = VMAX - SHR0, and SHR0 must keep a minimum sweep at
   * both ends of the range.
   */

  shr0 = dev->vmax - lines;
  if (shr0 < IMX415_SHR0_MIN)
    {
      shr0 = IMX415_SHR0_MIN;
    }
  else if (shr0 > dev->vmax - IMX415_SHR0_MARGIN)
    {
      shr0 = dev->vmax - IMX415_SHR0_MARGIN;
    }

  /* Group the update so the sensor applies it on a frame boundary */

  ret = rk3576_imx415_write(dev, IMX415_REG_REGHOLD, IMX415_REGHOLD_HOLD);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_imx415_write_bytes(dev, IMX415_REG_SHR0,
                                  shr0 & IMX415_SHR0_MASK, 3);

  /* Always release the hold, even after a failed write */

  rk3576_imx415_write(dev, IMX415_REG_REGHOLD, IMX415_REGHOLD_RELEASE);
  return ret;
}

/****************************************************************************
 * Name: rk3576_imx415_set_gain
 ****************************************************************************/

int rk3576_imx415_set_gain(struct imx415_dev_s *dev, uint32_t gain)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (gain > IMX415_GAIN_MAX)
    {
      return -ERANGE;
    }

  ret = rk3576_imx415_write(dev, IMX415_REG_REGHOLD, IMX415_REGHOLD_HOLD);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_imx415_write_bytes(dev, IMX415_REG_GAIN_PCG_0,
                                  gain & IMX415_GAIN_MASK, 2);

  rk3576_imx415_write(dev, IMX415_REG_REGHOLD, IMX415_REGHOLD_RELEASE);
  return ret;
}

/****************************************************************************
 * Name: rk3576_imx415_stream_on
 ****************************************************************************/

int rk3576_imx415_stream_on(struct imx415_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (!dev->mode_valid)
    {
      verr("ERROR: stream requested before a mode was set\n");
      return -EPERM;
    }

  if (dev->streaming)
    {
      return OK;
    }

  /* Leave standby, wait for the internal clocks to settle, then release
   * master mode so that the sensor starts driving the MIPI link.
   */

  ret = rk3576_imx415_write(dev, IMX415_REG_MODE, IMX415_MODE_OPERATING);
  if (ret < 0)
    {
      return ret;
    }

  up_mdelay(IMX415_T_STANDBY_EXIT_MS);

  ret = rk3576_imx415_write(dev, IMX415_REG_XMSTA, IMX415_XMSTA_START);
  if (ret < 0)
    {
      rk3576_imx415_write(dev, IMX415_REG_MODE, IMX415_MODE_STANDBY);
      return ret;
    }

  dev->streaming = true;
  return OK;
}

/****************************************************************************
 * Name: rk3576_imx415_stream_off
 ****************************************************************************/

int rk3576_imx415_stream_off(struct imx415_dev_s *dev)
{
  int ret;

  if (dev == NULL)
    {
      return -EINVAL;
    }

  if (!dev->streaming)
    {
      return OK;
    }

  ret = rk3576_imx415_write(dev, IMX415_REG_XMSTA, IMX415_XMSTA_STOP);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_imx415_write(dev, IMX415_REG_MODE, IMX415_MODE_STANDBY);
  if (ret < 0)
    {
      return ret;
    }

  dev->streaming = false;
  return OK;
}

#endif /* CONFIG_KICKPI_K7_IMX415 */
