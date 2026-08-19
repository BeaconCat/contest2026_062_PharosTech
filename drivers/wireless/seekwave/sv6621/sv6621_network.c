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
static int sv6621_network_queue_tx(FAR struct sv6621_network_s *network);
static bool sv6621_network_addresses_equal(
    FAR const struct sv6621_offload_addresses_s *left,
    FAR const struct sv6621_offload_addresses_s *right);
static void sv6621_network_collect_addresses(
    FAR struct sv6621_network_s *network,
    FAR struct sv6621_offload_addresses_s *addresses);
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
 * Name: sv6621_network_addresses_equal
 ****************************************************************************/

static bool sv6621_network_addresses_equal(
    FAR const struct sv6621_offload_addresses_s *left,
    FAR const struct sv6621_offload_addresses_s *right)
{
  if (left->ipv4_valid != right->ipv4_valid ||
      left->ipv6_count != right->ipv6_count)
    {
      return false;
    }

  if (left->ipv4_valid &&
      memcmp(left->ipv4, right->ipv4, sizeof(left->ipv4)) != 0)
    {
      return false;
    }

  return memcmp(left->ipv6, right->ipv6,
                left->ipv6_count * SV6621_OFFLOAD_IPV6_LENGTH) == 0;
}

/****************************************************************************
 * Name: sv6621_network_collect_addresses
 ****************************************************************************/

static void sv6621_network_collect_addresses(
    FAR struct sv6621_network_s *network,
    FAR struct sv6621_offload_addresses_s *addresses)
{
#ifdef CONFIG_NET_IPv6
  static const uint8_t zero[SV6621_OFFLOAD_IPV6_LENGTH];
  size_t index;
#endif

  memset(addresses, 0, sizeof(*addresses));
  net_lock();
#ifdef CONFIG_NET_IPv4
  if (network->dev.d_ipaddr != 0)
    {
      memcpy(addresses->ipv4, &network->dev.d_ipaddr,
             SV6621_OFFLOAD_IPV4_LENGTH);
      addresses->ipv4_valid = true;
    }
#endif
#ifdef CONFIG_NET_IPv6
  for (index = 0;
       index < CONFIG_NETDEV_MAX_IPv6_ADDR &&
       addresses->ipv6_count < SV6621_OFFLOAD_IPV6_LIMIT;
       index++)
    {
      FAR const uint8_t *address =
          (FAR const uint8_t *)network->dev.d_ipv6[index].addr;

      if (memcmp(address, zero, sizeof(zero)) != 0)
        {
          memcpy(addresses->ipv6[addresses->ipv6_count], address,
                 SV6621_OFFLOAD_IPV6_LENGTH);
          addresses->ipv6_count++;
        }
    }

  if (addresses->ipv6_count == 0)
    {
      FAR const uint8_t *mac =
          network->dev.d_mac.ether.ether_addr_octet;
      FAR uint8_t *link_local = addresses->ipv6[0];

      link_local[0] = 0xfe;
      link_local[1] = 0x80;
      link_local[8] = mac[0] ^ 0x02;
      link_local[9] = mac[1];
      link_local[10] = mac[2];
      link_local[11] = 0xff;
      link_local[12] = 0xfe;
      link_local[13] = mac[3];
      link_local[14] = mac[4];
      link_local[15] = mac[5];
      addresses->ipv6_count = 1;
    }
#endif
  net_unlock();
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

/****************************************************************************
 * Name: sv6621_network_queue_tx
 ****************************************************************************/

static int sv6621_network_queue_tx(FAR struct sv6621_network_s *network)
{
  irqstate_t flags;
  bool schedule = false;
  int ret;

  flags = spin_lock_irqsave(&network->lock);
  if (network->registered && network->interface_up && network->link_up)
    {
      if (!network->tx_scheduled)
        {
          network->tx_scheduled = true;
          schedule = true;
        }
      else
        {
          network->tx_reschedule = true;
        }
    }

  spin_unlock_irqrestore(&network->lock, flags);
  if (!schedule)
    {
      return 0;
    }

  ret = work_queue(LPWORK, &network->tx_work, sv6621_network_tx_worker,
                   network, 0);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&network->lock);
      network->tx_scheduled = false;
      network->tx_reschedule = false;
      spin_unlock_irqrestore(&network->lock, flags);
    }

  return ret;
}

