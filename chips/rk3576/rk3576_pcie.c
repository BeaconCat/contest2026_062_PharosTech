/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pcie.c
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
 * RK3576 PCIe root complex driver (Synopsys DesignWare core + Rockchip
 * client glue), implementing the NuttX struct pci_controller_s /
 * struct pci_ops_s interface from <nuttx/pci/pci.h>.
 *
 * Address translation is the part that is easy to get wrong, so it is
 * spelled out here.  Three CPU address ranges are handed to the controller
 * by the vendor device tree for port 0:
 *
 *   0x20000000..0x200FFFFF  configuration window (1MB)
 *   0x20100000..0x201FFFFF  PCI I/O space        (1MB)
 *   0x20200000..0x20FFFFFF  PCI 32-bit MMIO      (14MB)
 *
 * The DesignWare internal address translation unit (iATU) maps them onto
 * PCIe TLPs.  This driver uses the "unrolled" iATU register file at
 * DBI + 0x300000 (the DBI region is 4MB, which is exactly why it is that
 * large) and allocates three outbound regions plus one inbound region:
 *
 *   OB0  configuration  retargeted per access to CFG0 (bus 1) or CFG1
 *                       (bus >= 2) with target = bus<<24 | devfn<<16
 *   OB1  MMIO           identity mapped, CPU addr == PCI addr
 *   OB2  I/O            identity mapped
 *   IB0  inbound MEM    identity mapped over the low 4GB so that a device
 *                       mastering to DRAM reaches the right physical page
 *
 * Root-complex configuration space (bus 0, device 0) is not reached through
 * the configuration window at all: it is the DBI aperture itself.
 *
 * Interrupts: the four legacy INTx lines are aggregated by the client block
 * into a single GIC SPI, exposed through rk3576_pcie_legacy_irq().  MSI is
 * not implemented yet (see TODO near rk3576_pcie_setup_irq).
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
#include <sys/param.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/compiler.h>
#include <nuttx/nuttx.h>
#include <nuttx/pci/pci.h>
#include <nuttx/pci/pci_regs.h>

#include "arm64_internal.h"
#include "hardware/rk3576_cru.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_pcie.h"
#include "rk3576_combphy.h"
#include "rk3576_pcie.h"

#ifdef CONFIG_RK3576_PCIE

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* iATU region assignment (see the file banner). */

#define RK3576_PCIE_ATU_OB_CFG 0
#define RK3576_PCIE_ATU_OB_MEM 1
#define RK3576_PCIE_ATU_OB_IO  2
#define RK3576_PCIE_ATU_IB_MEM 0

/* Size of the slice of the configuration window used for one bus/device
 * translation.  A single 64KB page covers the whole 4KB config space of
 * one function with room to spare, and keeps the iATU limit aligned.
 */

#define RK3576_PCIE_CFG_SLICE 0x10000

/* Inbound window: identity map the low 4GB so bus masters reach DRAM. */

#define RK3576_PCIE_IB_LIMIT 0xffffffffu

/* Bus numbering of the root complex itself. */

#define RK3576_PCIE_ROOT_BUS 0
#define RK3576_PCIE_SEC_BUS  1
#define RK3576_PCIE_SUB_BUS  0x0f

/* Only device 0 exists on the point-to-point link below the root port. */

#define RK3576_PCIE_DEVFN_DEV(devfn) (((devfn) >> 3) & 0x1f)

/* Value returned for configuration reads that have no responder. */

#define RK3576_PCIE_CFG_ABSENT 0xffffffffu

/* Link training budget.  The PCIe base spec allows 100 ms from the end of
 * fundamental reset before software may enumerate; Rockchip uses a longer
 * timeout because some endpoints train slowly.
 */

#define RK3576_PCIE_LINK_TIMEOUT_US 500000
#define RK3576_PCIE_LINK_POLL_US    1000

/* PERST# timing: >100 us low, then >100 ms before the first config access
 * (PCIe CEM "T-PVPERL" is 100 ms; the link poll below absorbs the rest).
 */

#define RK3576_PCIE_PERST_LOW_US    200
#define RK3576_PCIE_PERST_SETTLE_US 20000

/* Number of lanes wired on the RK3576 root ports (num-lanes = 1). */

#define RK3576_PCIE_NUM_LANES 1

/* max-link-speed = 2 (Gen2, 5 GT/s) per the vendor device tree. */

#define RK3576_PCIE_MAX_LINK_SPEED 2

/* Software reset ids from the vendor device tree, main CRU domain:
 *   port 0: pipe = 0x22f -> SOFTRST_CON34 bit 15
 *           apb  = 0x22d -> SOFTRST_CON34 bit 13
 *   port 1: pipe = 0x249 -> SOFTRST_CON36 bit 9
 *           apb  = 0x247 -> SOFTRST_CON36 bit 7
 * register = SOFTRST_CON(id / 16), bit = id % 16.
 *
 * TODO: cross-check the SOFTRST_CON indices against the RK3576 TRM CRU
 * chapter; the encoding above is derived from the vendor device tree.
 */

