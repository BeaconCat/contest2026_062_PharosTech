/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_hym8563.c
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
 * Haoyu HYM8563 I2C real time clock, as fitted on the KICKPI-K7 at
 * i2c2 (i2c@2AC50000) slave address 0x51.  The driver implements the NuttX
 * RTC lower half (struct rtc_ops_s) and registers /dev/rtc0 through
 * rtc_initialize().
 *
 * Two board specifics matter here:
 *
 * 1. The CLKOUT pin of this RTC is the 32.768 kHz low speed clock source of
 *    the on-board SV6621 WiFi/BT companion chip.  Register 0x0D must stay
 *    at 0xC4 (FE=1, FD=00 -> 32.768 kHz).  This driver never clears it; it
 *    only restores the value when it finds CLKOUT disabled.
 *
 * 2. The alarm interrupt line (GPIO0_A0) has no raw IRQ attach interface in
 *    the RK3576 GPIO driver, so alarm expiry is detected by polling the AF
 *    flag of Control_Status2 from the low priority work queue.  The alarm
 *    resolution of the HYM8563 is one minute, so a one second poll adds no
 *    meaningful error.  The poll only runs while an alarm is armed.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <nuttx/clock.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/mutex.h>
#include <nuttx/timers/rtc.h>
#include <nuttx/wqueue.h>

#include "kickpi_k7_hym8563.h"

#ifdef CONFIG_KICKPI_K7_HYM8563

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Register map ************************************************************/

#define HYM8563_REG_CTL1        0x00 /* Control_Status1               */
#define HYM8563_REG_CTL2        0x01 /* Control_Status2               */
#define HYM8563_REG_SEC         0x02 /* Seconds (BCD) + VL            */
#define HYM8563_REG_MIN         0x03 /* Minutes (BCD)                 */
#define HYM8563_REG_HOUR        0x04 /* Hours (BCD)                   */
#define HYM8563_REG_DAY         0x05 /* Day of month (BCD)            */
#define HYM8563_REG_WEEKDAY     0x06 /* Day of week (binary 0-6)      */
#define HYM8563_REG_MONTH       0x07 /* Month (BCD) + century         */
#define HYM8563_REG_YEAR        0x08 /* Year (BCD, 00-99)             */
#define HYM8563_REG_ALM_MIN     0x09 /* Minute alarm                  */
#define HYM8563_REG_ALM_HOUR    0x0a /* Hour alarm                    */
#define HYM8563_REG_ALM_DAY     0x0b /* Day of month alarm            */
#define HYM8563_REG_ALM_WEEKDAY 0x0c /* Day of week alarm             */
#define HYM8563_REG_CLKOUT      0x0d /* CLKOUT frequency              */
#define HYM8563_REG_TIMER_CTL   0x0e /* Timer control                 */
#define HYM8563_REG_TIMER       0x0f /* Timer countdown value         */

/* Control_Status1 bits */

#define HYM8563_CTL1_TEST  (1 << 7)
#define HYM8563_CTL1_STOP  (1 << 5) /* 1: clock chain halted      */
#define HYM8563_CTL1_TESTC (1 << 3)

/* Control_Status2 bits */

#define HYM8563_CTL2_TI_TP (1 << 4)
#define HYM8563_CTL2_AF    (1 << 3) /* Alarm flag (W0C)          */
#define HYM8563_CTL2_TF    (1 << 2) /* Timer flag (W0C)          */
#define HYM8563_CTL2_AIE   (1 << 1) /* Alarm interrupt enable    */
#define HYM8563_CTL2_TIE   (1 << 0) /* Timer interrupt enable    */

/* Time field masks */

