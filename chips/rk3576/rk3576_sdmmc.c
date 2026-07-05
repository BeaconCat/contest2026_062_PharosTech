/***************************************************************************
 * arch/arm64/src/rk3576/rk3576_sdmmc.c
 *
 * RK3576 SDMMC (SD 卡) host 控制器驱动 —— Synopsys DesignWare MSHC
 * (dw_mmc)，与 rk3288/rk3399 同一 IP。实现 NuttX 的 struct sdio_dev_s。
 *
 * 第一版设计约束：
 *   1. 数据传输走 PIO(FIFO)，不做 DMA/IDMAC。
 *   2. 时钟/引脚由 bootloader 已初始化（板子从 SDMMC 启动），本驱动不写
 *      CRU/pinctrl/GPIO，只做控制器复位 + CLKDIV/CLKENA 设置。
 *   3. 卡检测用控制器自带 CDETECT 寄存器（bit0=0 表示有卡）。
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
 ***************************************************************************/

/***************************************************************************
 * Included Files
 ***************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/spinlock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/wdog.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>
#include <nuttx/sdio.h>
#include <nuttx/mmcsd.h>
#include <nuttx/wqueue.h>
#include <nuttx/cache.h>

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "chip.h"
#include "hardware/rk3576_memorymap.h"
#include "hardware/rk3576_sdmmc.h"

#ifdef CONFIG_RK3576_SDMMC

/***************************************************************************
 * Pre-processor Definitions
 ***************************************************************************/

/* 基础时钟频率（bootloader 已配 ciu 时钟源）。RK dw-mshc 常用 50MHz，本值
 * 仅用于计算 CLKDIV 分频，实际频率以 loader 为准。★ TODO：接 CRU 后按真实
 * ciu_clk 取值。
 */

#define RK3576_SDMMC_CLKIN      50000000

/* 各阶段目标卡时钟 */

#define RK3576_SDMMC_ID_FREQ    400000     /* ID 模式 <400KHz */
#define RK3576_SDMMC_MMC_FREQ   25000000   /* 普通传输 */
#define RK3576_SDMMC_SD1_FREQ   25000000   /* SD 1-bit */
#define RK3576_SDMMC_SD4_FREQ   50000000   /* SD 4-bit */

/* FIFO 深度（HCON[10:7]*2）。RK3576 dw-mshc 一般 256×32bit，这里保守用
 * STATUS 的 FIFO_COUNT 动态判断，阈值仅用于半满触发。
 */

#define RK3576_SDMMC_FIFO_DEPTH 256
#define RK3576_SDMMC_FIFO_HALF  (RK3576_SDMMC_FIFO_DEPTH / 2)

/* IDMAC 描述符表项数：256 项 x 4KB = 1MB 单次 DMA 传输上限 */

#define RK3576_IDMAC_NDESC      256

/* 轮询超时（忙等循环上限） */

#define RK3576_SDMMC_SPIN       1000000

/* 命令/数据中断组合 */

#define SDMMC_CMDDONE_INTS      (SDMMC_INT_CMD_DONE)
#define SDMMC_RESPERR_INTS      (SDMMC_INT_RE | SDMMC_INT_RCRC | SDMMC_INT_RTO)
#define SDMMC_XFRDONE_INTS      (SDMMC_INT_DTO)
#define SDMMC_DATAERR_INTS      (SDMMC_INT_DCRC | SDMMC_INT_DRTO | \
                                 SDMMC_INT_SBE | SDMMC_INT_EBE | \
                                 SDMMC_INT_FRUN | SDMMC_INT_HLE)

/***************************************************************************
 * Private Types
 ***************************************************************************/

/* RK3576 SDMMC host 私有状态。首成员是 sdio_dev_s，便于向上转型。 */

struct rk3576_dev_s
{
  struct sdio_dev_s  dev;        /* 标准 SDIO 接口（必须是首成员） */

  uintptr_t          base;       /* 控制器寄存器基址 */
  int                irq;        /* 控制器中断号 */

  /* 事件等待支持 */

  sem_t              waitsem;    /* 等待中断事件的信号量 */
  struct wdog_s      waitwdog;   /* 等待超时看门狗 */
  volatile sdio_eventset_t waitevents;  /* 使能的等待事件集 */
  volatile sdio_eventset_t wkupevent;   /* 唤醒事件集 */

  /* 媒体变化回调 */

  sdio_eventset_t    cbevents;   /* 使能的回调事件集 */
  worker_t           callback;   /* 注册的回调函数 */
  void              *cbarg;      /* 回调参数 */
  struct work_s      cbwork;     /* 回调 work 队列项 */

  /* 数据传输（PIO）状态 */

  uint32_t          *buffer;     /* 传输缓冲区（32bit 对齐） */
  volatile size_t    remaining;  /* 剩余字节数 */
  volatile uint32_t  xfrints;    /* 数据传输期间使能的中断 */
  bool               widebus;    /* 4-bit 总线 */

#ifdef CONFIG_SDIO_DMA
  /* IDMAC(内部 DMA)状态 */

  volatile bool      dmamode;    /* 当前传输走 IDMAC */
  bool               dmaread;    /* DMA 方向：true=读(DMA 写内存) */
  uintptr_t          dmabuf;     /* DMA 缓冲区起址 */
  size_t             dmalen;     /* DMA 缓冲区长度 */
  struct rk3576_idmac_desc_s *descs;  /* 描述符表(指向静态对齐数组) */
#endif
  uint32_t           blocksize;  /* 当前块大小(由 blocksetup 记录) */
};

/***************************************************************************
 * Private Function Prototypes
 ***************************************************************************/

/* 寄存器访问 */

static inline uint32_t rk3576_getreg(struct rk3576_dev_s *priv,
                                     unsigned int offset);
static inline void rk3576_putreg(struct rk3576_dev_s *priv,
                                 unsigned int offset, uint32_t value);

/* 低层辅助 */

static int  rk3576_ciu_update(struct rk3576_dev_s *priv, uint32_t cmdflags);
static void rk3576_setclock(struct rk3576_dev_s *priv, uint32_t freq);
static void rk3576_configwaitints(struct rk3576_dev_s *priv,
                                  uint32_t waitints,
                                  sdio_eventset_t waitevents,
                                  sdio_eventset_t wkupevent);
static void rk3576_configxfrints(struct rk3576_dev_s *priv,
                                 uint32_t xfrints);
static void rk3576_endwait(struct rk3576_dev_s *priv,
                           sdio_eventset_t wkupevent);
static void rk3576_eventtimeout(wdparm_t arg);
static void rk3576_recvfifo(struct rk3576_dev_s *priv);
static void rk3576_sendfifo(struct rk3576_dev_s *priv);
static void rk3576_callback(struct rk3576_dev_s *priv);

