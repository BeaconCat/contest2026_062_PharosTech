/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_pd.c
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
 * RK3576 PMU power-domain driver.
 *
 * NPU, GPU, VPU, VDEC, VI, VOP, USB, PHP and the audio/storage blocks all
 * sit behind PMU power gates.  While a gate is closed the peripheral's
 * register window reads back as zero and writes are dropped, so every
 * driver for such a block has to call rk3576_pd_on() before it probes its
 * hardware.
 *
 * The PMU offers a hardware sequencer (used for system suspend) and a
 * software sequence.  This driver implements the software sequence
 * described in TRM Part1 V1.2 sections 6.5.5.2 (BIU idle by software) and
 * 6.5.6.2 (PD power up/down by software):
 *
 *   power up:   clear voltage-domain off request
 *            -> clear PMU_PWR_GATE_SFTCONx bit
 *            -> poll PMU_PWR_GATE_STS until the domain reports "power up"
 *            -> clear PMU_MEM_PWR_GATE_SFTCONx bit (leave SRAM retention)
 *            -> clear PMU_BIU_IDLE_SFTCONx bits
 *            -> poll PMU_BIU_IDLE_ACK_STS and PMU_BIU_IDLE_STS until the
 *               BIUs of the domain are active again
 *
 *   power down: exactly the reverse order.
 *
 * All PMU *_SFTCON registers are HIWORD write masked, so a read-modify-
 * write is not needed and the sequence is SMP safe without holding a lock
 * across the polling loops.  A spinlock is still taken around a whole
 * transition so two CPUs cannot drive the same domain in opposite
 * directions at the same time.
 *
 * Peripheral resets are deliberately out of scope: they live in the CRU
 * and are owned by the individual peripheral drivers, which release them
 * after their power domain is up.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/spinlock.h>

#include "arm64_internal.h"
#include "hardware/rk3576_pd.h"
#include "rk3576_pd.h"

#ifdef CONFIG_RK3576_PD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Handshake timeout.  The longest documented power ramp (VD_NPU with the
 * slow ramp counters) is far below 1 ms; 10 ms leaves ample margin while
 * still failing fast on a broken domain.
 */

#define RK3576_PD_TIMEOUT_US 10000

/* Polling granularity of the handshake loops. */

#define RK3576_PD_POLL_US 1

/* "This domain has no such resource" markers. */

#define RK3576_PD_NO_MEM    0
#define RK3576_PD_NO_IDLE   0
#define RK3576_PD_NO_VOL    0
#define RK3576_PD_NO_PARENT (-1)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Static description of one power domain.
 *
 * "reg1" selects which of the two register pairs carries the power gate
 * and memory retention bits of this domain: false -> PMU_*_SFTCON0,
 * true -> PMU_*_SFTCON1.  The flattened status registers place the
 * SFTCON1 bits 16 positions higher, which is what pwr_sts()/idle_sts()
 * below take care of.
 */

