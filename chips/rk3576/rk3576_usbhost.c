/****************************************************************************
 * chips/rk3576/rk3576_usbhost.c
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

#ifdef CONFIG_RK3576_USBHOST

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/usb/xhci.h>

#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_usb.h"
#include "rk3576_usb.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_DWC3_ID_MASK       0xffff0000u
#define RK3576_DWC3_ID_VALUE      0x55330000u
#define RK3576_DWC3_RESET_DELAYMS 1

#define RK3576_HIWORD(mask, value) \
  (((uint32_t)(mask) << 16) | ((uint32_t)(value) & (mask)))

#define RK3576_CRU_GATE_CON(n)    (0x0800 + ((n) * 4))
#define RK3576_CRU_SOFTRST_CON(n) (0x0a00 + ((n) * 4))
#define RK3576_PHP_CLKSEL_CON(n)  (0x0300 + ((n) * 4))
#define RK3576_PHP_GATE_CON(n)    (0x0800 + ((n) * 4))

#define RK3576_USB1_ACLK_GATE       (1u << 3)
#define RK3576_USB1_REF_GATE        (1u << 4)
#define RK3576_USB1_SUSPEND_GATE    (1u << 5)
#define RK3576_USB1_MMU_ACLK_GATE   (1u << 14)
#define RK3576_USB1_MMU_SLV_GATE    (1u << 0)
#define RK3576_USB1_PIPE_GATE       (1u << 7)
#define RK3576_COMBPHY1_APB_GATE    (1u << 7)
#define RK3576_PHPPHY_ROOT_GATE     (1u << 2)
#define RK3576_PCIE_100M_GATE       (1u << 1)
#define RK3576_COMBPHY1_REF_GATE    (1u << 8)

#define RK3576_USB1_RESET_ID        563
#define RK3576_COMBPHY1_APB_RST_ID  0x20007
#define RK3576_COMBPHY1_RST_ID      0x20018
#define RK3576_USB2PHY1_APB_RST_ID  0x8000a
#define RK3576_USB2PHY1_RST_ID      0x80018

#define RK3576_USB2PHY1_SUSPEND     0x2000
#define RK3576_USB2PHY1_REFCLK      0x2004
#define RK3576_USB2PHY1_CLKOUT      0x2008
#define RK3576_USB2PHY1_TUNE0       0x200c
#define RK3576_USB2PHY1_TUNE1       0x2010

#define RK3576_COMBPHY_REG(n)       ((n) << 2)
#define RK3576_COMBPHY_READY        (1u << 6)
#define RK3576_COMBPHY_READY_US     1000

#define RK3576_PIPE_PHY_CON0        0x0000
#define RK3576_PIPE_PHY_CON1        0x0004
#define RK3576_PIPE_PHY_CON2        0x0008
#define RK3576_PIPE_PHY_STATUS0     0x0034

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rk3576_usbhost_getreg(uint32_t offset)
{
  return getreg32(RK3576_USB1_ADDR + offset);
}

static void rk3576_usbhost_putreg(uint32_t value, uint32_t offset)
{
  putreg32(value, RK3576_USB1_ADDR + offset);
}

static void rk3576_usbhost_grf_write(uintptr_t addr, uint32_t mask,
                                     uint32_t value)
{
  putreg32(RK3576_HIWORD(mask, value), addr);
}

static void rk3576_usbhost_reset(uint32_t id, bool assert)
{
  uintptr_t regaddr;
  uint32_t bit;

  regaddr = RK3576_CRU_ADDR +
            RK3576_CRU_SOFTRST_CON((id & 0xfffff) / 16);
  bit = 1u << (id & 15);
  rk3576_usbhost_grf_write(regaddr, bit, assert ? bit : 0);
}

static void rk3576_usbhost_clocks_enable(void)
{
  uint32_t mask;

  mask = RK3576_USB1_ACLK_GATE | RK3576_USB1_REF_GATE |
         RK3576_USB1_SUSPEND_GATE | RK3576_USB1_MMU_ACLK_GATE;
  rk3576_usbhost_grf_write(RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(35),
                           mask, 0);

  mask = RK3576_USB1_MMU_SLV_GATE | RK3576_USB1_PIPE_GATE;
  rk3576_usbhost_grf_write(RK3576_CRU_ADDR + RK3576_CRU_GATE_CON(36),
                           mask, 0);

  mask = RK3576_PHPPHY_ROOT_GATE | RK3576_COMBPHY1_APB_GATE;
  rk3576_usbhost_grf_write(RK3576_PPLL_CRU_ADDR +
                           RK3576_PHP_GATE_CON(0), mask, 0);

  /* PPLL is fixed at 1.3 GHz.  Divide by 13 for the 100 MHz Combo PHY
   * reference and route that source to Combo PHY1.
   */

  rk3576_usbhost_grf_write(RK3576_PPLL_CRU_ADDR +
                           RK3576_PHP_CLKSEL_CON(0),
                           (0x1fu << 2) | (3u << 14), 12u << 2);

  mask = RK3576_PCIE_100M_GATE | RK3576_COMBPHY1_REF_GATE;
  rk3576_usbhost_grf_write(RK3576_PPLL_CRU_ADDR +
                           RK3576_PHP_GATE_CON(1), mask, 0);

  /* USB2 PHY reference clock uses the 24 MHz crystal. */

  rk3576_usbhost_grf_write(RK3576_PMU0_GRF_ADDR + 0x18, 1u << 4, 0);
}

