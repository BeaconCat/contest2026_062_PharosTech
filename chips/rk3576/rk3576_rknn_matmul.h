/****************************************************************************
 * vendor/rockchip/chips/rk3576/rk3576_rknn_matmul.h
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
 * Public interface of the RK3576 RKNN typed quantised matrix-multiply
 * submission layer.
 *
 * The RKNPU exposes exactly one useful primitive to a language-model
 * runtime: a *typed* matrix multiply, "A.dtype MM B.dtype TO acc.dtype".
 * Everything else in a transformer (RMSNorm, RoPE, softmax, sampling,
 * KV-cache) runs on the Cortex-A cores.  This module implements that single
 * primitive:
 *
 *   C[M,N] = A[M,K] x B[K,N]
 *
 * with the operand types enumerated by rk3576_rknn_matmul_type_e.  The hero
 * case for on-device LLM inference is FLOAT16_MM_INT4_TO_FLOAT32: fp16
 * activations multiplied by group-quantised int4 weights, accumulated in
 * fp32.
 *
 * Two back ends are provided behind one API:
 *
 *   NPU - packs the operands into the NPU tile ("native") layout, emits a
 *         register-command stream and submits it through the low-level
 *         rk3576_rknpu.c core driver.
 *   CPU - a straightforward scalar reference implementation.  It is both
 *         the functional fallback when the NPU path is not available and
 *         the golden reference used to validate NPU results.
 *
 * NuttX has no GEMM/accelerator subsystem, so this is an in-kernel C API
 * rather than a lower-half driver bound to a standard upper half.  All
 * hardware-visible buffers come from rk3576_dma_alloc().
 *
 * There is no user-space character device: the only consumer is the
 * in-kernel transformer runtime, and exporting large tensors through
 * ioctl() would force an extra copy for no benefit.
 ****************************************************************************/

#ifndef __VENDOR_ROCKCHIP_RK3576_RK3576_RKNN_MATMUL_H
#define __VENDOR_ROCKCHIP_RK3576_RK3576_RKNN_MATMUL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef CONFIG_RK3576_RKNN_MATMUL

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Back-end selection for rk3576_rknn_matmul_set_backend(). */

#define RK3576_RKNN_MM_BACKEND_AUTO 0 /* NPU when usable, else CPU        */
#define RK3576_RKNN_MM_BACKEND_NPU  1 /* NPU only, fail if unavailable    */
#define RK3576_RKNN_MM_BACKEND_CPU  2 /* Scalar reference only            */

/* Upper bound on the worker threads used by the CPU back end.  The RK3576
 * has 4x Cortex-A72 + 4x Cortex-A53.
 */

#define RK3576_RKNN_MM_MAX_THREADS  8

/* Sentinel for the register-command template patch indices below: the
 * field is absent from this template and must not be patched.
 */

#define RK3576_RKNN_MM_NOPATCH      0xffffffffu

/* Sentinel for an unknown register offset in
 * struct rk3576_rknn_matmul_regmap_s.  Entries left at this value are
 * skipped by the register-command builder.
 */

#define RK3576_RKNN_MM_REG_UNKNOWN  0xffffffffu

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Typed matrix-multiply operand/accumulator combinations.
 *
 * The numbering mirrors the public rknn_matmul_api.h enumeration so that a
 * model exported by the Rockchip tool chain can be consumed without a
 * translation table.
 *
 * TODO: the numeric values were taken from the public header of RKNPU2
 * 2.3.x and have not been cross-checked against the exact SDK revision we
 * ship.  Only the identifiers are used internally, so a mismatch affects
 * externally supplied type codes only.
 */

enum rk3576_rknn_matmul_type_e
{
  RK3576_RKNN_FLOAT16_MM_FLOAT16_TO_FLOAT32 = 1,
  RK3576_RKNN_INT8_MM_INT8_TO_INT32         = 2,
  RK3576_RKNN_INT8_MM_INT8_TO_FLOAT32       = 3,
  RK3576_RKNN_FLOAT16_MM_INT8_TO_FLOAT32    = 4,
  RK3576_RKNN_FLOAT16_MM_INT8_TO_FLOAT16    = 5,
  RK3576_RKNN_FLOAT16_MM_FLOAT16_TO_FLOAT16 = 6,
  RK3576_RKNN_INT8_MM_INT8_TO_INT8          = 7,
  RK3576_RKNN_FLOAT16_MM_INT4_TO_FLOAT32    = 8,
  RK3576_RKNN_FLOAT16_MM_INT4_TO_FLOAT16    = 9,
  RK3576_RKNN_INT4_MM_INT4_TO_INT16         = 10,
  RK3576_RKNN_INT8_MM_INT4_TO_INT32         = 11,
};