#ifdef CONFIG_SDIO_DMA
static void rk3576_dma_disable(struct rk3576_dev_s *priv);
static int  rk3576_dma_build(struct rk3576_dev_s *priv,
                             const uint8_t *buffer, size_t buflen);
static int  rk3576_dma_common(struct rk3576_dev_s *priv,
                              const uint8_t *buffer, size_t buflen,
                              bool write);
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
static int  rk3576_dmapreflight(struct sdio_dev_s *dev,
                                const uint8_t *buffer, size_t buflen);
#endif
static int  rk3576_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                size_t buflen);
static int  rk3576_dmasendsetup(struct sdio_dev_s *dev,
                                const uint8_t *buffer, size_t buflen);
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
static void rk3576_blocksetup(struct sdio_dev_s *dev,
                              unsigned int blocklen, unsigned int nblocks);
#endif

/* 中断处理 */

static int  rk3576_interrupt(int irq, void *context, void *arg);

/* sdio_dev_s 方法 */

static void rk3576_reset(struct sdio_dev_s *dev);
static sdio_capset_t rk3576_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t rk3576_status(struct sdio_dev_s *dev);
static void rk3576_widebus(struct sdio_dev_s *dev, bool enable);
static void rk3576_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int  rk3576_attach(struct sdio_dev_s *dev);

static int  rk3576_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t arg);
static int  rk3576_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                             size_t nbytes);
static int  rk3576_sendsetup(struct sdio_dev_s *dev,
                             const uint8_t *buffer, size_t nbytes);
static int  rk3576_cancel(struct sdio_dev_s *dev);

static int  rk3576_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int  rk3576_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                             uint32_t *rshort);
static int  rk3576_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t rlong[4]);

static void rk3576_waitenable(struct sdio_dev_s *dev,
                              sdio_eventset_t eventset, uint32_t timeout);
static sdio_eventset_t rk3576_eventwait(struct sdio_dev_s *dev);
static void rk3576_callbackenable(struct sdio_dev_s *dev,
                                  sdio_eventset_t eventset);
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
static int  rk3576_registercallback(struct sdio_dev_s *dev,
                                    worker_t callback, void *arg);
#endif
static void rk3576_gotextcsd(struct sdio_dev_s *dev,
                             const uint8_t *buffer);

/***************************************************************************
 * Private Data
 ***************************************************************************/

#ifdef CONFIG_SDIO_DMA
/* IDMAC 描述符表：cache line(64B)对齐，CPU 与 DMA 共享 */

static struct rk3576_idmac_desc_s
  g_idmac_descs[RK3576_IDMAC_NDESC] aligned_data(64);
#endif

/* 单实例：RK3576 只挂 SD 卡槽（SDMMC0）。 */

static struct rk3576_dev_s g_sdmmcdev =
{
  .dev =
  {
    .reset           = rk3576_reset,
    .capabilities    = rk3576_capabilities,
    .status          = rk3576_status,
    .widebus         = rk3576_widebus,
    .clock           = rk3576_clock,
    .attach          = rk3576_attach,
    .sendcmd         = rk3576_sendcmd,
    .recvsetup       = rk3576_recvsetup,
    .sendsetup       = rk3576_sendsetup,
    .cancel          = rk3576_cancel,
    .waitresponse    = rk3576_waitresponse,
    .recv_r1         = rk3576_recvshort,
    .recv_r2         = rk3576_recvlong,
    .recv_r3         = rk3576_recvshort,
    .recv_r4         = rk3576_recvshort,
    .recv_r5         = rk3576_recvshort,
    .recv_r6         = rk3576_recvshort,
    .recv_r7         = rk3576_recvshort,
    .waitenable      = rk3576_waitenable,
    .eventwait       = rk3576_eventwait,
    .callbackenable  = rk3576_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
    .registercallback = rk3576_registercallback,
#endif
    .gotextcsd       = rk3576_gotextcsd,
#ifdef CONFIG_SDIO_DMA
#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
    .dmapreflight    = rk3576_dmapreflight,
#endif
    .dmarecvsetup    = rk3576_dmarecvsetup,
    .dmasendsetup    = rk3576_dmasendsetup,
#endif
#ifdef CONFIG_SDIO_BLOCKSETUP
    .blocksetup      = rk3576_blocksetup,
#endif
  },
  .base = RK3576_SDMMC_ADDR,
  .irq  = RK3576_IRQ_SDMMC,
};

/***************************************************************************
 * Private Functions
 ***************************************************************************/

/***************************************************************************
 * Name: rk3576_getreg / rk3576_putreg
 *
 * Description:
 *   控制器寄存器读写辅助。
 ***************************************************************************/

static inline uint32_t rk3576_getreg(struct rk3576_dev_s *priv,
                                     unsigned int offset)
{
  return getreg32(priv->base + offset);
}

static inline void rk3576_putreg(struct rk3576_dev_s *priv,
                                 unsigned int offset, uint32_t value)
{
  putreg32(value, priv->base + offset);
}

/***************************************************************************
 * Name: rk3576_ciu_update
 *
 * Description:
 *   下发一条"仅更新时钟"命令（SDMMC_CMD_UPD_CLK），并等待 CMD_START 位自清。
 *   dw-mshc 修改 CLKDIV/CLKENA 后必须发此命令时钟才真正生效。
 ***************************************************************************/

static int rk3576_ciu_update(struct rk3576_dev_s *priv, uint32_t cmdflags)
{
  uint32_t cmd = SDMMC_CMD_START | SDMMC_CMD_UPD_CLK |
                 SDMMC_CMD_WAIT_PRV | cmdflags;
  int i;

  rk3576_putreg(priv, RK3576_SDMMC_CMD, cmd);

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_CMD) & SDMMC_CMD_START) == 0)
        {
          return OK;
        }
    }

  mcerr("ERROR: CIU 更新时钟超时\n");
  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_setclock
 *
 * Description:
 *   设置卡时钟频率。先关卡时钟 -> 改 CLKDIV -> 开卡时钟，每步发 UPD_CLK。
 *   dw-mshc 实际卡时钟 = ciu_clk / (2 * CLKDIV)（CLKDIV=0 表示直通）。
 ***************************************************************************/

