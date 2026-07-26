/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_husb311.c
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
 * Hynetek HUSB311 USB Type-C port controller.
 *
 * The HUSB311 is a plain USB-TCPCI (Type-C Port Controller Interface)
 * device: every register documented by the TCPCI specification is reachable
 * over I2C, and the silicon takes care of the CC line analog front end, the
 * BMC PHY, CRC generation and the automatic GoodCRC reply.  Everything
 * above that -- the Type-C state machine, the PD protocol layer and the
 * policy engine -- is software, and lives here.
 *
 * Board wiring (from the vendor device tree):
 *   - husb311@4e on i2c2 (0x2ac50000), 7-bit address 0x4e
 *   - ALERT (active low, level triggered) on GPIO4_D1
 *   - usb-c-connector: data-role dual, power-role dual, try-power-role sink
 *   - sink-pdo   0x04019064: Fixed, 5 V, 1.0 A, USB communications capable
 *   - source-pdo 0x0401912c: Fixed, 5 V, 3.0 A, USB communications capable
 *   - altmode svid 0xff01 (VESA DisplayPort)
 *
 * NuttX has no USB Power Delivery framework to plug into, so the driver
 * exposes a character device (/dev/typec0) with a read() text snapshot and
 * a small ioctl set, plus in-kernel entry points for board code.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/irq.h>
#include <nuttx/kmalloc.h>
#include <nuttx/kthread.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include <arch/irq.h>

#include "arm64_internal.h"
#include "hardware/rk3576_gpio.h"
#include "kickpi_k7_husb311.h"
#include "rk3576_gpio.h"

#ifdef CONFIG_KICKPI_K7_HUSB311

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Board resources ***********************************************************/

#define HUSB311_I2C_ADDR      0x4e
#define HUSB311_I2C_FREQUENCY 400000

/* ALERT line: GPIO4_D1, active low, level triggered.  Pin D1 lives in the
 * fourth eight-pin group of bank 4, hence the GPIO4_3 combined interrupt.
 */

#define HUSB311_ALERT_PORT 4
#define HUSB311_ALERT_PIN  25
#define HUSB311_ALERT_IRQ  RK3576_IRQ_GPIO4_3
#define HUSB311_ALERT_PINSET                                         \
  (GPIO_PORT4 | GPIO_PIN_D1 | GPIO_INPUT | GPIO_PULLUP | GPIO_EXTI | \
   GPIO_INT_LEVEL | GPIO_INT_LOW_FALLING)

/* Service thread ***********************************************************/

#define HUSB311_THREAD_NAME  "husb311"
#define HUSB311_THREAD_PRIO  CONFIG_KICKPI_K7_HUSB311_PRIORITY
#define HUSB311_THREAD_STACK CONFIG_KICKPI_K7_HUSB311_STACKSIZE

/* Worst case wakeup period.  The ALERT line drives the thread; this only
 * bounds the latency of the software timers below.
 */

#define HUSB311_POLL_MS 200

/* PD timers (USB PD r2.0 chapter 6.5), rounded to whole milliseconds */

#define HUSB311_T_SENDER_RSP_MS      30  /* tSenderResponse  24..30 ms */
#define HUSB311_T_PS_TRANSITION_MS   500 /* tPSTransition   450..550 ms */
#define HUSB311_T_TYPEC_SINK_WAIT_MS 500 /* tTypeCSinkWaitCap 310..620 */
#define HUSB311_T_VDM_RSP_MS         30  /* tVDMSenderResponse 24..30 ms */

/* TCPCI register map *******************************************************/

#define HUSB311_REG_VENDOR_ID      0x00 /* 16-bit */
#define HUSB311_REG_PRODUCT_ID     0x02 /* 16-bit */
#define HUSB311_REG_DEVICE_ID      0x04 /* 16-bit */
#define HUSB311_REG_TYPEC_REV      0x06 /* 16-bit */
#define HUSB311_REG_USBPD_REV      0x08 /* 16-bit */
#define HUSB311_REG_PD_IF_REV      0x0a /* 16-bit */
#define HUSB311_REG_ALERT          0x0c /* 16-bit, write-1-to-clear */
#define HUSB311_REG_ALERT_MASK     0x0e /* 16-bit */
#define HUSB311_REG_POWER_STAT_MSK 0x10 /* 8-bit */
#define HUSB311_REG_FAULT_STAT_MSK 0x11 /* 8-bit */
#define HUSB311_REG_STD_OUT_CFG    0x13 /* 8-bit */
#define HUSB311_REG_TCPC_CTRL      0x14 /* 8-bit */
#define HUSB311_REG_ROLE_CTRL      0x15 /* 8-bit */
#define HUSB311_REG_FAULT_CTRL     0x16 /* 8-bit */
#define HUSB311_REG_POWER_CTRL     0x17 /* 8-bit */
#define HUSB311_REG_CC_STATUS      0x18 /* 8-bit */
#define HUSB311_REG_POWER_STATUS   0x19 /* 8-bit */
#define HUSB311_REG_FAULT_STATUS   0x1a /* 8-bit, write-1-to-clear */
#define HUSB311_REG_COMMAND        0x1c /* 8-bit */
#define HUSB311_REG_DEV_CAP_1      0x1e /* 16-bit */
#define HUSB311_REG_MSG_HDR_INFO   0x22 /* 8-bit */
#define HUSB311_REG_RECEIVE_DETECT 0x23 /* 8-bit */
#define HUSB311_REG_RX_BYTE_CNT    0x30 /* 8-bit, head of the RX block */
#define HUSB311_REG_RX_FRAME_TYPE  0x31 /* 8-bit */
#define HUSB311_REG_RX_HEADER      0x32 /* 16-bit */
#define HUSB311_REG_RX_DATA        0x34 /* up to 28 bytes */
#define HUSB311_REG_TRANSMIT       0x50 /* 8-bit */
#define HUSB311_REG_TX_BYTE_CNT    0x51 /* 8-bit, head of the TX block */
#define HUSB311_REG_TX_HEADER      0x52 /* 16-bit */
#define HUSB311_REG_TX_DATA        0x54 /* up to 28 bytes */
#define HUSB311_REG_VBUS_VOLTAGE   0x70 /* 16-bit */

/* ALERT / ALERT_MASK bits */

#define HUSB311_ALERT_CC_STATUS     (1 << 0)
#define HUSB311_ALERT_POWER_STATUS  (1 << 1)
#define HUSB311_ALERT_RX_STATUS     (1 << 2)
#define HUSB311_ALERT_RX_HARD_RST   (1 << 3)
#define HUSB311_ALERT_TX_FAILED     (1 << 4)
#define HUSB311_ALERT_TX_DISCARD    (1 << 5)
#define HUSB311_ALERT_TX_SUCCESS    (1 << 6)
#define HUSB311_ALERT_V_ALARM_HI    (1 << 7)
#define HUSB311_ALERT_V_ALARM_LO    (1 << 8)
#define HUSB311_ALERT_FAULT         (1 << 9)
#define HUSB311_ALERT_RX_OVERFLOW   (1 << 10)
#define HUSB311_ALERT_VBUS_SNK_DISC (1 << 11)

#define HUSB311_ALERT_ENABLED                             \
  (HUSB311_ALERT_CC_STATUS | HUSB311_ALERT_POWER_STATUS | \
   HUSB311_ALERT_RX_STATUS | HUSB311_ALERT_RX_HARD_RST |  \
   HUSB311_ALERT_TX_FAILED | HUSB311_ALERT_TX_DISCARD |   \
   HUSB311_ALERT_TX_SUCCESS | HUSB311_ALERT_FAULT |       \
   HUSB311_ALERT_RX_OVERFLOW | HUSB311_ALERT_VBUS_SNK_DISC)

/* TCPC_CONTROL bits */

#define HUSB311_TCPC_CTRL_ORIENT  (1 << 0) /* 1: CC2 is the active line */
#define HUSB311_TCPC_CTRL_BIST    (1 << 1)
#define HUSB311_TCPC_CTRL_DBG_ACC (1 << 2)

/* ROLE_CONTROL bits */

#define HUSB311_ROLE_CC1_SHIFT 0
#define HUSB311_ROLE_CC2_SHIFT 2
#define HUSB311_ROLE_RP_SHIFT  4
#define HUSB311_ROLE_DRP       (1 << 6)

#define HUSB311_CC_TERM_RA     0
#define HUSB311_CC_TERM_RP     1
#define HUSB311_CC_TERM_RD     2
#define HUSB311_CC_TERM_OPEN   3

#define HUSB311_RP_DEFAULT     0
#define HUSB311_RP_1A5         1
#define HUSB311_RP_3A0         2

/* POWER_CONTROL bits */

#define HUSB311_PWR_CTRL_VCONN_EN   (1 << 0)
#define HUSB311_PWR_CTRL_AUTO_DISCH (1 << 4)
#define HUSB311_PWR_CTRL_DIS_V_MON  (1 << 6)

/* CC_STATUS bits */

#define HUSB311_CC_STATUS_CC1_MASK  (3 << 0)
#define HUSB311_CC_STATUS_CC1_SHIFT 0
#define HUSB311_CC_STATUS_CC2_MASK  (3 << 2)
#define HUSB311_CC_STATUS_CC2_SHIFT 2
#define HUSB311_CC_STATUS_CONNRES   (1 << 4) /* 1: we present Rd (sink) */
#define HUSB311_CC_STATUS_LOOKING   (1 << 5)

