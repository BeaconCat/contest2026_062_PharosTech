/****************************************************************************
 * chips/rk3576/rk3576_tsadc.c
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
 * RK3576 Temperature-Sensor ADC (TS-ADC) driver.
 *
 * The controller owns six on-die temperature sensors (SoC, big core, little
 * core, DDR, NPU, GPU).  It is driven in "auto mode": the hardware loops
 * over the enabled channels forever, latching each result into TSADC_DATAn
 * and comparing it against two per-channel thresholds:
 *
 *   COMPn_INT   crossing it raises irq_tsadc (high-temperature interrupt)
 *   COMPn_SHUT  crossing it asserts the thermal-shutdown event, which can
 *               be routed to the TSADC_SHUT pad (PMIC) and/or to the CRU
 *               (immediate SoC reset)
 *
 * Readers therefore never trigger a conversion; they just sample the most
 * recent result, which makes rk3576_tsadc_get_temp() safe to call from any
 * context including a thermal-throttling loop.
 *
 * Board policy on the KICKPI-K7 follows the vendor device tree
 * (4-HardwareData/k7_debian_vendor.dts):
 *
 *   thermal-zones trips        critical at 115 C on every zone
 *   rockchip,hw-tshut-temp     120000  (120 C)
 *   rockchip,hw-tshut-mode     0       -> route shutdown to the pad, not
 *                                        to the CRU
 *   rockchip,hw-tshut-polarity 0       -> pad is active low
 *
 * Raw conversion codes are turned into milli-degrees Celsius through the
 * Rockchip v4 calibration table with linear interpolation between entries.
 *
 * The channels are exported two ways: through the NuttX sensor framework as
 * SENSOR_TYPE_AMBIENT_TEMPERATURE fetch-only devices, and directly through
 * rk3576_tsadc_get_temp() for in-kernel users (DVFS, fan control).
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
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#ifdef CONFIG_SENSORS
#  include <nuttx/fs/fs.h>
#  include <nuttx/sensors/sensor.h>
#endif

#include "arm64_internal.h"
#include "hardware/rk3576_tsadc.h"
#include "rk3576_tsadc.h"

#ifdef CONFIG_RK3576_TSADC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Thermal policy, taken from the vendor device tree (see file header). */

#define RK3576_TSADC_TRIP_INT_MC   115000 /* thermal-zones critical trip */
#define RK3576_TSADC_TRIP_SHUT_MC  120000 /* rockchip,hw-tshut-temp      */

/* Time one channel spends in the conversion loop, in milliseconds.  The
 * hardware counts clk_tsadc cycles, so the register value depends on the
 * clock rate reported by the CLK framework.
 */

#define RK3576_TSADC_PERIOD_MS 2

/* Number of consecutive out-of-range conversions required before the
 * corresponding event is raised (suppresses single-sample glitches).
 */

#define RK3576_TSADC_DEBOUNCE 4

/* Conversion-table bounds, milli-degrees Celsius. */

#define RK3576_TSADC_TEMP_MIN_MC (-40000)
#define RK3576_TSADC_TEMP_MAX_MC 125000

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of the code-to-temperature calibration table. */

struct rk3576_tsadc_entry_s
{
  uint16_t code;   /* Raw conversion result   */
  int32_t temp_mc; /* Milli-degrees Celsius   */
};

struct rk3576_tsadc_dev_s
{
  uintptr_t base;                 /* Register base address              */
  int irq;                        /* GIC interrupt number               */
  uint32_t clk_hz;                /* clk_tsadc rate, Hz                 */
  uint32_t chanmask;              /* Channels taking part in auto mode  */
  bool initialized;               /* Controller is up and looping       */
  rk3576_tsadc_alarm_cb_t alarm;  /* Over-temperature notification      */
  void *alarm_arg;                /* Opaque callback argument           */
};

#ifdef CONFIG_SENSORS
struct rk3576_tsadc_sensor_s
{
  struct sensor_lowerhalf_s lower; /* Base class (must be first)        */
  int ch;                          /* TS-ADC channel index              */
  bool enabled;                    /* Set by the activate() operation   */
};
#endif

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_tsadc_getreg(unsigned int off);
static void rk3576_tsadc_putreg(unsigned int off, uint32_t val);
static int32_t rk3576_tsadc_code_to_temp(uint32_t code);
static uint32_t rk3576_tsadc_temp_to_code(int32_t temp_mc);
static int rk3576_tsadc_clk_init(void);
static void rk3576_tsadc_hw_init(void);
static int rk3576_tsadc_interrupt(int irq, void *context, void *arg);

