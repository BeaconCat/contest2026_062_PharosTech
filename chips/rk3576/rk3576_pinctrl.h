/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pinctrl.h
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
 * RK3576 pinctrl: batch pad configuration for peripheral drivers.
 *
 * rk3576_gpio.c configures one pin at a time out of a gpio_pinset_t that is
 * modelled on the NuttX GPIO API (input/output/interrupt oriented).  This
 * module covers the other half: applying a whole "pin group" - the set of
 * pads that belong to one peripheral instance - in one call, the way the
 * Linux device tree describes it with
 *
 *   rockchip,pins = <bank pin func &pcfg>;
 *
 * A pin group here is simply an array of struct rk3576_pin_cfg_s that a
 * board file or a chip driver keeps in .rodata, e.g.
 *
 *   static const struct rk3576_pin_cfg_s g_uart0_pins[] =
 *   {
 *     { RK3576_PINMUX(0, RK3576_PIN_B(2), 10),
 *       RK3576_PIN_PULL_UP | RK3576_PIN_DRV_LEVEL(2) },
 *     { RK3576_PINMUX(0, RK3576_PIN_B(3), 10),
 *       RK3576_PIN_PULL_NONE | RK3576_PIN_SMT_ENABLE },
 *   };
 *
 *   rk3576_pinctrl_config_group(g_uart0_pins, nitems(g_uart0_pins));
 *
 * Every attribute has a KEEP encoding (value 0) meaning "do not touch the
 * hardware for this attribute", so a group only writes what it declares.
 * A zero flags word therefore performs a pure function-mux change.
 *
 * This module owns the pad registers only.  It never touches the GPIO
 * controller (direction / data / interrupt), so it is safe to call before
 * or after rk3576_config_gpio() on unrelated pins.
 *
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_PINCTRL_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_PINCTRL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Pin identifier encoding *************************************************/

/* A pin identifier packs bank, pin and function into a single uint32_t and
 * maps 1:1 onto the first three cells of a "rockchip,pins" device tree
 * entry:
 *
 *   bits [2:0]   bank    0..4   (GPIO0..GPIO4)
 *   bits [7:3]   pin     0..31  (A0..D7 inside the bank)
 *   bits [11:8]  func    0..15  (0 = GPIO, 1..15 = alternate functions)
 *   bits [31:12] reserved, must be zero
 */

#define RK3576_PIN_BANK_SHIFT 0
#define RK3576_PIN_BANK_MASK  (0x7u << RK3576_PIN_BANK_SHIFT)
#define RK3576_PIN_NUM_SHIFT  3
#define RK3576_PIN_NUM_MASK   (0x1fu << RK3576_PIN_NUM_SHIFT)
#define RK3576_PIN_FUNC_SHIFT 8
#define RK3576_PIN_FUNC_MASK  (0xfu << RK3576_PIN_FUNC_SHIFT)

#define RK3576_PINMUX(bank, pin, func)                            \
  ((((uint32_t)(bank) << RK3576_PIN_BANK_SHIFT) &                 \
    RK3576_PIN_BANK_MASK) |                                       \
   (((uint32_t)(pin) << RK3576_PIN_NUM_SHIFT) &                   \
    RK3576_PIN_NUM_MASK) |                                        \
   (((uint32_t)(func) << RK3576_PIN_FUNC_SHIFT) &                 \
    RK3576_PIN_FUNC_MASK))

/* Same, but with the pin named by its group letter and index, which is how
 * schematics and the TRM spell it (GPIO4_C3 -> RK3576_PINMUX_G(4, 2, 3, f)).
 */

#define RK3576_PIN_GROUP_A 0
#define RK3576_PIN_GROUP_B 1
#define RK3576_PIN_GROUP_C 2
#define RK3576_PIN_GROUP_D 3

#define RK3576_PIN_A(idx) (RK3576_PIN_GROUP_A * 8 + (idx))
#define RK3576_PIN_B(idx) (RK3576_PIN_GROUP_B * 8 + (idx))
#define RK3576_PIN_C(idx) (RK3576_PIN_GROUP_C * 8 + (idx))
#define RK3576_PIN_D(idx) (RK3576_PIN_GROUP_D * 8 + (idx))

#define RK3576_PINMUX_G(bank, group, idx, func) \
  RK3576_PINMUX((bank), (group) * 8 + (idx), (func))

/* Accessors */