#define RK3576_PCIE_RST_BANK(id) (((id)&0xffff) / 16)
#define RK3576_PCIE_RST_BIT(id)  (((id)&0xffff) % 16)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Immutable per-port description. */

struct rk3576_pcie_config_s
{
  uintptr_t apb;     /* Rockchip client block base */
  uintptr_t dbi;     /* DesignWare DBI base */
  uintptr_t cfg;     /* Configuration window CPU base */
  uintptr_t io_base; /* PCI I/O window CPU base */
  size_t io_size;
  uintptr_t mem_base; /* PCI MMIO window CPU base */
  size_t mem_size;
  uint32_t pipe_rst_id;
  uint32_t apb_rst_id;
  int phyid;      /* Combo PHY feeding this port */
  int legacy_irq; /* Aggregated INTx GIC interrupt */
  const char *clk_mst;
  const char *clk_slv;
  const char *clk_dbi;
  const char *clk_pclk;
  const char *clk_aux;
};

/* Per-port runtime state.  The NuttX controller object is embedded first so
 * that container_of() can recover the port from a struct pci_bus_s.
 */

struct rk3576_pcie_s
{
  struct pci_controller_s ctrl;
  const struct rk3576_pcie_config_s *cfg;
  int port;
  bool linkup;

  /* Currently programmed outbound configuration translation, cached so a
   * burst of accesses to the same function does not reprogram the iATU.
   */

