# SeekWave SV6621 (SWT6621-S) CP firmware

WiFi/BT combo co-processor firmware images loaded onto the chip at bring-up.

- `SWT6621S_IRAM_SDIO.bin` - instruction RAM image
- `SWT6621S_DRAM_SDIO.bin` - data RAM image

Copyright belongs to SeekWave (重庆希微); redistributed here for loading
only. openvela does not modify or link against them, it streams them to the
combo over SDIO during `rk3576_skw_initialize()`.