/* Memory layout of an operand.
 *
 *   NORM    - row major.  A is [M][K], B is [K][N], C is [M][N].
 *   NATIVE  - the tiled layout consumed directly by the NPU, see the
 *             "Tensor layout" comment block in rk3576_rknn_matmul.c.
 *   TP_NORM - transposed row major.  A is [K][M], B is [N][K],
 *             C is [N][M].  LLM weight matrices are commonly stored this
 *             way, which makes the K axis contiguous.
 */

enum rk3576_rknn_matmul_layout_e
{
  RK3576_RKNN_MM_LAYOUT_NORM    = 0,
  RK3576_RKNN_MM_LAYOUT_NATIVE  = 1,
  RK3576_RKNN_MM_LAYOUT_TP_NORM = 2,
};

/* Problem description passed to rk3576_rknn_matmul_create().
 *
 * M, K and N are the logical (unpadded) dimensions.  Padding of K and N to
 * the hardware alignment is performed internally; the caller never sees the
 * padded shape.
 */

struct rk3576_rknn_matmul_info_s
{
  uint32_t m;           /* Rows of A / rows of C                          */
  uint32_t k;           /* Columns of A / rows of B (reduction length)    */
  uint32_t n;           /* Columns of B / columns of C                    */
  int      type;        /* enum rk3576_rknn_matmul_type_e                 */
  int      a_layout;    /* enum rk3576_rknn_matmul_layout_e               */
  int      b_layout;    /* enum rk3576_rknn_matmul_layout_e               */
  int      ac_layout;   /* Layout of C (and of A when the NPU consumes    */
                        /* an already-native activation)                  */
  int      num_threads; /* CPU back-end worker threads, 0/1 = inline      */
};

/* Dequantisation parameters for an integer B operand.
 *
 * RKLLM-style weight quantisation is group-wise along the reduction axis:
 * every b_group consecutive K elements of one output column share a scale
 * and a zero point.  Element (k, n) therefore uses index
 *
 *   (k / b_group) * N + n
 *
 * into both arrays.  With b_group == 0 the whole K axis forms one group and
 * the index degenerates to n (classic per-channel quantisation).
 *
 * The dequantised weight is
 *
 *   w = (raw - zp) * scale
 *
 * where raw is sign-extended first when b_signed is true.
 */

struct rk3576_rknn_matmul_quant_s
{
  const float  *b_scale; /* [ngroups][N] scales, NULL means 1.0f          */
  const int8_t *b_zp;    /* [ngroups][N] zero points, NULL means 0        */
  uint32_t      b_group; /* K elements per group, 0 means the whole K     */
  bool          b_signed;/* Raw codes are two's complement (int4: -8..7)  */
};

/* Offsets, relative to the NPU core register block, of the fields the
 * register-command builder has to write for one matrix multiply.
 *
 * The RKNPU maps a GEMM onto its convolution pipeline as a 1x1 convolution:
 * A becomes the input feature map, B becomes the weight kernel and the DPU
 * writes C.  The *structure* of that command stream is understood; the
 * exact offsets for RK3576 are not published and are therefore left as
 * RK3576_RKNN_MM_REG_UNKNOWN until they are captured on hardware.  Install
 * measured offsets with rk3576_rknn_matmul_set_regmap() - no rebuild
 * required, which keeps the bring-up loop short.
 *
 * Any field still equal to RK3576_RKNN_MM_REG_UNKNOWN is omitted from the
 * generated stream; if that leaves the stream empty the NPU path reports
 * -ENOSYS and (in AUTO mode) the CPU reference runs instead.
 */

