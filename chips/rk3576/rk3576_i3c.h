/****************************************************************************
 * chips/rk3576/rk3576_i3c.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_I3C_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_I3C_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>

#include "hardware/rk3576_memorymap.h"

#ifdef CONFIG_RK3576_I3C

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RK3576_I3C_NUM              2  /* I3C0, I3C1 */

/* Maximum number of devices tracked in the device address table (DAT).
 * The real depth is read from DEV_ADDR_TABLE_PTR at init and clamped to
 * this value.
 */

#define RK3576_I3C_MAX_DEVS         8

/* Slot 0 of the DAT is reserved for legacy I2C traffic issued through the
 * i2c_master_s interface; I3C devices discovered by ENTDAA start at slot 1.
 */

#define RK3576_I3C_I2C_DAT_SLOT     0
#define RK3576_I3C_FIRST_I3C_SLOT   1

/* MIPI I3C common command codes used by this driver. */

#define RK3576_I3C_CCC_ENEC_B       0x00 /* Broadcast enable events */
#define RK3576_I3C_CCC_DISEC_B      0x01 /* Broadcast disable events */
#define RK3576_I3C_CCC_RSTDAA_B     0x06 /* Broadcast reset dynamic addr */
#define RK3576_I3C_CCC_ENTDAA       0x07 /* Enter dynamic addr assignment */
#define RK3576_I3C_CCC_SETMWL_B     0x09 /* Broadcast set max write length */
#define RK3576_I3C_CCC_SETMRL_B     0x0a /* Broadcast set max read length */
#define RK3576_I3C_CCC_ENEC_D       0x80 /* Direct enable events */
#define RK3576_I3C_CCC_DISEC_D      0x81 /* Direct disable events */
#define RK3576_I3C_CCC_SETDASA      0x87 /* Set dynamic addr from static */
#define RK3576_I3C_CCC_SETNEWDA     0x88 /* Set new dynamic address */
#define RK3576_I3C_CCC_GETPID       0x8d /* Get provisional ID */
#define RK3576_I3C_CCC_GETBCR       0x8e /* Get bus characteristics */
#define RK3576_I3C_CCC_GETDCR       0x8f /* Get device characteristics */
#define RK3576_I3C_CCC_GETSTATUS    0x90 /* Get device status */

/* Event bits for ENEC/DISEC payloads. */

#define RK3576_I3C_EVENT_SIR        (1 << 0) /* Slave interrupt request */
#define RK3576_I3C_EVENT_MR         (1 << 1) /* Master request */
#define RK3576_I3C_EVENT_HJ         (1 << 3) /* Hot join */

/* Largest IBI payload the driver buffers per event. */

#define RK3576_I3C_IBI_PAYLOAD_MAX  8

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Descriptor of one I3C device discovered by dynamic address assignment. */

struct rk3576_i3c_devinfo_s
{
  uint8_t slot;    /* DAT index used by the controller */
  uint8_t dynaddr; /* Assigned 7-bit dynamic address */
  uint8_t bcr;     /* Bus characteristics register */
  uint8_t dcr;     /* Device characteristics register */
  uint64_t pid;    /* 48-bit provisional ID */
};

/* One CCC (common command code) request. */

struct rk3576_i3c_ccc_s
{
  uint8_t id;       /* CCC code, see RK3576_I3C_CCC_* */
  uint8_t slot;     /* DAT index; ignored for broadcast CCCs */
  bool read;        /* true: GET-type CCC, false: SET-type */
  bool has_defbyte; /* true if defbyte below must be sent */
  uint8_t defbyte;  /* Defining byte */
  uint8_t *buffer;  /* Payload buffer (may be NULL when length == 0) */
  uint16_t length;  /* Payload length in bytes */
};

/* In-band interrupt callback.  Invoked from the interrupt handler, so it
 * must not block.
 *
 * Input Parameters:
 *   arg     - Opaque argument registered with the callback
 *   dynaddr - Dynamic address of the requesting device
 *   payload - Received payload bytes (may be NULL when length is 0)
 *   length  - Number of valid payload bytes
 */

typedef void (*rk3576_i3c_ibi_cb_t)(void *arg, uint8_t dynaddr,
                                    const uint8_t *payload, uint8_t length);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_i3c_initialize
 *
 * Description:
 *   Initialize one RK3576 I3C controller in master mode and return an
 *   i2c_master_s handle for it.  I3C is backwards compatible with legacy
 *   I2C devices, so the returned handle can be used with the whole NuttX
 *   I2C device stack.  Native I3C features are reached through the
 *   rk3576_i3c_* helpers below, which take the same handle.
 *
 *   The controller clocks are ungated here; pin muxing is the board's
 *   responsibility.
 *
 * Input Parameters:
 *   port - Controller number (0 or 1).
 *
 * Returned Value:
 *   A pointer to the i2c_master_s on success; NULL on failure.
 *
 ****************************************************************************/