static void sv6621_network_tx_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  irqstate_t flags;
  bool retry;

  do
    {
      flags = spin_lock_irqsave(&network->lock);
      network->tx_reschedule = false;
      spin_unlock_irqrestore(&network->lock, flags);

      (void)sv6621_network_sync_addresses(network);

      net_lock();
      if (sv6621_network_tx_snapshot(network, NULL))
        {
          network->dev.d_buf = network->tx_frame;
          devif_poll(&network->dev, sv6621_network_tx_poll);
        }

      net_unlock();

      flags = spin_lock_irqsave(&network->lock);
      retry = network->tx_reschedule && network->registered &&
              network->interface_up && network->link_up;
      if (!retry)
        {
          network->tx_scheduled = false;
        }

      spin_unlock_irqrestore(&network->lock, flags);
    }
  while (retry);
}

#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
/****************************************************************************
 * Name: sv6621_network_multicast_worker
 ****************************************************************************/

static void sv6621_network_multicast_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  irqstate_t flags;
  bool retry;
  int ret;

  do
    {
      ret = sv6621_network_sync_multicast(network);

      flags = spin_lock_irqsave(&network->lock);
      retry = ret == 0 && network->multicast_applied_generation !=
                            network->multicast_generation;
      if (!retry)
        {
          network->multicast_scheduled = false;
        }

      spin_unlock_irqrestore(&network->lock, flags);
    }
  while (retry);
}

/****************************************************************************
 * Name: sv6621_network_queue_multicast
 ****************************************************************************/

static int sv6621_network_queue_multicast(
    FAR struct sv6621_network_s *network)
{
  irqstate_t flags;
  bool schedule = false;
  int ret;

  flags = spin_lock_irqsave(&network->lock);
  if (!network->multicast_scheduled &&
      network->multicast_applied_generation !=
          network->multicast_generation)
    {
      network->multicast_scheduled = true;
      schedule = true;
    }

  spin_unlock_irqrestore(&network->lock, flags);
  if (!schedule)
    {
      return 0;
    }

  ret = work_queue(LPWORK, &network->multicast_work,
                   sv6621_network_multicast_worker, network, 0);
  if (ret < 0)
    {
      flags = spin_lock_irqsave(&network->lock);
      network->multicast_scheduled = false;
      spin_unlock_irqrestore(&network->lock, flags);
    }

  return ret;
}
#endif

static void sv6621_network_rx_worker(FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  FAR struct eth_hdr_s *ethernet;
  irqstate_t flags;
  bool ready;
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
      ready = network->interface_up && network->link_up;
      spin_unlock_irqrestore(&network->lock, flags);

      if (!ready)
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

  sv6621_network_queue_tx(network);
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
          return sv6621_network_queue_multicast(network);
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
      network->multicast_generation++;
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

          network->multicast_generation++;
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
  network->multicast_generation = 1;
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
  irqstate_t flags;
  bool registered;

  if (network == NULL)
    {
      return;
    }

  flags = spin_lock_irqsave(&network->lock);
  registered = network->registered;
  network->registered = false;
  network->interface_up = false;
  network->link_up = false;
  spin_unlock_irqrestore(&network->lock, flags);
  if (!registered)
    {
      return;
    }

  work_cancel_sync(LPWORK, &network->rx_work);
  work_cancel_sync(LPWORK, &network->tx_work);
  work_cancel_sync(LPWORK, &network->multicast_work);
  netdev_carrier_off(&network->dev);
  netdev_unregister(&network->dev);
  sv6621_ioctl_deinit(&network->ioctl);
}

/****************************************************************************
 * Name: sv6621_network_sync_multicast
 ****************************************************************************/

