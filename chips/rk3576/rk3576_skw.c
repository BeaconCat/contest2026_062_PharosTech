/****************************************************************************
 * chips/rk3576/rk3576_skw.c
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
 * SeekWave SV6621 (SWT6621-S) WiFi/BT combo SDIO core driver.
 *
 * Clean-room native port: the CP boot/transport protocol (register map,
 * command sequence, packet framing) is reproduced from behavioral facts;
 * no GPL driver code is copied.  Runs directly on the RK3576 DW-MSHC SDIO
 * host (mmc@2a320000) with raw register I/O for the low-level command
 * path -- the standard NuttX sdio_dev_s is used only to bring the
 * controller block and its clocks up.
 *
 * Bring-up chain (all validated on hardware, 2026-07-14):
 *   power seq -> CMD5(S18R)/CMD11 1.8 V -> CMD3/CMD7 -> CCCR + SDR104
 *   select + CMD19 tuning -> 198 MHz -> func1 -> DT chip-id "SV6160LITE"
 *   -> DMA-type/sleep -> firmware download (byte-mode, incrementing) ->
 *   download-done -> CP boot ("trunk_W" loopcheck) -> WIFI_START ->
 *   "WIFIREADY".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>
#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/sdio.h>

#include "rk3576_sdmmc.h"
#include "rk3576_skw.h"
#include "rk3576_skw_internal.h"
#include "rk3576_skw_wpa.h"

#ifdef CONFIG_RK3576_SKW

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DW-MSHC (SDIO0) register block. */

#define SKW_MSHC_BASE       0x2a320000
#define SKW_CTRL            (SKW_MSHC_BASE + 0x000)
#define SKW_CLKDIV          (SKW_MSHC_BASE + 0x008)
#define SKW_CLKENA          (SKW_MSHC_BASE + 0x010)
#define SKW_CTYPE           (SKW_MSHC_BASE + 0x018)
#define SKW_BLKSIZ          (SKW_MSHC_BASE + 0x01c)
#define SKW_BYTCNT          (SKW_MSHC_BASE + 0x020)
#define SKW_INTMASK         (SKW_MSHC_BASE + 0x024)
#define SKW_CMDARG          (SKW_MSHC_BASE + 0x028)
#define SKW_CMD             (SKW_MSHC_BASE + 0x02c)
#define SKW_RESP0           (SKW_MSHC_BASE + 0x030)
#define SKW_RINTSTS         (SKW_MSHC_BASE + 0x044)
#define SKW_STATUS          (SKW_MSHC_BASE + 0x048)
#define SKW_FIFO            (SKW_MSHC_BASE + 0x200)
#define SKW_TIMING0         (SKW_MSHC_BASE + 0x130)
#define SKW_TIMING1         (SKW_MSHC_BASE + 0x134)

#define SKW_CRU_SDIO_SEL    0x272004a0    /* CLKSEL_CON104: sdio clk src */

/* CMD register command words (start | resp/data/crc bits | index). */

#define SKW_CMD_START       (1u << 31)
#define SKW_CMD_VOLTSW      (1u << 28)
#define SKW_CMD_WAITPRV     (1u << 13)

#define SKW_CMDW_CMD3       0xa0000143    /* R6 */
#define SKW_CMDW_CMD5       0xa0000045    /* R4 (no CRC) */
#define SKW_CMDW_CMD7       0xa0000147    /* R1 */
#define SKW_CMDW_CMD11      0xb000014b    /* R1 + voltage switch */
#define SKW_CMDW_CMD19      0xa0002353    /* R1 + data (tuning) */
#define SKW_CMDW_CMD52      0xa0000174    /* R5 */
#define SKW_CMDW_CMD53RD    0xa0002375    /* R5 + data + wait-prev */
#define SKW_CMDW_CMD53WR    0xa0002775    /* R5 + data + write + wait-prev */

/* RINTSTS bits. */

#define SKW_INT_CMDDONE     (1u << 2)
#define SKW_INT_DTO         (1u << 3)
#define SKW_INT_RTO         (1u << 8)
#define SKW_INT_VOLTSW      (1u << 12)
#define SKW_INT_SDIO        (1u << 16)    /* SDIO card interrupt (DAT1) */
#define SKW_INT_DATAERR     0x0000ae80    /* DCRC/DRTO/SBE/EBE/FRUN/HTO */

#define SKW_CLK_UPDATE      0x80202000    /* start | update-clk-only | wait */
#define SKW_CLK_UPD_VOLT    0x90202000    /* + voltage-switch */

/* CP-side (SWT6621S) register map -- func0 CMD52 mailbox + DT windows. */

#define SKW_FBR_ADDR        0x15c         /* 4-byte LE DT address latch */
#define SKW_DT_WINDOW       0x0f          /* func1 DT data window */
#define SKW_PK_WINDOW       0x20          /* func1 packet window */
#define SKW_REG_INTX        0x05          /* CCCR INTx pending */
#define SKW_REG_INT_EXT     0x16          /* SDIO extended interrupt status */
#define SKW_REG_CP2AP_FIFO  0x181         /* CP RX FIFO non-empty indication (poll data-ready) */
#define SKW_REG_DMA_TYPE    0x165         /* 1 = ADMA */
#define SKW_REG_SLP         0x167         /* 1 = sleep disabled */
#define SKW_REG_RX_FTL0     0x16c         /* RX channel flow control (ports 0-7) */
#define SKW_REG_RX_FTL1     0x16d         /* RX channel flow control (ports 8+) */
#define SKW_REG_DL_DONE     0x160         /* download-done / boot signal */
#define SKW_REG_CPLOG_SW    0x166         /* CP-log-to-AP switch */
#define SKW_REG_AP2CP_IRQ   0x1b0         /* AP->CP doorbell */

