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
#include "sv6621_filter.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_network_rx_worker(FAR void *arg);
static void sv6621_network_tx_worker(FAR void *arg);
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static void sv6621_network_multicast_worker(FAR void *arg);
static int sv6621_network_queue_multicast(
    FAR struct sv6621_network_s *network);
#endif
static void sv6621_network_reply(FAR struct sv6621_network_s *network);
static bool sv6621_network_tx_snapshot(
    FAR struct sv6621_network_s *network,
    FAR struct sv6621_data_tx_context_s *context);
static int sv6621_network_transmit(FAR struct sv6621_network_s *network);
static int sv6621_network_tx_poll(FAR struct net_driver_s *dev);
static int sv6621_network_ifup(FAR struct net_driver_s *dev);
static int sv6621_network_ifdown(FAR struct net_driver_s *dev);
static int sv6621_network_txavail(FAR struct net_driver_s *dev);
#ifdef CONFIG_NETDEV_IOCTL
static int sv6621_network_ioctl(FAR struct net_driver_s *dev, int command,
                                unsigned long argument);
#endif
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

/****************************************************************************
 * Name: sv6621_network_tx_snapshot
 ****************************************************************************/

static bool sv6621_network_tx_snapshot(
    FAR struct sv6621_network_s *network,
    FAR struct sv6621_data_tx_context_s *context)
{
  irqstate_t flags;
  bool ready;

  flags = spin_lock_irqsave(&network->lock);
  ready = network->interface_up && network->link_up;
  if (ready && context != NULL)
    {
      *context = network->tx_context;
    }

  spin_unlock_irqrestore(&network->lock, flags);
  return ready;
}

static int sv6621_network_transmit(FAR struct sv6621_network_s *network)
{
  struct sv6621_data_tx_context_s context;
  int ret;

  if (!sv6621_network_tx_snapshot(network, &context))
    {
      return -ENETDOWN;
    }

  ret = sv6621_data_send(network->data, &context,
                          network->dev.d_buf, network->dev.d_len);
  if (ret < 0)
    {
      if (ret != -EAGAIN)
        {
          NETDEV_TXERRORS(&network->dev);
        }

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
  if (sv6621_network_tx_snapshot(network, NULL))
    {
      network->dev.d_buf = network->tx_frame;
      devif_poll(&network->dev, sv6621_network_tx_poll);
    }

  net_unlock();
}

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
/****************************************************************************
 * Name: sv6621_network_multicast_worker
 ****************************************************************************/

static void sv6621_network_multicast_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;

  sv6621_network_sync_multicast(network);
}

/****************************************************************************
 * Name: sv6621_network_queue_multicast
 ****************************************************************************/

static int sv6621_network_queue_multicast(
    FAR struct sv6621_network_s *network)
{
  if (work_available(&network->multicast_work))
    {
      return work_queue(LPWORK, &network->multicast_work,
                        sv6621_network_multicast_worker, network, 0);
    }

  return 0;
}
#endif

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
          network->rx_scheduled = false;
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

#ifdef CONFIG_NETDEV_IOCTL
/****************************************************************************
 * Name: sv6621_network_ioctl
 ****************************************************************************/

static int sv6621_network_ioctl(FAR struct net_driver_s *dev, int command,
                                unsigned long argument)
{
  FAR struct sv6621_network_s *network = dev->d_private;

  return sv6621_ioctl_handle(&network->ioctl, command, argument);
}
#endif

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
static int sv6621_network_addmac(FAR struct net_driver_s *dev,
                                 FAR const uint8_t *mac)
{
  FAR struct sv6621_network_s *network = dev->d_private;
  irqstate_t flags;
  size_t index;
  int ret = 0;

  if (mac == NULL || (mac[0] & 1) == 0)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&network->lock);
  for (index = 0; index < network->multicast_count; index++)
    {
      if (memcmp(network->multicast[index], mac, SV6621_MAC_LENGTH) == 0)
        {
          spin_unlock_irqrestore(&network->lock, flags);
          return 0;
        }
    }

  if (network->multicast_count >= network->multicast_limit)
    {
      ret = -ENOSPC;
    }
  else
    {
      memcpy(network->multicast[network->multicast_count], mac,
             SV6621_MAC_LENGTH);
      network->multicast_count++;
    }

  spin_unlock_irqrestore(&network->lock, flags);
  return ret < 0 ? ret : sv6621_network_queue_multicast(network);
}