#define HYM8563_SEC_VL        (1 << 7) /* Voltage low: data invalid */
#define HYM8563_SEC_MASK      0x7f
#define HYM8563_MIN_MASK      0x7f
#define HYM8563_HOUR_MASK     0x3f
#define HYM8563_DAY_MASK      0x3f
#define HYM8563_WEEKDAY_MASK  0x07
#define HYM8563_MONTH_MASK    0x1f
#define HYM8563_MONTH_CENTURY (1 << 7) /* 1: year is 2100..2199     */

/* Alarm registers: bit 7 set disables comparison of that field */

#define HYM8563_ALM_DISABLE (1 << 7)

/* CLKOUT register.  0xC4 is the value measured on the KICKPI-K7 while the
 * SV6621 WiFi/BT companion was running: FE=1 (output enabled), FD=00
 * (32.768 kHz).  Do not clear this register, WiFi/BT dies with it.
 */

#define HYM8563_CLKOUT_FE         (1 << 7) /* Frequency output enable   */
#define HYM8563_CLKOUT_FD_MASK    0x03
#define HYM8563_CLKOUT_FD_32768HZ 0x00
#define HYM8563_CLKOUT_32768HZ    0xc4

/* I2C transfer parameters */

#define HYM8563_I2C_FREQUENCY 400000
#define HYM8563_I2C_ADDRLEN   7

/* Number of consecutive time registers read in one burst (0x02..0x08) */

#define HYM8563_TIME_NREGS 7

/* struct tm year origin, and the two centuries the part can express */

#define HYM8563_TM_YEAR_BASE 1900
#define HYM8563_YEAR_2000    100 /* tm_year for the year 2000      */
#define HYM8563_YEAR_2100    200 /* tm_year for the year 2100      */

/* Only a single alarm is implemented (the part has exactly one) */

#define HYM8563_ALARM_ID 0

/* Alarm flag poll period */