/* POWER_STATUS bits */

#define HUSB311_PWR_STAT_SINKING   (1 << 0)
#define HUSB311_PWR_STAT_VCONN     (1 << 1)
#define HUSB311_PWR_STAT_VBUS_PRES (1 << 2)
#define HUSB311_PWR_STAT_VBUS_DET  (1 << 3)
#define HUSB311_PWR_STAT_SOURCING  (1 << 4)
#define HUSB311_PWR_STAT_SRC_HV    (1 << 5)
#define HUSB311_PWR_STAT_TCPC_INIT (1 << 6)
#define HUSB311_PWR_STAT_DEBUG_ACC (1 << 7)

/* COMMAND register values */

#define HUSB311_CMD_WAKE_I2C      0x11
#define HUSB311_CMD_DIS_VBUS_DET  0x22
#define HUSB311_CMD_EN_VBUS_DET   0x33
#define HUSB311_CMD_DIS_SINK_VBUS 0x44
#define HUSB311_CMD_SINK_VBUS     0x55
#define HUSB311_CMD_DIS_SRC_VBUS  0x66
#define HUSB311_CMD_SRC_VBUS_DFLT 0x77
#define HUSB311_CMD_SRC_VBUS_HV   0x88
#define HUSB311_CMD_LOOK4CONN     0x99
#define HUSB311_CMD_RX_ONE_MORE   0xaa
#define HUSB311_CMD_I2C_IDLE      0xcc

/* RECEIVE_DETECT bits */

#define HUSB311_RXDET_SOP         (1 << 0)
#define HUSB311_RXDET_SOP_P       (1 << 1)
#define HUSB311_RXDET_SOP_PP      (1 << 2)
#define HUSB311_RXDET_HARD_RESET  (1 << 5)
#define HUSB311_RXDET_CABLE_RESET (1 << 6)

/* MESSAGE_HEADER_INFO bits */

#define HUSB311_MHI_PWR_ROLE_SRC  (1 << 0)
#define HUSB311_MHI_REV_SHIFT     1
#define HUSB311_MHI_DATA_ROLE_DFP (1 << 3)

/* TRANSMIT register: [2:0] SOP type, [5:4] retry count */

#define HUSB311_TX_SOP         0
#define HUSB311_TX_SOP_P       1
#define HUSB311_TX_SOP_PP      2
#define HUSB311_TX_HARD_RESET  5
#define HUSB311_TX_CABLE_RESET 6
#define HUSB311_TX_RETRY_SHIFT 4
#define HUSB311_TX_RETRY_COUNT 3

/* VBUS_VOLTAGE: [9:0] raw counts of 25 mV, [11:10] scale factor */

#define HUSB311_VBUS_RAW_MASK    0x03ff
#define HUSB311_VBUS_SCALE_SHIFT 10
#define HUSB311_VBUS_SCALE_MASK  0x0003
#define HUSB311_VBUS_LSB_MV      25

/* PD message header ********************************************************/

#define PD_HDR_TYPE_MASK   0x001f
#define PD_HDR_DATA_ROLE   (1 << 5)
#define PD_HDR_REV_SHIFT   6
#define PD_HDR_PWR_ROLE    (1 << 8)
#define PD_HDR_MSGID_SHIFT 9
#define PD_HDR_MSGID_MASK  0x0007
#define PD_HDR_NDO_SHIFT   12
#define PD_HDR_NDO_MASK    0x0007
#define PD_HDR_EXTENDED    (1 << 15)

#define PD_REV_2_0         1

#define PD_HDR_TYPE(h)     ((h)&PD_HDR_TYPE_MASK)
#define PD_HDR_NDO(h)      (((h) >> PD_HDR_NDO_SHIFT) & PD_HDR_NDO_MASK)

/* Control message types */

#define PD_CTRL_GOOD_CRC       1
#define PD_CTRL_ACCEPT         3
#define PD_CTRL_REJECT         4
#define PD_CTRL_PS_RDY         6
#define PD_CTRL_GET_SOURCE_CAP 7
#define PD_CTRL_GET_SINK_CAP   8
#define PD_CTRL_DR_SWAP        9
#define PD_CTRL_PR_SWAP        10
#define PD_CTRL_VCONN_SWAP     11
#define PD_CTRL_WAIT           12
#define PD_CTRL_SOFT_RESET     13

/* Data message types */

#define PD_DATA_SOURCE_CAP     1
#define PD_DATA_REQUEST        2
#define PD_DATA_SINK_CAP       4
#define PD_DATA_VENDOR_DEFINED 15

/* Fixed supply PDO fields (source view) */

#define PD_PDO_TYPE_MASK         (3u << 30)
#define PD_PDO_TYPE_FIXED        (0u << 30)
#define PD_PDO_FIXED_VOLT_SHIFT  10 /* 50 mV units */
#define PD_PDO_FIXED_VOLT_MASK   0x03ff
#define PD_PDO_FIXED_CURR_MASK   0x03ff /* 10 mA units */
#define PD_PDO_FIXED_VOLT_LSB_MV 50
#define PD_PDO_FIXED_CURR_LSB_MA 10

/* Fixed Request Data Object fields */

#define PD_RDO_OBJPOS_SHIFT  28
#define PD_RDO_CAP_MISMATCH  (1u << 26)
#define PD_RDO_USB_COMM      (1u << 25)
#define PD_RDO_NO_SUSPEND    (1u << 24)
#define PD_RDO_OP_CURR_SHIFT 10

/* Board capabilities, verbatim from the device tree connector node */

#define HUSB311_SINK_PDO   0x04019064u /* Fixed 5 V 1.0 A */
#define HUSB311_SOURCE_PDO 0x0401912cu /* Fixed 5 V 3.0 A */

/* Highest voltage this board is wired to accept.  The connector only
 * advertises a 5 V sink PDO, so anything above vSafe5V is rejected when
 * picking a source capability.
 */

#define HUSB311_SINK_MAX_MV 5000

/* Structured VDM (USB PD r2.0 chapter 6.4.4) */

#define PD_VDM_SVID_SHIFT        16
#define PD_VDM_STRUCTURED        (1u << 15)
#define PD_VDM_VERSION_SHIFT     13
#define PD_VDM_VERSION_1_0       0
#define PD_VDM_CMDTYPE_SHIFT     6
#define PD_VDM_CMDTYPE_MASK      3
#define PD_VDM_CMDTYPE_REQ       0
#define PD_VDM_CMDTYPE_ACK       1
#define PD_VDM_CMDTYPE_NAK       2
#define PD_VDM_CMD_MASK          0x1f
#define PD_VDM_CMD_DISC_IDENTITY 1
#define PD_VDM_CMD_DISC_SVIDS    2
#define PD_VDM_CMD_DISC_MODES    3
#define PD_VDM_CMD_ENTER_MODE    4

#define PD_SID_PD                0xff00u /* Standard PD SID */
#define PD_SID_DISPLAYPORT       0xff01u /* VESA DisplayPort, per DT */

#define HUSB311_MAX_SVIDS        12
#define HUSB311_MAX_DP_MODES     6

/* Text snapshot returned by read() */

#define HUSB311_SNAPSHOT_SIZE 512

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Discovery state of the DisplayPort alternate mode */

#define HUSB311_DP_IDLE        0
#define HUSB311_DP_DISC_ID     1
#define HUSB311_DP_DISC_SVIDS  2
#define HUSB311_DP_DISC_MODES  3
#define HUSB311_DP_MODES_KNOWN 4
#define HUSB311_DP_UNSUPPORTED 5

struct husb311_dev_s
{
  struct i2c_master_s *i2c; /* I2C2 master this TCPC hangs off */
  mutex_t lock;             /* Serialises register and state access */
  sem_t alert;              /* Posted by the ALERT interrupt handler */
  bool running;             /* Service thread is alive */
  pid_t pid;                /* Service thread */

  /* Type-C layer */

  uint8_t role; /* HUSB311_ROLE_* programmed */
  uint8_t cc1;  /* HUSB311_CC_* */
  uint8_t cc2;  /* HUSB311_CC_* */
  uint8_t orientation;
  uint8_t partner;
  bool attached;
  bool vbus_present;
  uint16_t vbus_mv;

  /* PD protocol layer */

  uint8_t pd_state;
  uint8_t msgid; /* Our outgoing MessageID counter */
  bool explicit_contract;
  clock_t deadline; /* 0: no timer armed */
  uint8_t nr_src_pdo;
  uint32_t src_pdo[HUSB311_MAX_PDO];
  uint32_t rdo;

  /* DisplayPort alternate mode discovery */