/* CP memory map. */

#define SKW_CP_CHIPID       0x40000000    /* 16-byte ASCII "SV6160LITE" */
#define SKW_CP_IRAM         0x00100000
#define SKW_CP_DRAM         0x20200000

/* Clock: CRU source 0x2f02 = 396 MHz cclk_src_sdio; CLKDIV 0 -> 198 MHz. */

#define SKW_SDIO_SRC_396M   0x2f02

/* SDR104 sample/drive phase, hiword-masked TIMING encoding.  raw bits
 * [11:1]: DELAY_SEL(10) | delaynum(9:2) | nineties(1:0).  The on-board
 * tuning sweep found the whole 180 deg tap band passing; freeze at 180.
 */

#define SKW_TCON(raw)       (((uint32_t)0x7ff << 1 << 16) | \
                             ((uint32_t)(raw) << 1))
#define SKW_TCON_180        SKW_TCON(0x2)

#define SKW_POLL_LIMIT      200000        /* register spin bound */

/* Packet framing (SDIO2, V2.0).  32-bit LE header: len:16 | pad:7 |
 * eof:1 | channel:8.  Max packet 0x600; RX read granularity 512.
 */

#define SKW_PAC_SIZE        0x600          /* v2 MAX2_PAC_SIZE = 1536 */
#define SKW_NSIZE_OFF       1024          /* v2 ADMA next_size region */
#define SKW_HDR(ch, len)    (((uint32_t)(ch) << 24) | \
                             ((uint32_t)(len) & 0xffff))
#define SKW_HDR_EOF_TERM    (1u << 23)         /* trailing eof-only header */
#define SKW_HDR_CH(h)       (((h) >> 24) & 0xff)
#define SKW_HDR_LEN(h)      ((h) & 0xffff)
#define SKW_HDR_EOF(h)      (((h) >> 23) & 1)

/* Channels. */

#define SKW_CH_LOOPCHECK    1
#define SKW_CH_WIFI_CMD     6
#define SKW_CH_WIFI_DATA    7
#define SKW_CH_WIFI_DATA1   8

/* skw_msg (8-byte command header). */

#define SKW_MSG_CMD         0
#define SKW_MSG_CMD_ACK     1
#define SKW_MSG_EVENT       2

/* WiFi command IDs. */

#define SKW_CMD_GET_INFO    1
#define SKW_CMD_SYN_VERSION 2
#define SKW_CMD_OPEN_DEV    3
#define SKW_CMD_PHY_BB_CFG  50
#define SKW_CMD_ADD_KEY     12
#define SKW_CMD_START_SCAN  5
#define SKW_CMD_JOIN        9
#define SKW_CMD_TX_DATA_FRAME 15
#define SKW_CMD_AUTH        10
#define SKW_CMD_ASSOC       11
#define SKW_CMD_DISCONNECT  17
#define SKW_CMD_SET_MIB     40

/* WiFi events (skw_msg type=EVENT). */

#define SKW_EVENT_SCAN_CMPL   0
#define SKW_EVENT_DISCONNECT  2
#define SKW_EVENT_ASOCC       3
#define SKW_EVENT_RX_MGMT     4
#define SKW_EVENT_DEAUTH      5
#define SKW_EVENT_DISASOC     6
#define SKW_EVENT_JOIN_CMPL   7
#define SKW_EVENT_SCAN_REPORT 11

/* Internal connect-state signals (not CP event ids). */

#define SKW_CONN_ASSOC_OK     100
#define SKW_CONN_ASSOC_FAIL   101

#define SKW_AUTH_OPEN         0

#define SKW_STA_MODE        1

/* Max scan results cached for a scan pass. */

#define SKW_MAX_SCAN        32

#define SKW_CMD_TIMEOUT_MS  2000
#define SKW_CMD_MAXLEN      1588

/* Every channel payload carries a 12-byte inner link header before the
 * real content (skw_msg / ethernet frame / loopcheck text).
 */

#define SKW_LINK_HDR        12

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct sdio_dev_s *g_skw_dev;
static const struct rk3576_skw_board_s *g_skw_board;
static uint32_t g_skw_service;
static bool g_skw_run;

/* Last CP2AP FIFO level snapshot, read by the rx thread to gate work. */

static volatile uint8_t g_skw_fifo_last;

static uint8_t g_skw_mac[6];
static uint8_t g_skw_bssid[6];
static uint8_t g_skw_peer_idx;
static uint8_t g_skw_lmac;
static uint8_t g_skw_mcast;

/* Scan result cache (SSID/BSSID/channel/rssi), filled from SCAN_REPORT
 * events, drained by rk3576_skw_scan().
 */