int sv6621_network_sync_multicast(FAR struct sv6621_network_s *network)
{
  uint8_t addresses[SV6621_NETWORK_MULTICAST_CAPACITY][SV6621_MAC_LENGTH];
  irqstate_t flags;
  uint32_t generation;
  uint8_t count;
  int ret;

  if (network == NULL || network->command == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&network->lock);
  count = network->multicast_count;
  generation = network->multicast_generation;
  memcpy(addresses, network->multicast, count * SV6621_MAC_LENGTH);
  spin_unlock_irqrestore(&network->lock, flags);

  ret = sv6621_filter_set_multicast(network->command, addresses, count);
  if (ret == 0)
    {
      flags = spin_lock_irqsave(&network->lock);
      if (generation == network->multicast_generation)
        {
          network->multicast_applied_generation = generation;
        }

      spin_unlock_irqrestore(&network->lock, flags);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_network_sync_link_addresses
 ****************************************************************************/

int sv6621_network_sync_link_addresses(
    FAR struct sv6621_network_s *network,
    FAR const struct sv6621_data_tx_context_s *context)
{
  struct sv6621_offload_addresses_s addresses;
  irqstate_t flags;
  uint32_t epoch;
  bool already_applied;
  bool has_addresses;
  int ret;

  if (network == NULL || network->command == NULL || !network->registered ||
      context == NULL)
    {
      return -EINVAL;
    }

  sv6621_network_collect_addresses(network, &addresses);
  flags = spin_lock_irqsave(&network->lock);
  epoch = network->address_epoch;
  already_applied = network->addresses_applied &&
                    network->applied_address_instance == context->instance &&
                    sv6621_network_addresses_equal(
                        &network->applied_addresses, &addresses);
  spin_unlock_irqrestore(&network->lock, flags);
  if (already_applied)
    {
      return 0;
    }

  has_addresses = addresses.ipv4_valid || addresses.ipv6_count != 0;
  ret = has_addresses ?
        sv6621_offload_set_addresses(network->command, context->instance,
                                     &addresses) : 0;
  if (ret == 0)
    {
      flags = spin_lock_irqsave(&network->lock);
      if (epoch == network->address_epoch)
        {
          network->applied_addresses = addresses;
          network->applied_address_instance = context->instance;
          network->addresses_applied = true;
        }

      spin_unlock_irqrestore(&network->lock, flags);
    }

  return ret;
}

/****************************************************************************
 * Name: sv6621_network_sync_addresses
 ****************************************************************************/

int sv6621_network_sync_addresses(FAR struct sv6621_network_s *network)
{
  struct sv6621_data_tx_context_s context;

  if (network == NULL || network->command == NULL || !network->registered)
    {
      return -EINVAL;
    }

  if (!sv6621_network_tx_snapshot(network, &context))
    {
      return -ENETDOWN;
    }

  return sv6621_network_sync_link_addresses(network, &context);
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
  if (!link_up)
    {
      network->rx_tail = network->rx_head;
      network->addresses_applied = false;
      network->address_epoch++;
    }

  spin_unlock_irqrestore(&network->lock, flags);

  if (link_up)
    {
      netdev_carrier_on(&network->dev);
      sv6621_network_queue_tx(network);
#if defined(CONFIG_NET_MCASTGROUP) || defined(CONFIG_NET_ICMPv6)
      sv6621_network_queue_multicast(network);
#endif
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
  if (network != NULL)
    {
      sv6621_network_queue_tx(network);
    }
}

void sv6621_network_input(FAR const struct sv6621_data_rx_s *rx,
                          FAR void *arg)
{
  FAR struct sv6621_network_s *network = arg;
  irqstate_t flags;
  bool count_drop;
  bool schedule = false;
  uint8_t next;
  int ret;

  if (network == NULL || rx == NULL ||
      rx->frame_length > MAX_NETDEV_PKTSIZE)
    {
      return;
    }

  flags = spin_lock_irqsave(&network->lock);
  count_drop = network->registered;
  next = (network->rx_head + 1) % SV6621_NETWORK_RX_DEPTH;
  if (!network->registered || !network->interface_up || !network->link_up ||
      next == network->rx_tail)
    {
      spin_unlock_irqrestore(&network->lock, flags);
      if (count_drop)
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
