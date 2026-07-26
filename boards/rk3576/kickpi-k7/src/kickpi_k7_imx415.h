/****************************************************************************
 * boards/rk3576/kickpi-k7/src/kickpi_k7_imx415.h
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
 * Public interface of the Sony IMX415 CMOS image sensor driver used on the
 * KICKPI-K7 board (three modules: imx415_0 on I2C4, imx415_1 on I2C5 and
 * imx415_3 on I2C8, all at slave address 0x37).
 *
 * NuttX has no generic camera-sensor framework: include/nuttx/video/imgsensor.h
 * describes struct imgsensor_ops_s, but that interface is owned by the
 * capture/ISP driver (drivers/video/video.c binds one imgsensor to one
 * imgdata).  Until the RK3576 VICAP/ISP capture driver exists there is no
 * upper half to register against, so this sensor exposes plain in-kernel
 * functions.  They are shaped so that a thin imgsensor_ops_s shim can be
 * layered on later (set_mode/set_exposure/set_gain/stream on-off map 1:1 to
 * validate_frame_setting/set_frame_interval/set_imgsensor_value/start_capture).
 *
 * Bring-up order for one camera path:
 *
 *   1. rk3576_imx415_initialize()      - power, XVCLK, reset, probe ID
 *   2. rk3576_imx415_set_mode()        - load the mode register table
 *   3. rk3576_dphy_initialize(cfg->dphy_id, mode lane rate, cfg->num_lanes)
 *   4. rk3576_csi_initialize(cfg->csi_id, &csi_cfg)   (DT = RAW10)
 *   5. rk3576_imx415_stream_on() then rk3576_csi_start()
 *
 ****************************************************************************/

#ifndef __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_IMX415_H
#define __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_IMX415_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include "rk3576_gpio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C slave address of every IMX415 module on this board (DTS: reg = <0x37>,
 * all three modules sit on private buses so the address may repeat).
 */

#define IMX415_I2C_ADDR         0x37

/* External master clock feeding XVCLK, DTS node
 * "external-camera-37m-clock" (fixed-clock, 0x2367b88 Hz).
 */

#define IMX415_XVCLK_FREQ       37125000u
#define IMX415_XVCLK_NAME       "ext_cam_37m_clk"

/* Sensor limits used to range-check exposure and gain requests. */

#define IMX415_GAIN_MIN         0u    /* 0 dB                              */
#define IMX415_GAIN_MAX         2047u /* 11-bit GAIN_PCG_0, 0.3 dB / step  */
#define IMX415_EXPOSURE_MIN     8u    /* lines, SHR0 lower bound           */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Supported sensor output modes.  Both are 4-lane MIPI CSI-2 RAW10. */

enum imx415_mode_e
{
  IMX415_MODE_1920X1080P30 = 0, /* 2x2 binning + crop, 891 Mbps/lane */
  IMX415_MODE_3840X2160P30 = 1, /* all-pixel crop,     891 Mbps/lane */
  IMX415_MODE_COUNT
};

/* Static description of one mode, published for the CSI/D-PHY setup. */

struct imx415_modeinfo_s
{
  uint16_t width;          /* Active pixels per line                     */
  uint16_t height;         /* Active lines per frame                     */
  uint16_t fps;            /* Nominal frame rate                         */
  uint16_t hmax;           /* HMAX in INCK units (0x3028/0x3029)         */
  uint32_t vmax;           /* VMAX in lines  (0x3024..0x3026)            */
  uint32_t lane_rate_mbps; /* Per-lane HS bit rate, feeds the D-PHY      */
  uint8_t  csi_dt;         /* CSI-2 data type of the payload             */
};

/* Board wiring of one IMX415 instance.  The KICKPI-K7 vendor DTS carries no
 * reset-gpios / pwdn-gpios for these modules (power is sequenced by the
 * avdd-supply regulator and the VI power domain), so both GPIOs are
 * optional: leave has_reset / has_pwdn false when not wired.
 */

