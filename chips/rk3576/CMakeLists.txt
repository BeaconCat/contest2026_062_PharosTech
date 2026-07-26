# SPDX-License-Identifier: Apache-2.0
set(SRCS rk3576_boot.c rk3576_serial.c rk3576_clk_tree.c)

if(CONFIG_RK3576_GPIO)
  list(APPEND SRCS rk3576_gpio.c)
endif()

if(CONFIG_ARCH_EARLY_PRINT)
  list(APPEND SRCS rk3576_lowputc.S)
endif()

if(CONFIG_RK3576_SDMMC)
  list(APPEND SRCS rk3576_sdmmc.c)
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

# Peripheral clock registration with the CLK framework.  The per-peripheral
# registration functions inside the file are guarded by the individual
# CONFIG_RK3576_* symbols, so the file itself is built unconditionally.

list(APPEND SRCS rk3576_clk_tree_periph.c)

if(CONFIG_RK3576_PD)
  list(APPEND SRCS rk3576_pd.c)
endif()

if(CONFIG_RK3576_PINCTRL)
  list(APPEND SRCS rk3576_pinctrl.c)
endif()

if(CONFIG_RK3576_MAILBOX)
  list(APPEND SRCS rk3576_mailbox.c)
endif()

if(CONFIG_RK3576_OTP)
  list(APPEND SRCS rk3576_otp.c)
endif()

if(CONFIG_RK3576_RNG)
  list(APPEND SRCS rk3576_rng.c)
endif()

if(CONFIG_RK3576_WDT)
  list(APPEND SRCS rk3576_wdt.c)
endif()

if(CONFIG_RK3576_SPI)
  list(APPEND SRCS rk3576_spi.c)
endif()

if(CONFIG_RK3576_I3C)
  list(APPEND SRCS rk3576_i3c.c)
endif()

if(CONFIG_RK3576_SARADC)
  list(APPEND SRCS rk3576_saradc.c)
endif()

if(CONFIG_RK3576_TSADC)
  list(APPEND SRCS rk3576_tsadc.c)
endif()

if(CONFIG_RK3576_IR)
  list(APPEND SRCS rk3576_ir.c)
endif()

if(CONFIG_RK3576_CRYPTO)
  list(APPEND SRCS rk3576_crypto.c)
endif()

if(CONFIG_RK3576_GMAC)
  list(APPEND SRCS rk3576_gmac.c)
endif()

if(CONFIG_RK3576_PDM)
  list(APPEND SRCS rk3576_pdm.c)
endif()

if(CONFIG_RK3576_COMBPHY)
  list(APPEND SRCS rk3576_combphy.c)
endif()

if(CONFIG_RK3576_PCIE)
  list(APPEND SRCS rk3576_pcie.c)
endif()

if(CONFIG_RK3576_SKW_BT)
  list(APPEND SRCS rk3576_skw_bt.c)
endif()

if(CONFIG_RK3576_VOP)
  list(APPEND SRCS rk3576_vop.c)
endif()

if(CONFIG_RK3576_HDMI)
  list(APPEND SRCS rk3576_hdmi.c)
endif()

if(CONFIG_RK3576_RGA)
  list(APPEND SRCS rk3576_rga.c)
endif()

if(CONFIG_RK3576_VDEC)
  list(APPEND SRCS rk3576_vdec.c)
endif()

if(CONFIG_RK3576_CSI)
  list(APPEND SRCS rk3576_csi.c rk3576_dphy.c)
endif()

if(CONFIG_RK3576_VICAP)
  list(APPEND SRCS rk3576_vicap.c)
endif()

if(CONFIG_RK3576_ISP)
  list(APPEND SRCS rk3576_isp.c)
endif()

if(CONFIG_RK3576_MALI)
  list(APPEND SRCS rk3576_mali.c)
endif()

if(CONFIG_RK3576_RKNPU)
  list(APPEND SRCS rk3576_rknpu.c)
endif()

if(CONFIG_RK3576_RKNN_MATMUL)
  list(APPEND SRCS rk3576_rknn_matmul.c)
endif()

if(CONFIG_SMP)
  list(APPEND SRCS rk3576_cpuboot.c)
endif()

target_sources(arch PRIVATE ${SRCS})