static void rk3576_setclock(struct rk3576_dev_s *priv, uint32_t freq)
{
  uint32_t div;

  /* 1) 关卡时钟 */

  rk3576_putreg(priv, RK3576_SDMMC_CLKENA, 0);
  rk3576_ciu_update(priv, 0);

  if (freq == 0)
    {
      return;
    }

  /* 2) 计算分频：div = ceil(clkin / (2*freq))，0 表示直通(=clkin) */

  if (freq >= RK3576_SDMMC_CLKIN)
    {
      div = 0;
    }
  else
    {
      div = (RK3576_SDMMC_CLKIN + (2 * freq) - 1) / (2 * freq);
      if (div > 0xff)
        {
          div = 0xff;
        }
    }

  rk3576_putreg(priv, RK3576_SDMMC_CLKDIV, div);
  rk3576_ciu_update(priv, 0);

  /* 3) 开卡时钟（低功耗位：空闲自动停时钟） */

  rk3576_putreg(priv, RK3576_SDMMC_CLKENA,
                SDMMC_CLKENA_ENABLE | SDMMC_CLKENA_LOWPWR);
  rk3576_ciu_update(priv, 0);
}

/***************************************************************************
 * Name: rk3576_configwaitints
 *
 * Description:
 *   原子地配置等待事件集并使能对应的硬件中断。
 ***************************************************************************/

static void rk3576_configwaitints(struct rk3576_dev_s *priv,
                                  uint32_t waitints,
                                  sdio_eventset_t waitevents,
                                  sdio_eventset_t wkupevent)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->waitevents = waitevents;
  priv->wkupevent  = wkupevent;

  /* 合入数据传输中断，一起写 INTMASK */

  rk3576_putreg(priv, RK3576_SDMMC_INTMASK, priv->xfrints | waitints);
  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_configxfrints
 *
 * Description:
 *   配置数据传输期间的硬件中断。
 ***************************************************************************/

static void rk3576_configxfrints(struct rk3576_dev_s *priv,
                                 uint32_t xfrints)
{
  irqstate_t flags;

  flags = enter_critical_section();
  priv->xfrints = xfrints;

  /* 保留当前等待中断（由 waitevents 推出较繁琐，这里合并写全集）：
   * 简化处理——传输期间等待中断由 configwaitints 设置，这里只叠加 xfrints。
   */

  rk3576_putreg(priv, RK3576_SDMMC_INTMASK,
                rk3576_getreg(priv, RK3576_SDMMC_INTMASK) | xfrints);
  if (xfrints == 0)
    {
      /* 关闭数据传输中断（保留 SDIO 卡中断等其它位不在此处处理） */

      rk3576_putreg(priv, RK3576_SDMMC_INTMASK,
                    rk3576_getreg(priv, RK3576_SDMMC_INTMASK) &
                    ~(SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS |
                      SDMMC_INT_TXDR | SDMMC_INT_RXDR));
    }

  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_endwait
 *
 * Description:
 *   在中断上下文结束一次事件等待：停看门狗、关中断、唤醒等待线程。
 ***************************************************************************/

static void rk3576_endwait(struct rk3576_dev_s *priv,
                           sdio_eventset_t wkupevent)
{
  wd_cancel(&priv->waitwdog);

  /* 关掉本次等待关注的中断，记录唤醒事件 */

  rk3576_configwaitints(priv, 0, 0, wkupevent);
  nxsem_post(&priv->waitsem);
}

/***************************************************************************
 * Name: rk3576_eventtimeout
 *
 * Description:
 *   等待超时看门狗回调（软件超时）。
 ***************************************************************************/

static void rk3576_eventtimeout(wdparm_t arg)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)arg;

  if ((priv->waitevents & SDIOWAIT_TIMEOUT) != 0)
    {
      rk3576_endwait(priv, SDIOWAIT_TIMEOUT);
      mcerr("ERROR: 等待事件超时\n");
    }
}

/***************************************************************************
 * Name: rk3576_recvfifo
 *
 * Description:
 *   从 FIFO 读数据到接收缓冲区（PIO），直到 FIFO 空或缓冲区满。
 ***************************************************************************/

static void rk3576_recvfifo(struct rk3576_dev_s *priv)
{
  while (priv->remaining >= sizeof(uint32_t) &&
         (rk3576_getreg(priv, RK3576_SDMMC_STATUS) &
          SDMMC_STATUS_FIFO_EMPTY) == 0)
    {
      *priv->buffer++  = rk3576_getreg(priv, RK3576_SDMMC_DATA);
      priv->remaining -= sizeof(uint32_t);
    }
}

/***************************************************************************
 * Name: rk3576_sendfifo
 *
 * Description:
 *   从发送缓冲区写数据到 FIFO（PIO），直到 FIFO 满或缓冲区空。
 ***************************************************************************/

static void rk3576_sendfifo(struct rk3576_dev_s *priv)
{
  while (priv->remaining >= sizeof(uint32_t) &&
         (rk3576_getreg(priv, RK3576_SDMMC_STATUS) &
          SDMMC_STATUS_FIFO_FULL) == 0)
    {
      rk3576_putreg(priv, RK3576_SDMMC_DATA, *priv->buffer++);
      priv->remaining -= sizeof(uint32_t);
    }
}

/***************************************************************************
 * Name: rk3576_callback
 *
 * Description:
 *   若满足条件，调度媒体变化回调到工作队列执行。
 ***************************************************************************/

static void rk3576_callback(struct rk3576_dev_s *priv)
{
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
  if (priv->callback)
    {
      sdio_statset_t status = rk3576_status(&priv->dev);
      bool present = (status & SDIO_STATUS_PRESENT) != 0;

      if (( present && (priv->cbevents & SDIOMEDIA_INSERTED) != 0) ||
          (!present && (priv->cbevents & SDIOMEDIA_EJECTED)  != 0))
        {
          priv->cbevents = 0;
          work_queue(HPWORK, &priv->cbwork, priv->callback, priv->cbarg, 0);
        }
    }
#endif
}

/***************************************************************************
 * Name: rk3576_interrupt
 *
 * Description:
 *   控制器中断处理：处理命令完成/响应错误、PIO 数据搬运、传输完成/错误、
 *   卡检测。
 ***************************************************************************/

static int rk3576_interrupt(int irq, void *context, void *arg)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)arg;
  uint32_t pending;
  uint32_t enabled;

  /* 读被使能的中断状态（RINTSTS & INTMASK） */

  enabled = rk3576_getreg(priv, RK3576_SDMMC_INTMASK);
  pending = rk3576_getreg(priv, RK3576_SDMMC_RINTSTS) & enabled;

  /* 写 1 清除已读到的状态位 */

  rk3576_putreg(priv, RK3576_SDMMC_RINTSTS, pending);

#ifdef CONFIG_SDIO_DMA
  /* --- IDMAC 状态：处理 DMA 错误(收/发完成靠下面 DTO 收口) --- */

  if (priv->dmamode)
    {
      uint32_t idsts = rk3576_getreg(priv, RK3576_SDMMC_IDSTS);

      rk3576_putreg(priv, RK3576_SDMMC_IDSTS,
                    idsts & SDMMC_IDMAC_INT_ALL);

      if ((idsts & SDMMC_IDMAC_INT_ERR) != 0)
        {
          rk3576_dma_disable(priv);
          priv->remaining = 0;
          priv->buffer    = NULL;
          rk3576_configxfrints(priv, 0);

          if ((priv->waitevents &
               (SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR)) != 0)
            {
              rk3576_endwait(priv, SDIOWAIT_ERROR);
            }
        }
    }
