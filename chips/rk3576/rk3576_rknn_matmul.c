/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_rknn_matmul.c
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
 * RK3576 RKNN typed quantised matrix multiply.
 *
 * C[M,N] = A[M,K] x B[K,N], with the operand and accumulator types listed
 * in rk3576_rknn_matmul.h.  This is the one operation an LLM runtime needs
 * from the NPU; the rest of a transformer stays on the Cortex-A cores.
 *
 * This module owns the *submission* layer only.  Register access, clocks,
 * power domains, interrupts and job serialisation belong to the RKNPU core
 * driver (rk3576_rknpu.c); the two entry points used here are declared weak
 * so that this file links and degrades to the CPU reference when the core
 * driver is not configured in.
 *
 * ---------------------------------------------------------------------
 * Tensor layouts
 * ---------------------------------------------------------------------
 *
 * The NPU does not read row-major matrices.  Operands are tiled so that the
 * MAC array can stream one tile per cycle.  The tiling used here is
 *
 *   A native : [Kal/subKa][M][subKa]          element (m, k) lives at
 *                                             ((k/subKa)*M + m)*subKa
 *                                             + (k%subKa)
 *   B native : [Nal/subNb][Kal][subNb]        element (k, n) lives at
 *                                             ((n/subNb)*Kal + k)*subNb
 *                                             + (n%subNb)
 *   C native : [Nal/subNc][M][subNc]          element (m, n) lives at
 *                                             ((n/subNc)*M + m)*subNc
 *                                             + (n%subNc)
 *
 * where Kal and Nal are K and N rounded up to the per-type alignment.  M is
 * the batch-like axis and is never padded.
 *
 * INFERRED, NOT MEASURED.  The shape of the tiling (which axis is tiled,
 * and the [tile][row][lane] ordering) follows the documented behaviour of
 * the public rknn_matmul_api on the RK35xx family.  The concrete sub-tile
 * sizes and alignments in g_rk3576_rknn_mm_traits below are inferences from
 * that family and MUST be confirmed on hardware before the NPU results can
 * be trusted.  Getting them wrong produces plausible-looking but numerically
 * wrong output, so the very first bring-up step is a CPU-versus-NPU diff on
 * a tiny problem - see the notes in rk3576_rknn_matmul.h.
 *
 * Packed int4 operands store two codes per byte, the even element index in
 * the low nibble.  The nibble pair is taken along the fastest-varying axis
 * of the source layout, which is N for NORM, K for TP_NORM and the sub-tile
 * lane for NATIVE.
 *
 * ---------------------------------------------------------------------
 * Register command stream
 * ---------------------------------------------------------------------
 *
 * A submission is a DMA-resident list of (register offset, value) pairs
 * that the NPU program counter replays.  The builder below emits the
 * skeleton for a GEMM expressed as a 1x1 convolution: A is the input
 * feature map, B is the weight kernel, the DPU writes C.  The RK3576
 * offsets are not published, so they live in a patchable map
 * (rk3576_rknn_matmul_set_regmap) that starts out entirely unknown.  A
 * captured golden stream can be replayed instead - see
 * rk3576_rknn_matmul_set_template().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/param.h>

#include <nuttx/cache.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>

#include "rk3576_addrenv.h"
#include "rk3576_dma_alloc.h"
#include "rk3576_rknn_matmul.h"

#ifdef CONFIG_RK3576_RKNN_MATMUL

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Capacity of the per-context register-command buffer, in 32-bit words.
 * The synthesised stream needs a couple of dozen words; the headroom is for
 * captured golden streams installed with set_template().
 */

#define RK3576_RKNN_MM_REGCMD_WORDS 1024

/* Default NPU job timeout.  A single tile of an LLM projection completes in
 * tens of microseconds; this is a hang detector, not a budget.
 */

#define RK3576_RKNN_MM_TIMEOUT_MS 1000

/* Only NPU core 0 by default.  RK3576 has two cores (npu0_irq, npu1_irq in
 * the vendor device tree); using both requires splitting the N range, which
 * the caller can do by creating two contexts.
 */

#define RK3576_RKNN_MM_CORE_DEFAULT  0x1
#define RK3576_RKNN_MM_CORE_MASK_ALL 0x3

/* Do not spawn a worker thread unless it gets at least this many output
 * columns; below that the thread creation dominates.
 */

#define RK3576_RKNN_MM_MIN_COLS 32

/* Stack for a CPU back-end worker.  The kernel is scalar and iterative, so
 * this only has to cover the frame plus the accessor calls.
 */

#define RK3576_RKNN_MM_WORKER_STACK 4096

/* Sanity limits on the problem shape.  K is bounded by the NPU input buffer
 * and by the fact that an fp32 accumulator loses resolution long before
 * this; M and N are bounded to keep the staging allocations sane.
 */

#define RK3576_RKNN_MM_MAX_M 65536
#define RK3576_RKNN_MM_MAX_K 65536
#define RK3576_RKNN_MM_MAX_N 65536

/* Element type selectors written into the CNA/DPU type registers.
 *
 * TODO: encoding inferred from the RK35xx convolution pipeline; confirm
 * against a captured golden stream before trusting the NPU path.
 */

#define RK3576_RKNN_MM_DTYPE_INT4  0
#define RK3576_RKNN_MM_DTYPE_INT8  1
#define RK3576_RKNN_MM_DTYPE_FP16  2
#define RK3576_RKNN_MM_DTYPE_INT32 3
#define RK3576_RKNN_MM_DTYPE_FP32  4

/* Static values for the pipeline-mode registers: 1x1 kernel, stride 1, no
 * padding, single group, accumulate-and-store.
 *
 * TODO: placeholder bit patterns; replace with measured values together
 * with the register offsets.
 */

#define RK3576_RKNN_MM_CONV_CON1_1X1 0x00000000
#define RK3576_RKNN_MM_DPU_MODE_GEMM 0x00000000
#define RK3576_RKNN_MM_MAC_GATE_ALL  0xffffffff

/* Two-dimensional extent encoding used by the CNA/DPU size registers:
 * width in the low half, height in the high half, both stored biased by one.
 *
 * TODO: inferred from the RK35xx convolution register layout.
 */

#define RK3576_RKNN_MM_SIZE_ENC(w, h) \
  ((((uint32_t)(h)-1u) << 16) | (((uint32_t)(w)-1u) & 0xffffu))

/* Integer helpers. */

#define RK3576_RKNN_MM_DIV_UP(a, b)   (((a) + (b)-1u) / (b))
#define RK3576_RKNN_MM_ALIGN_UP(a, b) (RK3576_RKNN_MM_DIV_UP(a, b) * (b))

/* The NPU address registers are 32 bits wide. */

#define RK3576_RKNN_MM_ADDR_LIMIT 0x100000000ull

/* Native fp16 is available on every aarch64 tool chain we build with; the
 * software path exists so the conversions stay testable elsewhere and so
 * the file is not silently wrong if it is ever built for a host tool.
 */

#if defined(__ARM_FP16_FORMAT_IEEE)
#define RK3576_RKNN_MM_NATIVE_FP16 1
#endif

#ifdef CONFIG_RK3576_RKNN_MATMUL_SELFTEST

/* Self-test problem: one decode-step projection tile, already aligned so
 * that no padding is exercised on the first run.  K is two quantisation
 * groups of 32, which is the shape RKLLM-style w4a16 weights use.
 */

#define RK3576_RKNN_MM_SELFTEST_M     1
#define RK3576_RKNN_MM_SELFTEST_K     64
#define RK3576_RKNN_MM_SELFTEST_N     32
#define RK3576_RKNN_MM_SELFTEST_GROUP 32

/* Tolerance against the double-precision reference.  The kernels round
 * every product through fp32 and the operands are fp16, so a few ulp of
 * fp32 over a 64-term reduction is expected; anything larger is a bug, not
 * rounding.
 */

#define RK3576_RKNN_MM_SELFTEST_EPS 1.0e-3

#endif /* CONFIG_RK3576_RKNN_MATMUL_SELFTEST */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Per-matmul-type constants: element widths, tile geometry and alignment. */

struct rk3576_rknn_mm_traits_s
{
  int type;         /* enum rk3576_rknn_matmul_type_e                   */
  uint8_t a_bits;   /* Bits per A element                               */
  uint8_t b_bits;   /* Bits per B element                               */
  uint8_t c_bits;   /* Bits per C element                               */
  uint8_t subk_a;   /* A native K tile                                  */
  uint8_t subn_b;   /* B native N lane count                            */
  uint8_t subn_c;   /* C native N lane count                            */
  uint16_t k_align; /* Required padding of K, in elements               */
  uint16_t n_align; /* Required padding of N, in elements               */
  bool c_float;     /* C holds float rather than integer                */
  bool b_int;       /* B is an integer type and may carry quant params  */
};