static void rk3576_usbhost_usb2phy_initialize(void)
{
  rk3576_usbhost_reset(RK3576_USB2PHY1_APB_RST_ID, true);
  rk3576_usbhost_reset(RK3576_USB2PHY1_RST_ID, true);
  up_udelay(20);
  rk3576_usbhost_reset(RK3576_USB2PHY1_APB_RST_ID, false);

  /* Select 24 MHz, enable the PHY output clock and apply the RK3576
   * high-speed transmitter tuning from the original PHY table.
   */

  rk3576_usbhost_grf_write(RK3576_USB2PHY_GRF_ADDR +
                           RK3576_USB2PHY1_REFCLK, 0x7, 0x2);
  rk3576_usbhost_grf_write(RK3576_USB2PHY_GRF_ADDR +
                           RK3576_USB2PHY1_CLKOUT, 0x1, 0x1);
  rk3576_usbhost_grf_write(RK3576_USB2PHY_GRF_ADDR +
                           RK3576_USB2PHY1_TUNE1, 1u << 13, 0);
  rk3576_usbhost_grf_write(RK3576_USB2PHY_GRF_ADDR +
                           RK3576_USB2PHY1_TUNE0, 0x0f00, 0x0900);
  rk3576_usbhost_grf_write(RK3576_USB2PHY_GRF_ADDR +
                           RK3576_USB2PHY1_TUNE1, 0x0018, 0x0010);

  rk3576_usbhost_reset(RK3576_USB2PHY1_RST_ID, false);
  up_udelay(20);

  /* Exit suspend with FS termination, HS transceiver selection and the
   * downstream pull-downs enabled.
   */

  rk3576_usbhost_grf_write(RK3576_PHP_GRF_ADDR +
                           RK3576_USB2PHY1_SUSPEND, 0x01ff, 0x01d1);
}

static int rk3576_usbhost_combphy_initialize(void)
{
  uintptr_t base = RK3576_COMBPHY1_ADDR;
  uint32_t regval;
  int elapsed;

  rk3576_usbhost_reset(RK3576_COMBPHY1_APB_RST_ID, true);
  rk3576_usbhost_reset(RK3576_COMBPHY1_RST_ID, true);
  up_udelay(20);
  rk3576_usbhost_reset(RK3576_COMBPHY1_APB_RST_ID, false);

  /* Select USB3 and apply the RK3576 100 MHz reference-clock trim. */

  regval = getreg32(base + 0x7c);
  regval = (regval & ~(3u << 4)) | (1u << 4);
  putreg32(regval, base + 0x7c);

  regval = getreg32(base + 0x38);
  putreg32(regval | 1u, base + 0x38);

  regval = getreg32(base + 0x80);
  putreg32((regval & ~(7u << 2)) | (2u << 2), base + 0x80);

  putreg32(0x04, base + RK3576_COMBPHY_REG(0x0b));

  regval = getreg32(base + 0x14);
  putreg32((regval & ~(3u << 6)) | (1u << 6), base + 0x14);

  putreg32(0x32, base + RK3576_COMBPHY_REG(0x11));
  putreg32(0xf0, base + RK3576_COMBPHY_REG(0x0a));
  putreg32(0x0d, base + RK3576_COMBPHY_REG(0x14));

  rk3576_usbhost_grf_write(RK3576_PIPE_PHY1_GRF_ADDR +
                           RK3576_PIPE_PHY_CON2,
                           (1u << 15) | (1u << 12), 0);
  rk3576_usbhost_grf_write(RK3576_PIPE_PHY1_GRF_ADDR +
                           RK3576_PIPE_PHY_CON0, 0x3f, 0x04);
  rk3576_usbhost_grf_write(RK3576_PIPE_PHY1_GRF_ADDR +
                           RK3576_PIPE_PHY_CON1, 3u << 13, 2u << 13);

  rk3576_usbhost_reset(RK3576_COMBPHY1_RST_ID, false);

  for (elapsed = 0; elapsed < RK3576_COMBPHY_READY_US; elapsed += 10)
    {
      if ((getreg32(RK3576_PIPE_PHY1_GRF_ADDR +
                    RK3576_PIPE_PHY_STATUS0) &
           RK3576_COMBPHY_READY) != 0)
        {
          return OK;
        }

      up_udelay(10);
    }

  uerr("ERROR: USB1 Combo PHY did not become ready\n");
  return -ETIMEDOUT;
}