struct skw_bss_s
{
  uint8_t  bssid[6];
  uint8_t  ssid[33];
  uint8_t  ssid_len;
  uint8_t  channel;
  uint8_t  band;     /* 0 = 2.4 GHz, 1 = 5 GHz */
  int16_t  rssi;
  uint16_t capability;             /* beacon capability field */
  uint16_t beacon_int;             /* beacon interval (TU) */
  uint16_t ie_len;                 /* beacon IE blob length */
  uint8_t  ie[256];                /* beacon IEs (from offset 36) */
};

static struct skw_bss_s g_skw_scan[SKW_MAX_SCAN];
static int g_skw_scan_n;
static volatile bool g_skw_scanning;
static volatile bool g_skw_scan_done;

/* Connection state driven by JOIN/ASOCC/DISCONNECT events. */

static volatile int g_skw_conn_evt;   /* last connect event id, -1 idle */

/* RX burst scratch (0x600*n + 512, one 100-packet burst worst case). */

static uint8_t g_skw_rxbuf[SKW_PAC_SIZE * 8 + 512] aligned_data(64);

/* Serializes SDIO bus transactions between the rx thread and command
 * callers (single DW-MSHC command path).
 */

static mutex_t g_skw_buslock = NXMUTEX_INITIALIZER;

/* TX packet scratch (header + msg + payload, block-padded). */

static uint8_t g_skw_cmdbuf[2048] aligned_data(64);
static uint8_t g_skw_txbuf[2048] aligned_data(64);
static mutex_t g_skw_cmd_lock = NXMUTEX_INITIALIZER;
static mutex_t g_skw_tx_lock = NXMUTEX_INITIALIZER;

/* Command engine: one outstanding command matched by id+seq. */

static struct
{
  sem_t    done;         /* posted by rx thread on matching ACK */
  uint16_t seq;          /* running sequence */
  uint8_t  id;           /* command id awaiting ACK */
  bool     waiting;      /* a command is outstanding */
  int      status;       /* ACK status code */
  uint8_t  resp[256];    /* ACK return payload */
  int      resp_len;
}
g_skw_cmd;

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static inline uint32_t skw_rd(uintptr_t address);
static inline void skw_wr(uintptr_t address, uint32_t value);
static void skw_ciu_update(uint32_t command);
static void skw_set_clock(uint32_t source, uint32_t divider);
static uint32_t skw_cmd(uint32_t command, uint32_t argument,
                        uint32_t *response);
static int skw_cmd52(bool write, uint32_t function, uint32_t register_address,
                     uint8_t value, uint8_t *result);
static void skw_dt_latch(uint32_t cp_address);
static int skw_cmd53_read(uint32_t function, uint32_t register_address,
                          bool increment, uint8_t *buffer, int length);
static int skw_cmd53_write(uint32_t function, uint32_t register_address,
                           bool increment, const uint8_t *buffer, int length);
static int skw_dt_stream(uint32_t cp_address, const uint8_t *buffer,
                         int length);
static void skw_voltage_switch(void);
static void skw_tune_sdr104(void);
static void skw_handle_loopcheck(const uint8_t *payload, int length);
static void skw_handle_event(uint8_t id, const uint8_t *event, int length);
static void skw_handle_wifi_cmd(const uint8_t *payload, int length);
static void skw_data_rx(const uint8_t *payload, int length);
static int skw_rx_burst(int buffer_count);
static int skw_rx_thread(int argc, char **argv);
static int skw_send_cmd(uint8_t id, const uint8_t *payload, int length,
                        uint8_t *response, int *response_length);
static int skw_wifi_bringup_cmds(void);
static int skw_scan(void);
static uint8_t *skw_nv_patch(const uint8_t *iram, int iram_length);
static int skw_download_and_boot(void);
static int skw_bringup(void);
static int skw_wait_conn_evt(int timeout_ms);
static bool skw_bss_is_rsn(const struct skw_bss_s *bss);
static int skw_connect(const struct skw_bss_s *bss);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static inline uint32_t skw_rd(uintptr_t a)
{
  return *(volatile uint32_t *)a;
}

static inline void skw_wr(uintptr_t a, uint32_t v)
{
  *(volatile uint32_t *)a = v;
}

/****************************************************************************
 * Name: skw_ciu_update
 *
 * Description:
 *   Issue a DW-MSHC update-clock-only command and wait for CMD_START to
 *   self-clear (needed after any CLKENA/CLKDIV/source change).
 *
 ****************************************************************************/

static void skw_ciu_update(uint32_t cmdw)
{
  int k;

  skw_wr(SKW_CMD, cmdw);
  for (k = 0; (skw_rd(SKW_CMD) & SKW_CMD_START) && k < SKW_POLL_LIMIT; k++);
}

/****************************************************************************
 * Name: skw_set_clock
 *
 * Description:
 *   Reprogram the SDIO card clock: gate, set the CRU source + CLKDIV,
 *   ungate with low-power disabled (free-running), update.
 *
 ****************************************************************************/

static void skw_set_clock(uint32_t src, uint32_t clkdiv)
{
  skw_wr(SKW_CLKENA, 0x00000000);
  skw_ciu_update(SKW_CLK_UPDATE);
  skw_wr(SKW_CRU_SDIO_SEL, (0x3fffu << 16) | (src & 0x3fff));
  skw_wr(SKW_CLKDIV, clkdiv);
  skw_wr(SKW_CLKENA, 0x00000001);
  skw_ciu_update(SKW_CLK_UPDATE);
  up_mdelay(2);
}

