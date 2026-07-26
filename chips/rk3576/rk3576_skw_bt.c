/****************************************************************************
 * chips/rk3576/rk3576_skw_bt.c
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
 * Bluetooth HCI transport for the SeekWave SV6621 (SWT6621-S) Wi-Fi/BT
 * combo chip, implemented as a NuttX struct bt_driver_s.
 *
 * The part has no HCI UART.  Both radios live behind the same SDIO
 * function-1 data window and are separated by an 8-bit virtual channel
 * number carried in the outer packet header.  This driver therefore owns no
 * hardware of its own: the SeekWave SDIO core driver (rk3576_skw.c) boots
 * the CP firmware, runs the receive thread and demultiplexes channels; this
 * file only claims the four Bluetooth channels, performs the BT_START
 * handshake and translates between H4 framing and the NuttX Bluetooth
 * stack.
 *
 * Because there is no separate hardware block there is no clock gate and no
 * DMA buffer to manage here - the SDIO host controller clocks are already
 * owned by the SDIO host driver.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/signal.h>
#include <nuttx/wireless/bluetooth/bt_driver.h>

#include "rk3576_skw_bt.h"

#ifdef CONFIG_RK3576_SKW_BT

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Banner the CP prints on the loopcheck channel once the Bluetooth stack of
 * the firmware is up.  Matched as a substring because the banner is framed
 * by firmware-version text that differs between firmware builds.
 */

#define RK3576_SKW_BT_READY_TAG "BTREADY"

/* How long to wait for that banner, and the polling granularity. */

#define RK3576_SKW_BT_READY_TMO_MS 3000
#define RK3576_SKW_BT_POLL_MS      10

/* Fixed H4 header sizes (indicator byte excluded). */

#define RK3576_SKW_BT_EVT_HDR_LEN 2 /* event code, parameter length   */
#define RK3576_SKW_BT_ACL_HDR_LEN 4 /* handle (16), data length (16)  */
#define RK3576_SKW_BT_SCO_HDR_LEN 3 /* handle (16), data length (8)   */
#define RK3576_SKW_BT_ISO_HDR_LEN 4 /* handle (16), data length (16)  */

/* Largest HCI packet accepted in either direction.  The SeekWave command
 * path is limited to 1588 bytes by the CP; ACL fragments never exceed that
 * once the host MTU is negotiated.
 */

