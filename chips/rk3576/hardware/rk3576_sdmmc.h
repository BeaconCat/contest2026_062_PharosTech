/***************************************************************************
 * arch/arm64/src/rk3576/hardware/rk3576_sdmmc.h
 *
 * RK3576 SDMMC/SDIO 控制器寄存器 —— Synopsys DesignWare MSHC (dw_mmc)。
 * 与 rk3288/rk3399 同一 IP，寄存器布局标准；参考 Linux drivers/mmc/host/dw_mmc.h。
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0.
 ***************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H

#include <nuttx/config.h>
#include "rk3576_memorymap.h"

/* 寄存器偏移 (DW MSHC) ****************************************************/

#define RK3576_SDMMC_CTRL       0x000  /* 控制 */
#define RK3576_SDMMC_PWREN      0x004  /* 上电使能 */
#define RK3576_SDMMC_CLKDIV     0x008  /* 时钟分频 */
#define RK3576_SDMMC_CLKSRC     0x00c  /* 时钟源 */
#define RK3576_SDMMC_CLKENA     0x010  /* 时钟使能 */
#define RK3576_SDMMC_TMOUT      0x014  /* 超时 */
#define RK3576_SDMMC_CTYPE      0x018  /* 卡总线宽度 */
#define RK3576_SDMMC_BLKSIZ     0x01c  /* 块大小 */
#define RK3576_SDMMC_BYTCNT     0x020  /* 字节计数 */
#define RK3576_SDMMC_INTMASK    0x024  /* 中断屏蔽 */
#define RK3576_SDMMC_CMDARG     0x028  /* 命令参数 */
#define RK3576_SDMMC_CMD        0x02c  /* 命令 */
#define RK3576_SDMMC_RESP0      0x030  /* 响应0 */
#define RK3576_SDMMC_RESP1      0x034  /* 响应1 */
#define RK3576_SDMMC_RESP2      0x038  /* 响应2 */
#define RK3576_SDMMC_RESP3      0x03c  /* 响应3 */
#define RK3576_SDMMC_MINTSTS    0x040  /* 屏蔽后中断状态 */
#define RK3576_SDMMC_RINTSTS    0x044  /* 原始中断状态 (写1清) */
#define RK3576_SDMMC_STATUS     0x048  /* 状态 (FIFO/忙) */
#define RK3576_SDMMC_FIFOTH     0x04c  /* FIFO 阈值 */
#define RK3576_SDMMC_CDETECT    0x050  /* 卡检测 (bit0=0 有卡) */
#define RK3576_SDMMC_WRTPRT     0x054  /* 写保护 */
#define RK3576_SDMMC_TCBCNT     0x05c  /* 传输卡字节计数 */
#define RK3576_SDMMC_TBBCNT     0x060  /* 传输 FIFO 字节计数 */
#define RK3576_SDMMC_DEBNCE     0x064  /* 去抖 */
#define RK3576_SDMMC_USRID      0x068  /* 用户 ID */
#define RK3576_SDMMC_VERID      0x06c  /* 版本 ID */
#define RK3576_SDMMC_HCON       0x070  /* 硬件配置 */
#define RK3576_SDMMC_UHS_REG    0x074  /* UHS-1 */
#define RK3576_SDMMC_RST_N      0x078  /* 卡硬复位 */
#define RK3576_SDMMC_BMOD       0x080  /* 内部 DMA 总线模式 */
#define RK3576_SDMMC_DBADDR     0x088  /* 描述符基址 */
#define RK3576_SDMMC_IDSTS      0x08c  /* IDMAC 状态 */
#define RK3576_SDMMC_IDINTEN    0x090  /* IDMAC 中断使能 */
#define RK3576_SDMMC_CARDTHRCTL 0x100  /* 卡读阈值 */
/* 数据 FIFO：dw_mmc 在 RK 上位于 0x200 (由 HCON 决定，RK3288/3399/3576 = 0x200) */
#define RK3576_SDMMC_DATA       0x200

/* CTRL 位 ****************************************************************/

#define SDMMC_CTRL_RESET        (1 << 0)   /* 控制器复位 */
#define SDMMC_CTRL_FIFO_RESET   (1 << 1)   /* FIFO 复位 */
#define SDMMC_CTRL_DMA_RESET    (1 << 2)   /* DMA 复位 */
#define SDMMC_CTRL_INT_ENABLE   (1 << 4)   /* 全局中断使能 */
#define SDMMC_CTRL_USE_IDMAC    (1 << 25)  /* 用内部 DMA */
#define SDMMC_CTRL_RESET_ALL    (SDMMC_CTRL_RESET | SDMMC_CTRL_FIFO_RESET | \
                                 SDMMC_CTRL_DMA_RESET)

