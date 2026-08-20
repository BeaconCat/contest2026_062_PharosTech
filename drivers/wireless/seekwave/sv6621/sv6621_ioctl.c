/****************************************************************************
 * drivers/wireless/seekwave/sv6621/sv6621_ioctl.c
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

#include <nuttx/kmalloc.h>
#include <nuttx/wireless/wireless.h>

#include <errno.h>
#include <net/if_arp.h>
#include <string.h>

#include "sv6621_core.h"
#include "sv6621_ioctl.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int sv6621_ioctl_auth(FAR struct sv6621_ioctl_s *ioctl,
                             FAR const struct iwreq *request);
static int sv6621_ioctl_key(FAR struct sv6621_ioctl_s *ioctl,
                            FAR const struct iwreq *request);
static int sv6621_ioctl_bssid(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request, bool set);
static int sv6621_ioctl_essid(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request, bool set);
static int sv6621_ioctl_frequency(FAR struct sv6621_ioctl_s *ioctl,
                                  FAR struct iwreq *request);
static int sv6621_ioctl_decode_channel(FAR const struct iw_freq *frequency,
                                       FAR uint8_t *channel);
static int sv6621_ioctl_frequency_set(FAR struct sv6621_ioctl_s *ioctl,
                                      FAR const struct iwreq *request);
static int sv6621_ioctl_range(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request);
static int sv6621_ioctl_auth_query(FAR struct sv6621_ioctl_s *ioctl,
                                   FAR struct iwreq *request);
static int sv6621_ioctl_encoding_query(FAR struct sv6621_ioctl_s *ioctl,
                                       FAR struct iwreq *request);
static int sv6621_ioctl_sensitivity(FAR struct sv6621_ioctl_s *ioctl,
                                    FAR struct iwreq *request);
static int sv6621_ioctl_rate(FAR struct sv6621_ioctl_s *ioctl,
                             FAR struct iwreq *request);
static int sv6621_ioctl_stats(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request);
static int sv6621_ioctl_scan_start(FAR struct sv6621_ioctl_s *ioctl,
                                   FAR const struct iwreq *request);
static int sv6621_ioctl_scan_results(FAR struct sv6621_ioctl_s *ioctl,
                                     FAR struct iwreq *request);
static int sv6621_ioctl_country(FAR struct sv6621_ioctl_s *ioctl,
                                FAR struct iwreq *request, bool set);
static int sv6621_ioctl_mode(FAR struct sv6621_ioctl_s *ioctl,
                             FAR struct iwreq *request, bool set);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sv6621_ioctl_auth
 ****************************************************************************/

static int sv6621_ioctl_auth(FAR struct sv6621_ioctl_s *ioctl,
                             FAR const struct iwreq *request)
{
  uint16_t index = request->u.param.flags & IW_AUTH_INDEX;
  int32_t value = request->u.param.value;

  if (ioctl->mode == IW_MODE_MASTER)
    {
      if (index == IW_AUTH_WPA_VERSION)
        {
          if (value == IW_AUTH_WPA_VERSION_DISABLED)
            {
              ioctl->access_point.security = SV6621_SECURITY_OPEN;
              memset(ioctl->access_point.credential, 0,
                     sizeof(ioctl->access_point.credential));
              ioctl->access_point.credential_length = 0;
              return 0;
            }

          if (value == IW_AUTH_WPA_VERSION_WPA2)
            {
              ioctl->access_point.security = SV6621_SECURITY_WPA2_PSK;
              return 0;
            }

          return -EOPNOTSUPP;
        }

      if (index == IW_AUTH_CIPHER_PAIRWISE ||
          index == IW_AUTH_CIPHER_GROUP)
        {
          return value == IW_AUTH_CIPHER_NONE ||
                 value == IW_AUTH_CIPHER_CCMP ? 0 : -EOPNOTSUPP;
        }

      if (index == IW_AUTH_KEY_MGMT)
        {
          return value == 0 || value == IW_AUTH_KEY_MGMT_PSK ?
                 0 : -EOPNOTSUPP;
        }

      if (index == IW_AUTH_WPA_ENABLED)
        {
          ioctl->access_point.security = value ?
              SV6621_SECURITY_WPA2_PSK : SV6621_SECURITY_OPEN;
          if (!value)
            {
              memset(ioctl->access_point.credential, 0,
                     sizeof(ioctl->access_point.credential));
              ioctl->access_point.credential_length = 0;
            }

          return 0;
        }

      return index == IW_AUTH_80211_AUTH_ALG &&
             (value & IW_AUTH_ALG_OPEN_SYSTEM) != 0 ? 0 : -EOPNOTSUPP;
    }

  switch (index)
    {
      case IW_AUTH_WPA_VERSION:
        if (value == IW_AUTH_WPA_VERSION_DISABLED)
          {
            ioctl->connection.security = SV6621_SECURITY_OPEN;
            memset(ioctl->connection.credential, 0,
                   sizeof(ioctl->connection.credential));
            ioctl->connection.credential_length = 0;
            return 0;
          }

        if (value == IW_AUTH_WPA_VERSION_WPA2)
          {
            ioctl->connection.security = SV6621_SECURITY_WPA2_PSK;
            return 0;
          }

        if (value == IW_AUTH_WPA_VERSION_WPA3)
          {
            ioctl->connection.security = SV6621_SECURITY_WPA3_SAE;
            return 0;
          }

        return -EOPNOTSUPP;

      case IW_AUTH_CIPHER_PAIRWISE:
      case IW_AUTH_CIPHER_GROUP:
        return value == IW_AUTH_CIPHER_NONE || value == IW_AUTH_CIPHER_CCMP ?
               0 : -EOPNOTSUPP;

      case IW_AUTH_KEY_MGMT:
        return value == IW_AUTH_KEY_MGMT_PSK || value == 0 ?
               0 : -EOPNOTSUPP;

      case IW_AUTH_80211_AUTH_ALG:
        return (value & IW_AUTH_ALG_OPEN_SYSTEM) != 0 ? 0 : -EOPNOTSUPP;

      case IW_AUTH_WPA_ENABLED:
        ioctl->connection.security = value ? SV6621_SECURITY_WPA2_PSK :
                                             SV6621_SECURITY_OPEN;
        if (!value)
          {
            memset(ioctl->connection.credential, 0,
                   sizeof(ioctl->connection.credential));
            ioctl->connection.credential_length = 0;
          }

        return 0;

      default:
        return -EOPNOTSUPP;
    }
}