struct rk3576_rknn_matmul_regmap_s
{
  uint32_t op_enable;      /* Kick the operation                          */
  uint32_t cna_conv_con1;  /* Convolution mode: 1x1, no padding, group 1  */
  uint32_t cna_data_size;  /* Input feature map extent, encodes M and K   */
  uint32_t cna_weight_size;/* Weight kernel extent, encodes K and N       */
  uint32_t cna_data_base;  /* Physical base of the packed A operand       */
  uint32_t cna_weight_base;/* Physical base of the packed B operand       */
  uint32_t cna_data_type;  /* Input element type selector                 */
  uint32_t cna_weight_type;/* Weight element type selector                */
  uint32_t core_mac_gate;  /* MAC array enable mask                       */
  uint32_t dpu_feature_mode; /* Accumulator/output mode                   */
  uint32_t dpu_dst_base;   /* Physical base of the C output               */
  uint32_t dpu_dst_size;   /* Output extent, encodes M and N              */
  uint32_t dpu_out_cvt;    /* Output converter (accumulator -> C dtype)   */
};

/* A verbatim register-command stream captured from a known-good run,
 * together with the word indices that have to be patched with this
 * context's buffer addresses and shape.
 *
 * This is the fastest path to a first working submission: dump the regcmd
 * buffer that the vendor runtime hands to the kernel for one fp16 x int4
 * matmul, drop it in here, patch three addresses and replay it.  The same
 * "replay a golden trace, then generalise" method already brought up the
 * SDIO Wi-Fi companion on this board.
 *
 * Unused patch sites must be set to RK3576_RKNN_MM_NOPATCH.
 */

struct rk3576_rknn_matmul_template_s
{
  const uint32_t *words;  /* Register-command words                       */
  uint32_t nwords;        /* Number of words                              */
  uint32_t idx_a_addr;    /* Word index holding the A base address        */
  uint32_t idx_b_addr;    /* Word index holding the B base address        */
  uint32_t idx_c_addr;    /* Word index holding the C base address        */
  uint32_t idx_m;         /* Word index holding M, or NOPATCH             */
  uint32_t idx_k;         /* Word index holding the padded K, or NOPATCH  */
  uint32_t idx_n;         /* Word index holding the padded N, or NOPATCH  */
};

/* Opaque per-problem context. */

struct rk3576_rknn_matmul_ctx_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: rk3576_rknn_matmul_create
 *
 * Description:
 *   Validate a problem description, compute the padded shape and allocate
 *   the DMA-safe staging buffers for it.  The returned context is reusable:
 *   a decode step re-runs the same context with a new activation vector.
 *
 * Input Parameters:
 *   info - Problem description; copied into the context.
 *   ctx  - Receives the new context on success.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.  -ENOTSUP indicates a
 *   matmul type this module does not implement yet.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_create(const struct rk3576_rknn_matmul_info_s *info,
                              struct rk3576_rknn_matmul_ctx_s **ctx);

/****************************************************************************
 * Name: rk3576_rknn_matmul_destroy
 *
 * Description:
 *   Release a context and its staging buffers.  Passing NULL is a no-op.
 *
 ****************************************************************************/

void rk3576_rknn_matmul_destroy(struct rk3576_rknn_matmul_ctx_s *ctx);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_io
 *
 * Description:
 *   Bind the caller's operand buffers to a context.  The buffers are not
 *   copied and must stay valid until the next call to this function or
 *   until the context is destroyed; they need not be DMA-safe, the module
 *   stages them itself.
 *
 *   Re-binding the same B pointer keeps the previously packed weights, so
 *   the per-token cost of a decode step is one A pack plus one submission.
 *
 * Input Parameters:
 *   ctx - Context from rk3576_rknn_matmul_create().
 *   a   - A operand in info.a_layout.
 *   b   - B operand in info.b_layout.
 *   c   - C output in info.ac_layout; written by run().
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_set_io(struct rk3576_rknn_matmul_ctx_s *ctx,
                              const void *a, const void *b, void *c);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_quant
 *
 * Description:
 *   Install the dequantisation parameters for an integer B operand.  The
 *   arrays are referenced, not copied.  Without this call B is treated as
 *   symmetric with unit scale, which is what a plain integer GEMM wants.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_set_quant(struct rk3576_rknn_matmul_ctx_s *ctx,
                                 const struct rk3576_rknn_matmul_quant_s *q);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_backend
 *
 * Description:
 *   Force a back end.  Used by the validation path to run the same context
 *   twice, once on each back end, and diff the results.
 *
 * Input Parameters:
 *   ctx     - Context from rk3576_rknn_matmul_create().
 *   backend - One of RK3576_RKNN_MM_BACKEND_*.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_set_backend(struct rk3576_rknn_matmul_ctx_s *ctx,
                                   int backend);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_core_mask
 *
 * Description:
 *   Select which NPU cores may execute the submission.  RK3576 has two
 *   cores, so bit 0 and bit 1 are meaningful.  The default is core 0 only.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_set_core_mask(struct rk3576_rknn_matmul_ctx_s *ctx,
                                     uint32_t core_mask);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_template
 *
 * Description:
 *   Attach a captured register-command template to a context.  When set it
 *   replaces the synthesised command stream.  Pass NULL to go back to
 *   synthesis.  The template is referenced, not copied.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_set_template(
    struct rk3576_rknn_matmul_ctx_s *ctx,
    const struct rk3576_rknn_matmul_template_s *tpl);

