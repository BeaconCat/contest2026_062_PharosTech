# Development without the onboard SV6621

This configuration retains the development board peripherals, UART NSH and
USB ADB, but does not build the SV6621 driver, its SDIO transport, or its
firmware inputs. SDMMC storage remains enabled.

From the OpenVela workspace root, use either build system:

```sh
./build.sh contest2026_062_PharosTech/configs/dev-no-sv6621 -j4
./build.sh contest2026_062_PharosTech/configs/dev-no-sv6621 --cmake
```

Use clean or separate outputs when switching configurations. Make.defs is a
Git symbolic link to the board's existing build definitions, not a second
copy of the board build rules.

UART0 uses 1500000 baud. USB ADB provides shell and file transfer without
a physical network interface. This configuration does not add USB network
adapters, ADB network tunneling, or onboard Bluetooth support.

For the original full configuration and authorized firmware requirements,
see [the firmware input guide](../../drivers/drivers/sv6621/firmware/README.md).
