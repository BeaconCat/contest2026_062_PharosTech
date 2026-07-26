/****************************************************************************
 * chips/rk3576/rk3576_spi.h
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
 * RK3576 SPI master driver public API.
 ****************************************************************************/

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/spi/spi.h>

#ifdef CONFIG_RK3576_SPI

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rk3576_spi_initialize
 *
 * Description:
 *   Initialize one RK3576 SPI controller as a NuttX SPI master and return
 *   its struct spi_dev_s.  Repeated calls for the same port return the same
 *   (already initialized) instance.
 *
 *   The caller (board logic) is responsible for muxing the CLK/MOSI/MISO/CS
 *   pins and for making sure the controller clocks are ungated before the
 *   first transfer.
 *
 * Input Parameters:
 *   port - Controller index, 0..RK3576_SPI_NPORTS-1 (4 = SPI4, the only
 *          instance routed to the KICKPI-K7 expansion header).
 *
 * Returned Value:
 *   A pointer to the SPI master on success; NULL on an invalid port or if
 *   the port is not enabled in the configuration.
 *
 ****************************************************************************/

struct spi_dev_s *rk3576_spi_initialize(int port);

/****************************************************************************
 * Name: rk3576_spi_bus_select
 *
 * Description:
 *   Board-specific chip select assertion hook.  The driver provides a weak
 *   default that drives the controller's native chip select (SER register)
 *   for chip-select index SPIDEVID_INDEX(devid).  A board that wires the
 *   slave to a GPIO chip select (as the KICKPI-K7 vendor device tree does
 *   with cs-gpios) overrides this function.
 *
 * Input Parameters:
 *   port     - Controller index the request applies to.
 *   devid    - SPI device id (see include/nuttx/spi/spi.h).
 *   selected - true to assert (drive low) the chip select.
 *
 ****************************************************************************/

void rk3576_spi_bus_select(int port, uint32_t devid, bool selected);

/****************************************************************************
 * Name: rk3576_spi_bus_status
 *
 * Description:
 *   Board-specific status hook backing SPI_STATUS().  The driver provides a
 *   weak default that reports no status bits set.
 *
 * Input Parameters:
 *   port  - Controller index the request applies to.
 *   devid - SPI device id.
 *
 * Returned Value:
 *   A bitset of SPI_STATUS_* flags.
 *
 ****************************************************************************/

uint8_t rk3576_spi_bus_status(int port, uint32_t devid);

#ifdef CONFIG_SPI_CMDDATA
/****************************************************************************
 * Name: rk3576_spi_bus_cmddata
 *
 * Description:
 *   Board-specific command/data hook backing SPI_CMDDATA(), used by
 *   display controllers that carry the D/C selector on a side-band GPIO.
 *   The driver provides a weak default returning -ENOSYS.
 *
 * Input Parameters:
 *   port  - Controller index the request applies to.
 *   devid - SPI device id.
 *   cmd   - true to select command, false to select data.
 *
 * Returned Value:
 *   OK on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_spi_bus_cmddata(int port, uint32_t devid, bool cmd);
#endif

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_SPI */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_SPI_H */
