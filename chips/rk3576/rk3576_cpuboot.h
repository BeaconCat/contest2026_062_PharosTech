/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_cpuboot.h
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

#ifndef __ARCH_ARM64_SRC_RK3576_RK3576_CPUBOOT_H
#define __ARCH_ARM64_SRC_RK3576_RK3576_CPUBOOT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RK3576 CPU topology (source: vendor device tree, /cpus node).
 *
 *   cluster 0 (MPIDR Aff1 = 0): cpu@0   .. cpu@3    Cortex-A53 (LITTLE)
 *   cluster 1 (MPIDR Aff1 = 1): cpu@100 .. cpu@103  Cortex-A72 (big)
 *
 * All eight cores declare enable-method = "psci", and the boot core is
 * cluster 0 / core 0 (MPIDR Aff1:Aff0 = 0:0).
 */

#define RK3576_CPU_NCLUSTERS   2
#define RK3576_CPU_PER_CLUSTER 4
#define RK3576_CPU_NCORES      (RK3576_CPU_NCLUSTERS * RK3576_CPU_PER_CLUSTER)

#define RK3576_CLUSTER_LITTLE  0 /* Cortex-A53 */
#define RK3576_CLUSTER_BIG     1 /* Cortex-A72 */

/* MPIDR_EL1 affinity field layout (ARMv8-A, Aff2/Aff3 are zero on RK3576) */

#define RK3576_MPIDR_AFF0_SHIFT 0
#define RK3576_MPIDR_AFF1_SHIFT 8
#define RK3576_MPIDR_AFF_MASK   0xffull

/* Mask selecting the two affinity fields the RK3576 actually uses, so that
 * the U/MT/RES1 flag bits of a raw MPIDR_EL1 read can be discarded.
 */

#define RK3576_MPIDR_AFF01_MASK 0xffffull

/* Build an MPIDR affinity value (the PSCI "target_cpu" argument) from a
 * cluster number and the core index inside that cluster.
 */

#define RK3576_MPID(cluster, core)                                            \
  ((((uint64_t)(cluster)&RK3576_MPIDR_AFF_MASK) << RK3576_MPIDR_AFF1_SHIFT) | \
   (((uint64_t)(core)&RK3576_MPIDR_AFF_MASK) << RK3576_MPIDR_AFF0_SHIFT))

/* Extract the affinity fields out of an MPIDR value */

#define RK3576_MPID_CLUSTER(mpid) \
  (((mpid) >> RK3576_MPIDR_AFF1_SHIFT) & RK3576_MPIDR_AFF_MASK)
#define RK3576_MPID_CORE(mpid) \
  (((mpid) >> RK3576_MPIDR_AFF0_SHIFT) & RK3576_MPIDR_AFF_MASK)

/* Logical CPU index <-> MPIDR affinity.  The logical numbering is
 * cluster-major so that CPU 0 is the boot core:
 *
 *   logical 0..3 -> A53 cluster, logical 4..7 -> A72 cluster
 *   logical = (Aff1 * 4) + Aff0
 *
 * The same arithmetic is implemented by the get_cpu_id assembly macro in
 * arch/arm64/include/rk3576/chip.h, which the secondary cores use before
 * any C code runs.  Both must stay in sync.
 */

#define RK3576_CPU_TO_MPID(cpu) \
  RK3576_MPID((cpu) / RK3576_CPU_PER_CLUSTER, (cpu) % RK3576_CPU_PER_CLUSTER)
#define RK3576_MPID_TO_CPU(mpid)                                \
  ((int)((RK3576_MPID_CLUSTER(mpid) * RK3576_CPU_PER_CLUSTER) + \
         RK3576_MPID_CORE(mpid)))

#ifdef CONFIG_SMP

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C" {
#else
#define EXTERN extern
#endif

/* Logical CPU index -> MPIDR affinity value, one entry per configured CPU */

EXTERN const uint64_t g_rk3576_cpu_mpid[CONFIG_SMP_NCPUS];

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_cpu_start
 *
 * Description:
 *   Release a secondary core through the PSCI CPU_ON service and let it
 *   enter the NuttX reset vector.  up_cpu_start() in the arm64 common code
 *   performs the very same sequence during SMP bring-up; this helper exists
 *   for board level diagnostics and for out-of-scheduler use cases such as
 *   handing a core over to an AMP payload.
 *
 * Input Parameters:
 *   cpu   - Logical CPU index, 1..(CONFIG_SMP_NCPUS - 1)
 *   entry - Physical entry point the core jumps to, or 0 to use the NuttX
 *           reset vector
 *
 * Returned Value:
 *   Zero on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rk3576_cpu_start(int cpu, uintptr_t entry);

/****************************************************************************
 * Name: rk3576_cpu_is_on
 *
 * Description:
 *   Query the power state of a core through the PSCI AFFINITY_INFO service.
 *
 * Input Parameters:
 *   cpu - Logical CPU index, 0..(CONFIG_SMP_NCPUS - 1)
 *
 * Returned Value:
 *   True when the firmware reports the core as ON.
 *
 ****************************************************************************/

bool rk3576_cpu_is_on(int cpu);

/****************************************************************************
 * Name: rk3576_cpu_corename
 *
 * Description:
 *   Human readable core type of a logical CPU index, for boot banners.
 *
 ****************************************************************************/

const char *rk3576_cpu_corename(int cpu);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_SMP */
#endif /* __ARCH_ARM64_SRC_RK3576_RK3576_CPUBOOT_H */
