/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_cpuboot.c
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
 * RK3576 SMP secondary core bring-up.
 *
 * The RK3576 is a big.LITTLE part with two clusters:
 *
 *   cluster 0 (MPIDR Aff1 = 0): 4 x Cortex-A53  (LITTLE)
 *   cluster 1 (MPIDR Aff1 = 1): 4 x Cortex-A72  (big)
 *
 * The boot core is cluster 0 core 0.  ARM Trusted Firmware (BL31, PSCI
 * 1.x) owns power control of every core, so the only supported way to
 * release a secondary core is the PSCI CPU_ON service issued with SMC.
 * BL31 also performs the per-core work that cannot be done from a
 * non-secure EL: it powers the cluster rail and L2 up, programs
 * CPUECTLR_EL1.SMPEN so the core joins the inner-shareable coherency
 * domain through the CCI, and only then branches to the entry point
 * passed in the CPU_ON call.  Therefore this file does not touch any
 * CRU/PMU register and does not have to poke the SCU/CCI itself.
 *
 * The entry point handed to BL31 is the normal NuttX reset vector
 * (__start).  arm64_head.S dispatches on the get_cpu_id assembly macro
 * (see include/chip.h) and routes non-zero CPUs into
 * arm64_boot_secondary_c_routine(), which sets up the per-CPU stack,
 * MMU, caches and the GICv2 CPU interface (arm64_gic_secondary_init())
 * before entering the scheduler.  The logical numbering used by that
 * macro, by g_rk3576_cpu_mpid[] and by arm64_get_mpid() below must all
 * agree:
 *
 *   logical CPU = (MPIDR.Aff1 * 4) + MPIDR.Aff0
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

#include "arm64_arch.h"
#include "arm64_internal.h"
#include "rk3576_cpuboot.h"

#ifdef CONFIG_SMP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* PSCI function identifiers (ARM DEN 0022D).  The 64-bit variants are used
 * because NuttX runs the RK3576 in AArch64 state.
 */

#define RK3576_PSCI_VERSION       0x84000000
#define RK3576_PSCI_CPU_ON        0xc4000003
#define RK3576_PSCI_AFFINITY_INFO 0xc4000004

/* PSCI return codes */

#define RK3576_PSCI_SUCCESS          0
#define RK3576_PSCI_NOT_SUPPORTED    (-1)
#define RK3576_PSCI_INVALID_PARAMS   (-2)
#define RK3576_PSCI_DENIED           (-3)
#define RK3576_PSCI_ALREADY_ON       (-4)
#define RK3576_PSCI_ON_PENDING       (-5)
#define RK3576_PSCI_INTERNAL_FAILURE (-6)

/* AFFINITY_INFO states */

#define RK3576_PSCI_AFFINITY_ON      0
#define RK3576_PSCI_AFFINITY_OFF     1
#define RK3576_PSCI_AFFINITY_PENDING 2

/* Lowest affinity level, i.e. "the target is a single core" */

#define RK3576_PSCI_AFFINITY_LEVEL0 0

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int64_t rk3576_cpu_smc(uint64_t func, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2);
static int rk3576_cpu_psci_errno(int64_t status);

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* Logical CPU index -> MPIDR affinity value.  Cluster major ordering keeps
 * the boot core at index 0 and mirrors the get_cpu_id assembly macro.
 */

