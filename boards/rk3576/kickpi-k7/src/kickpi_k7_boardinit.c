/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_boardinit.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <syslog.h>
#include <nuttx/board.h>
#include "rk3576_gpio.h"
#include "kickpi_k7.h"

#ifdef CONFIG_RK3576_SDMMC
#  include <nuttx/mmcsd.h>
#  include "rk3576_sdmmc.h"
#endif

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)
#  include <sys/mount.h>
#  include <nuttx/fs/partition.h>
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#if defined(CONFIG_RK3576_SDMMC) && defined(CONFIG_GPT_PARTITION)

/****************************************************************************
 * Name: kickpi_k7_partition_handler
 *
 * Description:
 *   Called by parse_block_partition() for each valid GPT partition on
 *   /dev/mmcsd0.  The GPT layout (see build_sd.sh) is index 0 = uboot (BL33),
 *   index 1 = trust, index 2 = rootfs.  Register a block-device node
 *   /dev/mmcsd0p{index+1} for each (mapping only, no writes, so the boot
 *   images are never touched).
 ****************************************************************************/

static void kickpi_k7_partition_handler(FAR struct partition_s *part,
                                        FAR void *arg)
{
  char devname[] = "/dev/mmcsd0p0";

  if (part->index < 9)
    {
      /* ASCII digit: 0x31 = '1'.  Do not use (char)(1 + index) -- that would
       * be a control character.
       */

      devname[sizeof(devname) - 2] = (char)(0x31 + part->index);
      register_blockpartition(devname, 0660, "/dev/mmcsd0",
                              part->firstblock, part->nblocks);
      syslog(LOG_INFO, "INFO: partition %s firstblock=%lu nblocks=%lu\n",
             devname, (unsigned long)part->firstblock,
             (unsigned long)part->nblocks);
    }
}

/****************************************************************************
 * Name: kickpi_k7_mount_data
 *
 * Description:
 *   Parse the GPT of /dev/mmcsd0, register each partition as /dev/mmcsd0pN,
 *   then mount the rootfs partition (p3) as FAT on /data.
 *   Mount only -- NEVER mkfatfs here: formatting on the boot path would
 *   corrupt the boot images and brick the card if a partition maps wrong
 *   (already hit once).  Format once manually with `mkfatfs /dev/mmcsd0p3`
 *   or pre-format on the PC.  Any failure only warns and never blocks boot.
 ****************************************************************************/

static void kickpi_k7_mount_data(void)
{
  const char *datadev = "/dev/mmcsd0p3";   /* GPT partition 3 rootfs @ sector 32768 */
  const char *datadir = "/data";
  int ret;

  ret = parse_block_partition("/dev/mmcsd0", kickpi_k7_partition_handler,
                              NULL);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "WARNING: parse /dev/mmcsd0 GPT failed: %d\n", ret);
      return;
    }

  /* Mount an already-formatted FAT only; if unformatted (mount fails) just
   * warn -- do not format here.
   */

  ret = mount(datadev, datadir, "vfat", 0, NULL);
  if (ret >= 0)
    {
      syslog(LOG_INFO, "INFO: mounted %s (vfat) -> %s\n", datadev, datadir);
    }
  else
    {
      syslog(LOG_WARNING,
             "WARNING: %s not mounted (maybe unformatted, %d); "
             "format once with `mkfatfs %s`\n", datadev, ret, datadev);
    }
}
#endif /* CONFIG_RK3576_SDMMC && CONFIG_GPT_PARTITION */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_memory_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called early in the initialization before memory has
 *   been configured.  This board-specific function is responsible for
 *   configuring any on-board memories.
 *
 *   Logic in rk3576_memory_initialize must be careful to avoid using any
 *   global variables because those will be uninitialized at the time this
 *   function is called.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_memory_initialize(void)
{
  /* SDRAM was already init by bootloader in supported configuration */
}

/****************************************************************************
 * Name: rk3576_board_initialize
 *
 * Description:
 *   All RK3576 architectures must provide the following entry point.  This
 *   entry point is called in the initialization phase -- after
 *   rk3576_memory_initialize and after all memory has been configured and
 *   mapped but before any devices have been initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void rk3576_board_initialize(void)
{
#ifdef CONFIG_ARCH_LEDS
  /* Configure on-board LEDs if LED support has been selected. */

  /* board_autoled_initialize(); */
#endif
}

/****************************************************************************
 * Name: board_late_initialize
 *
 * Description:
 *   If CONFIG_BOARD_LATE_INITIALIZE is selected, then an additional
 *   initialization call will be performed in the boot-up sequence to a
 *   function called board_late_initialize(). board_late_initialize() will be
 *   called immediately after up_initialize() is called and just before the
 *   initial application is started.  This additional initialization phase
 *   may be used, for example, to initialize board-specific device drivers.
 *
 ****************************************************************************/

#ifdef CONFIG_BOARD_LATE_INITIALIZE
void board_late_initialize(void)
{
  /* Perform board initialization */

#ifdef CONFIG_DEV_GPIO
  /* setup gpio driver */
  rk3576_gpio_init();

  /* register LED GPIO pin */
  rk3576_gpio_register(GPIO_PORT0 | GPIO_PIN_B4 | GPIO_OUTPUT);
#endif

#ifdef CONFIG_RK3576_SDMMC
  /* Initialize the SD card slot (SDMMC0) -> /dev/mmcsd0.  The SD card is an
   * optional peripheral: on failure only warn, do not block the boot (booting
   * to NSH must succeed even with no card inserted).
   */

  {
    FAR struct sdio_dev_s *sdmmc = rk3576_sdmmc_initialize(0);
    if (sdmmc == NULL)
      {
        syslog(LOG_ERR, "ERROR: rk3576_sdmmc_initialize failed\n");
      }
    else if (mmcsd_slotinitialize(0, sdmmc) < 0)
      {
        syslog(LOG_ERR, "ERROR: mmcsd_slotinitialize failed\n");
      }
  }

#ifdef CONFIG_GPT_PARTITION
  /* Parse the GPT and mount the rootfs FAT partition on /data (mount only). */

  kickpi_k7_mount_data();
#endif
#endif
}
#endif /* CONFIG_BOARD_LATE_INITIALIZE */