/* Per-problem context.  Opaque to callers. */

struct rk3576_rknn_matmul_ctx_s
{
  struct rk3576_rknn_matmul_info_s info;
  const struct rk3576_rknn_mm_traits_s *tr;

  uint32_t kal; /* K padded to tr->k_align                   */
  uint32_t nal; /* N padded to tr->n_align                   */

  const void *a_src; /* Caller operands, not owned                */
  const void *b_src;
  void *c_dst;

  struct rk3576_rknn_matmul_quant_s quant;

  void *a_dma; /* Packed staging buffers, DMA-safe          */
  void *b_dma;
  void *c_dma;
  uint32_t *regcmd;
  size_t a_size;
  size_t b_size;
  size_t c_size;
  size_t regcmd_size;

  const struct rk3576_rknn_matmul_template_s *tpl;

  uint32_t core_mask;
  uint32_t timeout_ms;
  int backend;
  bool b_dirty;    /* B changed since the last pack             */
  bool npu_warned; /* AUTO fallback already reported            */

  mutex_t lock;
};

/* One slice of the output columns handed to a CPU worker thread. */

struct rk3576_rknn_mm_work_s
{
  struct rk3576_rknn_matmul_ctx_s *ctx;
  uint32_t n0;
  uint32_t n1;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

#ifndef RK3576_RKNN_MM_NATIVE_FP16
static float rk3576_rknn_mm_fp16_to_fp32_sw(uint16_t h);
static uint16_t rk3576_rknn_mm_fp32_to_fp16_sw(float f);
#endif
static const struct rk3576_rknn_mm_traits_s *rk3576_rknn_mm_traits(int type);
static size_t
rk3576_rknn_mm_a_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                       uint32_t k);
static size_t
rk3576_rknn_mm_b_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t k,
                       uint32_t n);
static size_t
rk3576_rknn_mm_c_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                       uint32_t n);
static uint32_t
rk3576_rknn_mm_a_raw(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                     uint32_t k);
static uint32_t
rk3576_rknn_mm_b_raw(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t k,
                     uint32_t n);
static int32_t rk3576_rknn_mm_sext(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                   uint32_t raw);
static float rk3576_rknn_mm_a_float(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                    uint32_t m, uint32_t k);
static void
rk3576_rknn_mm_c_store_f(const struct rk3576_rknn_matmul_ctx_s *ctx,
                         uint32_t m, uint32_t n, float v);
static void
rk3576_rknn_mm_c_store_i(const struct rk3576_rknn_matmul_ctx_s *ctx,
                         uint32_t m, uint32_t n, int32_t v);
static void rk3576_rknn_mm_quant_at(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                    uint32_t group, uint32_t n, float *scale,
                                    int32_t *zp);
static void rk3576_rknn_mm_cpu_float(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t n0, uint32_t n1);
static void rk3576_rknn_mm_cpu_int(struct rk3576_rknn_matmul_ctx_s *ctx,
                                   uint32_t n0, uint32_t n1);
static void rk3576_rknn_mm_cpu_range(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t n0, uint32_t n1);
static void *rk3576_rknn_mm_worker(void *arg);
static int rk3576_rknn_mm_cpu_exec(struct rk3576_rknn_matmul_ctx_s *ctx);
static void rk3576_rknn_mm_pack_a(struct rk3576_rknn_matmul_ctx_s *ctx);
static void rk3576_rknn_mm_pack_b(struct rk3576_rknn_matmul_ctx_s *ctx);
static void rk3576_rknn_mm_unpack_c(struct rk3576_rknn_matmul_ctx_s *ctx);
static int rk3576_rknn_mm_build_regcmd(struct rk3576_rknn_matmul_ctx_s *ctx,
                                       uintptr_t a_pa, uintptr_t b_pa,
                                       uintptr_t c_pa);
static int rk3576_rknn_mm_apply_template(struct rk3576_rknn_matmul_ctx_s *ctx,
                                         uintptr_t a_pa, uintptr_t b_pa,
                                         uintptr_t c_pa);
static int rk3576_rknn_mm_phys(void *va, uintptr_t *pa);
static int rk3576_rknn_mm_run_npu(struct rk3576_rknn_matmul_ctx_s *ctx);
static void rk3576_rknn_mm_free_buffers(struct rk3576_rknn_matmul_ctx_s *ctx);
#ifdef CONFIG_RK3576_RKNN_MATMUL_SELFTEST
static void rk3576_rknn_mm_selftest_naive(const uint16_t *a, const uint8_t *b,
                                          const float *scale, double *c,
                                          uint32_t m, uint32_t k, uint32_t n,
                                          uint32_t group);
#endif

/****************************************************************************
 * External Function Prototypes
 ****************************************************************************/

/* Low-level RKNPU core driver (chips/rk3576/rk3576_rknpu.c).
 *
 * Declared weak on purpose: this submission layer is useful on its own
 * through the CPU reference, and the core driver is an independent build
 * unit.  Both symbols resolve to NULL when it is absent, which the NPU path
 * checks before dereferencing.
 *
 * rk3576_rknpu_available() returns > 0 once the NPU is clocked, powered and
 * out of reset.  rk3576_rknpu_submit_regcmd() programs the program-counter
 * block with a DMA-resident command stream, kicks it and blocks until the
 * job-done interrupt or the timeout.
 */

int rk3576_rknpu_available(void) weak_function;
int rk3576_rknpu_submit_regcmd(uint32_t core_mask, uintptr_t regcmd_pa,
                               uint32_t nwords, uint32_t ntasks,
                               uint32_t timeout_ms) weak_function;

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Per-type geometry.
 *
 * INFERRED.  Every sub-tile size and alignment below is an inference from
 * the RK35xx family behaviour of the public rknn_matmul_api, not a value
 * read out of the RK3576 TRM.  The reduction axis is fetched 32 elements at
 * a time for 16-bit and 8-bit operands and 64 at a time for 4-bit operands
 * (twice as many codes fit in one fetch), which is where the K alignment
 * comes from - so a mixed fp16 x int4 problem has to pad K to 64.  The
 * output lane count follows the 16-byte DPU write granularity: 4 lanes of
 * fp32/int32, 8 lanes of fp16.
 */

static const struct rk3576_rknn_mm_traits_s g_rk3576_rknn_mm_traits[] = {
  {
      .type = RK3576_RKNN_FLOAT16_MM_FLOAT16_TO_FLOAT32,
      .a_bits = 16,
      .b_bits = 16,
      .c_bits = 32,
      .subk_a = 32,
      .subn_b = 16,
      .subn_c = 4,
      .k_align = 32,
      .n_align = 16,
      .c_float = true,
      .b_int = false,
  },
  {
      .type = RK3576_RKNN_FLOAT16_MM_FLOAT16_TO_FLOAT16,
      .a_bits = 16,
      .b_bits = 16,
      .c_bits = 16,
      .subk_a = 32,
      .subn_b = 16,
      .subn_c = 8,
      .k_align = 32,
      .n_align = 16,
      .c_float = true,
      .b_int = false,
  },
  {
      .type = RK3576_RKNN_FLOAT16_MM_INT4_TO_FLOAT32,
      .a_bits = 16,
      .b_bits = 4,
      .c_bits = 32,
      .subk_a = 32,
      .subn_b = 32,
      .subn_c = 4,
      .k_align = 64,
      .n_align = 32,
      .c_float = true,
      .b_int = true,
  },
  {
      .type = RK3576_RKNN_FLOAT16_MM_INT4_TO_FLOAT16,
      .a_bits = 16,
      .b_bits = 4,
      .c_bits = 16,
      .subk_a = 32,
      .subn_b = 32,
      .subn_c = 8,
      .k_align = 64,
      .n_align = 32,
      .c_float = true,
      .b_int = true,
  },
  {
      .type = RK3576_RKNN_INT8_MM_INT8_TO_INT32,
      .a_bits = 8,
      .b_bits = 8,
      .c_bits = 32,
      .subk_a = 32,
      .subn_b = 32,
      .subn_c = 4,
      .k_align = 32,
      .n_align = 32,
      .c_float = false,
      .b_int = true,
  },
};

/* Chip-wide register offset map.  Everything starts out unknown, so the
 * synthesised command stream is empty and the NPU path reports -ENOSYS
 * until measured offsets are installed with
 * rk3576_rknn_matmul_set_regmap().
 */

