/****************************************************************************
 * chips/rk3576/rk3576_gmac.c
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
 * RK3576 gigabit Ethernet driver.
 *
 * The SoC carries two Synopsys DesignWare MAC 4.20a ("dwmac4") controllers
 * at 0x2A220000 and 0x2A230000.  Both are wired as RGMII on the KICKPI-K7
 * with the receive delay generated inside the PHY ("rgmii-rxid"), so only
 * the transmit delay line of the Rockchip glue is programmed.
 *
 * The driver implements the NuttX struct net_driver_s interface and uses a
 * single DMA channel with one transmit and one receive queue, which matches
 * the vendor device tree ("snps,tx-queues-to-use = 1").  Descriptor rings
 * and packet buffers come from rk3576_dma_alloc(), which guarantees a
 * physical address below 4 GiB — the dwmac4 descriptor pointers are 32-bit.
 *
 * Descriptors are padded to one D-cache line each (the DMA descriptor skip
 * length feature is used to tell the controller about the padding) so that
 * cache maintenance on one descriptor can never corrupt a neighbour that the
 * DMA engine owns.  The vendor device tree marks the controller
 * "dma-coherent"; the explicit maintenance done here is harmless in that
 * case and required if the coherent path is not enabled.
 *
 * PHY management is generic clause 22: auto-negotiation is started at ifup
 * and a watchdog polls the link every second, reprogramming the MAC speed,
 * duplex and the Rockchip RGMII clock divider whenever the link changes.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <net/if.h>

#include <nuttx/arch.h>
#include <nuttx/clk/clk.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include <nuttx/net/arp.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/net/ioctl.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#ifdef CONFIG_NET_PKT
#include <nuttx/net/pkt.h>
#endif

#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_gmac.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_gmac.h"

#ifdef CONFIG_RK3576_OTP
#include "rk3576_otp.h"
#endif

#ifdef CONFIG_RK3576_GMAC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_SCHED_WORKQUEUE)
#error Work queue support is required (CONFIG_SCHED_WORKQUEUE)
#endif

#if !defined(CONFIG_RK3576_DMA_ALLOC)
#error CONFIG_RK3576_DMA_ALLOC is required by the GMAC driver
#endif

/* Use the dedicated Ethernet work queue when it exists. */

#if defined(CONFIG_NET_ETHERNET_WORKQUEUE) || \
    defined(CONFIG_NETDEV_WORK_THREAD)
#define RK3576_GMAC_WORK ETHWORK
#else
#define RK3576_GMAC_WORK LPWORK
#endif

/* Ring sizes.  Both must be powers of two so the wrap can use a mask. */

#ifndef CONFIG_RK3576_GMAC_NTXDESC
#define CONFIG_RK3576_GMAC_NTXDESC 8
#endif

#ifndef CONFIG_RK3576_GMAC_NRXDESC
#define CONFIG_RK3576_GMAC_NRXDESC 8
#endif

#define RK3576_GMAC_NTXDESC CONFIG_RK3576_GMAC_NTXDESC
#define RK3576_GMAC_NRXDESC CONFIG_RK3576_GMAC_NRXDESC

/* MDIO address of the PHY.  Both ports use address 0 on the KICKPI-K7. */

#ifndef CONFIG_RK3576_GMAC_PHYADDR
#define CONFIG_RK3576_GMAC_PHYADDR 0
#endif

#if (RK3576_GMAC_NTXDESC & (RK3576_GMAC_NTXDESC - 1)) != 0
#error CONFIG_RK3576_GMAC_NTXDESC must be a power of two
#endif

#if (RK3576_GMAC_NRXDESC & (RK3576_GMAC_NRXDESC - 1)) != 0
#error CONFIG_RK3576_GMAC_NRXDESC must be a power of two
#endif

#define RK3576_GMAC_TXMASK (RK3576_GMAC_NTXDESC - 1)
#define RK3576_GMAC_RXMASK (RK3576_GMAC_NRXDESC - 1)

/* Packet buffer size, rounded up to a D-cache line. */

#define RK3576_GMAC_ALIGN 64
#define RK3576_GMAC_ROUNDUP(n) \
  (((n) + RK3576_GMAC_ALIGN - 1) & ~(RK3576_GMAC_ALIGN - 1))
#define RK3576_GMAC_BUFSIZE \
  RK3576_GMAC_ROUNDUP(CONFIG_NET_ETH_PKTSIZE + CONFIG_NET_GUARDSIZE)

/* Descriptor stride.
 *
 * A descriptor is 16 bytes but is placed on its own 64-byte D-cache line.
 * DMA_CHn_CONTROL.DSL counts the words to skip between two descriptors,
 * where a word is one AXI bus beat.  The RK3576 GMAC master port is 64 bits
 * wide, so 48 bytes of padding is 6 skip units.
 */

#define RK3576_GMAC_AXI_BUS_BYTES 8
#define RK3576_GMAC_DESC_STRIDE   64
#define RK3576_GMAC_DESC_DSL                                            \
  ((RK3576_GMAC_DESC_STRIDE - (int)sizeof(struct rk3576_gmac_desc_s)) / \
   RK3576_GMAC_AXI_BUS_BYTES)

/* Timeouts */

#define RK3576_GMAC_RESET_TIMEOUT 100000 /* DMA software reset spins       */
#define RK3576_GMAC_MDIO_TIMEOUT  100000 /* MDIO busy spins                */
#define RK3576_GMAC_PHY_RESET_MS  20     /* Assert time of the PHY reset   */
#define RK3576_GMAC_PHY_SETTLE_MS 100    /* Post-release settle time       */
#define RK3576_GMAC_ANEG_TRIES    200    /* 200 * 10 ms = 2 s              */
#define RK3576_GMAC_ANEG_DELAY_MS 10
#define RK3576_GMAC_LINK_POLL_MS  1000 /* Link watchdog period           */

/* RGMII line rates fed to the PHY for each link speed */

#define RK3576_GMAC_RATE_1000M 125000000
#define RK3576_GMAC_RATE_100M  25000000
#define RK3576_GMAC_RATE_10M   2500000

/* Link speeds */

#define RK3576_GMAC_SPEED_10   10
#define RK3576_GMAC_SPEED_100  100
#define RK3576_GMAC_SPEED_1000 1000

/* CSR clock range boundaries, in Hz */

#define RK3576_GMAC_CSR_20MHZ  20000000
#define RK3576_GMAC_CSR_35MHZ  35000000
#define RK3576_GMAC_CSR_60MHZ  60000000
#define RK3576_GMAC_CSR_100MHZ 100000000
#define RK3576_GMAC_CSR_150MHZ 150000000
#define RK3576_GMAC_CSR_250MHZ 250000000
#define RK3576_GMAC_CSR_300MHZ 300000000
#define RK3576_GMAC_CSR_500MHZ 500000000

/* Name length for the clock lookups: "clk_gmac0_ref_en" plus terminator */

#define RK3576_GMAC_CLKNAME_LEN 24

/* Shorthands for the network device inside the private structure */

#define RK3576_GMAC_DEV(p) (&(p)->dev)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One descriptor padded out to a full D-cache line. */

struct rk3576_gmac_ring_s
{
  struct rk3576_gmac_desc_s desc;
  uint32_t pad[(RK3576_GMAC_DESC_STRIDE - sizeof(struct rk3576_gmac_desc_s)) /
               sizeof(uint32_t)];
};

struct rk3576_gmac_s
{
  struct net_driver_s dev; /* Interface understood by the network stack.
                            * Must be the first member.               */

  uintptr_t base;  /* Controller register base                */
  uint8_t intf;    /* Controller index, 0 or 1                */
  uint8_t phyaddr; /* PHY address on the MDIO bus             */
  int irq;         /* SBD interrupt number                    */

  bool ifup;       /* Interface has been brought up           */
  bool linkup;     /* PHY reports a usable link               */
  bool fullduplex; /* Negotiated duplex                       */
  uint16_t speed;  /* Negotiated speed in Mbps                */

  uint32_t csrclk; /* aclk_mac rate, drives the MDC divider   */

  struct clk_s *aclk; /* AXI clock gate                          */
  struct clk_s *pclk; /* APB clock gate                          */
  struct clk_s *mclk; /* MAC functional clock (rate switched)    */

