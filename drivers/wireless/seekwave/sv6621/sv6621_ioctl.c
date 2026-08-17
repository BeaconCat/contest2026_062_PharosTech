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

  if (set)
    {
      memcpy(ioctl->connection.bssid, request->u.ap_addr.sa_data,
             SV6621_MAC_LENGTH);
      ioctl->connection.bssid_valid =
          memcmp(ioctl->connection.bssid, zero, sizeof(zero)) != 0;
      return 0;
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
  size_t length;
  int ret;

  if (!set)
    {
      if (request->u.essid.pointer == NULL ||
          request->u.essid.length < ioctl->connection.ssid_length)
        {
          request->u.essid.length = ioctl->connection.ssid_length;
          return -E2BIG;
        }

      memcpy(request->u.essid.pointer, ioctl->connection.ssid,
             ioctl->connection.ssid_length);
      request->u.essid.length = ioctl->connection.ssid_length;
      request->u.essid.flags = ioctl->connection.ssid_length == 0 ?
                               IW_ESSID_OFF : IW_ESSID_ON;
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

      case SIOCSIWENCODEEXT:
        ret = sv6621_ioctl_key(ioctl, request);
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

      case SIOCSIWMODE:
        ret = request->u.mode == IW_MODE_INFRA ||
              request->u.mode == IW_MODE_AUTO ? 0 : -EOPNOTSUPP;
        break;

      case SIOCGIWMODE:
        request->u.mode = IW_MODE_INFRA;
        ret = 0;
        break;

      default:
        ret = -ENOTTY;
        break;
    }

  nxmutex_unlock(&ioctl->lock);
  return ret;
}
