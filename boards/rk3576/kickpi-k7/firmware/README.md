# SeekWave SV6621 (SWT6621-S) firmware inputs

This repository does not distribute SeekWave firmware. The Apache-2.0
license of the driver does not grant a license to the firmware.

## Building with the onboard radio

Obtain firmware authorized by SeekWave or an authorized supplier for your
hardware and intended use. Place these four files in this directory before
building an image with CONFIG_KICKPI_K7_WIFI enabled:

| File | Purpose |
| --- | --- |
| SWT6621S_IRAM_SDIO.bin | Instruction RAM image |
| SWT6621S_DRAM_SDIO.bin | Data RAM image |
| SWT6621S_NV_SDIO_ALONE.bin | SDIO operating-mode configuration |
| SWT6621S_SEEKWAVE_R00001.bin | Board RF calibration data |

NV and RF calibration data must match the target hardware. Do not use empty
placeholders or substitute unrelated board data. The board embeds these
inputs using .incbin; missing inputs must fail the full build.

Permission to obtain a file does not necessarily permit redistribution.
Ensure your authorization covers storage, CI use, and any distribution of
images containing the firmware. Do not commit these files, generated objects,
or firmware-containing images to the public repository.

The existing full configuration and both build systems remain available.
From the OpenVela workspace root:

```sh
./build.sh contest2026_062_PharosTech/configs/dev -j4
./build.sh contest2026_062_PharosTech/configs/dev --cmake
```

## Testing the board without SeekWave firmware

Use the separate dev-no-sv6621 configuration if authorized firmware is not
available. It excludes the SV6621 driver and onboard radio initialization,
while retaining UART NSH, USB ADB, storage, audio, and the other development
features whose dependencies are enabled.

```sh
./build.sh contest2026_062_PharosTech/configs/dev-no-sv6621 -j4
./build.sh contest2026_062_PharosTech/configs/dev-no-sv6621 --cmake
```

Use separate build outputs or clean the previous configuration before
switching. The configuration includes the Make.defs link required by the
legacy Make build. UART0 uses 1500000 baud; the development host currently
uses COM14. ADB provides shell and file transfer without a WiFi connection.

This alternative does not enable onboard SV6621 WiFi or Bluetooth. USB
network adapters and ADB network tunneling are separate development work,
not capabilities promised by this configuration. Rockchip boot components
retain their own licensing requirements.