  struct wdog_s linkwd;   /* Link poll watchdog                      */
  struct work_s irqwork;  /* Deferred interrupt processing           */
  struct work_s pollwork; /* Deferred link polling                   */

  struct rk3576_gmac_ring_s *txring; /* Transmit descriptor ring        */
  struct rk3576_gmac_ring_s *rxring; /* Receive descriptor ring         */
  uint8_t *txbuf; /* NTXDESC packet buffers                  */
  uint8_t *rxbuf; /* NRXDESC packet buffers                  */
  uint8_t *stage; /* Staging buffer handed to the stack      */

  uint16_t txhead;     /* Next transmit descriptor to fill        */
  uint16_t txtail;     /* Next transmit descriptor to reclaim     */
  uint16_t txinflight; /* Descriptors currently owned by the DMA  */
  uint16_t rxhead;     /* Next receive descriptor to inspect      */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register helpers */

static inline uint32_t rk3576_gmac_getreg(struct rk3576_gmac_s *priv,
                                          unsigned int off);
static inline void rk3576_gmac_putreg(struct rk3576_gmac_s *priv,
                                      unsigned int off, uint32_t val);
static inline void rk3576_gmac_modifyreg(struct rk3576_gmac_s *priv,
                                         unsigned int off, uint32_t clrbits,
                                         uint32_t setbits);

/* Clocks and Rockchip glue */

static int rk3576_gmac_clk_init(struct rk3576_gmac_s *priv);
static void rk3576_gmac_grf_init(struct rk3576_gmac_s *priv);
static void rk3576_gmac_set_speed_clk(struct rk3576_gmac_s *priv);

/* MDIO and PHY */

static uint32_t rk3576_gmac_csrdiv(uint32_t csrclk);
static int rk3576_gmac_mdio_wait(struct rk3576_gmac_s *priv);
static int rk3576_gmac_phyread(struct rk3576_gmac_s *priv, uint8_t phyad,
                               uint8_t regad, uint16_t *value);
static int rk3576_gmac_phywrite(struct rk3576_gmac_s *priv, uint8_t phyad,
                                uint8_t regad, uint16_t value);
static int rk3576_gmac_phyfind(struct rk3576_gmac_s *priv);
static int rk3576_gmac_phyinit(struct rk3576_gmac_s *priv);
static int rk3576_gmac_phyread_link(struct rk3576_gmac_s *priv, bool *up,
                                    uint16_t *speed, bool *fullduplex);
static void rk3576_gmac_linkwork(void *arg);
static void rk3576_gmac_linkexpiry(wdparm_t arg);

/* DMA rings */

static int rk3576_gmac_ring_alloc(struct rk3576_gmac_s *priv);
static void rk3576_gmac_ring_free(struct rk3576_gmac_s *priv);
static void rk3576_gmac_ring_reset(struct rk3576_gmac_s *priv);
static inline uint32_t rk3576_gmac_pa(void *va);

/* Hardware bring-up */

static int rk3576_gmac_dma_reset(struct rk3576_gmac_s *priv);
static void rk3576_gmac_hwinit(struct rk3576_gmac_s *priv);
static void rk3576_gmac_setmacaddr(struct rk3576_gmac_s *priv);
static void rk3576_gmac_enable(struct rk3576_gmac_s *priv, bool enable);

/* Data path */

static int rk3576_gmac_transmit(struct rk3576_gmac_s *priv);
static int rk3576_gmac_txpoll(struct net_driver_s *dev);
static void rk3576_gmac_txdone(struct rk3576_gmac_s *priv);
static void rk3576_gmac_receive(struct rk3576_gmac_s *priv);
static void rk3576_gmac_interrupt_work(void *arg);
static int rk3576_gmac_interrupt(int irq, void *context, void *arg);
static void rk3576_gmac_txavail_work(void *arg);

/* NuttX network driver callbacks */

static int rk3576_gmac_ifup(struct net_driver_s *dev);
static int rk3576_gmac_ifdown(struct net_driver_s *dev);
static int rk3576_gmac_txavail(struct net_driver_s *dev);
#ifdef CONFIG_NET_MCASTGROUP
static int rk3576_gmac_addmac(struct net_driver_s *dev, const uint8_t *mac);
static int rk3576_gmac_rmmac(struct net_driver_s *dev, const uint8_t *mac);
#endif
#ifdef CONFIG_NETDEV_IOCTL
static int rk3576_gmac_ioctl(struct net_driver_s *dev, int cmd,
                             unsigned long arg);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_gmac_s g_rk3576_gmac[RK3576_GMAC_NIFACES];
static bool g_rk3576_gmac_inited[RK3576_GMAC_NIFACES];

static const uintptr_t g_rk3576_gmac_base[RK3576_GMAC_NIFACES] = {
  RK3576_GMAC0_ADDR,
  RK3576_GMAC1_ADDR,
};

static const int g_rk3576_gmac_irq[RK3576_GMAC_NIFACES] = {
  RK3576_IRQ_GMAC0_SBD,
  RK3576_IRQ_GMAC1_SBD,
};

/* RGMII transmit delay line values taken from the vendor device tree. */

static const uint8_t g_rk3576_gmac_txdelay[RK3576_GMAC_NIFACES] = {
  RK3576_GMAC0_TX_DELAY,
  RK3576_GMAC1_TX_DELAY,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t rk3576_gmac_getreg(struct rk3576_gmac_s *priv,
                                          unsigned int off)
{
  return getreg32(priv->base + off);
}

static inline void rk3576_gmac_putreg(struct rk3576_gmac_s *priv,
                                      unsigned int off, uint32_t val)
{
  putreg32(val, priv->base + off);
}

static inline void rk3576_gmac_modifyreg(struct rk3576_gmac_s *priv,
                                         unsigned int off, uint32_t clrbits,
                                         uint32_t setbits)
{
  uint32_t regval = getreg32(priv->base + off);

  regval &= ~clrbits;
  regval |= setbits;
  putreg32(regval, priv->base + off);
}

/****************************************************************************
 * Name: rk3576_gmac_pa
 *
 * Description:
 *   Return the 32-bit physical address of a DMA buffer.  The allocator
 *   guarantees that everything it hands out lives below 4 GiB, which is the
 *   only range the dwmac4 descriptor pointers can express.
 *
 ****************************************************************************/

static inline uint32_t rk3576_gmac_pa(void *va)
{
  uintptr_t pa = up_addrenv_va_to_pa(va);

  DEBUGASSERT((uint64_t)pa <= UINT32_MAX);
  return (uint32_t)pa;
}

/****************************************************************************
 * Name: rk3576_gmac_clk_init
 *
 * Description:
 *   Acquire and enable every clock the controller needs.  All clock
 *   framework calls of this driver are concentrated here; the only other
 *   clock operation is the clk_set_rate() done on the handle cached below
 *   when the link speed changes.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_gmac_clk_init(struct rk3576_gmac_s *priv)
{
  char name[RK3576_GMAC_CLKNAME_LEN];
  int ret;

  snprintf(name, sizeof(name), "aclk_gmac%u_en", priv->intf);
  priv->aclk = clk_get(name);
  if (priv->aclk == NULL)
    {
      nerr("ERROR: GMAC%u: no clock %s\n", priv->intf, name);
      return -ENODEV;
    }

  ret = clk_enable(priv->aclk);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%u: cannot enable %s: %d\n", priv->intf, name, ret);
      return ret;
    }

  snprintf(name, sizeof(name), "pclk_gmac%u_en", priv->intf);
  priv->pclk = clk_get(name);
  if (priv->pclk == NULL)
    {
      nerr("ERROR: GMAC%u: no clock %s\n", priv->intf, name);
      return -ENODEV;
    }

  ret = clk_enable(priv->pclk);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%u: cannot enable %s: %d\n", priv->intf, name, ret);
      return ret;
    }

  snprintf(name, sizeof(name), "clk_gmac%u_en", priv->intf);
  priv->mclk = clk_get(name);
  if (priv->mclk == NULL)
    {
      nerr("ERROR: GMAC%u: no clock %s\n", priv->intf, name);
      return -ENODEV;
    }

  ret = clk_enable(priv->mclk);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%u: cannot enable %s: %d\n", priv->intf, name, ret);
      return ret;
    }

  /* The CSR (application) clock of the MAC is aclk_mac.  Read the real rate
   * instead of assuming one: it selects the MDC divider.
   */

  priv->csrclk = clk_get_rate(priv->aclk);
  if (priv->csrclk == 0)
    {
      nerr("ERROR: GMAC%u: aclk rate reads back as zero\n", priv->intf);
      return -EINVAL;
    }

  ninfo("GMAC%u: aclk %" PRIu32 " Hz\n", priv->intf, priv->csrclk);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_grf_init
 *
 * Description:
 *   Put the Rockchip glue into RGMII mode and program the transmit delay
 *   line.  Both ports are "rgmii-rxid", so the receive delay is produced by
 *   the PHY and the receive delay line stays disabled.
 *
 ****************************************************************************/

static void rk3576_gmac_grf_init(struct rk3576_gmac_s *priv)
{
  uintptr_t grf =
      RK3576_SDGMAC_GRF_ADDR + RK3576_GMAC_GRF_CON_OFFSET(priv->intf);
  uintptr_t ioc = RK3576_IOC_ADDR + RK3576_GMAC_IOC_CON_OFFSET(priv->intf);

  putreg32(RK3576_GMAC_GRF_RGMII_MODE, grf);

  putreg32(RK3576_GMAC_IOC_RXDLY_DIS | RK3576_GMAC_IOC_TXDLY_EN |
               RK3576_GMAC_IOC_TXDLY_CFG(g_rk3576_gmac_txdelay[priv->intf]),
           ioc);
}

/****************************************************************************
 * Name: rk3576_gmac_set_speed_clk
 *
 * Description:
 *   Route the RGMII transmit clock for the negotiated speed.  The board
 *   sources the clock from the SoC ("clock_in_out = output"), so both the
 *   glue divider and the MAC functional clock have to follow the link:
 *   125 MHz at 1000 Mbps, 25 MHz at 100 Mbps and 2.5 MHz at 10 Mbps.
 *
 ****************************************************************************/

static void rk3576_gmac_set_speed_clk(struct rk3576_gmac_s *priv)
{
  uintptr_t clkcon = RK3576_SDGMAC_GRF_ADDR + RK3576_GMAC_GRF_CLK_CON_OFFSET;
  unsigned int shift = priv->intf * RK3576_GMAC_GRF_CLK_SHIFT;
  uint32_t rate;
  uint32_t sel;
  uint32_t regval;

  switch (priv->speed)
    {
      case RK3576_GMAC_SPEED_1000:
        sel = RK3576_GMAC_GRF_DIV_RGMII_1;
        rate = RK3576_GMAC_RATE_1000M;
        break;

      case RK3576_GMAC_SPEED_100:
        sel = RK3576_GMAC_GRF_DIV_RGMII_5;
        rate = RK3576_GMAC_RATE_100M;
        break;

      default:
        sel = RK3576_GMAC_GRF_DIV_RGMII_50;
        rate = RK3576_GMAC_RATE_10M;
        break;
    }

  /* Drive the divider selector and keep the clock sourced internally. */

  regval =
      RK3576_GMAC_HIWORD_FIELD(sel, 3, RK3576_GMAC_GRF_CLK_DIV_LO + shift) |
      RK3576_GMAC_HIWORD_CLRBIT(RK3576_GMAC_GRF_CLK_SEL_IO + shift) |
      RK3576_GMAC_HIWORD_CLRBIT(RK3576_GMAC_GRF_CLK_RMII_GATE + shift);

  putreg32(regval, clkcon);

  /* Follow with the CRU so the divider input matches.  A clock tree that
   * does not expose a settable MAC clock simply refuses the request, which
   * is not fatal: the glue divider alone already produces a usable clock.
   */

  if (priv->mclk != NULL)
    {
      int ret = clk_set_rate(priv->mclk, rate);
      if (ret < 0)
        {
          nwarn("WARNING: GMAC%u: clk_set_rate(%" PRIu32 ") failed: %d\n",
                priv->intf, rate, ret);
        }
    }
}

/****************************************************************************
 * Name: rk3576_gmac_csrdiv
 *
 * Description:
 *   Map the CSR clock rate onto the MDC divider encoding that keeps MDC
 *   below the 2.5 MHz clause-22 limit.
 *
 ****************************************************************************/

static uint32_t rk3576_gmac_csrdiv(uint32_t csrclk)
{
  if (csrclk < RK3576_GMAC_CSR_35MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV16;
    }
  else if (csrclk < RK3576_GMAC_CSR_60MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV26;
    }
  else if (csrclk < RK3576_GMAC_CSR_100MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV42;
    }
  else if (csrclk < RK3576_GMAC_CSR_150MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV62;
    }
  else if (csrclk < RK3576_GMAC_CSR_250MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV102;
    }
  else if (csrclk < RK3576_GMAC_CSR_300MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV124;
    }
  else if (csrclk < RK3576_GMAC_CSR_500MHZ)
    {
      return RK3576_GMAC_MDIO_CR_DIV204;
    }

  return RK3576_GMAC_MDIO_CR_DIV324;
}

/****************************************************************************
 * Name: rk3576_gmac_mdio_wait
 *
 * Description:
 *   Spin until the management interface clears its busy bit.
 *
 ****************************************************************************/

static int rk3576_gmac_mdio_wait(struct rk3576_gmac_s *priv)
{
  int i;

  for (i = 0; i < RK3576_GMAC_MDIO_TIMEOUT; i++)
    {
      if ((rk3576_gmac_getreg(priv, RK3576_GMAC_MAC_MDIO_ADDR) &
           RK3576_GMAC_MDIO_GB) == 0)
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_gmac_phyread
 ****************************************************************************/

static int rk3576_gmac_phyread(struct rk3576_gmac_s *priv, uint8_t phyad,
                               uint8_t regad, uint16_t *value)
{
  uint32_t regval;
  int ret;

  ret = rk3576_gmac_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  regval = ((uint32_t)phyad << RK3576_GMAC_MDIO_PA_SHIFT) &
           RK3576_GMAC_MDIO_PA_MASK;
  regval |= ((uint32_t)regad << RK3576_GMAC_MDIO_RDA_SHIFT) &
            RK3576_GMAC_MDIO_RDA_MASK;
  regval |= (rk3576_gmac_csrdiv(priv->csrclk) << RK3576_GMAC_MDIO_CR_SHIFT) &
            RK3576_GMAC_MDIO_CR_MASK;
  regval |= RK3576_GMAC_MDIO_GOC_RD | RK3576_GMAC_MDIO_GB;

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_MDIO_ADDR, regval);

  ret = rk3576_gmac_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  *value = (uint16_t)(rk3576_gmac_getreg(priv, RK3576_GMAC_MAC_MDIO_DATA) &
                      RK3576_GMAC_MDIO_DATA_MASK);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_phywrite
 ****************************************************************************/

static int rk3576_gmac_phywrite(struct rk3576_gmac_s *priv, uint8_t phyad,
                                uint8_t regad, uint16_t value)
{
  uint32_t regval;
  int ret;

  ret = rk3576_gmac_mdio_wait(priv);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_MDIO_DATA, value);

  regval = ((uint32_t)phyad << RK3576_GMAC_MDIO_PA_SHIFT) &
           RK3576_GMAC_MDIO_PA_MASK;
  regval |= ((uint32_t)regad << RK3576_GMAC_MDIO_RDA_SHIFT) &
            RK3576_GMAC_MDIO_RDA_MASK;
  regval |= (rk3576_gmac_csrdiv(priv->csrclk) << RK3576_GMAC_MDIO_CR_SHIFT) &
            RK3576_GMAC_MDIO_CR_MASK;
  regval |= RK3576_GMAC_MDIO_GOC_WR | RK3576_GMAC_MDIO_GB;

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_MDIO_ADDR, regval);

  return rk3576_gmac_mdio_wait(priv);
}

/****************************************************************************
 * Name: rk3576_gmac_phyfind
 *
 * Description:
 *   Confirm the configured PHY address responds, and otherwise scan the bus
 *   for the first device with a sane identifier.  The vendor device tree
 *   places both PHYs at address 0.
 *
 ****************************************************************************/

static int rk3576_gmac_phyfind(struct rk3576_gmac_s *priv)
{
  uint16_t id1;
  uint8_t addr;
  int ret;

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_PHYID1, &id1);
  if (ret == OK && id1 != 0x0000 && id1 != 0xffff)
    {
      return OK;
    }

  for (addr = 0; addr < 32; addr++)
    {
      ret = rk3576_gmac_phyread(priv, addr, RK3576_GMAC_MII_PHYID1, &id1);
      if (ret == OK && id1 != 0x0000 && id1 != 0xffff)
        {
          ninfo("GMAC%u: PHY found at address %u (id1 0x%04x)\n", priv->intf,
                addr, id1);
          priv->phyaddr = addr;
          return OK;
        }
    }

  nerr("ERROR: GMAC%u: no PHY responds on the MDIO bus\n", priv->intf);
  return -ENODEV;
}

/****************************************************************************
 * Name: rk3576_gmac_phyinit
 *
 * Description:
 *   Reset the PHY, advertise every speed the RGMII link supports and start
 *   auto-negotiation.  Completion is not awaited here; the link watchdog
 *   picks the result up.
 *
 ****************************************************************************/

static int rk3576_gmac_phyinit(struct rk3576_gmac_s *priv)
{
  uint16_t bmcr;
  int ret;
  int i;

  /* Board specific hardware reset first, then the standard soft reset. */

  rk3576_gmac_board_phy_reset(priv->intf);

  ret = rk3576_gmac_phyfind(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_phywrite(priv, priv->phyaddr, RK3576_GMAC_MII_BMCR,
                             RK3576_GMAC_BMCR_RESET);
  if (ret < 0)
    {
      return ret;
    }

  for (i = 0; i < RK3576_GMAC_ANEG_TRIES; i++)
    {
      ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_BMCR,
                                &bmcr);
      if (ret < 0)
        {
          return ret;
        }

      if ((bmcr & RK3576_GMAC_BMCR_RESET) == 0)
        {
          break;
        }

      nxsig_usleep(RK3576_GMAC_ANEG_DELAY_MS * 1000);
    }

  if (i >= RK3576_GMAC_ANEG_TRIES)
    {
      nerr("ERROR: GMAC%u: PHY reset never completes\n", priv->intf);
      return -ETIMEDOUT;
    }

  /* Advertise 10/100 both duplexes plus symmetric pause ... */

  ret = rk3576_gmac_phywrite(
      priv, priv->phyaddr, RK3576_GMAC_MII_ADVERTISE,
      RK3576_GMAC_ADV_SELECT_802_3 | RK3576_GMAC_ADV_10HALF |
          RK3576_GMAC_ADV_10FULL | RK3576_GMAC_ADV_100HALF |
          RK3576_GMAC_ADV_100FULL | RK3576_GMAC_ADV_PAUSE);
  if (ret < 0)
    {
      return ret;
    }

  /* ... and 1000BASE-T full duplex. */

  ret = rk3576_gmac_phywrite(priv, priv->phyaddr, RK3576_GMAC_MII_CTRL1000,
                             RK3576_GMAC_CTRL1000_1000FULL);
  if (ret < 0)
    {
      return ret;
    }

  return rk3576_gmac_phywrite(priv, priv->phyaddr, RK3576_GMAC_MII_BMCR,
                              RK3576_GMAC_BMCR_ANENABLE |
                                  RK3576_GMAC_BMCR_ANRESTART);
}

/****************************************************************************
 * Name: rk3576_gmac_phyread_link
 *
 * Description:
 *   Read the current link state out of the PHY.  BMSR latches link loss, so
 *   it is read twice to obtain the live value.
 *
 ****************************************************************************/

static int rk3576_gmac_phyread_link(struct rk3576_gmac_s *priv, bool *up,
                                    uint16_t *speed, bool *fullduplex)
{
  uint16_t bmsr;
  uint16_t lpa;
  uint16_t adv;
  uint16_t stat1000;
  uint16_t ctrl1000;
  int ret;

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_BMSR, &bmsr);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_BMSR, &bmsr);
  if (ret < 0)
    {
      return ret;
    }

  if ((bmsr & RK3576_GMAC_BMSR_LSTATUS) == 0 ||
      (bmsr & RK3576_GMAC_BMSR_ANEGDONE) == 0)
    {
      *up = false;
      return OK;
    }

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_STAT1000,
                            &stat1000);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_CTRL1000,
                            &ctrl1000);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_LPA, &lpa);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_phyread(priv, priv->phyaddr, RK3576_GMAC_MII_ADVERTISE,
                            &adv);
  if (ret < 0)
    {
      return ret;
    }

  *up = true;

  /* Resolve the highest common denominator, gigabit first. */

  if ((ctrl1000 & RK3576_GMAC_CTRL1000_1000FULL) != 0 &&
      (stat1000 & RK3576_GMAC_STAT1000_LP1000FULL) != 0)
    {
      *speed = RK3576_GMAC_SPEED_1000;
      *fullduplex = true;
    }
  else if ((ctrl1000 & RK3576_GMAC_CTRL1000_1000HALF) != 0 &&
           (stat1000 & RK3576_GMAC_STAT1000_LP1000HALF) != 0)
    {
      *speed = RK3576_GMAC_SPEED_1000;
      *fullduplex = false;
    }
  else if ((adv & lpa & RK3576_GMAC_ADV_100FULL) != 0)
    {
      *speed = RK3576_GMAC_SPEED_100;
      *fullduplex = true;
    }
  else if ((adv & lpa & RK3576_GMAC_ADV_100HALF) != 0)
    {
      *speed = RK3576_GMAC_SPEED_100;
      *fullduplex = false;
    }
  else if ((adv & lpa & RK3576_GMAC_ADV_10FULL) != 0)
    {
      *speed = RK3576_GMAC_SPEED_10;
      *fullduplex = true;
    }
  else
    {
      *speed = RK3576_GMAC_SPEED_10;
      *fullduplex = false;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_linkwork
 *
 * Description:
 *   Watchdog worker: track the PHY link and reprogram the MAC whenever the
 *   negotiated parameters change.
 *
 ****************************************************************************/

static void rk3576_gmac_linkwork(void *arg)
{
  struct rk3576_gmac_s *priv = arg;
  uint16_t speed = priv->speed;
  bool fullduplex = priv->fullduplex;
  bool up = false;
  uint32_t regval;

  net_lock();

  if (!priv->ifup)
    {
      net_unlock();
      return;
    }

  if (rk3576_gmac_phyread_link(priv, &up, &speed, &fullduplex) < 0)
    {
      up = false;
    }

  if (up != priv->linkup || speed != priv->speed ||
      fullduplex != priv->fullduplex)
    {
      priv->linkup = up;
      priv->speed = speed;
      priv->fullduplex = fullduplex;

      if (up)
        {
          rk3576_gmac_set_speed_clk(priv);

          regval = rk3576_gmac_getreg(priv, RK3576_GMAC_MAC_CONFIG);
          regval &=
              ~(RK3576_GMAC_CFG_PS | RK3576_GMAC_CFG_FES | RK3576_GMAC_CFG_DM);

          if (speed != RK3576_GMAC_SPEED_1000)
            {
              regval |= RK3576_GMAC_CFG_PS;
            }

          if (speed == RK3576_GMAC_SPEED_100)
            {
              regval |= RK3576_GMAC_CFG_FES;
            }

          if (fullduplex)
            {
              regval |= RK3576_GMAC_CFG_DM;
            }

          rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_CONFIG, regval);

          ninfo("GMAC%u: link up, %u Mbps %s duplex\n", priv->intf, speed,
                fullduplex ? "full" : "half");
          netdev_carrier_on(RK3576_GMAC_DEV(priv));
        }
      else
        {
          ninfo("GMAC%u: link down\n", priv->intf);
          netdev_carrier_off(RK3576_GMAC_DEV(priv));
        }
    }

  net_unlock();

  wd_start(&priv->linkwd, MSEC2TICK(RK3576_GMAC_LINK_POLL_MS),
           rk3576_gmac_linkexpiry, (wdparm_t)priv);
}