#ifdef CONFIG_SENSORS
static int rk3576_tsadc_activate(struct sensor_lowerhalf_s *lower,
                                 struct file *filep, bool enabled);
static int rk3576_tsadc_fetch(struct sensor_lowerhalf_s *lower,
                              struct file *filep, char *buffer,
                              size_t buflen);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_tsadc_dev_s g_rk3576_tsadc =
{
  .base        = RK3576_TSADC_ADDR,
  .irq         = RK3576_IRQ_TSADC,
  .clk_hz      = RK3576_TSADC_CLK_HZ,
  .chanmask    = 0,
  .initialized = false,
  .alarm       = NULL,
  .alarm_arg   = NULL,
};

/* Rockchip v4 code-to-temperature calibration table, shared by the RK3588
 * and RK3576 TS-ADC generations.  The codes rise monotonically with
 * temperature once TSADC_AUTO_CON.Q_SEL is set, which is what the driver
 * programs below.
 *
 * TODO: cross-check the table against RK3576 TRM chapter 19 once the
 * temperature-calibration section is available; the entries below are the
 * documented 5 C grid of the v4 sensor and have not been trimmed with the
 * per-die OTP calibration value yet, so absolute accuracy is roughly
 * +/- 5 C until that trim is applied.
 */

static const struct rk3576_tsadc_entry_s g_rk3576_tsadc_table[] =
{
  {    0, -40000 }, {  215, -40000 }, {  285, -35000 }, {  354, -30000 },
  {  424, -25000 }, {  493, -20000 }, {  562, -15000 }, {  632, -10000 },
  {  702,  -5000 }, {  771,      0 }, {  841,   5000 }, {  910,  10000 },
  {  980,  15000 }, { 1050,  20000 }, { 1119,  25000 }, { 1189,  30000 },
  { 1259,  35000 }, { 1328,  40000 }, { 1398,  45000 }, { 1468,  50000 },
  { 1538,  55000 }, { 1608,  60000 }, { 1678,  65000 }, { 1747,  70000 },
  { 1817,  75000 }, { 1887,  80000 }, { 1957,  85000 }, { 2027,  90000 },
  { 2097,  95000 }, { 2167, 100000 }, { 2237, 105000 }, { 2307, 110000 },
  { 2377, 115000 }, { 2447, 120000 }, { 2517, 125000 },
  { TSADC_DATA_MASK, 125000 },
};

#define RK3576_TSADC_TABLE_LEN \
  (sizeof(g_rk3576_tsadc_table) / sizeof(g_rk3576_tsadc_table[0]))

#ifdef CONFIG_SENSORS
static const struct sensor_ops_s g_rk3576_tsadc_sensor_ops =
{
  .activate = rk3576_tsadc_activate,
  .fetch    = rk3576_tsadc_fetch,
};

static struct rk3576_tsadc_sensor_s
  g_rk3576_tsadc_sensor[RK3576_TSADC_NCHAN];
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_tsadc_getreg(unsigned int off)
{
  return getreg32(g_rk3576_tsadc.base + off);
}

static void rk3576_tsadc_putreg(unsigned int off, uint32_t val)
{
  putreg32(val, g_rk3576_tsadc.base + off);
}

/****************************************************************************
 * Name: rk3576_tsadc_code_to_temp
 *
 * Description:
 *   Convert a raw conversion result into milli-degrees Celsius by linear
 *   interpolation inside the calibration table.  Codes outside the table
 *   are clamped to its end points.
 *
 ****************************************************************************/

