/****************************************************************************
 * chips/rk3576/rk3576_mailbox.c
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

/* Command/data transmit primitive.  The caller enables the mailbox clock. */

#include "hardware/rk3576_mailbox.h"
#include "arm64_internal.h"
#include "hardware/rk3576_memorymap.h"
#include "rk3576_mailbox.h"
#include <errno.h>
#include <nuttx/arch.h>
#include <nuttx/config.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_MAILBOX
static inline uint32_t rk3576_mailbox_getreg(uintptr_t base,
                                             unsigned int offset);
static inline void rk3576_mailbox_putreg(uint32_t value, uintptr_t base,
                                         unsigned int offset);
static uintptr_t rk3576_mailbox_base(unsigned int instance);

static inline uint32_t rk3576_mailbox_getreg(uintptr_t base,
                                             unsigned int offset)
{
  return getreg32(base + offset);
}

static inline void rk3576_mailbox_putreg(uint32_t value, uintptr_t base,
                                         unsigned int offset)
{
  putreg32(value, base + offset);
}

static uintptr_t rk3576_mailbox_base(unsigned int instance)
{
  if (instance >= RK3576_MAILBOX_COUNT)
    {
      return 0;
    }

  return RK3576_MAILBOX_BASE + instance * RK3576_MAILBOX_STRIDE;
}

int rk3576_mailbox_send(unsigned int instance, uint32_t cmd, uint32_t data)
{
  uintptr_t base = rk3576_mailbox_base(instance);

  if (base == 0)
    {
      return -EINVAL;
    }

  /* Select data-write trigger without changing the peer's RX enable bit. */

  rk3576_mailbox_putreg(RK3576_MBOX_TRIGGER_UPDATE | RK3576_MBOX_TRIGGER_MASK,
                        base, RK3576_MBOX_B2A_INTEN);

  if ((rk3576_mailbox_getreg(base, RK3576_MBOX_B2A_STATUS) &
       RK3576_MBOX_INT_MASK) != 0)
    {
      return -EBUSY;
    }

  rk3576_mailbox_putreg(cmd, base, RK3576_MBOX_B2A_CMD);
  UP_DMB();
  rk3576_mailbox_putreg(data, base, RK3576_MBOX_B2A_DATA);
  return OK;
}

#endif /* CONFIG_RK3576_MAILBOX */