/****************************************************************************
 * Name: skw_cmd
 *
 * Description:
 *   Issue one raw command and wait for command-done or response-timeout.
 *   Returns RINTSTS; the response word goes to *resp (may be NULL).  Waits
 *   out the card's data-busy tail first (issuing into busy caused CRC
 *   errors during bring-up).
 *
 ****************************************************************************/

static uint32_t skw_cmd(uint32_t cmdw, uint32_t arg, uint32_t *resp)
{
  uint32_t rint;
  int k;

  for (k = 0; (skw_rd(SKW_STATUS) & (1u << 9)) && k < SKW_POLL_LIMIT; k++)
    {
      up_udelay(5);
    }

  skw_wr(SKW_RINTSTS, 0xffffffff);
  skw_wr(SKW_CMDARG, arg);
  skw_wr(SKW_CMD, cmdw);

  for (k = 0; (skw_rd(SKW_CMD) & SKW_CMD_START) && k < SKW_POLL_LIMIT; k++);

  for (k = 0; k < SKW_POLL_LIMIT; k++)
    {
      rint = skw_rd(SKW_RINTSTS);
      if (rint & (SKW_INT_CMDDONE | SKW_INT_RTO))
        {
          break;
        }

      up_udelay(5);
    }

  if (resp != NULL)
    {
      *resp = skw_rd(SKW_RESP0);
    }

  return skw_rd(SKW_RINTSTS);
}

/****************************************************************************
 * Name: skw_cmd52
 *
 * Description:
 *   CMD52 IO_RW_DIRECT.  Read data (or NULL) returned in *out.  Returns OK
 *   or a negated errno.
 *
 ****************************************************************************/

static int skw_cmd52(bool write, uint32_t func, uint32_t reg,
                     uint8_t in, uint8_t *out)
{
  uint32_t arg;
  uint32_t resp = 0;
  uint32_t rint;

  arg = (write ? (1u << 31) : 0) | ((func & 7) << 28) |
        (write ? (1u << 27) : 0) | ((reg & 0x1ffff) << 9) |
        (write ? in : 0);

  nxmutex_lock(&g_skw_buslock);
  rint = skw_cmd(SKW_CMDW_CMD52, arg, &resp);
  nxmutex_unlock(&g_skw_buslock);
  if (rint & SKW_INT_RTO)
    {
      return -ETIMEDOUT;
    }

  if (out != NULL)
    {
      *out = resp & 0xff;
    }

  return ((resp >> 8) & 0xcb) ? -EIO : 0;
}

/****************************************************************************
 * Name: skw_dt_latch
 *
 * Description:
 *   Latch a 32-bit CP address into the func0 DT address registers
 *   (0x15C..0x15F, little-endian).
 *
 ****************************************************************************/

static void skw_dt_latch(uint32_t cpaddr)
{
  skw_cmd52(true, 0, SKW_FBR_ADDR + 0, cpaddr & 0xff, NULL);
  skw_cmd52(true, 0, SKW_FBR_ADDR + 1, (cpaddr >> 8) & 0xff, NULL);
  skw_cmd52(true, 0, SKW_FBR_ADDR + 2, (cpaddr >> 16) & 0xff, NULL);
  skw_cmd52(true, 0, SKW_FBR_ADDR + 3, (cpaddr >> 24) & 0xff, NULL);
}

/****************************************************************************
 * Name: skw_cmd53_read / skw_cmd53_write
 *
 * Description:
 *   CMD53 IO_RW_EXTENDED PIO transfers via the DW-MSHC FIFO.  incr picks
 *   incrementing vs fixed addressing; block mode is used for 512-byte
 *   multiples (byte mode otherwise, with BLKSIZ = byte count).
 *
 ****************************************************************************/

static int skw_cmd53_read(uint32_t func, uint32_t reg, bool incr,
                          uint8_t *buf, int len)
{
  uint32_t arg;
  uint32_t rint;
  int words = (len + 3) / 4;
  int got = 0;
  int k;
  bool blk = (len >= 512 && (len % 512) == 0);

  nxmutex_lock(&g_skw_buslock);
  skw_wr(SKW_CTRL, skw_rd(SKW_CTRL) | (1u << 1));   /* FIFO reset */
  for (k = 0; (skw_rd(SKW_CTRL) & (1u << 1)) && k < 100000; k++);

  skw_wr(SKW_BLKSIZ, blk ? 512 : (uint32_t)len);
  skw_wr(SKW_BYTCNT, len);

  arg = ((func & 7) << 28) | (incr ? (1u << 26) : 0) |
        ((reg & 0x1ffff) << 9) |
        (blk ? ((1u << 27) | ((uint32_t)(len / 512) & 0x1ff))
             : ((uint32_t)len & 0x1ff));

  rint = skw_cmd(SKW_CMDW_CMD53RD, arg, NULL);
  if (rint & SKW_INT_RTO)
    {
      return -ETIMEDOUT;
    }

  for (k = 0; k < 400000 && got < words; k++)
    {
      uint32_t status = skw_rd(SKW_STATUS);
      uint32_t fifo_count = (status >> 17) & 0x1fff;

      while (fifo_count-- > 0 && got < words)
        {
          uint32_t w = skw_rd(SKW_FIFO);
          int b;

          for (b = 0; b < 4 && got * 4 + b < len; b++)
            {
              buf[got * 4 + b] = (w >> (8 * b)) & 0xff;
            }

          got++;
        }

      up_udelay(5);
    }

  rint = skw_rd(SKW_RINTSTS);
  nxmutex_unlock(&g_skw_buslock);

  /* A burst shorter than the request ends in DRTO with the received
   * bytes valid: deliver them (packet headers self-terminate).
   */

  if (got != words && got > 0 && (rint & (1u << 9)) != 0)
    {
      return 0;
    }

  return (rint & SKW_INT_DATAERR) ? -EIO : (got == words ? 0 : -EIO);
}

