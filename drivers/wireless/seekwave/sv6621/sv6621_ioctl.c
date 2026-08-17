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
static int sv6621_ioctl_scan_start(FAR struct sv6621_ioctl_s *ioctl);
static int sv6621_ioctl_scan_results(FAR struct sv6621_ioctl_s *ioctl,
                                     FAR struct iwreq *request);

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
  ioctl->connection.security = SV6621_SECURITY_WPA2_PSK;
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

      if (!status.connected)
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
      ret = sv6621_disconnect(ioctl->owner, 3);
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
 * Name: sv6621_ioctl_frequency_set
 ****************************************************************************/

static int sv6621_ioctl_frequency_set(FAR struct sv6621_ioctl_s *ioctl,
                                      FAR const struct iwreq *request)
{
  int64_t frequency;
  int exponent;
  int channel;

  if (request->u.freq.flags == IW_FREQ_AUTO || request->u.freq.m == 0)
    {
      ioctl->connection.channel = 0;
      return 0;
    }

  if (request->u.freq.e == 0 && request->u.freq.m <= UINT8_MAX)
    {
      channel = request->u.freq.m;
    }
  else
    {
      frequency = request->u.freq.m;
      exponent = request->u.freq.e;
      while (exponent > 6)
        {
          if (frequency > INT64_MAX / 10)
            {
              return -ERANGE;
            }

          frequency *= 10;
          exponent--;
        }

      while (exponent < 6)
        {
          frequency /= 10;
          exponent++;
        }

      if (frequency == 2484)
        {
          channel = 14;
        }
      else if (frequency >= 2412 && frequency <= 2472 &&
               (frequency - 2407) % 5 == 0)
        {
          channel = (frequency - 2407) / 5;
        }
      else if (frequency >= 5000 && frequency <= 5900 &&
               (frequency - 5000) % 5 == 0)
        {
          channel = (frequency - 5000) / 5;
        }
      else
        {
          return -EINVAL;
        }
    }

  if (channel <= 0 || channel > UINT8_MAX)
    {
      return -EINVAL;
    }

  ioctl->connection.channel = channel;
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_range
 ****************************************************************************/

static int sv6621_ioctl_range(FAR struct sv6621_ioctl_s *ioctl,
                              FAR struct iwreq *request)
{
  FAR struct iw_range *range = request->u.data.pointer;
  size_t count;
  size_t index;

  if (range == NULL || request->u.data.length < sizeof(*range))
    {
      request->u.data.length = sizeof(*range);
      return -E2BIG;
    }

  memset(range, 0, sizeof(*range));
  count = ioctl->owner->scan_channel_count;
  if (count > IW_MAX_FREQUENCIES)
    {
      count = IW_MAX_FREQUENCIES;
    }

  range->num_frequency = count;
  for (index = 0; index < count; index++)
    {
      range->freq[index].m = ioctl->owner->scan_channels[index].number;
      range->freq[index].e = 0;
      range->freq[index].i = ioctl->owner->scan_channels[index].number;
      range->freq[index].flags = IW_FREQ_FIXED;
    }

  request->u.data.length = sizeof(*range);
  return 0;
}

/****************************************************************************
 * Name: sv6621_ioctl_auth_query
 ****************************************************************************/

static int sv6621_ioctl_auth_query(FAR struct sv6621_ioctl_s *ioctl,
                                   FAR struct iwreq *request)
{
  switch (request->u.param.flags & IW_AUTH_INDEX)
    {
      case IW_AUTH_WPA_VERSION:
        request->u.param.value =
            ioctl->connection.security == SV6621_SECURITY_OPEN ?
            IW_AUTH_WPA_VERSION_DISABLED : IW_AUTH_WPA_VERSION_WPA2;
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
      ioctl->connection.security == SV6621_SECURITY_OPEN ?
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

static int sv6621_ioctl_scan_start(FAR struct sv6621_ioctl_s *ioctl)
{
  return sv6621_scan(ioctl->owner);
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

  active = ioctl->owner->scan.active;
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
    }

  return ret;
}

void sv6621_ioctl_deinit(FAR struct sv6621_ioctl_s *ioctl)
{
  if (ioctl != NULL && ioctl->owner != NULL)
    {
      memset(ioctl->connection.credential, 0,
             sizeof(ioctl->connection.credential));
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
        ret = sv6621_ioctl_scan_start(ioctl);
        break;

      case SIOCGIWSCAN:
        ret = sv6621_ioctl_scan_results(ioctl, request);
        break;

      case SIOCSIWMODE:
        ret = request->u.mode == IW_MODE_INFRA ||
              request->u.mode == IW_MODE_AUTO ? 0 : -EOPNOTSUPP;
        break;

      case SIOCGIWMODE:
        request->u.mode = IW_MODE_INFRA;
        ret = 0;
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

      default:
        ret = -ENOTTY;
        break;
    }

  nxmutex_unlock(&ioctl->lock);
  return ret;
}