#define RK3576_SKW_BT_MAX_FRAME 1588

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_skw_bt_dev_s
{
  struct bt_driver_s drv; /* NuttX Bluetooth lower half (first!)  */
  volatile bool cpready;  /* CP reported BTREADY                  */
  volatile bool opened;   /* Upper half has the transport open    */
  bool hooked;            /* Channel handlers installed           */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rk3576_skw_bt_open(struct bt_driver_s *btdev);
static int rk3576_skw_bt_send(struct bt_driver_s *btdev,
                              enum bt_buf_type_e type, void *data, size_t len);
static void rk3576_skw_bt_close(struct bt_driver_s *btdev);

static int rk3576_skw_bt_channel(uint8_t h4type);
static int rk3576_skw_bt_frame_len(const uint8_t *frame, size_t avail);
static enum bt_buf_type_e rk3576_skw_bt_buftype(uint8_t h4type);
static int rk3576_skw_bt_xmit(uint8_t h4type, uint8_t *head, size_t len);

static void rk3576_skw_bt_hci_handler(uint8_t channel, const void *data,
                                      size_t len, void *arg);
static void rk3576_skw_bt_boot_handler(uint8_t channel, const void *data,
                                       size_t len, void *arg);
static void rk3576_skw_bt_log_handler(uint8_t channel, const void *data,
                                      size_t len, void *arg);

static int rk3576_skw_bt_hook_channels(struct rk3576_skw_bt_dev_s *priv);
static void rk3576_skw_bt_unhook_channels(struct rk3576_skw_bt_dev_s *priv);
static int rk3576_skw_bt_start_cp(struct rk3576_skw_bt_dev_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_skw_bt_dev_s g_skw_bt =
{
  .drv =
    {
      .head_reserve = RK3576_SKW_BT_HEAD_RESERVE,
      .open         = rk3576_skw_bt_open,
      .send         = rk3576_skw_bt_send,
      .close        = rk3576_skw_bt_close,
    },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_skw_bt_channel
 *
 * Description:
 *   Map an H4 packet indicator onto the SeekWave virtual channel that
 *   carries it.  Commands and events share the command channel; the three
 *   payload classes each have their own channel.
 *
 ****************************************************************************/

static int rk3576_skw_bt_channel(uint8_t h4type)
{
  switch (h4type)
    {
      case RK3576_SKW_BT_H4_CMD:
      case RK3576_SKW_BT_H4_EVT:
        return RK3576_SKW_CH_BT_CMD;

      case RK3576_SKW_BT_H4_ACL:
        return RK3576_SKW_CH_BT_DATA;

      case RK3576_SKW_BT_H4_SCO:
        return RK3576_SKW_CH_BT_AUDIO;

      case RK3576_SKW_BT_H4_ISO:
        return RK3576_SKW_CH_BT_ISOC;

      default:
        return -EINVAL;
    }
}

/****************************************************************************
 * Name: rk3576_skw_bt_buftype
 *
 * Description:
 *   Map an H4 packet indicator received from the CP onto the NuttX
 *   Bluetooth buffer type used to hand it to the stack.
 *
 ****************************************************************************/

static enum bt_buf_type_e rk3576_skw_bt_buftype(uint8_t h4type)
{
  switch (h4type)
    {
      case RK3576_SKW_BT_H4_ACL:
        return BT_ACL_IN;

      case RK3576_SKW_BT_H4_ISO:
        return BT_ISO_IN;

      case RK3576_SKW_BT_H4_EVT:
      default:
        return BT_EVT;
    }
}

/****************************************************************************
 * Name: rk3576_skw_bt_frame_len
 *
 * Description:
 *   Compute the total on-wire length of the H4 frame at frame, indicator
 *   byte included.
 *
 * Input Parameters:
 *   frame - Start of the frame, frame[0] is the H4 indicator.
 *   avail - Bytes available at frame.
 *
 * Returned Value:
 *   Frame length in bytes, or a negated errno value if the indicator is
 *   unknown or the frame is truncated.
 *
 ****************************************************************************/

static int rk3576_skw_bt_frame_len(const uint8_t *frame, size_t avail)
{
  size_t hdrlen;
  size_t paylen;

  if (avail < 1)
    {
      return -EINVAL;
    }

  switch (frame[0])
    {
      case RK3576_SKW_BT_H4_EVT:
        hdrlen = RK3576_SKW_BT_EVT_HDR_LEN;
        if (avail < 1 + hdrlen)
          {
            return -EMSGSIZE;
          }

        paylen = frame[2];
        break;

      case RK3576_SKW_BT_H4_ACL:
        hdrlen = RK3576_SKW_BT_ACL_HDR_LEN;
        if (avail < 1 + hdrlen)
          {
            return -EMSGSIZE;
          }

        paylen = (size_t)frame[3] | ((size_t)frame[4] << 8);
        break;

      case RK3576_SKW_BT_H4_SCO:
        hdrlen = RK3576_SKW_BT_SCO_HDR_LEN;
        if (avail < 1 + hdrlen)
          {
            return -EMSGSIZE;
          }

        paylen = frame[3];
        break;

      case RK3576_SKW_BT_H4_ISO:
        hdrlen = RK3576_SKW_BT_ISO_HDR_LEN;
        if (avail < 1 + hdrlen)
          {
            return -EMSGSIZE;
          }

        /* The ISO data load length occupies 14 bits, the upper two are
         * reserved for future use and must be masked off.
         */

        paylen = ((size_t)frame[3] | ((size_t)frame[4] << 8)) & 0x3fff;
        break;

      default:
        return -EPROTO;
    }

  if (1 + hdrlen + paylen > avail)
    {
      return -EMSGSIZE;
    }

  return (int)(1 + hdrlen + paylen);
}

/****************************************************************************
 * Name: rk3576_skw_bt_xmit
 *
 * Description:
 *   Finish and hand one outgoing frame to the SDIO core.  head points at
 *   the start of the reserved head room; the HCI payload follows it at
 *   head + RK3576_SKW_BT_HEAD_RESERVE and is len bytes long.
 *
 ****************************************************************************/

static int rk3576_skw_bt_xmit(uint8_t h4type, uint8_t *head, size_t len)
{
  int channel;

  channel = rk3576_skw_bt_channel(h4type);
  if (channel < 0)
    {
      wlerr("ERROR: bad H4 packet indicator 0x%02x\n", h4type);
      return channel;
    }

  if (len == 0 || len > RK3576_SKW_BT_MAX_FRAME)
    {
      wlerr("ERROR: bad HCI frame length %zu\n", len);
      return -EINVAL;
    }

  /* The inner link header is CP-generated on the receive side and ignored
   * on the transmit side, so an all-zero header is what the reference
   * implementation puts on the wire.
   */

  memset(head, 0, RK3576_SKW_LINK_HDR_LEN);
  head[RK3576_SKW_LINK_HDR_LEN] = h4type;

  return skw_sdio_send_channel((uint8_t)channel, head,
                               len + RK3576_SKW_BT_HEAD_RESERVE);
}

/****************************************************************************
 * Name: rk3576_skw_bt_open
 *
 * Description:
 *   struct bt_driver_s open method.  The transport is already up when the
 *   driver registers, so this only latches the open state.
 *
 ****************************************************************************/

static int rk3576_skw_bt_open(struct bt_driver_s *btdev)
{
  struct rk3576_skw_bt_dev_s *priv = (struct rk3576_skw_bt_dev_s *)btdev;

  if (!priv->cpready)
    {
      wlerr("ERROR: CP Bluetooth stack is not ready\n");
      return -ENODEV;
    }

  priv->opened = true;
  wlinfo("SeekWave HCI transport opened\n");
  return OK;
}

/****************************************************************************
 * Name: rk3576_skw_bt_close
 *
 * Description:
 *   struct bt_driver_s close method.  Received frames are dropped while the
 *   transport is closed; the CP Bluetooth stack itself stays up so that a
 *   later open() does not have to redo the BT_START handshake.
 *
 ****************************************************************************/

static void rk3576_skw_bt_close(struct bt_driver_s *btdev)
{
  struct rk3576_skw_bt_dev_s *priv = (struct rk3576_skw_bt_dev_s *)btdev;

  priv->opened = false;
  wlinfo("SeekWave HCI transport closed\n");
}

/****************************************************************************
 * Name: rk3576_skw_bt_send
 *
 * Description:
 *   struct bt_driver_s send method.  head_reserve bytes are guaranteed to
 *   be available in front of data, which lets the link header and the H4
 *   indicator be written in place without a bounce buffer.
 *
 ****************************************************************************/

static int rk3576_skw_bt_send(struct bt_driver_s *btdev,
                              enum bt_buf_type_e type, void *data, size_t len)
{
  struct rk3576_skw_bt_dev_s *priv = (struct rk3576_skw_bt_dev_s *)btdev;
  uint8_t h4type;
  int ret;

  DEBUGASSERT(data != NULL);

  if (!priv->cpready)
    {
      return -ENODEV;
    }

  switch (type)
    {
      case BT_CMD:
        h4type = RK3576_SKW_BT_H4_CMD;
        break;

      case BT_ACL_OUT:
        h4type = RK3576_SKW_BT_H4_ACL;
        break;

      case BT_ISO_OUT:
        h4type = RK3576_SKW_BT_H4_ISO;
        break;

      default:
        wlerr("ERROR: unsupported buffer type %d\n", (int)type);
        return -EINVAL;
    }

  ret = rk3576_skw_bt_xmit(h4type,
                           (uint8_t *)data - RK3576_SKW_BT_HEAD_RESERVE, len);
  if (ret < 0)
    {
      wlerr("ERROR: HCI transmit failed: %d\n", ret);
      return ret;
    }

  return (int)len;
}

/****************************************************************************
 * Name: rk3576_skw_bt_hci_handler
 *
 * Description:
 *   Receive callback shared by the Bluetooth command, ACL, SCO and ISO
 *   channels.  One channel packet may carry several concatenated H4 frames,
 *   so the payload is walked until it is exhausted.
 *
 ****************************************************************************/

static void rk3576_skw_bt_hci_handler(uint8_t channel, const void *data,
                                      size_t len, void *arg)
{
  struct rk3576_skw_bt_dev_s *priv = arg;
  const uint8_t *frame;
  size_t remaining;
  int framelen;
  int ret;

  if (len <= RK3576_SKW_LINK_HDR_LEN)
    {
      wlwarn("WARNING: runt packet on channel %u (%zu bytes)\n", channel, len);
      return;
    }

  /* Skip the inner link header; the H4 indicator is the first real byte. */

  frame = (const uint8_t *)data + RK3576_SKW_LINK_HDR_LEN;
  remaining = len - RK3576_SKW_LINK_HDR_LEN;

  while (remaining > 0)
    {
      framelen = rk3576_skw_bt_frame_len(frame, remaining);
      if (framelen < 0)
        {
          wlerr("ERROR: malformed H4 frame on channel %u: %d\n", channel,
                framelen);
          return;
        }

      if (priv->opened && priv->drv.receive != NULL)
        {
          /* The stack takes the frame without the H4 indicator byte. */

          ret = priv->drv.receive(&priv->drv, rk3576_skw_bt_buftype(frame[0]),
                                  (void *)(frame + 1), (size_t)framelen - 1);
          if (ret < 0)
            {
              wlerr("ERROR: stack refused frame: %d\n", ret);
            }
        }

      frame += framelen;
      remaining -= (size_t)framelen;

      /* Trailing padding bytes are zero and cannot start a valid frame. */

      if (remaining > 0 && frame[0] == 0)
        {
          break;
        }
    }
}

/****************************************************************************
 * Name: rk3576_skw_bt_boot_handler
 *
 * Description:
 *   Loopcheck channel callback used only during bring-up: it watches for
 *   the CP "BTREADY" banner that answers the BT_START doorbell.
 *
 ****************************************************************************/

static void rk3576_skw_bt_boot_handler(uint8_t channel, const void *data,
                                       size_t len, void *arg)
{
  struct rk3576_skw_bt_dev_s *priv = arg;
  const char *text;
  size_t textlen;
  size_t i;

  UNUSED(channel);

  if (len <= RK3576_SKW_LINK_HDR_LEN)
    {
      return;
    }

  text = (const char *)data + RK3576_SKW_LINK_HDR_LEN;
  textlen = len - RK3576_SKW_LINK_HDR_LEN;

  /* The banner is not NUL terminated and may be embedded in other text, so
   * search it byte by byte rather than with strstr().
   */

  if (textlen < sizeof(RK3576_SKW_BT_READY_TAG) - 1)
    {
      return;
    }

  for (i = 0; i <= textlen - (sizeof(RK3576_SKW_BT_READY_TAG) - 1); i++)
    {
      if (memcmp(&text[i], RK3576_SKW_BT_READY_TAG,
                 sizeof(RK3576_SKW_BT_READY_TAG) - 1) == 0)
        {
          wlinfo("CP reported BTREADY\n");
          priv->cpready = true;
          return;
        }
    }
}

/****************************************************************************
 * Name: rk3576_skw_bt_log_handler
 *
 * Description:
 *   Forward the CP Bluetooth firmware log to the system log.  Only compiled
 *   in when wireless info output is enabled, because the CP can be very
 *   chatty and the receive thread must not be starved.
 *
 ****************************************************************************/

static void rk3576_skw_bt_log_handler(uint8_t channel, const void *data,
                                      size_t len, void *arg)
{
#ifdef CONFIG_DEBUG_WIRELESS_INFO
  const char *text;
  size_t textlen;

  UNUSED(channel);
  UNUSED(arg);

  if (len <= RK3576_SKW_LINK_HDR_LEN)
    {
      return;
    }

  text = (const char *)data + RK3576_SKW_LINK_HDR_LEN;
  textlen = len - RK3576_SKW_LINK_HDR_LEN;

  wlinfo("CP BT log: %.*s\n", (int)textlen, text);
#else
  UNUSED(channel);
  UNUSED(data);
  UNUSED(len);
  UNUSED(arg);
#endif
}

/****************************************************************************
 * Name: rk3576_skw_bt_hook_channels
 *
 * Description:
 *   Claim every virtual channel the Bluetooth side uses.
 *
 ****************************************************************************/

static int rk3576_skw_bt_hook_channels(struct rk3576_skw_bt_dev_s *priv)
{
  static const uint8_t hcichan[] = {
    RK3576_SKW_CH_BT_CMD,
    RK3576_SKW_CH_BT_DATA,
    RK3576_SKW_CH_BT_AUDIO,
    RK3576_SKW_CH_BT_ISOC,
  };

  int ret;
  int i;

  for (i = 0; i < (int)nitems(hcichan); i++)
    {
      ret = skw_sdio_register_channel_handler(hcichan[i],
                                              rk3576_skw_bt_hci_handler, priv);
      if (ret < 0)
        {
          wlerr("ERROR: cannot hook channel %u: %d\n", hcichan[i], ret);
          goto errout;
        }
    }

  ret = skw_sdio_register_channel_handler(RK3576_SKW_CH_LOOPCHECK,
                                          rk3576_skw_bt_boot_handler, priv);
  if (ret < 0)
    {
      wlerr("ERROR: cannot hook loopcheck channel: %d\n", ret);
      goto errout;
    }

  ret = skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_LOG,
                                          rk3576_skw_bt_log_handler, priv);
  if (ret < 0)
    {
      /* The firmware log is optional, keep going without it. */

      wlwarn("WARNING: cannot hook BT log channel: %d\n", ret);
    }

  priv->hooked = true;
  return OK;

errout:
  rk3576_skw_bt_unhook_channels(priv);
  return ret;
}

/****************************************************************************
 * Name: rk3576_skw_bt_unhook_channels
 *
 * Description:
 *   Release every virtual channel claimed by rk3576_skw_bt_hook_channels().
 *
 ****************************************************************************/

static void rk3576_skw_bt_unhook_channels(struct rk3576_skw_bt_dev_s *priv)
{
  skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_CMD, NULL, NULL);
  skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_DATA, NULL, NULL);
  skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_AUDIO, NULL, NULL);
  skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_ISOC, NULL, NULL);
  skw_sdio_register_channel_handler(RK3576_SKW_CH_BT_LOG, NULL, NULL);

  priv->hooked = false;
}