static int skw_cmd53_write(uint32_t func, uint32_t reg, bool incr,
                           const uint8_t *buf, int len)
{
  uint32_t arg;
  uint32_t rint;
  int words = (len + 3) / 4;
  int fed = 0;
  int k;
  bool blk = (len > 512 && (len % 512) == 0);

  nxmutex_lock(&g_skw_buslock);
  skw_wr(SKW_CTRL, skw_rd(SKW_CTRL) | (1u << 1));
  for (k = 0; (skw_rd(SKW_CTRL) & (1u << 1)) && k < 100000; k++);

  skw_wr(SKW_BLKSIZ, blk ? 512 : (uint32_t)len);
  skw_wr(SKW_BYTCNT, len);

  arg = (1u << 31) | ((func & 7) << 28) | (incr ? (1u << 26) : 0) |
        ((reg & 0x1ffff) << 9) |
        (blk ? ((1u << 27) | ((uint32_t)(len / 512) & 0x1ff))
             : ((uint32_t)len & 0x1ff));

  for (k = 0; (skw_rd(SKW_STATUS) & (1u << 9)) && k < SKW_POLL_LIMIT; k++)
    {
      up_udelay(5);
    }

  skw_wr(SKW_RINTSTS, 0xffffffff);
  skw_wr(SKW_CMDARG, arg);
  skw_wr(SKW_CMD, SKW_CMDW_CMD53WR);
  for (k = 0; (skw_rd(SKW_CMD) & SKW_CMD_START) && k < SKW_POLL_LIMIT; k++);

  for (k = 0; k < 2000000 && fed < words; k++)
    {
      if (!(skw_rd(SKW_STATUS) & (1u << 3)))    /* FIFO not full */
        {
          uint32_t w = 0;
          int b;

          for (b = 0; b < 4 && fed * 4 + b < len; b++)
            {
              w |= ((uint32_t)buf[fed * 4 + b]) << (8 * b);
            }

          skw_wr(SKW_FIFO, w);
          fed++;
        }
    }

  for (k = 0; k < 400000; k++)
    {
      rint = skw_rd(SKW_RINTSTS);
      if (rint & (SKW_INT_DTO | SKW_INT_RTO))
        {
          break;
        }

      up_udelay(5);
    }

  rint = skw_rd(SKW_RINTSTS);
  nxmutex_unlock(&g_skw_buslock);
  if ((rint & SKW_INT_RTO) || fed < words)
    {
      return -ETIMEDOUT;
    }

  return (rint & SKW_INT_DATAERR) ? -EIO : 0;
}

/****************************************************************************
 * Name: skw_dt_stream
 *
 * Description:
 *   Download an image to CP memory over the DT window: latch the base
 *   address once, then stream 512-byte incrementing byte-mode writes with
 *   an exact final chunk.  Retries a chunk on transient CRC via CCCR abort.
 *
 ****************************************************************************/

static int skw_dt_stream(uint32_t cpaddr, const uint8_t *buf, int len)
{
  int off = 0;

  skw_dt_latch(cpaddr);

  while (off < len)
    {
      int chunk = (len - off > 512) ? 512 : (len - off);
      int try;
      int ret;

      ret = skw_cmd53_write(1, SKW_DT_WINDOW, true, buf + off, chunk);
      for (try = 0; ret < 0 && try < 4; try++)
        {
          skw_cmd52(true, 0, 0x06, 0x01, NULL);   /* I/O abort */
          up_mdelay(2);
          skw_dt_latch(cpaddr + off);
          ret = skw_cmd53_write(1, SKW_DT_WINDOW, true, buf + off, chunk);
        }

      if (ret < 0)
        {
          wlerr("ERROR: DT stream failed at +%d\n", off);
          return ret;
        }

      off += chunk;
    }

  return 0;
}

/****************************************************************************
 * Name: skw_voltage_switch
 *
 * Description:
 *   Perform the SD 1.8 V signaling switch: CMD11 with the DW-MSHC voltage-
 *   switch command bit, wait the volt-switch interrupt, gate the clock,
 *   set the host UHS 1.8 V flag, restart, and wait for the card to release
 *   the lines (second volt-switch interrupt).
 *
 ****************************************************************************/

static void skw_voltage_switch(void)
{
  uint32_t resp;
  int k;

  skw_cmd(SKW_CMDW_CMD11, 0, &resp);

  for (k = 0; k < 100000; k++)
    {
      if (skw_rd(SKW_RINTSTS) & SKW_INT_VOLTSW)
        {
          break;
        }

      up_udelay(5);
    }

  skw_wr(SKW_RINTSTS, SKW_INT_VOLTSW);

  skw_wr(SKW_CLKENA, 0x00000000);
  skw_ciu_update(SKW_CLK_UPD_VOLT);
  skw_wr(SKW_MSHC_BASE + 0x074, skw_rd(SKW_MSHC_BASE + 0x074) | 1);
  up_mdelay(10);
  skw_wr(SKW_CLKENA, 0x00000001);
  skw_ciu_update(SKW_CLK_UPD_VOLT);

  for (k = 0; k < 200000; k++)
    {
      if (skw_rd(SKW_RINTSTS) & SKW_INT_VOLTSW)
        {
          break;
        }

      up_udelay(5);
    }

  skw_wr(SKW_RINTSTS, SKW_INT_VOLTSW);
  up_mdelay(5);
}

