/****************************************************************************
 * chips/rk3576/hardware/rk3576_i3c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I3C_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I3C_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The RK3576 "rockchip,i3c-master" block is a Synopsys DesignWare MIPI I3C
 * host controller.  The register map below follows the DesignWare I3C
 * databook layout; the RK3576 TRM I3C chapter was not available while this
 * driver was written, so field positions marked "TODO: verify against TRM"
 * are taken from the public DesignWare layout used by the mainline Linux
 * dw-i3c-master driver (which binds to this very block on RK3576).
 */

/* Register offsets ********************************************************/

#define RK3576_I3C_DEVICE_CTRL           0x0000 /* Device control */
#define RK3576_I3C_DEVICE_ADDR           0x0004 /* Controller own address */
#define RK3576_I3C_HW_CAPABILITY         0x0008 /* Hardware capability */
#define RK3576_I3C_COMMAND_QUEUE_PORT    0x000c /* Command queue (write) */
#define RK3576_I3C_RESPONSE_QUEUE_PORT   0x0010 /* Response queue (read) */
#define RK3576_I3C_RX_TX_DATA_PORT       0x0014 /* PIO data port */
#define RK3576_I3C_IBI_QUEUE_STATUS      0x0018 /* IBI status / data port */
#define RK3576_I3C_IBI_QUEUE_DATA        0x0018 /* Alias: IBI payload read */
#define RK3576_I3C_QUEUE_THLD_CTRL       0x001c /* Cmd/resp/IBI thresholds */
#define RK3576_I3C_DATA_BUFFER_THLD_CTRL 0x0020 /* TX/RX FIFO thresholds */
#define RK3576_I3C_IBI_QUEUE_CTRL        0x0024 /* IBI queue control */
#define RK3576_I3C_IBI_MR_REQ_REJECT     0x002c /* Master-request reject */
#define RK3576_I3C_IBI_SIR_REQ_REJECT    0x0030 /* Slave-IRQ reject */
#define RK3576_I3C_RESET_CTRL            0x0034 /* Soft reset control */
#define RK3576_I3C_SLV_EVENT_STATUS      0x0038 /* Slave event status */
#define RK3576_I3C_INTR_STATUS           0x003c /* Interrupt status (W1C) */
#define RK3576_I3C_INTR_STATUS_EN        0x0040 /* Status enable */
#define RK3576_I3C_INTR_SIGNAL_EN        0x0044 /* Signal (to GIC) enable */
#define RK3576_I3C_INTR_FORCE            0x0048 /* Force interrupt (debug) */
#define RK3576_I3C_QUEUE_STATUS_LEVEL    0x004c /* Cmd/resp/IBI levels */
#define RK3576_I3C_DATA_BUFFER_STATUS    0x0050 /* TX/RX FIFO levels */
#define RK3576_I3C_PRESENT_STATE         0x0054 /* Bus/CM state machine */
#define RK3576_I3C_CCC_DEVICE_STATUS     0x0058 /* Last GETSTATUS payload */
#define RK3576_I3C_DEV_ADDR_TABLE_PTR    0x005c /* DAT location/size */
#define RK3576_I3C_DEV_CHAR_TABLE_PTR    0x0060 /* DCT location/size */
#define RK3576_I3C_VENDOR_SPEC_REG_PTR   0x006c /* Vendor block location */
#define RK3576_I3C_SLV_PID_VALUE         0x0074 /* Provisional ID (slave) */
#define RK3576_I3C_SLV_CHAR_CTRL         0x0078 /* BCR/DCR (slave) */
#define RK3576_I3C_DEVICE_CTRL_EXTENDED  0x00b0 /* Master/slave mode */
#define RK3576_I3C_SCL_I3C_OD_TIMING     0x00b4 /* I3C open-drain timing */
#define RK3576_I3C_SCL_I3C_PP_TIMING     0x00b8 /* I3C push-pull timing */
#define RK3576_I3C_SCL_I2C_FM_TIMING     0x00bc /* I2C fast-mode timing */
#define RK3576_I3C_SCL_I2C_FMP_TIMING    0x00c0 /* I2C fast-mode+ timing */
#define RK3576_I3C_SCL_EXT_LCNT_TIMING   0x00c8 /* SDR1..SDR4 low counts */
#define RK3576_I3C_SCL_EXT_TERMN_LCNT    0x00cc /* Termination low count */
#define RK3576_I3C_BUS_FREE_TIMING       0x00d4 /* Bus-free / available */
#define RK3576_I3C_BUS_IDLE_TIMING       0x00d8 /* Bus-idle count */
#define RK3576_I3C_I3C_VER_ID            0x00e0 /* Version ID */
#define RK3576_I3C_I3C_VER_TYPE          0x00e4 /* Version type */
#define RK3576_I3C_QUEUE_SIZE_CAPABILITY 0x00e8 /* FIFO/queue sizes */