/****************************************************************************
 * Name: sv6621_ioctl_key
 ****************************************************************************/

static int sv6621_ioctl_key(FAR struct sv6621_ioctl_s *ioctl,
                            FAR const struct iwreq *request)
{
  FAR const struct iw_encode_ext *extension = request->u.encoding.pointer;

  if (ioctl->mode == IW_MODE_MASTER)
    {
      if (extension == NULL ||
          request->u.encoding.length < sizeof(*extension) ||
          extension->alg != IW_ENCODE_ALG_CCMP || extension->key_len < 8 ||
          extension->key_len > 63 ||
          request->u.encoding.length < sizeof(*extension) +
                                       extension->key_len)
        {
          return -EINVAL;
        }

      memset(ioctl->access_point.credential, 0,
             sizeof(ioctl->access_point.credential));
      memcpy(ioctl->access_point.credential, extension->key,
             extension->key_len);
      ioctl->access_point.credential_length = extension->key_len;
      ioctl->access_point.security = SV6621_SECURITY_WPA2_PSK;
      return 0;
    }

  if (extension == NULL ||
      request->u.encoding.length < sizeof(*extension) ||
      extension->alg != IW_ENCODE_ALG_CCMP ||
      (extension->key_len < 8 || extension->key_len > SV6621_KEY_MAX_LENGTH) ||
      request->u.encoding.length < sizeof(*extension) + extension->key_len)
    {
      return -EINVAL;
    }

  memset(ioctl->connection.credential, 0,
         sizeof(ioctl->connection.credential));
  memcpy(ioctl->connection.credential, extension->key, extension->key_len);
  ioctl->connection.credential_length = extension->key_len;
  if (ioctl->connection.security != SV6621_SECURITY_WPA3_SAE)
    {
      ioctl->connection.security = SV6621_SECURITY_WPA2_PSK;
    }

  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_bssid
 ****************************************************************************/

static int sv6621_ioctl_bssid(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request, bool set)
{
  static const uint8_t zero[SV6621_MAC_LENGTH];
  struct sv6621_status_s status;
  int ret;

  if (set)
    {
      memcpy(ioctl->connection.bssid, request->u.ap_addr.sa_data,
             SV6621_MAC_LENGTH);
      ioctl->connection.bssid_valid =
          memcmp(ioctl->connection.bssid, zero, sizeof(zero)) != 0;
      if (ioctl->connection.bssid_valid)
        {
          return sv6621_connect(ioctl->owner, &ioctl->connection);
        }

      ret = sv6621_disconnect(ioctl->owner, 3);
      return ret == -ENOTCONN ? 0 : ret;
    }

  if (sv6621_get_status(ioctl->owner, &status) < 0)
    {
      return -EIO;
    }

  request->u.ap_addr.sa_family = ARPHRD_ETHER;
  memcpy(request->u.ap_addr.sa_data, status.bssid, SV6621_MAC_LENGTH);
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_essid
 ****************************************************************************/

static int sv6621_ioctl_essid(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request, bool set)
{
  struct sv6621_status_s status;
  size_t length;
  int ret;

  if (!set)
    {
      ret = sv6621_get_status(ioctl->owner, &status);
      if (ret < 0)
        {
          return ret;
        }

      if (!status.connected && !status.ap_active)
        {
          request->u.essid.length = 0;
          request->u.essid.flags = IW_ESSID_OFF;
          return 0;
        }

      if (request->u.essid.pointer == NULL ||
          request->u.essid.length < status.ssid_length)
        {
          request->u.essid.length = status.ssid_length;
          return -E2BIG;
        }

      memcpy(request->u.essid.pointer, status.ssid, status.ssid_length);
      request->u.essid.length = status.ssid_length;
      request->u.essid.flags = IW_ESSID_ON;
      return 0;
    }

  if (request->u.essid.flags == IW_ESSID_OFF)
    {
      ret = ioctl->mode == IW_MODE_MASTER ?
            sv6621_stop_ap(ioctl->owner) :
            sv6621_disconnect(ioctl->owner, 3);
      return ret == -ENOTCONN ? 0 : ret;
    }

  if (request->u.essid.pointer == NULL || request->u.essid.length == 0 ||
      request->u.essid.length > SV6621_SSID_MAX_LENGTH + 1)
    {
      return -EINVAL;
    }

  length = request->u.essid.length;
  if (length > 0 &&
      ((FAR const uint8_t *)request->u.essid.pointer)[length - 1] == 0)
    {
      length--;
    }

  if (length == 0 || length > SV6621_SSID_MAX_LENGTH)
    {
      return -EINVAL;
    }

  if (ioctl->mode == IW_MODE_MASTER)
    {
      if (ioctl->access_point.channel == 0)
        {
          return -EINVAL;
        }

      memset(ioctl->access_point.ssid, 0,
             sizeof(ioctl->access_point.ssid));
      memcpy(ioctl->access_point.ssid, request->u.essid.pointer, length);
      ioctl->access_point.ssid_length = length;
      if (request->u.essid.flags == IW_ESSID_DELAY_ON)
        {
          return 0;
        }

      ret = sv6621_stop_ap(ioctl->owner);
      if (ret < 0 && ret != -ENOTCONN)
        {
          return ret;
        }

      return sv6621_start_ap(ioctl->owner, &ioctl->access_point);
    }

  memcpy(ioctl->connection.ssid, request->u.essid.pointer, length);
  ioctl->connection.ssid_length = length;
  if (request->u.essid.flags == IW_ESSID_DELAY_ON)
    {
      return 0;
    }

  return sv6621_connect(ioctl->owner, &ioctl->connection);
}

/****************************************************************************
 * Name: sv6621_ioctl_frequency
 ****************************************************************************/

static int sv6621_ioctl_frequency(FAR struct sv6621_ioctl_s *ioctl,
                                  FAR struct iwreq *request)
{
  struct sv6621_status_s status;
  int ret;

  if (ioctl->mode == IW_MODE_MASTER)
    {
      request->u.freq.m = ioctl->access_point.channel;
      request->u.freq.e = 0;
      request->u.freq.i = ioctl->access_point.channel;
      request->u.freq.flags = ioctl->access_point.channel == 0 ?
                              IW_FREQ_AUTO : IW_FREQ_FIXED;
      return 0;
    }

  ret = sv6621_get_status(ioctl->owner, &status);
  if (ret < 0)
    {
      return ret;
    }

  request->u.freq.m = status.channel;
  request->u.freq.e = 0;
  request->u.freq.i = status.channel;
  request->u.freq.flags = status.channel == 0 ? IW_FREQ_AUTO : IW_FREQ_FIXED;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_decode_channel
 ****************************************************************************/

static int sv6621_ioctl_decode_channel(FAR const struct iw_freq *frequency,
                                       FAR uint8_t *channel)
{
  int64_t mhz;
  int exponent;
  int decoded;

  if (frequency == NULL || channel == NULL || frequency->m <= 0)
    {
      return -EINVAL;
    }

  if (frequency->e == 0 && frequency->m <= UINT8_MAX)
    {
      decoded = frequency->m;
    }
  else
    {
      mhz = frequency->m;
      exponent = frequency->e;
      while (exponent > 6)
        {
          if (mhz > INT64_MAX / 10)
            {
              return -ERANGE;
            }

          mhz *= 10;
          exponent--;
        }

      while (exponent < 6)
        {
          mhz /= 10;
          exponent++;
        }

      if (mhz == 2484)
        {
          decoded = 14;
        }
      else if (mhz >= 2412 && mhz <= 2472 && (mhz - 2407) % 5 == 0)
        {
          decoded = (mhz - 2407) / 5;
        }
      else if (mhz >= 5000 && mhz <= 5900 && (mhz - 5000) % 5 == 0)
        {
          decoded = (mhz - 5000) / 5;
        }
      else
        {
          return -EINVAL;
        }
    }

  if (decoded <= 0 || decoded > UINT8_MAX)
    {
      return -EINVAL;
    }

  *channel = decoded;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_frequency_set
 ****************************************************************************/

static int sv6621_ioctl_frequency_set(FAR struct sv6621_ioctl_s *ioctl,
                                      FAR const struct iwreq *request)
{
  uint8_t channel;
  int ret;

  if (request->u.freq.flags == IW_FREQ_AUTO || request->u.freq.m == 0)
    {
      if (ioctl->mode == IW_MODE_MASTER)
        {
          ioctl->access_point.channel = 0;
          ioctl->access_point.center_channel1 = 0;
          ioctl->access_point.center_channel2 = 0;
        }
      else
        {
          ioctl->connection.channel = 0;
        }

      return 0;
    }

  ret = sv6621_ioctl_decode_channel(&request->u.freq, &channel);
  if (ret < 0)
    {
      return ret;
    }

  if (ioctl->mode == IW_MODE_MASTER)
    {
      struct sv6621_status_s status;

      ret = sv6621_get_status(ioctl->owner, &status);
      if (ret < 0)
        {
          return ret;
        }

      if (status.ap_active)
        {
          return -EBUSY;
        }

      ioctl->access_point.channel = channel;
      ioctl->access_point.center_channel1 = channel;
      ioctl->access_point.center_channel2 = 0;
      ioctl->access_point.band = channel <= 14 ?
          SV6621_BAND_2GHZ : SV6621_BAND_5GHZ;
      return 0;
    }

  ioctl->connection.channel = channel;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_mode
 ****************************************************************************/

static int sv6621_ioctl_mode(FAR struct sv6621_ioctl_s *ioctl,
                             FAR struct iwreq *request, bool set)
{
  struct sv6621_status_s status;
  int ret;

  if (!set)
    {
      request->u.mode = ioctl->mode;
      return 0;
    }

  if (request->u.mode != IW_MODE_INFRA &&
      request->u.mode != IW_MODE_AUTO &&
      request->u.mode != IW_MODE_MASTER)
    {
      return -EOPNOTSUPP;
    }

  ret = sv6621_get_status(ioctl->owner, &status);
  if (ret < 0)
    {
      return ret;
    }

  if (request->u.mode == IW_MODE_MASTER)
    {
      if (status.connected)
        {
          ret = sv6621_disconnect(ioctl->owner, 3);
          if (ret < 0 && ret != -ENOTCONN)
            {
              return ret;
            }
        }
    }
  else if (status.ap_active)
    {
      ret = sv6621_stop_ap(ioctl->owner);
      if (ret < 0 && ret != -ENOTCONN)
        {
          return ret;
        }
    }

  ioctl->mode = request->u.mode;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_range
 ****************************************************************************/

static int sv6621_ioctl_range(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request)
{
  FAR struct iw_range *range = request->u.data.pointer;
  size_t index;
  size_t output = 0;
  unsigned int pass;

  if (range == NULL || request->u.data.length < sizeof(*range))
    {
      request->u.data.length = sizeof(*range);
      return -E2BIG;
    }

  memset(range, 0, sizeof(*range));
  for (pass = 0; pass < 2 && output < IW_MAX_FREQUENCIES; pass++)
    {
      bool passive = pass != 0;

      for (index = 0; index < ioctl->owner->scan_channel_count &&
                      output < IW_MAX_FREQUENCIES;
           index++)
        {
          FAR const struct sv6621_scan_channel_s *channel =
              &ioctl->owner->scan_channels[index];

          if (((channel->flags & SV6621_SCAN_FLAG_PASSIVE) != 0) != passive)
            {
              continue;
            }

          range->freq[output].m = channel->number;
          range->freq[output].e = 0;
          range->freq[output].i = channel->number;
          range->freq[output].flags = IW_FREQ_FIXED;
          output++;
        }
    }

  range->num_frequency = output;
  request->u.data.length = sizeof(*range);
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_auth_query
 ****************************************************************************/

static int sv6621_ioctl_auth_query(FAR struct sv6621_ioctl_s *ioctl,
                                   FAR struct iwreq *request)
{
  if (ioctl->mode == IW_MODE_MASTER)
    {
      FAR const struct sv6621_ap_config_s *config = &ioctl->access_point;

      switch (request->u.param.flags & IW_AUTH_INDEX)
        {
          case IW_AUTH_WPA_VERSION:
            request->u.param.value =
                config->security == SV6621_SECURITY_OPEN ?
                IW_AUTH_WPA_VERSION_DISABLED : IW_AUTH_WPA_VERSION_WPA2;
            return 0;

          case IW_AUTH_CIPHER_PAIRWISE:
          case IW_AUTH_CIPHER_GROUP:
            request->u.param.value =
                config->security == SV6621_SECURITY_OPEN ?
                IW_AUTH_CIPHER_NONE : IW_AUTH_CIPHER_CCMP;
            return 0;

          case IW_AUTH_KEY_MGMT:
            request->u.param.value =
                config->security == SV6621_SECURITY_OPEN ?
                0 : IW_AUTH_KEY_MGMT_PSK;
            return 0;

          case IW_AUTH_WPA_ENABLED:
          case IW_AUTH_PRIVACY_INVOKED:
            request->u.param.value =
                config->security == SV6621_SECURITY_OPEN ? 0 : 1;
            return 0;

          case IW_AUTH_80211_AUTH_ALG:
            request->u.param.value = IW_AUTH_ALG_OPEN_SYSTEM;
            return 0;

          default:
            return -EOPNOTSUPP;
        }
    }

  switch (request->u.param.flags & IW_AUTH_INDEX)
    {
      case IW_AUTH_WPA_VERSION:
        if (ioctl->connection.security == SV6621_SECURITY_OPEN)
          {
            request->u.param.value = IW_AUTH_WPA_VERSION_DISABLED;
          }
        else if (ioctl->connection.security == SV6621_SECURITY_WPA3_SAE)
          {
            request->u.param.value = IW_AUTH_WPA_VERSION_WPA3;
          }
        else
          {
            request->u.param.value = IW_AUTH_WPA_VERSION_WPA2;
          }

        return 0;

      case IW_AUTH_CIPHER_PAIRWISE:
      case IW_AUTH_CIPHER_GROUP:
        request->u.param.value =
            ioctl->connection.security == SV6621_SECURITY_OPEN ?
            IW_AUTH_CIPHER_NONE : IW_AUTH_CIPHER_CCMP;
        return 0;

      case IW_AUTH_KEY_MGMT:
        request->u.param.value =
            ioctl->connection.security == SV6621_SECURITY_OPEN ?
            0 : IW_AUTH_KEY_MGMT_PSK;
        return 0;

      case IW_AUTH_80211_AUTH_ALG:
        request->u.param.value = IW_AUTH_ALG_OPEN_SYSTEM;
        return 0;

      case IW_AUTH_WPA_ENABLED:
      case IW_AUTH_PRIVACY_INVOKED:
        request->u.param.value =
            ioctl->connection.security == SV6621_SECURITY_OPEN ? 0 : 1;
        return 0;

      default:
        return -EOPNOTSUPP;
    }
}

/****************************************************************************
 * Name: sv6621_ioctl_encoding_query
 ****************************************************************************/

static int sv6621_ioctl_encoding_query(FAR struct sv6621_ioctl_s *ioctl,
                                       FAR struct iwreq *request)
{
  request->u.encoding.length = 0;
  request->u.encoding.flags =
      (ioctl->mode == IW_MODE_MASTER ?
       ioctl->access_point.security : ioctl->connection.security) ==
      SV6621_SECURITY_OPEN ?
      IW_ENCODE_DISABLED : IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_sensitivity
 ****************************************************************************/

static int sv6621_ioctl_sensitivity(FAR struct sv6621_ioctl_s *ioctl,
                                    FAR struct iwreq *request)
{
  struct sv6621_link_stats_s stats;
  int ret;

  ret = sv6621_get_link_stats(ioctl->owner, &stats);
  if (ret < 0)
    {
      return ret;
    }

  request->u.sens.value = stats.signal_dbm;
  request->u.sens.fixed = 1;
  request->u.sens.disabled = 0;
  request->u.sens.flags = IW_QUAL_DBM;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_rate
 ****************************************************************************/

static int sv6621_ioctl_rate(FAR struct sv6621_ioctl_s *ioctl,
                             FAR struct iwreq *request)
{
  struct sv6621_link_stats_s stats;
  uint64_t bitrate;
  int ret;

  ret = sv6621_get_link_stats(ioctl->owner, &stats);
  if (ret < 0)
    {
      return ret;
    }

  bitrate = (uint64_t)stats.tx_bitrate_100kbps * 100000;
  request->u.bitrate.value = bitrate > INT32_MAX ? INT32_MAX : bitrate;
  request->u.bitrate.fixed = 0;
  request->u.bitrate.disabled = stats.tx_bitrate_100kbps == 0;
  request->u.bitrate.flags = 0;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_stats
 ****************************************************************************/

static int sv6621_ioctl_stats(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request)
{
  FAR struct iw_statistics *wireless = request->u.data.pointer;
  struct sv6621_link_stats_s stats;
  int quality;
  int ret;

  if (wireless == NULL || request->u.data.length < sizeof(*wireless))
    {
      request->u.data.length = sizeof(*wireless);
      return -E2BIG;
    }

  ret = sv6621_get_link_stats(ioctl->owner, &stats);
  if (ret < 0)
    {
      return ret;
    }

  memset(wireless, 0, sizeof(*wireless));
  quality = stats.signal_dbm - stats.noise_dbm;
  if (quality < 0)
    {
      quality = 0;
    }
  else if (quality > UINT8_MAX)
    {
      quality = UINT8_MAX;
    }

  wireless->qual.qual = quality;
  wireless->qual.level = (uint8_t)stats.signal_dbm;
  wireless->qual.noise = (uint8_t)stats.noise_dbm;
  wireless->qual.updated = IW_QUAL_ALL_UPDATED | IW_QUAL_DBM;
  wireless->discard.retries = stats.tx_failed;
  request->u.data.length = sizeof(*wireless);
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_scan_start
 ****************************************************************************/

static int sv6621_ioctl_scan_start(FAR struct sv6621_ioctl_s *ioctl,
                                   FAR const struct iwreq *request)
{
  FAR const struct iw_scan_req *scan_request = request->u.data.pointer;
  struct sv6621_scan_channel_s selected[SV6621_REGULATORY_SCAN_CHANNEL_CAPACITY];
  FAR const uint8_t *ssid = NULL;
  size_t selected_count = 0;
  size_t ssid_length = 0;
  size_t available;
  size_t index;
  size_t requested;
  bool passive = false;
  int ret;

  if (scan_request != NULL && request->u.data.length < sizeof(*scan_request))
    {
      return -EINVAL;
    }

  if ((request->u.data.flags & IW_SCAN_THIS_ESSID) != 0)
    {
      if (scan_request == NULL ||
          request->u.data.length < sizeof(*scan_request) ||
          scan_request->essid_len == 0 ||
          scan_request->essid_len > IW_ESSID_MAX_SIZE)
        {
          return -EINVAL;
        }

      ssid = scan_request->essid;
      ssid_length = scan_request->essid_len;
    }

  if (scan_request != NULL)
    {
      if (scan_request->scan_type != IW_SCAN_TYPE_ACTIVE &&
          scan_request->scan_type != IW_SCAN_TYPE_PASSIVE)
        {
          return -EINVAL;
        }

      passive = scan_request->scan_type == IW_SCAN_TYPE_PASSIVE;
      requested = scan_request->num_channels;
      if (requested > IW_MAX_FREQUENCIES)
        {
          return -E2BIG;
        }
    }
  else
    {
      requested = 0;
    }

  available = ioctl->owner->scan_channel_count;
  if (requested == 0)
    {
      memcpy(selected, ioctl->owner->scan_channels,
             available * sizeof(*selected));
      selected_count = available;
    }
  else
    {
      size_t request_index;

      for (request_index = 0; request_index < requested; request_index++)
        {
          uint8_t channel;
          bool duplicate = false;
          bool found = false;

          ret = sv6621_ioctl_decode_channel(
              &scan_request->channel_list[request_index], &channel);
          if (ret < 0)
            {
              return ret;
            }

          for (index = 0; index < selected_count; index++)
            {
              if (selected[index].number == channel)
                {
                  duplicate = true;
                  break;
                }
            }

          if (duplicate)
            {
              continue;
            }

          for (index = 0; index < available; index++)
            {
              if (ioctl->owner->scan_channels[index].number == channel)
                {
                  selected[selected_count++] =
                      ioctl->owner->scan_channels[index];
                  found = true;
                  break;
                }
            }

          if (!found)
            {
              return -EINVAL;
            }
        }
    }

  if (selected_count == 0)
    {
      return -EINVAL;
    }

  if (passive)
    {
      for (index = 0; index < selected_count; index++)
        {
          selected[index].flags |= SV6621_SCAN_FLAG_PASSIVE;
        }
    }

  return sv6621_scan_selected(ioctl->owner, selected, selected_count, ssid,
                              ssid_length);
}

/****************************************************************************
 * Name: sv6621_ioctl_scan_results
 ****************************************************************************/

static int sv6621_ioctl_scan_results(FAR struct sv6621_ioctl_s *ioctl,
                                     FAR struct iwreq *request)
{
  FAR struct sv6621_bss_s *results;
  FAR struct iw_event *event;
  FAR char *cursor;
  size_t required = 0;
  size_t count = SV6621_SCAN_CACHE_CAPACITY;
  size_t index;
  size_t next;
  bool active;
  int ret;

  ret = nxmutex_lock(&ioctl->owner->scan.lock);
  if (ret < 0)
    {
      return ret;
    }

  active = ioctl->owner->scan.active || ioctl->owner->scan.stopping;
  nxmutex_unlock(&ioctl->owner->scan.lock);
  if (active)
    {
      request->u.data.length = 0;
      return -EAGAIN;
    }

  results = kmm_malloc(sizeof(*results) * SV6621_SCAN_CACHE_CAPACITY);
  if (results == NULL)
    {
      request->u.data.length = 0;
      return -ENOMEM;
    }

  ret = sv6621_scan_cache_snapshot(&ioctl->owner->scan.cache, results,
                                    &count);
  if (ret < 0)
    {
      request->u.data.length = 0;
      goto free_results;
    }

  for (index = 0; index < count; index++)
    {
      required += IW_EV_LEN(ap_addr) + IW_EV_LEN(essid) +
                  ((results[index].ssid_length + 3) & ~3) +
                  IW_EV_LEN(mode) + IW_EV_LEN(qual) + IW_EV_LEN(freq) +
                  IW_EV_LEN(data);
    }

  if (request->u.data.pointer == NULL || request->u.data.length < required)
    {
      request->u.data.length = required;
      ret = -E2BIG;
      goto free_results;
    }

  for (index = 0; index < count; index++)
    {
      for (next = index + 1; next < count; next++)
        {
          if (results[next].signal_dbm > results[index].signal_dbm)
            {
              struct sv6621_bss_s temporary = results[index];

              results[index] = results[next];
              results[next] = temporary;
            }
        }
    }

  cursor = request->u.data.pointer;
  for (index = 0; index < count; index++)
    {
      FAR const struct sv6621_bss_s *bss = &results[index];
      size_t event_length;

      event = (FAR struct iw_event *)cursor;
      memset(event, 0, IW_EV_LEN(ap_addr));
      event->cmd = SIOCGIWAP;
      event->u.ap_addr.sa_family = ARPHRD_ETHER;
      memcpy(event->u.ap_addr.sa_data, bss->bssid, SV6621_MAC_LENGTH);
      event->len = IW_EV_LEN(ap_addr);
      cursor += event->len;

      event = (FAR struct iw_event *)cursor;
      event_length = IW_EV_LEN(essid) + ((bss->ssid_length + 3) & ~3);
      memset(event, 0, event_length);
      event->cmd = SIOCGIWESSID;
      event->u.essid.flags = IW_ESSID_ON;
      event->u.essid.length = bss->ssid_length;
      event->u.essid.pointer = (FAR void *)sizeof(event->u.essid);
      memcpy(&event->u.essid + 1, bss->ssid, bss->ssid_length);
      event->len = event_length;
      cursor += event->len;

      event = (FAR struct iw_event *)cursor;
      memset(event, 0, IW_EV_LEN(mode));
      event->cmd = SIOCGIWMODE;
      event->u.mode = IW_MODE_INFRA;
      event->len = IW_EV_LEN(mode);
      cursor += event->len;

      event = (FAR struct iw_event *)cursor;
      memset(event, 0, IW_EV_LEN(qual));
      event->cmd = IWEVQUAL;
      event->u.qual.level = (uint8_t)bss->signal_dbm;
      event->u.qual.updated = IW_QUAL_DBM | IW_QUAL_LEVEL_UPDATED;
      event->len = IW_EV_LEN(qual);
      cursor += event->len;

      event = (FAR struct iw_event *)cursor;
      memset(event, 0, IW_EV_LEN(freq));
      event->cmd = SIOCGIWFREQ;
      event->u.freq.m = bss->channel;
      event->len = IW_EV_LEN(freq);
      cursor += event->len;

      event = (FAR struct iw_event *)cursor;
      memset(event, 0, IW_EV_LEN(data));
      event->cmd = SIOCGIWENCODE;
      event->u.data.flags = bss->security == SV6621_SECURITY_OPEN ?
                            IW_ENCODE_DISABLED :
                            IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
      event->len = IW_EV_LEN(data);
      cursor += event->len;
    }

  request->u.data.length = cursor - (FAR char *)request->u.data.pointer;
  ret = 0;

free_results:
  kmm_free(results);
  return ret;
}

/****************************************************************************
 * Name: sv6621_ioctl_country
 ****************************************************************************/

static int sv6621_ioctl_country(FAR struct sv6621_ioctl_s *ioctl,
                                FAR struct iwreq *request, bool set)
{
  FAR char *country = request->u.data.pointer;

  if (country == NULL)
    {
      return -EINVAL;
    }

  if (request->u.data.length < (set ? 2 : 3))
    {
      request->u.data.length = set ? 2 : 3;
      return set ? -EINVAL : -E2BIG;
    }

  if (set)
    {
      return sv6621_set_country(ioctl->owner, country);
    }

  request->u.data.length = 3;
  return sv6621_get_country(ioctl->owner, country);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sv6621_ioctl_init(FAR struct sv6621_ioctl_s *ioctl,
                      FAR struct sv6621_dev_s *owner)
{
  int ret;

  if (ioctl == NULL || owner == NULL)
    {
      return -EINVAL;
    }

  memset(ioctl, 0, sizeof(*ioctl));
  ret = nxmutex_init(&ioctl->lock);
  if (ret >= 0)
    {
      ioctl->owner = owner;
      ioctl->connection.security = SV6621_SECURITY_OPEN;
      ioctl->access_point.channel_width = SV6621_CHANNEL_WIDTH_20;
      ioctl->access_point.beacon_interval = 100;
      ioctl->access_point.dtim_period = 2;
      ioctl->access_point.security = SV6621_SECURITY_OPEN;
      ioctl->mode = IW_MODE_INFRA;
    }

  return ret;
}

void sv6621_ioctl_deinit(FAR struct sv6621_ioctl_s *ioctl)
{
  if (ioctl != NULL && ioctl->owner != NULL)
    {
      memset(ioctl->connection.credential, 0,
             sizeof(ioctl->connection.credential));
      memset(ioctl->access_point.credential, 0,
             sizeof(ioctl->access_point.credential));
      ioctl->owner = NULL;
      nxmutex_destroy(&ioctl->lock);
    }
}

int sv6621_ioctl_handle(FAR struct sv6621_ioctl_s *ioctl, int command,
                        unsigned long argument)
{
  FAR struct iwreq *request = (FAR struct iwreq *)argument;
  int ret;

  if (ioctl == NULL || ioctl->owner == NULL || request == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ioctl->lock);
  if (ret < 0)
    {
      return ret;
    }

  switch (command)
    {
      case SIOCGIWNAME:
        strlcpy(request->u.name, "SV6621 FullMAC", sizeof(request->u.name));
        ret = 0;
        break;

      case SIOCSIWAUTH:
        ret = sv6621_ioctl_auth(ioctl, request);
        break;

      case SIOCGIWAUTH:
        ret = sv6621_ioctl_auth_query(ioctl, request);
        break;

      case SIOCSIWENCODEEXT:
        ret = sv6621_ioctl_key(ioctl, request);
        break;

      case SIOCGIWENCODE:
        ret = sv6621_ioctl_encoding_query(ioctl, request);
        break;

      case SIOCSIWAP:
        ret = sv6621_ioctl_bssid(ioctl, request, true);
        break;

      case SIOCGIWAP:
        ret = sv6621_ioctl_bssid(ioctl, request, false);
        break;

      case SIOCSIWESSID:
        ret = sv6621_ioctl_essid(ioctl, request, true);
        break;

      case SIOCGIWESSID:
        ret = sv6621_ioctl_essid(ioctl, request, false);
        break;

      case SIOCSIWSCAN:
        ret = sv6621_ioctl_scan_start(ioctl, request);
        break;

      case SIOCGIWSCAN:
        ret = sv6621_ioctl_scan_results(ioctl, request);
        break;

      case SIOCSIWMODE:
        ret = sv6621_ioctl_mode(ioctl, request, true);
        break;

      case SIOCGIWMODE:
        ret = sv6621_ioctl_mode(ioctl, request, false);
        break;

      case SIOCGIWFREQ:
        ret = sv6621_ioctl_frequency(ioctl, request);
        break;

      case SIOCSIWFREQ:
        ret = sv6621_ioctl_frequency_set(ioctl, request);
        break;

      case SIOCGIWRANGE:
        ret = sv6621_ioctl_range(ioctl, request);
        break;

      case SIOCGIWSENS:
        ret = sv6621_ioctl_sensitivity(ioctl, request);
        break;

      case SIOCGIWRATE:
        ret = sv6621_ioctl_rate(ioctl, request);
        break;

      case SIOCGIWSTATS:
        ret = sv6621_ioctl_stats(ioctl, request);
        break;

      case SIOCSIWCOUNTRY:
        ret = sv6621_ioctl_country(ioctl, request, true);
        break;

      case SIOCGIWCOUNTRY:
        ret = sv6621_ioctl_country(ioctl, request, false);
        break;

      case SIOCSIWPWSAVE:
        /* Legacy station power save is selected autonomously by firmware,
         * matching the vendor set_power_mgmt contract.
         */

        ret = 0;
        break;

      case SIOCGIWPWSAVE:
        request->u.power.flags = 1;
        ret = 0;
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  nxmutex_unlock(&ioctl->lock);
  return ret;
}
