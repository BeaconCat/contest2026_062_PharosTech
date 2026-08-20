/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ap_beacon.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>

#include "sv6621_ap_beacon.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SV6621_AP_FRAME_HEADER_LENGTH        24
#define SV6621_AP_FIXED_PARAMETERS_LENGTH    12
#define SV6621_AP_BEACON_FRAME_CONTROL     0x80
#define SV6621_AP_PROBE_FRAME_CONTROL      0x50

#define SV6621_AP_CAPABILITY_ESS         0x0001
#define SV6621_AP_CAPABILITY_PRIVACY     0x0010
#define SV6621_AP_CAPABILITY_SHORT_PREAMBLE 0x0020
#define SV6621_AP_CAPABILITY_SHORT_SLOT  0x0400

#define SV6621_AP_IE_SSID                     0
#define SV6621_AP_IE_SUPPORTED_RATES          1
#define SV6621_AP_IE_DS_PARAMETERS            3
#define SV6621_AP_IE_EXTENDED_RATES          50
#define SV6621_AP_IE_RSN                     48

#define SV6621_AP_HIDDEN_ZERO_LENGTH          1
#define SV6621_AP_HIDDEN_ZERO_CONTENTS        2
#define SV6621_AP_RSN_VERSION                 1
#define SV6621_AP_RSN_OUI_0                   0x00
#define SV6621_AP_RSN_OUI_1                   0x0f
#define SV6621_AP_RSN_OUI_2                   0xac
#define SV6621_AP_RSN_SUITE_CCMP              4
#define SV6621_AP_RSN_SUITE_BIP_CMAC_128      6
#define SV6621_AP_RSN_AKM_PSK                 2
#define SV6621_AP_RSN_AKM_SAE                 8
#define SV6621_AP_RSN_CAP_MFPR                (1 << 6)
#define SV6621_AP_RSN_CAP_MFPC                (1 << 7)

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_sv6621_ap_rates_2ghz[] =
{
  0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24
};

static const uint8_t g_sv6621_ap_extended_rates_2ghz[] =
{
  0x30, 0x48, 0x60, 0x6c
};