struct i2c_master_s *rk3576_i3c_initialize(int port);

/****************************************************************************
 * Name: rk3576_i3c_uninitialize
 *
 * Description:
 *   Disable the controller, detach its interrupt and release the driver
 *   state obtained from rk3576_i3c_initialize().
 *
 * Input Parameters:
 *   dev - Handle returned by rk3576_i3c_initialize().
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_uninitialize(struct i2c_master_s *dev);

/****************************************************************************
 * Name: rk3576_i3c_send_ccc
 *
 * Description:
 *   Issue one common command code (CCC) transfer on the I3C bus.  Both
 *   broadcast (id < 0x80) and direct (id >= 0x80) CCCs are supported; for
 *   direct CCCs ccc->slot selects the target device address table entry.
 *
 * Input Parameters:
 *   dev - Handle returned by rk3576_i3c_initialize().
 *   ccc - Description of the command to run.  For read CCCs the payload is
 *         returned in ccc->buffer and ccc->length is updated with the
 *         number of bytes actually received.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_send_ccc(struct i2c_master_s *dev,
                        struct rk3576_i3c_ccc_s *ccc);

/****************************************************************************
 * Name: rk3576_i3c_do_daa
 *
 * Description:
 *   Run ENTDAA to assign dynamic addresses to every I3C device on the bus.
 *   Previously assigned addresses are dropped first with RSTDAA.  The
 *   resulting device list can be read back with rk3576_i3c_get_devices().
 *
 * Input Parameters:
 *   dev - Handle returned by rk3576_i3c_initialize().
 *
 * Returned Value:
 *   Number of devices that obtained a dynamic address (>= 0), or a negated
 *   errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_do_daa(struct i2c_master_s *dev);

/****************************************************************************
 * Name: rk3576_i3c_get_devices
 *
 * Description:
 *   Retrieve the list of I3C devices known to the controller after a
 *   successful rk3576_i3c_do_daa().
 *
 * Input Parameters:
 *   dev   - Handle returned by rk3576_i3c_initialize().
 *   info  - Caller-provided array receiving the descriptors.
 *   ndevs - Number of entries available in info.
 *
 * Returned Value:
 *   Number of descriptors written (>= 0), or a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_get_devices(struct i2c_master_s *dev,
                           struct rk3576_i3c_devinfo_s *info, int ndevs);

/****************************************************************************
 * Name: rk3576_i3c_ibi_enable
 *
 * Description:
 *   Accept in-band interrupts from one device and register the callback
 *   invoked when an IBI arrives.  The matching ENEC CCC is sent to the
 *   device so that it may start signalling.
 *
 * Input Parameters:
 *   dev      - Handle returned by rk3576_i3c_initialize().
 *   slot     - Device address table index of the target device.
 *   withdata - true if the device appends a mandatory data byte.
 *   cb       - Callback invoked from interrupt context.
 *   arg      - Opaque argument handed back to the callback.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_ibi_enable(struct i2c_master_s *dev, uint8_t slot,
                          bool withdata, rk3576_i3c_ibi_cb_t cb, void *arg);

/****************************************************************************
 * Name: rk3576_i3c_ibi_disable
 *
 * Description:
 *   Stop accepting in-band interrupts from one device and unregister its
 *   callback.
 *
 * Input Parameters:
 *   dev  - Handle returned by rk3576_i3c_initialize().
 *   slot - Device address table index of the target device.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_ibi_disable(struct i2c_master_s *dev, uint8_t slot);

/****************************************************************************
 * Name: rk3576_i3c_priv_transfer
 *
 * Description:
 *   Run an SDR private transfer against an I3C device that already owns a
 *   dynamic address.  This is the I3C equivalent of an I2C transfer: the
 *   controller drives the bus in push-pull mode at SDR speed.
 *
 * Input Parameters:
 *   dev    - Handle returned by rk3576_i3c_initialize().
 *   slot   - Device address table index of the target device.
 *   read   - true for a read, false for a write.
 *   buffer - Data buffer.
 *   length - Number of bytes to transfer.
 *
 * Returned Value:
 *   Number of bytes transferred (>= 0), or a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_i3c_priv_transfer(struct i2c_master_s *dev, uint8_t slot,
                             bool read, uint8_t *buffer, uint16_t length);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_I3C */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_I3C_H */
