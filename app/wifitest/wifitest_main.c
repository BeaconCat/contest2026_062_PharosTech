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

  /* "wifitest <ssid>" connects; no arg just scans. */

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