static const uint8_t g_sv6621_ap_rates_5ghz[] =
{
  0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void sv6621_ap_beacon_put_le16(FAR uint8_t *value,
                                      uint16_t number);
static int sv6621_ap_beacon_append(FAR uint8_t *buffer, size_t capacity,
                                  FAR size_t *length,
                                  FAR const void *data, size_t data_length);
static int sv6621_ap_beacon_append_ie(FAR uint8_t *buffer, size_t capacity,
                                     FAR size_t *length, uint8_t identifier,
                                     FAR const void *data,
                                     size_t data_length);
static int sv6621_ap_beacon_write_fixed(
    FAR uint8_t *buffer, size_t capacity, FAR size_t *length,
    uint8_t frame_control, FAR const uint8_t address[SV6621_MAC_LENGTH],
    uint16_t beacon_interval, uint16_t capabilities);
static int sv6621_ap_beacon_append_identity(
    FAR uint8_t *buffer, size_t capacity, FAR size_t *length,
    FAR const struct sv6621_ap_beacon_config_s *config, bool reveal_ssid);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sv6621_ap_beacon_put_le16(FAR uint8_t *value,
                                      uint16_t number)
{
  value[0] = number & 0xff;
  value[1] = number >> 8;
}

static int sv6621_ap_beacon_append(FAR uint8_t *buffer, size_t capacity,
                                  FAR size_t *length,
                                  FAR const void *data, size_t data_length)
{
  if (*length > capacity || data_length > capacity - *length)
    {
      return -ENOSPC;
    }

  if (data_length != 0)
    {
      memcpy(buffer + *length, data, data_length);
      *length += data_length;
    }

  return 0;
}

static int sv6621_ap_beacon_append_ie(FAR uint8_t *buffer, size_t capacity,
                                     FAR size_t *length, uint8_t identifier,
                                     FAR const void *data,
                                     size_t data_length)
{
  uint8_t header[2];
  int ret;

  if (data_length > UINT8_MAX || (data_length != 0 && data == NULL))
    {
      return -EINVAL;
    }

  header[0] = identifier;
  header[1] = data_length;
  ret = sv6621_ap_beacon_append(buffer, capacity, length, header,
                                sizeof(header));
  if (ret < 0)
    {
      return ret;
    }

  return sv6621_ap_beacon_append(buffer, capacity, length, data,
                                 data_length);
}

static int sv6621_ap_beacon_write_fixed(
    FAR uint8_t *buffer, size_t capacity, FAR size_t *length,
    uint8_t frame_control, FAR const uint8_t address[SV6621_MAC_LENGTH],
    uint16_t beacon_interval, uint16_t capabilities)
{
  static const uint8_t broadcast[SV6621_MAC_LENGTH] =
  {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff
  };

  if (capacity < SV6621_AP_FRAME_HEADER_LENGTH +
                 SV6621_AP_FIXED_PARAMETERS_LENGTH)
    {
      return -ENOSPC;
    }

  memset(buffer, 0, SV6621_AP_FRAME_HEADER_LENGTH +
                    SV6621_AP_FIXED_PARAMETERS_LENGTH);
  buffer[0] = frame_control;
  memcpy(buffer + 4, broadcast, sizeof(broadcast));
  memcpy(buffer + 10, address, SV6621_MAC_LENGTH);
  memcpy(buffer + 16, address, SV6621_MAC_LENGTH);
  sv6621_ap_beacon_put_le16(buffer + SV6621_AP_FRAME_HEADER_LENGTH + 8,
                           beacon_interval);
  sv6621_ap_beacon_put_le16(buffer + SV6621_AP_FRAME_HEADER_LENGTH + 10,
                           capabilities);
  *length = SV6621_AP_FRAME_HEADER_LENGTH +
            SV6621_AP_FIXED_PARAMETERS_LENGTH;
  return 0;
}

static int sv6621_ap_beacon_append_identity(
    FAR uint8_t *buffer, size_t capacity, FAR size_t *length,
    FAR const struct sv6621_ap_beacon_config_s *config, bool reveal_ssid)
{
  uint8_t hidden_ssid[SV6621_SSID_MAX_LENGTH];
  FAR const uint8_t *ssid = config->ssid;
  size_t ssid_length = config->ssid_length;
  int ret;

  if (!reveal_ssid && config->hidden_ssid == SV6621_AP_HIDDEN_ZERO_LENGTH)
    {
      ssid_length = 0;
    }
  else if (!reveal_ssid &&
           config->hidden_ssid == SV6621_AP_HIDDEN_ZERO_CONTENTS)
    {
      memset(hidden_ssid, 0, config->ssid_length);
      ssid = hidden_ssid;
    }

  ret = sv6621_ap_beacon_append_ie(buffer, capacity, length,
                                   SV6621_AP_IE_SSID, ssid, ssid_length);
  if (ret < 0)
    {
      return ret;
    }

  if (config->band == SV6621_BAND_2GHZ)
    {
      ret = sv6621_ap_beacon_append_ie(
          buffer, capacity, length, SV6621_AP_IE_SUPPORTED_RATES,
          g_sv6621_ap_rates_2ghz, sizeof(g_sv6621_ap_rates_2ghz));
      if (ret < 0)
        {
          return ret;
        }

      return sv6621_ap_beacon_append_ie(
          buffer, capacity, length, SV6621_AP_IE_DS_PARAMETERS,
          &config->channel, sizeof(config->channel));
    }

  return sv6621_ap_beacon_append_ie(
      buffer, capacity, length, SV6621_AP_IE_SUPPORTED_RATES,
      g_sv6621_ap_rates_5ghz, sizeof(g_sv6621_ap_rates_5ghz));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ap_build_beacon_templates(
    FAR const struct sv6621_ap_beacon_config_s *config,
    FAR struct sv6621_ap_beacon_templates_s *templates)
{
  uint16_t capabilities = SV6621_AP_CAPABILITY_ESS |
                          SV6621_AP_CAPABILITY_SHORT_SLOT;
  int ret;

  if (config == NULL || templates == NULL || config->ssid_length == 0 ||
      config->ssid_length > SV6621_SSID_MAX_LENGTH ||
      config->hidden_ssid > SV6621_AP_HIDDEN_ZERO_CONTENTS ||
      config->channel == 0 || config->band > SV6621_BAND_5GHZ ||
      config->beacon_interval == 0 ||
      (config->extra_ies_length != 0 && config->extra_ies == NULL) ||
      config->extra_ies_length > SV6621_AP_BEACON_TAIL_MAX_LENGTH)
    {
      return -EINVAL;
    }

  if (config->band == SV6621_BAND_2GHZ)
    {
      capabilities |= SV6621_AP_CAPABILITY_SHORT_PREAMBLE;
    }

  if (config->privacy)
    {
      capabilities |= SV6621_AP_CAPABILITY_PRIVACY;
    }

  memset(templates, 0, sizeof(*templates));
  ret = sv6621_ap_beacon_write_fixed(
      templates->beacon_head, sizeof(templates->beacon_head),
      &templates->beacon_head_length, SV6621_AP_BEACON_FRAME_CONTROL,
      config->address, config->beacon_interval, capabilities);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_ap_beacon_append_identity(
      templates->beacon_head, sizeof(templates->beacon_head),
      &templates->beacon_head_length, config, false);
  if (ret < 0)
    {
      return ret;
    }

  if (config->band == SV6621_BAND_2GHZ)
    {
      ret = sv6621_ap_beacon_append_ie(
          templates->beacon_tail, sizeof(templates->beacon_tail),
          &templates->beacon_tail_length, SV6621_AP_IE_EXTENDED_RATES,
          g_sv6621_ap_extended_rates_2ghz,
          sizeof(g_sv6621_ap_extended_rates_2ghz));
      if (ret < 0)
        {
          return ret;
        }
    }

  ret = sv6621_ap_beacon_append(
      templates->beacon_tail, sizeof(templates->beacon_tail),
      &templates->beacon_tail_length, config->extra_ies,
      config->extra_ies_length);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_ap_beacon_write_fixed(
      templates->probe_response, sizeof(templates->probe_response),
      &templates->probe_response_length, SV6621_AP_PROBE_FRAME_CONTROL,
      config->address, config->beacon_interval, capabilities);
  if (ret < 0)
    {
      return ret;
    }

  ret = sv6621_ap_beacon_append_identity(
      templates->probe_response, sizeof(templates->probe_response),
      &templates->probe_response_length, config, true);
  if (ret < 0)
    {
      return ret;
    }

  if (config->band == SV6621_BAND_2GHZ)
    {
      ret = sv6621_ap_beacon_append_ie(
          templates->probe_response, sizeof(templates->probe_response),
          &templates->probe_response_length,
          SV6621_AP_IE_EXTENDED_RATES,
          g_sv6621_ap_extended_rates_2ghz,
          sizeof(g_sv6621_ap_extended_rates_2ghz));
      if (ret < 0)
        {
          return ret;
        }
    }

  return sv6621_ap_beacon_append(
      templates->probe_response, sizeof(templates->probe_response),
      &templates->probe_response_length, config->extra_ies,
      config->extra_ies_length);
}

/****************************************************************************
 * Name: sv6621_ap_build_rsn_ie
 ****************************************************************************/

int sv6621_ap_build_rsn_ie(enum sv6621_security_e security,
                           bool pmf_required, FAR uint8_t *ie,
                           size_t capacity, FAR size_t *length)
{
  uint8_t akm_count;
  uint8_t body_length;
  uint16_t capabilities;
  size_t offset;

  if (ie == NULL || length == NULL ||
      (security != SV6621_SECURITY_WPA2_PSK &&
       security != SV6621_SECURITY_WPA3_SAE &&
       security != SV6621_SECURITY_WPA2_WPA3_PSK))
    {
      return -EINVAL;
    }

  akm_count = security == SV6621_SECURITY_WPA2_WPA3_PSK ? 2 : 1;
  capabilities = security == SV6621_SECURITY_WPA2_PSK ? 0 :
                 SV6621_AP_RSN_CAP_MFPC;
  if (pmf_required)
    {
      capabilities |= SV6621_AP_RSN_CAP_MFPR | SV6621_AP_RSN_CAP_MFPC;
    }

  body_length = security == SV6621_SECURITY_WPA2_PSK ? 20 :
                security == SV6621_SECURITY_WPA3_SAE ? 26 : 30;
  if (capacity < (size_t)body_length + 2)
    {
      return -ENOSPC;
    }

  ie[0] = SV6621_AP_IE_RSN;
  ie[1] = body_length;
  ie[2] = SV6621_AP_RSN_VERSION;
  ie[3] = 0;
  ie[4] = SV6621_AP_RSN_OUI_0;
  ie[5] = SV6621_AP_RSN_OUI_1;
  ie[6] = SV6621_AP_RSN_OUI_2;
  ie[7] = SV6621_AP_RSN_SUITE_CCMP;
  ie[8] = 1;
  ie[9] = 0;
  ie[10] = SV6621_AP_RSN_OUI_0;
  ie[11] = SV6621_AP_RSN_OUI_1;
  ie[12] = SV6621_AP_RSN_OUI_2;
  ie[13] = SV6621_AP_RSN_SUITE_CCMP;
  ie[14] = akm_count;
  ie[15] = 0;
  offset = 16;
  ie[offset++] = SV6621_AP_RSN_OUI_0;
  ie[offset++] = SV6621_AP_RSN_OUI_1;
  ie[offset++] = SV6621_AP_RSN_OUI_2;
  ie[offset++] = security == SV6621_SECURITY_WPA3_SAE ?
                 SV6621_AP_RSN_AKM_SAE : SV6621_AP_RSN_AKM_PSK;
  if (security == SV6621_SECURITY_WPA2_WPA3_PSK)
    {
      ie[offset++] = SV6621_AP_RSN_OUI_0;
      ie[offset++] = SV6621_AP_RSN_OUI_1;
      ie[offset++] = SV6621_AP_RSN_OUI_2;
      ie[offset++] = SV6621_AP_RSN_AKM_SAE;
    }

  ie[offset++] = capabilities & 0xff;
  ie[offset++] = capabilities >> 8;
  if (security != SV6621_SECURITY_WPA2_PSK)
    {
      ie[offset++] = 0;
      ie[offset++] = 0;
      ie[offset++] = SV6621_AP_RSN_OUI_0;
      ie[offset++] = SV6621_AP_RSN_OUI_1;
      ie[offset++] = SV6621_AP_RSN_OUI_2;
      ie[offset++] = SV6621_AP_RSN_SUITE_BIP_CMAC_128;
    }

  *length = offset;
  return 0;
}