/* CMD 位 *****************************************************************/

#define SDMMC_CMD_INDEX_SHIFT   0
#define SDMMC_CMD_RESP_EXPECT   (1 << 6)   /* 期望响应 */
#define SDMMC_CMD_RESP_LONG     (1 << 7)   /* 长响应 (R2) */
#define SDMMC_CMD_RESP_CRC      (1 << 8)   /* 校验响应 CRC */
#define SDMMC_CMD_DATA_EXPECTED (1 << 9)   /* 有数据传输 */
#define SDMMC_CMD_WRITE         (1 << 10)  /* 写方向 (0=读) */
#define SDMMC_CMD_SEND_STOP     (1 << 12)  /* 自动发 STOP */
#define SDMMC_CMD_WAIT_PRV      (1 << 13)  /* 等前次数据完成 */
#define SDMMC_CMD_INIT          (1 << 15)  /* 发 80 时钟初始化序列 */
#define SDMMC_CMD_UPD_CLK       (1 << 21)  /* 仅更新时钟寄存器 */
#define SDMMC_CMD_USE_HOLD_REG  (1 << 29)  /* 用 hold 寄存器 */
#define SDMMC_CMD_START         (1 << 31)  /* 启动命令 (硬件自清) */

/* RINTSTS / INTMASK 位 ***************************************************/

#define SDMMC_INT_CD            (1 << 0)   /* 卡检测 */
#define SDMMC_INT_RE            (1 << 1)   /* 响应错误 */
#define SDMMC_INT_CMD_DONE      (1 << 2)   /* 命令完成 */
#define SDMMC_INT_DTO           (1 << 3)   /* 数据传输结束 */
#define SDMMC_INT_TXDR          (1 << 4)   /* 发 FIFO 数据请求 */
#define SDMMC_INT_RXDR          (1 << 5)   /* 收 FIFO 数据请求 */
#define SDMMC_INT_RCRC          (1 << 6)   /* 响应 CRC 错 */
#define SDMMC_INT_DCRC          (1 << 7)   /* 数据 CRC 错 */
#define SDMMC_INT_RTO           (1 << 8)   /* 响应超时 */
#define SDMMC_INT_DRTO          (1 << 9)   /* 数据读超时 */
#define SDMMC_INT_HTO           (1 << 10)  /* 数据饥饿超时 */
#define SDMMC_INT_FRUN          (1 << 11)  /* FIFO 上溢/下溢 */
#define SDMMC_INT_HLE           (1 << 12)  /* 硬件锁写错误 */
#define SDMMC_INT_SBE           (1 << 13)  /* 起始位错 */
#define SDMMC_INT_ACD           (1 << 14)  /* 自动命令完成 */
#define SDMMC_INT_EBE           (1 << 15)  /* 结束位错 */
#define SDMMC_INT_SDIO          (1 << 16)  /* SDIO 卡中断 (功能1) */

#define SDMMC_INT_CMD_ERROR     (SDMMC_INT_RE | SDMMC_INT_RCRC | SDMMC_INT_RTO)
#define SDMMC_INT_DATA_ERROR    (SDMMC_INT_DCRC | SDMMC_INT_DRTO | SDMMC_INT_SBE | \
                                 SDMMC_INT_EBE | SDMMC_INT_FRUN | SDMMC_INT_HLE)
#define SDMMC_INT_ALL           0xffffffff

/* STATUS 位 **************************************************************/

#define SDMMC_STATUS_FIFO_EMPTY (1 << 2)
#define SDMMC_STATUS_FIFO_FULL  (1 << 3)
#define SDMMC_STATUS_DATA_BUSY  (1 << 9)   /* card_data_busy */
#define SDMMC_STATUS_FIFO_CNT_SHIFT 17
#define SDMMC_STATUS_FIFO_CNT_MASK  (0x1fff << 17)

/* CTYPE 位 (总线宽度) ****************************************************/

#define SDMMC_CTYPE_1BIT        0
#define SDMMC_CTYPE_4BIT        (1 << 0)
#define SDMMC_CTYPE_8BIT        (1 << 16)