#define HYM8563_ALARM_POLL_TICKS SEC2TICK(1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct kickpi_k7_hym8563_s
{
  /* The lower half must be the first element so that the upper half can
   * cast between the two.
   */

  struct rtc_lowerhalf_s lower;

  struct i2c_master_s *i2c;
  struct i2c_config_s config;
  mutex_t lock;
  bool timeset; /* Time believed valid (VL clear)  */

#ifdef CONFIG_RTC_ALARM
  struct work_s work;      /* Alarm flag poller               */
  rtc_alarm_callback_t cb; /* Client callback                 */
  void *priv;              /* Client private data             */
  struct rtc_time alarm;   /* Alarm time as programmed        */
  bool armed;              /* Alarm currently armed           */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register level helpers */

static int rk3576_hym8563_getregs(struct kickpi_k7_hym8563_s *priv,
                                  uint8_t regaddr, uint8_t *regval, int nregs);
static int rk3576_hym8563_putregs(struct kickpi_k7_hym8563_s *priv,
                                  uint8_t regaddr, const uint8_t *regval,
                                  int nregs);
static int rk3576_hym8563_getreg(struct kickpi_k7_hym8563_s *priv,
                                 uint8_t regaddr, uint8_t *regval);
static int rk3576_hym8563_putreg(struct kickpi_k7_hym8563_s *priv,
                                 uint8_t regaddr, uint8_t regval);

/* BCD and calendar helpers */

static uint8_t rk3576_hym8563_bin2bcd(int bin);
static int rk3576_hym8563_bcd2bin(uint8_t bcd);
static int rk3576_hym8563_weekday(int year, int mon, int mday);

/* Board glue */

static int rk3576_hym8563_clkout_check(struct kickpi_k7_hym8563_s *priv);

/* rtc_ops_s methods */

static int rk3576_hym8563_rdtime(struct rtc_lowerhalf_s *lower,
                                 struct rtc_time *rtctime);
static int rk3576_hym8563_settime(struct rtc_lowerhalf_s *lower,
                                  const struct rtc_time *rtctime);
static bool rk3576_hym8563_havesettime(struct rtc_lowerhalf_s *lower);

#ifdef CONFIG_RTC_ALARM
static void rk3576_hym8563_worker(void *arg);
static int rk3576_hym8563_setalarm(struct rtc_lowerhalf_s *lower,
                                   const struct lower_setalarm_s *alarm);
static int rk3576_hym8563_setrelative(struct rtc_lowerhalf_s *lower,
                                      const struct lower_setrelative_s *alarm);
static int rk3576_hym8563_cancelalarm(struct rtc_lowerhalf_s *lower,
                                      int alarmid);
static int rk3576_hym8563_rdalarm(struct rtc_lowerhalf_s *lower,
                                  struct lower_rdalarm_s *alarm);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct rtc_ops_s g_hym8563_ops = {
  .rdtime = rk3576_hym8563_rdtime,
  .settime = rk3576_hym8563_settime,
  .havesettime = rk3576_hym8563_havesettime,
#ifdef CONFIG_RTC_ALARM
  .setalarm = rk3576_hym8563_setalarm,
  .setrelative = rk3576_hym8563_setrelative,
  .cancelalarm = rk3576_hym8563_cancelalarm,
  .rdalarm = rk3576_hym8563_rdalarm,
#endif
};

static struct kickpi_k7_hym8563_s g_hym8563 = {
  .lock = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_hym8563_getregs
 *
 * Description:
 *   Burst read nregs registers starting at regaddr.  The HYM8563 auto
 *   increments its address pointer and wraps at 0x0F.
 *
 ****************************************************************************/

static int rk3576_hym8563_getregs(struct kickpi_k7_hym8563_s *priv,
                                  uint8_t regaddr, uint8_t *regval, int nregs)
{
  int ret;

  ret = i2c_writeread(priv->i2c, &priv->config, &regaddr, 1, regval, nregs);
  if (ret < 0)
    {
      rtcerr("ERROR: read of register 0x%02x failed: %d\n", regaddr, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_putregs
 *
 * Description:
 *   Burst write nregs registers starting at regaddr.
 *
 ****************************************************************************/

static int rk3576_hym8563_putregs(struct kickpi_k7_hym8563_s *priv,
                                  uint8_t regaddr, const uint8_t *regval,
                                  int nregs)
{
  uint8_t buffer[HYM8563_TIME_NREGS + 1];
  int ret;

  DEBUGASSERT(nregs > 0 && nregs <= HYM8563_TIME_NREGS);

  buffer[0] = regaddr;
  memcpy(&buffer[1], regval, nregs);

  ret = i2c_write(priv->i2c, &priv->config, buffer, nregs + 1);
  if (ret < 0)
    {
      rtcerr("ERROR: write of register 0x%02x failed: %d\n", regaddr, ret);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_getreg
 ****************************************************************************/

static int rk3576_hym8563_getreg(struct kickpi_k7_hym8563_s *priv,
                                 uint8_t regaddr, uint8_t *regval)
{
  return rk3576_hym8563_getregs(priv, regaddr, regval, 1);
}

/****************************************************************************
 * Name: rk3576_hym8563_putreg
 ****************************************************************************/

static int rk3576_hym8563_putreg(struct kickpi_k7_hym8563_s *priv,
                                 uint8_t regaddr, uint8_t regval)
{
  return rk3576_hym8563_putregs(priv, regaddr, &regval, 1);
}

/****************************************************************************
 * Name: rk3576_hym8563_bin2bcd
 ****************************************************************************/

static uint8_t rk3576_hym8563_bin2bcd(int bin)
{
  return (uint8_t)(((bin / 10) << 4) | (bin % 10));
}

/****************************************************************************
 * Name: rk3576_hym8563_bcd2bin
 ****************************************************************************/

static int rk3576_hym8563_bcd2bin(uint8_t bcd)
{
  return ((bcd >> 4) * 10) + (bcd & 0x0f);
}

/****************************************************************************
 * Name: rk3576_hym8563_weekday
 *
 * Description:
 *   Sakamoto's day of week algorithm, so that the weekday register can be
 *   programmed even in configurations where struct rtc_time has no tm_wday
 *   member (CONFIG_TIME_EXTENDED / CONFIG_LIBC_LOCALTIME disabled).
 *
 * Input Parameters:
 *   year - Full year (e.g. 2026)
 *   mon  - Month, 1..12
 *   mday - Day of month, 1..31
 *
 * Returned Value:
 *   Day of week, 0 = Sunday .. 6 = Saturday.
 *
 ****************************************************************************/

static int rk3576_hym8563_weekday(int year, int mon, int mday)
{
  static const int offset[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };

  int y = year;

  if (mon < 3)
    {
      y -= 1;
    }

  return (y + y / 4 - y / 100 + y / 400 + offset[mon - 1] + mday) % 7;
}

/****************************************************************************
 * Name: rk3576_hym8563_clkout_check
 *
 * Description:
 *   Make sure the 32.768 kHz CLKOUT that clocks the SV6621 WiFi/BT
 *   companion chip is running.  Never disables it: if it is already
 *   enabled at 32.768 kHz the register is left untouched.
 *
 ****************************************************************************/

static int rk3576_hym8563_clkout_check(struct kickpi_k7_hym8563_s *priv)
{
  uint8_t regval;
  int ret;

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CLKOUT, &regval);
  if (ret < 0)
    {
      return ret;
    }

  if ((regval & HYM8563_CLKOUT_FE) != 0 &&
      (regval & HYM8563_CLKOUT_FD_MASK) == HYM8563_CLKOUT_FD_32768HZ)
    {
      rtcinfo("CLKOUT already enabled at 32.768kHz (0x%02x)\n", regval);
      return OK;
    }

  rtcwarn("CLKOUT is 0x%02x, restoring 32.768kHz for WiFi/BT\n", regval);
  return rk3576_hym8563_putreg(priv, HYM8563_REG_CLKOUT,
                               HYM8563_CLKOUT_32768HZ);
}

/****************************************************************************
 * Name: rk3576_hym8563_rdtime
 *
 * Description:
 *   Read the current time from the RTC.
 *
 ****************************************************************************/

static int rk3576_hym8563_rdtime(struct rtc_lowerhalf_s *lower,
                                 struct rtc_time *rtctime)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  uint8_t buffer[HYM8563_TIME_NREGS];
  int ret;

  DEBUGASSERT(priv != NULL && rtctime != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_hym8563_getregs(priv, HYM8563_REG_SEC, buffer,
                               HYM8563_TIME_NREGS);
  if (ret < 0)
    {
      goto errout;
    }

  /* A set VL flag means the oscillator stopped at some point: the time is
   * not trustworthy.  Report it, but still return what the part holds so
   * that userspace can see (and correct) the bogus value.
   */

  if ((buffer[0] & HYM8563_SEC_VL) != 0)
    {
      rtcwarn("WARNING: VL flag set, RTC lost power, time is not valid\n");
      priv->timeset = false;
    }

  memset(rtctime, 0, sizeof(*rtctime));

  rtctime->tm_sec = rk3576_hym8563_bcd2bin(buffer[0] & HYM8563_SEC_MASK);
  rtctime->tm_min = rk3576_hym8563_bcd2bin(buffer[1] & HYM8563_MIN_MASK);
  rtctime->tm_hour = rk3576_hym8563_bcd2bin(buffer[2] & HYM8563_HOUR_MASK);
  rtctime->tm_mday = rk3576_hym8563_bcd2bin(buffer[3] & HYM8563_DAY_MASK);
  rtctime->tm_mon = rk3576_hym8563_bcd2bin(buffer[5] & HYM8563_MONTH_MASK) - 1;
  rtctime->tm_year =
      rk3576_hym8563_bcd2bin(buffer[6]) +
      (((buffer[5] & HYM8563_MONTH_CENTURY) != 0) ? HYM8563_YEAR_2100
                                                  : HYM8563_YEAR_2000);

#if defined(CONFIG_LIBC_LOCALTIME) || defined(CONFIG_TIME_EXTENDED)
  rtctime->tm_wday = buffer[4] & HYM8563_WEEKDAY_MASK;
  rtctime->tm_yday = 0;
  rtctime->tm_isdst = 0;
#endif

  rtcinfo("Read %04d-%02d-%02d %02d:%02d:%02d\n",
          rtctime->tm_year + HYM8563_TM_YEAR_BASE, rtctime->tm_mon + 1,
          rtctime->tm_mday, rtctime->tm_hour, rtctime->tm_min,
          rtctime->tm_sec);

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_settime
 *
 * Description:
 *   Program the RTC with the given time.  The clock chain is halted (STOP)
 *   across the burst write so that no carry can propagate into a partially
 *   written field, and released afterwards.  Writing the seconds register
 *   also clears the VL flag.
 *
 ****************************************************************************/

static int rk3576_hym8563_settime(struct rtc_lowerhalf_s *lower,
                                  const struct rtc_time *rtctime)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  uint8_t buffer[HYM8563_TIME_NREGS];
  uint8_t ctl1;
  int year;
  int wday;
  int ret;

  DEBUGASSERT(priv != NULL && rtctime != NULL);

  year = rtctime->tm_year + HYM8563_TM_YEAR_BASE;
  if (rtctime->tm_year < HYM8563_YEAR_2000 ||
      rtctime->tm_year >= HYM8563_YEAR_2100 + 100)
    {
      rtcerr("ERROR: year %d out of range (2000..2199)\n", year);
      return -EINVAL;
    }

  wday = rk3576_hym8563_weekday(year, rtctime->tm_mon + 1, rtctime->tm_mday);

  buffer[0] = rk3576_hym8563_bin2bcd(rtctime->tm_sec);
  buffer[1] = rk3576_hym8563_bin2bcd(rtctime->tm_min);
  buffer[2] = rk3576_hym8563_bin2bcd(rtctime->tm_hour);
  buffer[3] = rk3576_hym8563_bin2bcd(rtctime->tm_mday);
  buffer[4] = (uint8_t)wday;
  buffer[5] = rk3576_hym8563_bin2bcd(rtctime->tm_mon + 1);
  if (rtctime->tm_year >= HYM8563_YEAR_2100)
    {
      buffer[5] |= HYM8563_MONTH_CENTURY;
      buffer[6] = rk3576_hym8563_bin2bcd(rtctime->tm_year - HYM8563_YEAR_2100);
    }
  else
    {
      buffer[6] = rk3576_hym8563_bin2bcd(rtctime->tm_year - HYM8563_YEAR_2000);
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Halt the clock chain while the calendar registers are rewritten */

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CTL1, &ctl1);
  if (ret < 0)
    {
      goto errout;
    }

  ret =
      rk3576_hym8563_putreg(priv, HYM8563_REG_CTL1, ctl1 | HYM8563_CTL1_STOP);
  if (ret < 0)
    {
      goto errout;
    }

  ret = rk3576_hym8563_putregs(priv, HYM8563_REG_SEC, buffer,
                               HYM8563_TIME_NREGS);

  /* Always release STOP, even if the burst write failed */

  ctl1 &= ~HYM8563_CTL1_STOP;
  if (rk3576_hym8563_putreg(priv, HYM8563_REG_CTL1, ctl1) < 0 && ret >= 0)
    {
      ret = -EIO;
    }

  if (ret >= 0)
    {
      priv->timeset = true;
      rtcinfo("Set %04d-%02d-%02d %02d:%02d:%02d (wday %d)\n", year,
              rtctime->tm_mon + 1, rtctime->tm_mday, rtctime->tm_hour,
              rtctime->tm_min, rtctime->tm_sec, wday);
    }

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_havesettime
 *
 * Description:
 *   Return true if the RTC holds a time that can be trusted, i.e. it has
 *   not lost power (VL clear) since it was last programmed.
 *
 ****************************************************************************/

static bool rk3576_hym8563_havesettime(struct rtc_lowerhalf_s *lower)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  uint8_t regval;
  int ret;

  DEBUGASSERT(priv != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return false;
    }

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_SEC, &regval);
  if (ret >= 0)
    {
      priv->timeset = ((regval & HYM8563_SEC_VL) == 0);
    }

  ret = priv->timeset ? 1 : 0;
  nxmutex_unlock(&priv->lock);

  return ret != 0;
}

#ifdef CONFIG_RTC_ALARM

/****************************************************************************
 * Name: rk3576_hym8563_worker
 *
 * Description:
 *   Low priority work queue callback that polls the alarm flag.  The alarm
 *   INT line (GPIO0_A0) cannot be attached directly with the current
 *   RK3576 GPIO driver, and the HYM8563 alarm granularity is one minute,
 *   so a one second poll is sufficient.
 *
 ****************************************************************************/

static void rk3576_hym8563_worker(void *arg)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)arg;
  rtc_alarm_callback_t cb = NULL;
  void *cbpriv = NULL;
  uint8_t ctl2;
  int ret;

  DEBUGASSERT(priv != NULL);

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return;
    }

  if (!priv->armed)
    {
      nxmutex_unlock(&priv->lock);
      return;
    }

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CTL2, &ctl2);
  if (ret >= 0 && (ctl2 & HYM8563_CTL2_AF) != 0)
    {
      /* Acknowledge the alarm and disarm it: the NuttX alarm contract is
       * one shot.
       */

      ctl2 &= ~(HYM8563_CTL2_AF | HYM8563_CTL2_AIE);
      ctl2 |= HYM8563_CTL2_TF; /* Write 1 to preserve the timer flag */
      rk3576_hym8563_putreg(priv, HYM8563_REG_CTL2, ctl2);

      cb = priv->cb;
      cbpriv = priv->priv;
      priv->cb = NULL;
      priv->priv = NULL;
      priv->armed = false;
    }
  else
    {
      work_queue(LPWORK, &priv->work, rk3576_hym8563_worker, priv,
                 HYM8563_ALARM_POLL_TICKS);
    }

  nxmutex_unlock(&priv->lock);

  /* Call the client outside of the lock so that it may re-arm the alarm */

  if (cb != NULL)
    {
      rtcinfo("Alarm expired\n");
      cb(cbpriv, HYM8563_ALARM_ID);
    }
}

/****************************************************************************
 * Name: rk3576_hym8563_setalarm
 *
 * Description:
 *   Set an absolute alarm.  The HYM8563 compares minute, hour, day of
 *   month and day of week only, so the alarm always fires at second 0 of
 *   the requested minute and cannot be more than one month ahead.
 *
 ****************************************************************************/

static int rk3576_hym8563_setalarm(struct rtc_lowerhalf_s *lower,
                                   const struct lower_setalarm_s *alarm)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  uint8_t buffer[4];
  uint8_t ctl2;
  int wday;
  int ret;

  DEBUGASSERT(priv != NULL && alarm != NULL);

  if (alarm->id != HYM8563_ALARM_ID || alarm->cb == NULL)
    {
      return -EINVAL;
    }

  wday = rk3576_hym8563_weekday(alarm->time.tm_year + HYM8563_TM_YEAR_BASE,
                                alarm->time.tm_mon + 1, alarm->time.tm_mday);

  buffer[0] = rk3576_hym8563_bin2bcd(alarm->time.tm_min);
  buffer[1] = rk3576_hym8563_bin2bcd(alarm->time.tm_hour);
  buffer[2] = rk3576_hym8563_bin2bcd(alarm->time.tm_mday);
  buffer[3] = (uint8_t)wday;

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (priv->armed)
    {
      nxmutex_unlock(&priv->lock);
      return -EBUSY;
    }

  ret = rk3576_hym8563_putregs(priv, HYM8563_REG_ALM_MIN, buffer,
                               sizeof(buffer));
  if (ret < 0)
    {
      goto errout;
    }

  /* Clear any stale alarm flag and enable the alarm interrupt.  AF and TF
   * are cleared by writing 0, so TF is written back as 1 to keep it.
   */

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CTL2, &ctl2);
  if (ret < 0)
    {
      goto errout;
    }

  ctl2 &= ~HYM8563_CTL2_AF;
  ctl2 |= HYM8563_CTL2_AIE | HYM8563_CTL2_TF;

  ret = rk3576_hym8563_putreg(priv, HYM8563_REG_CTL2, ctl2);
  if (ret < 0)
    {
      goto errout;
    }

  priv->cb = alarm->cb;
  priv->priv = alarm->priv;
  priv->alarm = alarm->time;
  priv->armed = true;

  rtcinfo("Alarm armed for day %d %02d:%02d (wday %d)\n", alarm->time.tm_mday,
          alarm->time.tm_hour, alarm->time.tm_min, wday);

  work_queue(LPWORK, &priv->work, rk3576_hym8563_worker, priv,
             HYM8563_ALARM_POLL_TICKS);