static int rk3576_usbhost_irq_attach(FAR void *arg, xcpt_t handler,
                                     FAR void *priv)
{
  int ret;

  (void)arg;

  ret = irq_attach(RK3576_IRQ_USB1, handler, priv);
  if (ret < 0)
    {
      return ret;
    }

  up_enable_irq(RK3576_IRQ_USB1);
  return OK;
}

static void rk3576_usbhost_irq_detach(FAR void *arg)
{
  (void)arg;

  up_disable_irq(RK3576_IRQ_USB1);
  irq_detach(RK3576_IRQ_USB1);
}

static int rk3576_usbhost_core_initialize(void)
{
  uint32_t regval;

  regval = rk3576_usbhost_getreg(DWC3_GSNPSID);
  if ((regval & RK3576_DWC3_ID_MASK) != RK3576_DWC3_ID_VALUE)
    {
      uerr("ERROR: USB1 DWC3 core unavailable: GSNPSID=%08" PRIx32 "\n",
           regval);
      return -ENODEV;
    }

  /* Reset the DWC3 core with both PHY interfaces held in reset. */

  regval = rk3576_usbhost_getreg(DWC3_GCTL);
  rk3576_usbhost_putreg(regval | GCTL_CORESOFTRESET, DWC3_GCTL);

  regval = rk3576_usbhost_getreg(DWC3_GUSB2PHYCFG);
  rk3576_usbhost_putreg(regval | GUSB2PHYCFG_PHYSOFTRST,
                        DWC3_GUSB2PHYCFG);

  regval = rk3576_usbhost_getreg(DWC3_GUSB3PIPECTL);
  rk3576_usbhost_putreg(regval | GUSB3PIPECTL_PHYSOFTRST,
                        DWC3_GUSB3PIPECTL);
  up_mdelay(RK3576_DWC3_RESET_DELAYMS);

  regval = rk3576_usbhost_getreg(DWC3_GUSB2PHYCFG);
  rk3576_usbhost_putreg(regval & ~GUSB2PHYCFG_PHYSOFTRST,
                        DWC3_GUSB2PHYCFG);

  regval = rk3576_usbhost_getreg(DWC3_GUSB3PIPECTL);
  rk3576_usbhost_putreg(regval & ~GUSB3PIPECTL_PHYSOFTRST,
                        DWC3_GUSB3PIPECTL);

  regval = rk3576_usbhost_getreg(DWC3_GCTL);
  rk3576_usbhost_putreg(regval & ~GCTL_CORESOFTRESET, DWC3_GCTL);
  up_mdelay(RK3576_DWC3_RESET_DELAYMS);

  /* Select the host controller and apply the RK3576 device-tree quirks
   * shared with USB0: 16-bit UTMI, no U2 free-running clock assumption,
   * and no PHY auto-suspend while xHCI starts.
   */

  regval = rk3576_usbhost_getreg(DWC3_GCTL);
  regval &= ~(GCTL_PRTCAPDIR_MASK | GCTL_DSBLCLKGTNG);
  regval |= GCTL_PRTCAP_HOST;
  rk3576_usbhost_putreg(regval, DWC3_GCTL);

  regval = rk3576_usbhost_getreg(DWC3_GUSB2PHYCFG);
  regval &= ~(GUSB2PHYCFG_SUSPHY | GUSB2PHYCFG_ENBLSLPM |
              GUSB2PHYCFG_TRDTIM_MASK | GUSB2PHYCFG_U2FREECLK);
  regval |= GUSB2PHYCFG_PHYIF | GUSB2PHYCFG_TRDTIM(5);
  rk3576_usbhost_putreg(regval, DWC3_GUSB2PHYCFG);

  regval = rk3576_usbhost_getreg(DWC3_GUSB3PIPECTL);
  regval &= ~(GUSB3PIPECTL_SUSPEND | GUSB3PIPECTL_PHYSOFTRST);
  regval |= GUSB3PIPECTL_DISRXDETINP3;
  rk3576_usbhost_putreg(regval, DWC3_GUSB3PIPECTL);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

FAR struct usbhost_connection_s *rk3576_usbhost_initialize(void)
{
  static const struct xhci_bus_ops_s g_rk3576_xhci_ops =
  {
    .irq_attach = rk3576_usbhost_irq_attach,
    .irq_detach = rk3576_usbhost_irq_detach,
  };
  int ret;

  rk3576_usbhost_reset(RK3576_USB1_RESET_ID, true);
  rk3576_usbhost_clocks_enable();
  rk3576_usbhost_usb2phy_initialize();

  ret = rk3576_usbhost_combphy_initialize();
  if (ret < 0)
    {
      return NULL;
    }

  rk3576_usbhost_reset(RK3576_USB1_RESET_ID, false);
  up_udelay(20);

  ret = rk3576_usbhost_core_initialize();
  if (ret < 0)
    {
      return NULL;
    }

  return xhci_initialize("rk3576-usb1", RK3576_USB1_ADDR,
                         &g_rk3576_xhci_ops, NULL);
}

#endif /* CONFIG_RK3576_USBHOST */