#define RK3576_PIN_BANK(id) \
  (((id) & RK3576_PIN_BANK_MASK) >> RK3576_PIN_BANK_SHIFT)
#define RK3576_PIN_NUM(id) \
  (((id) & RK3576_PIN_NUM_MASK) >> RK3576_PIN_NUM_SHIFT)
#define RK3576_PIN_FUNC(id) \
  (((id) & RK3576_PIN_FUNC_MASK) >> RK3576_PIN_FUNC_SHIFT)

/* Pad configuration flags *************************************************/

/* Bias.  KEEP leaves the pull registers untouched. */

#define RK3576_PIN_PULL_SHIFT 0
#define RK3576_PIN_PULL_MASK  (0x7u << RK3576_PIN_PULL_SHIFT)
#define RK3576_PIN_PULL_KEEP  (0x0u << RK3576_PIN_PULL_SHIFT)
#define RK3576_PIN_PULL_NONE  (0x1u << RK3576_PIN_PULL_SHIFT)
#define RK3576_PIN_PULL_DOWN  (0x2u << RK3576_PIN_PULL_SHIFT)
#define RK3576_PIN_PULL_UP    (0x3u << RK3576_PIN_PULL_SHIFT)

/* Drive strength.  Encoded as level+1 so that 0 means KEEP.
 *
 * Levels are the logical RK3576_DRIVE_LEVEL_* scale shared with
 * rk3576_gpio.c: 0=100ohm, 1=66ohm, 2=50ohm, 3=40ohm, 4=33ohm, 5=25ohm.
 * Levels 1 and 3 exist on 6-level pads only; requesting them on a 4-level
 * pad (GPIO0_A, GPIO0_B[0:3], GPIO4_D[0:1]) returns -EINVAL.
 */

#define RK3576_PIN_DRV_SHIFT     3
#define RK3576_PIN_DRV_MASK      (0xfu << RK3576_PIN_DRV_SHIFT)
#define RK3576_PIN_DRV_KEEP      (0x0u << RK3576_PIN_DRV_SHIFT)
#define RK3576_PIN_DRV_LEVEL(n)  ((((uint32_t)(n) + 1u) << RK3576_PIN_DRV_SHIFT) \
                                  & RK3576_PIN_DRV_MASK)

#define RK3576_PIN_DRV_100OHM    RK3576_PIN_DRV_LEVEL(0)
#define RK3576_PIN_DRV_66OHM     RK3576_PIN_DRV_LEVEL(1)
#define RK3576_PIN_DRV_50OHM     RK3576_PIN_DRV_LEVEL(2)
#define RK3576_PIN_DRV_40OHM     RK3576_PIN_DRV_LEVEL(3)
#define RK3576_PIN_DRV_33OHM     RK3576_PIN_DRV_LEVEL(4)
#define RK3576_PIN_DRV_25OHM     RK3576_PIN_DRV_LEVEL(5)

/* Schmitt trigger input. */

#define RK3576_PIN_SMT_SHIFT     7
#define RK3576_PIN_SMT_MASK      (0x3u << RK3576_PIN_SMT_SHIFT)
#define RK3576_PIN_SMT_KEEP      (0x0u << RK3576_PIN_SMT_SHIFT)
#define RK3576_PIN_SMT_DISABLE   (0x1u << RK3576_PIN_SMT_SHIFT)
#define RK3576_PIN_SMT_ENABLE    (0x2u << RK3576_PIN_SMT_SHIFT)

/* Slew rate.
 *
 * TODO: the RK3576 TRM revision available to this project does not document
 * the per-pad slew-rate (SR) register offsets, and the vendor device tree
 * never sets slew-rate on this board.  The encoding is reserved here so the
 * flags word stays stable; rk3576_pinctrl_config() rejects any value other
 * than KEEP with -ENOTSUP until the offsets are confirmed against silicon.
 */

#define RK3576_PIN_SLEW_SHIFT    9
#define RK3576_PIN_SLEW_MASK     (0x3u << RK3576_PIN_SLEW_SHIFT)
#define RK3576_PIN_SLEW_KEEP     (0x0u << RK3576_PIN_SLEW_SHIFT)
#define RK3576_PIN_SLEW_SLOW     (0x1u << RK3576_PIN_SLEW_SHIFT)
#define RK3576_PIN_SLEW_FAST     (0x2u << RK3576_PIN_SLEW_SHIFT)

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One entry of a pin group: which pad, which function, how it is biased. */