/****************************************************************************
 * Name: rk3576_rknn_matmul_set_regmap
 *
 * Description:
 *   Install the chip-wide register offset map used by the command-stream
 *   builder.  Global rather than per-context: the offsets are a property of
 *   the silicon.  Pass NULL to restore the built-in (all unknown) map.
 *
 ****************************************************************************/

void rk3576_rknn_matmul_set_regmap(
    const struct rk3576_rknn_matmul_regmap_s *map);

/****************************************************************************
 * Name: rk3576_rknn_matmul_run
 *
 * Description:
 *   Execute C = A x B with the operands bound by set_io(), using the back
 *   end selected by set_backend().  In AUTO mode a failure to build or
 *   submit the NPU command stream silently degrades to the CPU reference.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_run(struct rk3576_rknn_matmul_ctx_s *ctx);

/****************************************************************************
 * Name: rk3576_rknn_matmul_run_cpu
 *
 * Description:
 *   Run the scalar reference implementation regardless of the selected back
 *   end.  Exposed so that a test can obtain golden values for the very
 *   buffers it just pushed through the NPU.
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_run_cpu(struct rk3576_rknn_matmul_ctx_s *ctx);

/****************************************************************************
 * Name: rk3576_rknn_matmul_query_align
 *
 * Description:
 *   Report the K and N alignment the hardware requires for a matmul type,
 *   so a caller that owns the weight layout can pad once at load time.
 *
 * Input Parameters:
 *   type    - enum rk3576_rknn_matmul_type_e.
 *   k_align - Receives the K alignment in elements; may be NULL.
 *   n_align - Receives the N alignment in elements; may be NULL.
 *
 * Returned Value:
 *   OK on success, -ENOTSUP for an unimplemented type.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_query_align(int type, uint32_t *k_align,
                                   uint32_t *n_align);

/****************************************************************************
 * Name: rk3576_rknn_fp16_to_fp32 / rk3576_rknn_fp32_to_fp16
 *
 * Description:
 *   IEEE 754 binary16 conversion helpers, exported because every caller of
 *   this module has to build fp16 activations.  Subnormals, infinities and
 *   NaNs are handled; fp32 -> fp16 rounds to nearest even.
 *
 ****************************************************************************/

float rk3576_rknn_fp16_to_fp32(uint16_t h);
uint16_t rk3576_rknn_fp32_to_fp16(float f);

#ifdef CONFIG_RK3576_RKNN_MATMUL_SELFTEST

/****************************************************************************
 * Name: rk3576_rknn_matmul_selftest
 *
 * Description:
 *   Run a small fp16 x int4 problem through both back ends and check them.
 *
 *   The CPU kernel is first compared against a double-precision
 *   recomputation that shares no code with it, which validates the int4
 *   nibble order, the sign extension and the group-wise dequantisation.
 *   The NPU path is then run on the same operands and diffed against the
 *   CPU result; when the NPU is not usable yet that step is reported and
 *   skipped rather than failed.
 *
 *   This is the intended first bring-up step on hardware.
 *
 * Returned Value:
 *   OK when everything that could be checked agreed, a negated errno
 *   otherwise.  -EIO means a back end produced wrong numbers.
 *
 ****************************************************************************/

int rk3576_rknn_matmul_selftest(void);

#endif /* CONFIG_RK3576_RKNN_MATMUL_SELFTEST */

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RK3576_RKNN_MATMUL */
#endif /* __VENDOR_ROCKCHIP_RK3576_RK3576_RKNN_MATMUL_H */
