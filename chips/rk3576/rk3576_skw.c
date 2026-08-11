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

/****************************************************************************
 * Name: skw_rx_thread
 *
 * Description:
 *   Poll the CP interrupt line, drain queued packets, and demux by channel.
 *   (A true inband-IRQ hookup replaces the poll once the netdev data path
 *   needs the latency.)
 *
 ****************************************************************************/

static int skw_rx_thread(int argc, char **argv)
{
  int buf_num = 1;

  while (g_skw_run)
    {
      uint8_t intx = 0;
      int nsize;
      int drained;

      skw_cmd52(false, 0, SKW_REG_INTX, 0, &intx);

      /* Service the DW-MSHC SDIO card interrupt (DAT1): the CP latches it
       * when it has queued data; clearing it acks the controller so the CP
       * re-presents the packet window.  Leaving it latched makes the CP
       * treat its interrupt as unserviced and withhold data frames.
       */

      {
        uint32_t _ri = skw_rd(SKW_RINTSTS);
        if (_ri & SKW_INT_SDIO)
          {
            skw_wr(SKW_RINTSTS, SKW_INT_SDIO);
          }
      }

      int _fi_changed = 0;
      {
        uint8_t _fi = 0;
        skw_cmd52(false, 0, SKW_REG_CP2AP_FIFO, 0, &_fi);
        if (_fi != g_skw_fifo_last)
          {
            g_skw_fifo_last = _fi;
            _fi_changed = 1;
          }
      }

      /* Only touch the packet window when the CP signals new RX data via
       * CP2AP_FIFO_IND (0x181), or while a command ACK is outstanding.
       * Blindly polling an empty window otherwise desyncs the CP's
       * presentation so it withholds data frames.
       */

      {
        static uint32_t idle_cnt;

        if (!_fi_changed && !g_skw_cmd.waiting)
          {
            idle_cnt++;
            if ((idle_cnt & 7) != 0)          /* ~1 in 8 idle polls reads */
              {
                up_mdelay(g_skw_scanning ? 2 : 5);
                continue;
              }
          }
      }

      /* Drain queued packets in a bounded burst, then always yield.  The
       * hard cap + mandatory sleep prevent a runaway spin (a stuck FIFO
       * indication or a persistent read error otherwise pins the CPU and
       * starves the console).
       */

      for (drained = 0; drained < 16 && g_skw_run; drained++)
        {
          nsize = skw_rx_burst(buf_num);
          if (nsize <= 0)
            {
              uint8_t ext = 0;

              /* Empty window: read the extended interrupt status to
               * re-arm the CP so it re-presents any queued data frames
               * (the reference driver does this on every zero-size read).
               */

              skw_cmd52(false, 0, SKW_REG_INT_EXT, 0, &ext);
              buf_num = 1;
              break;
            }

          /* rx_nsize is the packet COUNT still queued in the CP (v2
           * set_packet_num semantics), not a byte count.
           */

          buf_num = (nsize >= 8) ? 8 : nsize;
        }

      /* Poll cadence: brisk while a scan/command is active, relaxed idle. */

      up_mdelay(g_skw_scanning ? 5 : 20);
    }

  return 0;
}

/****************************************************************************
 * Name: skw_send_cmd
 *
 * Description:
 *   Send a WiFi command on channel 6 and wait for its ACK (matched by
 *   id + seq, 2 s timeout).  Returns the ACK status (>=0) or a negated
 *   errno; the ACK return payload is copied to resp/resp_len if given.
 *
 ****************************************************************************/