  uint32_t cfg_target;
  uint32_t cfg_type;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_pcie_reset_assert(uint32_t id);
static void rk3576_pcie_reset_deassert(uint32_t id);
static int rk3576_pcie_clk_init(struct rk3576_pcie_s *priv);
static void rk3576_pcie_dbi_wr_enable(struct rk3576_pcie_s *priv, bool enable);
static void rk3576_pcie_atu_outbound(struct rk3576_pcie_s *priv,
                                     unsigned int index, uint32_t type,
                                     uint64_t cpu_addr, uint64_t pci_addr,
                                     uint64_t size);
static void rk3576_pcie_atu_inbound(struct rk3576_pcie_s *priv,
                                    unsigned int index, uint32_t type,
                                    uint64_t pci_addr, uint64_t cpu_addr,
                                    uint64_t size);
static void rk3576_pcie_setup_windows(struct rk3576_pcie_s *priv);
static void rk3576_pcie_setup_rc(struct rk3576_pcie_s *priv);
static void rk3576_pcie_setup_irq(struct rk3576_pcie_s *priv);
static void rk3576_pcie_enable_ltssm(struct rk3576_pcie_s *priv, bool enable);
static bool rk3576_pcie_link_is_up(struct rk3576_pcie_s *priv);
static int rk3576_pcie_wait_link(struct rk3576_pcie_s *priv);
static int rk3576_pcie_cfg_prepare(struct rk3576_pcie_s *priv, uint8_t busno,
                                   uint32_t devfn, int where,
                                   FAR uintptr_t *addr);
static int rk3576_pcie_read_config(FAR struct pci_bus_s *bus, uint32_t devfn,
                                   int where, int size, FAR uint32_t *val);
static int rk3576_pcie_write_config(FAR struct pci_bus_s *bus, uint32_t devfn,
                                    int where, int size, uint32_t val);
static int rk3576_pcie_read_io(FAR struct pci_bus_s *bus, uintptr_t addr,
                               int size, FAR uint32_t *val);
static int rk3576_pcie_write_io(FAR struct pci_bus_s *bus, uintptr_t addr,
                                int size, uint32_t val);
static FAR void *rk3576_pcie_map(FAR struct pci_bus_s *bus, uintptr_t start,
                                 uintptr_t end);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct pci_ops_s g_rk3576_pcie_ops = {
  .read = rk3576_pcie_read_config,
  .write = rk3576_pcie_write_config,
  .read_io = rk3576_pcie_read_io,
  .write_io = rk3576_pcie_write_io,
  .map = rk3576_pcie_map,
};

static const struct rk3576_pcie_config_s
    g_rk3576_pcie_config[RK3576_PCIE_NPORTS] = {
      {
          .apb = RK3576_PCIE0_APB_ADDR,
          .dbi = RK3576_PCIE0_DBI_ADDR,
          .cfg = RK3576_PCIE0_CFG_ADDR,
          .io_base = RK3576_PCIE0_IO_ADDR,
          .io_size = RK3576_PCIE0_IO_SIZE,
          .mem_base = RK3576_PCIE0_MEM_ADDR,
          .mem_size = RK3576_PCIE0_MEM_SIZE,
          .pipe_rst_id = 0x22f,
          .apb_rst_id = 0x22d,
          .phyid = RK3576_COMBPHY0,
          .legacy_irq = RK3576_IRQ_PCIE0_LEGACY,
          .clk_mst = "aclk_pcie0_mst_en",
          .clk_slv = "aclk_pcie0_slv_en",
          .clk_dbi = "aclk_pcie0_dbi_en",
          .clk_pclk = "pclk_pcie0_en",
          .clk_aux = "clk_pcie0_aux_en",
      },
      {
          .apb = RK3576_PCIE1_APB_ADDR,
          .dbi = RK3576_PCIE1_DBI_ADDR,
          .cfg = RK3576_PCIE1_CFG_ADDR,
          .io_base = RK3576_PCIE1_IO_ADDR,
          .io_size = RK3576_PCIE1_IO_SIZE,
          .mem_base = RK3576_PCIE1_MEM_ADDR,
          .mem_size = RK3576_PCIE1_MEM_SIZE,
          .pipe_rst_id = 0x249,
          .apb_rst_id = 0x247,
          .phyid = RK3576_COMBPHY1,
          .legacy_irq = RK3576_IRQ_PCIE1_LEGACY,
          .clk_mst = "aclk_pcie1_mst_en",
          .clk_slv = "aclk_pcie1_slv_en",
          .clk_dbi = "aclk_pcie1_dbi_en",
          .clk_pclk = "pclk_pcie1_en",
          .clk_aux = "clk_pcie1_aux_en",
      },
    };

static struct rk3576_pcie_s g_rk3576_pcie[RK3576_PCIE_NPORTS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pcie_reset_assert
 *
 * Description:
 *   Assert one main-CRU software reset identified by its vendor reset id.
 *
 ****************************************************************************/

static void rk3576_pcie_reset_assert(uint32_t id)
{
  uintptr_t reg =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_PCIE_RST_BANK(id));
  uint32_t bit = 1u << RK3576_PCIE_RST_BIT(id);

  putreg32(RK3576_PCIE_HIWORD(bit, bit), reg);
}

/****************************************************************************
 * Name: rk3576_pcie_reset_deassert
 *
 * Description:
 *   Release one main-CRU software reset identified by its vendor reset id.
 *
 ****************************************************************************/

static void rk3576_pcie_reset_deassert(uint32_t id)
{
  uintptr_t reg =
      RK3576_CRU_ADDR + RK3576_CRU_SOFTRST_CON(RK3576_PCIE_RST_BANK(id));
  uint32_t bit = 1u << RK3576_PCIE_RST_BIT(id);

  putreg32(RK3576_PCIE_HIWORD(bit, 0), reg);
}

/****************************************************************************
 * Name: rk3576_pcie_clk_init
 *
 * Description:
 *   Sole point of contact with the NuttX CLK framework for this driver:
 *   enables the shared PHP (peripheral high-speed) bus root clock plus the
 *   five per-port clocks named by the vendor device tree (pclk, aclk_dbi,
 *   aclk_mst, aclk_slv and aux).
 *
 *   clk_enable() is reference counted, so bringing up the second port does
 *   not disturb the first.
 *
 * Input Parameters:
 *   priv - Port state, already bound to its configuration
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_pcie_clk_init(struct rk3576_pcie_s *priv)
{
  const char *names[] = {
    "aclk_php_root_en", priv->cfg->clk_pclk, priv->cfg->clk_dbi,
    priv->cfg->clk_mst, priv->cfg->clk_slv,  priv->cfg->clk_aux,
  };

  struct clk_s *clk;
  size_t i;
  int ret;

  for (i = 0; i < nitems(names); i++)
    {
      clk = clk_get(names[i]);
      if (clk == NULL)
        {
          pcierr("ERROR: failed to get %s
", names[i]);
          return -ENODEV;
        }

      ret = clk_enable(clk);
      if (ret < 0)
        {
          pcierr("ERROR: failed to enable %s: %d
", names[i], ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_dbi_wr_enable
 *
 * Description:
 *   Open or close write access to the read-only bits of the root complex'
 *   own configuration header (vendor/device id, class code, BARs).
 *
 ****************************************************************************/

static void rk3576_pcie_dbi_wr_enable(struct rk3576_pcie_s *priv, bool enable)
{
  uintptr_t reg = priv->cfg->dbi + RK3576_PCIE_MISC_CONTROL_1;
  uint32_t val = getreg32(reg);

  if (enable)
    {
      val |= RK3576_PCIE_DBI_RO_WR_EN;
    }
  else
    {
      val &= ~RK3576_PCIE_DBI_RO_WR_EN;
    }

  putreg32(val, reg);
}

/****************************************************************************
 * Name: rk3576_pcie_atu_outbound
 *
 * Description:
 *   Program one outbound iATU region: CPU addresses in
 *   [cpu_addr, cpu_addr + size) generate TLPs of the given type carrying
 *   addresses starting at pci_addr.
 *
 *   The limit register holds the last byte of the window, so size must be
 *   non-zero.  The upper limit register is written as well, which is what
 *   makes windows larger than 4GB (and windows whose base is above 4GB)
 *   work; the INCREASE_REGION_SIZE bit in CTRL1 enables it.
 *
 * Input Parameters:
 *   priv     - Port state
 *   index    - Outbound region index
 *   type     - RK3576_PCIE_ATU_TYPE_* TLP type
 *   cpu_addr - First CPU address covered by the window
 *   pci_addr - PCI address the first CPU address translates to
 *   size     - Window size in bytes
 *
 ****************************************************************************/

static void rk3576_pcie_atu_outbound(struct rk3576_pcie_s *priv,
                                     unsigned int index, uint32_t type,
                                     uint64_t cpu_addr, uint64_t pci_addr,
                                     uint64_t size)
{
  uintptr_t base = priv->cfg->dbi + RK3576_PCIE_ATU_OB_OFFSET(index);
  uint64_t limit = cpu_addr + size - 1;

  putreg32((uint32_t)cpu_addr, base + RK3576_PCIE_ATU_LOWER_BASE);
  putreg32((uint32_t)(cpu_addr >> 32), base + RK3576_PCIE_ATU_UPPER_BASE);
  putreg32((uint32_t)limit, base + RK3576_PCIE_ATU_LIMIT);
  putreg32((uint32_t)(limit >> 32), base + RK3576_PCIE_ATU_UPPER_LIMIT);
  putreg32((uint32_t)pci_addr, base + RK3576_PCIE_ATU_LOWER_TGT);
  putreg32((uint32_t)(pci_addr >> 32), base + RK3576_PCIE_ATU_UPPER_TGT);

  putreg32(type | RK3576_PCIE_ATU_INCREASE_REGION_SIZE,
           base + RK3576_PCIE_ATU_CTRL1);
  putreg32(RK3576_PCIE_ATU_ENABLE, base + RK3576_PCIE_ATU_CTRL2);

  /* CTRL2 is posted; read it back so the window is live before the caller
   * issues the access that depends on it.
   */

  while ((getreg32(base + RK3576_PCIE_ATU_CTRL2) & RK3576_PCIE_ATU_ENABLE) ==
         0)
    {
    }
}

/****************************************************************************
 * Name: rk3576_pcie_atu_inbound
 *
 * Description:
 *   Program one inbound iATU region in address-match mode: PCI addresses in
 *   [pci_addr, pci_addr + size) are forwarded to CPU/DRAM addresses
 *   starting at cpu_addr.
 *
 ****************************************************************************/

static void rk3576_pcie_atu_inbound(struct rk3576_pcie_s *priv,
                                    unsigned int index, uint32_t type,
                                    uint64_t pci_addr, uint64_t cpu_addr,
                                    uint64_t size)
{
  uintptr_t base = priv->cfg->dbi + RK3576_PCIE_ATU_IB_OFFSET(index);
  uint64_t limit = pci_addr + size - 1;

  putreg32((uint32_t)pci_addr, base + RK3576_PCIE_ATU_LOWER_BASE);
  putreg32((uint32_t)(pci_addr >> 32), base + RK3576_PCIE_ATU_UPPER_BASE);
  putreg32((uint32_t)limit, base + RK3576_PCIE_ATU_LIMIT);
  putreg32((uint32_t)(limit >> 32), base + RK3576_PCIE_ATU_UPPER_LIMIT);
  putreg32((uint32_t)cpu_addr, base + RK3576_PCIE_ATU_LOWER_TGT);
  putreg32((uint32_t)(cpu_addr >> 32), base + RK3576_PCIE_ATU_UPPER_TGT);

  putreg32(type | RK3576_PCIE_ATU_INCREASE_REGION_SIZE,
           base + RK3576_PCIE_ATU_CTRL1);

  /* Address-match mode: BAR_MODE_ENABLE stays clear. */

  putreg32(RK3576_PCIE_ATU_ENABLE, base + RK3576_PCIE_ATU_CTRL2);

  while ((getreg32(base + RK3576_PCIE_ATU_CTRL2) & RK3576_PCIE_ATU_ENABLE) ==
         0)
    {
    }
}

/****************************************************************************
 * Name: rk3576_pcie_setup_windows
 *
 * Description:
 *   Program the static iATU windows: MMIO and I/O outbound (identity
 *   mapped, matching the "ranges" property of the vendor device tree) and
 *   one inbound window covering the low 4GB so that bus masters can reach
 *   DRAM at its physical address.
 *
 *   The configuration outbound region is left unprogrammed here; it is
 *   retargeted on demand by rk3576_pcie_cfg_prepare().
 *
 ****************************************************************************/

static void rk3576_pcie_setup_windows(struct rk3576_pcie_s *priv)
{
  const struct rk3576_pcie_config_s *cfg = priv->cfg;

  rk3576_pcie_atu_outbound(priv, RK3576_PCIE_ATU_OB_MEM,
                           RK3576_PCIE_ATU_TYPE_MEM, cfg->mem_base,
                           cfg->mem_base, cfg->mem_size);

  rk3576_pcie_atu_outbound(priv, RK3576_PCIE_ATU_OB_IO,
                           RK3576_PCIE_ATU_TYPE_IO, cfg->io_base, cfg->io_base,
                           cfg->io_size);

  rk3576_pcie_atu_inbound(priv, RK3576_PCIE_ATU_IB_MEM,
                          RK3576_PCIE_ATU_TYPE_MEM, 0, 0,
                          (uint64_t)RK3576_PCIE_IB_LIMIT + 1);
}

/****************************************************************************
 * Name: rk3576_pcie_setup_rc
 *
 * Description:
 *   Configure the DesignWare core as a root complex: lane count, target
 *   link speed, the root port's own type-1 header (class code, bus
 *   numbers, disabled BARs) and the command register.
 *
 ****************************************************************************/

static void rk3576_pcie_setup_rc(struct rk3576_pcie_s *priv)
{
  uintptr_t dbi = priv->cfg->dbi;
  uint32_t val;

  rk3576_pcie_dbi_wr_enable(priv, true);

  /* Lane count in the port logic. */

  val = getreg32(dbi + RK3576_PCIE_PL_PORT_LINK_CTRL);
  val &= ~RK3576_PCIE_PL_LINK_MODE_MASK;
  val |= RK3576_PCIE_PL_LINK_MODE_1_LANE;
  putreg32(val, dbi + RK3576_PCIE_PL_PORT_LINK_CTRL);

  val = getreg32(dbi + RK3576_PCIE_PL_GEN2_CTRL);
  val &= ~RK3576_PCIE_PL_LINK_WIDTH_MASK;
  val |= RK3576_PCIE_PL_LINK_WIDTH(RK3576_PCIE_NUM_LANES);
  val |= RK3576_PCIE_PL_SPEED_CHANGE;
  putreg32(val, dbi + RK3576_PCIE_PL_GEN2_CTRL);

  /* Advertise and target Gen2. */

  val = getreg32(dbi + RK3576_PCIE_CAP_LINK_CAP);
  val &= ~RK3576_PCIE_LINK_SPEED_MASK;
  val |= RK3576_PCIE_MAX_LINK_SPEED;
  putreg32(val, dbi + RK3576_PCIE_CAP_LINK_CAP);

  val = getreg32(dbi + RK3576_PCIE_CAP_LINK_CTRL2);
  val &= ~RK3576_PCIE_LINK_SPEED_MASK;
  val |= RK3576_PCIE_MAX_LINK_SPEED;
  putreg32(val, dbi + RK3576_PCIE_CAP_LINK_CTRL2);

  /* The root port must present itself as a PCI-to-PCI bridge or generic
   * enumeration code will not walk past it.
   */

  val = getreg32(dbi + RK3576_PCIE_DBI_CLASS_REV);
  val = (val & 0xff) | (RK3576_PCIE_CLASS_BRIDGE_PCI << 8);
  putreg32(val, dbi + RK3576_PCIE_DBI_CLASS_REV);

  /* The root port claims no memory of its own; zero both BARs so that the
   * resource allocator does not try to place them.
   */

  putreg32(0, dbi + RK3576_PCIE_DBI_BAR0);
  putreg32(0, dbi + RK3576_PCIE_DBI_BAR1);

  /* primary = 0, secondary = 1, subordinate = 0x0f (bus-range from DTS). */

  val = getreg32(dbi + RK3576_PCIE_DBI_PRIMARY_BUS);
  val &= 0xff000000;
  val |= RK3576_PCIE_ROOT_BUS | (RK3576_PCIE_SEC_BUS << 8) |
         (RK3576_PCIE_SUB_BUS << 16);
  putreg32(val, dbi + RK3576_PCIE_DBI_PRIMARY_BUS);

  rk3576_pcie_dbi_wr_enable(priv, false);

  /* Enable I/O, memory and bus mastering on the root port itself. */

  putreg16(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
               PCI_COMMAND_SERR,
           dbi + RK3576_PCIE_DBI_COMMAND);
}

/****************************************************************************
 * Name: rk3576_pcie_setup_irq
 *
 * Description:
 *   Unmask the four aggregated legacy INTx lines in the client block and
 *   mask the "misc" sources (hot reset, link-state change) that this driver
 *   does not act on yet.
 *
 *   TODO: MSI is not implemented.  The DesignWare core exposes an MSI
 *   controller (MSI_ADDR_LO/HI and 8 groups of 32 vectors in DBI space at
 *   0x820..0x8a0) whose output is the separate "msi" GIC SPI.  Wiring it up
 *   requires an interrupt-controller abstraction to hand vectors out
 *   through pci_ops_s::alloc_irq / connect_irq; until then endpoints must
 *   use legacy INTx via rk3576_pcie_legacy_irq().
 *
 ****************************************************************************/

static void rk3576_pcie_setup_irq(struct rk3576_pcie_s *priv)
{
  uintptr_t apb = priv->cfg->apb;

  /* Clear then unmask INTA..INTD (mask register is hiword-masked, writing
   * zero to a bit unmasks it).
   */

  putreg32(RK3576_PCIE_LEGACY_INT_MASK,
           apb + RK3576_PCIE_CLIENT_INTR_STATUS_LEGACY);
  putreg32(RK3576_PCIE_HIWORD_CLR(RK3576_PCIE_LEGACY_INT_MASK),
           apb + RK3576_PCIE_CLIENT_INTR_MASK_LEGACY);

  /* Mask the misc sources we do not service. */

  putreg32(RK3576_PCIE_HIWORD_SET(RK3576_PCIE_MISC_RDLH_LINK_UP_CHGED |
                                  RK3576_PCIE_MISC_LINK_REQ_RST_NOT),
           apb + RK3576_PCIE_CLIENT_INTR_MASK_MISC);
}

/****************************************************************************
 * Name: rk3576_pcie_enable_ltssm
 *
 * Description:
 *   Start or stop link training in the Rockchip client block.
 *
 ****************************************************************************/

static void rk3576_pcie_enable_ltssm(struct rk3576_pcie_s *priv, bool enable)
{
  uint32_t val = enable ? RK3576_PCIE_CLIENT_LTSSM_ENABLE
                        : RK3576_PCIE_CLIENT_LTSSM_DISABLE;

  putreg32(RK3576_PCIE_HIWORD(RK3576_PCIE_CLIENT_LTSSM_MASK, val),
           priv->cfg->apb + RK3576_PCIE_CLIENT_GENERAL_CON);
}

/****************************************************************************
 * Name: rk3576_pcie_link_is_up
 *
 * Description:
 *   Report whether both the physical and the data link layer are up and the
 *   LTSSM has reached L0.
 *
 ****************************************************************************/

static bool rk3576_pcie_link_is_up(struct rk3576_pcie_s *priv)
{
  uint32_t status = getreg32(priv->cfg->apb + RK3576_PCIE_CLIENT_LTSSM_STATUS);

  return (status & RK3576_PCIE_LINKUP) == RK3576_PCIE_LINKUP &&
         (status & RK3576_PCIE_LTSSM_STATE_MASK) == RK3576_PCIE_LTSSM_STATE_L0;
}

/****************************************************************************
 * Name: rk3576_pcie_wait_link
 *
 * Description:
 *   Poll until the link trains or the timeout expires.
 *
 * Returned Value:
 *   OK if the link came up, -ENODEV otherwise (an empty slot ends here).
 *
 ****************************************************************************/

static int rk3576_pcie_wait_link(struct rk3576_pcie_s *priv)
{
  uint32_t elapsed;

  for (elapsed = 0; elapsed < RK3576_PCIE_LINK_TIMEOUT_US;
       elapsed += RK3576_PCIE_LINK_POLL_US)
    {
      if (rk3576_pcie_link_is_up(priv))
        {
          pciinfo("pcie%d link up after %" PRIu32 " us, ltssm 0x%08" PRIx32
                  "\n",
                  priv->port, elapsed,
                  getreg32(priv->cfg->apb + RK3576_PCIE_CLIENT_LTSSM_STATUS));
          return OK;
        }

      up_udelay(RK3576_PCIE_LINK_POLL_US);
    }

  pciwarn("WARNING: pcie%d link down, ltssm 0x%08" PRIx32 "\n", priv->port,
          getreg32(priv->cfg->apb + RK3576_PCIE_CLIENT_LTSSM_STATUS));
  return -ENODEV;
}

/****************************************************************************
 * Name: rk3576_pcie_cfg_prepare
 *
 * Description:
 *   Work out the CPU address at which a configuration access to
 *   (busno, devfn, where) can be performed, reprogramming the configuration
 *   outbound iATU region when the target function changes.
 *
 *   Bus 0 is the root complex itself and lives in DBI space; there is no
 *   iATU involvement and only device 0 exists.  Bus 1 is reached with CFG0
 *   type TLPs (the link partner), deeper buses with CFG1 so that the switch
 *   below forwards them.
 *
 * Returned Value:
 *   OK with *addr set, or -ENODEV when nothing can respond.
 *
 ****************************************************************************/

static int rk3576_pcie_cfg_prepare(struct rk3576_pcie_s *priv, uint8_t busno,
                                   uint32_t devfn, int where,
                                   FAR uintptr_t *addr)
{
  uint32_t target;
  uint32_t type;

  if (busno == RK3576_PCIE_ROOT_BUS)
    {
      if (RK3576_PCIE_DEVFN_DEV(devfn) != 0)
        {
          return -ENODEV;
        }

      *addr = priv->cfg->dbi + (uintptr_t)where;
      return OK;
    }

  if (!priv->linkup)
    {
      return -ENODEV;
    }

  /* The link below the root port is point to point: only device 0 of the
   * secondary bus can answer.  Filtering here avoids a completion timeout
   * per non-existent device during enumeration.
   */

  if (busno == RK3576_PCIE_SEC_BUS && RK3576_PCIE_DEVFN_DEV(devfn) != 0)
    {
      return -ENODEV;
    }

  type = (busno == RK3576_PCIE_SEC_BUS) ? RK3576_PCIE_ATU_TYPE_CFG0
                                        : RK3576_PCIE_ATU_TYPE_CFG1;
  target = ((uint32_t)busno << 24) | ((devfn & 0xff) << 16);

  if (target != priv->cfg_target || type != priv->cfg_type)
    {
      rk3576_pcie_atu_outbound(priv, RK3576_PCIE_ATU_OB_CFG, type,
                               priv->cfg->cfg, target, RK3576_PCIE_CFG_SLICE);
      priv->cfg_target = target;
      priv->cfg_type = type;
    }

  *addr = priv->cfg->cfg + (uintptr_t)where;
  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_read_config
 *
 * Description:
 *   pci_ops_s::read -- read 1, 2 or 4 bytes of configuration space.
 *   Absent devices report all ones, which is what the enumerator expects.
 *
 ****************************************************************************/

static int rk3576_pcie_read_config(FAR struct pci_bus_s *bus, uint32_t devfn,
                                   int where, int size, FAR uint32_t *val)
{
  FAR struct rk3576_pcie_s *priv =
      container_of(bus->ctrl, struct rk3576_pcie_s, ctrl);
  uintptr_t addr;
  int ret;

  if (val == NULL || (size != 1 && size != 2 && size != 4))
    {
      return -EINVAL;
    }

  /* Natural alignment is required by the DesignWare DBI slave. */

  if ((where & (size - 1)) != 0)
    {
      return -EINVAL;
    }

  ret = rk3576_pcie_cfg_prepare(priv, bus->number, devfn, where, &addr);
  if (ret < 0)
    {
      *val = RK3576_PCIE_CFG_ABSENT >> ((4 - size) * 8);
      return OK;
    }

  switch (size)
    {
      case 1:
        *val = getreg8(addr);
        break;

      case 2:
        *val = getreg16(addr);
        break;

      default:
        *val = getreg32(addr);
        break;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_write_config
 *
 * Description:
 *   pci_ops_s::write -- write 1, 2 or 4 bytes of configuration space.
 *
 ****************************************************************************/

static int rk3576_pcie_write_config(FAR struct pci_bus_s *bus, uint32_t devfn,
                                    int where, int size, uint32_t val)
{
  FAR struct rk3576_pcie_s *priv =
      container_of(bus->ctrl, struct rk3576_pcie_s, ctrl);
  uintptr_t addr;
  bool dbi_ro = false;
  int ret;

  if (size != 1 && size != 2 && size != 4)
    {
      return -EINVAL;
    }

  if ((where & (size - 1)) != 0)
    {
      return -EINVAL;
    }

  ret = rk3576_pcie_cfg_prepare(priv, bus->number, devfn, where, &addr);
  if (ret < 0)
    {
      return ret;
    }

  /* Writes to the root port's own header go through the DBI, where the
   * bridge bus numbers and similar fields are read-only unless the
   * RO_WR_EN escape hatch is open.
   */

  if (bus->number == RK3576_PCIE_ROOT_BUS)
    {
      rk3576_pcie_dbi_wr_enable(priv, true);
      dbi_ro = true;
    }

  switch (size)
    {
      case 1:
        putreg8((uint8_t)val, addr);
        break;

      case 2:
        putreg16((uint16_t)val, addr);
        break;

      default:
        putreg32(val, addr);
        break;
    }

  if (dbi_ro)
    {
      rk3576_pcie_dbi_wr_enable(priv, false);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_read_io
 *
 * Description:
 *   pci_ops_s::read_io -- PCI I/O space is memory mapped on this platform,
 *   so an I/O access is a plain load from the outbound I/O window.
 *
 ****************************************************************************/

static int rk3576_pcie_read_io(FAR struct pci_bus_s *bus, uintptr_t addr,
                               int size, FAR uint32_t *val)
{
  UNUSED(bus);

  if (val == NULL)
    {
      return -EINVAL;
    }

  switch (size)
    {
      case 1:
        *val = getreg8(addr);
        break;

      case 2:
        *val = getreg16(addr);
        break;

      case 4:
        *val = getreg32(addr);
        break;

      default:
        return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_write_io
 *
 * Description:
 *   pci_ops_s::write_io -- see rk3576_pcie_read_io().
 *
 ****************************************************************************/

static int rk3576_pcie_write_io(FAR struct pci_bus_s *bus, uintptr_t addr,
                                int size, uint32_t val)
{
  UNUSED(bus);

  switch (size)
    {
      case 1:
        putreg8((uint8_t)val, addr);
        break;

      case 2:
        putreg16((uint16_t)val, addr);
        break;

      case 4:
        putreg32(val, addr);
        break;

      default:
        return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_pcie_map
 *
 * Description:
 *   pci_ops_s::map -- NuttX runs flat on this SoC and the PCIe apertures
 *   are already covered by the device MMU region, so the CPU address is
 *   also the virtual address.
 *
 ****************************************************************************/

static FAR void *rk3576_pcie_map(FAR struct pci_bus_s *bus, uintptr_t start,
                                 uintptr_t end)
{
  UNUSED(bus);
  UNUSED(end);

  return (FAR void *)start;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pcie_board_perst
 *
 * Description:
 *   Weak default for the board PERST# hook; see rk3576_pcie.h.  Boards that
 *   route reset-gpios to the slot override this.
 *
 ****************************************************************************/

void weak_function rk3576_pcie_board_perst(int port, bool asserted)
{
  UNUSED(port);
  UNUSED(asserted);
}

/****************************************************************************
 * Name: rk3576_pcie_legacy_irq
 *
 * Description:
 *   Return the GIC interrupt aggregating the four INTx lines of one port.
 *
 ****************************************************************************/

int rk3576_pcie_legacy_irq(int port)
{
  if (port < 0 || port >= RK3576_PCIE_NPORTS)
    {
      return -EINVAL;
    }

  return g_rk3576_pcie_config[port].legacy_irq;
}

/****************************************************************************
 * Name: rk3576_pcie_initialize
 *
 * Description:
 *   Bring up one RK3576 PCIe root port and register it with the NuttX PCI
 *   subsystem.  See rk3576_pcie.h for the full contract.
 *
 ****************************************************************************/

int rk3576_pcie_initialize(int port)
{
  struct rk3576_pcie_s *priv;
  const struct rk3576_pcie_config_s *cfg;
  int ret;

  if (port < 0 || port >= RK3576_PCIE_NPORTS)
    {
      return -EINVAL;
    }

  priv = &g_rk3576_pcie[port];
  cfg = &g_rk3576_pcie_config[port];

  priv->cfg = cfg;
  priv->port = port;
  priv->linkup = false;
  priv->cfg_target = UINT32_MAX;
  priv->cfg_type = UINT32_MAX;

  /* Hold the controller and the endpoint in reset for the whole
   * PHY/clock bring-up.
   */

  rk3576_pcie_reset_assert(cfg->apb_rst_id);
  rk3576_pcie_reset_assert(cfg->pipe_rst_id);
  rk3576_pcie_board_perst(port, true);

  ret = rk3576_pcie_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  /* The PIPE interface only makes sense once the combo PHY has locked. */

  ret = rk3576_combphy_pcie_init(cfg->phyid);
  if (ret < 0)
    {
      pcierr("ERROR: pcie%d combphy init failed: %d\n", port, ret);
      return ret;
    }

  rk3576_pcie_reset_deassert(cfg->pipe_rst_id);
  rk3576_pcie_reset_deassert(cfg->apb_rst_id);
  up_udelay(RK3576_PCIE_PERST_LOW_US);

  /* Root complex mode must be selected before the LTSSM is allowed to run,
   * and the "enhanced" LTSSM enable makes the core honour app_ltssm_enable
   * transitions rather than starting on reset release.
   */

  putreg32(RK3576_PCIE_HIWORD(RK3576_PCIE_CLIENT_MODE_MASK,
                              RK3576_PCIE_CLIENT_RC_MODE),
           cfg->apb + RK3576_PCIE_CLIENT_GENERAL_CON);

  putreg32(RK3576_PCIE_HIWORD_SET(RK3576_PCIE_CLIENT_LTSSM_EN_ENHANCE),
           cfg->apb + RK3576_PCIE_CLIENT_HOT_RESET_CTRL);

  rk3576_pcie_enable_ltssm(priv, false);

  rk3576_pcie_setup_rc(priv);
  rk3576_pcie_setup_windows(priv);
  rk3576_pcie_setup_irq(priv);

  /* Release the endpoint and start training. */

  rk3576_pcie_board_perst(port, false);
  up_udelay(RK3576_PCIE_PERST_SETTLE_US);

  rk3576_pcie_enable_ltssm(priv, true);

  ret = rk3576_pcie_wait_link(priv);
  if (ret < 0)
    {
      rk3576_pcie_enable_ltssm(priv, false);
      rk3576_combphy_uninit(cfg->phyid);
      return ret;
    }

  priv->linkup = true;

  /* Hand the port to the PCI core, which walks the bus behind it. */

  priv->ctrl.ops = &g_rk3576_pcie_ops;

  priv->ctrl.io.start = cfg->io_base;
  priv->ctrl.io.end = cfg->io_base + cfg->io_size - 1;
  priv->ctrl.io.flags = PCI_RESOURCE_IO;

  priv->ctrl.mem.start = cfg->mem_base;
  priv->ctrl.mem.end = cfg->mem_base + cfg->mem_size - 1;
  priv->ctrl.mem.flags = PCI_RESOURCE_MEM;

  /* No prefetchable aperture is programmed: the 64-bit prefetch range in
   * the vendor device tree would need a fourth outbound window and the
   * device tree only promises two (num-ob-windows = 2), which the
   * configuration and MMIO windows already consume.
   *
   * TODO: revisit once the real outbound window count is confirmed on
   * silicon; the core reports eight viewports (num-viewport = 8).
   */

  ret = pci_register_controller(&priv->ctrl);
  if (ret < 0)
    {
      pcierr("ERROR: pcie%d controller registration failed: %d\n", port, ret);
      return ret;
    }

  pciinfo("pcie%d root complex registered\n", port);
  return OK;
}

#endif /* CONFIG_RK3576_PCIE */