static int32_t rk3576_tsadc_code_to_temp(uint32_t code)
{
  size_t i;

  if (code <= g_rk3576_tsadc_table[0].code)
    {
      return RK3576_TSADC_TEMP_MIN_MC;
    }

  for (i = 1; i < RK3576_TSADC_TABLE_LEN; i++)
    {
      const struct rk3576_tsadc_entry_s *hi = &g_rk3576_tsadc_table[i];
      const struct rk3576_tsadc_entry_s *lo = &g_rk3576_tsadc_table[i - 1];
      int32_t span;

      if (code > hi->code)
        {
          continue;
        }

      span = (int32_t)hi->code - (int32_t)lo->code;
      if (span <= 0)
        {
          return hi->temp_mc;
        }

      return lo->temp_mc + ((hi->temp_mc - lo->temp_mc) *
                            ((int32_t)code - (int32_t)lo->code)) / span;
    }

  return RK3576_TSADC_TEMP_MAX_MC;
}

/****************************************************************************
 * Name: rk3576_tsadc_temp_to_code
 *
 * Description:
 *   Inverse of rk3576_tsadc_code_to_temp(), used to program the comparator
 *   thresholds.  Temperatures outside the table are clamped.
 *
 ****************************************************************************/

static uint32_t rk3576_tsadc_temp_to_code(int32_t temp_mc)
{
  size_t i;

  if (temp_mc <= RK3576_TSADC_TEMP_MIN_MC)
    {
      return g_rk3576_tsadc_table[1].code;
    }

  for (i = 1; i < RK3576_TSADC_TABLE_LEN; i++)
    {
      const struct rk3576_tsadc_entry_s *hi = &g_rk3576_tsadc_table[i];
      const struct rk3576_tsadc_entry_s *lo = &g_rk3576_tsadc_table[i - 1];
      int32_t span;

      if (temp_mc > hi->temp_mc)
        {
          continue;
        }

      span = hi->temp_mc - lo->temp_mc;
      if (span <= 0)
        {
          return hi->code;
        }

      return (uint32_t)((int32_t)lo->code +
                        (((int32_t)hi->code - (int32_t)lo->code) *
                         (temp_mc - lo->temp_mc)) / span);
    }

  return TSADC_DATA_MASK;
}

/****************************************************************************
 * Name: rk3576_tsadc_clk_init
 *
 * Description:
 *   Ungate the TS-ADC APB and functional clocks and latch the real
 *   functional clock rate, which the conversion-period arithmetic needs.
 *   All clock handling of this driver is confined to this function.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_tsadc_clk_init(void)
{
  struct clk_s *pclk;
  struct clk_s *fclk;
  uint32_t rate;
  int ret;

  pclk = clk_get("pclk_tsadc_en");
  if (pclk == NULL)
    {
      snerr("ERROR: failed to get pclk_tsadc_en\n");
      return -ENODEV;
    }

  ret = clk_enable(pclk);
  if (ret < 0)
    {
      snerr("ERROR: failed to enable pclk_tsadc_en: %d\n", ret);
      return ret;
    }

  fclk = clk_get("clk_tsadc_en");
  if (fclk == NULL)
    {
      snerr("ERROR: failed to get clk_tsadc_en\n");
      return -ENODEV;
    }

  ret = clk_enable(fclk);
  if (ret < 0)
    {
      snerr("ERROR: failed to enable clk_tsadc_en: %d\n", ret);
      return ret;
    }

  rate = clk_get_rate(fclk);
  if (rate == 0)
    {
      snerr("ERROR: clk_tsadc_en reports a zero rate\n");
      return -EINVAL;
    }

  g_rk3576_tsadc.clk_hz = rate;
  return OK;
}

/****************************************************************************
 * Name: rk3576_tsadc_hw_init
 *
 * Description:
 *   Program the conversion loop, the per-channel thresholds and the
 *   shutdown routing, then start auto mode.  The clocks must already be
 *   running and g_rk3576_tsadc.chanmask must be valid.
 *
 ****************************************************************************/