struct rk3576_pin_cfg_s
{
  uint32_t pinmux; /* RK3576_PINMUX(bank, pin, func)                       */
  uint32_t flags;  /* RK3576_PIN_PULL_* | _DRV_* | _SMT_* | _SLEW_*        */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_pinctrl_set_mux
 *
 * Description:
 *   Select the IOMUX function of a single pad.  No other pad attribute is
 *   modified.
 *
 * Input Parameters:
 *   pinmux - Pin identifier built with RK3576_PINMUX().
 *
 * Returned Value:
 *   OK on success, -EINVAL if the bank, pin or function is out of range.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_mux(uint32_t pinmux);

/****************************************************************************
 * Name: rk3576_pinctrl_get_mux
 *
 * Description:
 *   Read back the IOMUX function currently selected for a pad.  Useful to
 *   inspect what the bootloader left behind.
 *
 * Input Parameters:
 *   pinmux - Pin identifier; its function field is ignored.
 *   func   - Receives the function number, 0..15.
 *
 * Returned Value:
 *   OK on success, -EINVAL on a bad pin identifier or NULL func.
 *
 ****************************************************************************/

int rk3576_pinctrl_get_mux(uint32_t pinmux, unsigned int *func);

/****************************************************************************
 * Name: rk3576_pinctrl_set_pull
 *
 * Description:
 *   Set the bias of a single pad.
 *
 * Input Parameters:
 *   pinmux - Pin identifier; its function field is ignored.
 *   pull   - RK3576_PIN_PULL_NONE / _DOWN / _UP.  _KEEP is a no-op.
 *
 * Returned Value:
 *   OK on success, -EINVAL on a bad pin identifier or bias value.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_pull(uint32_t pinmux, uint32_t pull);

/****************************************************************************
 * Name: rk3576_pinctrl_set_drive
 *
 * Description:
 *   Set the drive strength of a single pad.
 *
 * Input Parameters:
 *   pinmux - Pin identifier; its function field is ignored.
 *   level  - Logical drive level 0..5 (RK3576_DRIVE_LEVEL_*).
 *
 * Returned Value:
 *   OK on success, -EINVAL on a bad pin identifier, or when the level is
 *   not available on this pad class.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_drive(uint32_t pinmux, unsigned int level);

/****************************************************************************
 * Name: rk3576_pinctrl_set_schmitt
 *
 * Description:
 *   Enable or disable the schmitt-trigger input buffer of a single pad.
 *
 * Input Parameters:
 *   pinmux - Pin identifier; its function field is ignored.
 *   enable - true to enable the schmitt trigger.
 *
 * Returned Value:
 *   OK on success, -EINVAL on a bad pin identifier.
 *
 ****************************************************************************/

int rk3576_pinctrl_set_schmitt(uint32_t pinmux, bool enable);

/****************************************************************************
 * Name: rk3576_pinctrl_config
 *
 * Description:
 *   Apply one pin group entry: function mux first, then the pad attributes
 *   that the flags word explicitly asks for.
 *
 * Input Parameters:
 *   cfg - The entry to apply.
 *
 * Returned Value:
 *   OK on success, -EINVAL on a malformed entry, -ENOTSUP if the entry asks
 *   for slew-rate control.
 *
 ****************************************************************************/

int rk3576_pinctrl_config(const struct rk3576_pin_cfg_s *cfg);

/****************************************************************************
 * Name: rk3576_pinctrl_config_group
 *
 * Description:
 *   Apply a whole pin group.  The group is applied atomically with respect
 *   to other pinctrl and GPIO register writes.  On the first failing entry
 *   the function stops and returns; entries already applied are left in
 *   place, because a half-configured peripheral is a driver bug that should
 *   be caught at bring-up rather than silently rolled back.
 *
 * Input Parameters:
 *   cfgs - Array of group entries.
 *   n    - Number of entries.
 *
 * Returned Value:
 *   OK on success, -EINVAL if cfgs is NULL, otherwise the error reported by
 *   rk3576_pinctrl_config() for the offending entry.
 *
 ****************************************************************************/

int rk3576_pinctrl_config_group(const struct rk3576_pin_cfg_s *cfgs,
                                size_t n);

#ifdef __cplusplus
}
#endif

#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_PINCTRL_H */