/* CLKENA 位 **************************************************************/

#define SDMMC_CLKENA_ENABLE     (1 << 0)   /* 卡时钟使能 */
#define SDMMC_CLKENA_LOWPWR     (1 << 16)  /* 低功耗 (空闲停时钟) */

/* CDETECT：bit0 = 0 表示有卡插入 */

#define SDMMC_CDETECT_PRESENT   (1 << 0)


/* ===== IDMAC (内部 DMA) 相关定义 —— 参考 Linux drivers/mmc/host/dw_mmc.h =====
 *
 * DW-MSHC 内部 DMA 控制器(IDMAC)使用 32 位链式描述符。以下位定义严格照
 * Linux dw_mmc.h 的 IDMAC_DES0_* / IDMAC_INT_* / BMOD，勿凭记忆改。
 */

/* BMOD (0x080) 总线模式 */

#define SDMMC_IDMAC_SWRESET     (1 << 0)   /* 软复位 IDMAC */
#define SDMMC_IDMAC_FB          (1 << 1)   /* 固定突发 */
#define SDMMC_IDMAC_ENABLE      (1 << 7)   /* DE：使能内部 DMA */

/* IDSTS (0x08c) / IDINTEN (0x090) 位 —— 两者位布局相同 */

#define SDMMC_IDMAC_INT_TI      (1 << 0)   /* 发送完成 */
#define SDMMC_IDMAC_INT_RI      (1 << 1)   /* 接收完成 */
#define SDMMC_IDMAC_INT_FBE     (1 << 2)   /* 致命总线错误 */
#define SDMMC_IDMAC_INT_DU      (1 << 4)   /* 描述符不可用 */
#define SDMMC_IDMAC_INT_CES     (1 << 5)   /* 卡错误汇总 */
#define SDMMC_IDMAC_INT_NI      (1 << 8)   /* 正常中断汇总 */
#define SDMMC_IDMAC_INT_AI      (1 << 9)   /* 异常中断汇总 */
#define SDMMC_IDMAC_INT_ALL     0x337      /* 全部状态位(写1清) */

#define SDMMC_IDMAC_INT_ERR     (SDMMC_IDMAC_INT_FBE | SDMMC_IDMAC_INT_DU | \
                                 SDMMC_IDMAC_INT_CES)
#define SDMMC_IDMAC_INT_DONE    (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI)

/* 使能的 IDMAC 中断：收/发完成 + 各类错误 + 汇总 */

#define SDMMC_IDMAC_INT_ENA     (SDMMC_IDMAC_INT_TI | SDMMC_IDMAC_INT_RI | \
                                 SDMMC_IDMAC_INT_FBE | SDMMC_IDMAC_INT_DU | \
                                 SDMMC_IDMAC_INT_CES | SDMMC_IDMAC_INT_NI | \
                                 SDMMC_IDMAC_INT_AI)

/* 32 位链式描述符 DES0 位 (Linux IDMAC_DES0_*) */

#define IDMAC_DES0_DIC          (1 << 1)   /* 完成不产生中断 */
#define IDMAC_DES0_LD           (1 << 2)   /* 末描述符 */
#define IDMAC_DES0_FD           (1 << 3)   /* 首描述符 */
#define IDMAC_DES0_CH           (1 << 4)   /* 链式(第二地址接下一描述符) */
#define IDMAC_DES0_ER           (1 << 5)   /* 环末尾 */
#define IDMAC_DES0_CES          (1 << 30)  /* 卡错误汇总 */
#define IDMAC_DES0_OWN          (1u << 31) /* 所有权归 DMA */

/* DES1 buffer1 大小域(bits 12:0)，单描述符最大 8191B；本驱动用 4KB/描述符 */

#define IDMAC_DES1_BS_MASK      0x1fff
#define IDMAC_DES1_BS1(x)       ((x) & IDMAC_DES1_BS_MASK)

/* 每描述符搬运字节数：4KB(页对齐、<8191 安全) */

#define RK3576_IDMAC_BUFSZ      4096

/* 32 位链式描述符结构 */

struct rk3576_idmac_desc_s
{
  uint32_t des0;   /* 控制/状态位 */
  uint32_t des1;   /* buffer1 字节数(BS1) */
  uint32_t des2;   /* buffer1 物理地址 */
  uint32_t des3;   /* 链式模式：下一描述符物理地址 */
};


#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_SDMMC_H */