/****************************************************************************
 * Name: rk3576_gmac_linkexpiry
 ****************************************************************************/

static void rk3576_gmac_linkexpiry(wdparm_t arg)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)arg;

  work_queue(RK3576_GMAC_WORK, &priv->pollwork, rk3576_gmac_linkwork, priv, 0);
}

/****************************************************************************
 * Name: rk3576_gmac_ring_alloc
 *
 * Description:
 *   Allocate descriptor rings and packet buffers from the DMA heap, which
 *   guarantees 64-byte alignment and a sub-4 GiB physical address.
 *
 ****************************************************************************/

static int rk3576_gmac_ring_alloc(struct rk3576_gmac_s *priv)
{
  priv->txring = rk3576_dma_alloc(RK3576_GMAC_NTXDESC *
                                  sizeof(struct rk3576_gmac_ring_s));
  priv->rxring = rk3576_dma_alloc(RK3576_GMAC_NRXDESC *
                                  sizeof(struct rk3576_gmac_ring_s));
  priv->txbuf = rk3576_dma_alloc(RK3576_GMAC_NTXDESC * RK3576_GMAC_BUFSIZE);
  priv->rxbuf = rk3576_dma_alloc(RK3576_GMAC_NRXDESC * RK3576_GMAC_BUFSIZE);
  priv->stage = rk3576_dma_alloc(RK3576_GMAC_BUFSIZE);

  if (priv->txring == NULL || priv->rxring == NULL || priv->txbuf == NULL ||
      priv->rxbuf == NULL || priv->stage == NULL)
    {
      nerr("ERROR: GMAC%u: DMA heap exhausted\n", priv->intf);
      rk3576_gmac_ring_free(priv);
      return -ENOMEM;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_ring_free
 ****************************************************************************/

static void rk3576_gmac_ring_free(struct rk3576_gmac_s *priv)
{
  if (priv->txring != NULL)
    {
      rk3576_dma_free(priv->txring,
                      RK3576_GMAC_NTXDESC * sizeof(struct rk3576_gmac_ring_s));
      priv->txring = NULL;
    }

  if (priv->rxring != NULL)
    {
      rk3576_dma_free(priv->rxring,
                      RK3576_GMAC_NRXDESC * sizeof(struct rk3576_gmac_ring_s));
      priv->rxring = NULL;
    }

  if (priv->txbuf != NULL)
    {
      rk3576_dma_free(priv->txbuf, RK3576_GMAC_NTXDESC * RK3576_GMAC_BUFSIZE);
      priv->txbuf = NULL;
    }

  if (priv->rxbuf != NULL)
    {
      rk3576_dma_free(priv->rxbuf, RK3576_GMAC_NRXDESC * RK3576_GMAC_BUFSIZE);
      priv->rxbuf = NULL;
    }

  if (priv->stage != NULL)
    {
      rk3576_dma_free(priv->stage, RK3576_GMAC_BUFSIZE);
      priv->stage = NULL;
    }
}

/****************************************************************************
 * Name: rk3576_gmac_ring_reset
 *
 * Description:
 *   Return both rings to their idle state: every transmit descriptor free
 *   and owned by software, every receive descriptor armed with its buffer
 *   and owned by the DMA engine.
 *
 ****************************************************************************/

static void rk3576_gmac_ring_reset(struct rk3576_gmac_s *priv)
{
  struct rk3576_gmac_desc_s *desc;
  int i;

  memset(priv->txring, 0,
         RK3576_GMAC_NTXDESC * sizeof(struct rk3576_gmac_ring_s));
  memset(priv->rxring, 0,
         RK3576_GMAC_NRXDESC * sizeof(struct rk3576_gmac_ring_s));

  for (i = 0; i < RK3576_GMAC_NRXDESC; i++)
    {
      desc = &priv->rxring[i].desc;
      desc->des0 = rk3576_gmac_pa(&priv->rxbuf[i * RK3576_GMAC_BUFSIZE]);
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = RK3576_GMAC_RDES3_OWN | RK3576_GMAC_RDES3_BUF1V |
                   RK3576_GMAC_RDES3_IOC;
    }

  priv->txhead = 0;
  priv->txtail = 0;
  priv->txinflight = 0;
  priv->rxhead = 0;

  up_clean_dcache((uintptr_t)priv->txring,
                  (uintptr_t)priv->txring +
                      RK3576_GMAC_NTXDESC * sizeof(struct rk3576_gmac_ring_s));
  up_clean_dcache((uintptr_t)priv->rxring,
                  (uintptr_t)priv->rxring +
                      RK3576_GMAC_NRXDESC * sizeof(struct rk3576_gmac_ring_s));
}

/****************************************************************************
 * Name: rk3576_gmac_dma_reset
 *
 * Description:
 *   Issue the DMA software reset, which also resets the MAC and MTL blocks,
 *   and wait for it to self clear.
 *
 ****************************************************************************/

static int rk3576_gmac_dma_reset(struct rk3576_gmac_s *priv)
{
  int i;

  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_MODE, RK3576_GMAC_DMAMODE_SWR);

  for (i = 0; i < RK3576_GMAC_RESET_TIMEOUT; i++)
    {
      if ((rk3576_gmac_getreg(priv, RK3576_GMAC_DMA_MODE) &
           RK3576_GMAC_DMAMODE_SWR) == 0)
        {
          return OK;
        }
    }

  nerr("ERROR: GMAC%u: DMA reset never completes\n", priv->intf);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_gmac_setmacaddr
 *
 * Description:
 *   Load the station address of the device structure into MAC address
 *   filter slot 0.
 *
 ****************************************************************************/

static void rk3576_gmac_setmacaddr(struct rk3576_gmac_s *priv)
{
  const uint8_t *mac = priv->dev.d_mac.ether.ether_addr_octet;

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_ADDR_HIGH(0),
                     RK3576_GMAC_ADDRHI_AE | ((uint32_t)mac[5] << 8) |
                         (uint32_t)mac[4]);
  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_ADDR_LOW(0),
                     ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16) |
                         ((uint32_t)mac[1] << 8) | (uint32_t)mac[0]);
}