static int skw_send_cmd(uint8_t id, const uint8_t *payload, int plen,
                        uint8_t *resp, int *resp_len)
{
  int msg_len = 8 + plen;
  int msg_pad = (msg_len + 3) & ~3;             /* word-align the payload */
  int pkt_len = 4 + msg_pad + 4;                /* hdr + payload + eof term */
  int padded = (pkt_len + 511) & ~511;
  uint32_t hdr;
  uint32_t eof;
  int ret;

  if (msg_len > SKW_CMD_MAXLEN || padded > (int)sizeof(g_skw_cmdbuf))
    {
      return -E2BIG;
    }

  ret = nxmutex_lock(&g_skw_cmd_lock);
  if (ret < 0)
    {
      return ret;
    }

  g_skw_cmd.seq++;
  g_skw_cmd.id = id;
  g_skw_cmd.status = -1;
  g_skw_cmd.resp_len = 0;
  g_skw_cmd.waiting = true;

  memset(g_skw_cmdbuf, 0, padded);

  /* Channel-6 data header (eof=0), then the skw_msg + payload, then a
   * trailing eof-only terminator header (matches setup_sdio2_packet).
   */

  hdr = SKW_HDR(SKW_CH_WIFI_CMD, msg_len);
  g_skw_cmdbuf[0] = hdr & 0xff;
  g_skw_cmdbuf[1] = (hdr >> 8) & 0xff;
  g_skw_cmdbuf[2] = (hdr >> 16) & 0xff;
  g_skw_cmdbuf[3] = (hdr >> 24) & 0xff;

  /* skw_msg: inst_id:4|type:4, id, seq(u16), total_len(u16), resv[2]. */

  g_skw_cmdbuf[4] = (SKW_MSG_CMD << 4) | 0;      /* inst 0, type CMD */
  g_skw_cmdbuf[5] = id;
  g_skw_cmdbuf[6] = g_skw_cmd.seq & 0xff;
  g_skw_cmdbuf[7] = (g_skw_cmd.seq >> 8) & 0xff;
  g_skw_cmdbuf[8] = msg_len & 0xff;
  g_skw_cmdbuf[9] = (msg_len >> 8) & 0xff;

  if (plen > 0)
    {
      memcpy(g_skw_cmdbuf + 12, payload, plen);
    }

  eof = SKW_HDR_EOF_TERM;
  g_skw_cmdbuf[4 + msg_pad + 0] = eof & 0xff;
  g_skw_cmdbuf[4 + msg_pad + 1] = (eof >> 8) & 0xff;
  g_skw_cmdbuf[4 + msg_pad + 2] = (eof >> 16) & 0xff;
  g_skw_cmdbuf[4 + msg_pad + 3] = (eof >> 24) & 0xff;

  ret = nxmutex_lock(&g_skw_tx_lock);
  if (ret >= 0)
    {
      ret = skw_cmd53_write(1, SKW_PK_WINDOW, false, g_skw_cmdbuf,
                            padded);
      if (ret >= 0)
        {
          /* Ring the AP->CP doorbell so the CP services the command. */

          ret = skw_cmd52(true, 0, SKW_REG_AP2CP_IRQ, 0x01, NULL);
        }

      nxmutex_unlock(&g_skw_tx_lock);
    }
#ifdef CONFIG_RK3576_SKW_DEBUG
  syslog(LOG_ERR, "SKW: tx cmd id=%u seq=%u msglen=%d padded=%d wr=%d\n",
         id, g_skw_cmd.seq, msg_len, padded, ret);
#endif
  if (ret < 0)
    {
      g_skw_cmd.waiting = false;
      nxmutex_unlock(&g_skw_cmd_lock);
      return ret;
    }

  ret = nxsem_tickwait(&g_skw_cmd.done,
                       MSEC2TICK(SKW_CMD_TIMEOUT_MS));
  if (ret < 0)
    {
      g_skw_cmd.waiting = false;
      wlerr("ERROR: cmd %u ACK timeout\n", id);
      nxmutex_unlock(&g_skw_cmd_lock);
      return -ETIMEDOUT;
    }

  if (resp != NULL && resp_len != NULL)
    {
      int n = g_skw_cmd.resp_len < *resp_len ?
              g_skw_cmd.resp_len : *resp_len;

      memcpy(resp, g_skw_cmd.resp, n);
      *resp_len = n;
    }

  ret = g_skw_cmd.status;
  nxmutex_unlock(&g_skw_cmd_lock);
  return ret;
}

/****************************************************************************
 * Name: skw_wifi_bringup_cmds
 *
 * Description:
 *   Post-WIFIREADY command bring-up: sync the command/event version, read
 *   the chip info (MAC address / capabilities).  Calibration + OPEN_DEV
 *   land with the netdev interface-up path (P2/P3).
 *
 ****************************************************************************/

static int skw_wifi_bringup_cmds(void)
{
  uint8_t resp[128];
  int resp_len;
  int ret;

  /* Silence the CP BSP debug log (channel 9): it otherwise floods the
   * SDIO RX path and buries command ACKs.  Switch = 1 (disable), then the
   * AP->CP doorbell bit5.
   */

  skw_cmd52(true, 0, SKW_REG_CPLOG_SW, 0x01, NULL);
  skw_cmd52(true, 0, SKW_REG_AP2CP_IRQ, 1u << 5, NULL);

  /* SYN_VERSION: a 4-byte host cmd/event version pair (0 = don't care). */

  {
    uint8_t ver[4] = { 0, 0, 0, 0 };

    ret = skw_send_cmd(SKW_CMD_SYN_VERSION, ver, sizeof(ver), NULL, NULL);
    if (ret < 0)
      {
        wlerr("ERROR: SYN_VERSION failed: %d\n", ret);
        return ret;
      }
  }

  /* GET_INFO: returns fw version + capabilities + the efuse MAC.  The MAC
   * sits at a fixed offset in the return payload; capture the first valid
   * 6-byte address (bytes with the group bit clear on byte 0).
   */

  resp_len = sizeof(resp);
  ret = skw_send_cmd(SKW_CMD_GET_INFO, NULL, 0, resp, &resp_len);
  if (ret < 0)
    {
      wlerr("ERROR: GET_INFO failed: %d\n", ret);
      return ret;
    }

  /* The efuse MAC in GET_INFO is all-zero on this module (no efuse burn,
   * same as the vendor stack).  Assign a stable locally-administered
   * address (FE:FD:FC + low bytes of the fw info) so OPEN_DEV's valid-
   * address check passes.
   */

  g_skw_mac[0] = 0xfe;
  g_skw_mac[1] = 0xfd;
  g_skw_mac[2] = 0xfc;
  g_skw_mac[3] = (resp_len > 0) ? resp[0] : 0x01;
  g_skw_mac[4] = (resp_len > 1) ? resp[1] : 0x02;
  g_skw_mac[5] = 0x03;

  syslog(LOG_INFO,
         "SKW: GET_INFO ok (%d bytes) MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         resp_len, g_skw_mac[0], g_skw_mac[1], g_skw_mac[2],
         g_skw_mac[3], g_skw_mac[4], g_skw_mac[5]);

  /* RF calibration download (PHY_BB_CFG): 512-byte chunks with a
   * seq/end/len header.  Without it the CP runs default RF parameters:
   * weak or empty scans and firmware asserts on RF-heavy operations.
   */

  if (g_skw_board->calib != NULL && g_skw_board->calib_len > 0)
    {
      const uint8_t *cd = g_skw_board->calib;
      int remain = g_skw_board->calib_len;
      uint8_t chunk[4 + 512];
      int seq = 0;

      while (remain > 0)
        {
          int len = (remain < 512) ? remain : 512;

          chunk[0] = (uint8_t)seq;
          chunk[1] = (remain == len) ? 1 : 0;
          chunk[2] = len & 0xff;
          chunk[3] = (len >> 8) & 0xff;
          memcpy(chunk + 4, cd, len);

          ret = skw_send_cmd(SKW_CMD_PHY_BB_CFG, chunk, 4 + len,
                             NULL, NULL);
          if (ret < 0)
            {
              wlerr("ERROR: PHY_BB_CFG seq %d failed: %d\n", seq, ret);
              break;
            }

          cd += len;
          remain -= len;
          seq++;
        }

      syslog(LOG_ERR, "SKW: calib download %s (%d bytes)\n",
             ret == 0 ? "ok" : "FAILED", g_skw_board->calib_len);
    }

  /* OPEN_DEV: bring up the station interface (mode + flags + MAC). */

  {
    uint8_t op[10];

    op[0] = SKW_STA_MODE & 0xff;   /* u16 mode = STA */
    op[1] = 0;
    op[2] = 0;                     /* u16 flags = 0 */
    op[3] = 0;
    memcpy(op + 4, g_skw_mac, 6);

    ret = skw_send_cmd(SKW_CMD_OPEN_DEV, op, sizeof(op), NULL, NULL);
    if (ret < 0)
      {
        wlerr("ERROR: OPEN_DEV failed: %d\n", ret);
        return ret;
      }

    syslog(LOG_INFO, "SKW: OPEN_DEV ok (STA)\n");
  }

  return OK;
}

