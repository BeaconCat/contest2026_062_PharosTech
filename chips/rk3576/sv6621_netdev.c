/****************************************************************************
 * chips/rk3576/sv6621_netdev.c
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

#if defined(CONFIG_NET) && defined(CONFIG_SV6621)

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>

#ifdef CONFIG_NET_PKT
#include <nuttx/net/pkt.h>
#endif

#include "sv6621.h"
#include "sv6621_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_SCHED_LPWORK)
#error "CONFIG_SCHED_LPWORK is required by the SV6621 netdev"
#endif

#define SKW_NETWORK       LPWORK

#define SKW_NET_RXQ_DEPTH 8
#define SKW_NET_BUFSIZE   MAX_NETDEV_PKTSIZE

#define SKW_ETHBUF        ((FAR struct eth_hdr_s *)g_sv6621_net.dev.d_buf)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sv6621_net_s
{
  struct net_driver_s dev; /* Interface understood by the net
                            * stack (must be first) */
  bool bifup;              /* true when the interface is up */

  struct work_s rxwork;   /* RX deferral to SKW_NETWORK */
  struct work_s pollwork; /* TX poll deferral to SKW_NETWORK */

  /* RX staging ring, filled by sv6621_net_input() from the rx thread */

  uint8_t rxq[SKW_NET_RXQ_DEPTH][SKW_NET_BUFSIZE];
  uint16_t rxq_len[SKW_NET_RXQ_DEPTH];
  volatile uint8_t rxq_head; /* producer index (rx thread) */
  volatile uint8_t rxq_tail; /* consumer index (LPWORK) */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct sv6621_net_s g_sv6621_net;
static spinlock_t g_sv6621_net_lock = SP_UNLOCKED;

static uint8_t g_sv6621_net_txbuf[SKW_NET_BUFSIZE] aligned_data(4);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_net_transmit(FAR struct sv6621_net_s *priv);
static int sv6621_net_txpoll(FAR struct net_driver_s *dev);
static void sv6621_net_reply(FAR struct sv6621_net_s *priv);
static void sv6621_net_rxwork(FAR void *arg);
static void sv6621_net_txwork(FAR void *arg);

static int sv6621_net_ifup(FAR struct net_driver_s *dev);
static int sv6621_net_ifdown(FAR struct net_driver_s *dev);
static int sv6621_net_txavail(FAR struct net_driver_s *dev);
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int sv6621_net_addmac(FAR struct net_driver_s *dev,
                             FAR const uint8_t *mac);
#ifdef CONFIG_NET_MCASTGROUP
static int sv6621_net_rmmac(FAR struct net_driver_s *dev,
                            FAR const uint8_t *mac);
#endif
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_net_transmit
 ****************************************************************************/

static int sv6621_net_transmit(FAR struct sv6621_net_s *priv)
{
  int ret;

  ret = sv6621_data_tx(priv->dev.d_buf, priv->dev.d_len);
  if (ret < 0)
    {
      nerr("ERROR: skw data tx failed: %d\n", ret);
      NETDEV_TXERRORS(&priv->dev);
      return ret;
    }

  NETDEV_TXPACKETS(&priv->dev);
  return OK;
}

/****************************************************************************
 * Name: sv6621_net_txpoll
 ****************************************************************************/

static int sv6621_net_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)dev->d_private;

  return sv6621_net_transmit(priv);
}

/****************************************************************************
 * Name: sv6621_net_reply
 ****************************************************************************/

static void sv6621_net_reply(FAR struct sv6621_net_s *priv)
{
  if (priv->dev.d_len > 0)
    {
      sv6621_net_transmit(priv);
    }
}

/****************************************************************************
 * Name: sv6621_net_rxwork
 ****************************************************************************/

static void sv6621_net_rxwork(FAR void *arg)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)arg;
  irqstate_t flags;
  uint8_t tail;

  net_lock();

  for (;;)
    {
      flags = spin_lock_irqsave(&g_sv6621_net_lock);
      if (priv->rxq_tail == priv->rxq_head)
        {
          spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
          break;
        }

      tail = priv->rxq_tail;
      spin_unlock_irqrestore(&g_sv6621_net_lock, flags);

      if (!priv->bifup)
        {
          flags = spin_lock_irqsave(&g_sv6621_net_lock);
          priv->rxq_tail = (tail + 1) % SKW_NET_RXQ_DEPTH;
          spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
          continue;
        }

      priv->dev.d_buf = priv->rxq[tail];
      priv->dev.d_len = priv->rxq_len[tail];

#ifdef CONFIG_NET_PKT
      pkt_input(&priv->dev);
#endif

#ifdef CONFIG_NET_IPv4
      if (SKW_ETHBUF->type == HTONS(ETHTYPE_IP))
        {
          ninfo("IPv4 frame\n");
          NETDEV_RXIPV4(&priv->dev);
          ipv4_input(&priv->dev);
          sv6621_net_reply(priv);
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
          if (SKW_ETHBUF->type == HTONS(ETHTYPE_IP6))
        {
          ninfo("IPv6 frame\n");
          NETDEV_RXIPV6(&priv->dev);
          ipv6_input(&priv->dev);
          sv6621_net_reply(priv);
        }
      else
#endif
#ifdef CONFIG_NET_ARP
          if (SKW_ETHBUF->type == HTONS(ETHTYPE_ARP))
        {
          NETDEV_RXARP(&priv->dev);
          arp_input(&priv->dev);
          sv6621_net_reply(priv);
        }
      else
#endif
        {
          NETDEV_RXDROPPED(&priv->dev);
        }

      flags = spin_lock_irqsave(&g_sv6621_net_lock);
      priv->rxq_tail = (tail + 1) % SKW_NET_RXQ_DEPTH;
      spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
    }

  priv->dev.d_buf = g_sv6621_net_txbuf;

  net_unlock();
}