/****************************************************************************
 * Name: rk3576_gmac_hwinit
 *
 * Description:
 *   Program the DMA, MTL and MAC blocks for a single transmit and receive
 *   queue driven by DMA channel 0.  The rings must already be initialised.
 *
 ****************************************************************************/

static void rk3576_gmac_hwinit(struct rk3576_gmac_s *priv)
{
  uint32_t regval;
  uint32_t hwfeat1;
  uint32_t txfifo;
  uint32_t rxfifo;

  /* DMA bus behaviour: mixed bursts with the outstanding request limits the
   * vendor device tree asks for (the register holds "limit - 1").
   */

  regval = RK3576_GMAC_SYSBUS_MB | RK3576_GMAC_SYSBUS_AAL |
           RK3576_GMAC_SYSBUS_BLEN16 | RK3576_GMAC_SYSBUS_BLEN8 |
           RK3576_GMAC_SYSBUS_BLEN4 |
           (((RK3576_GMAC_RD_OSR_LMT - 1) << RK3576_GMAC_SYSBUS_RD_OSR_SHIFT) &
            RK3576_GMAC_SYSBUS_RD_OSR_MASK) |
           (((RK3576_GMAC_WR_OSR_LMT - 1) << RK3576_GMAC_SYSBUS_WR_OSR_SHIFT) &
            RK3576_GMAC_SYSBUS_WR_OSR_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_SYSBUS_MODE, regval);

  /* Channel 0: 8x burst multiplier and the descriptor padding stride. */

  regval = RK3576_GMAC_CHCTRL_PBLX8 |
           (((uint32_t)RK3576_GMAC_DESC_DSL << RK3576_GMAC_CHCTRL_DSL_SHIFT) &
            RK3576_GMAC_CHCTRL_DSL_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_CONTROL, regval);

  /* Descriptor rings.  The upper halves stay zero: the DMA heap never hands
   * out anything above 4 GiB.
   */

  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TXDESC_HI, 0);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TXDESC_LO,
                     rk3576_gmac_pa(priv->txring));
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TXRING_LEN,
                     RK3576_GMAC_NTXDESC - 1);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TXTAIL,
                     rk3576_gmac_pa(priv->txring));

  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RXDESC_HI, 0);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RXDESC_LO,
                     rk3576_gmac_pa(priv->rxring));
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RXRING_LEN,
                     RK3576_GMAC_NRXDESC - 1);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RXTAIL,
                     rk3576_gmac_pa(priv->rxring) +
                         RK3576_GMAC_NRXDESC *
                             sizeof(struct rk3576_gmac_ring_s));

  /* Transmit and receive burst lengths and the receive buffer size. */

  regval = RK3576_GMAC_TXCTRL_OSF |
           (((uint32_t)RK3576_GMAC_DMA_PBL << RK3576_GMAC_TXCTRL_PBL_SHIFT) &
            RK3576_GMAC_TXCTRL_PBL_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL, regval);

  regval = (((uint32_t)RK3576_GMAC_BUFSIZE << RK3576_GMAC_RXCTRL_RBSZ_SHIFT) &
            RK3576_GMAC_RXCTRL_RBSZ_MASK) |
           (((uint32_t)RK3576_GMAC_DMA_PBL << RK3576_GMAC_RXCTRL_PBL_SHIFT) &
            RK3576_GMAC_RXCTRL_PBL_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL, regval);

  /* MTL: store and forward in both directions, with the whole FIFO given to
   * the single queue.  The FIFO sizes are read out of the feature register
   * rather than assumed.
   */

  hwfeat1 = rk3576_gmac_getreg(priv, RK3576_GMAC_MAC_HW_FEATURE1);
  txfifo = (hwfeat1 & RK3576_GMAC_HWF1_TXFIFO_MASK) >>
           RK3576_GMAC_HWF1_TXFIFO_SHIFT;
  rxfifo = (hwfeat1 & RK3576_GMAC_HWF1_RXFIFO_MASK) >>
           RK3576_GMAC_HWF1_RXFIFO_SHIFT;

  /* The queue size fields count 256-byte blocks minus one, while the
   * feature field encodes 128 << n bytes.
   */

  regval = RK3576_GMAC_TXQ_TSF | RK3576_GMAC_TXQ_EN_ENABLED |
           ((((1u << txfifo) / 2) << RK3576_GMAC_TXQ_TQS_SHIFT) &
            RK3576_GMAC_TXQ_TQS_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_MTL_TXQ0_OPMODE,
                     regval | RK3576_GMAC_TXQ_FTQ);

  regval = RK3576_GMAC_RXQ_RSF |
           ((((1u << rxfifo) / 2) << RK3576_GMAC_RXQ_RQS_SHIFT) &
            RK3576_GMAC_RXQ_RQS_MASK);
  rk3576_gmac_putreg(priv, RK3576_GMAC_MTL_RXQ0_OPMODE, regval);

  /* MAC: enable receive queue 0 for DCB traffic, strip the FCS, insert the
   * transmit checksums and verify the received ones.
   */

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_RXQ_CTRL0, RK3576_GMAC_RXQ_EN_DCB);

  regval = RK3576_GMAC_CFG_JD | RK3576_GMAC_CFG_DCRS | RK3576_GMAC_CFG_ACS |
           RK3576_GMAC_CFG_CST | RK3576_GMAC_CFG_IPC | RK3576_GMAC_CFG_DM;
  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_CONFIG, regval);

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_PKT_FILTER, 0);
  rk3576_gmac_setmacaddr(priv);

  /* Mask the MAC level interrupts; the driver only uses the DMA ones. */

  rk3576_gmac_putreg(priv, RK3576_GMAC_MAC_INT_ENABLE, 0);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_STATUS, RK3576_GMAC_DMAINT_ALL);
}