static void rk3576_tsadc_hw_init(void)
{
  uint32_t mask = g_rk3576_tsadc.chanmask;
  uint32_t period;
  uint32_t int_code;
  uint32_t shut_code;
  int ch;

  /* Stop the conversion loop and leave user (single-shot) mode powered
   * down while the thresholds are being programmed.
   */

  rk3576_tsadc_putreg(RK3576_TSADC_AUTO_CON,
                      RK3576_TSADC_WE_CLR(TSADC_AUTO_CON_AUTO_EN));
  rk3576_tsadc_putreg(RK3576_TSADC_USER_CON,
                      RK3576_TSADC_WE_CLR(TSADC_USER_CON_POWER_UP |
                                          TSADC_USER_CON_START_MODE_SW |
                                          TSADC_USER_CON_EOC_INTEN));

  /* Time each channel spends in the loop.  The same interleave is used
   * once a high-temperature threshold has been crossed, so throttling
   * software keeps getting samples at the nominal rate.
   */

  period = (g_rk3576_tsadc.clk_hz / 1000u) * RK3576_TSADC_PERIOD_MS;
  rk3576_tsadc_putreg(RK3576_TSADC_AUTO_PERIOD, period);
  rk3576_tsadc_putreg(RK3576_TSADC_AUTO_PERIOD_HT, period);

  rk3576_tsadc_putreg(RK3576_TSADC_HIGH_INT_DEBOUNCE,
                      RK3576_TSADC_DEBOUNCE);
  rk3576_tsadc_putreg(RK3576_TSADC_HIGH_TSHUT_DEBOUNCE,
                      RK3576_TSADC_DEBOUNCE);

  /* Per-channel comparator thresholds. */

  int_code  = rk3576_tsadc_temp_to_code(RK3576_TSADC_TRIP_INT_MC);
  shut_code = rk3576_tsadc_temp_to_code(RK3576_TSADC_TRIP_SHUT_MC);

  for (ch = 0; ch < RK3576_TSADC_NCHAN; ch++)
    {
      rk3576_tsadc_putreg(RK3576_TSADC_COMP_INT(ch), int_code);
      rk3576_tsadc_putreg(RK3576_TSADC_COMP_SHUT(ch), shut_code);
    }

  /* Low-temperature checking is not used. */

  rk3576_tsadc_putreg(RK3576_TSADC_LT_EN,
                      RK3576_TSADC_WE_VAL(0, TSADC_CHAN_MASK));
  rk3576_tsadc_putreg(RK3576_TSADC_LT_INT_EN,
                      RK3576_TSADC_WE_VAL(0, TSADC_CHAN_MASK));

  /* Enable the requested channels, their high-temperature interrupt and
   * the shutdown path.  hw-tshut-mode = 0 in the device tree means the
   * shutdown event drives the TSADC_SHUT pad towards the PMIC; the CRU
   * reset path stays disabled so software gets a chance to react first.
   */

  rk3576_tsadc_putreg(RK3576_TSADC_AUTO_SRC,
                      RK3576_TSADC_WE_VAL(mask, TSADC_CHAN_MASK));
  rk3576_tsadc_putreg(RK3576_TSADC_HT_INT_EN,
                      RK3576_TSADC_WE_VAL(mask, TSADC_CHAN_MASK));
  rk3576_tsadc_putreg(RK3576_TSADC_GPIO_EN,
                      RK3576_TSADC_WE_VAL(mask, TSADC_CHAN_MASK));
  rk3576_tsadc_putreg(RK3576_TSADC_CRU_EN,
                      RK3576_TSADC_WE_VAL(0, TSADC_CHAN_MASK));

  /* Acknowledge anything the boot loader may have left pending. */

  rk3576_tsadc_putreg(RK3576_TSADC_HLT_INT_PD,
                      TSADC_HLT_INT_PD_HT_ALL | TSADC_HLT_INT_PD_LT_ALL);
  rk3576_tsadc_putreg(RK3576_TSADC_EOC_HSHUT_PD,
                      TSADC_EOC_HSHUT_PD_SHUT_ALL |
                      TSADC_EOC_HSHUT_PD_USR_EOC |
                      TSADC_EOC_HSHUT_PD_ROUND);

  /* Start the loop.  Q_SEL selects the (q_max - q) result polarity, which
   * is the one the v4 calibration table above is expressed in; the
   * TSHUT pad is active low (hw-tshut-polarity = 0).
   */

  rk3576_tsadc_putreg(RK3576_TSADC_AUTO_CON,
                      RK3576_TSADC_WE(TSADC_AUTO_CON_AUTO_EN |
                                      TSADC_AUTO_CON_Q_SEL) |
                      RK3576_TSADC_WE_CLR(TSADC_AUTO_CON_TSHUT_POL_HIGH |
                                          TSADC_AUTO_CON_ROUND_INT_EN));
}