/* Device address table.  The real base comes from DEV_ADDR_TABLE_PTR, this
 * is the reset default used as a fallback.
 */

#define RK3576_I3C_DEV_ADDR_TABLE_LOC    0x0280
#define RK3576_I3C_DEV_ADDR_TABLE_ENTRY(n) \
  (RK3576_I3C_DEV_ADDR_TABLE_LOC + ((n) * 4))

/* DEVICE_CTRL bits ********************************************************/

#define RK3576_I3C_DEVCTRL_IBA_INCLUDE   (1 << 0)  /* Include I3C bcast addr */
#define RK3576_I3C_DEVCTRL_HOT_JOIN_NACK (1 << 8)  /* NACK hot-join requests */
#define RK3576_I3C_DEVCTRL_I2C_SLAVE_PRESENT (1 << 7)
#define RK3576_I3C_DEVCTRL_DMA_ENABLE    (1 << 28) /* Use DMA instead of PIO */
#define RK3576_I3C_DEVCTRL_ABORT         (1 << 29) /* Abort current transfer */
#define RK3576_I3C_DEVCTRL_RESUME        (1 << 30) /* Resume after error */
#define RK3576_I3C_DEVCTRL_ENABLE        (1u << 31) /* Controller enable */

/* DEVICE_ADDR bits ********************************************************/

#define RK3576_I3C_DEVADDR_STATIC_SHIFT  0
#define RK3576_I3C_DEVADDR_STATIC_MASK   (0x7f << 0)
#define RK3576_I3C_DEVADDR_STATIC_VALID  (1 << 15)
#define RK3576_I3C_DEVADDR_DYNAMIC_SHIFT 16
#define RK3576_I3C_DEVADDR_DYNAMIC_MASK  (0x7f << 16)
#define RK3576_I3C_DEVADDR_DYNAMIC_VALID (1u << 31)

/* DEVICE_CTRL_EXTENDED bits ***********************************************/

#define RK3576_I3C_DEVCTRL_EXT_MODE_MASK   (0x3 << 0)
#define RK3576_I3C_DEVCTRL_EXT_MODE_MASTER (0x0 << 0)
#define RK3576_I3C_DEVCTRL_EXT_MODE_SLAVE  (0x1 << 0)

/* RESET_CTRL bits *********************************************************/

#define RK3576_I3C_RESET_CORE     (1 << 0)
#define RK3576_I3C_RESET_CMD_QUE  (1 << 1)
#define RK3576_I3C_RESET_RESP_QUE (1 << 2)
#define RK3576_I3C_RESET_TX_FIFO  (1 << 3)
#define RK3576_I3C_RESET_RX_FIFO  (1 << 4)
#define RK3576_I3C_RESET_IBI_QUE  (1 << 5)
#define RK3576_I3C_RESET_ALL      0x3f

/* INTR_STATUS / INTR_STATUS_EN / INTR_SIGNAL_EN bits **********************/

#define RK3576_I3C_INTR_TX_THLD          (1 << 0)
#define RK3576_I3C_INTR_RX_THLD          (1 << 1)
#define RK3576_I3C_INTR_IBI_THLD         (1 << 2)
#define RK3576_I3C_INTR_CMD_QUEUE_READY  (1 << 3)
#define RK3576_I3C_INTR_RESP_READY       (1 << 4)
#define RK3576_I3C_INTR_TRANSFER_ABORT   (1 << 5)
#define RK3576_I3C_INTR_CCC_UPDATED      (1 << 6)
#define RK3576_I3C_INTR_DYN_ADDR_ASSGN   (1 << 8)
#define RK3576_I3C_INTR_TRANSFER_ERR     (1 << 9)
#define RK3576_I3C_INTR_DEFSLV           (1 << 10)
#define RK3576_I3C_INTR_READ_REQ_RECV    (1 << 11)
#define RK3576_I3C_INTR_IBI_UPDATED      (1 << 12)
#define RK3576_I3C_INTR_BUSOWNER_UPDATED (1 << 13)
#define RK3576_I3C_INTR_ALL              0x3f7f

/* QUEUE_THLD_CTRL fields **************************************************/