/****************************************************************************
 * Name: rk3576_gmac_enable
 *
 * Description:
 *   Start or stop the transmit and receive engines together with the DMA
 *   channel interrupts.
 *
 ****************************************************************************/

static void rk3576_gmac_enable(struct rk3576_gmac_s *priv, bool enable)
{
  if (enable)
    {
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL, 0,
                            RK3576_GMAC_TXCTRL_ST);
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL, 0,
                            RK3576_GMAC_RXCTRL_SR);
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_MAC_CONFIG, 0,
                            RK3576_GMAC_CFG_TE | RK3576_GMAC_CFG_RE);

      rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_INT_ENABLE,
                         RK3576_GMAC_DMAINT_NIS | RK3576_GMAC_DMAINT_AIS |
                             RK3576_GMAC_DMAINT_TI | RK3576_GMAC_DMAINT_RI |
                             RK3576_GMAC_DMAINT_RBU | RK3576_GMAC_DMAINT_FBE);
    }
  else
    {
      rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_INT_ENABLE, 0);
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_MAC_CONFIG,
                            RK3576_GMAC_CFG_TE | RK3576_GMAC_CFG_RE, 0);
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_DMA_CH0_RX_CONTROL,
                            RK3576_GMAC_RXCTRL_SR, 0);
      rk3576_gmac_modifyreg(priv, RK3576_GMAC_DMA_CH0_TX_CONTROL,
                            RK3576_GMAC_TXCTRL_ST, 0);
    }
}