struct imx415_config_s
{
  int  csi_id;             /* RK3576_CSI0..4 receiving this sensor       */
  int  dphy_id;            /* RK3576_DPHY0/1 in front of that CSI host   */
  int  num_lanes;          /* Active MIPI data lanes, 1..4               */
  bool has_reset;          /* true when reset_pin is wired               */
  bool has_pwdn;           /* true when pwdn_pin is wired                */
  gpio_pinset_t reset_pin; /* Active-low XCLR, valid if has_reset        */
  gpio_pinset_t pwdn_pin;  /* Active-low power-down, valid if has_pwdn   */
};

/* Opaque per-sensor state. */

struct imx415_dev_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_imx415_initialize
 *
 * Description:
 *   Bring up one IMX415: enable the 37.125 MHz external master clock, take
 *   the sensor out of power-down and reset following the Sony power-up
 *   sequence, open the I2C master and verify communication by reading back
 *   a known register.  The sensor is left in standby; call
 *   rk3576_imx415_set_mode() and rk3576_imx415_stream_on() to start it.
 *
 * Input Parameters:
 *   i2c_port - RK3576 I2C controller index (4, 5 or 8 on this board)
 *   i2c_addr - 7-bit slave address, IMX415_I2C_ADDR
 *   cfg      - Board wiring description, must stay valid for the lifetime
 *              of the device
 *
 * Returned Value:
 *   Device handle on success, NULL on failure.
 *
 ****************************************************************************/

struct imx415_dev_s *rk3576_imx415_initialize(
    int i2c_port, uint8_t i2c_addr, const struct imx415_config_s *cfg);

/****************************************************************************
 * Name: rk3576_imx415_set_mode
 *
 * Description:
 *   Load the register table of one output mode.  The sensor must not be
 *   streaming; the mode is applied in standby.
 *
 * Input Parameters:
 *   dev  - Handle from rk3576_imx415_initialize()
 *   mode - Requested mode
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_imx415_set_mode(struct imx415_dev_s *dev, enum imx415_mode_e mode);

/****************************************************************************
 * Name: rk3576_imx415_get_modeinfo
 *
 * Description:
 *   Return the static description of a mode so that the caller can program
 *   the D-PHY lane rate and the CSI-2 data type to match.
 *
 * Input Parameters:
 *   mode - Mode to describe
 *
 * Returned Value:
 *   Pointer to a constant descriptor, NULL if mode is out of range.
 *
 ****************************************************************************/

const struct imx415_modeinfo_s *rk3576_imx415_get_modeinfo(
    enum imx415_mode_e mode);

/****************************************************************************
 * Name: rk3576_imx415_set_exposure
 *
 * Description:
 *   Program the coarse integration time of the current mode.  The value is
 *   given in lines and converted to the SHR0 shutter register
 *   (SHR0 = VMAX - lines).
 *
 * Input Parameters:
 *   dev   - Device handle
 *   lines - Integration time in lines, IMX415_EXPOSURE_MIN .. VMAX - 4
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_imx415_set_exposure(struct imx415_dev_s *dev, uint32_t lines);

/****************************************************************************
 * Name: rk3576_imx415_set_gain
 *
 * Description:
 *   Program the analog/digital conversion gain (GAIN_PCG_0), 0.3 dB per
 *   step.
 *
 * Input Parameters:
 *   dev  - Device handle
 *   gain - IMX415_GAIN_MIN .. IMX415_GAIN_MAX
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_imx415_set_gain(struct imx415_dev_s *dev, uint32_t gain);

/****************************************************************************
 * Name: rk3576_imx415_stream_on
 *
 * Description:
 *   Leave standby (0x3000 = 0) and release the master mode start
 *   (0x3002 = 0) so that the sensor drives the MIPI link.
 *
 * Input Parameters:
 *   dev - Device handle
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_imx415_stream_on(struct imx415_dev_s *dev);

/****************************************************************************
 * Name: rk3576_imx415_stream_off
 *
 * Description:
 *   Stop the master mode (0x3002 = 1) and return the sensor to standby
 *   (0x3000 = 1).
 *
 * Input Parameters:
 *   dev - Device handle
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_imx415_stream_off(struct imx415_dev_s *dev);

#ifdef __cplusplus
}
#endif

#endif /* __BOARDS_RK3576_KICKPI_K7_SRC_KICKPI_K7_IMX415_H */