errout:
  nxmutex_unlock(&priv->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_setrelative
 *
 * Description:
 *   Set an alarm relative to the current RTC time.  Converted to an
 *   absolute alarm because the part has no relative alarm mode (its down
 *   counter is a separate, much coarser, facility).
 *
 ****************************************************************************/

static int rk3576_hym8563_setrelative(struct rtc_lowerhalf_s *lower,
                                      const struct lower_setrelative_s *alarm)
{
  struct lower_setalarm_s setalarm;
  struct rtc_time now;
  time_t abstime;
  int ret;

  DEBUGASSERT(lower != NULL && alarm != NULL);

  if (alarm->id != HYM8563_ALARM_ID || alarm->reltime <= 0)
    {
      return -EINVAL;
    }

  ret = rk3576_hym8563_rdtime(lower, &now);
  if (ret < 0)
    {
      return ret;
    }

  abstime = timegm((struct tm *)&now) + alarm->reltime;

  setalarm.id = alarm->id;
  setalarm.cb = alarm->cb;
  setalarm.priv = alarm->priv;

  if (gmtime_r(&abstime, (struct tm *)&setalarm.time) == NULL)
    {
      return -EINVAL;
    }

  return rk3576_hym8563_setalarm(lower, &setalarm);
}

/****************************************************************************
 * Name: rk3576_hym8563_cancelalarm
 ****************************************************************************/

static int rk3576_hym8563_cancelalarm(struct rtc_lowerhalf_s *lower,
                                      int alarmid)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  uint8_t buffer[4];
  uint8_t ctl2;
  int ret;

  DEBUGASSERT(priv != NULL);

  if (alarmid != HYM8563_ALARM_ID)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->armed)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODATA;
    }

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CTL2, &ctl2);
  if (ret >= 0)
    {
      ctl2 &= ~(HYM8563_CTL2_AF | HYM8563_CTL2_AIE);
      ctl2 |= HYM8563_CTL2_TF;
      ret = rk3576_hym8563_putreg(priv, HYM8563_REG_CTL2, ctl2);
    }

  /* Disable every alarm comparison field as well */

  buffer[0] = HYM8563_ALM_DISABLE;
  buffer[1] = HYM8563_ALM_DISABLE;
  buffer[2] = HYM8563_ALM_DISABLE;
  buffer[3] = HYM8563_ALM_DISABLE;
  rk3576_hym8563_putregs(priv, HYM8563_REG_ALM_MIN, buffer, sizeof(buffer));

  priv->armed = false;
  priv->cb = NULL;
  priv->priv = NULL;

  nxmutex_unlock(&priv->lock);
  work_cancel(LPWORK, &priv->work);

  return ret;
}

