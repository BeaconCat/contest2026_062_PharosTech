/****************************************************************************
 * apps/examples/wifitest/wifitest_main.c
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

#include <nuttx/config.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef CONFIG_NETUTILS_DHCPC
#include <arpa/inet.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#endif

struct rk3576_skw_bss_s
{
  uint8_t  bssid[6];
  uint8_t  ssid[33];
  uint8_t  ssid_len;
  uint8_t  channel;
  int16_t  rssi;
};

int kickpi_k7_wifi_initialize(void);
uint32_t rk3576_skw_state(void);
int rk3576_skw_scan(struct rk3576_skw_bss_s *list, int max);
int rk3576_skw_connect(const char *ssid);
int rk3576_skw_wpa_connect(const char *ssid, const char *psk);
void rk3576_skw_get_mac(uint8_t mac[6]);

int main(int argc, FAR char *argv[])
{
  static struct rk3576_skw_bss_s bss[32];
  int ret = kickpi_k7_wifi_initialize();
  unsigned st = (unsigned)rk3576_skw_state();
  int n;
  int i;

  printf("wifitest: init=%d state=0x%x\n", ret, st);
  if (!(st & 0x2))
    {
      return 1;
    }

  /* "wifitest <ssid> <psk>" runs the full WPA2-PSK 4-way handshake;
   * "wifitest <ssid>" does open/L2 association only; no arg just scans.
   */

  if (argc >= 3)
    {
      ret = rk3576_skw_wpa_connect(argv[1], argv[2]);
      printf("wifitest: wpa2 connect \"%s\" -> %d %s\n", argv[1], ret,
             ret == 0 ? "CONNECTED (keys installed)" : "failed");
#ifdef CONFIG_NETUTILS_DHCPC
      if (ret == 0)
        {
          uint8_t mac[6];
          void *handle;
          struct dhcpc_state ds;
          int ur;

          rk3576_skw_get_mac(mac);
          ur = netlib_ifup("eth0");
          printf("wifitest: ifup ret=%d\n", ur);
          handle = dhcpc_open("eth0", mac, 6);
          printf("wifitest: dhcpc_open handle=%p\n", handle);
          ur = -1;
          if (handle)
            {
              int try;

              for (try = 0; try < 5 && ur != OK; try++)
                {
                  ur = dhcpc_request(handle, &ds);
                  printf("wifitest: dhcpc_request try %d ret=%d\n",
                         try, ur);
                  if (ur != OK)
                    {
                      sleep(1);
                    }
                }
            }
          if (handle != NULL && ur == OK)
            {
              netlib_set_ipv4addr("eth0", &ds.ipaddr);
              if (ds.netmask.s_addr != 0)
                {
                  netlib_set_ipv4netmask("eth0", &ds.netmask);
                }

              if (ds.default_router.s_addr != 0)
                {
                  netlib_set_dripv4addr("eth0", &ds.default_router);
                }

              printf("wifitest: dhcp IP %s\n", inet_ntoa(ds.ipaddr));
            }
          else
            {
              printf("wifitest: dhcp failed\n");
            }

          if (handle != NULL)
            {
              dhcpc_close(handle);
            }
        }
#endif
      return ret == 0 ? 0 : 1;
    }

  if (argc >= 2)
    {
      ret = rk3576_skw_connect(argv[1]);
      printf("wifitest: connect \"%s\" -> %d %s\n", argv[1], ret,
             ret == 0 ? "ASSOCIATED" : "failed");
      return ret == 0 ? 0 : 1;
    }

  n = rk3576_skw_scan(bss, 32);
  printf("wifitest: scan found %d AP(s)\n", n);

  for (i = 0; i < n; i++)
    {
      char ssid[34];
      int l = bss[i].ssid_len > 32 ? 32 : bss[i].ssid_len;

      memcpy(ssid, bss[i].ssid, l);
      ssid[l] = 0;
      printf("  [%2d] ch%-3d %4ddBm %02x:%02x:%02x:%02x:%02x:%02x  \"%s\"\n",
             i, bss[i].channel, bss[i].rssi,
             bss[i].bssid[0], bss[i].bssid[1], bss[i].bssid[2],
             bss[i].bssid[3], bss[i].bssid[4], bss[i].bssid[5], ssid);
    }

  return 0;
}