#endif

  /* --- PIO 数据搬运 --- */

  if ((pending & SDMMC_INT_RXDR) != 0 && priv->buffer != NULL)
    {
      rk3576_recvfifo(priv);
    }

  if ((pending & SDMMC_INT_TXDR) != 0 && priv->buffer != NULL)
    {
      rk3576_sendfifo(priv);
    }

  /* --- 数据传输错误 --- */

  if ((pending & SDMMC_DATAERR_INTS) != 0)
    {
#ifdef CONFIG_SDIO_DMA
      if (priv->dmamode)
        {
          rk3576_dma_disable(priv);
        }
#endif
      priv->remaining = 0;
      priv->buffer    = NULL;
      rk3576_configxfrints(priv, 0);

      if ((priv->waitevents & (SDIOWAIT_TRANSFERDONE | SDIOWAIT_ERROR)) != 0)
        {
          rk3576_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- 数据传输完成 --- */

  else if ((pending & SDMMC_INT_DTO) != 0)
    {
#ifdef CONFIG_SDIO_DMA
      if (priv->dmamode)
        {
          /* ★ DMA 读：DTO 后再 invalidate 一次，确保 CPU 读到 DMA 写入内存
           * 的新数据(而非旧的 cache 行)。
           */

          if (priv->dmaread)
            {
              up_invalidate_dcache(priv->dmabuf,
                                   priv->dmabuf + priv->dmalen);
            }

          rk3576_dma_disable(priv);
        }
#endif
      /* 收尾：把 FIFO 里残留数据读完(仅 PIO) */

      if (priv->buffer != NULL)
        {
          rk3576_recvfifo(priv);
        }

      priv->remaining = 0;
      priv->buffer    = NULL;
      rk3576_configxfrints(priv, 0);

      if ((priv->waitevents & SDIOWAIT_TRANSFERDONE) != 0)
        {
          rk3576_endwait(priv, SDIOWAIT_TRANSFERDONE);
        }
    }

  /* --- 命令响应错误 --- */

  if ((pending & SDMMC_RESPERR_INTS) != 0)
    {
      if ((priv->waitevents & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE |
                               SDIOWAIT_ERROR)) != 0)
        {
          rk3576_endwait(priv, SDIOWAIT_ERROR);
        }
    }

  /* --- 命令完成 --- */

  else if ((pending & SDMMC_INT_CMD_DONE) != 0)
    {
      if ((priv->waitevents & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE))
          != 0)
        {
          rk3576_endwait(priv, priv->waitevents &
                         (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE));
        }
    }

  /* --- 卡检测变化 --- */

  if ((pending & SDMMC_INT_CD) != 0)
    {
      rk3576_callback(priv);
    }

  return OK;
}

/***************************************************************************
 * Name: rk3576_reset
 *
 * Description:
 *   复位控制器与 FIFO，清中断，恢复默认时钟/总线宽度。
 *   ★ TODO：不含 CRU 时钟门控/软复位与 pinctrl，依赖 bootloader 已配好。
 ***************************************************************************/

static void rk3576_reset(struct sdio_dev_s *dev)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  irqstate_t flags;
  int i;

  flags = enter_critical_section();

  /* 复位控制器 + FIFO + DMA */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL, SDMMC_CTRL_RESET_ALL);

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_CTRL) &
           SDMMC_CTRL_RESET_ALL) == 0)
        {
          break;
        }
    }

  /* 上电使能卡电源 */

  rk3576_putreg(priv, RK3576_SDMMC_PWREN, 1);

  /* 清所有原始中断，屏蔽所有中断 */

  rk3576_putreg(priv, RK3576_SDMMC_RINTSTS, SDMMC_INT_ALL);
  rk3576_putreg(priv, RK3576_SDMMC_INTMASK, 0);

  /* FIFO 阈值：RX/TX 半满触发，突发长度 1（DW_MSHC FIFOTH 格式：
   * [30:28]=DW_MSIZE, [27:16]=RX_WMARK, [11:0]=TX_WMARK）。
   */

  rk3576_putreg(priv, RK3576_SDMMC_FIFOTH,
                (2 << 28) |
                ((RK3576_SDMMC_FIFO_HALF - 1) << 16) |
                (RK3576_SDMMC_FIFO_HALF));

  /* 数据/响应超时设最大 */

  rk3576_putreg(priv, RK3576_SDMMC_TMOUT, 0xffffffff);

  /* 默认 1-bit 总线 */

  rk3576_putreg(priv, RK3576_SDMMC_CTYPE, SDMMC_CTYPE_1BIT);
  priv->widebus = false;

  /* 全局中断使能（控制器级） */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) |
                SDMMC_CTRL_INT_ENABLE);

  /* 复位驱动软件状态 */

  priv->buffer     = NULL;
  priv->remaining  = 0;
  priv->xfrints    = 0;
  priv->waitevents = 0;
  priv->wkupevent  = 0;

#ifdef CONFIG_SDIO_DMA
  /* 初始化 IDMAC：软复位并等自清，清状态，使能收/发完成+错误中断。
   * 复位默认不选 IDMAC(USE_IDMAC=0)，PIO 为缺省路径。
   */

  rk3576_putreg(priv, RK3576_SDMMC_BMOD, SDMMC_IDMAC_SWRESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_BMOD) &
           SDMMC_IDMAC_SWRESET) == 0)
        {
          break;
        }
    }

  rk3576_putreg(priv, RK3576_SDMMC_IDSTS, SDMMC_IDMAC_INT_ALL);
  rk3576_putreg(priv, RK3576_SDMMC_IDINTEN, SDMMC_IDMAC_INT_ENA);
  priv->dmamode = false;
#endif
  priv->blocksize = 0;

  /* ID 模式初始时钟（<400KHz） */

  rk3576_setclock(priv, RK3576_SDMMC_ID_FREQ);

  leave_critical_section(flags);
}

/***************************************************************************
 * Name: rk3576_capabilities
 *
 * Description:
 *   报告 host 能力：第一版仅支持 4-bit、无 DMA（纯 PIO）。
 ***************************************************************************/