/****************************************************************************
 * Name: skw_scan
 *
 * Description:
 *   Active scan on all channels (wildcard SSID) and collect the SCAN_REPORT
 *   results.  Returns the number of BSSes found.
 *
 ****************************************************************************/

static int skw_scan(void)
{
  /* skw_scan_param (32-byte fixed head) + 2.4 GHz (ch 1-13, band 0) and
   * 5 GHz non-DFS channels (band 1) in the variable tail.
   * skw_scan_chan_info = {chan_num, band, scan_flags} = 3 bytes each.
   */

  static const uint8_t g_5g_chans[] =
  {
    36, 40, 44, 48,              /* UNII-1          */
    52, 56, 60, 64,              /* UNII-2 no DFS   */
    149, 153, 157, 161, 165      /* UNII-3          */
  };

  enum { N_2G = 13, N_5G = sizeof(g_5g_chans), N_CH = N_2G + N_5G };

  uint8_t sp[32 + N_CH * 3];
  int ret;
  int wait;
  int ch;

  memset(sp, 0, sizeof(sp));
  sp[8]  = N_CH;                    /* nr_chan (u32 LE) */
  sp[12] = 32;                      /* chan_offset = end of fixed head */

  for (ch = 0; ch < N_2G; ch++)
    {
      sp[32 + ch * 3 + 0] = ch + 1; /* chan_num 1..13   */
      sp[32 + ch * 3 + 1] = 0;      /* band 2.4 GHz    */
      sp[32 + ch * 3 + 2] = 0;      /* scan_flags      */
    }

  for (ch = 0; ch < N_5G; ch++)
    {
      sp[32 + (N_2G + ch) * 3 + 0] = g_5g_chans[ch]; /* chan_num */
      sp[32 + (N_2G + ch) * 3 + 1] = 1;              /* band 5 GHz */
      sp[32 + (N_2G + ch) * 3 + 2] = 0;              /* scan_flags */
    }

  g_skw_scan_n = 0;
  g_skw_scan_done = false;
  g_skw_scanning = true;

  ret = skw_send_cmd(SKW_CMD_START_SCAN, sp, sizeof(sp), NULL, NULL);
  if (ret < 0)
    {
      g_skw_scanning = false;
      wlerr("ERROR: START_SCAN failed: %d\n", ret);
      return ret;
    }

  /* Reports arrive as events on the rx thread; wait up to 5 s or CMPL. */

  for (wait = 0; wait < 500 && !g_skw_scan_done; wait++)
    {
      up_mdelay(10);
    }

  g_skw_scanning = false;
  return g_skw_scan_n;
}

/****************************************************************************
 * Name: skw_nv_patch
 *
 * Description:
 *   Copy the IRAM image and splice the NV common-config blob into its NV
 *   slot ("TSVN" marker + 4).  Booting with default NV content leaves the
 *   CP on wrong RF/common parameters.
 *
 ****************************************************************************/

static uint8_t *skw_nv_patch(const uint8_t *iram, int iram_len)
{
  const uint8_t *nv = g_skw_board->nv;
  uint32_t off;
  uint32_t size;
  uint8_t *copy;
  int i;

  if (nv == NULL || g_skw_board->nv_len < 36)
    {
      return NULL;
    }

  off  = nv[8] | (nv[9] << 8) | ((uint32_t)nv[10] << 16) |
         ((uint32_t)nv[11] << 24);
  size = nv[12] | (nv[13] << 8) | ((uint32_t)nv[14] << 16) |
         ((uint32_t)nv[15] << 24);

  if (off == 0 || size == 0 || (int)(off + size) > g_skw_board->nv_len)
    {
      return NULL;
    }

  copy = kmm_malloc(iram_len);
  if (copy == NULL)
    {
      return NULL;
    }

  memcpy(copy, iram, iram_len);

  for (i = 0; i + 4 <= iram_len && i < 0x200; i += 4)
    {
      if (memcmp(copy + i, "TSVN", 4) == 0)
        {
          syslog(LOG_ERR, "BR: NV patch @0x%x size %u\n",
                 (unsigned)(i + 4), (unsigned)size);
          memcpy(copy + i + 4, nv + off, size);
          return copy;
        }
    }

  kmm_free(copy);
  return NULL;
}