/****************************************************************************
 * Name: sv6621_net_txwork
 ****************************************************************************/

static void sv6621_net_txwork(FAR void *arg)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)arg;

  net_lock();

  if (priv->bifup)
    {
      priv->dev.d_buf = g_sv6621_net_txbuf;
      devif_poll(&priv->dev, sv6621_net_txpoll);
    }

  net_unlock();
}

/****************************************************************************
 * Name: sv6621_net_ifup
 ****************************************************************************/

static int sv6621_net_ifup(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)dev->d_private;
  irqstate_t flags;

  flags = spin_lock_irqsave(&g_sv6621_net_lock);
  priv->bifup = true;
  spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
  netdev_carrier_on(&priv->dev);
  return OK;
}

/****************************************************************************
 * Name: sv6621_net_ifdown
 ****************************************************************************/

static int sv6621_net_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)dev->d_private;
  irqstate_t flags;

  flags = spin_lock_irqsave(&g_sv6621_net_lock);
  priv->bifup = false;
  priv->rxq_tail = priv->rxq_head;
  spin_unlock_irqrestore(&g_sv6621_net_lock, flags);

  return OK;
}

/****************************************************************************
 * Name: sv6621_net_txavail
 ****************************************************************************/

static int sv6621_net_txavail(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_net_s *priv = (FAR struct sv6621_net_s *)dev->d_private;

  if (work_available(&priv->pollwork))
    {
      work_queue(SKW_NETWORK, &priv->pollwork, sv6621_net_txwork, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Name: sv6621_net_addmac / sv6621_net_rmmac
 ****************************************************************************/

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int sv6621_net_addmac(FAR struct net_driver_s *dev,
                             FAR const uint8_t *mac)
{
  return OK;
}

#ifdef CONFIG_NET_MCASTGROUP
static int sv6621_net_rmmac(FAR struct net_driver_s *dev,
                            FAR const uint8_t *mac)
{
  return OK;
}
#endif
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_net_input
 *
 * Description:
 *   Called from the SKW receive thread (sv6621_data_rx) for every non-EAPOL
 *   Ethernet frame.  Stages the frame and defers processing to LPWORK: the
 *   caller holds the SDIO bus and must not run the network stack.
 *
 ****************************************************************************/

void sv6621_net_input(FAR const uint8_t *frame, int len)
{
  irqstate_t flags;
  uint8_t next;

  if (len < ETH_HDRLEN || len > SKW_NET_BUFSIZE)
    {
      return;
    }

  flags = spin_lock_irqsave(&g_sv6621_net_lock);

  if (!g_sv6621_net.bifup)
    {
      spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
      return;
    }

  next = (g_sv6621_net.rxq_head + 1) % SKW_NET_RXQ_DEPTH;
  if (next == g_sv6621_net.rxq_tail)
    {
      spin_unlock_irqrestore(&g_sv6621_net_lock, flags);
      NETDEV_RXDROPPED(&g_sv6621_net.dev);
      return;
    }

  memcpy(g_sv6621_net.rxq[g_sv6621_net.rxq_head], frame, len);
  g_sv6621_net.rxq_len[g_sv6621_net.rxq_head] = (uint16_t)len;
  g_sv6621_net.rxq_head = next;

  spin_unlock_irqrestore(&g_sv6621_net_lock, flags);

  if (work_available(&g_sv6621_net.rxwork))
    {
      work_queue(SKW_NETWORK, &g_sv6621_net.rxwork, sv6621_net_rxwork,
                 &g_sv6621_net, 0);
    }
}

/****************************************************************************
 * Name: sv6621_netdev_register
 *
 * Description:
 *   Allocate and register the SKW WiFi network interface.  Call once after
 *   the driver init has read the STA MAC at GET_INFO.
 *
 ****************************************************************************/

int sv6621_netdev_register(void)
{
  FAR struct sv6621_net_s *priv = &g_sv6621_net;

  memset(priv, 0, sizeof(*priv));

  priv->dev.d_ifup = sv6621_net_ifup;
  priv->dev.d_ifdown = sv6621_net_ifdown;
  priv->dev.d_txavail = sv6621_net_txavail;
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
  priv->dev.d_addmac = sv6621_net_addmac;
#ifdef CONFIG_NET_MCASTGROUP
  priv->dev.d_rmmac = sv6621_net_rmmac;
#endif
#endif
  priv->dev.d_private = priv;
  priv->dev.d_buf = g_sv6621_net_txbuf;

  sv6621_get_mac(priv->dev.d_mac.ether.ether_addr_octet);

  return netdev_register(&priv->dev, NET_LL_ETHERNET);
}

#endif /* CONFIG_NET && CONFIG_SV6621 */
