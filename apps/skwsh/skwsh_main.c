/****************************************************************************
 * apps/external/app/skwsh/skwsh_main.c
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
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied.  See the License for the specific language governing
 * permissions and limitations under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

/* Raw debug primitives exported by the SKW driver. */

int rk3576_skw_dbg_cmd52(int write, unsigned reg, unsigned val,
                         unsigned *out);
int rk3576_skw_dbg_cmd53_read(unsigned char *buf, int len, int *actual);
int rk3576_skw_dbg_dt_read(unsigned char *buf, int len, int *actual);
int rk3576_skw_dbg_rx_eapol(unsigned char *buf, int max);
int rk3576_skw_dbg_data_tx(const unsigned char *eth, int len);
int rk3576_skw_dbg_connect(const char *ssid);
int rk3576_skw_dbg_get_bssid(unsigned char *b);
int rk3576_skw_dbg_get_mac(unsigned char *m);
int rk3576_skw_dbg_cmd53_write(const unsigned char *buf, int len);
int rk3576_skw_dbg_latch(unsigned cpaddr);
int rk3576_skw_dbg_bringup(void);
int rk3576_skw_dbg_cplog(int enable);
int rk3576_skw_dbg_sendcmd(unsigned id, const unsigned char *payload,
                           int plen);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int hex2buf(const char *hex, unsigned char *buf, int max)
{
  int n = 0;

  while (hex[0] != 0 && hex[1] != 0 && n < max)
    {
      char b[3] = { hex[0], hex[1], 0 };

      buf[n++] = (unsigned char)strtoul(b, NULL, 16);
      hex += 2;
    }

  return n;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  static unsigned char buf[4096];
  int i;

  if (argc < 2)
    {
      printf("usage: skwsh r52|w52|r53|w53|latch|mem|br|cmd ...\n");
      return 1;
    }

  if (strcmp(argv[1], "r52") == 0 && argc >= 3)
    {
      unsigned reg = strtoul(argv[2], NULL, 0);
      unsigned v = 0;
      int ret = rk3576_skw_dbg_cmd52(0, reg, 0, &v);

      printf("r52 %03x = %02x (%d)\n", reg, v, ret);
    }
  else if (strcmp(argv[1], "w52") == 0 && argc >= 4)
    {
      unsigned reg = strtoul(argv[2], NULL, 0);
      unsigned val = strtoul(argv[3], NULL, 0);
      int ret = rk3576_skw_dbg_cmd52(1, reg, val, NULL);

      printf("w52 %03x <= %02x (%d)\n", reg, val, ret);
    }
  else if (strcmp(argv[1], "r53d") == 0 && argc >= 3)
    {
      int len = strtoul(argv[2], NULL, 0);
      int actual = 0;
      int ret;

      if (len > (int)sizeof(buf))
        {
          len = sizeof(buf);
        }

      memset(buf, 0, sizeof(buf));
      ret = rk3576_skw_dbg_dt_read(buf, len, &actual);
      printf("r53d ret=%d actual=%d\n", ret, actual);
      for (i = 0; i < len && i < 64; i++)
        {
          printf("%02x%s", buf[i], ((i + 1) % 16) ? " " : "\n");
        }

      printf("\n");
    }
  else if (strcmp(argv[1], "r53") == 0 && argc >= 3)
    {
      int len = strtoul(argv[2], NULL, 0);
      int actual = 0;
      int ret;

      if (len > (int)sizeof(buf))
        {
          len = sizeof(buf);
        }

      memset(buf, 0, sizeof(buf));
      ret = rk3576_skw_dbg_cmd53_read(buf, len, &actual);
      printf("r53 ret=%d actual=%d\n", ret, actual);
      for (i = 0; i < actual && i < 128; i++)
        {
          printf("%02x%s", buf[i], ((i + 1) % 16) ? " " : "\n");
        }

      printf("\n");
    }
  else if (strcmp(argv[1], "w53") == 0 && argc >= 3)
    {
      int n = hex2buf(argv[2], buf, sizeof(buf));
      int ret = rk3576_skw_dbg_cmd53_write(buf, n);

      printf("w53 %d bytes (%d)\n", n, ret);
    }
  else if (strcmp(argv[1], "latch") == 0 && argc >= 3)
    {
      unsigned addr = strtoul(argv[2], NULL, 0);
      int ret = rk3576_skw_dbg_latch(addr);

      printf("latch %08x (%d)\n", addr, ret);
    }
  else if (strcmp(argv[1], "mem") == 0 && argc >= 3)
    {
      volatile unsigned *p =
        (volatile unsigned *)(uintptr_t)strtoul(argv[2], NULL, 0);

      if (argc >= 4)
        {
          *p = strtoul(argv[3], NULL, 0);
        }

      printf("mem %p = %08x\n", p, *p);
    }
  else if (strcmp(argv[1], "cplog") == 0 && argc >= 3)
    {
      printf("cplog=%d ret=%d\n", atoi(argv[2]),
             rk3576_skw_dbg_cplog(atoi(argv[2])));
    }
  else if (strcmp(argv[1], "br") == 0)
    {
      printf("bringup = %d\n", rk3576_skw_dbg_bringup());
    }
  else if (strcmp(argv[1], "cmd") == 0 && argc >= 3)
    {
      unsigned id = strtoul(argv[2], NULL, 0);
      int n = 0;

      if (argc >= 4)
        {
          n = hex2buf(argv[3], buf, sizeof(buf));
        }

      printf("cmd %u ret=%d\n", id,
             rk3576_skw_dbg_sendcmd(id, buf, n));
    }
  else if (strcmp(argv[1], "wconn") == 0 && argc >= 3)
    {
      printf("wconn %s = %d\n", argv[2], rk3576_skw_dbg_connect(argv[2]));
    }
  else if (strcmp(argv[1], "wrx") == 0)
    {
      int n = rk3576_skw_dbg_rx_eapol(buf, sizeof(buf));
      if (n < 0)
        {
          printf("wrx empty\n");
        }
      else
        {
          printf("wrx %d\n", n);
          for (i = 0; i < n; i++)
            {
              printf("%02x%s", buf[i], ((i + 1) % 16) ? " " : "\n");
            }

          printf("\n");
        }
    }
  else if (strcmp(argv[1], "wbssid") == 0)
    {
      unsigned char bm[6], mm[6];
      rk3576_skw_dbg_get_bssid(bm);
      rk3576_skw_dbg_get_mac(mm);
      printf("wbssid %02x%02x%02x%02x%02x%02x mac %02x%02x%02x%02x%02x%02x\n",
             bm[0], bm[1], bm[2], bm[3], bm[4], bm[5],
             mm[0], mm[1], mm[2], mm[3], mm[4], mm[5]);
    }
  else if (strcmp(argv[1], "wtx") == 0 && argc >= 3)
    {
      int n = hex2buf(argv[2], buf, sizeof(buf));
      printf("wtx %d = %d\n", n, rk3576_skw_dbg_data_tx(buf, n));
    }
  else
    {
      printf("skwsh: bad args\n");
      return 1;
    }

  return 0;
}