  uint8_t dp_state;
  uint8_t nr_dp_modes;
  uint32_t dp_modes[HUSB311_MAX_DP_MODES];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* Register level helpers */

static int husb311_read(struct husb311_dev_s *priv, uint8_t reg, uint8_t *buf,
                        size_t len);
static int husb311_write(struct husb311_dev_s *priv, uint8_t reg,
                         const uint8_t *buf, size_t len);
static int husb311_read8(struct husb311_dev_s *priv, uint8_t reg,
                         uint8_t *val);
static int husb311_write8(struct husb311_dev_s *priv, uint8_t reg,
                          uint8_t val);
static int husb311_read16(struct husb311_dev_s *priv, uint8_t reg,
                          uint16_t *val);
static int husb311_write16(struct husb311_dev_s *priv, uint8_t reg,
                           uint16_t val);
static int husb311_command(struct husb311_dev_s *priv, uint8_t cmd);

/* Type-C layer */

static uint8_t husb311_decode_cc(uint8_t raw, bool sink_view);
static int husb311_apply_role(struct husb311_dev_s *priv, uint8_t role);
static int husb311_update_vbus(struct husb311_dev_s *priv);
static int husb311_update_cc(struct husb311_dev_s *priv);
static void husb311_detach(struct husb311_dev_s *priv);

/* PD protocol layer */

static void husb311_arm_timer(struct husb311_dev_s *priv, unsigned int ms);
static int husb311_transmit(struct husb311_dev_s *priv, uint8_t sop,
                            uint16_t header, const uint32_t *objs,
                            unsigned int ndo);
static int husb311_send_ctrl(struct husb311_dev_s *priv, uint8_t type);
static int husb311_send_data(struct husb311_dev_s *priv, uint8_t type,
                             const uint32_t *objs, unsigned int ndo);
static int husb311_send_hard_reset(struct husb311_dev_s *priv);
static int husb311_send_vdm(struct husb311_dev_s *priv, uint16_t svid,
                            uint8_t cmd, const uint32_t *extra,
                            unsigned int nextra);
static int husb311_select_pdo(struct husb311_dev_s *priv, uint32_t *rdo);
static int husb311_send_request(struct husb311_dev_s *priv);
static void husb311_handle_vdm(struct husb311_dev_s *priv,
                               const uint32_t *objs, unsigned int ndo);
static void husb311_handle_msg(struct husb311_dev_s *priv, uint16_t header,
                               const uint32_t *objs, unsigned int ndo);
static int husb311_receive(struct husb311_dev_s *priv);
static void husb311_pd_reset(struct husb311_dev_s *priv, bool hard);
static void husb311_check_timer(struct husb311_dev_s *priv);

/* Interrupt and service thread */

static int husb311_alert_isr(int irq, void *context, void *arg);
static void husb311_service(struct husb311_dev_s *priv);
static int husb311_thread(int argc, char **argv);

/* Character driver */

static ssize_t husb311_dev_read(struct file *filep, char *buffer,
                                size_t buflen);
static ssize_t husb311_dev_write(struct file *filep, const char *buffer,
                                 size_t buflen);
static int husb311_dev_ioctl(struct file *filep, int cmd, unsigned long arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct husb311_dev_s g_husb311 = {
  .lock = NXMUTEX_INITIALIZER,
};

static const struct file_operations g_husb311_fops = {
  NULL,              /* open */
  NULL,              /* close */
  husb311_dev_read,  /* read */
  husb311_dev_write, /* write */
  NULL,              /* seek */
  husb311_dev_ioctl, /* ioctl */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: husb311_read
 *
 * Description:
 *   Read a block of TCPC registers starting at 'reg'.  TCPCI uses the plain
 *   SMBus style "write register index, repeated start, read data" access.
 *
 ****************************************************************************/

static int husb311_read(struct husb311_dev_s *priv, uint8_t reg, uint8_t *buf,
                        size_t len)
{
  struct i2c_msg_s msg[2];

  msg[0].frequency = HUSB311_I2C_FREQUENCY;
  msg[0].addr = HUSB311_I2C_ADDR;
  msg[0].flags = 0;
  msg[0].buffer = &reg;
  msg[0].length = 1;

  msg[1].frequency = HUSB311_I2C_FREQUENCY;
  msg[1].addr = HUSB311_I2C_ADDR;
  msg[1].flags = I2C_M_READ;
  msg[1].buffer = buf;
  msg[1].length = len;

  return I2C_TRANSFER(priv->i2c, msg, 2);
}

/****************************************************************************
 * Name: husb311_write
 *
 * Description:
 *   Write a block of TCPC registers starting at 'reg'.  The register index
 *   and the payload must travel in a single transaction, so they are copied
 *   into one contiguous buffer.  The largest write is a full PD data
 *   message: index + byte count + header + 7 data objects.
 *
 ****************************************************************************/

static int husb311_write(struct husb311_dev_s *priv, uint8_t reg,
                         const uint8_t *buf, size_t len)
{
  uint8_t txbuf[1 + 1 + 2 + 4 * HUSB311_MAX_PDO];
  struct i2c_msg_s msg;

  if (len + 1 > sizeof(txbuf))
    {
      return -E2BIG;
    }

  txbuf[0] = reg;
  memcpy(&txbuf[1], buf, len);

  msg.frequency = HUSB311_I2C_FREQUENCY;
  msg.addr = HUSB311_I2C_ADDR;
  msg.flags = 0;
  msg.buffer = txbuf;
  msg.length = len + 1;

  return I2C_TRANSFER(priv->i2c, &msg, 1);
}

/****************************************************************************
 * Name: husb311_read8 / husb311_write8 / husb311_read16 / husb311_write16
 *
 * Description:
 *   Scalar register accessors.  All TCPCI multi-byte registers are little
 *   endian on the wire.
 *
 ****************************************************************************/

static int husb311_read8(struct husb311_dev_s *priv, uint8_t reg, uint8_t *val)
{
  return husb311_read(priv, reg, val, 1);
}

static int husb311_write8(struct husb311_dev_s *priv, uint8_t reg, uint8_t val)
{
  return husb311_write(priv, reg, &val, 1);
}

static int husb311_read16(struct husb311_dev_s *priv, uint8_t reg,
                          uint16_t *val)
{
  uint8_t buf[2];
  int ret;

  ret = husb311_read(priv, reg, buf, 2);
  if (ret >= 0)
    {
      *val = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    }

  return ret;
}

static int husb311_write16(struct husb311_dev_s *priv, uint8_t reg,
                           uint16_t val)
{
  uint8_t buf[2];

  buf[0] = (uint8_t)(val & 0xff);
  buf[1] = (uint8_t)(val >> 8);

  return husb311_write(priv, reg, buf, 2);
}

/****************************************************************************
 * Name: husb311_command
 *
 * Description:
 *   Issue a TCPCI COMMAND register opcode.
 *
 ****************************************************************************/

static int husb311_command(struct husb311_dev_s *priv, uint8_t cmd)
{
  return husb311_write8(priv, HUSB311_REG_COMMAND, cmd);
}

/****************************************************************************
 * Name: husb311_decode_cc
 *
 * Description:
 *   Translate the two-bit CC state reported by CC_STATUS into one of the
 *   HUSB311_CC_* values.  The encoding is overloaded: when the port
 *   presents Rd (sink view) the code describes the Rp current advertised by
 *   the far end, otherwise it describes the termination the far end shows.
 *
 ****************************************************************************/

static uint8_t husb311_decode_cc(uint8_t raw, bool sink_view)
{
  if (raw == 0)
    {
      return HUSB311_CC_OPEN;
    }

  if (sink_view)
    {
      switch (raw)
        {
          case 1:
            return HUSB311_CC_RP_DEFAULT;
          case 2:
            return HUSB311_CC_RP_1A5;
          default:
            return HUSB311_CC_RP_3A0;
        }
    }

  /* Source view: 1 = Ra, 2 = Rd, 3 is reserved */

  return (raw == 1) ? HUSB311_CC_RA : HUSB311_CC_RD;
}

/****************************************************************************
 * Name: husb311_apply_role
 *
 * Description:
 *   Program ROLE_CONTROL for the requested port role and restart the
 *   Type-C connection detection state machine.
 *
 ****************************************************************************/

static int husb311_apply_role(struct husb311_dev_s *priv, uint8_t role)
{
  uint8_t val;
  int ret;

  switch (role)
    {
      case HUSB311_ROLE_SINK:
        val = (HUSB311_CC_TERM_RD << HUSB311_ROLE_CC1_SHIFT) |
              (HUSB311_CC_TERM_RD << HUSB311_ROLE_CC2_SHIFT);
        break;

      case HUSB311_ROLE_SOURCE:
        val = (HUSB311_CC_TERM_RP << HUSB311_ROLE_CC1_SHIFT) |
              (HUSB311_CC_TERM_RP << HUSB311_ROLE_CC2_SHIFT) |
              (HUSB311_RP_3A0 << HUSB311_ROLE_RP_SHIFT);
        break;

      case HUSB311_ROLE_DRP:

        /* DRP toggling starts from the Rd (sink) half of the cycle, which
         * is what "try-power-role = sink" asks for on this board.  The Rp
         * value applies to the source half and matches the 5 V 3 A
         * source-pdo the connector advertises.
         */

        val = HUSB311_ROLE_DRP |
              (HUSB311_CC_TERM_RD << HUSB311_ROLE_CC1_SHIFT) |
              (HUSB311_CC_TERM_RD << HUSB311_ROLE_CC2_SHIFT) |
              (HUSB311_RP_3A0 << HUSB311_ROLE_RP_SHIFT);
        break;

      default:
        return -EINVAL;
    }

  ret = husb311_write8(priv, HUSB311_REG_ROLE_CTRL, val);
  if (ret < 0)
    {
      return ret;
    }

  priv->role = role;

  /* Look4Connection is only meaningful (and only legal) while DRP toggling
   * is enabled.  A fixed role simply presents its termination.
   */

  if (role == HUSB311_ROLE_DRP)
    {
      ret = husb311_command(priv, HUSB311_CMD_LOOK4CONN);
    }

  return ret;
}

/****************************************************************************
 * Name: husb311_update_vbus
 *
 * Description:
 *   Refresh the cached VBUS measurement from the TCPC ADC.
 *
 ****************************************************************************/

static int husb311_update_vbus(struct husb311_dev_s *priv)
{
  uint16_t raw;
  uint32_t mv;
  int ret;

  ret = husb311_read16(priv, HUSB311_REG_VBUS_VOLTAGE, &raw);
  if (ret < 0)
    {
      return ret;
    }

  mv = (uint32_t)(raw & HUSB311_VBUS_RAW_MASK) * HUSB311_VBUS_LSB_MV;
  mv <<= (raw >> HUSB311_VBUS_SCALE_SHIFT) & HUSB311_VBUS_SCALE_MASK;

  priv->vbus_mv = (uint16_t)(mv > UINT16_MAX ? UINT16_MAX : mv);
  return OK;
}

/****************************************************************************
 * Name: husb311_detach
 *
 * Description:
 *   Drop every piece of connection state.  Called when CC_STATUS reports
 *   both lines open.
 *
 ****************************************************************************/

static void husb311_detach(struct husb311_dev_s *priv)
{
  if (priv->attached)
    {
      uinfo("HUSB311: detached\n");
    }

  priv->attached = false;
  priv->orientation = HUSB311_ORIENT_NONE;
  priv->partner = HUSB311_PARTNER_NONE;
  priv->cc1 = HUSB311_CC_OPEN;
  priv->cc2 = HUSB311_CC_OPEN;
  priv->pd_state = HUSB311_PD_DISABLED;
  priv->explicit_contract = false;
  priv->nr_src_pdo = 0;
  priv->nr_dp_modes = 0;
  priv->dp_state = HUSB311_DP_IDLE;
  priv->rdo = 0;
  priv->msgid = 0;
  priv->deadline = 0;

  /* Stop accepting PD traffic and stop sinking VBUS while unattached */

  husb311_write8(priv, HUSB311_REG_RECEIVE_DETECT, 0);
  husb311_command(priv, HUSB311_CMD_DIS_SINK_VBUS);
}

/****************************************************************************
 * Name: husb311_update_cc
 *
 * Description:
 *   Read CC_STATUS / POWER_STATUS, work out the attachment, the plug
 *   orientation and the partner role, and kick the PD sink policy engine
 *   when a source has just been attached.
 *
 ****************************************************************************/

static int husb311_update_cc(struct husb311_dev_s *priv)
{
  uint8_t ccstat;
  uint8_t pwrstat;
  uint8_t raw1;
  uint8_t raw2;
  bool sink_view;
  bool was_attached;
  int ret;

  ret = husb311_read8(priv, HUSB311_REG_CC_STATUS, &ccstat);
  if (ret < 0)
    {
      return ret;
    }

  ret = husb311_read8(priv, HUSB311_REG_POWER_STATUS, &pwrstat);
  if (ret < 0)
    {
      return ret;
    }

  priv->vbus_present = (pwrstat & HUSB311_PWR_STAT_VBUS_PRES) != 0;
  husb311_update_vbus(priv);

  if ((ccstat & HUSB311_CC_STATUS_LOOKING) != 0)
    {
      /* Still toggling: the CC fields are not valid yet */

      return OK;
    }

  sink_view = (ccstat & HUSB311_CC_STATUS_CONNRES) != 0;
  raw1 = (ccstat & HUSB311_CC_STATUS_CC1_MASK) >> HUSB311_CC_STATUS_CC1_SHIFT;
  raw2 = (ccstat & HUSB311_CC_STATUS_CC2_MASK) >> HUSB311_CC_STATUS_CC2_SHIFT;

  priv->cc1 = husb311_decode_cc(raw1, sink_view);
  priv->cc2 = husb311_decode_cc(raw2, sink_view);

  was_attached = priv->attached;

  if (priv->cc1 == HUSB311_CC_OPEN && priv->cc2 == HUSB311_CC_OPEN)
    {
      husb311_detach(priv);
      return OK;
    }

  /* Exactly one CC line carries the partner; the other may show Ra when a
   * powered (e-marked) cable is used.  Ra on both lines is an audio
   * accessory.
   */

  if (priv->cc1 == HUSB311_CC_RA && priv->cc2 == HUSB311_CC_RA)
    {
      priv->partner = HUSB311_PARTNER_AUDIO;
      priv->orientation = HUSB311_ORIENT_NONE;
      priv->attached = true;
      return OK;
    }

  if (priv->cc1 != HUSB311_CC_OPEN && priv->cc1 != HUSB311_CC_RA)
    {
      priv->orientation = HUSB311_ORIENT_CC1;
    }
  else
    {
      priv->orientation = HUSB311_ORIENT_CC2;
    }

  if ((pwrstat & HUSB311_PWR_STAT_DEBUG_ACC) != 0)
    {
      priv->partner = HUSB311_PARTNER_DEBUG;
    }
  else if (sink_view)
    {
      priv->partner = HUSB311_PARTNER_SOURCE;
    }
  else
    {
      priv->partner = HUSB311_PARTNER_SINK;
    }

  priv->attached = true;

  /* Tell the PHY which CC line to run BMC signalling on */

  ret = husb311_write8(
      priv, HUSB311_REG_TCPC_CTRL,
      priv->orientation == HUSB311_ORIENT_CC2 ? HUSB311_TCPC_CTRL_ORIENT : 0);
  if (ret < 0)
    {
      return ret;
    }

  if (!was_attached)
    {
      uinfo("HUSB311: attached cc1=%u cc2=%u orient=CC%u partner=%u\n",
            priv->cc1, priv->cc2, priv->orientation, priv->partner);
    }

  if (priv->partner == HUSB311_PARTNER_SOURCE)
    {
      /* Sink path: start drawing Type-C current immediately, then wait for
       * the source to advertise its capabilities over PD.  If it never
       * does, the Type-C contract implied by the Rp level stays in force.
       */

      husb311_command(priv, HUSB311_CMD_SINK_VBUS);

      if (priv->pd_state == HUSB311_PD_DISABLED)
        {
          husb311_pd_reset(priv, false);
          priv->pd_state = HUSB311_PD_WAIT_SRCCAP;
          husb311_arm_timer(priv, HUSB311_T_TYPEC_SINK_WAIT_MS);
        }
    }
  else if (priv->partner == HUSB311_PARTNER_SINK)
    {
      /* Source path is limited to the Type-C contract implied by the Rp
       * value programmed in ROLE_CONTROL (5 V 3 A, matching source-pdo).
       *
       * TODO: implement the PD source policy engine (Source_Capabilities
       * advertisement and Request evaluation).  It needs board control of
       * the VBUS regulator, which the vbus-supply regulator node in the
       * device tree describes but which has no NuttX driver yet.
       */

      husb311_pd_reset(priv, false);
      husb311_command(priv, HUSB311_CMD_SRC_VBUS_DFLT);
    }

  return OK;
}

/****************************************************************************
 * Name: husb311_arm_timer
 *
 * Description:
 *   Arm the single software timer used by the policy engine.
 *
 ****************************************************************************/

static void husb311_arm_timer(struct husb311_dev_s *priv, unsigned int ms)
{
  priv->deadline = clock_systime_ticks() + MSEC2TICK(ms);

  /* Zero doubles as "disarmed", so never store it */

  if (priv->deadline == 0)
    {
      priv->deadline = 1;
    }
}

/****************************************************************************
 * Name: husb311_transmit
 *
 * Description:
 *   Push one PD message into the TCPC transmit buffer and start the
 *   transmission.  The byte count, header and data objects are written as
 *   one burst starting at TX_BYTE_CNT, which is how the TCPCI register
 *   block is laid out.
 *
 * Input Parameters:
 *   sop    - HUSB311_TX_* ordered set.
 *   header - 16-bit PD message header (ignored for hard/cable reset).
 *   objs   - Data objects, may be NULL when ndo is zero.
 *   ndo    - Number of data objects.
 *
 ****************************************************************************/

static int husb311_transmit(struct husb311_dev_s *priv, uint8_t sop,
                            uint16_t header, const uint32_t *objs,
                            unsigned int ndo)
{
  uint8_t buf[1 + 2 + 4 * HUSB311_MAX_PDO];
  unsigned int i;
  size_t len;
  int ret;

  if (ndo > HUSB311_MAX_PDO)
    {
      return -EINVAL;
    }

  if (sop != HUSB311_TX_HARD_RESET && sop != HUSB311_TX_CABLE_RESET)
    {
      buf[0] = (uint8_t)(2 + 4 * ndo);
      buf[1] = (uint8_t)(header & 0xff);
      buf[2] = (uint8_t)(header >> 8);

      for (i = 0; i < ndo; i++)
        {
          buf[3 + 4 * i] = (uint8_t)(objs[i] & 0xff);
          buf[4 + 4 * i] = (uint8_t)((objs[i] >> 8) & 0xff);
          buf[5 + 4 * i] = (uint8_t)((objs[i] >> 16) & 0xff);
          buf[6 + 4 * i] = (uint8_t)((objs[i] >> 24) & 0xff);
        }

      len = 3 + 4 * ndo;

      ret = husb311_write(priv, HUSB311_REG_TX_BYTE_CNT, buf, len);
      if (ret < 0)
        {
          return ret;
        }
    }

  return husb311_write8(
      priv, HUSB311_REG_TRANSMIT,
      sop | (HUSB311_TX_RETRY_COUNT << HUSB311_TX_RETRY_SHIFT));
}

/****************************************************************************
 * Name: husb311_send_ctrl
 *
 * Description:
 *   Send a PD control message (no data objects) as a sink UFP.
 *
 ****************************************************************************/

static int husb311_send_ctrl(struct husb311_dev_s *priv, uint8_t type)
{
  uint16_t header;

  /* Power role Sink (0) and data role UFP (0) are both encoded as zero */

  header = (uint16_t)(type & PD_HDR_TYPE_MASK) |
           (PD_REV_2_0 << PD_HDR_REV_SHIFT) |
           ((uint16_t)(priv->msgid & PD_HDR_MSGID_MASK) << PD_HDR_MSGID_SHIFT);

  priv->msgid = (priv->msgid + 1) & PD_HDR_MSGID_MASK;

  return husb311_transmit(priv, HUSB311_TX_SOP, header, NULL, 0);
}

/****************************************************************************
 * Name: husb311_send_data
 *
 * Description:
 *   Send a PD data message carrying 'ndo' data objects.
 *
 ****************************************************************************/

static int husb311_send_data(struct husb311_dev_s *priv, uint8_t type,
                             const uint32_t *objs, unsigned int ndo)
{
  uint16_t header;

  if (ndo == 0 || ndo > HUSB311_MAX_PDO)
    {
      return -EINVAL;
    }

  header =
      (uint16_t)(type & PD_HDR_TYPE_MASK) | (PD_REV_2_0 << PD_HDR_REV_SHIFT) |
      ((uint16_t)(priv->msgid & PD_HDR_MSGID_MASK) << PD_HDR_MSGID_SHIFT) |
      ((uint16_t)ndo << PD_HDR_NDO_SHIFT);

  priv->msgid = (priv->msgid + 1) & PD_HDR_MSGID_MASK;

  return husb311_transmit(priv, HUSB311_TX_SOP, header, objs, ndo);
}

/****************************************************************************
 * Name: husb311_send_hard_reset
 ****************************************************************************/

static int husb311_send_hard_reset(struct husb311_dev_s *priv)
{
  husb311_pd_reset(priv, true);
  return husb311_transmit(priv, HUSB311_TX_HARD_RESET, 0, NULL, 0);
}

/****************************************************************************
 * Name: husb311_send_vdm
 *
 * Description:
 *   Send a structured Vendor Defined Message.  'extra' carries the optional
 *   VDOs that follow the VDM header.
 *
 ****************************************************************************/

static int husb311_send_vdm(struct husb311_dev_s *priv, uint16_t svid,
                            uint8_t cmd, const uint32_t *extra,
                            unsigned int nextra)
{
  uint32_t objs[HUSB311_MAX_PDO];
  unsigned int i;

  if (nextra + 1 > HUSB311_MAX_PDO)
    {
      return -EINVAL;
    }

  objs[0] = ((uint32_t)svid << PD_VDM_SVID_SHIFT) | PD_VDM_STRUCTURED |
            ((uint32_t)PD_VDM_VERSION_1_0 << PD_VDM_VERSION_SHIFT) |
            ((uint32_t)PD_VDM_CMDTYPE_REQ << PD_VDM_CMDTYPE_SHIFT) |
            (cmd & PD_VDM_CMD_MASK);

  for (i = 0; i < nextra; i++)
    {
      objs[i + 1] = extra[i];
    }

  return husb311_send_data(priv, PD_DATA_VENDOR_DEFINED, objs, nextra + 1);
}

/****************************************************************************
 * Name: husb311_select_pdo
 *
 * Description:
 *   Pick the best source capability this board may consume and build the
 *   matching fixed Request Data Object.  The connector only advertises a
 *   5 V sink PDO, so the search is limited to fixed supplies at or below
 *   HUSB311_SINK_MAX_MV and simply takes the highest voltage available,
 *   which in practice is always the mandatory vSafe5V object.
 *
 * Returned Value:
 *   The one-based object position on success, a negated errno on failure.
 *
 ****************************************************************************/

static int husb311_select_pdo(struct husb311_dev_s *priv, uint32_t *rdo)
{
  unsigned int best = 0;
  uint32_t best_mv = 0;
  uint32_t best_ma = 0;
  unsigned int i;
  uint32_t want_ma;
  bool mismatch;

  /* Operating current asked for by our own sink PDO */

  want_ma =
      (HUSB311_SINK_PDO & PD_PDO_FIXED_CURR_MASK) * PD_PDO_FIXED_CURR_LSB_MA;

  for (i = 0; i < priv->nr_src_pdo; i++)
    {
      uint32_t pdo = priv->src_pdo[i];
      uint32_t mv;
      uint32_t ma;

      if ((pdo & PD_PDO_TYPE_MASK) != PD_PDO_TYPE_FIXED)
        {
          /* Battery, variable and augmented (PPS) supplies are not used by
           * this board.
           */

          continue;
        }

      mv = ((pdo >> PD_PDO_FIXED_VOLT_SHIFT) & PD_PDO_FIXED_VOLT_MASK) *
           PD_PDO_FIXED_VOLT_LSB_MV;
      ma = (pdo & PD_PDO_FIXED_CURR_MASK) * PD_PDO_FIXED_CURR_LSB_MA;

      if (mv > HUSB311_SINK_MAX_MV || mv < best_mv)
        {
          continue;
        }

      best = i + 1;
      best_mv = mv;
      best_ma = ma;
    }

  if (best == 0)
    {
      return -ENOENT;
    }

  mismatch = best_ma < want_ma;

  *rdo = ((uint32_t)best << PD_RDO_OBJPOS_SHIFT) | PD_RDO_USB_COMM |
         PD_RDO_NO_SUSPEND | (mismatch ? PD_RDO_CAP_MISMATCH : 0) |
         (((mismatch ? best_ma : want_ma) / PD_PDO_FIXED_CURR_LSB_MA)
          << PD_RDO_OP_CURR_SHIFT) |
         ((mismatch ? best_ma : want_ma) / PD_PDO_FIXED_CURR_LSB_MA);

  uinfo("HUSB311: selecting PDO%u %" PRIu32 " mV %" PRIu32 " mA%s\n", best,
        best_mv, best_ma, mismatch ? " (mismatch)" : "");

  return (int)best;
}

/****************************************************************************
 * Name: husb311_send_request
 ****************************************************************************/

static int husb311_send_request(struct husb311_dev_s *priv)
{
  uint32_t rdo = 0;
  int ret;

  ret = husb311_select_pdo(priv, &rdo);
  if (ret < 0)
    {
      uerr("ERROR: no usable source capability: %d\n", ret);
      return ret;
    }

  ret = husb311_send_data(priv, PD_DATA_REQUEST, &rdo, 1);
  if (ret < 0)
    {
      return ret;
    }

  priv->rdo = rdo;
  priv->pd_state = HUSB311_PD_REQ_SENT;
  husb311_arm_timer(priv, HUSB311_T_SENDER_RSP_MS);

  return OK;
}

/****************************************************************************
 * Name: husb311_handle_vdm
 *
 * Description:
 *   Walk the DisplayPort alternate mode discovery chain: Discover Identity,
 *   Discover SVIDs, Discover Modes.
 *
 ****************************************************************************/

static void husb311_handle_vdm(struct husb311_dev_s *priv,
                               const uint32_t *objs, unsigned int ndo)
{
  unsigned int cmdtype;
  unsigned int cmd;
  uint16_t svid;
  unsigned int i;

  if (ndo == 0 || (objs[0] & PD_VDM_STRUCTURED) == 0)
    {
      /* Unstructured VDMs are ignored */

      return;
    }

  svid = (uint16_t)(objs[0] >> PD_VDM_SVID_SHIFT);
  cmd = objs[0] & PD_VDM_CMD_MASK;
  cmdtype = (objs[0] >> PD_VDM_CMDTYPE_SHIFT) & PD_VDM_CMDTYPE_MASK;

  if (cmdtype == PD_VDM_CMDTYPE_REQ)
    {
      /* We initiate discovery but never answer it: reply NAK so the
       * partner does not wait for tVDMSenderResponse.
       */

      uint32_t nak = (objs[0] & ~((uint32_t)PD_VDM_CMDTYPE_MASK
                                  << PD_VDM_CMDTYPE_SHIFT)) |
                     ((uint32_t)PD_VDM_CMDTYPE_NAK << PD_VDM_CMDTYPE_SHIFT);

      husb311_send_data(priv, PD_DATA_VENDOR_DEFINED, &nak, 1);
      return;
    }

  if (cmdtype == PD_VDM_CMDTYPE_NAK)
    {
      uinfo("HUSB311: VDM cmd %u NAKed by svid 0x%04x\n", cmd, svid);
      priv->dp_state = HUSB311_DP_UNSUPPORTED;
      priv->deadline = 0;
      return;
    }

  /* ACK */

  priv->deadline = 0;

  switch (cmd)
    {
      case PD_VDM_CMD_DISC_IDENTITY:
        priv->dp_state = HUSB311_DP_DISC_SVIDS;
        husb311_send_vdm(priv, PD_SID_PD, PD_VDM_CMD_DISC_SVIDS, NULL, 0);
        husb311_arm_timer(priv, HUSB311_T_VDM_RSP_MS);
        break;

      case PD_VDM_CMD_DISC_SVIDS:

        /* Each VDO after the header packs two 16-bit SVIDs, high half
         * first.  A zero SVID terminates the list.
         */

        for (i = 1; i < ndo; i++)
          {
            uint16_t hi = (uint16_t)(objs[i] >> 16);
            uint16_t lo = (uint16_t)(objs[i] & 0xffff);

            if (hi == PD_SID_DISPLAYPORT || lo == PD_SID_DISPLAYPORT)
              {
                priv->dp_state = HUSB311_DP_DISC_MODES;
                husb311_send_vdm(priv, PD_SID_DISPLAYPORT,
                                 PD_VDM_CMD_DISC_MODES, NULL, 0);
                husb311_arm_timer(priv, HUSB311_T_VDM_RSP_MS);
                return;
              }
          }

        uinfo("HUSB311: partner does not expose DisplayPort\n");
        priv->dp_state = HUSB311_DP_UNSUPPORTED;
        break;

      case PD_VDM_CMD_DISC_MODES:
        priv->nr_dp_modes = 0;
        for (i = 1; i < ndo && priv->nr_dp_modes < HUSB311_MAX_DP_MODES; i++)
          {
            priv->dp_modes[priv->nr_dp_modes++] = objs[i];
          }

        priv->dp_state = HUSB311_DP_MODES_KNOWN;
        uinfo("HUSB311: DisplayPort svid 0x%04x, %u mode(s) discovered\n",
              PD_SID_DISPLAYPORT, priv->nr_dp_modes);

        /* TODO: enter the mode and drive it.  Entering DP alt mode means
         * Enter Mode, DP Status Update and DP Configure VDMs, plus routing
         * the four SuperSpeed lanes to the DP controller through the
         * board's Type-C mux (usbc0_orien_sw / dp_altmode_mux in the
         * device tree).  Neither the mux nor the RK3576 DP output has a
         * NuttX driver yet, so discovery stops here.
         */

        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: husb311_handle_msg
 *
 * Description:
 *   PD protocol layer.  The TCPC has already answered with GoodCRC, so this
 *   only implements the sink policy engine.
 *
 ****************************************************************************/

static void husb311_handle_msg(struct husb311_dev_s *priv, uint16_t header,
                               const uint32_t *objs, unsigned int ndo)
{
  unsigned int type = PD_HDR_TYPE(header);
  uint32_t sinkpdo = HUSB311_SINK_PDO;
  unsigned int i;

  if (ndo == 0)
    {
      /* Control message */

      switch (type)
        {
          case PD_CTRL_ACCEPT:
            if (priv->pd_state == HUSB311_PD_REQ_SENT)
              {
                priv->pd_state = HUSB311_PD_WAIT_PSRDY;
                husb311_arm_timer(priv, HUSB311_T_PS_TRANSITION_MS);
              }
            break;

          case PD_CTRL_REJECT:
          case PD_CTRL_WAIT:
            if (priv->pd_state == HUSB311_PD_REQ_SENT)
              {
                uwarn("HUSB311: request rejected, staying on 5 V\n");
                priv->pd_state = HUSB311_PD_WAIT_SRCCAP;
                priv->rdo = 0;
                priv->deadline = 0;
              }
            break;

          case PD_CTRL_PS_RDY:
            if (priv->pd_state == HUSB311_PD_WAIT_PSRDY)
              {
                priv->pd_state = HUSB311_PD_READY;
                priv->explicit_contract = true;
                priv->deadline = 0;
                husb311_update_vbus(priv);
                uinfo("HUSB311: explicit contract, vbus=%u mV\n",
                      priv->vbus_mv);

                /* Alternate mode discovery may only start once the
                 * contract exists.
                 */

                if (priv->dp_state == HUSB311_DP_IDLE)
                  {
                    priv->dp_state = HUSB311_DP_DISC_ID;
                    husb311_send_vdm(priv, PD_SID_PD, PD_VDM_CMD_DISC_IDENTITY,
                                     NULL, 0);
                    husb311_arm_timer(priv, HUSB311_T_VDM_RSP_MS);
                  }
              }
            break;

          case PD_CTRL_GET_SINK_CAP:
            husb311_send_data(priv, PD_DATA_SINK_CAP, &sinkpdo, 1);
            break;

          case PD_CTRL_GET_SOURCE_CAP:

            /* We never source over PD; a Reject keeps the partner happy */

            husb311_send_ctrl(priv, PD_CTRL_REJECT);
            break;

          case PD_CTRL_SOFT_RESET:
            husb311_pd_reset(priv, false);
            husb311_send_ctrl(priv, PD_CTRL_ACCEPT);
            priv->pd_state = HUSB311_PD_WAIT_SRCCAP;
            husb311_arm_timer(priv, HUSB311_T_TYPEC_SINK_WAIT_MS);
            break;

          case PD_CTRL_DR_SWAP:
          case PD_CTRL_PR_SWAP:
          case PD_CTRL_VCONN_SWAP:

            /* TODO: role swaps need VBUS regulator and USB controller role
             * switching support, neither of which exists yet.
             */

            husb311_send_ctrl(priv, PD_CTRL_REJECT);
            break;

          default:
            break;
        }

      return;
    }

  /* Data message */

  switch (type)
    {
      case PD_DATA_SOURCE_CAP:
        priv->nr_src_pdo =
            (uint8_t)(ndo > HUSB311_MAX_PDO ? HUSB311_MAX_PDO : ndo);
        for (i = 0; i < priv->nr_src_pdo; i++)
          {
            priv->src_pdo[i] = objs[i];
          }

        husb311_send_request(priv);
        break;

      case PD_DATA_VENDOR_DEFINED:
        husb311_handle_vdm(priv, objs, ndo);
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: husb311_receive
 *
 * Description:
 *   Drain one message from the TCPC receive buffer.  The byte count, frame
 *   type, header and payload are contiguous, so a single block read of
 *   count+1 bytes starting at RECEIVE_BYTE_COUNT gets everything.
 *
 ****************************************************************************/

static int husb311_receive(struct husb311_dev_s *priv)
{
  uint8_t buf[1 + 1 + 2 + 4 * HUSB311_MAX_PDO];
  uint32_t objs[HUSB311_MAX_PDO];
  unsigned int ndo;
  uint16_t header;
  uint8_t count;
  unsigned int i;
  int ret;

  ret = husb311_read8(priv, HUSB311_REG_RX_BYTE_CNT, &count);
  if (ret < 0)
    {
      return ret;
    }

  /* count covers frame type + header + data objects */

  if (count < 3 || (size_t)count + 1 > sizeof(buf))
    {
      /* Nothing usable: acknowledge so the buffer is released */

      husb311_write16(priv, HUSB311_REG_ALERT, HUSB311_ALERT_RX_STATUS);
      return -EIO;
    }

  ret = husb311_read(priv, HUSB311_REG_RX_BYTE_CNT, buf, count + 1);
  if (ret < 0)
    {
      return ret;
    }

  /* Releasing the receive buffer is what clearing the RX alert does */

  husb311_write16(priv, HUSB311_REG_ALERT, HUSB311_ALERT_RX_STATUS);

  /* buf[1] is the SOP* frame type; only SOP is consumed here */

  if (buf[1] != HUSB311_TX_SOP)
    {
      return OK;
    }

  header = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);

  if ((header & PD_HDR_EXTENDED) != 0)
    {
      /* TODO: chunked extended messages (PD 3.0) are not supported.  The
       * board negotiates PD 2.0, where no extended message is mandatory.
       */

      return OK;
    }

  ndo = PD_HDR_NDO(header);
  if (ndo > HUSB311_MAX_PDO || (unsigned int)count < 3 + 4 * ndo)
    {
      return -EIO;
    }

  for (i = 0; i < ndo; i++)
    {
      objs[i] = (uint32_t)buf[4 + 4 * i] | ((uint32_t)buf[5 + 4 * i] << 8) |
                ((uint32_t)buf[6 + 4 * i] << 16) |
                ((uint32_t)buf[7 + 4 * i] << 24);
    }

  husb311_handle_msg(priv, header, objs, ndo);
  return OK;
}

/****************************************************************************
 * Name: husb311_pd_reset
 *
 * Description:
 *   Bring the protocol layer back to its initial state.  A hard reset also
 *   invalidates the power contract.
 *
 ****************************************************************************/

static void husb311_pd_reset(struct husb311_dev_s *priv, bool hard)
{
  priv->msgid = 0;
  priv->deadline = 0;
  priv->rdo = 0;

  if (hard)
    {
      priv->explicit_contract = false;
      priv->nr_src_pdo = 0;
      priv->nr_dp_modes = 0;
      priv->dp_state = HUSB311_DP_IDLE;
      priv->pd_state = HUSB311_PD_HARD_RESET;
    }

  /* Accept SOP and hard reset once again */

  husb311_write8(priv, HUSB311_REG_RECEIVE_DETECT,
                 HUSB311_RXDET_SOP | HUSB311_RXDET_HARD_RESET);

  /* Sink, UFP, PD revision 2.0 */

  husb311_write8(priv, HUSB311_REG_MSG_HDR_INFO,
                 PD_REV_2_0 << HUSB311_MHI_REV_SHIFT);
}

/****************************************************************************
 * Name: husb311_check_timer
 *
 * Description:
 *   Expire the policy engine timer if it is armed and overdue.
 *
 ****************************************************************************/

static void husb311_check_timer(struct husb311_dev_s *priv)
{
  if (priv->deadline == 0 ||
      (sclock_t)(clock_systime_ticks() - priv->deadline) < 0)
    {
      return;
    }

  priv->deadline = 0;

  switch (priv->pd_state)
    {
      case HUSB311_PD_WAIT_SRCCAP:

        /* No Source_Capabilities arrived.  The partner is a plain Type-C
         * source; the Rp based contract remains in force and nothing else
         * has to happen.
         */

        uinfo("HUSB311: no PD source capabilities, Type-C only supply\n");
        priv->pd_state = HUSB311_PD_DISABLED;
        break;

      case HUSB311_PD_REQ_SENT:
      case HUSB311_PD_WAIT_PSRDY:

        /* tSenderResponse / tPSTransition expired: the spec mandates a
         * hard reset.
         */

        uwarn("HUSB311: PD response timeout in state %u, hard reset\n",
              priv->pd_state);
        husb311_send_hard_reset(priv);
        break;

      case HUSB311_PD_READY:
        if (priv->dp_state == HUSB311_DP_DISC_ID ||
            priv->dp_state == HUSB311_DP_DISC_SVIDS ||
            priv->dp_state == HUSB311_DP_DISC_MODES)
          {
            uinfo("HUSB311: alt mode discovery timed out\n");
            priv->dp_state = HUSB311_DP_UNSUPPORTED;
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Name: husb311_alert_isr
 *
 * Description:
 *   GPIO bank interrupt handler for the active low ALERT line.  I2C cannot
 *   be touched here, so the handler only acknowledges the pin at the GPIO
 *   controller and wakes the service thread.
 *
 ****************************************************************************/

static int husb311_alert_isr(int irq, void *context, void *arg)
{
  struct husb311_dev_s *priv = (struct husb311_dev_s *)arg;
  uint32_t status;

  status = getreg32(RK3576_GPIO_INT_STATUS(HUSB311_ALERT_PORT));

  if ((status & RK3576_GPIO_PIN_BIT(HUSB311_ALERT_PIN)) == 0)
    {
      /* Another pin in this group; leave it to its owner */

      return OK;
    }

  /* Mask until the thread has serviced the TCPC, otherwise the level
   * triggered line re-asserts the interrupt immediately.
   */

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(HUSB311_ALERT_PORT),
                           HUSB311_ALERT_PIN, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(HUSB311_ALERT_PORT),
                           HUSB311_ALERT_PIN, 1);

  nxsem_post(&priv->alert);
  return OK;
}

/****************************************************************************
 * Name: husb311_service
 *
 * Description:
 *   Handle every pending ALERT source, then run the policy engine timer.
 *   Runs on the service thread with the device lock held.
 *
 ****************************************************************************/

static void husb311_service(struct husb311_dev_s *priv)
{
  unsigned int guard;
  uint16_t alert;
  uint16_t acked;
  uint8_t fault;

  for (guard = 0; guard < 16; guard++)
    {
      if (husb311_read16(priv, HUSB311_REG_ALERT, &alert) < 0)
        {
          break;
        }

      if (alert == 0)
        {
          break;
        }

      /* The RX alert is cleared inside husb311_receive() together with the
       * buffer release, so keep it out of the bulk acknowledge.
       */

      acked = alert & ~HUSB311_ALERT_RX_STATUS;
      if (acked != 0)
        {
          husb311_write16(priv, HUSB311_REG_ALERT, acked);
        }

      if ((alert & HUSB311_ALERT_FAULT) != 0)
        {
          if (husb311_read8(priv, HUSB311_REG_FAULT_STATUS, &fault) >= 0)
            {
              uwarn("HUSB311: fault status 0x%02x\n", fault);
              husb311_write8(priv, HUSB311_REG_FAULT_STATUS, fault);
            }
        }

      if ((alert & HUSB311_ALERT_RX_HARD_RST) != 0)
        {
          uwarn("HUSB311: hard reset received\n");
          husb311_pd_reset(priv, true);
          priv->pd_state = HUSB311_PD_WAIT_SRCCAP;
          husb311_arm_timer(priv, HUSB311_T_TYPEC_SINK_WAIT_MS);
        }

      if ((alert & (HUSB311_ALERT_CC_STATUS | HUSB311_ALERT_VBUS_SNK_DISC)) !=
          0)
        {
          husb311_update_cc(priv);
        }

      if ((alert & HUSB311_ALERT_POWER_STATUS) != 0)
        {
          husb311_update_cc(priv);
        }

      if ((alert & HUSB311_ALERT_RX_OVERFLOW) != 0)
        {
          uwarn("HUSB311: receive buffer overflow\n");
        }

      if ((alert & HUSB311_ALERT_RX_STATUS) != 0)
        {
          husb311_receive(priv);
        }

      if ((alert & (HUSB311_ALERT_TX_FAILED | HUSB311_ALERT_TX_DISCARD)) != 0)
        {
          /* The TCPC already retried nRetryCount times.  A discard means
           * an incoming message won the race and our transmission has to
           * be repeated by the policy engine timer.
           */

          uwarn("HUSB311: transmit not delivered (alert 0x%04x)\n", alert);
        }
    }

  husb311_check_timer(priv);
}

/****************************************************************************
 * Name: husb311_thread
 *
 * Description:
 *   Service thread.  Sleeps on the ALERT semaphore and falls back to a
 *   periodic wakeup so the software timers keep running even if an edge is
 *   ever missed.
 *
 ****************************************************************************/

static int husb311_thread(int argc, char **argv)
{
  struct husb311_dev_s *priv = &g_husb311;

  /* Service once at startup: a cable may already be plugged in */

  nxmutex_lock(&priv->lock);
  husb311_update_cc(priv);
  husb311_service(priv);
  nxmutex_unlock(&priv->lock);

  while (priv->running)
    {
      nxsem_tickwait(&priv->alert, MSEC2TICK(HUSB311_POLL_MS));

      nxmutex_lock(&priv->lock);
      husb311_service(priv);
      nxmutex_unlock(&priv->lock);

      /* Re-arm the level triggered ALERT line now that the TCPC has been
       * drained and has released it.
       */

      RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTMASK(HUSB311_ALERT_PORT),
                               HUSB311_ALERT_PIN, 0);
    }

  return 0;
}

/****************************************************************************
 * Name: husb311_dev_read
 *
 * Description:
 *   Return a human readable snapshot of the port state, so that a plain
 *   "cat /dev/typec0" is enough to inspect the connection from NSH.
 *
 ****************************************************************************/

static ssize_t husb311_dev_read(struct file *filep, char *buffer,
                                size_t buflen)
{
  struct husb311_dev_s *priv = &g_husb311;
  char snapshot[HUSB311_SNAPSHOT_SIZE];
  size_t len;
  int total;
  int i;

  if (filep->f_pos > 0)
    {
      return 0;
    }

  nxmutex_lock(&priv->lock);

  total = snprintf(snapshot, sizeof(snapshot),
                   "attached:  %s\n"
                   "cc1/cc2:   %u/%u\n"
                   "orient:    %u\n"
                   "partner:   %u\n"
                   "role:      %u\n"
                   "vbus:      %u mV (%s)\n"
                   "pd_state:  %u\n"
                   "contract:  %s\n"
                   "rdo:       0x%08" PRIx32 "\n"
                   "src_pdos:  %u\n",
                   priv->attached ? "yes" : "no", priv->cc1, priv->cc2,
                   priv->orientation, priv->partner, priv->role, priv->vbus_mv,
                   priv->vbus_present ? "present" : "absent", priv->pd_state,
                   priv->explicit_contract ? "explicit" : "implicit",
                   priv->rdo, priv->nr_src_pdo);

  for (i = 0;
       i < priv->nr_src_pdo && total > 0 && (size_t)total < sizeof(snapshot);
       i++)
    {
      total += snprintf(&snapshot[total], sizeof(snapshot) - total,
                        "  pdo[%d]: 0x%08" PRIx32 "\n", i, priv->src_pdo[i]);
    }

  nxmutex_unlock(&priv->lock);

  if (total < 0)
    {
      return -EIO;
    }

  len = (size_t)total;
  if (len > sizeof(snapshot))
    {
      len = sizeof(snapshot);
    }

  if (len > buflen)
    {
      len = buflen;
    }

  memcpy(buffer, snapshot, len);
  filep->f_pos += len;

  return (ssize_t)len;
}

/****************************************************************************
 * Name: husb311_dev_write
 ****************************************************************************/

static ssize_t husb311_dev_write(struct file *filep, const char *buffer,
                                 size_t buflen)
{
  UNUSED(filep);
  UNUSED(buffer);

  /* Writes are meaningless but must not look like an error to shell
   * redirection; report everything consumed.
   */

  return (ssize_t)buflen;
}

/****************************************************************************
 * Name: husb311_dev_ioctl
 ****************************************************************************/

static int husb311_dev_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  struct husb311_dev_s *priv = &g_husb311;
  int ret = OK;

  UNUSED(filep);

  switch (cmd)
    {
      case HUSB311_IOC_GET_STATUS:
        {
          struct husb311_status_s *status =
              (struct husb311_status_s *)((uintptr_t)arg);

          if (status == NULL)
            {
              return -EINVAL;
            }

          ret = kickpi_k7_husb311_get_cc_status(status);
        }
        break;

      case HUSB311_IOC_GET_VBUS:
        {
          uint16_t *mv = (uint16_t *)((uintptr_t)arg);

          if (mv == NULL)
            {
              return -EINVAL;
            }

          ret = kickpi_k7_husb311_get_vbus_mv(mv);
        }
        break;

      case HUSB311_IOC_SET_ROLE:
        ret = kickpi_k7_husb311_set_role((int)arg);
        break;

      case HUSB311_IOC_GET_CAPS:
        {
          struct husb311_caps_s *caps =
              (struct husb311_caps_s *)((uintptr_t)arg);

          if (caps == NULL)
            {
              return -EINVAL;
            }

          nxmutex_lock(&priv->lock);
          caps->nr_pdo = priv->nr_src_pdo;
          memcpy(caps->pdo, priv->src_pdo, sizeof(caps->pdo));
          nxmutex_unlock(&priv->lock);
        }
        break;

      case HUSB311_IOC_HARD_RESET:
        nxmutex_lock(&priv->lock);
        ret = husb311_send_hard_reset(priv);
        nxmutex_unlock(&priv->lock);
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: kickpi_k7_husb311_initialize
 *
 * Description:
 *   See kickpi_k7_husb311.h.
 *
 ****************************************************************************/

int kickpi_k7_husb311_initialize(struct i2c_master_s *i2c)
{
  struct husb311_dev_s *priv = &g_husb311;
  uint16_t vendor;
  uint16_t product;
  uint16_t device;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  if (priv->running)
    {
      /* Already brought up */

      return OK;
    }

  priv->i2c = i2c;

  /* Take the part out of any low power state before the first read */

  husb311_command(priv, HUSB311_CMD_WAKE_I2C);

  ret = husb311_read16(priv, HUSB311_REG_VENDOR_ID, &vendor);
  if (ret < 0)
    {
      uerr("ERROR: HUSB311 not responding at 0x%02x: %d\n", HUSB311_I2C_ADDR,
           ret);
      return ret;
    }

  husb311_read16(priv, HUSB311_REG_PRODUCT_ID, &product);
  husb311_read16(priv, HUSB311_REG_DEVICE_ID, &device);

  uinfo("HUSB311: vendor 0x%04x product 0x%04x device 0x%04x\n", vendor,
        product, device);

  nxsem_init(&priv->alert, 0, 0);

  priv->cc1 = HUSB311_CC_OPEN;
  priv->cc2 = HUSB311_CC_OPEN;
  priv->orientation = HUSB311_ORIENT_NONE;
  priv->partner = HUSB311_PARTNER_NONE;
  priv->pd_state = HUSB311_PD_DISABLED;
  priv->dp_state = HUSB311_DP_IDLE;

  /* Mask everything while the TCPC is being configured */

  husb311_write16(priv, HUSB311_REG_ALERT_MASK, 0);
  husb311_write16(priv, HUSB311_REG_ALERT, 0xffff);

  /* Route CC/VBUS/fault events to the ALERT pin, no VCONN sourcing (the
   * board has no VCONN switch) and keep the voltage monitor running so
   * VBUS_VOLTAGE stays valid.
   */

  husb311_write8(priv, HUSB311_REG_POWER_CTRL, 0);
  husb311_write8(priv, HUSB311_REG_FAULT_CTRL, 0);
  husb311_write8(priv, HUSB311_REG_STD_OUT_CFG, 0);
  husb311_write8(priv, HUSB311_REG_POWER_STAT_MSK, 0xff);
  husb311_write8(priv, HUSB311_REG_FAULT_STAT_MSK, 0xff);
  husb311_command(priv, HUSB311_CMD_EN_VBUS_DET);

  /* PD protocol layer defaults: sink, UFP, revision 2.0 */

  husb311_pd_reset(priv, false);

  /* Dual role port starting on the sink half of the DRP cycle, matching
   * "power-role = dual" plus "try-power-role = sink".
   */

  ret = husb311_apply_role(priv, HUSB311_ROLE_DRP);
  if (ret < 0)
    {
      uerr("ERROR: failed to program ROLE_CONTROL: %d\n", ret);
      return ret;
    }

  /* ALERT line: input, pull-up, level low */

  ret = rk3576_config_gpio(HUSB311_ALERT_PINSET);
  if (ret < 0)
    {
      uerr("ERROR: failed to configure ALERT pin: %d\n", ret);
      return ret;
    }

  /* rk3576_config_gpio() programs type/polarity and clears INTMASK, but
   * INTEN is only initialised by the optional /dev/gpio driver.  Set it
   * here so the driver works without CONFIG_DEV_GPIO too.
   */

  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_INTEN(HUSB311_ALERT_PORT),
                           HUSB311_ALERT_PIN, 1);
  RK3576_GPIO_V2_WRITE_BIT(RK3576_GPIO_PORTA_EOI(HUSB311_ALERT_PORT),
                           HUSB311_ALERT_PIN, 1);

  ret = irq_attach(HUSB311_ALERT_IRQ, husb311_alert_isr, priv);
  if (ret < 0)
    {
      uerr("ERROR: failed to attach IRQ %d: %d\n", HUSB311_ALERT_IRQ, ret);
      return ret;
    }

  up_enable_irq(HUSB311_ALERT_IRQ);

  /* Unmask the TCPC alert sources now that the handler is in place */

  husb311_write16(priv, HUSB311_REG_ALERT_MASK, HUSB311_ALERT_ENABLED);

  ret = register_driver(HUSB311_DEVPATH, &g_husb311_fops, 0444, priv);
  if (ret < 0)
    {
      uerr("ERROR: failed to register %s: %d\n", HUSB311_DEVPATH, ret);
      goto err_irq;
    }

  priv->running = true;

  ret = kthread_create(HUSB311_THREAD_NAME, HUSB311_THREAD_PRIO,
                       HUSB311_THREAD_STACK, husb311_thread, NULL);
  if (ret < 0)
    {
      uerr("ERROR: failed to start service thread: %d\n", ret);
      priv->running = false;
      unregister_driver(HUSB311_DEVPATH);
      goto err_irq;
    }

  priv->pid = (pid_t)ret;

  uinfo("HUSB311: registered %s (DRP, try.SNK)\n", HUSB311_DEVPATH);
  return OK;

err_irq:
  up_disable_irq(HUSB311_ALERT_IRQ);
  irq_detach(HUSB311_ALERT_IRQ);
  husb311_write16(priv, HUSB311_REG_ALERT_MASK, 0);
  return ret;
}

/****************************************************************************
 * Name: kickpi_k7_husb311_get_cc_status
 ****************************************************************************/

int kickpi_k7_husb311_get_cc_status(struct husb311_status_s *status)
{
  struct husb311_dev_s *priv = &g_husb311;

  if (status == NULL)
    {
      return -EINVAL;
    }

  if (priv->i2c == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&priv->lock);

  status->cc1 = priv->cc1;
  status->cc2 = priv->cc2;
  status->orientation = priv->orientation;
  status->partner = priv->partner;
  status->role = priv->role;
  status->pd_state = priv->pd_state;
  status->attached = priv->attached;
  status->vbus_present = priv->vbus_present;
  status->explicit_contract = priv->explicit_contract;
  status->vbus_mv = priv->vbus_mv;
  status->rdo = priv->rdo;

  nxmutex_unlock(&priv->lock);
  return OK;
}

/****************************************************************************
 * Name: kickpi_k7_husb311_get_vbus_mv
 ****************************************************************************/

int kickpi_k7_husb311_get_vbus_mv(uint16_t *mv)
{
  struct husb311_dev_s *priv = &g_husb311;
  int ret;

  if (mv == NULL)
    {
      return -EINVAL;
    }

  if (priv->i2c == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&priv->lock);
  ret = husb311_update_vbus(priv);
  *mv = priv->vbus_mv;
  nxmutex_unlock(&priv->lock);

  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: kickpi_k7_husb311_set_role
 ****************************************************************************/

int kickpi_k7_husb311_set_role(int role)
{
  struct husb311_dev_s *priv = &g_husb311;
  int ret;

  if (role != HUSB311_ROLE_SINK && role != HUSB311_ROLE_SOURCE &&
      role != HUSB311_ROLE_DRP)
    {
      return -EINVAL;
    }

  if (priv->i2c == NULL)
    {
      return -ENODEV;
    }

  nxmutex_lock(&priv->lock);

  husb311_detach(priv);
  ret = husb311_apply_role(priv, (uint8_t)role);
  if (ret >= 0)
    {
      husb311_pd_reset(priv, false);
    }

  nxmutex_unlock(&priv->lock);
  return ret < 0 ? ret : OK;
}

#endif /* CONFIG_KICKPI_K7_HUSB311 */