/****************************************************************************
 * Name: skw_tune_sdr104
 *
 * Description:
 *   Select 4-bit + SDR104 on the card and host, raise the clock to 198
 *   MHz, freeze the sample/drive phase at 180 deg (the on-board sweep
 *   found the full band passing at this rate), and run one CMD19 tuning
 *   block read to satisfy the card.
 *
 ****************************************************************************/

static void skw_tune_sdr104(void)
{
  uint32_t resp;
  uint8_t v;
  int got = 0;
  int k;

  skw_cmd52(false, 0, 0x07, 0, &v);
  skw_cmd52(true, 0, 0x07, 0x02, NULL);         /* card 4-bit */
  skw_wr(SKW_CTYPE, 0x00000001);                /* host 4-bit */
  skw_cmd52(false, 0, 0x13, 0, &v);
  skw_cmd52(true, 0, 0x13, 0x07, NULL);         /* EHS + SDR104 */

  skw_set_clock(SKW_SDIO_SRC_396M, 0);          /* 198 MHz */
  skw_wr(SKW_TIMING0, SKW_TCON_180);
  skw_wr(SKW_TIMING1, SKW_TCON_180);

  skw_wr(SKW_CTRL, skw_rd(SKW_CTRL) | (1u << 1));
  for (k = 0; (skw_rd(SKW_CTRL) & (1u << 1)) && k < 100000; k++);
  skw_wr(SKW_BLKSIZ, 64);
  skw_wr(SKW_BYTCNT, 64);
  skw_cmd(SKW_CMDW_CMD19, 0, &resp);
  for (k = 0; k < 200000 && got < 16; k++)
    {
      uint32_t fc = (skw_rd(SKW_STATUS) >> 17) & 0x1fff;

      while (fc-- > 0 && got < 16)
        {
          (void)skw_rd(SKW_FIFO);
          got++;
        }

      up_udelay(2);
    }
}

/****************************************************************************
 * Name: skw_handle_loopcheck
 *
 * Description:
 *   Parse a loopcheck-channel packet for the CP ready tokens and advance
 *   the service state / doorbell.
 *
 ****************************************************************************/

static void skw_handle_loopcheck(const uint8_t *pl, int len)
{
  /* Print every loopcheck message: the CP announces firmware asserts
   * here ("BSPASSERT ...file-line") and losing them hides the crash
   * cause.
   */

  {
    char txt[120];
    int n = (len < (int)sizeof(txt) - 1) ? len : (int)sizeof(txt) - 1;
    int i;

    for (i = 0; i < n; i++)
      {
        txt[i] = (pl[i] >= 0x20 && pl[i] < 0x7f) ? pl[i] : 46;
      }

    txt[n] = 0;
    syslog(LOG_ERR, "SKW: cp> %s\n", txt);
  }

  if (memmem(pl, len, "trunk_W", 7) != NULL &&
      !(g_skw_service & RK3576_SKW_STATE_BSP))
    {
      g_skw_service |= RK3576_SKW_STATE_BSP;

      /* WIFI_START doorbell: 1 << ((service 0 << 1) | cmd 0). */

      skw_cmd52(true, 0, SKW_REG_AP2CP_IRQ, 0x01, NULL);
    }

  if (memmem(pl, len, "WIFIREADY", 9) != NULL)
    {
      g_skw_service |= RK3576_SKW_STATE_WIFI;
    }

  if (memmem(pl, len, "BTREADY", 7) != NULL)
    {
      g_skw_service |= RK3576_SKW_STATE_BT;
    }
}

/****************************************************************************
 * Name: skw_handle_wifi_cmd
 *
 * Description:
 *   Dispatch a channel-6 packet: an skw_msg (8-byte header) followed by
 *   payload.  A CMD_ACK matching the outstanding command's id+seq wakes
 *   the command waiter; asynchronous scan and connection events are
 *   handled separately by the state machines below.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: skw_handle_event
 *
 * Description:
 *   Handle a WiFi event (skw_msg type=EVENT).  SCAN_REPORT carries an
 *   skw_mgmt_hdr {chan, band, s16 signal, u16 mgmt_len, u16 resv} followed
 *   by a full 802.11 beacon/probe-resp frame; extract BSSID (addr3), the
 *   SSID IE (tag 0), channel and RSSI into the scan cache.  SCAN_CMPL ends
 *   the scan.
 *
 ****************************************************************************/