const uint64_t g_rk3576_cpu_mpid[CONFIG_SMP_NCPUS] = {
  RK3576_MPID(RK3576_CLUSTER_LITTLE, 0), /* CPU0 A53 (boot core) */
#if CONFIG_SMP_NCPUS > 1
  RK3576_MPID(RK3576_CLUSTER_LITTLE, 1), /* CPU1 A53 */
#endif
#if CONFIG_SMP_NCPUS > 2
  RK3576_MPID(RK3576_CLUSTER_LITTLE, 2), /* CPU2 A53 */
#endif
#if CONFIG_SMP_NCPUS > 3
  RK3576_MPID(RK3576_CLUSTER_LITTLE, 3), /* CPU3 A53 */
#endif
#if CONFIG_SMP_NCPUS > 4
  RK3576_MPID(RK3576_CLUSTER_BIG, 0), /* CPU4 A72 */
#endif
#if CONFIG_SMP_NCPUS > 5
  RK3576_MPID(RK3576_CLUSTER_BIG, 1), /* CPU5 A72 */
#endif
#if CONFIG_SMP_NCPUS > 6
  RK3576_MPID(RK3576_CLUSTER_BIG, 2), /* CPU6 A72 */
#endif
#if CONFIG_SMP_NCPUS > 7
  RK3576_MPID(RK3576_CLUSTER_BIG, 3), /* CPU7 A72 */
#endif
};

#if CONFIG_SMP_NCPUS > RK3576_CPU_NCORES
#error "CONFIG_SMP_NCPUS exceeds the eight cores of the RK3576"
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_cpu_smc
 *
 * Description:
 *   Issue an SMC64 call to the secure monitor (BL31) following the SMC
 *   calling convention: the function identifier goes in x0, up to three
 *   arguments in x1..x3, and the status is returned in x0.
 *
 ****************************************************************************/

