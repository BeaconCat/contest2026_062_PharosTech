# SPDX-License-Identifier: Apache-2.0
set(SRCS kickpi_k7_boardinit.c kickpi_k7_appinit.c kickpi_k7_reset.c)

if(CONFIG_IEEE80211_BROADCOM_FULLMAC_SDIO)
  list(APPEND SRCS kickpi_k7_wlan.c)
endif()

if(CONFIG_KICKPI_K7_SDIO_PROBE)
  list(APPEND SRCS kickpi_k7_sdioprobe.c)
endif()

target_sources(board PRIVATE ${SRCS})