static sdio_capset_t rk3576_capabilities(struct sdio_dev_s *dev)
{
  sdio_capset_t caps = 0;

#ifndef CONFIG_SDIO_WIDTH_D1_ONLY
  caps |= SDIO_CAPS_4BIT;
#endif

  /* 第一版无 DMA，不设 SDIO_CAPS_DMASUPPORTED。
   * 但必须设 DMABEFOREWRITE：它让 mmcsd 把写命令(CMD24/25)延到 SENDSETUP
   * 之后再发。否则 mmcsd 会先发 CMD24 再 SENDSETUP，控制器进入写数据态时
   * TX FIFO 还空 → underrun → 数据错误(甚至 SENDSETUP 预填撞上错误中断清空
   * 的 buffer 而崩)。设它后：SENDSETUP 预填 FIFO → 再发 CMD24 → 不 underrun。
   */

  caps |= SDIO_CAPS_DMABEFOREWRITE;

#ifdef CONFIG_SDIO_DMA
  /* IDMAC 硬件搬数据：提速 + 解决多块写 underrun。不对齐/超容量的 buffer 由
   * dmapreflight 拒绝，mmcsd 自动回退到 PIO 路径。
   */

  caps |= SDIO_CAPS_DMASUPPORTED;
#endif

  return caps;
}

/***************************************************************************
 * Name: rk3576_status
 *
 * Description:
 *   返回卡在位/写保护状态。用控制器 CDETECT（bit0=0 有卡）与 WRTPRT。
 ***************************************************************************/

static sdio_statset_t rk3576_status(struct sdio_dev_s *dev)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  sdio_statset_t status = 0;

  if ((rk3576_getreg(priv, RK3576_SDMMC_CDETECT) &
       SDMMC_CDETECT_PRESENT) == 0)
    {
      status |= SDIO_STATUS_PRESENT;   /* bit0=0 => 有卡 */
    }

  if ((rk3576_getreg(priv, RK3576_SDMMC_WRTPRT) & 1) != 0)
    {
      status |= SDIO_STATUS_WRPROTECTED;
    }

  return status;
}

/***************************************************************************
 * Name: rk3576_widebus
 ***************************************************************************/

static void rk3576_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  rk3576_putreg(priv, RK3576_SDMMC_CTYPE,
                enable ? SDMMC_CTYPE_4BIT : SDMMC_CTYPE_1BIT);
  priv->widebus = enable;
}

/***************************************************************************
 * Name: rk3576_clock
 ***************************************************************************/

static void rk3576_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t freq;

  switch (rate)
    {
      default:
      case CLOCK_SDIO_DISABLED:
        freq = 0;
        break;

      case CLOCK_IDMODE:
        freq = RK3576_SDMMC_ID_FREQ;
        break;

      case CLOCK_MMC_TRANSFER:
        freq = RK3576_SDMMC_MMC_FREQ;
        break;

      case CLOCK_SD_TRANSFER_1BIT:
        freq = RK3576_SDMMC_SD1_FREQ;
        break;

      case CLOCK_SD_TRANSFER_4BIT:
        freq = RK3576_SDMMC_SD4_FREQ;
        break;
    }

  rk3576_setclock(priv, freq);
}

/***************************************************************************
 * Name: rk3576_attach
 *
 * Description:
 *   挂接控制器中断并使能。
 ***************************************************************************/

static int rk3576_attach(struct sdio_dev_s *dev)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  int ret;

  ret = irq_attach(priv->irq, rk3576_interrupt, priv);
  if (ret < 0)
    {
      mcerr("ERROR: irq_attach 失败 irq=%d ret=%d\n", priv->irq, ret);
      return ret;
    }

  /* 屏蔽所有源，清挂起状态，随后开控制器级中断门 */

  rk3576_putreg(priv, RK3576_SDMMC_INTMASK, 0);
  rk3576_putreg(priv, RK3576_SDMMC_RINTSTS, SDMMC_INT_ALL);

  up_enable_irq(priv->irq);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sendcmd
 *
 * Description:
 *   发送命令。根据 NuttX 命令编码设置 dw-mshc CMD 寄存器的响应/数据位。
 ***************************************************************************/

static int rk3576_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                          uint32_t arg)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t regval;
  uint32_t cmdidx;
  int i;

  cmdidx = (cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT;
  regval = (cmdidx << SDMMC_CMD_INDEX_SHIFT) |
           SDMMC_CMD_START | SDMMC_CMD_USE_HOLD_REG | SDMMC_CMD_WAIT_PRV;

  /* 响应类型 */

  switch (cmd & MMCSD_RESPONSE_MASK)
    {
      case MMCSD_NO_RESPONSE:
        break;

      case MMCSD_R2_RESPONSE:
        /* 长响应（136-bit），校验 CRC */

        regval |= SDMMC_CMD_RESP_EXPECT | SDMMC_CMD_RESP_LONG |
                  SDMMC_CMD_RESP_CRC;
        break;

      case MMCSD_R3_RESPONSE:
      case MMCSD_R4_RESPONSE:
        /* 短响应，不校验 CRC（OCR 类） */

        regval |= SDMMC_CMD_RESP_EXPECT;
        break;

      default:
        /* R1/R1B/R5/R6/R7：短响应 + CRC */

        regval |= SDMMC_CMD_RESP_EXPECT | SDMMC_CMD_RESP_CRC;
        break;
    }

  /* 数据传输方向 */

  if ((cmd & MMCSD_DATAXFR_MASK) != 0)
    {
      regval |= SDMMC_CMD_DATA_EXPECTED;
      if ((cmd & MMCSD_WRXFR) != 0)
        {
          regval |= SDMMC_CMD_WRITE;
        }
    }

  /* 停止传输命令（CMD12） */

  if ((cmd & MMCSD_STOPXFR) != 0)
    {
      regval |= SDMMC_CMD_SEND_STOP;
    }

  /* CMD0 需要发 80-clock 初始化序列 */

  if (cmdidx == MMCSD_CMDIDX0)
    {
      regval |= SDMMC_CMD_INIT;
    }

  /* 写参数，清命令完成/响应状态位，再启动命令 */

  rk3576_putreg(priv, RK3576_SDMMC_RINTSTS,
                SDMMC_INT_CMD_DONE | SDMMC_RESPERR_INTS);
  rk3576_putreg(priv, RK3576_SDMMC_CMDARG, arg);
  rk3576_putreg(priv, RK3576_SDMMC_CMD, regval);

  /* 等硬件接收命令（CMD_START 自清） */

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_CMD) & SDMMC_CMD_START) == 0)
        {
          return OK;
        }
    }

  mcerr("ERROR: 命令下发超时 cmd=%08" PRIx32 "\n", cmd);
  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_recvsetup
 *
 * Description:
 *   PIO 接收准备：复位 FIFO，设块大小/字节数，登记缓冲区并开 RX 中断。
 ***************************************************************************/