/****************************************************************************
 * Name: rk3576_tsadc_interrupt
 *
 * Description:
 *   Over-temperature interrupt handler.  Acknowledges the high-temperature
 *   and thermal-shutdown status bits and forwards them to the registered
 *   alarm callback, if any.
 *
 ****************************************************************************/

static int rk3576_tsadc_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_tsadc_dev_s *dev = (struct rk3576_tsadc_dev_s *)arg;
  uint32_t hltpd;
  uint32_t shutpd;
  uint32_t chanmask;
  uint32_t shutmask;

  hltpd  = rk3576_tsadc_getreg(RK3576_TSADC_HLT_INT_PD);
  shutpd = rk3576_tsadc_getreg(RK3576_TSADC_EOC_HSHUT_PD);

  /* Write 1 to clear.  Only the bits that were actually latched are
   * acknowledged so a threshold crossed in between is not lost.
   */

  if (hltpd != 0)
    {
      rk3576_tsadc_putreg(RK3576_TSADC_HLT_INT_PD, hltpd);
    }

  if (shutpd != 0)
    {
      rk3576_tsadc_putreg(RK3576_TSADC_EOC_HSHUT_PD, shutpd);
    }

  chanmask = hltpd & TSADC_CHAN_MASK;
  shutmask = shutpd & TSADC_EOC_HSHUT_PD_SHUT_ALL;

  if ((chanmask | shutmask) == 0)
    {
      return OK;
    }

  snwarn("TSADC over-temperature: int=0x%02" PRIx32 " shut=0x%02" PRIx32
         "\n", chanmask, shutmask);

  if (dev->alarm != NULL)
    {
      dev->alarm(chanmask, shutmask, dev->alarm_arg);
    }

  return OK;
}

#ifdef CONFIG_SENSORS

/****************************************************************************
 * Name: rk3576_tsadc_activate
 *
 * Description:
 *   Sensor framework activate operation.  The conversion loop runs
 *   unconditionally in hardware, so this only records the state and makes
 *   a disabled sensor reject fetch().
 *
 ****************************************************************************/

static int rk3576_tsadc_activate(struct sensor_lowerhalf_s *lower,
                                 struct file *filep, bool enabled)
{
  struct rk3576_tsadc_sensor_s *priv =
    (struct rk3576_tsadc_sensor_s *)lower;

  priv->enabled = enabled;
  return OK;
}

/****************************************************************************
 * Name: rk3576_tsadc_fetch
 *
 * Description:
 *   Sensor framework fetch operation: return the most recent conversion
 *   result of this channel as a struct sensor_temp.
 *
 ****************************************************************************/

static int rk3576_tsadc_fetch(struct sensor_lowerhalf_s *lower,
                              struct file *filep, char *buffer,
                              size_t buflen)
{
  struct rk3576_tsadc_sensor_s *priv =
    (struct rk3576_tsadc_sensor_s *)lower;
  struct sensor_temp temp;
  struct timespec ts;
  int millicelsius;
  int ret;

  if (buflen < sizeof(temp))
    {
      return -EINVAL;
    }

  if (!priv->enabled)
    {
      return -EAGAIN;
    }

  ret = rk3576_tsadc_get_temp(priv->ch, &millicelsius);
  if (ret < 0)
    {
      return ret;
    }

  clock_systime_timespec(&ts);

  temp.timestamp   = 1000000ull * (uint64_t)ts.tv_sec +
                     (uint64_t)ts.tv_nsec / 1000ull;
  temp.temperature = (float)millicelsius / 1000.0f;

  memcpy(buffer, &temp, sizeof(temp));
  return (int)sizeof(temp);
}

#endif /* CONFIG_SENSORS */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_tsadc_initialize
 *
 * Description:
 *   See rk3576_tsadc.h.
 *
 ****************************************************************************/