/****************************************************************************
 * Name: rk3576_hym8563_rdalarm
 *
 * Description:
 *   Return the alarm time as it was programmed.  The hardware only stores
 *   minute/hour/day/weekday, so the year and month of the cached copy are
 *   used to return a complete struct rtc_time.
 *
 ****************************************************************************/

static int rk3576_hym8563_rdalarm(struct rtc_lowerhalf_s *lower,
                                  struct lower_rdalarm_s *alarm)
{
  struct kickpi_k7_hym8563_s *priv = (struct kickpi_k7_hym8563_s *)lower;
  int ret;

  DEBUGASSERT(priv != NULL && alarm != NULL && alarm->time != NULL);

  if (alarm->id != HYM8563_ALARM_ID)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&priv->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!priv->armed)
    {
      nxmutex_unlock(&priv->lock);
      return -ENODATA;
    }

  *alarm->time = priv->alarm;
  nxmutex_unlock(&priv->lock);

  return OK;
}

#endif /* CONFIG_RTC_ALARM */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_hym8563_initialize
 *
 * Description:
 *   Bind the on-board HYM8563 RTC to the given I2C master and register it
 *   as /dev/rtc0.
 *
 ****************************************************************************/

int kickpi_k7_hym8563_initialize(struct i2c_master_s *i2c)
{
  struct kickpi_k7_hym8563_s *priv = &g_hym8563;
  uint8_t regval;
  int ret;

  DEBUGASSERT(i2c != NULL);

  priv->lower.ops = &g_hym8563_ops;
  priv->i2c = i2c;
  priv->config.frequency = HYM8563_I2C_FREQUENCY;
  priv->config.address = KICKPI_K7_HYM8563_I2C_ADDR;
  priv->config.addrlen = HYM8563_I2C_ADDRLEN;
  priv->timeset = false;

  /* Probe: Control_Status1 must be readable and the part must not be stuck
   * in test mode.
   */

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_CTL1, &regval);
  if (ret < 0)
    {
      rtcerr("ERROR: HYM8563 not responding at 0x%02x: %d\n",
             KICKPI_K7_HYM8563_I2C_ADDR, ret);
      return ret;
    }

  if ((regval &
       (HYM8563_CTL1_TEST | HYM8563_CTL1_STOP | HYM8563_CTL1_TESTC)) != 0)
    {
      rtcwarn("Control_Status1 is 0x%02x, releasing test/stop\n", regval);
      ret = rk3576_hym8563_putreg(priv, HYM8563_REG_CTL1, 0);
      if (ret < 0)
        {
          return ret;
        }
    }

  /* Report whether the retained time survived */

  ret = rk3576_hym8563_getreg(priv, HYM8563_REG_SEC, &regval);
  if (ret < 0)
    {
      return ret;
    }

  if ((regval & HYM8563_SEC_VL) != 0)
    {
      rtcwarn("WARNING: VL set: RTC lost power, time must be set\n");
      priv->timeset = false;
    }
  else
    {
      priv->timeset = true;
    }

  /* The 32.768 kHz CLKOUT feeds the SV6621 WiFi/BT companion.  Verify it,
   * never disable it.
   */

  ret = rk3576_hym8563_clkout_check(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = rtc_initialize(0, (struct rtc_lowerhalf_s *)priv);
  if (ret < 0)
    {
      rtcerr("ERROR: rtc_initialize failed: %d\n", ret);
      return ret;
    }

  rtcinfo("HYM8563 registered as /dev/rtc0 (i2c%d, addr 0x%02x)\n",
          KICKPI_K7_HYM8563_I2C_BUS, KICKPI_K7_HYM8563_I2C_ADDR);
  return OK;
}

#endif /* CONFIG_KICKPI_K7_HYM8563 */