static int rk3576_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                            size_t nbytes)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0);   /* 32bit 对齐 */

  /* 复位 FIFO */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) |
                SDMMC_CTRL_FIFO_RESET);

  priv->buffer    = (uint32_t *)buffer;
  priv->remaining = nbytes;

  /* 设块大小与字节数：块大小取 blocksetup 记录值(缺省 nbytes) */

  rk3576_putreg(priv, RK3576_SDMMC_BLKSIZ,
                priv->blocksize ? priv->blocksize : nbytes);
  rk3576_putreg(priv, RK3576_SDMMC_BYTCNT, nbytes);

  /* 开数据接收中断：RXDR + DTO + 错误 */

  rk3576_configxfrints(priv, SDMMC_INT_RXDR | SDMMC_XFRDONE_INTS |
                       SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_sendsetup
 *
 * Description:
 *   PIO 发送准备：复位 FIFO，设块大小/字节数，登记缓冲区并开 TX 中断。
 ***************************************************************************/

static int rk3576_sendsetup(struct sdio_dev_s *dev, const uint8_t *buffer,
                            size_t nbytes)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  int i;

  DEBUGASSERT(buffer != NULL && nbytes > 0);
  DEBUGASSERT(((uintptr_t)buffer & 3) == 0);

  /* 复位 FIFO 并等待该位自清，否则随后预填的数据会被复位清掉 */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) |
                SDMMC_CTRL_FIFO_RESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_CTRL) &
           SDMMC_CTRL_FIFO_RESET) == 0)
        {
          break;
        }
    }

  priv->buffer    = (uint32_t *)buffer;
  priv->remaining = nbytes;

  rk3576_putreg(priv, RK3576_SDMMC_BLKSIZ,
                priv->blocksize ? priv->blocksize : nbytes);
  rk3576_putreg(priv, RK3576_SDMMC_BYTCNT, nbytes);

  /* ★ 预填 TX FIFO：DW-MSHC PIO 写必须在发命令前把数据塞进 FIFO，否则发
   * CMD24 后 TX FIFO 为空 → 控制器 underrun(FRUN) → 数据错误 → 写失败。
   * 单块(<=FIFO 容量)一次填完；更大传输靠后续 TXDR 中断续填。
   * 预填必须在开中断【之前】做：否则开 TXDR 后 FIFO 空会立即触发中断，
   * 中断里的 sendfifo 会与此处预填并发争用 priv->buffer/remaining。
   */

  rk3576_sendfifo(priv);

  /* 预填完再开数据发送中断：TXDR(续填) + DTO + 错误 */

  rk3576_configxfrints(priv, SDMMC_INT_TXDR | SDMMC_XFRDONE_INTS |
                       SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_cancel
 *
 * Description:
 *   取消未完成的数据传输：关中断、停看门狗、清软件状态。
 ***************************************************************************/

static int rk3576_cancel(struct sdio_dev_s *dev)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  rk3576_configxfrints(priv, 0);
  rk3576_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);

  priv->buffer    = NULL;
  priv->remaining = 0;
  return OK;
}

/***************************************************************************
 * Name: rk3576_waitresponse
 *
 * Description:
 *   轮询等待命令响应就绪，检查响应超时/CRC 错误。
 ***************************************************************************/

static int rk3576_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t status;
  int i;

  /* 无响应命令：只等命令完成 */

  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      status = rk3576_getreg(priv, RK3576_SDMMC_RINTSTS);

      if ((status & SDMMC_INT_RTO) != 0)
        {
          mcerr("ERROR: 响应超时 cmd=%08" PRIx32 "\n", cmd);
          return -ETIMEDOUT;
        }

      if ((status & SDMMC_INT_RCRC) != 0 &&
          (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
          (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE)
        {
          mcerr("ERROR: 响应 CRC 错 cmd=%08" PRIx32 "\n", cmd);
          return -EIO;
        }

      if ((status & SDMMC_INT_CMD_DONE) != 0)
        {
          return OK;
        }
    }

  return -ETIMEDOUT;
}

/***************************************************************************
 * Name: rk3576_recvshort / rk3576_recvlong
 *
 * Description:
 *   读命令响应。短响应取 RESP0；长响应（R2）取 RESP0..3。
 *   dw-mshc 长响应 RESP0 为最低位字，NuttX 期望 rlong[0] 为最高位字，
 *   故按逆序填充。
 ***************************************************************************/

static int rk3576_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                            uint32_t *rshort)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t status = rk3576_getreg(priv, RK3576_SDMMC_RINTSTS);

  if ((status & SDMMC_INT_RTO) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((status & SDMMC_INT_RCRC) != 0 &&
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
      (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE)
    {
      return -EIO;
    }

  if (rshort != NULL)
    {
      *rshort = rk3576_getreg(priv, RK3576_SDMMC_RESP0);
    }

  return OK;
}

static int rk3576_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                           uint32_t rlong[4])
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t status = rk3576_getreg(priv, RK3576_SDMMC_RINTSTS);

  if ((status & SDMMC_INT_RTO) != 0)
    {
      return -ETIMEDOUT;
    }

  if ((status & SDMMC_INT_RCRC) != 0)
    {
      return -EIO;
    }

  if (rlong != NULL)
    {
      /* RESP3 为最高位字 -> rlong[0] */

      rlong[0] = rk3576_getreg(priv, RK3576_SDMMC_RESP3);
      rlong[1] = rk3576_getreg(priv, RK3576_SDMMC_RESP2);
      rlong[2] = rk3576_getreg(priv, RK3576_SDMMC_RESP1);
      rlong[3] = rk3576_getreg(priv, RK3576_SDMMC_RESP0);
    }

  return OK;
}

/***************************************************************************
 * Name: rk3576_waitenable
 *
 * Description:
 *   使能一组等待事件并启动软件超时看门狗。
 ***************************************************************************/

static void rk3576_waitenable(struct sdio_dev_s *dev,
                              sdio_eventset_t eventset, uint32_t timeout)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  uint32_t waitints = 0;

  /* 由等待事件映射到硬件中断 */

  if ((eventset & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
    {
      waitints |= SDMMC_INT_CMD_DONE | SDMMC_RESPERR_INTS;
    }

  if ((eventset & SDIOWAIT_TRANSFERDONE) != 0)
    {
      waitints |= SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS;
    }

  rk3576_configwaitints(priv, waitints, eventset, 0);

  /* 启动超时看门狗（仅当关注 TIMEOUT 且 timeout>0） */

  if ((eventset & SDIOWAIT_TIMEOUT) != 0 && timeout > 0)
    {
      int ret = wd_start(&priv->waitwdog, MSEC2TICK(timeout),
                         rk3576_eventtimeout, (wdparm_t)priv);
      if (ret < 0)
        {
          mcerr("ERROR: wd_start 失败 ret=%d\n", ret);
        }
    }
}

/***************************************************************************
 * Name: rk3576_eventwait
 *
 * Description:
 *   阻塞等待被使能的事件之一发生（或超时）。返回唤醒事件集。
 ***************************************************************************/

static sdio_eventset_t rk3576_eventwait(struct sdio_dev_s *dev)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;
  sdio_eventset_t wkupevent;

  /* 等中断把 waitsem post 起来（endwait 中会做） */

  for (; ; )
    {
      nxsem_wait_uninterruptible(&priv->waitsem);

      /* wkupevent 非 0 表示确有事件唤醒 */

      wkupevent = priv->wkupevent;
      if (wkupevent != 0)
        {
          break;
        }
    }

  /* 等待结束，清所有等待中断 */

  rk3576_configwaitints(priv, 0, 0, 0);
  wd_cancel(&priv->waitwdog);
  priv->wkupevent = 0;

  return wkupevent;
}

