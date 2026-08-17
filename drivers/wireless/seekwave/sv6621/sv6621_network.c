/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_network.c
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

#ifdef CONFIG_NET

#include <nuttx/net/ethernet.h>
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>

#ifdef CONFIG_NET_PKT
#include <nuttx/net/pkt.h>
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#include "sv6621_network.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_network_rx_worker(FAR void *arg);
static void sv6621_network_tx_worker(FAR void *arg);
static void sv6621_network_reply(FAR struct sv6621_network_s *network);
static int sv6621_network_transmit(FAR struct sv6621_network_s *network);
static int sv6621_network_tx_poll(FAR struct net_driver_s *dev);
static int sv6621_network_ifup(FAR struct net_driver_s *dev);
static int sv6621_network_ifdown(FAR struct net_driver_s *dev);
static int sv6621_network_txavail(FAR struct net_driver_s *dev);
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int sv6621_network_addmac(FAR struct net_driver_s *dev,
                                 FAR const uint8_t *mac);
#ifdef CONFIG_NET_MCASTGROUP
static int sv6621_network_rmmac(FAR struct net_driver_s *dev,
                                FAR const uint8_t *mac);
#endif
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_network_reply(FAR struct sv6621_network_s *network)
{
  if (network->dev.d_len > 0)
    {
      sv6621_network_transmit(network);
    }
}

static int sv6621_network_transmit(FAR struct sv6621_network_s *network)
{
  int ret;

  if (!network->link_up)
    {
      return -ENETDOWN;
    }

  ret = sv6621_data_send(network->data, &network->tx_context,
                          network->dev.d_buf, network->dev.d_len);
  if (ret < 0)
    {
      NETDEV_TXERRORS(&network->dev);
      return ret;
    }

  NETDEV_TXPACKETS(&network->dev);
  return 0;
}

static int sv6621_network_tx_poll(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_network_s *network = dev->d_private;

  return sv6621_network_transmit(network);
}

static void sv6621_network_tx_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;

  net_lock();
  if (network->interface_up && network->link_up)
    {
      network->dev.d_buf = network->tx_frame;
      devif_poll(&network->dev, sv6621_network_tx_poll);
    }

  net_unlock();
}

static void sv6621_network_rx_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  FAR struct eth_hdr_s *ethernet;
  irqstate_t flags;
  uint8_t tail;

  net_lock();
  for (;;)
    {
      flags = spin_lock_irqsave(&network->lock);
      if (network->rx_tail == network->rx_head)
        {
          spin_unlock_irqrestore(&network->lock, flags);
          break;
        }

      tail = network->rx_tail;
      network->rx_tail = (tail + 1) % SV6621_NETWORK_RX_DEPTH;
      spin_unlock_irqrestore(&network->lock, flags);

      if (!network->interface_up)
        {
          continue;
        }

      network->dev.d_buf = network->rx_frame[tail];
      network->dev.d_len = network->rx_length[tail];
      ethernet = (FAR struct eth_hdr_s *)network->dev.d_buf;

#ifdef CONFIG_NET_PKT
      pkt_input(&network->dev);
#endif
#ifdef CONFIG_NET_IPv4
      if (ethernet->type == HTONS(ETHTYPE_IP))
        {
          NETDEV_RXIPV4(&network->dev);
          ipv4_input(&network->dev);
          sv6621_network_reply(network);
          continue;
        }
#endif
#ifdef CONFIG_NET_IPv6
      if (ethernet->type == HTONS(ETHTYPE_IP6))
        {
          NETDEV_RXIPV6(&network->dev);
          ipv6_input(&network->dev);
          sv6621_network_reply(network);
          continue;
        }
#endif
#ifdef CONFIG_NET_ARP
      if (ethernet->type == HTONS(ETHTYPE_ARP))
        {
          NETDEV_RXARP(&network->dev);
          arp_input(&network->dev);
          sv6621_network_reply(network);
          continue;
        }
#endif
      NETDEV_RXDROPPED(&network->dev);
    }

  network->dev.d_buf = network->tx_frame;
  net_unlock();
}

static int sv6621_network_ifup(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_network_s *network = dev->d_private;
  irqstate_t flags;

  flags = spin_lock_irqsave(&network->lock);
  network->interface_up = true;
  spin_unlock_irqrestore(&network->lock, flags);
  return 0;
}