#define RK3576_I3C_QUEUE_THLD_CMD_EMPTY_SHIFT  0
#define RK3576_I3C_QUEUE_THLD_RESP_BUF_SHIFT   8
#define RK3576_I3C_QUEUE_THLD_IBI_STATUS_SHIFT 24
#define RK3576_I3C_QUEUE_THLD_FIELD_MASK       0xff

/* DATA_BUFFER_THLD_CTRL fields.  Threshold n means 2^(n+1) entries. */

#define RK3576_I3C_DATA_THLD_TX_EMPTY_SHIFT 0
#define RK3576_I3C_DATA_THLD_RX_BUF_SHIFT   8
#define RK3576_I3C_DATA_THLD_FIELD_MASK     0x7

/* QUEUE_STATUS_LEVEL fields ***********************************************/

#define RK3576_I3C_QSTATUS_CMD_EMPTY_SHIFT  0
#define RK3576_I3C_QSTATUS_CMD_EMPTY_MASK   (0xff << 0)
#define RK3576_I3C_QSTATUS_RESP_LEVEL_SHIFT 8
#define RK3576_I3C_QSTATUS_RESP_LEVEL_MASK  (0xff << 8)
#define RK3576_I3C_QSTATUS_IBI_BUF_SHIFT    16
#define RK3576_I3C_QSTATUS_IBI_BUF_MASK     (0xff << 16)
#define RK3576_I3C_QSTATUS_IBI_STAT_SHIFT   24
#define RK3576_I3C_QSTATUS_IBI_STAT_MASK    (0x1f << 24)

/* DATA_BUFFER_STATUS fields ***********************************************/

#define RK3576_I3C_DSTATUS_TX_EMPTY_SHIFT 0
#define RK3576_I3C_DSTATUS_TX_EMPTY_MASK  (0xff << 0)
#define RK3576_I3C_DSTATUS_RX_LEVEL_SHIFT 16
#define RK3576_I3C_DSTATUS_RX_LEVEL_MASK  (0xff << 16)

/* QUEUE_SIZE_CAPABILITY fields.  Each field encodes 2^(n+1) words. */

#define RK3576_I3C_QSIZE_CMD_SHIFT  0
#define RK3576_I3C_QSIZE_RESP_SHIFT 8
#define RK3576_I3C_QSIZE_TX_SHIFT   16
#define RK3576_I3C_QSIZE_RX_SHIFT   24
#define RK3576_I3C_QSIZE_FIELD_MASK 0xff

/* DEV_ADDR_TABLE_PTR fields ***********************************************/

#define RK3576_I3C_DAT_PTR_ADDR_SHIFT  0
#define RK3576_I3C_DAT_PTR_ADDR_MASK   (0xffff << 0)
#define RK3576_I3C_DAT_PTR_DEPTH_SHIFT 16
#define RK3576_I3C_DAT_PTR_DEPTH_MASK  (0x7f << 16)

/* Device address table entry fields ***************************************/

#define RK3576_I3C_DAT_STATIC_ADDR_SHIFT  0
#define RK3576_I3C_DAT_STATIC_ADDR_MASK   (0x7f << 0)
#define RK3576_I3C_DAT_IBI_PEC_EN         (1 << 11)
#define RK3576_I3C_DAT_IBI_WITH_DATA      (1 << 12)
#define RK3576_I3C_DAT_DYNAMIC_ADDR_SHIFT 16
#define RK3576_I3C_DAT_DYNAMIC_ADDR_MASK  (0xff << 16)
#define RK3576_I3C_DAT_SIR_REJECT         (1 << 13)
#define RK3576_I3C_DAT_MR_REJECT          (1 << 14)
#define RK3576_I3C_DAT_LEGACY_I2C_DEVICE  (1u << 31)

/* Command queue: common attribute field ***********************************/

#define RK3576_I3C_CMD_ATTR_SHIFT       0
#define RK3576_I3C_CMD_ATTR_XFER_CMD    0x0 /* Transfer command */
#define RK3576_I3C_CMD_ATTR_XFER_ARG    0x1 /* Transfer argument */
#define RK3576_I3C_CMD_ATTR_SHORT_ARG   0x2 /* Short data argument */
#define RK3576_I3C_CMD_ATTR_ADDR_ASSGN  0x3 /* Address assignment (ENTDAA) */

/* Transfer command (CMD_ATTR == 0) ****************************************/

