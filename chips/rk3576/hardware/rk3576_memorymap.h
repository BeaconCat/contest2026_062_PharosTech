/* arch/arm64/src/rk3576/hardware/rk3576_memorymap.h - RK3576 peripherals */
#ifndef __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H
#define __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H

#include <nuttx/config.h>

/* Fixed crystal oscillator, source of the ARM generic timer */

#define RK3576_OSC_FREQ        24000000

/* GPIO banks (TRM) */
#define RK3576_GPIO0_ADDR      0x27320000
#define RK3576_GPIO1_ADDR      0x2AE10000
#define RK3576_GPIO2_ADDR      0x2AE20000
#define RK3576_GPIO3_ADDR      0x2AE30000
#define RK3576_GPIO4_ADDR      0x2AE40000
#define RK3576_PIO_ADDR        RK3576_GPIO0_ADDR

/* DesignWare 16550 UARTs (TRM). UART0 = debug console (vendor DTS earlycon). */
#define RK3576_UART0_ADDR      0x2AD40000
#define RK3576_UART1_ADDR      0x27310000
#define RK3576_UART2_ADDR      0x2AD50000
#define RK3576_UART3_ADDR      0x2AD60000
#define RK3576_UART4_ADDR      0x2AD70000

/* Synopsys DesignWare MSHC (dw_mmc, same IP as rk3288/rk3399). Three MMC hosts.
 * NOTE: The board boots from SDMMC (SD card) -- the loader has already set up
 * its clock and pins, so a first-cut host driver can skip CRU/pinctrl.
 */
#define RK3576_SDMMC_ADDR      0x2A310000   /* SD card slot (dw-mshc, removable) */
#define RK3576_SDIO_ADDR       0x2A320000   /* SDIO WiFi AP6256 (dw-mshc)        */
/* eMMC (dwcmshc, different IP). ★ This holds the vendor Android fallback -- the
 * firmware-update safety rail MUST never target this. Defined here only so the
 * update path can explicitly refuse it.
 */
#define RK3576_EMMC_ADDR       0x2A330000

/* Clock & Reset Unit / IO mux (for future CRU/pinctrl drivers). */
#define RK3576_CRU_ADDR        0x27200000
#define RK3576_IOC_ADDR        0x26040000

#endif /* __ARCH_ARM64_SRC_RK3576_HARDWARE_RK3576_MEMORYMAP_H */
