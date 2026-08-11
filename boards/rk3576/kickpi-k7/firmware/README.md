# SeekWave SV6621 (SWT6621-S) firmware

These files are the co-processor images and board data used by the on-board
SeekWave combo:

- `SWT6621S_IRAM_SDIO.bin`: instruction RAM image.
- `SWT6621S_DRAM_SDIO.bin`: data RAM image.
- `SWT6621S_NV_SDIO_ALONE.bin`: SDIO operating-mode configuration.
- `SWT6621S_SEEKWAVE_R00001.bin`: board RF calibration data.

The files were obtained from the KICKPI-K7 vendor system image supplied for
this board. Copyright and firmware rights remain with SeekWave and the board
vendor. They are stored verbatim and are only streamed to the SV6621 during
`rk3576_skw_initialize()`; they are not linked into or derived from the
Apache-licensed driver source.