static int64_t rk3576_cpu_smc(uint64_t func, uint64_t arg0, uint64_t arg1,
                              uint64_t arg2)
{
  register uint64_t x0 asm("x0") = func;
  register uint64_t x1 asm("x1") = arg0;
  register uint64_t x2 asm("x2") = arg1;
  register uint64_t x3 asm("x3") = arg2;

  asm volatile("smc #0"
               : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
               :
               : "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                 "x12", "x13", "x14", "x15", "x16", "x17");

  return (int64_t)x0;
}

/****************************************************************************
 * Name: rk3576_cpu_psci_errno
 *
 * Description:
 *   Translate a PSCI status code into a negated errno value.
 *
 ****************************************************************************/

static int rk3576_cpu_psci_errno(int64_t status)
{
  switch (status)
    {
      case RK3576_PSCI_SUCCESS:
        return OK;

      case RK3576_PSCI_NOT_SUPPORTED:
        return -ENOTSUP;

      case RK3576_PSCI_INVALID_PARAMS:
        return -EINVAL;

      case RK3576_PSCI_DENIED:
        return -EPERM;

      case RK3576_PSCI_ALREADY_ON:
        return -EALREADY;

      case RK3576_PSCI_ON_PENDING:
        return -EINPROGRESS;

      case RK3576_PSCI_INTERNAL_FAILURE:
      default:
        return -EIO;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CONFIG_RK3576_SMP_MPID_MAP

/****************************************************************************
 * Name: arm64_get_mpid
 *
 * Description:
 *   Logical CPU index -> MPIDR affinity value.  The arm64 common code
 *   provides a default that assumes a single cluster (Aff0 == cpu index),
 *   which is wrong for the two-cluster RK3576; this definition overrides
 *   it.  Disable CONFIG_RK3576_SMP_MPID_MAP should the common default
 *   ever stop being overridable.
 *
 ****************************************************************************/

uint64_t arm64_get_mpid(int cpu)
{
  DEBUGASSERT((unsigned int)cpu < CONFIG_SMP_NCPUS);
  return g_rk3576_cpu_mpid[cpu];
}

/****************************************************************************
 * Name: arm64_get_cpuid
 *
 * Description:
 *   MPIDR affinity value -> logical CPU index.  Inverse of
 *   arm64_get_mpid().  Returns -EINVAL when the affinity value does not
 *   belong to a configured core.
 *
 ****************************************************************************/

int arm64_get_cpuid(uint64_t mpid)
{
  int cpu;

  for (cpu = 0; cpu < CONFIG_SMP_NCPUS; cpu++)
    {
      if (g_rk3576_cpu_mpid[cpu] == (mpid & RK3576_MPIDR_AFF01_MASK))
        {
          return cpu;
        }
    }

  return -EINVAL;
}
#endif /* CONFIG_RK3576_SMP_MPID_MAP */

/****************************************************************************
 * Name: rk3576_cpu_start
 *
 * Description:
 *   Release a secondary core through PSCI CPU_ON.  See rk3576_cpuboot.h.
 *
 ****************************************************************************/

int rk3576_cpu_start(int cpu, uintptr_t entry)
{
  uintptr_t target = entry;
  int64_t status;
  int ret;

  if (cpu <= 0 || cpu >= CONFIG_SMP_NCPUS)
    {
      return -EINVAL;
    }

  if (target == 0)
    {
      /* Default to the NuttX reset vector.  NuttX runs flat mapped on this
       * chip, so the link time address is also the physical address BL31
       * needs.
       */

      target = (uintptr_t)__start;
    }

  /* The context id is handed back to the core in x0.  Pass the logical CPU
   * index; the reset vector does not consume it but it makes the call
   * traceable from a secure world log.
   */

  status = rk3576_cpu_smc(RK3576_PSCI_CPU_ON, g_rk3576_cpu_mpid[cpu],
                          (uint64_t)target, (uint64_t)cpu);
  ret = rk3576_cpu_psci_errno(status);

  if (ret < 0)
    {
      serr("ERROR: PSCI CPU_ON of CPU%d (mpid %llx) failed: %" PRId64 "\n",
           cpu, (unsigned long long)g_rk3576_cpu_mpid[cpu], status);
    }
  else
    {
      sinfo("CPU%d (%s, mpid %llx) started at %p\n", cpu,
            rk3576_cpu_corename(cpu),
            (unsigned long long)g_rk3576_cpu_mpid[cpu], (void *)target);
    }

  return ret;
}

/****************************************************************************
 * Name: rk3576_cpu_is_on
 *
 * Description:
 *   Query a core power state through PSCI AFFINITY_INFO.
 *
 ****************************************************************************/

bool rk3576_cpu_is_on(int cpu)
{
  int64_t status;

  if ((unsigned int)cpu >= CONFIG_SMP_NCPUS)
    {
      return false;
    }

  status = rk3576_cpu_smc(RK3576_PSCI_AFFINITY_INFO, g_rk3576_cpu_mpid[cpu],
                          RK3576_PSCI_AFFINITY_LEVEL0, 0);

  return status == RK3576_PSCI_AFFINITY_ON;
}

/****************************************************************************
 * Name: rk3576_cpu_corename
 *
 * Description:
 *   Human readable core type of a logical CPU index.
 *
 ****************************************************************************/

const char *rk3576_cpu_corename(int cpu)
{
  if ((unsigned int)cpu >= CONFIG_SMP_NCPUS)
    {
      return "invalid";
    }

  return RK3576_MPID_CLUSTER(g_rk3576_cpu_mpid[cpu]) == RK3576_CLUSTER_BIG
             ? "Cortex-A72"
             : "Cortex-A53";
}

#ifdef CONFIG_RK3576_SMP_CPUSTART

/****************************************************************************
 * Name: up_cpu_start
 *
 * Description:
 *   Start CPU 'cpu' and make it ready to take work from the scheduler.
 *   This is only built when the arm64 common arm64_cpustart.c is not part
 *   of the build (see CONFIG_RK3576_SMP_CPUSTART); otherwise the common
 *   implementation, which performs the same PSCI CPU_ON sequence through
 *   arm64_get_mpid() above, is used.
 *
 * Input Parameters:
 *   cpu - Logical CPU index, 1..(CONFIG_SMP_NCPUS - 1)
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/

int up_cpu_start(int cpu)
{
  /* Make every write performed by the boot core visible to a core that is
   * about to leave reset with its caches invalidated.
   */

  UP_DSB();
  UP_ISB();

  return rk3576_cpu_start(cpu, 0);
}
#endif /* CONFIG_RK3576_SMP_CPUSTART */

#endif /* CONFIG_SMP */