static void skw_handle_event(uint8_t id, const uint8_t *ev, int len)
{
  if (id == SKW_EVENT_SCAN_CMPL)
    {
      g_skw_scan_done = true;
      return;
    }

  if (id == SKW_EVENT_ASOCC || id == SKW_EVENT_JOIN_CMPL)
    {
      g_skw_conn_evt = SKW_CONN_ASSOC_OK;
      return;
    }

  if (id == SKW_EVENT_DISCONNECT || id == SKW_EVENT_DEAUTH ||
      id == SKW_EVENT_DISASOC)
    {
      g_skw_conn_evt = SKW_CONN_ASSOC_FAIL;
      return;
    }

  /* RX_MGMT: the CP forwards a received 802.11 mgmt frame.  Auth-resp and
   * assoc-resp arrive here (there is no separate ASOCC event).  Payload =
   * skw_mgmt_hdr {chan, band, s16 signal, u16 mgmt_len, u16 resv} + frame.
   * Assoc-resp (subtype 0x10) status_code at frame offset 26; 0 = success.
   */

  if (id == SKW_EVENT_RX_MGMT && len >= 8 + 28)
    {
      const uint8_t *mgmt = ev + 8;
      uint8_t subtype = mgmt[0] & 0xf0;

      if (subtype == 0x10)                        /* assoc-response */
        {
          uint16_t status = mgmt[26] | (mgmt[27] << 8);

          syslog(LOG_INFO, "SKW: assoc-resp status=%u\n", status);
          g_skw_conn_evt = (status == 0) ? SKW_CONN_ASSOC_OK :
                                           SKW_CONN_ASSOC_FAIL;
        }
      else if (subtype == 0xc0 || subtype == 0xa0) /* deauth/disassoc */
        {
          g_skw_conn_evt = SKW_CONN_ASSOC_FAIL;
        }

      return;
    }

  if (id == SKW_EVENT_SCAN_REPORT && len >= 8 + 24)
    {
      uint8_t chan = ev[0];
      int16_t signal = (int16_t)(ev[2] | (ev[3] << 8));
      uint16_t mgmt_len = ev[4] | (ev[5] << 8);
      const uint8_t *mgmt = ev + 8;
      int ie_off;
      int mlen = len - 8;
      struct skw_bss_s *bss;
      int i;

      if ((int)mgmt_len < mlen)
        {
          mlen = mgmt_len;
        }

      if (mlen < 36 || g_skw_scan_n >= SKW_MAX_SCAN)
        {
          return;
        }

      /* 802.11 mgmt: fc(2) dur(2) a1(6) a2(6) a3=BSSID(6) seq(2) +
       * beacon body: timestamp(8) beacon_int(2) capab(2) then IEs @36.
       */

      bss = &g_skw_scan[g_skw_scan_n];
      memset(bss, 0, sizeof(*bss));
      memcpy(bss->bssid, mgmt + 16, 6);
      bss->channel = chan;
      bss->band    = ev[1];  /* band from skw_mgmt_hdr */
      bss->rssi = signal;

      /* Beacon/probe-resp body: timestamp(8) beacon_int(2) capability(2)
       * at offset 34, IEs from offset 36.
       */

      bss->beacon_int = mgmt[32] | (mgmt[33] << 8);
      bss->capability = mgmt[34] | (mgmt[35] << 8);
      bss->ie_len = (mlen - 36 > (int)sizeof(bss->ie)) ?
                    sizeof(bss->ie) : (mlen - 36);
      memcpy(bss->ie, mgmt + 36, bss->ie_len);

      for (ie_off = 36; ie_off + 2 <= mlen; )
        {
          uint8_t tag = mgmt[ie_off];
          uint8_t taglen = mgmt[ie_off + 1];

          if (ie_off + 2 + taglen > mlen)
            {
              break;
            }

          if (tag == 0)                 /* SSID */
            {
              int n = taglen > 32 ? 32 : taglen;

              memcpy(bss->ssid, mgmt + ie_off + 2, n);
              bss->ssid_len = n;
            }

          ie_off += 2 + taglen;
        }

      /* De-dup by BSSID. */

      for (i = 0; i < g_skw_scan_n; i++)
        {
          if (memcmp(g_skw_scan[i].bssid, bss->bssid, 6) == 0)
            {
              return;
            }
        }

      g_skw_scan_n++;
    }
}

static void skw_handle_wifi_cmd(const uint8_t *pl, int len)
{
  const uint8_t *msg;
  int mlen;
  uint8_t type;
  uint8_t id;
  uint16_t seq;

  /* Skip the 12-byte inner link header; the skw_msg follows. */

  if (len <= SKW_LINK_HDR + 8)
    {
      return;
    }

  msg  = pl + SKW_LINK_HDR;
  mlen = len - SKW_LINK_HDR;
  type = (msg[0] >> 4) & 0xf;
  id   = msg[1];
  seq  = msg[2] | (msg[3] << 8);

  if (type == SKW_MSG_EVENT)
    {
      skw_handle_event(id, msg + 8, mlen - 8);
      return;
    }

  if (type == SKW_MSG_CMD_ACK && g_skw_cmd.waiting &&
      id == g_skw_cmd.id && seq == g_skw_cmd.seq)
    {
      /* ACK: skw_msg(8) + status(u16) + return data. */

      g_skw_cmd.status   = (mlen >= 10) ? (msg[8] | (msg[9] << 8)) : -1;
      g_skw_cmd.resp_len = (mlen > 10) ? (mlen - 10) : 0;
      if (g_skw_cmd.resp_len > (int)sizeof(g_skw_cmd.resp))
        {
          g_skw_cmd.resp_len = sizeof(g_skw_cmd.resp);
        }

      if (g_skw_cmd.resp_len > 0)
        {
          memcpy(g_skw_cmd.resp, msg + 10, g_skw_cmd.resp_len);
        }

      g_skw_cmd.waiting = false;
      nxsem_post(&g_skw_cmd.done);
    }
}

