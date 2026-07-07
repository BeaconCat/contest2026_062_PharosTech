/****************************************************************************
 * chips/rk3576/rk3576_rptun.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_RPTUN_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_RPTUN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_RK3576_RPTUN

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_rptun_init
 *
 * Description:
 *   Bring up the RK3576 AMP rptun/rpmsg transport over the inter-core
 *   mailbox.  openvela runs as the rpmsg remote; the peer OS (Linux) is the
 *   master and owns the shared-memory resource table.
 *
 * Input Parameters:
 *   cpuname - Name advertised to the peer for this rpmsg endpoint (e.g.
 *             "openvela"); must match the peer remoteproc/rpmsg config.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_rptun_init(const char *cpuname);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_RPTUN */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_RPTUN_H */