/****************************************************************************
 * Name: skw_download_and_boot
 *
 * Description:
 *   Parse the IRAM head marker for the download addresses, program the DMA
 *   type + sleep disable, stream both images to CP memory, and raise the
 *   download-done signal.  Returns OK on a clean download.
 *
 ****************************************************************************/

static int skw_download_and_boot(void)
{
  const uint8_t *iram = g_skw_board->iram;
  int iram_len = g_skw_board->iram_len;
  uint32_t iram_addr = 0;
  uint32_t dram_addr = 0;
  int off;
  int ret;

  /* Head marker "kees0616" in the first 0x200 bytes; the following two
   * 32-bit LE words are the IRAM and DRAM download addresses.
   */

  for (off = 0; off + 16 <= 0x200 && off + 16 <= iram_len; off += 4)
    {
      if (memcmp(iram + off, "kees0616", 8) == 0)
        {
          const uint8_t *p = iram + off + 8;

          iram_addr = p[0] | (p[1] << 8) | (p[2] << 16) |
                      ((uint32_t)p[3] << 24);
          dram_addr = p[4] | (p[5] << 8) | (p[6] << 16) |
                      ((uint32_t)p[7] << 24);
          break;
        }
    }

  if (iram_addr != SKW_CP_IRAM || dram_addr != SKW_CP_DRAM)
    {
      wlerr("ERROR: bad fw head iram=0x%" PRIx32 " dram=0x%" PRIx32 "\n",
            iram_addr, dram_addr);
      return -EINVAL;
    }

  skw_cmd52(true, 0, SKW_REG_DMA_TYPE, 0x01, NULL);   /* ADMA */
  skw_cmd52(true, 0, SKW_REG_SLP, 0x01, NULL);        /* sleep disabled */

  /* Un-throttle every RX channel: the CP holds data for a port until its
   * flow-control bit is clear.
   */

  skw_cmd52(true, 0, SKW_REG_RX_FTL0, 0x00, NULL);
  skw_cmd52(true, 0, SKW_REG_RX_FTL1, 0x00, NULL);

  ret = skw_dt_stream(dram_addr, g_skw_board->dram, g_skw_board->dram_len);
  if (ret < 0)
    {
      return ret;
    }

  {
    uint8_t *patched = skw_nv_patch(iram, iram_len);

    ret = skw_dt_stream(iram_addr, patched != NULL ? patched : iram,
                        iram_len);
    if (patched != NULL)
      {
        kmm_free(patched);
      }
  }

  if (ret < 0)
    {
      return ret;
    }

  skw_cmd52(true, 0, SKW_REG_DL_DONE, 0x01, NULL);    /* boot the CP */
  return OK;
}

/****************************************************************************
 * Name: skw_bringup
 *
 * Description:
 *   Full power/enumeration/download/boot sequence up to the download-done
 *   signal.  The host controller must already be initialized and its clock
 *   configured to the ~800 kHz ID rate; the board power callback controls
 *   WL_REG_ON + companion pins.
 *
 ****************************************************************************/

static int skw_bringup(void)
{
  uint32_t resp = 0;
  uint32_t rint;
  uint32_t rca;
  uint8_t id[16];
  uint8_t v;
  int k;

  /* Power sequence: assert module reset, gate the clock, a long quiet
   * power-down, release reset into a quiet bus, 200 ms settle, then start
   * the ID-mode clock (~800 kHz).
   */

  g_skw_board->power(false);
  SDIO_CLOCK(g_skw_dev, CLOCK_SDIO_DISABLED);
  up_mdelay(1000);

  skw_wr(SKW_CLKENA, 0x00000000);
  skw_ciu_update(SKW_CLK_UPDATE);
  skw_wr(SKW_CLKDIV, 0x00000000);
  skw_wr(SKW_TIMING0, 0x0ffe0002);
  skw_wr(SKW_CRU_SDIO_SEL, (0x3fffu << 16) | 0x2f9d);

  g_skw_board->power(true);
  up_mdelay(200);
  skw_wr(SKW_CLKENA, 0x00000001);
  skw_ciu_update(SKW_CLK_UPDATE);
  up_mdelay(10);

  /* Neutralize the host driver ISR for the raw command phase: mask every
   * source and close the controller interrupt gate (it races our polled
   * RINTSTS handling).
   */

  /* Enable SDIO card-interrupt detection (INTMASK bit16) so the DW-MSHC
   * latches RINTSTS[16] when the CP asserts DAT1 to signal queued data.
   * CTRL.INT_ENABLE stays off: we poll RINTSTS rather than take a GIC IRQ.
   */

  skw_wr(SKW_INTMASK, 0x00000000);   /* all masked: no GIC IRQ line, RINTSTS still latches for polling */
  skw_wr(SKW_CTRL, skw_rd(SKW_CTRL) | (1u << 4));   /* INT_ENABLE on (golden CTRL=0x10): detect CP DAT1 SDIO interrupt */

  /* CMD5 with S18R (request 1.8 V), then the voltage switch if granted. */

  rint = skw_cmd(SKW_CMDW_CMD5, 0x01300000, &resp);
  if (rint & SKW_INT_RTO)
    {
      wlerr("ERROR: CMD5 no response\n");
      return -ENODEV;
    }

  if (resp & (1u << 24))
    {
      skw_voltage_switch();
    }

  /* CMD3 (get RCA) + CMD7 (select). */

  rint = skw_cmd(SKW_CMDW_CMD3, 0, &resp);
  rca = resp >> 16;
  if (rint & SKW_INT_RTO)
    {
      return -EIO;
    }

  rint = skw_cmd(SKW_CMDW_CMD7, rca << 16, &resp);
  if (rint & SKW_INT_RTO)
    {
      return -EIO;
    }

  /* CCCR: bus interface control (async int), IEN, 4-bit + SDR104 tune. */

  skw_cmd52(true, 0, 0x16, 0x03, NULL);
  skw_cmd52(true, 0, 0x04, 0x03, NULL);
  skw_tune_sdr104();

  /* FBR1 block size = 512, enable function 1, wait ready. */

  skw_cmd52(true, 0, 0x110, 0x00, NULL);
  skw_cmd52(true, 0, 0x111, 0x02, NULL);
  skw_cmd52(true, 0, 0x02, 0x02, NULL);
  for (k = 0; k < 100; k++)
    {
      v = 0;
      skw_cmd52(false, 0, 0x03, 0, &v);
      if (v & 0x02)
        {
          break;
        }

      up_mdelay(10);
    }

  if (!(v & 0x02))
    {
      wlerr("ERROR: func1 not ready\n");
      return -EIO;
    }

  /* DT chip-id sanity: "SV6160LITE". */

  skw_dt_latch(SKW_CP_CHIPID);
  memset(id, 0, sizeof(id));
  skw_cmd53_read(1, SKW_DT_WINDOW, true, id, 16);
  if (memcmp(id, "SV6160LITE", 10) != 0)
    {
      wlerr("ERROR: chip-id mismatch\n");
      return -ENODEV;
    }

  wlinfo("SV6160LITE detected, downloading firmware\n");
  return skw_download_and_boot();
}