static struct rk3576_rknn_matmul_regmap_s g_rk3576_rknn_mm_regmap = {
  .op_enable = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_conv_con1 = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_data_size = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_weight_size = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_data_base = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_weight_base = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_data_type = RK3576_RKNN_MM_REG_UNKNOWN,
  .cna_weight_type = RK3576_RKNN_MM_REG_UNKNOWN,
  .core_mac_gate = RK3576_RKNN_MM_REG_UNKNOWN,
  .dpu_feature_mode = RK3576_RKNN_MM_REG_UNKNOWN,
  .dpu_dst_base = RK3576_RKNN_MM_REG_UNKNOWN,
  .dpu_dst_size = RK3576_RKNN_MM_REG_UNKNOWN,
  .dpu_out_cvt = RK3576_RKNN_MM_REG_UNKNOWN,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifndef RK3576_RKNN_MM_NATIVE_FP16

/****************************************************************************
 * Name: rk3576_rknn_mm_fp16_to_fp32_sw
 *
 * Description:
 *   IEEE 754 binary16 to binary32, in software.  Subnormals are normalised,
 *   infinities and NaNs keep their payload.
 *
 ****************************************************************************/

static float rk3576_rknn_mm_fp16_to_fp32_sw(uint16_t h)
{
  union
  {
    uint32_t u;
    float f;
  } cvt;

  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (uint32_t)(h >> 10) & 0x1f;
  uint32_t man = (uint32_t)h & 0x3ff;

  if (exp == 0)
    {
      if (man == 0)
        {
          cvt.u = sign;
        }
      else
        {
          /* Subnormal: shift the mantissa left until the hidden bit
           * appears, decrementing the exponent for each shift.
           */

          uint32_t e = 127 - 15 + 1;

          while ((man & 0x400) == 0)
            {
              man <<= 1;
              e--;
            }

          man &= 0x3ff;
          cvt.u = sign | (e << 23) | (man << 13);
        }
    }
  else if (exp == 0x1f)
    {
      cvt.u = sign | 0x7f800000u | (man << 13);
    }
  else
    {
      cvt.u = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }

  return cvt.f;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_fp32_to_fp16_sw
 *
 * Description:
 *   IEEE 754 binary32 to binary16, in software, rounding to nearest even.
 *   Overflow saturates to infinity, underflow flushes through the subnormal
 *   range to zero.
 *
 ****************************************************************************/

static uint16_t rk3576_rknn_mm_fp32_to_fp16_sw(float f)
{
  union
  {
    float f;
    uint32_t u;
  } cvt;

  uint32_t sign;
  uint32_t man;
  uint32_t bexp;
  int32_t exp;

  cvt.f = f;
  sign = (cvt.u >> 16) & 0x8000;
  bexp = (cvt.u >> 23) & 0xff;
  man = cvt.u & 0x7fffff;

  if (bexp == 0xff)
    {
      /* Infinity, or a NaN with a non-zero payload preserved as a quiet
       * NaN so that the sign of a failure is visible downstream.
       */

      return (uint16_t)(sign | 0x7c00 | (man != 0 ? 0x200 : 0));
    }

  exp = (int32_t)bexp - 127 + 15;

  if (exp >= 0x1f)
    {
      return (uint16_t)(sign | 0x7c00);
    }

  if (exp <= 0)
    {
      uint32_t shift;
      uint32_t sub;
      uint32_t rem;
      uint32_t half;

      if (exp < -10)
        {
          return (uint16_t)sign;
        }

      man |= 0x800000;
      shift = (uint32_t)(14 - exp);
      sub = man >> shift;
      rem = man & ((1u << shift) - 1u);
      half = 1u << (shift - 1);

      if (rem > half || (rem == half && (sub & 1) != 0))
        {
          sub++;
        }

      return (uint16_t)(sign | sub);
    }

  /* Normal range.  A carry out of the mantissa propagates into the
   * exponent field, which is exactly the required behaviour, including the
   * promotion to infinity at the top of the range.
   */

  {
    uint32_t bits = ((uint32_t)exp << 10) | (man >> 13);
    uint32_t rem = man & 0x1fff;

    if (rem > 0x1000 || (rem == 0x1000 && (bits & 1) != 0))
      {
        bits++;
      }

    return (uint16_t)(sign | bits);
  }
}

#endif /* !RK3576_RKNN_MM_NATIVE_FP16 */

/****************************************************************************
 * Name: rk3576_rknn_mm_traits
 *
 * Description:
 *   Look up the geometry of a matmul type.
 *
 * Returned Value:
 *   Pointer to the traits entry, or NULL if the type is not implemented.
 *
 ****************************************************************************/

static const struct rk3576_rknn_mm_traits_s *rk3576_rknn_mm_traits(int type)
{
  size_t i;

  for (i = 0; i < nitems(g_rk3576_rknn_mm_traits); i++)
    {
      if (g_rk3576_rknn_mm_traits[i].type == type)
        {
          return &g_rk3576_rknn_mm_traits[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_a_index
 *
 * Description:
 *   Element index of A(m, k) in the caller's layout.
 *
 ****************************************************************************/

static size_t
rk3576_rknn_mm_a_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                       uint32_t k)
{
  switch (ctx->info.a_layout)
    {
      case RK3576_RKNN_MM_LAYOUT_NATIVE:
        {
          uint32_t sub = ctx->tr->subk_a;

          return (((size_t)(k / sub) * ctx->info.m) + m) * sub + (k % sub);
        }

      case RK3576_RKNN_MM_LAYOUT_TP_NORM:
        return (size_t)k * ctx->info.m + m;

      default:
        return (size_t)m * ctx->info.k + k;
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_b_index
 *
 * Description:
 *   Element index of B(k, n) in the caller's layout.
 *
 ****************************************************************************/

static size_t
rk3576_rknn_mm_b_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t k,
                       uint32_t n)
{
  switch (ctx->info.b_layout)
    {
      case RK3576_RKNN_MM_LAYOUT_NATIVE:
        {
          uint32_t sub = ctx->tr->subn_b;

          return (((size_t)(n / sub) * ctx->kal) + k) * sub + (n % sub);
        }

      case RK3576_RKNN_MM_LAYOUT_TP_NORM:
        return (size_t)n * ctx->info.k + k;

      default:
        return (size_t)k * ctx->info.n + n;
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_c_index
 *
 * Description:
 *   Element index of C(m, n) in the caller's layout.
 *
 ****************************************************************************/

static size_t
rk3576_rknn_mm_c_index(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                       uint32_t n)
{
  switch (ctx->info.ac_layout)
    {
      case RK3576_RKNN_MM_LAYOUT_NATIVE:
        {
          uint32_t sub = ctx->tr->subn_c;

          return (((size_t)(n / sub) * ctx->info.m) + m) * sub + (n % sub);
        }

      case RK3576_RKNN_MM_LAYOUT_TP_NORM:
        return (size_t)n * ctx->info.m + m;

      default:
        return (size_t)m * ctx->info.n + n;
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_a_raw
 *
 * Description:
 *   Raw stored code of A(m, k): fp16 bit pattern or int8 byte.
 *
 ****************************************************************************/

static uint32_t
rk3576_rknn_mm_a_raw(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t m,
                     uint32_t k)
{
  size_t idx = rk3576_rknn_mm_a_index(ctx, m, k);

  if (ctx->tr->a_bits == 16)
    {
      return ((const uint16_t *)ctx->a_src)[idx];
    }

  return ((const uint8_t *)ctx->a_src)[idx];
}

/****************************************************************************
 * Name: rk3576_rknn_mm_b_raw
 *
 * Description:
 *   Raw stored code of B(k, n): fp16 bit pattern, int8 byte, or 4-bit
 *   nibble.  Nibbles are packed two per byte with the even element index in
 *   the low nibble.
 *
 ****************************************************************************/

static uint32_t
rk3576_rknn_mm_b_raw(const struct rk3576_rknn_matmul_ctx_s *ctx, uint32_t k,
                     uint32_t n)
{
  size_t idx = rk3576_rknn_mm_b_index(ctx, k, n);

  switch (ctx->tr->b_bits)
    {
      case 4:
        {
          uint8_t byte = ((const uint8_t *)ctx->b_src)[idx >> 1];

          return ((idx & 1) != 0) ? (uint32_t)(byte >> 4)
                                  : (uint32_t)(byte & 0x0f);
        }

      case 16:
        return ((const uint16_t *)ctx->b_src)[idx];

      default:
        return ((const uint8_t *)ctx->b_src)[idx];
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_sext
 *
 * Description:
 *   Turn a raw B code into a signed integer, honouring the width of the
 *   type and the caller's signed/unsigned convention.  Unsigned codes are
 *   returned as-is; the zero point is applied by the caller.
 *
 ****************************************************************************/

static int32_t rk3576_rknn_mm_sext(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                   uint32_t raw)
{
  if (!ctx->quant.b_signed)
    {
      return (int32_t)raw;
    }

  if (ctx->tr->b_bits == 4)
    {
      return ((raw & 0x8) != 0) ? (int32_t)raw - 16 : (int32_t)raw;
    }

  return (int32_t)(int8_t)(uint8_t)raw;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_a_float
 *
 * Description:
 *   A(m, k) as a float, whatever its stored type.
 *
 ****************************************************************************/

static float rk3576_rknn_mm_a_float(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                    uint32_t m, uint32_t k)
{
  uint32_t raw = rk3576_rknn_mm_a_raw(ctx, m, k);

  if (ctx->tr->a_bits == 16)
    {
      return rk3576_rknn_fp16_to_fp32((uint16_t)raw);
    }

  return (float)(int32_t)(int8_t)(uint8_t)raw;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_c_store_f
 *
 * Description:
 *   Write one float result into the caller's C buffer, narrowing to fp16
 *   when that is the accumulator type.
 *
 ****************************************************************************/

static void
rk3576_rknn_mm_c_store_f(const struct rk3576_rknn_matmul_ctx_s *ctx,
                         uint32_t m, uint32_t n, float v)
{
  size_t idx = rk3576_rknn_mm_c_index(ctx, m, n);

  if (ctx->tr->c_bits == 16)
    {
      ((uint16_t *)ctx->c_dst)[idx] = rk3576_rknn_fp32_to_fp16(v);
    }
  else
    {
      ((float *)ctx->c_dst)[idx] = v;
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_c_store_i
 *
 * Description:
 *   Write one integer result into the caller's C buffer.
 *
 ****************************************************************************/

static void
rk3576_rknn_mm_c_store_i(const struct rk3576_rknn_matmul_ctx_s *ctx,
                         uint32_t m, uint32_t n, int32_t v)
{
  size_t idx = rk3576_rknn_mm_c_index(ctx, m, n);

  if (ctx->tr->c_bits == 16)
    {
      ((int16_t *)ctx->c_dst)[idx] = (int16_t)v;
    }
  else
    {
      ((int32_t *)ctx->c_dst)[idx] = v;
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_quant_at
 *
 * Description:
 *   Fetch the scale and zero point of one quantisation group of one output
 *   column, defaulting to the identity transform when no parameters were
 *   installed.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_quant_at(const struct rk3576_rknn_matmul_ctx_s *ctx,
                                    uint32_t group, uint32_t n, float *scale,
                                    int32_t *zp)
{
  size_t idx = (size_t)group * ctx->info.n + n;

  *scale = (ctx->quant.b_scale != NULL) ? ctx->quant.b_scale[idx] : 1.0f;
  *zp = (ctx->quant.b_zp != NULL) ? (int32_t)ctx->quant.b_zp[idx] : 0;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_cpu_float
 *
 * Description:
 *   Scalar reference kernel for the float accumulator types, covering the
 *   fp16 x int4 hero case and fp16 x fp16.  Output columns [n0, n1) only,
 *   so the caller can shard across threads.
 *
 *   For a quantised B the reduction is split at the group boundaries: the
 *   integer products of one group are summed first and the group scale is
 *   applied once, which is both faster and closer to the arithmetic the
 *   hardware performs than dequantising every weight individually.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_cpu_float(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t n0, uint32_t n1)
{
  const uint32_t kdim = ctx->info.k;
  const uint32_t group = (ctx->quant.b_group != 0) ? ctx->quant.b_group : kdim;
  uint32_t m;
  uint32_t n;
  uint32_t k;

  for (m = 0; m < ctx->info.m; m++)
    {
      for (n = n0; n < n1; n++)
        {
          float acc = 0.0f;

          if (ctx->tr->b_int)
            {
              uint32_t k0;

              for (k0 = 0; k0 < kdim; k0 += group)
                {
                  uint32_t kend = MIN(k0 + group, kdim);
                  float gacc = 0.0f;
                  float scale;
                  int32_t zp;

                  rk3576_rknn_mm_quant_at(ctx, k0 / group, n, &scale, &zp);

                  for (k = k0; k < kend; k++)
                    {
                      int32_t w = rk3576_rknn_mm_sext(
                                      ctx, rk3576_rknn_mm_b_raw(ctx, k, n)) -
                                  zp;

                      gacc += rk3576_rknn_mm_a_float(ctx, m, k) * (float)w;
                    }

                  acc += gacc * scale;
                }
            }
          else
            {
              for (k = 0; k < kdim; k++)
                {
                  float w = rk3576_rknn_fp16_to_fp32(
                      (uint16_t)rk3576_rknn_mm_b_raw(ctx, k, n));

                  acc += rk3576_rknn_mm_a_float(ctx, m, k) * w;
                }
            }

          rk3576_rknn_mm_c_store_f(ctx, m, n, acc);
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_cpu_int
 *
 * Description:
 *   Scalar reference kernel for the integer accumulator types.  The scale
 *   is deliberately ignored: an int32 accumulator carries the raw dot
 *   product and requantisation is the caller's business.  The zero point is
 *   applied because it changes the integer result.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_cpu_int(struct rk3576_rknn_matmul_ctx_s *ctx,
                                   uint32_t n0, uint32_t n1)
{
  const uint32_t kdim = ctx->info.k;
  const uint32_t group = (ctx->quant.b_group != 0) ? ctx->quant.b_group : kdim;
  uint32_t m;
  uint32_t n;
  uint32_t k;

  for (m = 0; m < ctx->info.m; m++)
    {
      for (n = n0; n < n1; n++)
        {
          int32_t acc = 0;
          uint32_t k0;

          for (k0 = 0; k0 < kdim; k0 += group)
            {
              uint32_t kend = MIN(k0 + group, kdim);
              float scale;
              int32_t zp;

              rk3576_rknn_mm_quant_at(ctx, k0 / group, n, &scale, &zp);
              (void)scale; /* An integer accumulator carries the raw sum. */

              for (k = k0; k < kend; k++)
                {
                  int32_t w = rk3576_rknn_mm_sext(
                                  ctx, rk3576_rknn_mm_b_raw(ctx, k, n)) -
                              zp;
                  int32_t a = (int32_t)(int8_t)(uint8_t)rk3576_rknn_mm_a_raw(
                      ctx, m, k);

                  acc += a * w;
                }
            }

          rk3576_rknn_mm_c_store_i(ctx, m, n, acc);
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_cpu_range
 *
 * Description:
 *   Dispatch one column slice to the kernel matching the accumulator type.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_cpu_range(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t n0, uint32_t n1)
{
  if (ctx->tr->c_float)
    {
      rk3576_rknn_mm_cpu_float(ctx, n0, n1);
    }
  else
    {
      rk3576_rknn_mm_cpu_int(ctx, n0, n1);
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_worker
 *
 * Description:
 *   pthread entry point for one column slice.
 *
 ****************************************************************************/

static void *rk3576_rknn_mm_worker(void *arg)
{
  struct rk3576_rknn_mm_work_s *work = arg;

  rk3576_rknn_mm_cpu_range(work->ctx, work->n0, work->n1);
  return NULL;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_cpu_exec
 *
 * Description:
 *   Run the reference implementation over the whole output, sharding the
 *   columns across info.num_threads workers when the problem is wide enough
 *   to pay for them.  Slices are disjoint column ranges, so no locking is
 *   needed on the output.
 *
 * Returned Value:
 *   OK always; a thread that fails to spawn is executed inline.
 *
 ****************************************************************************/

static int rk3576_rknn_mm_cpu_exec(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  struct rk3576_rknn_mm_work_s work[RK3576_RKNN_MM_MAX_THREADS];
  pthread_t tid[RK3576_RKNN_MM_MAX_THREADS];
  pthread_attr_t attr;
  uint32_t chunk;
  int nthreads;
  int started = 0;
  int i;

  nthreads = ctx->info.num_threads;
  if (nthreads > RK3576_RKNN_MM_MAX_THREADS)
    {
      nthreads = RK3576_RKNN_MM_MAX_THREADS;
    }

  if (nthreads <= 1 ||
      ctx->info.n < (uint32_t)nthreads * RK3576_RKNN_MM_MIN_COLS)
    {
      rk3576_rknn_mm_cpu_range(ctx, 0, ctx->info.n);
      return OK;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, RK3576_RKNN_MM_WORKER_STACK);

  chunk = ctx->info.n / (uint32_t)nthreads;

  for (i = 0; i < nthreads - 1; i++)
    {
      work[i].ctx = ctx;
      work[i].n0 = chunk * (uint32_t)i;
      work[i].n1 = chunk * (uint32_t)(i + 1);

      if (pthread_create(&tid[started], &attr, rk3576_rknn_mm_worker,
                         &work[i]) == 0)
        {
          started++;
        }
      else
        {
          rk3576_rknn_mm_cpu_range(ctx, work[i].n0, work[i].n1);
        }
    }

  pthread_attr_destroy(&attr);

  /* The last slice absorbs the remainder and runs on the calling thread. */

  rk3576_rknn_mm_cpu_range(ctx, chunk * (uint32_t)(nthreads - 1), ctx->info.n);

  for (i = 0; i < started; i++)
    {
      pthread_join(tid[i], NULL);
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_pack_a
 *
 * Description:
 *   Stage A into the NPU native layout [Kal/subKa][M][subKa], zero-filling
 *   the K padding.  An operand already in native layout is copied straight
 *   through; in that case the caller is responsible for the padding.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_pack_a(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  uint32_t sub = ctx->tr->subk_a;
  uint32_t nkb = ctx->kal / sub;
  uint32_t kb;
  uint32_t m;
  uint32_t kk;

  if (ctx->info.a_layout == RK3576_RKNN_MM_LAYOUT_NATIVE)
    {
      memcpy(ctx->a_dma, ctx->a_src, ctx->a_size);
      return;
    }

  for (kb = 0; kb < nkb; kb++)
    {
      for (m = 0; m < ctx->info.m; m++)
        {
          for (kk = 0; kk < sub; kk++)
            {
              uint32_t k = kb * sub + kk;
              size_t dst = ((size_t)kb * ctx->info.m + m) * sub + kk;
              uint32_t val =
                  (k < ctx->info.k) ? rk3576_rknn_mm_a_raw(ctx, m, k) : 0;

              if (ctx->tr->a_bits == 16)
                {
                  ((uint16_t *)ctx->a_dma)[dst] = (uint16_t)val;
                }
              else
                {
                  ((uint8_t *)ctx->a_dma)[dst] = (uint8_t)val;
                }
            }
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_pack_b
 *
 * Description:
 *   Stage B into the NPU native layout [Nal/subNb][Kal][subNb], zero-filling
 *   the K and N padding.  Zero-padding a quantised operand is only correct
 *   because the padded columns are discarded when C is unpacked, and the
 *   padded rows contribute (0 - zp) * scale to real columns - which is why
 *   the pad is written as the zero *code* and the caller must supply a zero
 *   point of zero for symmetric quantisation.
 *
 *   TODO: an asymmetric (non-zero zero point) int4 weight matrix needs the
 *   K padding filled with the per-group zero point instead of the zero
 *   code.  The CPU reference is unaffected because it never reads past K.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_pack_b(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  uint32_t sub = ctx->tr->subn_b;
  uint32_t nnb = ctx->nal / sub;
  uint32_t nb;
  uint32_t k;
  uint32_t nn;

  if (ctx->info.b_layout == RK3576_RKNN_MM_LAYOUT_NATIVE)
    {
      memcpy(ctx->b_dma, ctx->b_src, ctx->b_size);
      return;
    }

  memset(ctx->b_dma, 0, ctx->b_size);

  for (nb = 0; nb < nnb; nb++)
    {
      for (k = 0; k < ctx->kal; k++)
        {
          for (nn = 0; nn < sub; nn++)
            {
              uint32_t n = nb * sub + nn;
              size_t dst = ((size_t)nb * ctx->kal + k) * sub + nn;
              uint32_t val;

              if (k >= ctx->info.k || n >= ctx->info.n)
                {
                  continue; /* Already zero from the memset above. */
                }

              val = rk3576_rknn_mm_b_raw(ctx, k, n);

              switch (ctx->tr->b_bits)
                {
                  case 4:
                    {
                      uint8_t *byte = &((uint8_t *)ctx->b_dma)[dst >> 1];

                      if ((dst & 1) != 0)
                        {
                          *byte =
                              (uint8_t)((*byte & 0x0f) | ((val & 0x0f) << 4));
                        }
                      else
                        {
                          *byte = (uint8_t)((*byte & 0xf0) | (val & 0x0f));
                        }
                    }
                    break;

                  case 16:
                    ((uint16_t *)ctx->b_dma)[dst] = (uint16_t)val;
                    break;

                  default:
                    ((uint8_t *)ctx->b_dma)[dst] = (uint8_t)val;
                    break;
                }
            }
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_unpack_c
 *
 * Description:
 *   Scatter the NPU output from the native layout [Nal/subNc][M][subNc]
 *   into the caller's C buffer, dropping the N padding.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_unpack_c(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  uint32_t sub = ctx->tr->subn_c;
  uint32_t m;
  uint32_t n;

  if (ctx->info.ac_layout == RK3576_RKNN_MM_LAYOUT_NATIVE)
    {
      memcpy(ctx->c_dst, ctx->c_dma, ctx->c_size);
      return;
    }

  for (m = 0; m < ctx->info.m; m++)
    {
      for (n = 0; n < ctx->info.n; n++)
        {
          size_t src = ((size_t)(n / sub) * ctx->info.m + m) * sub + (n % sub);

          if (ctx->tr->c_float)
            {
              float v = (ctx->tr->c_bits == 16)
                            ? rk3576_rknn_fp16_to_fp32(
                                  ((const uint16_t *)ctx->c_dma)[src])
                            : ((const float *)ctx->c_dma)[src];

              rk3576_rknn_mm_c_store_f(ctx, m, n, v);
            }
          else
            {
              int32_t v = (ctx->tr->c_bits == 16)
                              ? ((const int16_t *)ctx->c_dma)[src]
                              : ((const int32_t *)ctx->c_dma)[src];

              rk3576_rknn_mm_c_store_i(ctx, m, n, v);
            }
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_mm_build_regcmd
 *
 * Description:
 *   Synthesise the register command stream for this problem from the
 *   installed register map.  Fields whose offset is still unknown are
 *   skipped, so an uncalibrated map yields an empty stream.
 *
 * Returned Value:
 *   Number of words emitted on success, -ENOSYS when the register map has
 *   not been calibrated.
 *
 ****************************************************************************/

static int rk3576_rknn_mm_build_regcmd(struct rk3576_rknn_matmul_ctx_s *ctx,
                                       uintptr_t a_pa, uintptr_t b_pa,
                                       uintptr_t c_pa)
{
  const struct rk3576_rknn_matmul_regmap_s *map = &g_rk3576_rknn_mm_regmap;
  uint32_t *cmd = ctx->regcmd;
  uint32_t w = 0;
  uint32_t a_dtype;
  uint32_t b_dtype;
  uint32_t c_dtype;

#define RK3576_RKNN_MM_EMIT(off, val)          \
  do                                           \
    {                                          \
      if ((off) != RK3576_RKNN_MM_REG_UNKNOWN) \
        {                                      \
          cmd[w++] = (uint32_t)(off);          \
          cmd[w++] = (uint32_t)(val);          \
        }                                      \
    }                                          \
  while (0)

  a_dtype = (ctx->tr->a_bits == 16) ? RK3576_RKNN_MM_DTYPE_FP16
                                    : RK3576_RKNN_MM_DTYPE_INT8;

  b_dtype = (ctx->tr->b_bits == 16)  ? RK3576_RKNN_MM_DTYPE_FP16
            : (ctx->tr->b_bits == 8) ? RK3576_RKNN_MM_DTYPE_INT8
                                     : RK3576_RKNN_MM_DTYPE_INT4;

  c_dtype = ctx->tr->c_float
                ? ((ctx->tr->c_bits == 16) ? RK3576_RKNN_MM_DTYPE_FP16
                                           : RK3576_RKNN_MM_DTYPE_FP32)
                : RK3576_RKNN_MM_DTYPE_INT32;

  /* Pipeline configuration first, then the operand descriptors, then the
   * output descriptor, and only then the enable - the program counter
   * replays the stream in order and the enable must be last.
   */

  RK3576_RKNN_MM_EMIT(map->cna_conv_con1, RK3576_RKNN_MM_CONV_CON1_1X1);
  RK3576_RKNN_MM_EMIT(map->cna_data_type, a_dtype);
  RK3576_RKNN_MM_EMIT(map->cna_weight_type, b_dtype);
  RK3576_RKNN_MM_EMIT(map->cna_data_size,
                      RK3576_RKNN_MM_SIZE_ENC(ctx->kal, ctx->info.m));
  RK3576_RKNN_MM_EMIT(map->cna_weight_size,
                      RK3576_RKNN_MM_SIZE_ENC(ctx->nal, ctx->kal));
  RK3576_RKNN_MM_EMIT(map->cna_data_base, (uint32_t)a_pa);
  RK3576_RKNN_MM_EMIT(map->cna_weight_base, (uint32_t)b_pa);
  RK3576_RKNN_MM_EMIT(map->core_mac_gate, RK3576_RKNN_MM_MAC_GATE_ALL);
  RK3576_RKNN_MM_EMIT(map->dpu_feature_mode, RK3576_RKNN_MM_DPU_MODE_GEMM);
  RK3576_RKNN_MM_EMIT(map->dpu_out_cvt, c_dtype);
  RK3576_RKNN_MM_EMIT(map->dpu_dst_size,
                      RK3576_RKNN_MM_SIZE_ENC(ctx->nal, ctx->info.m));
  RK3576_RKNN_MM_EMIT(map->dpu_dst_base, (uint32_t)c_pa);
  RK3576_RKNN_MM_EMIT(map->op_enable, 1);

#undef RK3576_RKNN_MM_EMIT

  if (w == 0)
    {
      return -ENOSYS;
    }

  ctx->regcmd_words = w;
  return (int)w;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_apply_template
 *
 * Description:
 *   Copy a captured golden command stream into the DMA buffer and patch it
 *   with this context's buffer addresses and shape.
 *
 * Returned Value:
 *   Number of words on success, a negated errno on failure.
 *
 ****************************************************************************/

static int rk3576_rknn_mm_apply_template(struct rk3576_rknn_matmul_ctx_s *ctx,
                                         uintptr_t a_pa, uintptr_t b_pa,
                                         uintptr_t c_pa)
{
  const struct rk3576_rknn_matmul_template_s *tpl = ctx->tpl;
  uint32_t *cmd = ctx->regcmd;

  memcpy(cmd, tpl->words, (size_t)tpl->nwords * sizeof(uint32_t));

  if (tpl->idx_a_addr != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_a_addr] = (uint32_t)a_pa;
    }

  if (tpl->idx_b_addr != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_b_addr] = (uint32_t)b_pa;
    }

  if (tpl->idx_c_addr != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_c_addr] = (uint32_t)c_pa;
    }

  if (tpl->idx_m != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_m] = ctx->info.m;
    }

  if (tpl->idx_k != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_k] = ctx->kal;
    }

  if (tpl->idx_n != RK3576_RKNN_MM_NOPATCH)
    {
      cmd[tpl->idx_n] = ctx->nal;
    }

  ctx->regcmd_words = tpl->nwords;
  return (int)tpl->nwords;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_phys
 *
 * Description:
 *   Translate a DMA-heap virtual address and check that it is reachable by
 *   the NPU's 32-bit address registers.
 *
 * Returned Value:
 *   OK on success, -EFAULT when the buffer sits above 4GB.
 *
 ****************************************************************************/

static int rk3576_rknn_mm_phys(void *va, uintptr_t *pa)
{
  uintptr_t addr = up_addrenv_va_to_pa(va);

  if ((uint64_t)addr >= RK3576_RKNN_MM_ADDR_LIMIT)
    {
      _err("ERROR: buffer %p maps to 0x%" PRIxPTR ", beyond the NPU 32-bit "
           "address range\n",
           va, addr);
      return -EFAULT;
    }

  *pa = addr;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_run_npu
 *
 * Description:
 *   Pack the operands, build or patch the command stream, hand it to the
 *   RKNPU core driver and scatter the result back.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.  -ENODEV means the core
 *   driver is absent or the NPU is not up; -ENOSYS means the register map
 *   has not been calibrated yet.  Both are recoverable in AUTO mode.
 *
 ****************************************************************************/

static int rk3576_rknn_mm_run_npu(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  uintptr_t a_pa;
  uintptr_t b_pa;
  uintptr_t c_pa;
  uintptr_t cmd_pa;
  int ret;

  if (rk3576_rknpu_submit_regcmd == NULL)
    {
      return -ENODEV;
    }

  if (rk3576_rknpu_available != NULL && rk3576_rknpu_available() <= 0)
    {
      return -ENODEV;
    }

  ret = rk3576_rknn_mm_phys(ctx->a_dma, &a_pa);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_rknn_mm_phys(ctx->b_dma, &b_pa);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_rknn_mm_phys(ctx->c_dma, &c_pa);
  if (ret < 0)
    {
      return ret;
    }

  ret = rk3576_rknn_mm_phys(ctx->regcmd, &cmd_pa);
  if (ret < 0)
    {
      return ret;
    }

  if (ctx->tpl != NULL)
    {
      ret = rk3576_rknn_mm_apply_template(ctx, a_pa, b_pa, c_pa);
    }
  else
    {
      ret = rk3576_rknn_mm_build_regcmd(ctx, a_pa, b_pa, c_pa);
    }

  if (ret < 0)
    {
      return ret;
    }

  rk3576_rknn_mm_pack_a(ctx);

  if (ctx->b_dirty)
    {
      rk3576_rknn_mm_pack_b(ctx);
      ctx->b_dirty = false;
    }

  memset(ctx->c_dma, 0, ctx->c_size);

  /* Push everything the NPU reads - and the zeroed output - out of the
   * D-cache before the job starts.
   */

  up_clean_dcache((uintptr_t)ctx->a_dma, (uintptr_t)ctx->a_dma + ctx->a_size);
  up_clean_dcache((uintptr_t)ctx->b_dma, (uintptr_t)ctx->b_dma + ctx->b_size);
  up_clean_dcache((uintptr_t)ctx->c_dma, (uintptr_t)ctx->c_dma + ctx->c_size);
  up_clean_dcache((uintptr_t)ctx->regcmd,
                  (uintptr_t)ctx->regcmd +
                      (size_t)ctx->regcmd_words * sizeof(uint32_t));

  ret = rk3576_rknpu_submit_regcmd(ctx->core_mask, cmd_pa, ctx->regcmd_words,
                                   1, ctx->timeout_ms);
  if (ret < 0)
    {
      _err("ERROR: NPU submission failed: %d\n", ret);
      return ret;
    }

  up_invalidate_dcache((uintptr_t)ctx->c_dma,
                       (uintptr_t)ctx->c_dma + ctx->c_size);

  rk3576_rknn_mm_unpack_c(ctx);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_mm_free_buffers
 *
 * Description:
 *   Release whatever staging buffers a context holds.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_free_buffers(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  if (ctx->a_dma != NULL)
    {
      rk3576_dma_free(ctx->a_dma, ctx->a_size);
      ctx->a_dma = NULL;
    }

  if (ctx->b_dma != NULL)
    {
      rk3576_dma_free(ctx->b_dma, ctx->b_size);
      ctx->b_dma = NULL;
    }

  if (ctx->c_dma != NULL)
    {
      rk3576_dma_free(ctx->c_dma, ctx->c_size);
      ctx->c_dma = NULL;
    }

  if (ctx->regcmd != NULL)
    {
      rk3576_dma_free(ctx->regcmd, ctx->regcmd_size);
      ctx->regcmd = NULL;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_rknn_fp16_to_fp32
 ****************************************************************************/

float rk3576_rknn_fp16_to_fp32(uint16_t h)
{
#ifdef RK3576_RKNN_MM_NATIVE_FP16
  __fp16 v;

  memcpy(&v, &h, sizeof(v));
  return (float)v;
#else
  return rk3576_rknn_mm_fp16_to_fp32_sw(h);
#endif
}

/****************************************************************************
 * Name: rk3576_rknn_fp32_to_fp16
 ****************************************************************************/

uint16_t rk3576_rknn_fp32_to_fp16(float f)
{
#ifdef RK3576_RKNN_MM_NATIVE_FP16
  __fp16 v = (__fp16)f;
  uint16_t h;

  memcpy(&h, &v, sizeof(h));
  return h;
#else
  return rk3576_rknn_mm_fp32_to_fp16_sw(f);
#endif
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_query_align
 ****************************************************************************/

int rk3576_rknn_matmul_query_align(int type, uint32_t *k_align,
                                   uint32_t *n_align)
{
  const struct rk3576_rknn_mm_traits_s *tr = rk3576_rknn_mm_traits(type);

  if (tr == NULL)
    {
      return -ENOTSUP;
    }

  if (k_align != NULL)
    {
      *k_align = tr->k_align;
    }

  if (n_align != NULL)
    {
      *n_align = tr->n_align;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_create
 ****************************************************************************/

int rk3576_rknn_matmul_create(const struct rk3576_rknn_matmul_info_s *info,
                              struct rk3576_rknn_matmul_ctx_s **ctxp)
{
  const struct rk3576_rknn_mm_traits_s *tr;
  struct rk3576_rknn_matmul_ctx_s *ctx;
  uint64_t bits;

  if (info == NULL || ctxp == NULL)
    {
      return -EINVAL;
    }

  if (info->m == 0 || info->k == 0 || info->n == 0 ||
      info->m > RK3576_RKNN_MM_MAX_M || info->k > RK3576_RKNN_MM_MAX_K ||
      info->n > RK3576_RKNN_MM_MAX_N)
    {
      _err("ERROR: bad matmul shape %" PRIu32 "x%" PRIu32 "x%" PRIu32 "\n",
           info->m, info->k, info->n);
      return -EINVAL;
    }

  tr = rk3576_rknn_mm_traits(info->type);
  if (tr == NULL)
    {
      _err("ERROR: matmul type %d is not implemented\n", info->type);
      return -ENOTSUP;
    }

  /* A 4-bit operand is addressed by nibble pairs along the fastest-varying
   * axis of its layout, so that axis has to have an even extent.
   */

  if (tr->b_bits == 4)
    {
      if ((info->b_layout == RK3576_RKNN_MM_LAYOUT_NORM &&
           (info->n & 1) != 0) ||
          (info->b_layout == RK3576_RKNN_MM_LAYOUT_TP_NORM &&
           (info->k & 1) != 0))
        {
          _err("ERROR: int4 operand needs an even fastest axis "
               "(K=%" PRIu32 " N=%" PRIu32 " layout=%d)\n",
               info->k, info->n, info->b_layout);
          return -EINVAL;
        }
    }

  ctx = kmm_zalloc(sizeof(*ctx));
  if (ctx == NULL)
    {
      return -ENOMEM;
    }

  ctx->info = *info;
  ctx->tr = tr;
  ctx->kal = RK3576_RKNN_MM_ALIGN_UP(info->k, tr->k_align);
  ctx->nal = RK3576_RKNN_MM_ALIGN_UP(info->n, tr->n_align);
  ctx->core_mask = RK3576_RKNN_MM_CORE_DEFAULT;
  ctx->timeout_ms = RK3576_RKNN_MM_TIMEOUT_MS;
  ctx->backend = RK3576_RKNN_MM_BACKEND_AUTO;
  ctx->b_dirty = true;

  /* Default quantisation: symmetric, unit scale, one group over the whole
   * reduction axis.  This makes a plain integer GEMM behave sensibly
   * without the caller having to say anything.
   */

  ctx->quant.b_signed = true;

  bits = (uint64_t)info->m * ctx->kal * tr->a_bits;
  ctx->a_size = (size_t)((bits + 7) / 8);
  bits = (uint64_t)ctx->kal * ctx->nal * tr->b_bits;
  ctx->b_size = (size_t)((bits + 7) / 8);
  bits = (uint64_t)info->m * ctx->nal * tr->c_bits;
  ctx->c_size = (size_t)((bits + 7) / 8);
  ctx->regcmd_size = RK3576_RKNN_MM_REGCMD_WORDS * sizeof(uint32_t);

  ctx->a_dma = rk3576_dma_alloc(ctx->a_size);
  ctx->b_dma = rk3576_dma_alloc(ctx->b_size);
  ctx->c_dma = rk3576_dma_alloc(ctx->c_size);
  ctx->regcmd = rk3576_dma_alloc(ctx->regcmd_size);

  if (ctx->a_dma == NULL || ctx->b_dma == NULL || ctx->c_dma == NULL ||
      ctx->regcmd == NULL)
    {
      _err("ERROR: DMA heap exhausted (need %zu+%zu+%zu bytes)\n", ctx->a_size,
           ctx->b_size, ctx->c_size);
      rk3576_rknn_mm_free_buffers(ctx);
      kmm_free(ctx);
      return -ENOMEM;
    }

  nxmutex_init(&ctx->lock);

  _info("matmul %" PRIu32 "x%" PRIu32 "x%" PRIu32 " type %d, padded K=%" PRIu32
        " N=%" PRIu32 ", staging %zu/%zu/%zu bytes\n",
        info->m, info->k, info->n, info->type, ctx->kal, ctx->nal, ctx->a_size,
        ctx->b_size, ctx->c_size);

  *ctxp = ctx;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_destroy
 ****************************************************************************/

void rk3576_rknn_matmul_destroy(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  nxmutex_destroy(&ctx->lock);
  rk3576_rknn_mm_free_buffers(ctx);
  kmm_free(ctx);
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_io
 ****************************************************************************/

int rk3576_rknn_matmul_set_io(struct rk3576_rknn_matmul_ctx_s *ctx,
                              const void *a, const void *b, void *c)
{
  int ret;

  if (ctx == NULL || a == NULL || b == NULL || c == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ctx->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (ctx->b_src != b)
    {
      ctx->b_dirty = true;
    }

  ctx->a_src = a;
  ctx->b_src = b;
  ctx->c_dst = c;

  nxmutex_unlock(&ctx->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_quant
 ****************************************************************************/

int rk3576_rknn_matmul_set_quant(struct rk3576_rknn_matmul_ctx_s *ctx,
                                 const struct rk3576_rknn_matmul_quant_s *q)
{
  int ret;

  if (ctx == NULL || q == NULL)
    {
      return -EINVAL;
    }

  if (!ctx->tr->b_int)
    {
      _err("ERROR: matmul type %d has a float B operand\n", ctx->info.type);
      return -EINVAL;
    }

  if (q->b_group != 0 && (ctx->info.k % q->b_group) != 0)
    {
      _err("ERROR: quant group %" PRIu32 " does not divide K=%" PRIu32 "\n",
           q->b_group, ctx->info.k);
      return -EINVAL;
    }

  ret = nxmutex_lock(&ctx->lock);
  if (ret < 0)
    {
      return ret;
    }

  ctx->quant = *q;
  nxmutex_unlock(&ctx->lock);
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_backend
 ****************************************************************************/

int rk3576_rknn_matmul_set_backend(struct rk3576_rknn_matmul_ctx_s *ctx,
                                   int backend)
{
  if (ctx == NULL || backend < RK3576_RKNN_MM_BACKEND_AUTO ||
      backend > RK3576_RKNN_MM_BACKEND_CPU)
    {
      return -EINVAL;
    }

  ctx->backend = backend;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_core_mask
 ****************************************************************************/

int rk3576_rknn_matmul_set_core_mask(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t core_mask)
{
  if (ctx == NULL || core_mask == 0 ||
      (core_mask & ~(uint32_t)RK3576_RKNN_MM_CORE_MASK_ALL) != 0)
    {
      return -EINVAL;
    }

  ctx->core_mask = core_mask;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_template
 ****************************************************************************/

int rk3576_rknn_matmul_set_template(
    struct rk3576_rknn_matmul_ctx_s *ctx,
    const struct rk3576_rknn_matmul_template_s *tpl)
{
  if (ctx == NULL)
    {
      return -EINVAL;
    }

  if (tpl != NULL)
    {
      if (tpl->words == NULL || tpl->nwords == 0)
        {
          return -EINVAL;
        }

      if (tpl->nwords > RK3576_RKNN_MM_REGCMD_WORDS)
        {
          _err("ERROR: template of %" PRIu32 " words exceeds the %d word "
               "command buffer\n",
               tpl->nwords, RK3576_RKNN_MM_REGCMD_WORDS);
          return -E2BIG;
        }
    }

  ctx->tpl = tpl;
  return OK;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_regmap
 ****************************************************************************/

void rk3576_rknn_matmul_set_regmap(
    const struct rk3576_rknn_matmul_regmap_s *map)
{
  if (map != NULL)
    {
      g_rk3576_rknn_mm_regmap = *map;
    }
  else
    {
      memset(&g_rk3576_rknn_mm_regmap, 0xff, sizeof(g_rk3576_rknn_mm_regmap));
    }
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_run_cpu
 ****************************************************************************/

int rk3576_rknn_matmul_run_cpu(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  int ret;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ctx->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (ctx->a_src == NULL || ctx->b_src == NULL || ctx->c_dst == NULL)
    {
      nxmutex_unlock(&ctx->lock);
      return -EINVAL;
    }

  ret = rk3576_rknn_mm_cpu_exec(ctx);
  nxmutex_unlock(&ctx->lock);
  return ret;
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_run
 ****************************************************************************/

int rk3576_rknn_matmul_run(struct rk3576_rknn_matmul_ctx_s *ctx)
{
  int ret;

  if (ctx == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&ctx->lock);
  if (ret < 0)
    {
      return ret;
    }

  if (ctx->a_src == NULL || ctx->b_src == NULL || ctx->c_dst == NULL)
    {
      nxmutex_unlock(&ctx->lock);
      return -EINVAL;
    }

  if (ctx->backend == RK3576_RKNN_MM_BACKEND_CPU)
    {
      ret = rk3576_rknn_mm_cpu_exec(ctx);
    }
  else
    {
      ret = rk3576_rknn_mm_run_npu(ctx);

      if (ret < 0 && ctx->backend == RK3576_RKNN_MM_BACKEND_AUTO)
        {
          if (!ctx->npu_warned)
            {
              _warn("WARNING: NPU path unavailable (%d), using the CPU "
                    "reference\n",
                    ret);
              ctx->npu_warned = true;
            }

          ret = rk3576_rknn_mm_cpu_exec(ctx);
        }
    }

  nxmutex_unlock(&ctx->lock);
  return ret;
}

#ifdef CONFIG_RK3576_RKNN_MATMUL_SELFTEST

/****************************************************************************
 * Name: rk3576_rknn_mm_selftest_naive
 *
 * Description:
 *   Recompute the reference problem the long way round, in double
 *   precision, straight from the raw buffers.  It shares no code with the
 *   kernels under test, which is the point: it catches an inverted nibble
 *   order, a wrong group stride or a sign-extension slip that a
 *   self-consistent implementation would hide.
 *
 ****************************************************************************/

static void rk3576_rknn_mm_selftest_naive(const uint16_t *a, const uint8_t *b,
                                          const float *scale, double *c,
                                          uint32_t m, uint32_t k, uint32_t n,
                                          uint32_t group)
{
  uint32_t row;
  uint32_t col;
  uint32_t idx;

  for (row = 0; row < m; row++)
    {
      for (col = 0; col < n; col++)
        {
          double acc = 0.0;

          for (idx = 0; idx < k; idx++)
            {
              size_t elem = (size_t)idx * n + col;
              uint8_t byte = b[elem >> 1];
              int code = ((elem & 1) != 0) ? (byte >> 4) : (byte & 0x0f);

              if (code >= 8)
                {
                  code -= 16;
                }

              acc +=
                  (double)rk3576_rknn_fp16_to_fp32(a[(size_t)row * k + idx]) *
                  (double)code *
                  (double)scale[(size_t)(idx / group) * n + col];
            }

          c[(size_t)row * n + col] = acc;
        }
    }
}

/****************************************************************************
 * Name: rk3576_rknn_matmul_selftest
 ****************************************************************************/

int rk3576_rknn_matmul_selftest(void)
{
  const uint32_t m = RK3576_RKNN_MM_SELFTEST_M;
  const uint32_t k = RK3576_RKNN_MM_SELFTEST_K;
  const uint32_t n = RK3576_RKNN_MM_SELFTEST_N;
  const uint32_t group = RK3576_RKNN_MM_SELFTEST_GROUP;
  struct rk3576_rknn_matmul_info_s info;
  struct rk3576_rknn_matmul_quant_s quant;
  struct rk3576_rknn_matmul_ctx_s *ctx = NULL;
  uint16_t *a = NULL;
  uint8_t *b = NULL;
  float *scale = NULL;
  float *c_cpu = NULL;
  float *c_npu = NULL;
  double *c_ref = NULL;
  uint32_t seed = 0x13572468;
  uint32_t ngroups = k / group;
  double worst = 0.0;
  size_t i;
  int ret;

  a = kmm_malloc((size_t)m * k * sizeof(uint16_t));
  b = kmm_malloc((size_t)k * n / 2);
  scale = kmm_malloc((size_t)ngroups * n * sizeof(float));
  c_cpu = kmm_malloc((size_t)m * n * sizeof(float));
  c_npu = kmm_malloc((size_t)m * n * sizeof(float));
  c_ref = kmm_malloc((size_t)m * n * sizeof(double));

  if (a == NULL || b == NULL || scale == NULL || c_cpu == NULL ||
      c_npu == NULL || c_ref == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  /* Deterministic pseudo-random operands: activations in [-1, 1), weights
   * spread over the whole int4 range, per-group scales around 2^-7 so the
   * accumulator lands in a range fp16 can also represent.
   */

  for (i = 0; i < (size_t)m * k; i++)
    {
      seed = seed * 1664525u + 1013904223u;
      a[i] = rk3576_rknn_fp32_to_fp16(
          (float)((int32_t)((seed >> 16) & 0x7ff) - 1024) / 1024.0f);
    }

  for (i = 0; i < (size_t)k * n / 2; i++)
    {
      seed = seed * 1664525u + 1013904223u;
      b[i] = (uint8_t)((seed >> 16) & 0xff);
    }

  for (i = 0; i < (size_t)ngroups * n; i++)
    {
      seed = seed * 1664525u + 1013904223u;
      scale[i] = 0.0078125f * (1.0f + (float)((seed >> 16) & 0xff) / 512.0f);
    }

  rk3576_rknn_mm_selftest_naive(a, b, scale, c_ref, m, k, n, group);

  memset(&info, 0, sizeof(info));
  info.m = m;
  info.k = k;
  info.n = n;
  info.type = RK3576_RKNN_FLOAT16_MM_INT4_TO_FLOAT32;
  info.a_layout = RK3576_RKNN_MM_LAYOUT_NORM;
  info.b_layout = RK3576_RKNN_MM_LAYOUT_NORM;
  info.ac_layout = RK3576_RKNN_MM_LAYOUT_NORM;

  ret = rk3576_rknn_matmul_create(&info, &ctx);
  if (ret < 0)
    {
      goto out;
    }

  memset(&quant, 0, sizeof(quant));
  quant.b_scale = scale;
  quant.b_group = group;
  quant.b_signed = true;

  ret = rk3576_rknn_matmul_set_quant(ctx, &quant);
  if (ret < 0)
    {
      goto out;
    }

  ret = rk3576_rknn_matmul_set_io(ctx, a, b, c_cpu);
  if (ret < 0)
    {
      goto out;
    }

  ret = rk3576_rknn_matmul_run_cpu(ctx);
  if (ret < 0)
    {
      goto out;
    }

  for (i = 0; i < (size_t)m * n; i++)
    {
      double diff = (double)c_cpu[i] - c_ref[i];

      if (diff < 0.0)
        {
          diff = -diff;
        }

      if (diff > worst)
        {
          worst = diff;
        }
    }

  if (worst > RK3576_RKNN_MM_SELFTEST_EPS)
    {
      _err("ERROR: CPU kernel disagrees with the naive reference, worst "
           "absolute error %ld ppm\n",
           (long)(worst * 1.0e6));
      ret = -EIO;
      goto out;
    }

  _info("CPU reference matches the naive computation, worst error "
        "%ld ppm\n",
        (long)(worst * 1.0e6));

  /* Now the same problem on the NPU, diffed against the CPU result. */

  ret = rk3576_rknn_matmul_set_io(ctx, a, b, c_npu);
  if (ret < 0)
    {
      goto out;
    }

  rk3576_rknn_matmul_set_backend(ctx, RK3576_RKNN_MM_BACKEND_NPU);

  ret = rk3576_rknn_matmul_run(ctx);
  if (ret < 0)
    {
      _warn("WARNING: NPU path not exercised (%d); the CPU reference is "
            "validated and usable\n",
            ret);
      ret = OK;
      goto out;
    }

  worst = 0.0;
  for (i = 0; i < (size_t)m * n; i++)
    {
      double diff = (double)c_npu[i] - (double)c_cpu[i];

      if (diff < 0.0)
        {
          diff = -diff;
        }

      if (diff > worst)
        {
          worst = diff;
        }
    }

  if (worst > RK3576_RKNN_MM_SELFTEST_EPS)
    {
      _err("ERROR: NPU result differs from the CPU reference, worst "
           "absolute error %ld ppm -- check the tile geometry and the "
           "register map\n",
           (long)(worst * 1.0e6));
      ret = -EIO;
      goto out;
    }

  _info("NPU matches the CPU reference, worst error %ld ppm\n",
        (long)(worst * 1.0e6));
  ret = OK;

out:
  rk3576_rknn_matmul_destroy(ctx);
  kmm_free(c_ref);
  kmm_free(c_npu);
  kmm_free(c_cpu);
  kmm_free(scale);
  kmm_free(b);
  kmm_free(a);
  return ret;
}

#endif /* CONFIG_RK3576_RKNN_MATMUL_SELFTEST */

#endif /* CONFIG_RK3576_RKNN_MATMUL */