/****************************************************************************
 * Name: rk3576_gmac_transmit
 *
 * Description:
 *   Hand the frame currently held in d_buf to the transmit ring.  The frame
 *   is copied into the descriptor's own buffer so that the caller's buffer
 *   (which may be a receive buffer being reused for a reply) can be
 *   released immediately.
 *
 * Returned Value:
 *   OK on success, -EBUSY when the ring is full.
 *
 ****************************************************************************/

static int rk3576_gmac_transmit(struct rk3576_gmac_s *priv)
{
  struct rk3576_gmac_desc_s *desc;
  uint8_t *buf;
  unsigned int len = priv->dev.d_len;

  if (priv->txinflight >= RK3576_GMAC_NTXDESC)
    {
      return -EBUSY;
    }

  if (len == 0 || len > RK3576_GMAC_BUFSIZE)
    {
      nerr("ERROR: GMAC%u: bad transmit length %u\n", priv->intf, len);
      return -EINVAL;
    }

  buf = &priv->txbuf[priv->txhead * RK3576_GMAC_BUFSIZE];
  memcpy(buf, priv->dev.d_buf, len);
  up_clean_dcache((uintptr_t)buf, (uintptr_t)buf + len);

  desc = &priv->txring[priv->txhead].desc;
  desc->des0 = rk3576_gmac_pa(buf);
  desc->des1 = 0;
  desc->des2 =
      ((uint32_t)len & RK3576_GMAC_TDES2_B1L_MASK) | RK3576_GMAC_TDES2_IOC;

  /* The ownership bit must be published last. */

  desc->des3 = RK3576_GMAC_TDES3_OWN | RK3576_GMAC_TDES3_FD |
               RK3576_GMAC_TDES3_LD | RK3576_GMAC_TDES3_CIC_FULL |
               ((uint32_t)len & RK3576_GMAC_TDES3_FL_MASK);

  up_clean_dcache((uintptr_t)desc,
                  (uintptr_t)desc + sizeof(struct rk3576_gmac_ring_s));
  UP_DSB();

  priv->txhead = (priv->txhead + 1) & RK3576_GMAC_TXMASK;
  priv->txinflight++;

  /* Poke the tail pointer one slot past the descriptor just filled. */

  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_TXTAIL,
                     rk3576_gmac_pa(&priv->txring[priv->txhead]));

  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_txpoll
 *
 * Description:
 *   devif_poll() callback: send whatever the stack has produced.
 *
 ****************************************************************************/

static int rk3576_gmac_txpoll(struct net_driver_s *dev)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;

  if (dev->d_len == 0)
    {
      return 0;
    }

  if (!priv->linkup)
    {
      /* Drop silently: the stack retransmits once the carrier is back. */

      dev->d_len = 0;
      return 0;
    }

  if (rk3576_gmac_transmit(priv) < 0)
    {
      return -EBUSY;
    }

  return 0;
}

/****************************************************************************
 * Name: rk3576_gmac_txdone
 *
 * Description:
 *   Reclaim every transmit descriptor the DMA engine has finished with and
 *   give the stack a chance to queue more.
 *
 ****************************************************************************/