/***************************************************************************
 * Name: rk3576_callbackenable
 ***************************************************************************/

static void rk3576_callbackenable(struct sdio_dev_s *dev,
                                  sdio_eventset_t eventset)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  priv->cbevents = eventset;
  rk3576_callback(priv);
}

#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
/***************************************************************************
 * Name: rk3576_registercallback
 ***************************************************************************/

static int rk3576_registercallback(struct sdio_dev_s *dev,
                                    worker_t callback, void *arg)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  priv->cbevents = 0;
  priv->cbarg    = arg;
  priv->callback = callback;
  return OK;
}
#endif

/***************************************************************************
 * Name: rk3576_gotextcsd
 *
 * Description:
 *   接收 EXT CSD 通知（eMMC 用）。SD 卡不需要，空实现。
 ***************************************************************************/

static void rk3576_gotextcsd(struct sdio_dev_s *dev,
                             const uint8_t *buffer)
{
  UNUSED(dev);
  UNUSED(buffer);
}

#ifdef CONFIG_SDIO_DMA

/***************************************************************************
 * Name: rk3576_dma_disable
 *
 * Description:
 *   关闭 IDMAC：清 BMOD 的 DE 位与 CTRL 的 USE_IDMAC，退出 DMA 模式。
 ***************************************************************************/

static void rk3576_dma_disable(struct rk3576_dev_s *priv)
{
  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) &
                ~SDMMC_CTRL_USE_IDMAC);
  rk3576_putreg(priv, RK3576_SDMMC_BMOD,
                rk3576_getreg(priv, RK3576_SDMMC_BMOD) &
                ~SDMMC_IDMAC_ENABLE);
  priv->dmamode = false;
}

/***************************************************************************
 * Name: rk3576_dma_build
 *
 * Description:
 *   把 [buffer, buffer+buflen) 拆成 <=4KB 的段，填入静态描述符链(32 位链式)。
 *   首段置 FD、末段置 LD，非末段置 DIC(仅末段产生完成中断)。填完 clean 描述符
 *   表的 dcache，让 IDMAC 读到 CPU 写入的描述符。
 ***************************************************************************/

static int rk3576_dma_build(struct rk3576_dev_s *priv,
                            const uint8_t *buffer, size_t buflen)
{
  struct rk3576_idmac_desc_s *desc = priv->descs;
  uintptr_t addr = (uintptr_t)buffer;
  size_t remaining = buflen;
  unsigned int n;
  unsigned int i;

  n = (buflen + RK3576_IDMAC_BUFSZ - 1) / RK3576_IDMAC_BUFSZ;
  if (n == 0 || n > RK3576_IDMAC_NDESC)
    {
      return -EFAULT;
    }

  for (i = 0; i < n; i++)
    {
      size_t seg = remaining > RK3576_IDMAC_BUFSZ ?
                   RK3576_IDMAC_BUFSZ : remaining;
      uint32_t ctrl = IDMAC_DES0_OWN | IDMAC_DES0_CH;

      desc[i].des1 = IDMAC_DES1_BS1(seg);
      desc[i].des2 = (uint32_t)addr;      /* buffer1 物理地址(低 4G) */

      if (i == 0)
        {
          ctrl |= IDMAC_DES0_FD;
        }

      if (i == n - 1)
        {
          ctrl |= IDMAC_DES0_LD;          /* 末段：完成后停止 */
          desc[i].des3 = 0;
        }
      else
        {
          ctrl |= IDMAC_DES0_DIC;         /* 非末段不产生完成中断 */
          desc[i].des3 = (uint32_t)(uintptr_t)&desc[i + 1];
        }

      desc[i].des0 = ctrl;

      addr      += seg;
      remaining -= seg;
    }

  /* ★ 描述符由 CPU 写、由 DMA 读 —— 必须把描述符表刷回内存 */

  up_clean_dcache((uintptr_t)desc,
                  (uintptr_t)desc + n * sizeof(struct rk3576_idmac_desc_s));
  return OK;
}

/***************************************************************************
 * Name: rk3576_dma_common
 *
 * Description:
 *   dmarecvsetup/dmasendsetup 公共流程：复位 FIFO+DMA、建描述符链、处理 cache、
 *   使能 IDMAC、设块大小/字节数、切到 DTO+错误中断(关 PIO 的 RXDR/TXDR)。
 ***************************************************************************/

