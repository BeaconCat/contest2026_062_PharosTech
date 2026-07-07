# SPDX-License-Identifier: Apache-2.0
set(SRCS rk3576_boot.c rk3576_serial.c)

if(CONFIG_RK3576_GPIO)
  list(APPEND SRCS rk3576_gpio.c)
endif()

if(CONFIG_ARCH_EARLY_PRINT)
  list(APPEND SRCS rk3576_lowputc.S)
endif()

if(CONFIG_RK3576_SDMMC OR CONFIG_RK3576_SDIO)
  list(APPEND SRCS rk3576_sdmmc.c)
endif()

if(CONFIG_RK3576_SDIO)
  list(APPEND SRCS rk3576_cru.c)
endif()

if(CONFIG_RK3576_MAILBOX)
  list(APPEND SRCS rk3576_mailbox.c)
endif()

target_sources(arch PRIVATE ${SRCS})