static void rk3576_gmac_txdone(struct rk3576_gmac_s *priv)
{
  struct rk3576_gmac_desc_s *desc;

  while (priv->txinflight > 0)
    {
      desc = &priv->txring[priv->txtail].desc;

      up_invalidate_dcache((uintptr_t)desc,
                           (uintptr_t)desc +
                               sizeof(struct rk3576_gmac_ring_s));

      if ((desc->des3 & RK3576_GMAC_TDES3_OWN) != 0)
        {
          break;
        }

      if ((desc->des3 & RK3576_GMAC_TDES3_ES) != 0)
        {
          nwarn("WARNING: GMAC%u: transmit error, TDES3 0x%08" PRIx32 "\n",
                priv->intf, desc->des3);
          NETDEV_TXERRORS(RK3576_GMAC_DEV(priv));
        }
      else
        {
          NETDEV_TXDONE(RK3576_GMAC_DEV(priv));
        }

      priv->txtail = (priv->txtail + 1) & RK3576_GMAC_TXMASK;
      priv->txinflight--;
    }

  /* Poll for new work with the staging buffer attached. */

  priv->dev.d_buf = priv->stage;
  priv->dev.d_len = 0;
  devif_poll(RK3576_GMAC_DEV(priv), rk3576_gmac_txpoll);
}

/****************************************************************************
 * Name: rk3576_gmac_receive
 *
 * Description:
 *   Drain the receive ring, dispatch each good frame to the network stack
 *   and re-arm the descriptor.
 *
 ****************************************************************************/

static void rk3576_gmac_receive(struct rk3576_gmac_s *priv)
{
  struct net_driver_s *dev = RK3576_GMAC_DEV(priv);
  struct rk3576_gmac_desc_s *desc;
  uint8_t *buf;
  uint32_t des3;
  unsigned int len;

  for (;;)
    {
      desc = &priv->rxring[priv->rxhead].desc;
      up_invalidate_dcache((uintptr_t)desc,
                           (uintptr_t)desc +
                               sizeof(struct rk3576_gmac_ring_s));

      des3 = desc->des3;
      if ((des3 & RK3576_GMAC_RDES3_OWN) != 0)
        {
          break;
        }

      buf = &priv->rxbuf[priv->rxhead * RK3576_GMAC_BUFSIZE];
      len = des3 & RK3576_GMAC_RDES3_PL_MASK;

      NETDEV_RXPACKETS(dev);

      if ((des3 & RK3576_GMAC_RDES3_ES) != 0 ||
          (des3 & (RK3576_GMAC_RDES3_FD | RK3576_GMAC_RDES3_LD)) !=
              (RK3576_GMAC_RDES3_FD | RK3576_GMAC_RDES3_LD) ||
          len == 0 || len > RK3576_GMAC_BUFSIZE)
        {
          nwarn("WARNING: GMAC%u: dropping frame, RDES3 0x%08" PRIx32 "\n",
                priv->intf, des3);
          NETDEV_RXERRORS(dev);
        }
      else
        {
          up_invalidate_dcache((uintptr_t)buf, (uintptr_t)buf + len);

          dev->d_buf = buf;
          dev->d_len = len;

#ifdef CONFIG_NET_PKT
          pkt_input(dev);
#endif
          switch (ntohs(((struct eth_hdr_s *)buf)->type))
            {
#ifdef CONFIG_NET_IPv4
              case ETHTYPE_IP:
                NETDEV_RXIPV4(dev);
                ipv4_input(dev);
                break;
#endif
#ifdef CONFIG_NET_IPv6
              case ETHTYPE_IP6:
                NETDEV_RXIPV6(dev);
                ipv6_input(dev);
                break;
#endif
#ifdef CONFIG_NET_ARP
              case ETHTYPE_ARP:
                NETDEV_RXARP(dev);
                arp_input(dev);
                break;
#endif
              default:
                NETDEV_RXDROPPED(dev);
                dev->d_len = 0;
                break;
            }

          /* The upper layer may have produced a reply in d_buf. */

          if (dev->d_len > 0)
            {
              rk3576_gmac_transmit(priv);
            }
        }

      /* Re-arm the descriptor and hand it back to the DMA engine. */

      desc->des0 = rk3576_gmac_pa(buf);
      desc->des1 = 0;
      desc->des2 = 0;
      desc->des3 = RK3576_GMAC_RDES3_OWN | RK3576_GMAC_RDES3_BUF1V |
                   RK3576_GMAC_RDES3_IOC;

      up_clean_dcache((uintptr_t)desc,
                      (uintptr_t)desc + sizeof(struct rk3576_gmac_ring_s));
      UP_DSB();

      rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_RXTAIL,
                         rk3576_gmac_pa(&priv->rxring[priv->rxhead]));

      priv->rxhead = (priv->rxhead + 1) & RK3576_GMAC_RXMASK;
    }

  dev->d_buf = priv->stage;
  dev->d_len = 0;
}

/****************************************************************************
 * Name: rk3576_gmac_interrupt_work
 ****************************************************************************/

static void rk3576_gmac_interrupt_work(void *arg)
{
  struct rk3576_gmac_s *priv = arg;
  uint32_t status;

  net_lock();

  status = rk3576_gmac_getreg(priv, RK3576_GMAC_DMA_CH0_STATUS);
  rk3576_gmac_putreg(priv, RK3576_GMAC_DMA_CH0_STATUS, status);

  if ((status & RK3576_GMAC_DMAINT_FBE) != 0)
    {
      nerr("ERROR: GMAC%u: fatal bus error, status 0x%08" PRIx32 "\n",
           priv->intf, status);
      NETDEV_ERRORS(RK3576_GMAC_DEV(priv));
    }

  if ((status & (RK3576_GMAC_DMAINT_RI | RK3576_GMAC_DMAINT_RBU)) != 0)
    {
      rk3576_gmac_receive(priv);
    }

  if ((status & (RK3576_GMAC_DMAINT_TI | RK3576_GMAC_DMAINT_TBU)) != 0)
    {
      rk3576_gmac_txdone(priv);
    }

  net_unlock();

  up_enable_irq(priv->irq);
}

/****************************************************************************
 * Name: rk3576_gmac_interrupt
 ****************************************************************************/

static int rk3576_gmac_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_gmac_s *priv = arg;

  /* Defer everything: the descriptor rings may only be touched with the
   * network locked.
   */

  up_disable_irq(priv->irq);
  work_queue(RK3576_GMAC_WORK, &priv->irqwork, rk3576_gmac_interrupt_work,
             priv, 0);

  UNUSED(irq);
  UNUSED(context);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_txavail_work
 ****************************************************************************/

static void rk3576_gmac_txavail_work(void *arg)
{
  struct rk3576_gmac_s *priv = arg;

  net_lock();

  if (priv->ifup && priv->txinflight < RK3576_GMAC_NTXDESC)
    {
      priv->dev.d_buf = priv->stage;
      priv->dev.d_len = 0;
      devif_poll(RK3576_GMAC_DEV(priv), rk3576_gmac_txpoll);
    }

  net_unlock();
}

/****************************************************************************
 * Name: rk3576_gmac_ifup
 ****************************************************************************/

static int rk3576_gmac_ifup(struct net_driver_s *dev)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;
  int ret;

  if (priv->ifup)
    {
      return OK;
    }

  rk3576_gmac_grf_init(priv);

  ret = rk3576_gmac_dma_reset(priv);
  if (ret < 0)
    {
      return ret;
    }

  rk3576_gmac_ring_reset(priv);
  rk3576_gmac_hwinit(priv);

  /* Start at gigabit until auto-negotiation says otherwise. */

  priv->speed = RK3576_GMAC_SPEED_1000;
  priv->fullduplex = true;
  priv->linkup = false;
  rk3576_gmac_set_speed_clk(priv);

  ret = rk3576_gmac_phyinit(priv);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%u: PHY initialisation failed: %d\n", priv->intf, ret);
      return ret;
    }

  dev->d_buf = priv->stage;
  dev->d_len = 0;

  rk3576_gmac_enable(priv, true);

  priv->ifup = true;
  up_enable_irq(priv->irq);

  wd_start(&priv->linkwd, MSEC2TICK(RK3576_GMAC_LINK_POLL_MS),
           rk3576_gmac_linkexpiry, (wdparm_t)priv);

  ninfo("GMAC%u: %s up\n", priv->intf, dev->d_ifname);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_ifdown
 ****************************************************************************/