#define RK3576_I3C_XFER_TID_SHIFT       3
#define RK3576_I3C_XFER_TID_MASK        (0xf << 3)
#define RK3576_I3C_XFER_CMD_SHIFT       7  /* CCC code when CP == 1 */
#define RK3576_I3C_XFER_CP              (1 << 15) /* Command present */
#define RK3576_I3C_XFER_DEV_INDEX_SHIFT 16 /* DAT index */
#define RK3576_I3C_XFER_SPEED_SHIFT     21
#define RK3576_I3C_XFER_SPEED_I3C_SDR0  (0x0 << 21)
#define RK3576_I3C_XFER_SPEED_I3C_SDR1  (0x1 << 21)
#define RK3576_I3C_XFER_SPEED_I2C_FM    (0x0 << 21) /* In I2C transfers */
#define RK3576_I3C_XFER_SPEED_I2C_FMP   (0x1 << 21)
#define RK3576_I3C_XFER_DBP             (1 << 25) /* Defining byte present */
#define RK3576_I3C_XFER_ROC             (1 << 26) /* Response on completion */
#define RK3576_I3C_XFER_SDAP            (1 << 27) /* Short data argument */
#define RK3576_I3C_XFER_RNW             (1 << 28) /* Read (1) / write (0) */
#define RK3576_I3C_XFER_TOC             (1 << 30) /* Terminate w/ STOP */
#define RK3576_I3C_XFER_PEC             (1u << 31)

/* Transfer argument (CMD_ATTR == 1) ***************************************/

#define RK3576_I3C_XFER_ARG_DB_SHIFT    8  /* Defining byte */
#define RK3576_I3C_XFER_ARG_DL_SHIFT    16 /* Data length in bytes */

/* Address assignment command (CMD_ATTR == 3) ******************************/

#define RK3576_I3C_ADDR_ASSGN_TID_SHIFT       3
#define RK3576_I3C_ADDR_ASSGN_CMD_SHIFT       7  /* ENTDAA / SETDASA code */
#define RK3576_I3C_ADDR_ASSGN_DEV_INDEX_SHIFT 16 /* First DAT index */
#define RK3576_I3C_ADDR_ASSGN_DEV_COUNT_SHIFT 21 /* Number of DAT slots */
#define RK3576_I3C_ADDR_ASSGN_ROC             (1 << 26)
#define RK3576_I3C_ADDR_ASSGN_TOC             (1 << 30)

/* Response queue entry ****************************************************/

#define RK3576_I3C_RESP_DL_SHIFT     0  /* Remaining/received data length */
#define RK3576_I3C_RESP_DL_MASK      (0xffff << 0)
#define RK3576_I3C_RESP_CCCT_SHIFT   16
#define RK3576_I3C_RESP_TID_SHIFT    24
#define RK3576_I3C_RESP_TID_MASK     (0xf << 24)
#define RK3576_I3C_RESP_ERR_SHIFT    28
#define RK3576_I3C_RESP_ERR_MASK     (0xfu << 28)

#define RK3576_I3C_RESP_ERR_NONE       0x0
#define RK3576_I3C_RESP_ERR_CRC        0x1
#define RK3576_I3C_RESP_ERR_PARITY     0x2
#define RK3576_I3C_RESP_ERR_FRAME      0x3
#define RK3576_I3C_RESP_ERR_IBA_NACK   0x4
#define RK3576_I3C_RESP_ERR_ADDR_NACK  0x5
#define RK3576_I3C_RESP_ERR_OVER_UNDER 0x6
#define RK3576_I3C_RESP_ERR_XFER_ABORT 0x8
#define RK3576_I3C_RESP_ERR_I2C_NACK   0x9

/* IBI queue status entry **************************************************/

#define RK3576_I3C_IBI_STS_DL_SHIFT   0 /* Payload length in bytes */
#define RK3576_I3C_IBI_STS_DL_MASK    (0xff << 0)
#define RK3576_I3C_IBI_STS_ID_SHIFT   8 /* (addr << 1) | RnW */
#define RK3576_I3C_IBI_STS_ID_MASK    (0xff << 8)
#define RK3576_I3C_IBI_STS_ERROR      (1u << 30)
#define RK3576_I3C_IBI_STS_ACKED      (1u << 31)

/* Timing register field layout: (high count << 16) | low count */

#define RK3576_I3C_SCL_TIMING(hcnt, lcnt) \
  ((((hcnt) & 0xffff) << 16) | ((lcnt) & 0xffff))

#define RK3576_I3C_EXT_LCNT(l1, l2, l3, l4)                      \
  (((l1) & 0xff) | (((l2) & 0xff) << 8) | (((l3) & 0xff) << 16) | \
   (((l4) & 0xff) << 24))

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_I3C_H */