/****************************************************************************
 * Name: rk3576_skw_bt_start_cp
 *
 * Description:
 *   Ring the AP->CP doorbell with the BT_START command and wait until the
 *   CP answers with its BTREADY banner.
 *
 ****************************************************************************/

static int rk3576_skw_bt_start_cp(struct rk3576_skw_bt_dev_s *priv)
{
  int elapsed;
  int ret;

  priv->cpready = false;

  ret = skw_sdio_func0_writeb(RK3576_SKW_REG_AP2CP_DOORBELL,
                              RK3576_SKW_DOORBELL_BT_START);
  if (ret < 0)
    {
      wlerr("ERROR: BT_START doorbell failed: %d\n", ret);
      return ret;
    }

  for (elapsed = 0; elapsed < RK3576_SKW_BT_READY_TMO_MS;
       elapsed += RK3576_SKW_BT_POLL_MS)
    {
      if (priv->cpready)
        {
          /* The loopcheck channel is only interesting until the banner
           * arrives; hand it back so the Wi-Fi side keeps ownership.
           */

          skw_sdio_register_channel_handler(RK3576_SKW_CH_LOOPCHECK, NULL,
                                            NULL);
          return OK;
        }

      nxsig_usleep(RK3576_SKW_BT_POLL_MS * USEC_PER_MSEC);
    }

  wlerr("ERROR: timed out waiting for BTREADY\n");
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_skw_bt_initialize
 *
 * Description:
 *   Bring the Bluetooth side of the SeekWave combo chip up and register it
 *   with the NuttX Bluetooth stack.  See rk3576_skw_bt.h.
 *
 ****************************************************************************/

int rk3576_skw_bt_initialize(int id)
{
  struct rk3576_skw_bt_dev_s *priv = &g_skw_bt;
  int ret;

  if (priv->hooked)
    {
      wlwarn("WARNING: SeekWave HCI transport already initialized\n");
      return -EALREADY;
    }

  if (!skw_sdio_cp_ready())
    {
      wlerr("ERROR: CP firmware is not running yet\n");
      return -ENODEV;
    }

  ret = rk3576_skw_bt_hook_channels(priv);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_skw_bt_start_cp(priv);
  if (ret < 0)
    {
      goto errout;
    }

  ret = bt_driver_register_with_id(&priv->drv, id);
  if (ret < 0)
    {
      wlerr("ERROR: bt_driver_register_with_id failed: %d\n", ret);
      goto errout;
    }

  wlinfo("SeekWave SV6621 HCI over SDIO registered as /dev/ttyBT%d\n", id);
  return OK;

errout:
  rk3576_skw_bt_unhook_channels(priv);
  priv->cpready = false;
  return ret;
}

/****************************************************************************
 * Name: rk3576_skw_bt_send_hci
 *
 * Description:
 *   Send a raw HCI packet.  See rk3576_skw_bt.h.
 *
 ****************************************************************************/

int rk3576_skw_bt_send_hci(uint8_t h4type, const void *data, size_t len)
{
  struct rk3576_skw_bt_dev_s *priv = &g_skw_bt;
  uint8_t *frame;
  int ret;

  if (data == NULL || len == 0 || len > RK3576_SKW_BT_MAX_FRAME)
    {
      return -EINVAL;
    }

  if (!priv->cpready)
    {
      return -ENODEV;
    }

  frame = kmm_malloc(len + RK3576_SKW_BT_HEAD_RESERVE);
  if (frame == NULL)
    {
      return -ENOMEM;
    }

  memcpy(frame + RK3576_SKW_BT_HEAD_RESERVE, data, len);
  ret = rk3576_skw_bt_xmit(h4type, frame, len);

  kmm_free(frame);
  return ret;
}

/****************************************************************************
 * Name: rk3576_skw_bt_is_ready
 *
 * Description:
 *   True once the CP has reported BTREADY.  See rk3576_skw_bt.h.
 *
 ****************************************************************************/

bool rk3576_skw_bt_is_ready(void) { return g_skw_bt.cpready; }

#endif /* CONFIG_RK3576_SKW_BT */