static int rk3576_gmac_ifdown(struct net_driver_s *dev)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;
  irqstate_t flags;

  if (!priv->ifup)
    {
      return OK;
    }

  flags = enter_critical_section();

  up_disable_irq(priv->irq);
  wd_cancel(&priv->linkwd);

  rk3576_gmac_enable(priv, false);
  rk3576_gmac_dma_reset(priv);

  priv->ifup = false;
  priv->linkup = false;

  leave_critical_section(flags);

  netdev_carrier_off(dev);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_txavail
 ****************************************************************************/

static int rk3576_gmac_txavail(struct net_driver_s *dev)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;

  if (work_available(&priv->pollwork))
    {
      work_queue(RK3576_GMAC_WORK, &priv->pollwork, rk3576_gmac_txavail_work,
                 priv, 0);
    }

  return OK;
}

#ifdef CONFIG_NET_MCASTGROUP
/****************************************************************************
 * Name: rk3576_gmac_addmac
 *
 * Description:
 *   Accept an additional multicast address.  Only one perfect filter slot is
 *   reserved for the station address, so multicast reception is handled by
 *   passing every multicast frame up.  That is a superset of what was asked
 *   for, which the stack filters again in software.
 *
 ****************************************************************************/

static int rk3576_gmac_addmac(struct net_driver_s *dev, const uint8_t *mac)
{
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;

  UNUSED(mac);
  rk3576_gmac_modifyreg(priv, RK3576_GMAC_MAC_PKT_FILTER, 0,
                        RK3576_GMAC_FILTER_PM);
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_rmmac
 *
 * Description:
 *   Stop accepting one multicast address.  Because addmac() opens the filter
 *   to all multicast traffic, and the driver does not track how many groups
 *   are still joined, the filter is left open here.
 *
 ****************************************************************************/

static int rk3576_gmac_rmmac(struct net_driver_s *dev, const uint8_t *mac)
{
  UNUSED(dev);
  UNUSED(mac);
  return OK;
}
#endif /* CONFIG_NET_MCASTGROUP */

#ifdef CONFIG_NETDEV_IOCTL
/****************************************************************************
 * Name: rk3576_gmac_ioctl
 ****************************************************************************/

static int rk3576_gmac_ioctl(struct net_driver_s *dev, int cmd,
                             unsigned long arg)
{
#ifdef CONFIG_NETDEV_PHY_IOCTL
  struct rk3576_gmac_s *priv = (struct rk3576_gmac_s *)dev;

  switch (cmd)
    {
      case SIOCGMIIPHY:
        {
          struct mii_ioctl_data_s *req =
              (struct mii_ioctl_data_s *)(uintptr_t)arg;

          req->phy_id = priv->phyaddr;
          return OK;
        }

      case SIOCGMIIREG:
        {
          struct mii_ioctl_data_s *req =
              (struct mii_ioctl_data_s *)(uintptr_t)arg;

          return rk3576_gmac_phyread(priv, req->phy_id, req->reg_num,
                                     &req->val_out);
        }

      case SIOCSMIIREG:
        {
          struct mii_ioctl_data_s *req =
              (struct mii_ioctl_data_s *)(uintptr_t)arg;

          return rk3576_gmac_phywrite(priv, req->phy_id, req->reg_num,
                                      req->val_in);
        }

      default:
        break;
    }
#else
  UNUSED(dev);
  UNUSED(cmd);
  UNUSED(arg);
#endif

  return -ENOTTY;
}
#endif /* CONFIG_NETDEV_IOCTL */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_gmac_board_phy_reset
 *
 * Description:
 *   Weak default: boards without a PHY reset GPIO need no implementation.
 *
 ****************************************************************************/

void weak_function rk3576_gmac_board_phy_reset(int intf) { UNUSED(intf); }

/****************************************************************************
 * Name: rk3576_gmac_get_macaddr
 *
 * Description:
 *   Weak default: derive the station address from the factory fuses so that
 *   it is stable across reboots, and fall back to a fixed locally
 *   administered address when no OTP driver is configured.
 *
 ****************************************************************************/

int weak_function rk3576_gmac_get_macaddr(int intf, uint8_t *mac)
{
#ifdef CONFIG_RK3576_OTP
  int ret = rk3576_otp_get_mac((uint8_t)intf, mac);
  if (ret >= 0)
    {
      return OK;
    }

  nwarn("WARNING: GMAC%d: OTP address unavailable (%d), using fallback\n",
        intf, ret);
#endif

  /* Locally administered, unicast. */

  mac[0] = 0x02;
  mac[1] = 0x35;
  mac[2] = 0x76;
  mac[3] = 0x00;
  mac[4] = 0x00;
  mac[5] = (uint8_t)intf;
  return OK;
}

/****************************************************************************
 * Name: rk3576_gmac_mdio_read
 ****************************************************************************/

int rk3576_gmac_mdio_read(int intf, uint8_t phyad, uint8_t regad,
                          uint16_t *value)
{
  if (intf < 0 || intf >= RK3576_GMAC_NIFACES || value == NULL ||
      !g_rk3576_gmac_inited[intf])
    {
      return -EINVAL;
    }

  return rk3576_gmac_phyread(&g_rk3576_gmac[intf], phyad, regad, value);
}

/****************************************************************************
 * Name: rk3576_gmac_mdio_write
 ****************************************************************************/

int rk3576_gmac_mdio_write(int intf, uint8_t phyad, uint8_t regad,
                           uint16_t value)
{
  if (intf < 0 || intf >= RK3576_GMAC_NIFACES || !g_rk3576_gmac_inited[intf])
    {
      return -EINVAL;
    }

  return rk3576_gmac_phywrite(&g_rk3576_gmac[intf], phyad, regad, value);
}

/****************************************************************************
 * Name: rk3576_gmac_initialize
 ****************************************************************************/

int rk3576_gmac_initialize(int intf)
{
  struct rk3576_gmac_s *priv;
  int ret;

  if (intf < 0 || intf >= RK3576_GMAC_NIFACES)
    {
      nerr("ERROR: invalid GMAC index %d\n", intf);
      return -EINVAL;
    }

  if (g_rk3576_gmac_inited[intf])
    {
      return OK;
    }

  priv = &g_rk3576_gmac[intf];
  memset(priv, 0, sizeof(*priv));

  priv->intf = (uint8_t)intf;
  priv->base = g_rk3576_gmac_base[intf];
  priv->irq = g_rk3576_gmac_irq[intf];
  priv->phyaddr = CONFIG_RK3576_GMAC_PHYADDR;

  ret = rk3576_gmac_clk_init(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_gmac_ring_alloc(priv);
  if (ret < 0)
    {
      return ret;
    }

  priv->dev.d_ifup = rk3576_gmac_ifup;
  priv->dev.d_ifdown = rk3576_gmac_ifdown;
  priv->dev.d_txavail = rk3576_gmac_txavail;
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_addmac = rk3576_gmac_addmac;
  priv->dev.d_rmmac = rk3576_gmac_rmmac;
#endif
#ifdef CONFIG_NETDEV_IOCTL
  priv->dev.d_ioctl = rk3576_gmac_ioctl;
#endif
  priv->dev.d_private = priv;
  priv->dev.d_buf = priv->stage;

  ret = rk3576_gmac_get_macaddr(intf, priv->dev.d_mac.ether.ether_addr_octet);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%d: no station address: %d\n", intf, ret);
      rk3576_gmac_ring_free(priv);
      return ret;
    }

  /* Hold the controller in reset until ifup() runs. */

  rk3576_gmac_grf_init(priv);
  ret = rk3576_gmac_dma_reset(priv);
  if (ret < 0)
    {
      rk3576_gmac_ring_free(priv);
      return ret;
    }

  ret = irq_attach(priv->irq, rk3576_gmac_interrupt, priv);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%d: cannot attach IRQ %d: %d\n", intf, priv->irq, ret);
      rk3576_gmac_ring_free(priv);
      return ret;
    }

  up_disable_irq(priv->irq);

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      nerr("ERROR: GMAC%d: netdev_register failed: %d\n", intf, ret);
      irq_detach(priv->irq);
      rk3576_gmac_ring_free(priv);
      return ret;
    }

  g_rk3576_gmac_inited[intf] = true;

  ninfo("GMAC%d: registered at 0x%08lx, IRQ %d\n", intf,
        (unsigned long)priv->base, priv->irq);
  return OK;
}

#endif /* CONFIG_RK3576_GMAC */
