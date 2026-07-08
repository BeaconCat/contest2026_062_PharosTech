/****************************************************************************
 * arch/arm64/src/rk3576/rk3576_rptun.c
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
 * RK3576 AMP rptun back end (openvela rpmsg remote, Linux master).
 *
 * This is the openvela side of the OpenAMP rpmsg link to a Linux master
 * running on another cluster of the same RK3576.  Motivation: legal WiFi.
 * The on-board RTL8822CS driver is GPLv2-only and cannot link into Apache
 * openvela, so Linux owns the radio (rtw88) and openvela reaches the network
 * through this rpmsg bridge (two independent kernels sharing memory = mere
 * aggregation, so the licenses do not mix).
 *
 * The Linux master is Rockchip's rpmsg-over-mailbox transport
 * (drivers/rpmsg/rockchip_rpmsg_mbox.c, DT arch/arm64/.../rk3576-amp.dtsi),
 * which is NOT the standard OpenAMP remoteproc protocol: there is no
 * resource table on the wire, the vrings live at fixed reserved-memory
 * addresses, and a "kick" is a mailbox message {cmd = link-id, data = magic}.
 * We therefore:
 *   - Build our OWN static resource table locally (both sides just need the
 *     same vring geometry); get_resource() never waits for a peer to publish
 *     one.
 *   - Pin the two vrings at the Rockchip reserved-memory addresses with the
 *     Rockchip geometry (64 buffers, 4 KiB align, 512-byte buffers), and the
 *     rpmsg buffer pool at the shared-dma carveout.
 *   - Advertise only VIRTIO_RPMSG_F_NS (name service) -- the openvela rpmsg
 *     extensions (ACK/BUFSZ/CPUNAME) are not understood by the Linux peer.
 *   - notify() sends the Rockchip mailbox message; the mailbox RX interrupt
 *     is bridged back into the rpmsg core.
 *
 * Wire details (features/role/buffer ownership) are validated on-board during
 * AMP bring-up; see docs 工具/AMP互通契约.md for the authoritative contract.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/kmalloc.h>
#include <nuttx/rptun/rptun.h>

#include "rk3576_mailbox.h"
#include "rk3576_rptun.h"

#ifdef CONFIG_RK3576_RPTUN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Rockchip rpmsg-over-mailbox wire constants (must match the Linux master,
 * include/linux/rpmsg/rockchip_rpmsg.h + rk3576-amp.dtsi).
 */

#define RK3576_RPMSG_MBOX_MAGIC   0x524d5347            /* "RMSG" */
#define RK3576_RPMSG_LINK_ID      0x03                  /* master cpu0 <-> remote cpu3 */

#define RK3576_RPMSG_VRING0_DA    0x47800000            /* rpmsg reserved-memory base */
#define RK3576_RPMSG_VRING_SIZE   0x8000                /* per-vring window */
#define RK3576_RPMSG_VRING1_DA    (RK3576_RPMSG_VRING0_DA + RK3576_RPMSG_VRING_SIZE)
#define RK3576_RPMSG_VRING_NUM    64                    /* RPMSG_BUF_COUNT */
#define RK3576_RPMSG_VRING_ALIGN  0x1000                /* 4 KiB (Linux requirement) */
#define RK3576_RPMSG_BUF_SIZE     512                   /* 496 payload + 16 header */