#ifdef CONFIG_NET_MCASTGROUP
static int sv6621_network_rmmac(FAR struct net_driver_s *dev,
                                FAR const uint8_t *mac)
{
  FAR struct sv6621_network_s *network = dev->d_private;
  irqstate_t flags;
  size_t index;

  if (mac == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&network->lock);
  for (index = 1; index < network->multicast_count; index++)
    {
      if (memcmp(network->multicast[index], mac, SV6621_MAC_LENGTH) == 0)
        {
          network->multicast_count--;
          if (index < network->multicast_count)
            {
              memcpy(network->multicast[index],
                     network->multicast[network->multicast_count],
                     SV6621_MAC_LENGTH);
            }

          break;
        }
    }

  spin_unlock_irqrestore(&network->lock, flags);
  return sv6621_network_queue_multicast(network);
}
#endif
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_network_init(FAR struct sv6621_network_s *network,
                        FAR struct sv6621_dev_s *owner,
                        FAR struct sv6621_data_s *data,
                        FAR struct sv6621_command_engine_s *command,
                        uint8_t multicast_limit,
                        FAR const uint8_t mac[SV6621_MAC_LENGTH])
{
  int ret;

  if (network == NULL || owner == NULL || data == NULL || command == NULL ||
      mac == NULL ||
      multicast_limit == 0)
    {
      return -EINVAL;
    }

  memset(network, 0, sizeof(*network));
  network->data = data;
  network->command = command;
  network->multicast_limit =
      multicast_limit > SV6621_NETWORK_MULTICAST_CAPACITY ?
      SV6621_NETWORK_MULTICAST_CAPACITY : multicast_limit;
  network->multicast_count = 1;
  memset(network->multicast[0], 0xff, SV6621_MAC_LENGTH);
  network->dev.d_ifup = sv6621_network_ifup;
  network->dev.d_ifdown = sv6621_network_ifdown;
  network->dev.d_txavail = sv6621_network_txavail;
#ifdef CONFIG_NETDEV_IOCTL
  network->dev.d_ioctl = sv6621_network_ioctl;
#endif
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
  network->dev.d_addmac = sv6621_network_addmac;
#ifdef CONFIG_NET_MCASTGROUP
  network->dev.d_rmmac = sv6621_network_rmmac;
#endif
#endif
  network->dev.d_private = network;
  network->dev.d_buf = network->tx_frame;
  memcpy(network->dev.d_mac.ether.ether_addr_octet, mac, SV6621_MAC_LENGTH);

  ret = sv6621_ioctl_init(&network->ioctl, owner);
  if (ret < 0)
    {
      return ret;
    }

  ret = netdev_register(&network->dev, NET_LL_IEEE80211);
  if (ret >= 0)
    {
      network->registered = true;
      netdev_carrier_off(&network->dev);
    }
  else
    {
      sv6621_ioctl_deinit(&network->ioctl);
    }

  return ret;
}

void sv6621_network_deinit(FAR struct sv6621_network_s *network)
{
  if (network != NULL && network->registered)
    {
      work_cancel(LPWORK, &network->rx_work);
      work_cancel(LPWORK, &network->tx_work);
      work_cancel_sync(LPWORK, &network->multicast_work);
      netdev_unregister(&network->dev);
      sv6621_ioctl_deinit(&network->ioctl);
      network->registered = false;
    }
}

/****************************************************************************
 * Name: sv6621_network_sync_multicast
 ****************************************************************************/

int sv6621_network_sync_multicast(FAR struct sv6621_network_s *network)
{
  uint8_t addresses[SV6621_NETWORK_MULTICAST_CAPACITY][SV6621_MAC_LENGTH];
  irqstate_t flags;
  uint8_t count;

  if (network == NULL || network->command == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&network->lock);
  count = network->multicast_count;
  memcpy(addresses, network->multicast, count * SV6621_MAC_LENGTH);
  spin_unlock_irqrestore(&network->lock, flags);

  return sv6621_filter_set_multicast(network->command, addresses, count);
}

/****************************************************************************
 * Name: sv6621_network_sync_addresses
 ****************************************************************************/

int sv6621_network_sync_addresses(FAR struct sv6621_network_s *network)
{
  struct sv6621_offload_addresses_s addresses;
  struct sv6621_data_tx_context_s context;
#ifdef CONFIG_NET_IPv6
  static const uint8_t zero[SV6621_OFFLOAD_IPV6_LENGTH];
  size_t index;
#endif

  if (network == NULL || network->command == NULL || !network->registered)
    {
      return -EINVAL;
    }

  if (!sv6621_network_tx_snapshot(network, &context))
    {
      return -ENETDOWN;
    }

  memset(&addresses, 0, sizeof(addresses));
  net_lock();
#ifdef CONFIG_NET_IPv4
  if (network->dev.d_ipaddr != 0)
    {
      memcpy(addresses.ipv4, &network->dev.d_ipaddr,
             SV6621_OFFLOAD_IPV4_LENGTH);
      addresses.ipv4_valid = true;
    }
#endif
#ifdef CONFIG_NET_IPv6
  for (index = 0;
       index < CONFIG_NETDEV_MAX_IPv6_ADDR &&
       addresses.ipv6_count < SV6621_OFFLOAD_IPV6_LIMIT;
       index++)
    {
      FAR const uint8_t *address =
          (FAR const uint8_t *)network->dev.d_ipv6[index].addr;

      if (memcmp(address, zero, sizeof(zero)) != 0)
        {
          memcpy(addresses.ipv6[addresses.ipv6_count], address,
                 SV6621_OFFLOAD_IPV6_LENGTH);
          addresses.ipv6_count++;
        }
    }
#endif
  net_unlock();

  if (!addresses.ipv4_valid && addresses.ipv6_count == 0)
    {
      return 0;
    }

  return sv6621_offload_set_addresses(network->command,
                                       context.instance,
                                       &addresses);
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

/****************************************************************************
 * Name: sv6621_network_credit_available
 ****************************************************************************/

void sv6621_network_credit_available(FAR struct sv6621_network_s *network)
{
  if (network != NULL && network->registered && network->interface_up &&
      network->link_up && work_available(&network->tx_work))
    {
      work_queue(LPWORK, &network->tx_work, sv6621_network_tx_worker,
                 network, 0);
    }
}

void sv6621_network_input(FAR const struct sv6621_data_rx_s *rx,
                          FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  irqstate_t flags;
  bool schedule = false;
  uint8_t next;
  int ret;

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
  if (!network->rx_scheduled)
    {
      network->rx_scheduled = true;
      schedule = true;
    }

  spin_unlock_irqrestore(&network->lock, flags);

  if (schedule)
    {
      ret = work_queue(LPWORK, &network->rx_work, sv6621_network_rx_worker,
                       network, 0);
      if (ret < 0)
        {
          flags = spin_lock_irqsave(&network->lock);
          network->rx_scheduled = false;
          spin_unlock_irqrestore(&network->lock, flags);
        }
    }
}

#endif /* CONFIG_NET */