/****************************************************************************
 * Name: skw_wait_conn_evt
 *
 * Description:
 *   Wait up to timeout_ms for a connection event; returns the event id or
 *   -ETIMEDOUT.
 *
 ****************************************************************************/

static int skw_wait_conn_evt(int timeout_ms)
{
  int t;

  for (t = 0; t < timeout_ms / 10; t++)
    {
      if (g_skw_conn_evt >= 0)
        {
          return g_skw_conn_evt;
        }

      up_mdelay(10);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: skw_connect
 *
 * Description:
 *   Associate with a scanned BSS: JOIN (channel/bssid/capability + the
 *   beacon IEs) -> AUTH (open) -> ASSOC (bssid + a minimal request IE),
 *   waiting for the ASOCC event.  Open networks associate outright; RSN
 *   networks reach ASSOC but need a host 4-way handshake for data (a
 *   wpa_supplicant integration, not yet present).
 *
 ****************************************************************************/

/****************************************************************************
 * Name: skw_bss_is_rsn
 *
 * Description:
 *   True if the BSS beacon advertised an RSN IE (tag 48) -> WPA2/WPA3.
 *
 ****************************************************************************/

static bool skw_bss_is_rsn(const struct skw_bss_s *bss)
{
  int off;

  for (off = 0; off + 2 <= bss->ie_len; )
    {
      uint8_t tag = bss->ie[off];
      uint8_t taglen = bss->ie[off + 1];

      if (tag == 48)
        {
          return true;
        }

      off += 2 + taglen;
    }

  return false;
}

static int skw_connect(const struct skw_bss_s *bss)
{
  uint8_t buf[320];
  int n;
  int evt;
  int ret;

  g_skw_conn_evt = -1;

  /* The reference driver sends SET_MIB(DOT11_MODE_HE = false) before JOIN
   * when the AP is not HE-capable, configuring the CP's PHY mode so it
   * forwards the AP's (non-HE) data frames to the host.  TLV payload:
   * plen(u16) + {type(u16), len(u16)} + value(u8).
   */

  {
    uint8_t mib[7] =
    {
      0x07, 0x00,   /* plen = 7 */
      0x10, 0x00,   /* type = 16 (SKW_MIB_DOT11_MODE_HE) */
      0x01, 0x00,   /* len  = 1 */
      0x00          /* value = false */
    };

    skw_send_cmd(SKW_CMD_SET_MIB, mib, sizeof(mib), NULL, NULL);
  }

  /* JOIN: skw_join_param + the beacon IEs in the tail.  Field offsets:
   *   0 chan_num, 1 center_chn1, 2 center_chn2, 3 bandwidth, 4 band,
   *   5 beacon_interval(u16), 7 capability(u16), 9 bssid_index,
   *   10 max_bssid_indicator, 11 bssid[6], 17 roaming:1/reserved(u16),
   *   19 bss_ie_offset(u16), 21 bss_ie_len(u32), 25 bss_ie[].
   */

  memset(buf, 0, sizeof(buf));
  buf[0] = bss->channel;                        /* chan_num */
  buf[1] = bss->channel;                        /* center_chn1 (20 MHz) */
  buf[3] = 0;                                   /* bandwidth 20 MHz */
  buf[4] = bss->band;                           /* band: 0=2.4 GHz, 1=5 GHz */
  buf[5] = bss->beacon_int & 0xff;              /* beacon_interval u16 */
  buf[6] = (bss->beacon_int >> 8) & 0xff;
  buf[7] = bss->capability & 0xff;              /* capability u16 */
  buf[8] = (bss->capability >> 8) & 0xff;
  memcpy(buf + 11, bss->bssid, 6);              /* bssid */
  memcpy(g_skw_bssid, bss->bssid, 6);

  {
    int fixed = 25;                             /* struct head length */
    int ielen = bss->ie_len;

    if (fixed + ielen > (int)sizeof(buf))
      {
        ielen = sizeof(buf) - fixed;
      }

    buf[19] = fixed & 0xff;                     /* bss_ie_offset u16 */
    buf[20] = (fixed >> 8) & 0xff;
    buf[21] = ielen & 0xff;                     /* bss_ie_len u32 */
    buf[22] = (ielen >> 8) & 0xff;
    memcpy(buf + fixed, bss->ie, ielen);
    n = fixed + ielen;
  }

  {
    uint8_t jresp[8];
    int jrlen = sizeof(jresp);

    ret = skw_send_cmd(SKW_CMD_JOIN, buf, n, jresp, &jrlen);
    if (ret < 0)
      {
        wlerr("ERROR: JOIN failed: %d\n", ret);
        return ret;
      }

    /* JOIN ACK returns skw_join_resp {peer_idx,lmac_id,inst,mcast_idx}. */

    g_skw_peer_idx = (jrlen > 0) ? jresp[0] : 0;
    g_skw_lmac = (jrlen > 1) ? (jresp[1] & 0x3) : 0;
    g_skw_mcast = (jrlen > 3) ? jresp[3] : 0;
    syslog(LOG_ERR, "SKW: JOIN resp len=%d %02x %02x %02x %02x "
           "peer_idx=%d lmac=%d\n", jrlen,
           jrlen > 0 ? jresp[0] : 0, jrlen > 1 ? jresp[1] : 0,
           jrlen > 2 ? jresp[2] : 0, jrlen > 3 ? jresp[3] : 0,
           g_skw_peer_idx, g_skw_lmac);
  }

  up_mdelay(50);

  /* AUTH: open (algorithm 0), no key/auth data. */

  memset(buf, 0, sizeof(buf));
  buf[0] = SKW_AUTH_OPEN & 0xff;                /* auth_algorithm u16 */
  ret = skw_send_cmd(SKW_CMD_AUTH, buf, 14, NULL, NULL);
  if (ret < 0)
    {
      wlerr("ERROR: AUTH failed: %d\n", ret);
      return ret;
    }

  up_mdelay(100);
  g_skw_conn_evt = -1;

  /* ASSOC: ht_capa + vht_capa (zeroed) + bssid + pre_bssid + req_ie.  The
   * request IE = supported-rates (tag 1) and, for an RSN AP, an RSN IE
   * (tag 48) advertising WPA2-PSK / CCMP so the AP starts the 4-way
   * handshake (which the host supplicant completes -- W2).
   */

  memset(buf, 0, sizeof(buf));
  {
    /* struct: ieee80211_ht_cap(26) + ieee80211_vht_cap(12) + bssid(6) +
     * pre_bssid(6) + req_ie_offset(2) + req_ie_len(2) + req_ie[].
     */

    /* ieee80211_ht_cap: cap_info(2) + ampdu(1) + mcs(16) + ext(2) +
     * txbf(4) + asel(1).  2.4 GHz 20/40, SGI, SM-PS disabled, 1 stream
     * (MCS0-7).  Advertising HT enables the AP's QoS data path.
     */

    static const uint8_t htcap[26] =
    {
      0x6e, 0x00,             /* cap_info: 40MHz, SM-PS off, SGI20/40 */
      0x17,                   /* A-MPDU: 64k, 4us density */
      0xff, 0x00, 0x00, 0x00, /* MCS rx_mask MCS0-7 */
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00,       /* rx_highest + tx_params */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00  /* ext + txbf + asel */
    };

    memcpy(buf, htcap, sizeof(htcap));

    int off_bssid = 26 + 12;
    int off_ieoff = off_bssid + 12;
    int req_off   = off_ieoff + 4;
    int ie = req_off;
    static const uint8_t rates[] =
      { 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c };

    /* WPA2-PSK/CCMP RSN IE: group CCMP, 1 pairwise CCMP, 1 AKM PSK. */

    static const uint8_t rsn_ie[] =
      {
        0x30, 0x14, 0x01, 0x00,               /* RSN, len 20, ver 1 */
        0x00, 0x0f, 0xac, 0x04,               /* group cipher CCMP */
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,   /* 1 pairwise CCMP */
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x02,   /* 1 AKM PSK */
        0x00, 0x00                            /* RSN capabilities */
      };

    memcpy(buf + off_bssid, bss->bssid, 6);

    memcpy(buf + ie, rates, sizeof(rates));
    ie += sizeof(rates);

    if (skw_bss_is_rsn(bss))
      {
        memcpy(buf + ie, rsn_ie, sizeof(rsn_ie));
        ie += sizeof(rsn_ie);
      }

    buf[off_ieoff]     = req_off & 0xff;
    buf[off_ieoff + 1] = (req_off >> 8) & 0xff;
    buf[off_ieoff + 2] = (ie - req_off) & 0xff;
    buf[off_ieoff + 3] = ((ie - req_off) >> 8) & 0xff;
    n = ie;
  }

  ret = skw_send_cmd(SKW_CMD_ASSOC, buf, n, NULL, NULL);
  if (ret < 0)
    {
      wlerr("ERROR: ASSOC failed: %d\n", ret);
      return ret;
    }

  /* Wait for the association result (assoc-resp via RX_MGMT). */

  evt = skw_wait_conn_evt(3000);
  syslog(LOG_INFO, "SKW: connect result = %d (%s)\n", evt,
         evt == SKW_CONN_ASSOC_OK ? "ASSOCIATED" :
         evt == SKW_CONN_ASSOC_FAIL ? "rejected" : "timeout");

  return (evt == SKW_CONN_ASSOC_OK) ? OK : -EIO;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_skw_initialize(const struct rk3576_skw_board_s *board)
{
  int ret;

  DEBUGASSERT(board != NULL && board->power != NULL);
  DEBUGASSERT(board->iram != NULL && board->dram != NULL);

  /* Idempotent: a second call (e.g. re-running the test command) must not
   * power-cycle the chip or spawn a second rx thread on top of the live
   * one -- that double-drives the SDIO bus and hangs the board.
   */

  if (g_skw_run)
    {
      return (g_skw_service & RK3576_SKW_STATE_WIFI) ? OK : -EBUSY;
    }

  g_skw_board = board;

  g_skw_dev = rk3576_sdmmc_initialize(RK3576_SDIO_SLOT);
  if (g_skw_dev == NULL)
    {
      wlerr("ERROR: SDIO host init failed\n");
      return -ENODEV;
    }

  ret = skw_bringup();
  if (ret < 0)
    {
      return ret;
    }

  nxsem_init(&g_skw_cmd.done, 0, 0);

  g_skw_run = true;
  ret = kthread_create("skw_rx", CONFIG_RK3576_SKW_RXPRIO,
                       CONFIG_RK3576_SKW_RXSTACK, skw_rx_thread, NULL);
  if (ret < 0)
    {
      g_skw_run = false;
      wlerr("ERROR: rx thread create failed: %d\n", ret);
      return ret;
    }

  /* Wait up to 2 s for the WiFi service to come ready. */

  for (ret = 0; ret < 200; ret++)
    {
      if (g_skw_service & RK3576_SKW_STATE_WIFI)
        {
          break;
        }

      up_mdelay(10);
    }

  if (!(g_skw_service & RK3576_SKW_STATE_WIFI))
    {
      wlwarn("WARNING: WiFi service not ready (state=0x%" PRIx32 ")\n",
             g_skw_service);
      return -ETIMEDOUT;
    }

  /* WiFi service up: run the post-ready command bring-up. */

  ret = skw_wifi_bringup_cmds();
  return ret;
}

uint32_t rk3576_skw_state(void)
{
  return g_skw_service;
}

int rk3576_skw_scan(struct rk3576_skw_bss_s *list, int max)
{
  int n;
  int i;

  if (!(g_skw_service & RK3576_SKW_STATE_WIFI))
    {
      return -ENODEV;
    }

  n = skw_scan();
  if (n < 0)
    {
      return n;
    }

  if (list != NULL)
    {
      int cnt = (n < max) ? n : max;

      for (i = 0; i < cnt; i++)
        {
          memcpy(list[i].bssid, g_skw_scan[i].bssid, 6);
          memcpy(list[i].ssid, g_skw_scan[i].ssid, 33);
          list[i].ssid_len = g_skw_scan[i].ssid_len;
          list[i].channel  = g_skw_scan[i].channel;
          list[i].band     = g_skw_scan[i].band;
          list[i].rssi     = g_skw_scan[i].rssi;
        }
    }

  return n;
}

int rk3576_skw_connect(const char *ssid)
{
  int i;
  int pass;

  if (!(g_skw_service & RK3576_SKW_STATE_WIFI) || ssid == NULL)
    {
      return -ENODEV;
    }

  /* Look up the target; scan once if it is not cached yet. */

  for (pass = 0; pass < 2; pass++)
    {
      for (i = 0; i < g_skw_scan_n; i++)
        {
          if (g_skw_scan[i].ssid_len == (int)strlen(ssid) &&
              memcmp(g_skw_scan[i].ssid, ssid,
                     g_skw_scan[i].ssid_len) == 0)
            {
              return skw_connect(&g_skw_scan[i]);
            }
        }

      if (pass == 0)
        {
          skw_scan();
        }
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: rk3576_skw_get_mac / rk3576_skw_get_bssid
 ****************************************************************************/

void rk3576_skw_get_mac(uint8_t mac[6])
{
  memcpy(mac, g_skw_mac, 6);
}

void rk3576_skw_get_bssid(uint8_t bssid[6])
{
  memcpy(bssid, g_skw_bssid, 6);
}

/****************************************************************************
 * Name: rk3576_skw_send_control
 ****************************************************************************/

int rk3576_skw_send_control(uint8_t id, const uint8_t *payload, int length)
{
  return skw_send_cmd(id, payload, length, NULL, NULL);
}

/***************************************************************************
 * Name: rk3576_skw_data_tx
 *
 * Description:
 *   Transmit a bare Ethernet frame on the data channel (7).  Builds the
 *   channel-7 packet: outer header + skw_tx_desc_hdr(6) + skw_tx_desc_conf
 *   (2) + Ethernet frame + trailing eof terminator.  Best-effort TX credit
 *   handling (read 0x184, write consumed to 0x168); the CP ignores credit
 *   when SKW_FLAG_FW_IGNORE_CRED is set.
 *
 ****************************************************************************/

int rk3576_skw_data_tx(const uint8_t *eth, int ethlen)
{
  int inner = 6 + 2 + ethlen;                  /* desc_hdr + conf + frame */
  int pkt_len = 4 + inner + 4;                 /* outer hdr + inner + eof */
  int padded = (pkt_len + 511) & ~511;

  uint32_t hdr;
  uint32_t eof;
  uint16_t w0;
  uint16_t w1;
  uint8_t credit = 0;
  int ret;

  if (ethlen < 14 || padded > (int)sizeof(g_skw_txbuf))
    {
      return -E2BIG;
    }

  ret = nxmutex_lock(&g_skw_tx_lock);
  if (ret < 0)
    {
      return ret;
    }

  /* Best-effort credit check (func0 SIG registers). */

  skw_cmd52(false, 0, 0x184, 0, &credit);

  memset(g_skw_txbuf, 0, padded);

  hdr = SKW_HDR(SKW_CH_WIFI_DATA, inner);
  g_skw_txbuf[0] = hdr & 0xff;
  g_skw_txbuf[1] = (hdr >> 8) & 0xff;
  g_skw_txbuf[2] = (hdr >> 16) & 0xff;
  g_skw_txbuf[3] = (hdr >> 24) & 0xff;

  /* skw_tx_desc_hdr word0: padding_gap:2 inst:2 tid:4 peer_lut:5
   * frame_type:1 encry_dis:1 rate:1.  STA data: peer_lut = peer_idx,
   * everything else 0 (frame_type SKW_ETHER_FRAME, encry enabled).
   */

  {
    uint8_t lut = (eth[0] & 0x01) ? g_skw_mcast : g_skw_peer_idx;
    w0 = ((uint16_t)(lut & 0x1f)) << 8;
  }

  /* encry_dis stays 0 (matches the reference driver): the CP sends the
   * frame in the clear until the pairwise key is installed.
   */

  g_skw_txbuf[4] = w0 & 0xff;
  g_skw_txbuf[5] = (w0 >> 8) & 0xff;

  /* word1: msdu_len:12 lmac_id:2 rsv:2 */

  w1 = (ethlen & 0x0fff) | ((uint16_t)(g_skw_lmac & 0x3) << 12);
  g_skw_txbuf[6] = w1 & 0xff;
  g_skw_txbuf[7] = (w1 >> 8) & 0xff;

  /* word2: eth_type.  The reference driver stores eth->h_proto verbatim
   * (network byte order), so copy the frame's EtherType bytes as-is.
   */

  g_skw_txbuf[8] = eth[12];
  g_skw_txbuf[9] = eth[13];

  /* skw_tx_desc_conf (2B): checksum offload off. */

  g_skw_txbuf[10] = 0;
  g_skw_txbuf[11] = 0;

  memcpy(g_skw_txbuf + 12, eth, ethlen);

  /* Match the reference driver TX filter: only EAPOL and DHCP (UDP ports
   * 67/68) use the command engine (cmd15); all other data frames use the
   * ch7 credit-gated data path.
   */

  {
    uint16_t et = ((uint16_t)eth[12] << 8) | eth[13];
    bool filter = (et == 0x888e);

    if (filter)
      {
        /* Pre-auth EAPOL goes on the command engine (cmd15): the CP uses
         * eth_type=0x888e in the descriptor to egress it unencrypted.
         * Copy the descriptor and frame to the stack before releasing the
         * shared data buffer.
         */

        {
          uint8_t eapbuf[256];
          int cp = inner < (int)sizeof(eapbuf) ?
                   inner : (int)sizeof(eapbuf);

          memcpy(eapbuf, g_skw_txbuf + 4, cp);
          nxmutex_unlock(&g_skw_tx_lock);
          return skw_send_cmd(SKW_CMD_TX_DATA_FRAME, eapbuf, cp,
                              NULL, NULL);
        }
      }
  }

  eof = SKW_HDR_EOF_TERM;
  g_skw_txbuf[4 + inner + 0] = eof & 0xff;
  g_skw_txbuf[4 + inner + 1] = (eof >> 8) & 0xff;
  g_skw_txbuf[4 + inner + 2] = (eof >> 16) & 0xff;
  g_skw_txbuf[4 + inner + 3] = (eof >> 24) & 0xff;

  /* Return one consumed credit to the CP (best-effort). */

  if (credit > 0)
    {
      skw_cmd52(true, 0, 0x168, 1, NULL);
    }

  ret = skw_cmd53_write(1, SKW_PK_WINDOW, false, g_skw_txbuf, padded);

  /* Ring the AP->CP doorbell so the CP services the queued data
   * frame (same mechanism the command path uses).
   */

  if (ret >= 0)
    {
      ret = skw_cmd52(true, 0, SKW_REG_AP2CP_IRQ, 0x01, NULL);
    }

  nxmutex_unlock(&g_skw_tx_lock);
  return ret;
}

/***************************************************************************
 * Name: rk3576_skw_add_key
 *
 * Description:
 *   Install a key into the CP via ADD_KEY(12).  Builds skw_key_params:
 *   mac_addr[6], key_type, cipher_type, pn[6], key_id, key_len, key[].
 *
 ****************************************************************************/

int rk3576_skw_add_key(uint8_t key_type, uint8_t cipher,
                       const uint8_t *mac, uint8_t key_id,
                       const uint8_t *key, int key_len, const uint8_t *pn)
{
  uint8_t buf[64];
  int n = 0;

  if (key_len < 0 || 16 + key_len > (int)sizeof(buf))
    {
      return -E2BIG;
    }

  memcpy(buf + n, mac, 6);
  n += 6;
  buf[n++] = key_type;
  buf[n++] = cipher;
  if (pn != NULL)
    {
      memcpy(buf + n, pn, 6);
    }
  else
    {
      memset(buf + n, 0, 6);
    }

  n += 6;
  buf[n++] = key_id;
  buf[n++] = (uint8_t)key_len;
  memcpy(buf + n, key, key_len);
  n += key_len;

  return skw_send_cmd(SKW_CMD_ADD_KEY, buf, n, NULL, NULL);
}
