/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_skw_netdev.c
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

#if defined(CONFIG_NET) && defined(CONFIG_RK3576_SKW)

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <arpa/inet.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/wqueue.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ethernet.h>

#ifdef CONFIG_NET_PKT
#  include <nuttx/net/pkt.h>
#endif

#include "rk3576_skw.h"
#include "rk3576_skw_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#if !defined(CONFIG_SCHED_LPWORK)
#  error "CONFIG_SCHED_LPWORK is required by the RK3576 SKW netdev"
#endif

#define SKW_NETWORK      LPWORK

#define SKW_NET_RXQ_DEPTH   8
#define SKW_NET_BUFSIZE     MAX_NETDEV_PKTSIZE

#define SKW_ETHBUF ((FAR struct eth_hdr_s *)g_skw_net.dev.d_buf)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_skw_net_s
{
  struct net_driver_s dev;             /* Interface understood by the net
                                        * stack (must be first) */
  bool                 bifup;          /* true when the interface is up */

  struct work_s        rxwork;         /* RX deferral to SKW_NETWORK */
  struct work_s        pollwork;       /* TX poll deferral to SKW_NETWORK */

  /* RX staging ring, filled by rk3576_skw_net_input() from the rx thread */

  uint8_t rxq[SKW_NET_RXQ_DEPTH][SKW_NET_BUFSIZE];
  uint16_t rxq_len[SKW_NET_RXQ_DEPTH];
  volatile uint8_t rxq_head;           /* producer index (rx thread) */
  volatile uint8_t rxq_tail;           /* consumer index (LPWORK) */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_skw_net_s g_skw_net;
static spinlock_t g_skw_net_lock = SP_UNLOCKED;

static uint8_t g_skw_net_txbuf[SKW_NET_BUFSIZE]
               aligned_data(4);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  rk3576_skw_net_transmit(FAR struct rk3576_skw_net_s *priv);
static int  rk3576_skw_net_txpoll(FAR struct net_driver_s *dev);
static void rk3576_skw_net_reply(FAR struct rk3576_skw_net_s *priv);
static void rk3576_skw_net_rxwork(FAR void *arg);
static void rk3576_skw_net_txwork(FAR void *arg);

static int  rk3576_skw_net_ifup(FAR struct net_driver_s *dev);
static int  rk3576_skw_net_ifdown(FAR struct net_driver_s *dev);
static int  rk3576_skw_net_txavail(FAR struct net_driver_s *dev);
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int  rk3576_skw_net_addmac(FAR struct net_driver_s *dev,
                                  FAR const uint8_t *mac);
#ifdef CONFIG_NET_MCASTGROUP
static int  rk3576_skw_net_rmmac(FAR struct net_driver_s *dev,
                                 FAR const uint8_t *mac);
#endif
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_skw_net_transmit
 ****************************************************************************/

static int rk3576_skw_net_transmit(FAR struct rk3576_skw_net_s *priv)
{
  int ret;

  ret = rk3576_skw_data_tx(priv->dev.d_buf, priv->dev.d_len);
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
 * Name: rk3576_skw_net_txpoll
 ****************************************************************************/

static int rk3576_skw_net_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct rk3576_skw_net_s *priv =
    (FAR struct rk3576_skw_net_s *)dev->d_private;

  return rk3576_skw_net_transmit(priv);
}

/****************************************************************************
 * Name: rk3576_skw_net_reply
 ****************************************************************************/

static void rk3576_skw_net_reply(FAR struct rk3576_skw_net_s *priv)
{
  if (priv->dev.d_len > 0)
    {
      rk3576_skw_net_transmit(priv);
    }
}

/****************************************************************************
 * Name: rk3576_skw_net_rxwork
 ****************************************************************************/

static void rk3576_skw_net_rxwork(FAR void *arg)
{
  FAR struct rk3576_skw_net_s *priv = (FAR struct rk3576_skw_net_s *)arg;
  irqstate_t flags;
  uint8_t tail;

  net_lock();

  for (; ; )
    {
      flags = spin_lock_irqsave(&g_skw_net_lock);
      if (priv->rxq_tail == priv->rxq_head)
        {
          spin_unlock_irqrestore(&g_skw_net_lock, flags);
          break;
        }

      tail = priv->rxq_tail;
      spin_unlock_irqrestore(&g_skw_net_lock, flags);

      if (!priv->bifup)
        {
          flags = spin_lock_irqsave(&g_skw_net_lock);
          priv->rxq_tail = (tail + 1) % SKW_NET_RXQ_DEPTH;
          spin_unlock_irqrestore(&g_skw_net_lock, flags);
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
          rk3576_skw_net_reply(priv);
        }
      else
#endif
#ifdef CONFIG_NET_IPv6
      if (SKW_ETHBUF->type == HTONS(ETHTYPE_IP6))
        {
          ninfo("IPv6 frame\n");
          NETDEV_RXIPV6(&priv->dev);
          ipv6_input(&priv->dev);
          rk3576_skw_net_reply(priv);
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (SKW_ETHBUF->type == HTONS(ETHTYPE_ARP))
        {
          NETDEV_RXARP(&priv->dev);
          arp_input(&priv->dev);
          rk3576_skw_net_reply(priv);
        }
      else
#endif
        {
          NETDEV_RXDROPPED(&priv->dev);
        }

      flags = spin_lock_irqsave(&g_skw_net_lock);
      priv->rxq_tail = (tail + 1) % SKW_NET_RXQ_DEPTH;
      spin_unlock_irqrestore(&g_skw_net_lock, flags);
    }

  priv->dev.d_buf = g_skw_net_txbuf;

  net_unlock();
}

/****************************************************************************
 * Name: rk3576_skw_net_txwork
 ****************************************************************************/

static void rk3576_skw_net_txwork(FAR void *arg)
{
  FAR struct rk3576_skw_net_s *priv = (FAR struct rk3576_skw_net_s *)arg;

  net_lock();

  if (priv->bifup)
    {
      priv->dev.d_buf = g_skw_net_txbuf;
      devif_poll(&priv->dev, rk3576_skw_net_txpoll);
    }

  net_unlock();
}


#endif /* CONFIG_NET && CONFIG_RK3576_SKW */
