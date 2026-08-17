# SPDX-License-Identifier: Apache-2.0
set(SRCS rk3576_boot.c rk3576_serial.c rk3576_clk_tree.c)

if(CONFIG_RK3576_GPIO)
  list(APPEND SRCS rk3576_gpio.c)
endif()

if(CONFIG_ARCH_EARLY_PRINT)
  list(APPEND SRCS rk3576_lowputc.S)
endif()

if(CONFIG_RK3576_SDMMC OR CONFIG_RK3576_SDIO)
  list(APPEND SRCS rk3576_sdmmc.c)
endif()

if(CONFIG_RK3576_SV6621_TRANSPORT)
  list(APPEND SRCS rk3576_sv6621_transport.c)
endif()

if(CONFIG_SV6621)
  set(SV6621_DIR
      ${CMAKE_CURRENT_LIST_DIR}/../../drivers/wireless/seekwave/sv6621)
  target_include_directories(arch PRIVATE ${SV6621_DIR}/include)
  list(APPEND SRCS
       ${SV6621_DIR}/sv6621_command.c
       ${SV6621_DIR}/sv6621_connection.c
       ${SV6621_DIR}/sv6621_core.c
       ${SV6621_DIR}/sv6621_data.c
       ${SV6621_DIR}/sv6621_firmware.c
       ${SV6621_DIR}/sv6621_filter.c
       ${SV6621_DIR}/sv6621_ioctl.c
       ${SV6621_DIR}/sv6621_memory.c
       ${SV6621_DIR}/sv6621_network.c
       ${SV6621_DIR}/sv6621_offload.c
       ${SV6621_DIR}/sv6621_packet.c
       ${SV6621_DIR}/sv6621_power.c
       ${SV6621_DIR}/sv6621_protocol.c
       ${SV6621_DIR}/sv6621_regulatory.c
       ${SV6621_DIR}/sv6621_rx.c
       ${SV6621_DIR}/sv6621_scan.c
       ${SV6621_DIR}/sv6621_security.c
       ${SV6621_DIR}/sv6621_signal.c
       ${SV6621_DIR}/sv6621_service.c
       ${SV6621_DIR}/sv6621_station.c
       ${SV6621_DIR}/sv6621_stats.c
       ${SV6621_DIR}/sv6621_transport.c
       ${SV6621_DIR}/sv6621_tx.c
       ${SV6621_DIR}/sv6621_wifi.c)
  list(APPEND SRCS ${SV6621_DIR}/sv6621_wpa_crypto.c)
  list(APPEND SRCS ${SV6621_DIR}/sv6621_wpa_eapol.c)
  list(APPEND SRCS ${SV6621_DIR}/sv6621_wpa_handshake.c)
endif()

if(CONFIG_RK3576_I2C)
  list(APPEND SRCS rk3576_i2c.c)
endif()

if(CONFIG_RK3576_PWM)
  list(APPEND SRCS rk3576_pwm.c)
endif()

if(CONFIG_RK3576_USB)
  list(APPEND SRCS rk3576_usb.c)
endif()

if(CONFIG_RK3576_EMMC)
  list(APPEND SRCS rk3576_emmc.c)
endif()

if(CONFIG_RK3576_DMA)
  list(APPEND SRCS rk3576_addrenv.c rk3576_dma.c)
endif()

if(CONFIG_RK3576_DMA_ALLOC)
  list(APPEND SRCS rk3576_dma_alloc.c)
endif()

if(CONFIG_RK3576_SAI)
  list(APPEND SRCS rk3576_sai.c)
endif()

if(CONFIG_RK3576_FSPI)
  list(APPEND SRCS rk3576_fspi.c)
endif()

if(CONFIG_RTC_ARCH)
  list(APPEND SRCS rk3576_rtc.c)
endif()

target_sources(arch PRIVATE ${SRCS})