int rk3576_tsadc_initialize(uint32_t chanmask)
{
  int ret;

  if (g_rk3576_tsadc.initialized)
    {
      return OK;
    }

  chanmask &= TSADC_CHAN_MASK;
  if (chanmask == 0)
    {
      chanmask = TSADC_CHAN_MASK;
    }

  g_rk3576_tsadc.chanmask = chanmask;

  ret = rk3576_tsadc_clk_init();
  if (ret < 0)
    {
      return ret;
    }

  rk3576_tsadc_hw_init();

  ret = irq_attach(g_rk3576_tsadc.irq, rk3576_tsadc_interrupt,
                   &g_rk3576_tsadc);
  if (ret < 0)
    {
      snerr("ERROR: irq_attach(%d) failed: %d\n", g_rk3576_tsadc.irq, ret);
      rk3576_tsadc_putreg(RK3576_TSADC_AUTO_CON,
                          RK3576_TSADC_WE_CLR(TSADC_AUTO_CON_AUTO_EN));
      return ret;
    }

  up_enable_irq(g_rk3576_tsadc.irq);

  g_rk3576_tsadc.initialized = true;

  sninfo("TSADC up: base=%08" PRIxPTR " clk=%" PRIu32 "Hz chans=0x%02"
         PRIx32 " int=%dmC shut=%dmC\n",
         g_rk3576_tsadc.base, g_rk3576_tsadc.clk_hz, chanmask,
         RK3576_TSADC_TRIP_INT_MC, RK3576_TSADC_TRIP_SHUT_MC);
  return OK;
}

/****************************************************************************
 * Name: rk3576_tsadc_get_temp
 *
 * Description:
 *   See rk3576_tsadc.h.
 *
 ****************************************************************************/

int rk3576_tsadc_get_temp(int ch, int *millicelsius)
{
  uint32_t code;

  if (ch < 0 || ch >= RK3576_TSADC_NCHAN || millicelsius == NULL)
    {
      return -EINVAL;
    }

  if (!g_rk3576_tsadc.initialized)
    {
      return -ENODEV;
    }

  if ((g_rk3576_tsadc.chanmask & TSADC_CHAN_BIT(ch)) == 0)
    {
      return -EINVAL;
    }

  code = rk3576_tsadc_getreg(RK3576_TSADC_DATA(ch)) & TSADC_DATA_MASK;

  /* A channel that has not been converted yet reads back as zero, and the
   * all-ones pattern means the sensor is not driving a valid result.
   */

  if (code == 0 || code == TSADC_DATA_MASK)
    {
      return -EAGAIN;
    }

  *millicelsius = (int)rk3576_tsadc_code_to_temp(code);
  return OK;
}

/****************************************************************************
 * Name: rk3576_tsadc_set_alarm_cb
 *
 * Description:
 *   See rk3576_tsadc.h.
 *
 ****************************************************************************/

int rk3576_tsadc_set_alarm_cb(rk3576_tsadc_alarm_cb_t cb, void *arg)
{
  irqstate_t flags = enter_critical_section();

  g_rk3576_tsadc.alarm     = cb;
  g_rk3576_tsadc.alarm_arg = arg;

  leave_critical_section(flags);
  return OK;
}

#ifdef CONFIG_SENSORS

/****************************************************************************
 * Name: rk3576_tsadc_register
 *
 * Description:
 *   See rk3576_tsadc.h.
 *
 ****************************************************************************/

int rk3576_tsadc_register(int ch, int devno)
{
  struct rk3576_tsadc_sensor_s *priv;
  int ret;

  if (ch < 0 || ch >= RK3576_TSADC_NCHAN)
    {
      return -EINVAL;
    }

  if (!g_rk3576_tsadc.initialized)
    {
      return -ENODEV;
    }

  priv = &g_rk3576_tsadc_sensor[ch];
  if (priv->lower.ops != NULL)
    {
      return -EEXIST;
    }

  priv->ch            = ch;
  priv->enabled       = false;
  priv->lower.ops     = &g_rk3576_tsadc_sensor_ops;
  priv->lower.type    = SENSOR_TYPE_AMBIENT_TEMPERATURE;

  /* Fetch-only device: the hardware keeps a single latched result per
   * channel, so no circular buffer is needed.
   */

  priv->lower.nbuffer = 0;

  ret = sensor_register(&priv->lower, devno);
  if (ret < 0)
    {
      snerr("ERROR: sensor_register(ch=%d, devno=%d) failed: %d\n", ch,
            devno, ret);
      priv->lower.ops = NULL;
      return ret;
    }

  sninfo("TSADC channel %d registered as sensor_temp%d\n", ch, devno);
  return OK;
}

#endif /* CONFIG_SENSORS */

#endif /* CONFIG_RK3576_TSADC */
