/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_husb311.h
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

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HUSB311_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HUSB311_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Character device published by this driver */

#define HUSB311_DEVPATH        "/dev/typec0"

/* Maximum number of Power Data Objects carried by a single PD message */

#define HUSB311_MAX_PDO        7

/* CC line termination states as reported by the TCPC CC_STATUS register.
 * The meaning of the raw 2-bit code depends on whether the port is
 * presenting Rp (source view) or Rd (sink view); the driver normalises it
 * into the enumeration below.
 */

#define HUSB311_CC_OPEN        0  /* Nothing attached on this CC line */
#define HUSB311_CC_RA          1  /* Ra: powered cable / VCONN request */
#define HUSB311_CC_RD          2  /* Rd: a sink is attached */
#define HUSB311_CC_RP_DEFAULT  3  /* Rp presenting default USB current */
#define HUSB311_CC_RP_1A5      4  /* Rp presenting 1.5 A @ 5 V */
#define HUSB311_CC_RP_3A0      5  /* Rp presenting 3.0 A @ 5 V */

/* Cable orientation (which CC line carries the attached partner) */

#define HUSB311_ORIENT_NONE    0
#define HUSB311_ORIENT_CC1     1
#define HUSB311_ORIENT_CC2     2

/* Attached partner classification */

#define HUSB311_PARTNER_NONE   0
#define HUSB311_PARTNER_SINK   1  /* We are the source, partner sinks */
#define HUSB311_PARTNER_SOURCE 2  /* We are the sink, partner sources */
#define HUSB311_PARTNER_DEBUG  3  /* Debug accessory */
#define HUSB311_PARTNER_AUDIO  4  /* Audio accessory (Ra/Ra) */

/* Port role requested through HUSB311_IOC_SET_ROLE */

#define HUSB311_ROLE_SINK      0  /* Present Rd only */
#define HUSB311_ROLE_SOURCE    1  /* Present Rp only */
#define HUSB311_ROLE_DRP       2  /* Dual-role, try.SNK (board default) */

/* USB Power Delivery policy engine state (sink path) */

#define HUSB311_PD_DISABLED    0  /* No connection / PD not running */
#define HUSB311_PD_WAIT_SRCCAP 1  /* Attached, waiting Source_Capabilities */
#define HUSB311_PD_REQ_SENT    2  /* Request sent, waiting Accept */
#define HUSB311_PD_WAIT_PSRDY  3  /* Accept seen, waiting PS_RDY */
#define HUSB311_PD_READY       4  /* Explicit contract in place */
#define HUSB311_PD_HARD_RESET  5  /* Hard reset in progress */

/* ioctl commands.
 *
 * NuttX has no USB Power Delivery / Type-C class, therefore no ioctl base
 * is reserved for it in include/nuttx/fs/ioctl.h.  A private base outside
 * the ranges used by the core subsystems is used instead; only this driver
 * decodes these commands.
 */

#define HUSB311_IOC_BASE       0x8b00

#define HUSB311_IOC_GET_STATUS (HUSB311_IOC_BASE + 0x01) /* struct
                                                          * husb311_status_s *
                                                          */
#define HUSB311_IOC_GET_VBUS   (HUSB311_IOC_BASE + 0x02) /* uint16_t * (mV) */
#define HUSB311_IOC_SET_ROLE   (HUSB311_IOC_BASE + 0x03) /* int role */
#define HUSB311_IOC_GET_CAPS   (HUSB311_IOC_BASE + 0x04) /* struct
                                                          * husb311_caps_s *
                                                          */
#define HUSB311_IOC_HARD_RESET (HUSB311_IOC_BASE + 0x05) /* no argument */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Snapshot of the Type-C / PD port state */

struct husb311_status_s
{
  uint8_t  cc1;                /* HUSB311_CC_* seen on CC1 */
  uint8_t  cc2;                /* HUSB311_CC_* seen on CC2 */
  uint8_t  orientation;        /* HUSB311_ORIENT_* */
  uint8_t  partner;            /* HUSB311_PARTNER_* */
  uint8_t  role;               /* HUSB311_ROLE_* currently programmed */
  uint8_t  pd_state;           /* HUSB311_PD_* */
  bool     attached;           /* Type-C attachment debounced */
  bool     vbus_present;       /* VBUS above vSafe5V detection threshold */
  bool     explicit_contract;  /* PD contract negotiated */
  uint16_t vbus_mv;            /* Last VBUS measurement in millivolts */
  uint32_t rdo;                /* Request Data Object we sent, 0 if none */
};

/* Source capabilities advertised by the attached partner */

struct husb311_caps_s
{
  uint8_t  nr_pdo;                     /* Number of valid entries below */
  uint32_t pdo[HUSB311_MAX_PDO];       /* Raw Power Data Objects */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: kickpi_k7_husb311_initialize
 *
 * Description:
 *   Probe the HUSB311 USB Type-C port controller sitting at address 0x4e on
 *   I2C2, bring it up as a dual-role port defaulting to sink (Try.SNK as
 *   described by the board connector node), hook the ALERT interrupt line
 *   (GPIO4_D1, active low) and register the /dev/typec0 character device.
 *
 * Input Parameters:
 *   i2c - An initialised I2C2 master instance.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_husb311_initialize(struct i2c_master_s *i2c);

/****************************************************************************
 * Name: kickpi_k7_husb311_get_cc_status
 *
 * Description:
 *   Return the debounced Type-C connection state of the port.
 *
 * Input Parameters:
 *   status - Caller provided buffer filled in on success.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_husb311_get_cc_status(struct husb311_status_s *status);

/****************************************************************************
 * Name: kickpi_k7_husb311_get_vbus_mv
 *
 * Description:
 *   Read the VBUS voltage measured by the TCPC ADC.
 *
 * Input Parameters:
 *   mv - Receives the voltage in millivolts.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_husb311_get_vbus_mv(uint16_t *mv);

/****************************************************************************
 * Name: kickpi_k7_husb311_set_role
 *
 * Description:
 *   Reprogram the port role (sink, source or dual-role) by rewriting the
 *   TCPC ROLE_CONTROL register and restarting connection detection.
 *
 * Input Parameters:
 *   role - One of HUSB311_ROLE_SINK, HUSB311_ROLE_SOURCE, HUSB311_ROLE_DRP.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int kickpi_k7_husb311_set_role(int role);

#ifdef __cplusplus
}
#endif

#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_HUSB311_H */
