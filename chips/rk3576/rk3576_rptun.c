/****************************************************************************
 * chips/rk3576/rk3576_rptun.c
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
 * RK3576 AMP rptun back end.
 *
 * This is the openvela-side transport for OpenAMP rpmsg between openvela and
 * a peer OS (Linux) running on a separate CPU cluster of the same RK3576.
 * The motivating use case is legal WiFi: the on-board RTL8822CS driver is
 * GPLv2-only and cannot be linked into Apache-2.0 openvela, so Linux owns
 * the radio (rtw88) and openvela reaches the network through an rpmsg bridge
 * (virtio-net / usrsock).  Two independent kernels sharing memory is mere
 * aggregation under GPLv2 section 2, so the licenses do not mix.
 *
 * openvela is the rpmsg *remote*; Linux is the *master* and publishes the
 * resource table in shared memory (a reserved-memory carveout described by
 * the Linux remoteproc device tree).  Doorbell notification rides the
 * inter-core mailbox (rk3576_mailbox.c) instead of an SGI: notify() writes
 * the peer's A2B doorbell, and the peer's B2A interrupt is forwarded back
 * into the rpmsg core through the registered callback.
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

/* Physical base/size of the shared-memory region that holds the resource
 * table and the vrings.  This must match the reserved-memory carveout the
 * Linux master exposes to its remoteproc node; the two sides agree on it
 * out of band (device tree on Linux, these macros here).
 *
 * TODO: confirm against the final Linux remoteproc reserved-memory DT before
 * bring-up; the values below are placeholders in high DDR.
 */

#ifndef CONFIG_RK3576_RPTUN_SHM_BASE
#  define CONFIG_RK3576_RPTUN_SHM_BASE 0x7c000000
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_rptun_dev_s
{
  struct rptun_dev_s      rptun;    /* Base rptun device (must be first)    */
  rptun_callback_t        callback; /* rpmsg core notify callback           */
  void                   *arg;      /* Opaque argument for the callback     */
  bool                    master;   /* true if openvela is the rpmsg master */
  struct rptun_rsc_s     *rsc;      /* Shared-memory resource table         */
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

static const char *rk3576_rptun_get_cpuname(struct rptun_dev_s *dev)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;
  return priv->cpuname;
}

/****************************************************************************
 * Name: rk3576_rptun_get_resource
 *
 * Description:
 *   Return the shared-memory resource table.  As the remote we wait for the
 *   master (Linux) to publish it: the table header version stays zero until
 *   the master has filled it in.
 ****************************************************************************/

static struct resource_table *
rk3576_rptun_get_resource(struct rptun_dev_s *dev)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;

  if (!priv->master)
    {
      while (priv->rsc->rsc_tbl_hdr.ver == 0)
        {
          up_udelay(100);
        }
    }

  return &priv->rsc->rsc_tbl_hdr;
}

static bool rk3576_rptun_is_autostart(struct rptun_dev_s *dev)
{
  return true;
}

static bool rk3576_rptun_is_master(struct rptun_dev_s *dev)
{
  struct rk3576_rptun_dev_s *priv = (struct rk3576_rptun_dev_s *)dev;
  return priv->master;
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
 *   Kick the peer to process a virtqueue.  The vqid travels in the mailbox
 *   command word; the payload itself is already in the shared vrings.
 ****************************************************************************/

static int rk3576_rptun_notify(struct rptun_dev_s *dev, uint32_t notifyid)
{
  rk3576_mailbox_send(notifyid, 0);
  return 0;
}

/****************************************************************************
 * Name: rk3576_rptun_register_callback
 *
 * Description:
 *   Store the rpmsg core callback and route the mailbox doorbell to it.
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
 *   Mailbox B2A doorbell handler (runs in the mailbox IRQ context).  The
 *   command word carries the peer's virtqueue id, but the rpmsg core scans
 *   all queues on any kick, so RPTUN_NOTIFY_ALL is sufficient here.
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

  /* openvela is the rpmsg remote; Linux is the master. */

  priv->master    = false;
  priv->rptun.ops = &g_rk3576_rptun_ops;
  priv->rsc       = (struct rptun_rsc_s *)CONFIG_RK3576_RPTUN_SHM_BASE;
  strlcpy(priv->cpuname, cpuname, sizeof(priv->cpuname));

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