static int rk3576_dma_common(struct rk3576_dev_s *priv,
                             const uint8_t *buffer, size_t buflen,
                             bool write)
{
  uint32_t blksz;
  int ret;
  int i;

  /* 进入 DMA 模式：清 PIO 用的 buffer 指针，防 ISR 走 recvfifo/sendfifo */

  priv->buffer    = NULL;
  priv->remaining = 0;
  priv->dmabuf    = (uintptr_t)buffer;
  priv->dmalen    = buflen;
  priv->dmaread   = !write;
  priv->dmamode   = true;

  /* 复位 FIFO + DMA，并等待自清 */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) |
                SDMMC_CTRL_FIFO_RESET | SDMMC_CTRL_DMA_RESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_CTRL) &
           (SDMMC_CTRL_FIFO_RESET | SDMMC_CTRL_DMA_RESET)) == 0)
        {
          break;
        }
    }

  /* 软复位 IDMAC，等自清 */

  rk3576_putreg(priv, RK3576_SDMMC_BMOD, SDMMC_IDMAC_SWRESET);
  for (i = 0; i < RK3576_SDMMC_SPIN; i++)
    {
      if ((rk3576_getreg(priv, RK3576_SDMMC_BMOD) &
           SDMMC_IDMAC_SWRESET) == 0)
        {
          break;
        }
    }

  /* 建描述符链 */

  ret = rk3576_dma_build(priv, buffer, buflen);
  if (ret < 0)
    {
      priv->dmamode = false;
      return ret;
    }

  /* ★ cache 一致性：
   *   发送(DMA 读内存) -> clean，把 CPU 缓存刷到内存；
   *   接收(DMA 写内存) -> invalidate，避免脏行回写覆盖 DMA 数据，
   *                        DTO 后 ISR 再 invalidate 一次让 CPU 读到新数据。
   */

  if (write)
    {
      up_clean_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }
  else
    {
      up_invalidate_dcache((uintptr_t)buffer, (uintptr_t)buffer + buflen);
    }

  /* 使能 IDMAC 中断(收/发完成 + 错误) */

  rk3576_putreg(priv, RK3576_SDMMC_IDSTS, SDMMC_IDMAC_INT_ALL);
  rk3576_putreg(priv, RK3576_SDMMC_IDINTEN, SDMMC_IDMAC_INT_ENA);

  /* 描述符基址 -> DBADDR */

  rk3576_putreg(priv, RK3576_SDMMC_DBADDR, (uint32_t)(uintptr_t)priv->descs);

  /* CTRL 选内部 DMA；BMOD 使能 DE(+固定突发) */

  rk3576_putreg(priv, RK3576_SDMMC_CTRL,
                rk3576_getreg(priv, RK3576_SDMMC_CTRL) |
                SDMMC_CTRL_USE_IDMAC);
  rk3576_putreg(priv, RK3576_SDMMC_BMOD,
                SDMMC_IDMAC_FB | SDMMC_IDMAC_ENABLE);

  /* 块大小/字节数：blocksize 由 blocksetup 记录，缺省 512 */

  blksz = priv->blocksize ? priv->blocksize : buflen;
  rk3576_putreg(priv, RK3576_SDMMC_BLKSIZ, blksz);
  rk3576_putreg(priv, RK3576_SDMMC_BYTCNT, buflen);

  /* 数据完成用 DTO，错误用 DATAERR；DMA 模式不开 RXDR/TXDR */

  rk3576_configxfrints(priv, SDMMC_XFRDONE_INTS | SDMMC_DATAERR_INTS);
  return OK;
}

/***************************************************************************
 * Name: rk3576_dmapreflight
 *
 * Description:
 *   校验 buffer 是否适合 IDMAC：cache line 对齐、长度对齐、不超描述符容量、
 *   物理地址落在低 4G(32 位描述符)。不满足返回 -EFAULT，mmcsd 自动回退 PIO。
 ***************************************************************************/

/* 判断 buffer 能否走 IDMAC:cache line 对齐 + 不超描述符容量 + 落低 4G。 */

static bool rk3576_dma_ok(const uint8_t *buffer, size_t buflen)
{
  uintptr_t addr = (uintptr_t)buffer;
  size_t line = up_get_dcache_linesize();

  if (line == 0)
    {
      line = 64;   /* armv8-a L1 D-cache line */
    }

  return (addr & (line - 1)) == 0 && (buflen & (line - 1)) == 0 &&
         buflen <= (size_t)RK3576_IDMAC_NDESC * RK3576_IDMAC_BUFSZ &&
         (uint64_t)addr + buflen <= 0x100000000ull;
}

#ifdef CONFIG_ARCH_HAVE_SDIO_PREFLIGHT
static int rk3576_dmapreflight(struct sdio_dev_s *dev,
                               const uint8_t *buffer, size_t buflen)
{
  /* ★ 总是返回 OK:mmcsd 对 preflight 失败【不回退 PIO】(直接返回 EFAULT),
   * 所以这里不能拒绝任何 buffer。实际能否走 DMA 在下面的 setup 里判,不满足
   * (不对齐/超容量/超 4G)时由【驱动自身】回退到 PIO,保证任何 buffer 都能读写。
   */

  UNUSED(dev);
  UNUSED(buffer);
  UNUSED(buflen);
  return OK;
}
#endif

/***************************************************************************
 * Name: rk3576_dmarecvsetup / rk3576_dmasendsetup
 ***************************************************************************/

static int rk3576_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                               size_t buflen)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && buflen > 0);

  /* buffer 不适合 DMA -> 驱动自行回退 PIO(mmcsd 不会替我们回退) */

  if (!rk3576_dma_ok(buffer, buflen))
    {
      priv->dmamode = false;
      return rk3576_recvsetup(dev, buffer, buflen);
    }

  return rk3576_dma_common(priv, buffer, buflen, false);
}

static int rk3576_dmasendsetup(struct sdio_dev_s *dev,
                               const uint8_t *buffer, size_t buflen)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  DEBUGASSERT(buffer != NULL && buflen > 0);

  if (!rk3576_dma_ok(buffer, buflen))
    {
      priv->dmamode = false;
      return rk3576_sendsetup(dev, buffer, buflen);
    }

  return rk3576_dma_common(priv, buffer, buflen, true);
}

#endif /* CONFIG_SDIO_DMA */

#ifdef CONFIG_SDIO_BLOCKSETUP
/***************************************************************************
 * Name: rk3576_blocksetup
 *
 * Description:
 *   记录块大小并预置 BLKSIZ/BYTCNT。DMA 与 PIO 路径都据此设块大小。
 ***************************************************************************/

static void rk3576_blocksetup(struct sdio_dev_s *dev,
                              unsigned int blocklen, unsigned int nblocks)
{
  struct rk3576_dev_s *priv = (struct rk3576_dev_s *)dev;

  priv->blocksize = blocklen;
  rk3576_putreg(priv, RK3576_SDMMC_BLKSIZ, blocklen);
  rk3576_putreg(priv, RK3576_SDMMC_BYTCNT, blocklen * nblocks);
}
#endif


/***************************************************************************
 * Public Functions
 ***************************************************************************/

/***************************************************************************
 * Name: rk3576_sdmmc_initialize
 *
 * Description:
 *   初始化 RK3576 SDMMC(SD 卡) host，返回 sdio_dev_s 供 mmcsd 层挂载。
 *
 * Input Parameters:
 *   slotno - 槽号（RK3576 仅 1 个 SD 卡槽，取 0）。
 *
 * Returned Value:
 *   成功返回 sdio_dev_s 指针，失败返回 NULL。
 ***************************************************************************/

struct sdio_dev_s *rk3576_sdmmc_initialize(int slotno)
{
  struct rk3576_dev_s *priv = &g_sdmmcdev;

  DEBUGASSERT(slotno == 0);

#ifdef CONFIG_SDIO_DMA
  priv->descs = g_idmac_descs;
#endif

  nxmutex_init(&priv->dev.mutex);
  nxsem_init(&priv->waitsem, 0, 0);
  wd_init(&priv->waitwdog);

  /* 复位控制器到已知状态 */

  rk3576_reset(&priv->dev);

  mcinfo("RK3576 SDMMC 初始化完成 base=%08" PRIxPTR " irq=%d\n",
         priv->base, priv->irq);

  return &priv->dev;
}

#endif /* CONFIG_RK3576_SDMMC */