/****************************************************************************
 * Name: skw_data_rx
 *
 * Description:
 *   Handle a channel-7 (data) receive packet.  The CP delivers 802.3
 *   frames behind an RX descriptor; EAPOL frames (EtherType 0x888e) are
 *   routed to the host WPA supplicant, other frames are dropped until the
 *   netdev data plane is wired up.  The descriptor length is located by
 *   scanning for the EAPOL EtherType.
 *
 ****************************************************************************/

static void skw_data_rx(const uint8_t *pl, int len)
{
  int i;

  /* The CP prepends an RX descriptor of variable length, so locate the
   * 802.3 frame by its EtherType.  EAPOL (0x888e) is matched first in its
   * own pass so a MAC byte that happens to equal an IP/ARP EtherType can
   * never mis-route a 4-way handshake frame away from the supplicant.
   */

  for (i = 12; i + 2 <= len; i++)
    {
      if (pl[i] == 0x88 && pl[i + 1] == 0x8e)
        {
          const uint8_t *eth = pl + (i - 12);
          int ethlen = len - (i - 12);

          if (ethlen > 14)
            {
              rk3576_skw_wpa_eapol_input(eth + 14, ethlen - 14);
            }

          return;
        }
    }

#ifdef CONFIG_NET
  /* Not EAPOL: dispatch IPv4/ARP/IPv6 data frames to the netdev. */

  for (i = 12; i + 2 <= len; i++)
    {
      uint16_t et = (pl[i] << 8) | pl[i + 1];

      if (et == 0x0800 || et == 0x0806 || et == 0x86dd)
        {
          const uint8_t *eth = pl + (i - 12);
          int ethlen = len - (i - 12);

          if (ethlen > 14)
            {
              rk3576_skw_net_input(eth, ethlen);
            }

          return;
        }
    }
#endif
}

/****************************************************************************
 * Name: skw_rx_burst
 *
 * Description:
 *   Read one RX burst from the packet window and walk its packets.  The
 *   burst length is 0x600*buf_num + 512 (buf_num from the previous burst's
 *   trailing rx_nsize); packets advance by the fixed 0x600 stride.  The
 *   trailing bytes carry rx_nsize (last -4) and valid_len (last -8).
 *   Returns rx_nsize (bytes still queued in the CP), or <0 on error.
 *
 ****************************************************************************/

static int skw_rx_burst(int buf_num)
{
  int readlen;
  uint32_t rx_nsize;
  int off;
  int ret;

  if (buf_num < 1)
    {
      buf_num = 1;
    }
  else if (buf_num > 8)
    {
      buf_num = 8;
    }

  readlen = SKW_PAC_SIZE * buf_num + SKW_NSIZE_OFF;

  ret = skw_cmd53_read(1, SKW_PK_WINDOW, false, g_skw_rxbuf, readlen);
  if (ret < 0)
    {
      return ret;
    }

  /* Walk packets on the 0x600 stride. */

  for (off = 0; off + 4 <= SKW_PAC_SIZE * buf_num; off += SKW_PAC_SIZE)
    {
      const uint8_t *p = g_skw_rxbuf + off;
      uint32_t hdr = p[0] | (p[1] << 8) | (p[2] << 16) |
                     ((uint32_t)p[3] << 24);
      uint32_t ch = SKW_HDR_CH(hdr);
      int plen = SKW_HDR_LEN(hdr);

      if (plen == 0 || ch >= 12 || off + 4 + plen > readlen)
        {
          break;
        }

#ifdef CONFIG_RK3576_SKW_DEBUG
      if (ch == SKW_CH_WIFI_CMD || ch == SKW_CH_WIFI_DATA)
        {
          syslog(LOG_ERR, "SKW: rx ch=%" PRIu32 " len=%d off=%d hdr=%08"
                 PRIx32 "\n", ch, plen, off, hdr);
          syslog(LOG_ERR, "SKW:   %02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                 p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11],
                 p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19]);
          syslog(LOG_ERR, "SKW:   %02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                 p[20], p[21], p[22], p[23], p[24], p[25], p[26], p[27],
                 p[28], p[29], p[30], p[31], p[32], p[33], p[34], p[35]);
        }
#endif

      switch (ch)
        {
          case SKW_CH_LOOPCHECK:
            skw_handle_loopcheck(p + 4, plen);
            break;

          case SKW_CH_WIFI_CMD:
            skw_handle_wifi_cmd(p + 4, plen);
            break;

          case SKW_CH_WIFI_DATA:
            skw_data_rx(p + 4, plen);
            break;

          case SKW_CH_WIFI_DATA1:
            skw_data_rx(p + 4, plen);
            break;

          default:
            break;
        }

      if (SKW_HDR_EOF(hdr))
        {
          break;
        }
    }

  rx_nsize = g_skw_rxbuf[readlen - 4] |
             (g_skw_rxbuf[readlen - 3] << 8) |
             (g_skw_rxbuf[readlen - 2] << 16) |
             ((uint32_t)g_skw_rxbuf[readlen - 1] << 24);

  return (int)rx_nsize;
}


#endif /* CONFIG_RK3576_SKW */