static int sv6621_network_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_network_s *network = dev->d_private;
  irqstate_t flags;

  flags = spin_lock_irqsave(&network->lock);
  network->interface_up = false;
  network->rx_tail = network->rx_head;
  spin_unlock_irqrestore(&network->lock, flags);
  return 0;
}

static int sv6621_network_txavail(FAR struct net_driver_s *dev)
{
  FAR struct sv6621_network_s *network = dev->d_private;

  if (work_available(&network->tx_work))
    {
      work_queue(LPWORK, &network->tx_work, sv6621_network_tx_worker, network,
                 0);
    }

  return 0;
}

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int sv6621_network_addmac(FAR struct net_driver_s *dev,
                                 FAR const uint8_t *mac)
{
  return 0;
}

#ifdef CONFIG_NET_MCASTGROUP
static int sv6621_network_rmmac(FAR struct net_driver_s *dev,
                                FAR const uint8_t *mac)
{
  return 0;
}
#endif
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_network_init(FAR struct sv6621_network_s *network,
                        FAR struct sv6621_data_s *data,
                        FAR const uint8_t mac[SV6621_MAC_LENGTH])
{
  int ret;

  if (network == NULL || data == NULL || mac == NULL)
    {
      return -EINVAL;
    }

  memset(network, 0, sizeof(*network));
  network->data = data;
  network->dev.d_ifup = sv6621_network_ifup;
  network->dev.d_ifdown = sv6621_network_ifdown;
  network->dev.d_txavail = sv6621_network_txavail;
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
  network->dev.d_addmac = sv6621_network_addmac;
#ifdef CONFIG_NET_MCASTGROUP
  network->dev.d_rmmac = sv6621_network_rmmac;
#endif
#endif
  network->dev.d_private = network;
  network->dev.d_buf = network->tx_frame;
  memcpy(network->dev.d_mac.ether.ether_addr_octet, mac, SV6621_MAC_LENGTH);

  ret = netdev_register(&network->dev, NET_LL_ETHERNET);
  if (ret >= 0)
    {
      network->registered = true;
      netdev_carrier_off(&network->dev);
    }

  return ret;
}

void sv6621_network_deinit(FAR struct sv6621_network_s *network)
{
  if (network != NULL && network->registered)
    {
      work_cancel(LPWORK, &network->rx_work);
      work_cancel(LPWORK, &network->tx_work);
      netdev_unregister(&network->dev);
      network->registered = false;
    }
}

void sv6621_network_set_link(
    FAR struct sv6621_network_s *network, bool link_up,
    FAR const struct sv6621_data_tx_context_s *context)
{
  irqstate_t flags;

  if (network == NULL || !network->registered ||
      (link_up && context == NULL))
    {
      return;
    }

  flags = spin_lock_irqsave(&network->lock);
  if (context != NULL)
    {
      network->tx_context = *context;
    }

  network->link_up = link_up;
  spin_unlock_irqrestore(&network->lock, flags);

  if (link_up)
    {
      netdev_carrier_on(&network->dev);
    }
  else
    {
      netdev_carrier_off(&network->dev);
    }
}

void sv6621_network_input(FAR const struct sv6621_data_rx_s *rx,
                          FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  irqstate_t flags;
  uint8_t next;

  if (network == NULL || rx == NULL ||
      rx->frame_length > MAX_NETDEV_PKTSIZE)
    {
      return;
    }

  flags = spin_lock_irqsave(&network->lock);
  next = (network->rx_head + 1) % SV6621_NETWORK_RX_DEPTH;
  if (!network->registered || next == network->rx_tail)
    {
      spin_unlock_irqrestore(&network->lock, flags);
      if (network->registered)
        {
          NETDEV_RXDROPPED(&network->dev);
        }

      return;
    }

  memcpy(network->rx_frame[network->rx_head], rx->frame, rx->frame_length);
  network->rx_length[network->rx_head] = rx->frame_length;
  network->rx_head = next;
  spin_unlock_irqrestore(&network->lock, flags);

  if (work_available(&network->rx_work))
    {
      work_queue(LPWORK, &network->rx_work, sv6621_network_rx_worker, network,
                 0);
    }
}

#endif /* CONFIG_NET */
