/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pinctrl.c
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
 * RK3576 pinctrl framework.
 *
 * Applies "pin groups" - the device-tree style <bank pin func config>
 * description of every pad a peripheral needs - to the IOC pad control
 * registers.  See rk3576_pinctrl.h for the encoding and the intended usage,
 * and hardware/rk3576_pinctrl.h for the IOC sub-domain layout.
 *
 * The IOMUX register geometry and the hiword-mask write helpers are shared
 * with the single-pin path in rk3576_gpio.c (g_iomux_groups[],
 * RK3576_WRITE_MASK(), RK3576_DRIVE_IS_4LEVEL()); only the pull / drive /
 * schmitt region table is owned here, expressed with the same bank-origin
 * offset convention so the two paths can be diffed against each other.
 *
 * No clock is requested from the CLK framework.  The IOC pad-control
 * registers are reached over the APB gates of their own domains
 * (pclk_busioc, pclk_vccio_ioc, pclk_vccio6_ioc, pclk_vccio7_ioc,
 * pclk_pmu1_ioc, pclk_pmu0ioc), all of which are open out of reset - the
 * RK3576 gate bits disable a clock when set and every one of them resets to
 * zero - and none of which may be gated off again, because pad
 * configuration has to stay reachable for the whole lifetime of the system.
 * The vendor device tree likewise gives its pinctrl node no "clocks"
 * property.
 *
 * TODO: rk3576_gpio.c still carries its own private copies of the pull /
 * drive / schmitt routing chains.  Once this module has been validated on
 * silicon for all five banks, rk3576_config_gpio() should call into
 * rk3576_pinctrl_set_*() instead so there is a single routing table.
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

#include <nuttx/irq.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "hardware/rk3576_gpio.h"
#include "hardware/rk3576_pinctrl.h"
#include "rk3576_pinctrl.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Pad-control routing table.
 *
 * Regions are cut where the IOC sub-domain changes:
 *   GPIO0  splits at pin 12 (PMU1_IOC -> PMU2_IOC)
 *   GPIO1..GPIO3 are one TOP_IOC region each
 *   GPIO4  splits at pin 16 (TOP_IOC -> VCCIO6_IOC) and at pin 24
 *          (VCCIO6_IOC -> VCCIO7_IOC)
 *
 * Every offset is a bank origin: the register of a pin is
 * base + (pin / pins_per_reg) * 4 using the absolute pin number, so the
 * values match the constants in rk3576_gpio.c one for one.
 */

const struct rk3576_pinctrl_route_s
    g_rk3576_pinctrl_routes[RK3576_PINCTRL_NROUTES] = {
      /* bank  first  last   pull        drive       schmitt */

      /* GPIO0_A + GPIO0_B[0:3] - PMU1_IOC */

      { 0, 0, 11, 0x00000020, 0x00000010, 0x00000030 },

      /* GPIO0_B[4:7] + GPIO0_C + GPIO0_D - PMU2_IOC */

      { 0, 12, 31, 0x00002024, 0x00002008, 0x0000203c },

      /* GPIO1 - TOP_IOC */

      { 1, 0, 31, 0x00006110, 0x00006020, 0x00006210 },

      /* GPIO2 - TOP_IOC */

      { 2, 0, 31, 0x00006120, 0x00006040, 0x00006220 },

      /* GPIO3 - TOP_IOC */

      { 3, 0, 31, 0x00006130, 0x00006060, 0x00006230 },

      /* GPIO4_A + GPIO4_B - TOP_IOC */

      { 4, 0, 15, 0x00006140, 0x00006080, 0x00006240 },

      /* GPIO4_C - VCCIO6_IOC */

      { 4, 16, 23, 0x0000a140, 0x0000a080, 0x0000a240 },

      /* GPIO4_D - VCCIO7_IOC */

      { 4, 24, 31, 0x0000b140, 0x0000b080, 0x0000b240 },
    };

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Serialises IOC register writes.  The IOC registers are hiword-masked, so
 * a single putreg32() is already atomic at the hardware level; the lock
 * exists so that a whole pin group is applied without another CPU muxing a
 * pad of the same peripheral in between.
 */

static spinlock_t g_rk3576_pinctrl_lock = SP_UNLOCKED;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const struct rk3576_pinctrl_route_s *
rk3576_pinctrl_route(unsigned int bank, unsigned int pin);
static int rk3576_pinctrl_decode(uint32_t pinmux, unsigned int *bank,
                                 unsigned int *pin);
static uint32_t rk3576_pinctrl_mux_reg(unsigned int bank, unsigned int pin,
                                       unsigned int *shift);