struct rk3576_pd_info_s
{
  const char *name;  /* Domain name, for logging                      */
  uint32_t pwr_mask; /* PMU_PWR_GATE_SFTCON{0,1} bit                  */
  uint32_t mem_mask; /* PMU_MEM_PWR_GATE_SFTCON{0,1} bit, 0 if none   */
  uint32_t idle0;    /* PMU_BIU_IDLE_SFTCON0 bits, 0 if none          */
  uint32_t idle1;    /* PMU_BIU_IDLE_SFTCON1 bits, 0 if none          */
  uint32_t vol_mask; /* PMU_VOL_GATE_CON{0,1} bit, 0 if not a VD      */
  int8_t parent;     /* Parent domain, RK3576_PD_NO_PARENT if none    */
  bool reg1;         /* false: SFTCON0 group, true: SFTCON1 group     */
  bool vol_reg1;     /* false: VOL_GATE_CON0, true: VOL_GATE_CON1     */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void rk3576_pd_hiword(uint32_t offset, uint32_t mask, bool set);
static bool rk3576_pd_pwr_down(const struct rk3576_pd_info_s *info);
static int rk3576_pd_wait_pwr(const struct rk3576_pd_info_s *info, bool down);
static int rk3576_pd_wait_idle(const struct rk3576_pd_info_s *info, bool idle);
static void rk3576_pd_set_idle(const struct rk3576_pd_info_s *info, bool idle);
static void rk3576_pd_set_mem_down(const struct rk3576_pd_info_s *info,
                                   bool down);
static void rk3576_pd_set_vol_off(const struct rk3576_pd_info_s *info,
                                  bool off);
static int rk3576_pd_do_on(int domain);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Domain table.  Bit assignments come from the RK3576 TRM Part1 V1.2 PMU
 * register descriptions; the BIU membership of each domain comes from TRM
 * table 6-1 ("Domain Description Summary"); the parent relations and the
 * domain numbering come from the power-controller node of the vendor
 * device tree (4-HardwareData/k7_debian_vendor.dts).
 */

static const struct rk3576_pd_info_s g_rk3576_pd_info[RK3576_PD_NDOMAINS] =
{
  [RK3576_PD_NPU] =
  {
    /* VD_NPU root.  No BIU of its own: BIU_NPUTOP/BIU_NPUSYS belong to
     * PD_NPUTOP.  Carries the VD_NPU power-off request (VOL_GATE_CON0[0]).
     */

    .name     = "npu",
    .pwr_mask = RK3576_PMU_PWR0_NPU,          /* SFTCON0[0]  */
    .mem_mask = RK3576_PD_NO_MEM,
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PMU_VOL0_NPU,          /* VOL_CON0[0] */
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_NPUTOP] =
  {
    .name     = "nputop",
    .pwr_mask = RK3576_PMU_PWR1_NPUTOP,       /* SFTCON1[6]     */
    .mem_mask = RK3576_PMU_MEM1_NPUTOP,       /* MEM_SFTCON1[6] */
    .idle0    = RK3576_PMU_IDLE0_NPUTOP |     /* IDLE0[3]       */
                RK3576_PMU_IDLE0_NPUSYS,      /* IDLE0[4]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NPU,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_NPU0] =
  {
    .name     = "npu0",
    .pwr_mask = RK3576_PMU_PWR1_NPU0,         /* SFTCON1[7]     */
    .mem_mask = RK3576_PMU_MEM1_NPU0,         /* MEM_SFTCON1[7] */
    .idle0    = RK3576_PMU_IDLE0_NPU0,        /* IDLE0[1]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NPUTOP,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_NPU1] =
  {
    .name     = "npu1",
    .pwr_mask = RK3576_PMU_PWR1_NPU1,         /* SFTCON1[8]     */
    .mem_mask = RK3576_PMU_MEM1_NPU1,         /* MEM_SFTCON1[8] */
    .idle0    = RK3576_PMU_IDLE0_NPU1,        /* IDLE0[2]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NPUTOP,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_GPU] =
  {
    /* VD_GPU: single domain, no memory retention bit, carries the VD_GPU
     * power-off request (VOL_GATE_CON1[9]).
     */

    .name     = "gpu",
    .pwr_mask = RK3576_PMU_PWR1_GPU,          /* SFTCON1[9]  */
    .mem_mask = RK3576_PD_NO_MEM,
    .idle0    = RK3576_PMU_IDLE0_GPU,         /* IDLE0[0]    */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PMU_VOL1_GPU,          /* VOL_CON1[9] */
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = true,
    .vol_reg1 = true,
  },

  [RK3576_PD_NVM] =
  {
    .name     = "nvm",
    .pwr_mask = RK3576_PMU_PWR0_NVM,          /* SFTCON0[6]     */
    .mem_mask = RK3576_PMU_MEM0_NVM,          /* MEM_SFTCON0[6] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PMU_IDLE1_NVM,         /* IDLE1[2]       */
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_SDGMAC] =
  {
    .name     = "sdgmac",
    .pwr_mask = RK3576_PMU_PWR0_SDGMAC,       /* SFTCON0[7]     */
    .mem_mask = RK3576_PMU_MEM0_SDGMAC,       /* MEM_SFTCON0[7] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PMU_IDLE1_GMAC,        /* IDLE1[1]       */
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NVM,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_USB] =
  {
    /* The vendor device tree nests PD_USB below PD_VOP; keep that parent
     * relation so the handshake order matches the reference software.
     */

    .name     = "usb",
    .pwr_mask = RK3576_PMU_PWR1_USB,          /* SFTCON1[0]     */
    .mem_mask = RK3576_PMU_MEM1_USB,          /* MEM_SFTCON1[0] */
    .idle0    = RK3576_PMU_IDLE0_USB,         /* IDLE0[10]      */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VOP,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_PHP] =
  {
    .name     = "php",
    .pwr_mask = RK3576_PMU_PWR0_PHP,          /* SFTCON0[9]     */
    .mem_mask = RK3576_PMU_MEM0_PHP,          /* MEM_SFTCON0[9] */
    .idle0    = RK3576_PMU_IDLE0_PHP,         /* IDLE0[15]      */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_SUBPHP] =
  {
    /* PCIE1/SATA0/1 share BIU_PHP with the parent domain, so there is no
     * separate idle request for PD_SUBPHP.
     */

    .name     = "subphp",
    .pwr_mask = RK3576_PMU_PWR0_SUBPHP,       /* SFTCON0[10]     */
    .mem_mask = RK3576_PMU_MEM0_SUBPHP,       /* MEM_SFTCON0[10] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_PHP,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_AUDIO] =
  {
    .name     = "audio",
    .pwr_mask = RK3576_PMU_PWR0_AUDIO,        /* SFTCON0[8]     */
    .mem_mask = RK3576_PMU_MEM0_AUDIO,        /* MEM_SFTCON0[8] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PMU_IDLE1_AUDIO,       /* IDLE1[0]       */
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_VEPU0] =
  {
    .name     = "vepu0",
    .pwr_mask = RK3576_PMU_PWR1_VEPU0,        /* SFTCON1[2]     */
    .mem_mask = RK3576_PMU_MEM1_VEPU0,        /* MEM_SFTCON1[2] */
    .idle0    = RK3576_PMU_IDLE0_VEPU0,       /* IDLE0[7]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VI,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_VEPU1] =
  {
    .name     = "vepu1",
    .pwr_mask = RK3576_PMU_PWR1_VEPU1,        /* SFTCON1[3]     */
    .mem_mask = RK3576_PMU_MEM1_VEPU1,        /* MEM_SFTCON1[3] */
    .idle0    = RK3576_PMU_IDLE0_VEPU1,       /* IDLE0[8]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_VPU] =
  {
    .name     = "vpu",
    .pwr_mask = RK3576_PMU_PWR1_VPU,          /* SFTCON1[5]     */
    .mem_mask = RK3576_PMU_MEM1_VPU,          /* MEM_SFTCON1[5] */
    .idle0    = RK3576_PMU_IDLE0_VPU,         /* IDLE0[5]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_VDEC] =
  {
    .name     = "vdec",
    .pwr_mask = RK3576_PMU_PWR1_VDEC,         /* SFTCON1[4]     */
    .mem_mask = RK3576_PMU_MEM1_VDEC,         /* MEM_SFTCON1[4] */
    .idle0    = RK3576_PMU_IDLE0_VDEC,        /* IDLE0[6]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_VI] =
  {
    .name     = "vi",
    .pwr_mask = RK3576_PMU_PWR1_VI,           /* SFTCON1[1]     */
    .mem_mask = RK3576_PMU_MEM1_VI,           /* MEM_SFTCON1[1] */
    .idle0    = RK3576_PMU_IDLE0_VI,          /* IDLE0[9]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = true,
    .vol_reg1 = false,
  },

  [RK3576_PD_VO0] =
  {
    .name     = "vo0",
    .pwr_mask = RK3576_PMU_PWR0_VO0,          /* SFTCON0[15]     */
    .mem_mask = RK3576_PMU_MEM0_VO0,          /* MEM_SFTCON0[15] */
    .idle0    = RK3576_PMU_IDLE0_VO0,         /* IDLE0[11]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VOP,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_VO1] =
  {
    .name     = "vo1",
    .pwr_mask = RK3576_PMU_PWR0_VO1,          /* SFTCON0[14]     */
    .mem_mask = RK3576_PMU_MEM0_VO1,          /* MEM_SFTCON0[14] */
    .idle0    = RK3576_PMU_IDLE0_VO1,         /* IDLE0[12]       */
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VOP,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_VOP] =
  {
    /* PD_VOP owns three bus interfaces: BIU_VOP, BIU_VOP_DDRSCH and the
     * BIU_VO0VOP_CHANNEL bridge towards PD_VO0 (TRM table 6-1).
     */

    .name     = "vop",
    .pwr_mask = RK3576_PMU_PWR0_VOP,          /* SFTCON0[11]     */
    .mem_mask = RK3576_PMU_MEM0_VOP,          /* MEM_SFTCON0[11] */
    .idle0    = RK3576_PMU_IDLE0_VOP |        /* IDLE0[13]       */
                RK3576_PMU_IDLE0_VOP_DDRSCH,  /* IDLE0[14]       */
    .idle1    = RK3576_PMU_IDLE1_VO0VOP_CHANNEL, /* IDLE1[11]    */
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_NO_PARENT,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_VOP_ESMART] =
  {
    .name     = "vop_esmart",
    .pwr_mask = RK3576_PMU_PWR0_VOP_ESMART,   /* SFTCON0[12]     */
    .mem_mask = RK3576_PMU_MEM0_VOP_ESMART,   /* MEM_SFTCON0[12] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VOP,
    .reg1     = false,
    .vol_reg1 = false,
  },

  [RK3576_PD_VOP_CLUSTER] =
  {
    .name     = "vop_cluster",
    .pwr_mask = RK3576_PMU_PWR0_VOP_CLUSTER,  /* SFTCON0[13]     */
    .mem_mask = RK3576_PMU_MEM0_VOP_CLUSTER,  /* MEM_SFTCON0[13] */
    .idle0    = RK3576_PD_NO_IDLE,
    .idle1    = RK3576_PD_NO_IDLE,
    .vol_mask = RK3576_PD_NO_VOL,
    .parent   = RK3576_PD_VOP,
    .reg1     = false,
    .vol_reg1 = false,
  },
};

static spinlock_t g_rk3576_pd_lock = SP_UNLOCKED;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pd_hiword
 *
 * Description:
 *   Write a HIWORD masked PMU register: only the bits in "mask" are
 *   modified, they are set to 1 when "set" is true and to 0 otherwise.
 *
 ****************************************************************************/

static void rk3576_pd_hiword(uint32_t offset, uint32_t mask, bool set)
{
  if (mask != 0)
    {
      putreg32(RK3576_PMU_HIWORD(mask, set ? mask : 0),
               RK3576_PMU_REG(offset));
    }
}

/****************************************************************************
 * Name: rk3576_pd_pwr_down
 *
 * Description:
 *   Read PMU_PWR_GATE_STS and tell whether the domain is currently gated.
 *
 ****************************************************************************/

static bool rk3576_pd_pwr_down(const struct rk3576_pd_info_s *info)
{
  uint32_t mask = info->reg1 ? info->pwr_mask << RK3576_PMU_PWR_STS_CON1_SHIFT
                             : info->pwr_mask;

  return (getreg32(RK3576_PMU_REG(RK3576_PMU_PWR_GATE_STS)) & mask) != 0;
}

/****************************************************************************
 * Name: rk3576_pd_wait_pwr
 *
 * Description:
 *   Poll PMU_PWR_GATE_STS until the domain reached the requested state.
 *
 * Input Parameters:
 *   info - Domain description.
 *   down - true to wait for "powered down", false for "powered up".
 *
 * Returned Value:
 *   OK, or -ETIMEDOUT.
 *
 ****************************************************************************/

static int rk3576_pd_wait_pwr(const struct rk3576_pd_info_s *info, bool down)
{
  int elapsed;

  for (elapsed = 0; elapsed < RK3576_PD_TIMEOUT_US;
       elapsed += RK3576_PD_POLL_US)
    {
      if (rk3576_pd_pwr_down(info) == down)
        {
          return OK;
        }

      up_udelay(RK3576_PD_POLL_US);
    }

  pwrerr("ERROR: pd %s power %s timeout, sts=0x%08" PRIx32 "\n", info->name,
         down ? "down" : "up",
         getreg32(RK3576_PMU_REG(RK3576_PMU_PWR_GATE_STS)));

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_pd_set_idle
 *
 * Description:
 *   Assert (idle == true) or release the software bus idle request of
 *   every BIU that belongs to the domain.
 *
 ****************************************************************************/

static void rk3576_pd_set_idle(const struct rk3576_pd_info_s *info, bool idle)
{
  rk3576_pd_hiword(RK3576_PMU_BIU_IDLE_SFTCON0, info->idle0, idle);
  rk3576_pd_hiword(RK3576_PMU_BIU_IDLE_SFTCON1, info->idle1, idle);
}

/****************************************************************************
 * Name: rk3576_pd_wait_idle
 *
 * Description:
 *   Poll PMU_BIU_IDLE_ACK_STS and PMU_BIU_IDLE_STS until every BIU of the
 *   domain reports the requested state.  Both registers flatten the two
 *   request registers: SFTCON1 bit n appears at bit (16 + n).
 *
 * Input Parameters:
 *   info - Domain description.
 *   idle - true to wait for "idle", false to wait for "active".
 *
 * Returned Value:
 *   OK, or -ETIMEDOUT.
 *
 ****************************************************************************/

static int rk3576_pd_wait_idle(const struct rk3576_pd_info_s *info, bool idle)
{
  uint32_t mask;
  uint32_t want;
  uint32_t ack;
  uint32_t sts;
  int elapsed;

  mask = info->idle0 | (info->idle1 << RK3576_PMU_IDLE_STS_CON1_SHIFT);
  if (mask == 0)
    {
      return OK;
    }

  want = idle ? mask : 0;

  for (elapsed = 0; elapsed < RK3576_PD_TIMEOUT_US;
       elapsed += RK3576_PD_POLL_US)
    {
      ack = getreg32(RK3576_PMU_REG(RK3576_PMU_BIU_IDLE_ACK_STS)) & mask;
      sts = getreg32(RK3576_PMU_REG(RK3576_PMU_BIU_IDLE_STS)) & mask;

      if (ack == want && sts == want)
        {
          return OK;
        }

      up_udelay(RK3576_PD_POLL_US);
    }

  pwrerr("ERROR: pd %s biu %s timeout, ack=0x%08" PRIx32 " sts=0x%08" PRIx32
         " mask=0x%08" PRIx32 "\n",
         info->name, idle ? "idle" : "active", ack, sts, mask);

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_pd_set_mem_down
 *
 * Description:
 *   Enable (down == true) or release the SRAM retention request of the
 *   domain.  Domains without an SRAM block (VD_NPU, VD_GPU) are skipped.
 *
 ****************************************************************************/

static void rk3576_pd_set_mem_down(const struct rk3576_pd_info_s *info,
                                   bool down)
{
  uint32_t offset = info->reg1 ? RK3576_PMU_MEM_PWR_GATE_SFTCON1
                               : RK3576_PMU_MEM_PWR_GATE_SFTCON0;

  rk3576_pd_hiword(offset, info->mem_mask, down);
}

/****************************************************************************
 * Name: rk3576_pd_set_vol_off
 *
 * Description:
 *   Assert or release the voltage domain power-off request.  Only VD_NPU
 *   and VD_GPU have one (TRM sections 6.5.7.2 and 6.5.7.3).  The request
 *   drives an external PMIC rail, so it must be released before the power
 *   gate of the domain is opened and asserted only after the gate closed.
 *
 ****************************************************************************/

static void rk3576_pd_set_vol_off(const struct rk3576_pd_info_s *info,
                                  bool off)
{
  uint32_t offset =
      info->vol_reg1 ? RK3576_PMU_VOL_GATE_CON1 : RK3576_PMU_VOL_GATE_CON0;

  rk3576_pd_hiword(offset, info->vol_mask, off);
}

/****************************************************************************
 * Name: rk3576_pd_do_on
 *
 * Description:
 *   Power one domain up, after having powered up its parent.  Called with
 *   the driver spinlock held by rk3576_pd_on().
 *
 ****************************************************************************/

static int rk3576_pd_do_on(int domain)
{
  const struct rk3576_pd_info_s *info = &g_rk3576_pd_info[domain];
  int ret;

  if (info->parent != RK3576_PD_NO_PARENT)
    {
      ret = rk3576_pd_do_on(info->parent);
      if (ret < 0)
        {
          return ret;
        }
    }

  if (!rk3576_pd_pwr_down(info))
    {
      /* Already up.  Bootloaders leave several domains powered, so this is
       * the common case for USB, NVM/SDGMAC and VOP.
       */

      return OK;
    }

  pwrinfo("pd %s: power up\n", info->name);

  /* 1. Withdraw the voltage domain power-off request. */

  rk3576_pd_set_vol_off(info, false);

  /* 2. Open the power gate and wait for the power acknowledge. */

  rk3576_pd_hiword(info->reg1 ? RK3576_PMU_PWR_GATE_SFTCON1
                              : RK3576_PMU_PWR_GATE_SFTCON0,
                   info->pwr_mask, false);

  ret = rk3576_pd_wait_pwr(info, false);
  if (ret < 0)
    {
      return ret;
    }

  /* 3. Leave SRAM retention. */

  rk3576_pd_set_mem_down(info, false);

  /* 4. Release the bus idle request and wait for the BIUs to go active. */

  rk3576_pd_set_idle(info, false);

  return rk3576_pd_wait_idle(info, false);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_pd_on
 *
 * Description:
 *   Power a domain up.  See rk3576_pd.h for the full contract.
 *
 ****************************************************************************/

int rk3576_pd_on(int domain)
{
  irqstate_t flags;
  int ret;

  if (domain < 0 || domain >= RK3576_PD_NDOMAINS ||
      g_rk3576_pd_info[domain].name == NULL)
    {
      return -EINVAL;
    }

  flags = spin_lock_irqsave(&g_rk3576_pd_lock);
  ret = rk3576_pd_do_on(domain);
  spin_unlock_irqrestore(&g_rk3576_pd_lock, flags);

  return ret;
}

/****************************************************************************
 * Name: rk3576_pd_off
 *
 * Description:
 *   Power a domain down.  See rk3576_pd.h for the full contract.
 *
 ****************************************************************************/

int rk3576_pd_off(int domain)
{
  const struct rk3576_pd_info_s *info;
  irqstate_t flags;
  int ret;

  if (domain < 0 || domain >= RK3576_PD_NDOMAINS ||
      g_rk3576_pd_info[domain].name == NULL)
    {
      return -EINVAL;
    }

  info = &g_rk3576_pd_info[domain];
  flags = spin_lock_irqsave(&g_rk3576_pd_lock);

  if (rk3576_pd_pwr_down(info))
    {
      /* Already down. */

      spin_unlock_irqrestore(&g_rk3576_pd_lock, flags);
      return OK;
    }

  pwrinfo("pd %s: power down\n", info->name);

  /* 1. Ask the bus interfaces of the domain to go idle. */

  rk3576_pd_set_idle(info, true);

  ret = rk3576_pd_wait_idle(info, true);
  if (ret < 0)
    {
      /* Undo the idle request so the domain keeps working. */

      rk3576_pd_set_idle(info, false);
      spin_unlock_irqrestore(&g_rk3576_pd_lock, flags);
      return ret;
    }

  /* 2. Put the SRAM into retention. */

  rk3576_pd_set_mem_down(info, true);

  /* 3. Close the power gate and wait for the status. */

  rk3576_pd_hiword(info->reg1 ? RK3576_PMU_PWR_GATE_SFTCON1
                              : RK3576_PMU_PWR_GATE_SFTCON0,
                   info->pwr_mask, true);

  ret = rk3576_pd_wait_pwr(info, true);
  if (ret >= 0)
    {
      /* 4. Finally raise the voltage domain power-off request, if any. */

      rk3576_pd_set_vol_off(info, true);
    }

  spin_unlock_irqrestore(&g_rk3576_pd_lock, flags);
  return ret;
}

/****************************************************************************
 * Name: rk3576_pd_is_on
 *
 * Description:
 *   Read the current power state of a domain.
 *
 ****************************************************************************/

bool rk3576_pd_is_on(int domain)
{
  if (domain < 0 || domain >= RK3576_PD_NDOMAINS ||
      g_rk3576_pd_info[domain].name == NULL)
    {
      return false;
    }

  return !rk3576_pd_pwr_down(&g_rk3576_pd_info[domain]);
}

/****************************************************************************
 * Name: rk3576_pd_name
 *
 * Description:
 *   Return the human readable name of a domain.
 *
 ****************************************************************************/

const char *rk3576_pd_name(int domain)
{
  if (domain < 0 || domain >= RK3576_PD_NDOMAINS ||
      g_rk3576_pd_info[domain].name == NULL)
    {
      return "unknown";
    }

  return g_rk3576_pd_info[domain].name;
}

#endif /* CONFIG_RK3576_PD */