#define RK3576_RPMSG_BUFFER_DA    0x47a00000            /* rpmsg-dma shared pool */
#define RK3576_RPMSG_BUFFER_LEN   0x200000              /* 2 MiB */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_rptun_dev_s
{
  struct rptun_dev_s      rptun;    /* Base rptun device (must be first)    */
  rptun_callback_t        callback; /* rpmsg core notify callback           */
  void                   *arg;      /* Opaque argument for the callback     */
  char                    cpuname[RPMSG_NAME_SIZE + 1];
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static const char *rk3576_rptun_get_cpuname(struct rptun_dev_s *dev);
static struct resource_table *
                   rk3576_rptun_get_resource(struct rptun_dev_s *dev);
static bool        rk3576_rptun_is_autostart(struct rptun_dev_s *dev);
static bool        rk3576_rptun_is_master(struct rptun_dev_s *dev);
static int         rk3576_rptun_start(struct rptun_dev_s *dev);
static int         rk3576_rptun_stop(struct rptun_dev_s *dev);
static int         rk3576_rptun_notify(struct rptun_dev_s *dev,
                                       uint32_t notifyid);
static int         rk3576_rptun_register_callback(struct rptun_dev_s *dev,
                                                  rptun_callback_t callback,
                                                  void *arg);
static void        rk3576_rptun_mbox_callback(void *arg, uint32_t cmd);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The static resource table describing the fixed Rockchip vring layout.
 * We publish it ourselves (ver = 1) so get_resource() never blocks.
 */

static struct rptun_rsc_s g_rk3576_rptun_rsc aligned_data(8);

static const struct rptun_ops_s g_rk3576_rptun_ops =
{
  .get_cpuname       = rk3576_rptun_get_cpuname,
  .get_resource      = rk3576_rptun_get_resource,
  .is_autostart      = rk3576_rptun_is_autostart,
  .is_master         = rk3576_rptun_is_master,
  .start             = rk3576_rptun_start,
  .stop              = rk3576_rptun_stop,
  .notify            = rk3576_rptun_notify,
  .register_callback = rk3576_rptun_register_callback,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rptun_setup_rsc
 *
 * Description:
 *   Populate the static resource table with the fixed Rockchip vring
 *   geometry.  openvela is the rpmsg remote (device role); the Linux master
 *   is the driver/host and owns the buffer pool carveout.
 ****************************************************************************/

static void rk3576_rptun_setup_rsc(void)
{
  struct rptun_rsc_s *rsc = &g_rk3576_rptun_rsc;

  memset(rsc, 0, sizeof(*rsc));

  rsc->offset[0]                = offsetof(struct rptun_rsc_s, rpmsg_vdev);

  rsc->rpmsg_vdev.type          = RSC_VDEV;
  rsc->rpmsg_vdev.id            = VIRTIO_ID_RPMSG;
  rsc->rpmsg_vdev.dfeatures     = 1 << VIRTIO_RPMSG_F_NS;
  rsc->rpmsg_vdev.num_of_vrings = 2;
  rsc->rpmsg_vdev.notifyid      = RSC_NOTIFY_ID_ANY;
  rsc->rpmsg_vdev.reserved[0]   = VIRTIO_DEV_DEVICE;

  rsc->rpmsg_vring0.da          = RK3576_RPMSG_VRING0_DA;
  rsc->rpmsg_vring0.align       = RK3576_RPMSG_VRING_ALIGN;
  rsc->rpmsg_vring0.num         = RK3576_RPMSG_VRING_NUM;
  rsc->rpmsg_vring0.notifyid    = 0;

  rsc->rpmsg_vring1.da          = RK3576_RPMSG_VRING1_DA;
  rsc->rpmsg_vring1.align       = RK3576_RPMSG_VRING_ALIGN;
  rsc->rpmsg_vring1.num         = RK3576_RPMSG_VRING_NUM;
  rsc->rpmsg_vring1.notifyid    = 1;

  rsc->offset[1]                = offsetof(struct rptun_rsc_s, carveout);
  rsc->carveout.type            = RSC_CARVEOUT;
  rsc->carveout.da              = RK3576_RPMSG_BUFFER_DA;
  rsc->carveout.pa              = RK3576_RPMSG_BUFFER_DA;
  rsc->carveout.len             = RK3576_RPMSG_BUFFER_LEN;
  memcpy(rsc->carveout.name, "vdev0buffer", 11);

  rsc->rsc_tbl_hdr.num          = 2;
  rsc->rsc_tbl_hdr.ver          = 1;
}

static const char *rk3576_rptun_get_cpuname(struct rptun_dev_s *dev)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;
  return priv->cpuname;
}

static struct resource_table *
rk3576_rptun_get_resource(struct rptun_dev_s *dev)
{
  /* The table is built locally in rk3576_rptun_init(); no peer to wait for. */

  return &g_rk3576_rptun_rsc.rsc_tbl_hdr;
}

static bool rk3576_rptun_is_autostart(struct rptun_dev_s *dev)
{
  return true;
}

static bool rk3576_rptun_is_master(struct rptun_dev_s *dev)
{
  /* Linux is the rpmsg master; openvela is the remote. */

  return false;
}

static int rk3576_rptun_start(struct rptun_dev_s *dev)
{
  return 0;
}

static int rk3576_rptun_stop(struct rptun_dev_s *dev)
{
  return 0;
}

/****************************************************************************
 * Name: rk3576_rptun_notify
 *
 * Description:
 *   Kick the Linux master.  Rockchip's mailbox message is {cmd = link-id,
 *   data = magic}; a single kick makes the peer scan both virtqueues, so the
 *   notifyid itself is not carried.
 ****************************************************************************/

static int rk3576_rptun_notify(struct rptun_dev_s *dev, uint32_t notifyid)
{
  rk3576_mailbox_send(RK3576_RPMSG_LINK_ID, RK3576_RPMSG_MBOX_MAGIC);
  return 0;
}

/****************************************************************************
 * Name: rk3576_rptun_register_callback
 *
 * Description:
 *   Store the rpmsg core callback and route the mailbox RX doorbell to it.
 ****************************************************************************/

static int rk3576_rptun_register_callback(struct rptun_dev_s *dev,
                                          rptun_callback_t callback,
                                          void *arg)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;

  priv->callback = callback;
  priv->arg      = arg;

  if (callback != NULL)
    {
      rk3576_mailbox_register_callback(rk3576_rptun_mbox_callback, priv);
    }
  else
    {
      rk3576_mailbox_register_callback(NULL, NULL);
    }

  return 0;
}

/****************************************************************************
 * Name: rk3576_rptun_mbox_callback
 *
 * Description:
 *   Mailbox RX doorbell handler (runs in the mailbox IRQ context).  The peer
 *   sends {link-id, magic}; a kick means "scan the virtqueues", so we notify
 *   the rpmsg core for all queues.
 ****************************************************************************/

static void rk3576_rptun_mbox_callback(void *arg, uint32_t cmd)
{
  struct rk3576_rptun_dev_s *priv = arg;

  if (priv->callback != NULL)
    {
      priv->callback(priv->arg, RPTUN_NOTIFY_ALL);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int rk3576_rptun_init(const char *cpuname)
{
  struct rk3576_rptun_dev_s *priv;
  int ret;

  priv = kmm_zalloc(sizeof(*priv));
  if (priv == NULL)
    {
      return -ENOMEM;
    }

  priv->rptun.ops = &g_rk3576_rptun_ops;
  strlcpy(priv->cpuname, cpuname, sizeof(priv->cpuname));

  rk3576_rptun_setup_rsc();

  ret = rk3576_mailbox_initialize();
  if (ret < 0)
    {
      mcerr("ERROR: mailbox init failed: %d\n", ret);
      goto err;
    }

  ret = rptun_initialize(&priv->rptun);
  if (ret < 0)
    {
      mcerr("ERROR: rptun_initialize failed: %d\n", ret);
      goto err;
    }

  return OK;

err:
  kmm_free(priv);
  return ret;
}

#endif /* CONFIG_RK3576_RPTUN */