static int rk3576_pinctrl_drv_to_hw(unsigned int bank, unsigned int pin,
                                    unsigned int level, uint32_t *hw_val);
static void rk3576_pinctrl_set_mux_raw(unsigned int bank, unsigned int pin,
                                       unsigned int func);
static void
rk3576_pinctrl_set_pull_raw(const struct rk3576_pinctrl_route_s *route,
                            unsigned int pin, uint32_t hw_pull);
static void
rk3576_pinctrl_set_drive_raw(const struct rk3576_pinctrl_route_s *route,
                             unsigned int pin, uint32_t hw_val);
static void
rk3576_pinctrl_set_schmitt_raw(const struct rk3576_pinctrl_route_s *route,
                               unsigned int pin, bool enable);
static int rk3576_pinctrl_config_locked(const struct rk3576_pin_cfg_s *cfg);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pinctrl_route
 *
 * Description:
 *   Look up the pad-control region that owns a pin.
 *
 * Returned Value:
 *   The region descriptor, or NULL if bank/pin is out of range.
 *
 ****************************************************************************/

static const struct rk3576_pinctrl_route_s *
rk3576_pinctrl_route(unsigned int bank, unsigned int pin)
{
  unsigned int i;

  for (i = 0; i < RK3576_PINCTRL_NROUTES; i++)
    {
      const struct rk3576_pinctrl_route_s *route = &g_rk3576_pinctrl_routes[i];

      if (route->bank == bank && pin >= route->first_pin &&
          pin <= route->last_pin)
        {
          return route;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: rk3576_pinctrl_decode
 *
 * Description:
 *   Split a pin identifier into bank and pin and range-check both.
 *
 * Returned Value:
 *   OK on success, -EINVAL if the identifier is malformed.
 *
 ****************************************************************************/

static int rk3576_pinctrl_decode(uint32_t pinmux, unsigned int *bank,
                                 unsigned int *pin)
{
  *bank = RK3576_PIN_BANK(pinmux);
  *pin = RK3576_PIN_NUM(pinmux);

  if (*bank >= RK3576_GPIO_NPORTS)
    {
      gpioerr("ERROR: invalid GPIO bank %u\n", *bank);
      return -EINVAL;
    }

  /* The pin field is 5 bits wide, so it can never exceed 31; the check is
   * kept for the day the encoding grows.
   */

  if (*pin >= RK3576_GPIO_NPINS)
    {
      gpioerr("ERROR: invalid GPIO pin %u\n", *pin);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_mux_reg
 *
 * Description:
 *   Compute the absolute address of the IOMUX register holding a pin and
 *   the bit position of its 4-bit function field.
 *
 *   Layout: each 8-pin group owns two consecutive registers, pins 0-3 of
 *   the group in the first and pins 4-7 in the second.  GPIO0_B[4:7] is
 *   the one irregularity: those four pins are in PMU2_IOC and their
 *   register sits RK3576_MUX_GPIO0BH_FIXUP above the PMU1_IOC group base.
 *
 * Input Parameters:
 *   bank  - GPIO bank, already range-checked.
 *   pin   - Pin inside the bank, already range-checked.
 *   shift - Receives the bit offset of the function field.
 *
 * Returned Value:
 *   Absolute register address.
 *
 ****************************************************************************/

static uint32_t rk3576_pinctrl_mux_reg(unsigned int bank, unsigned int pin,
                                       unsigned int *shift)
{
  unsigned int group = pin / 8;
  unsigned int group_pin = pin % 8;
  uint32_t reg;

  reg = RK3576_IOC_ADDR + g_iomux_groups[bank][group].offset;

  if (bank == 0 && pin >= RK3576_GPIO0_PMU2_FIRST_PIN &&
      pin < RK3576_GPIO0_PMU2_FIRST_PIN + 4)
    {
      reg += RK3576_MUX_GPIO0BH_FIXUP;
    }

  if (group_pin >= RK3576_MUX_PINS_PER_REG)
    {
      reg += 4;
      group_pin -= RK3576_MUX_PINS_PER_REG;
    }

  *shift = group_pin * RK3576_MUX_BITS_PER_PIN;

  return reg;
}

/****************************************************************************
 * Name: rk3576_pinctrl_drv_to_hw
 *
 * Description:
 *   Translate a logical drive level (0..5) into the register encoding for
 *   the pad class this pin belongs to.
 *
 *   4-level pads (GPIO0_A, GPIO0_B[0:3], GPIO4_D[0:1]) implement only
 *   100/50/33/25 ohms, so levels 1 (66 ohms) and 3 (40 ohms) are rejected
 *   rather than silently rounded.
 *
 * Returned Value:
 *   OK on success, -EINVAL if the level is not available on this pad.
 *
 ****************************************************************************/

static int rk3576_pinctrl_drv_to_hw(unsigned int bank, unsigned int pin,
                                    unsigned int level, uint32_t *hw_val)
{
  bool is_4level = RK3576_DRIVE_IS_4LEVEL(bank, pin);

  switch (level)
    {
      case RK3576_DRIVE_LEVEL_0: /* 100 ohms */
        *hw_val = 0x0;
        return OK;

      case RK3576_DRIVE_LEVEL_1: /* 66 ohms, 6-level pads only */
        if (is_4level)
          {
            break;
          }

        *hw_val = 0x4;
        return OK;

      case RK3576_DRIVE_LEVEL_2: /* 50 ohms */
        *hw_val = 0x2;
        return OK;

      case RK3576_DRIVE_LEVEL_3: /* 40 ohms, 6-level pads only */
        if (is_4level)
          {
            break;
          }

        *hw_val = 0x6;
        return OK;

      case RK3576_DRIVE_LEVEL_4: /* 33 ohms, same encoding in both classes */
        *hw_val = 0x1;
        return OK;

      case RK3576_DRIVE_LEVEL_5: /* 25 ohms */
        *hw_val = is_4level ? 0x3 : 0x5;
        return OK;

      default:
        gpioerr("ERROR: invalid drive level %u\n", level);
        return -EINVAL;
    }

  gpioerr("ERROR: drive level %u unsupported on 4-level pad GPIO%u_%u\n",
          level, bank, pin);
  return -EINVAL;
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_mux_raw
 *
 * Description:
 *   Write the IOMUX function field.  Caller holds the lock and has already
 *   validated bank, pin and func.
 *
 ****************************************************************************/

static void rk3576_pinctrl_set_mux_raw(unsigned int bank, unsigned int pin,
                                       unsigned int func)
{
  unsigned int shift;
  uint32_t reg;

  reg = rk3576_pinctrl_mux_reg(bank, pin, &shift);

  putreg32(RK3576_WRITE_MASK(shift + RK3576_MUX_BITS_PER_PIN - 1, shift, func),
           reg);
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_pull_raw
 *
 * Description:
 *   Write the bias field.  2 bits per pin, 8 pins per register.  hw_pull is
 *   the register encoding (RK3576_PULL_DISABLE / _DOWN / _UP).
 *
 ****************************************************************************/

static void
rk3576_pinctrl_set_pull_raw(const struct rk3576_pinctrl_route_s *route,
                            unsigned int pin, uint32_t hw_pull)
{
  uint32_t reg;
  unsigned int shift;

  reg = RK3576_IOC_ADDR + route->pull_base +
        (pin / RK3576_PULL_PINS_PER_REG) * 4;
  shift = (pin % RK3576_PULL_PINS_PER_REG) * RK3576_PULL_BITS_PER_PIN;

  putreg32(
      RK3576_WRITE_MASK(shift + RK3576_PULL_BITS_PER_PIN - 1, shift, hw_pull),
      reg);
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_drive_raw
 *
 * Description:
 *   Write the drive-strength field.  4 bits per pin, 4 pins per register.
 *   hw_val is the register encoding produced by rk3576_pinctrl_drv_to_hw().
 *
 ****************************************************************************/

static void
rk3576_pinctrl_set_drive_raw(const struct rk3576_pinctrl_route_s *route,
                             unsigned int pin, uint32_t hw_val)
{
  uint32_t reg;
  unsigned int shift;

  reg =
      RK3576_IOC_ADDR + route->drv_base + (pin / RK3576_DRV_PINS_PER_REG) * 4;
  shift = (pin % RK3576_DRV_PINS_PER_REG) * RK3576_DRV_BITS_PER_PIN;

  putreg32(
      RK3576_WRITE_MASK(shift + RK3576_DRV_BITS_PER_PIN - 1, shift, hw_val),
      reg);
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_schmitt_raw
 *
 * Description:
 *   Write the schmitt-trigger bit.  1 bit per pin, 8 pins per register.
 *
 ****************************************************************************/

static void
rk3576_pinctrl_set_schmitt_raw(const struct rk3576_pinctrl_route_s *route,
                               unsigned int pin, bool enable)
{
  uint32_t reg;
  unsigned int shift;

  reg =
      RK3576_IOC_ADDR + route->smt_base + (pin / RK3576_SMT_PINS_PER_REG) * 4;
  shift = (pin % RK3576_SMT_PINS_PER_REG) * RK3576_SMT_BITS_PER_PIN;

  putreg32(RK3576_WRITE_BIT(shift, enable ? 1u : 0u), reg);
}

/****************************************************************************
 * Name: rk3576_pinctrl_config_locked
 *
 * Description:
 *   Body of rk3576_pinctrl_config(), executed with the pinctrl lock held so
 *   that a whole group can be applied as one critical section.
 *
 ****************************************************************************/

static int rk3576_pinctrl_config_locked(const struct rk3576_pin_cfg_s *cfg)
{
  const struct rk3576_pinctrl_route_s *route;
  unsigned int bank;
  unsigned int pin;
  unsigned int func;
  uint32_t field;
  int ret;

  ret = rk3576_pinctrl_decode(cfg->pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  route = rk3576_pinctrl_route(bank, pin);
  if (route == NULL)
    {
      gpioerr("ERROR: no pad control region for GPIO%u_%u\n", bank, pin);
      return -EINVAL;
    }

  /* Slew-rate control is not implemented yet - fail loudly instead of
   * quietly ignoring a request the caller believes was honoured.
   */

  if ((cfg->flags & RK3576_PIN_SLEW_MASK) != RK3576_PIN_SLEW_KEEP)
    {
      gpioerr("ERROR: slew rate control not supported (GPIO%u_%u)\n", bank,
              pin);
      return -ENOTSUP;
    }

  /* Bias first: settle the pad before the function drives it. */

  field = cfg->flags & RK3576_PIN_PULL_MASK;
  if (field != RK3576_PIN_PULL_KEEP)
    {
      uint32_t hw_pull;

      switch (field)
        {
          case RK3576_PIN_PULL_NONE:
            hw_pull = RK3576_PULL_DISABLE;
            break;

          case RK3576_PIN_PULL_DOWN:
            hw_pull = RK3576_PULL_DOWN;
            break;

          case RK3576_PIN_PULL_UP:
            hw_pull = RK3576_PULL_UP;
            break;

          default:
            gpioerr("ERROR: invalid bias 0x%lx on GPIO%u_%u\n",
                    (unsigned long)field, bank, pin);
            return -EINVAL;
        }

      rk3576_pinctrl_set_pull_raw(route, pin, hw_pull);
    }

  /* Drive strength. */

  field = cfg->flags & RK3576_PIN_DRV_MASK;
  if (field != RK3576_PIN_DRV_KEEP)
    {
      unsigned int level = (field >> RK3576_PIN_DRV_SHIFT) - 1u;
      uint32_t hw_val;

      ret = rk3576_pinctrl_drv_to_hw(bank, pin, level, &hw_val);
      if (ret < 0)
        {
          return ret;
        }

      rk3576_pinctrl_set_drive_raw(route, pin, hw_val);
    }

  /* Schmitt trigger. */

  field = cfg->flags & RK3576_PIN_SMT_MASK;
  if (field != RK3576_PIN_SMT_KEEP)
    {
      rk3576_pinctrl_set_schmitt_raw(route, pin,
                                     field == RK3576_PIN_SMT_ENABLE);
    }

  /* Function mux last: the pad is fully conditioned when the peripheral
   * takes it over.
   */

  func = RK3576_PIN_FUNC(cfg->pinmux);
  rk3576_pinctrl_set_mux_raw(bank, pin, func);

  gpioinfo("GPIO%u_%u -> func %u, flags 0x%lx\n", bank, pin, func,
           (unsigned long)cfg->flags);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pinctrl_set_mux
 *
 * Description:
 *   Select the IOMUX function of a single pad.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_mux(uint32_t pinmux)
{
  unsigned int bank;
  unsigned int pin;
  unsigned int func;
  irqstate_t flags;
  int ret;

  ret = rk3576_pinctrl_decode(pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  /* The function field is 4 bits wide, so it is always in range; the
   * comparison documents the hardware limit for future encodings.
   */

  func = RK3576_PIN_FUNC(pinmux);
  if (func > RK3576_MUX_FUNC_MAX)
    {
      gpioerr("ERROR: invalid mux function %u\n", func);
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_rk3576_pinctrl_lock);
  rk3576_pinctrl_set_mux_raw(bank, pin, func);
  spin_unlock_irqrestore(&g_rk3576_pinctrl_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_get_mux
 *
 * Description:
 *   Read back the IOMUX function currently selected for a pad.
 *
 ****************************************************************************/

int rk3576_pinctrl_get_mux(uint32_t pinmux, unsigned int *func)
{
  unsigned int bank;
  unsigned int pin;
  unsigned int shift;
  uint32_t reg;
  int ret;

  if (func == NULL)
    {
      return -EINVAL;
    }

  ret = rk3576_pinctrl_decode(pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  reg = rk3576_pinctrl_mux_reg(bank, pin, &shift);

  /* Hiword-mask registers read back the plain value in bits [15:0]. */

  *func = (getreg32(reg) >> shift) & RK3576_MUX_FUNC_MAX;

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_pull
 *
 * Description:
 *   Set the bias of a single pad.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_pull(uint32_t pinmux, uint32_t pull)
{
  const struct rk3576_pinctrl_route_s *route;
  unsigned int bank;
  unsigned int pin;
  uint32_t hw_pull;
  irqstate_t flags;
  int ret;

  ret = rk3576_pinctrl_decode(pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  route = rk3576_pinctrl_route(bank, pin);
  if (route == NULL)
    {
      return -EINVAL;
    }

  switch (pull & RK3576_PIN_PULL_MASK)
    {
      case RK3576_PIN_PULL_KEEP:
        return OK;

      case RK3576_PIN_PULL_NONE:
        hw_pull = RK3576_PULL_DISABLE;
        break;

      case RK3576_PIN_PULL_DOWN:
        hw_pull = RK3576_PULL_DOWN;
        break;

      case RK3576_PIN_PULL_UP:
        hw_pull = RK3576_PULL_UP;
        break;

      default:
        gpioerr("ERROR: invalid bias 0x%lx on GPIO%u_%u\n",
                (unsigned long)pull, bank, pin);
        return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_rk3576_pinctrl_lock);
  rk3576_pinctrl_set_pull_raw(route, pin, hw_pull);
  spin_unlock_irqrestore(&g_rk3576_pinctrl_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_drive
 *
 * Description:
 *   Set the drive strength of a single pad.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_drive(uint32_t pinmux, unsigned int level)
{
  const struct rk3576_pinctrl_route_s *route;
  unsigned int bank;
  unsigned int pin;
  uint32_t hw_val;
  irqstate_t flags;
  int ret;

  ret = rk3576_pinctrl_decode(pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  route = rk3576_pinctrl_route(bank, pin);
  if (route == NULL)
    {
      return -EINVAL;
    }

  ret = rk3576_pinctrl_drv_to_hw(bank, pin, level, &hw_val);
  if (ret < 0)
    {
      return ret;
    }

  flags = spin_lock_irqsave(&g_rk3576_pinctrl_lock);
  rk3576_pinctrl_set_drive_raw(route, pin, hw_val);
  spin_unlock_irqrestore(&g_rk3576_pinctrl_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_set_schmitt
 *
 * Description:
 *   Enable or disable the schmitt-trigger input buffer of a single pad.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_schmitt(uint32_t pinmux, bool enable)
{
  const struct rk3576_pinctrl_route_s *route;
  unsigned int bank;
  unsigned int pin;
  irqstate_t flags;
  int ret;

  ret = rk3576_pinctrl_decode(pinmux, &bank, &pin);
  if (ret < 0)
    {
      return ret;
    }

  route = rk3576_pinctrl_route(bank, pin);
  if (route == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_rk3576_pinctrl_lock);
  rk3576_pinctrl_set_schmitt_raw(route, pin, enable);
  spin_unlock_irqrestore(&g_rk3576_pinctrl_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: rk3576_pinctrl_config
 *
 * Description:
 *   Apply one pin group entry.
 *
 ****************************************************************************/

int rk3576_pinctrl_config(const struct rk3576_pin_cfg_s *cfg)
{
  return rk3576_pinctrl_config_group(cfg, 1);
}

/****************************************************************************
 * Name: rk3576_pinctrl_config_group
 *
 * Description:
 *   Apply a whole pin group in one critical section.
 *
 ****************************************************************************/

int rk3576_pinctrl_config_group(const struct rk3576_pin_cfg_s *cfgs, size_t n)
{
  irqstate_t flags;
  size_t i;
  int ret = OK;

  if (cfgs == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_rk3576_pinctrl_lock);

  for (i = 0; i < n; i++)
    {
      ret = rk3576_pinctrl_config_locked(&cfgs[i]);
      if (ret < 0)
        {
          gpioerr("ERROR: pin group entry %zu failed: %d\n", i, ret);
          break;
        }
    }

  spin_unlock_irqrestore(&g_rk3576_pinctrl_lock, flags);

  return ret;
}
