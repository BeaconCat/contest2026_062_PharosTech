/****************************************************************************
 * apps/contest2026_062_llm/rkllm_format.h
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
 * LLM model container reader.  Two on-disk containers are supported:
 *
 *   1. ".nllm"  - the native Nyabula container.  Fully specified here and
 *                 produced by tools/convert_to_nllm.py.  This is the
 *                 fallback path we control end to end.
 *   2. ".rkllm" - the Rockchip RKLLM container.  Its layout is *inferred*
 *                 (clean-room, from public headers and the runtime's
 *                 observable behaviour); every inferred field is marked
 *                 with a TODO describing how to confirm it against a real
 *                 file.  Parsing degrades gracefully: an unknown revision
 *                 is rejected instead of mis-parsed.
 *
 * Both containers expose the same accessor API so the transformer engine
 * does not care which one it was handed.
 ****************************************************************************/

#ifndef __APPS_CONTEST2026_062_LLM_RKLLM_FORMAT_H
#define __APPS_CONTEST2026_062_LLM_RKLLM_FORMAT_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Container magics (little endian, read as a 4 byte string) */

#define NLLM_MAGIC     "NLLM"
#define NLLM_MAGIC_LEN 4
#define NLLM_VERSION   1

/* Tokenizer sub-section magic inside a .nllm file */

#define NLLM_TOKENIZER_MAGIC "NTOK"

/* Fixed on-disk record sizes.  These are part of the .nllm ABI: never
 * change them, bump NLLM_VERSION and add a new record instead.
 */

#define NLLM_HEADER_SIZE       256
#define NLLM_TENSOR_ENTRY_SIZE 128
#define NLLM_TENSOR_NAME_MAX   64
#define NLLM_MAX_DIMS          4

/* Data section alignment.  64 bytes matches the RK3576 cache line and the
 * alignment guarantee of rk3576_dma_alloc(), so a tensor copied into DMA
 * memory keeps its intra-tensor alignment.
 */

#define NLLM_DATA_ALIGN 64

/* Tensor element types */

#define NLLM_DTYPE_F32     0 /* IEEE754 binary32                     */
#define NLLM_DTYPE_F16     1 /* IEEE754 binary16                     */
#define NLLM_DTYPE_I8      2 /* int8, per-group symmetric scale      */
#define NLLM_DTYPE_I4_SYM  3 /* int4 packed, per-group symmetric     */
#define NLLM_DTYPE_I4_ASYM 4 /* int4 packed, per-group scale + zero  */
#define NLLM_DTYPE_U8      5 /* raw bytes (tokenizer blobs, etc.)    */
#define NLLM_DTYPE_I32     6 /* int32                                */

/* Model architecture identifiers */

#define NLLM_ARCH_UNKNOWN 0
#define NLLM_ARCH_LLAMA   1 /* TinyLlama, Llama-2/3 family          */
#define NLLM_ARCH_QWEN2   2 /* Qwen2 / Qwen2.5                      */

/* Header flags */

#define NLLM_FLAG_TIED_EMBED (1 << 0) /* lm_head shares tok_embeddings */
#define NLLM_FLAG_QKV_BIAS   (1 << 1) /* Q/K/V projections have bias   */

/* Open flags for rkllm_open() */

#define RKLLM_OPEN_PRELOAD (1 << 0) /* slurp whole file into RAM     */
#define RKLLM_OPEN_LAZY    0        /* read tensors on first use     */

/* Inferred RKLLM container constants.
 *
 * TODO: confirm against a real model.  Verification recipe:
 *   $ hexdump -C model.rkllm | head -16
 * The runtime string "invalid rknn llm model magic" proves a magic exists
 * at offset 0.  Both candidate spellings below are accepted; whichever one
 * matches is reported through rkllm_container_kind().  Once observed on
 * hardware, delete the loser and drop this comment.
 */

#define RKLLM_MAGIC_A "RKLL"
#define RKLLM_MAGIC_B "RKNN"

/* Largest header we are willing to believe, used as a sanity clamp so a
 * corrupt length cannot drive a huge allocation.
 */

#define RKLLM_HEADER_SIZE_MAX (64 * 1024)

/* Upper bound on the tensor table, same rationale */

#define RKLLM_MAX_TENSORS 4096

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Which container was recognised */

enum rkllm_kind_e
{
  RKLLM_KIND_NONE = 0,
  RKLLM_KIND_NLLM, /* native .nllm                                */
  RKLLM_KIND_RKLLM /* Rockchip .rkllm (inferred layout)           */
};

/* Model hyper-parameters.  Mirrors the fields the transformer engine needs
 * (see llm_transformer.h struct llm_model_s); rkllm_get_config() returns a
 * pointer to the copy owned by the container context.
 */

struct llm_config_s
{
  uint32_t arch;        /* NLLM_ARCH_*                                 */
  uint32_t n_layer;     /* transformer blocks                          */
  uint32_t n_embd;      /* hidden size                                 */
  uint32_t n_head;      /* attention heads                             */
  uint32_t n_kv_head;   /* KV heads (GQA); == n_head when MHA          */
  uint32_t head_dim;    /* per-head width, usually n_embd / n_head     */
  uint32_t n_ff;        /* MLP intermediate size                       */
  uint32_t vocab_size;  /* token count                                 */
  uint32_t max_seq_len; /* context window the weights were built for   */
  float rope_theta;     /* RoPE base frequency                         */
  float rms_eps;        /* RMSNorm epsilon                             */
  int32_t bos_token_id;
  int32_t eos_token_id;
  int32_t pad_token_id;
  uint32_t quant_type; /* dominant NLLM_DTYPE_* of the weights        */
  uint32_t group_size; /* quantisation group, 32 or 64                */
  uint32_t flags;      /* NLLM_FLAG_*                                 */
};

/* A single weight tensor.
 *
 * For the quantised types the payload is a single contiguous blob laid out
 * as:  [packed quants][scales fp16][zeros fp16 (asym only)]
 * and the three sub-pointers below point into it.  For the float types
 * only 'data' is meaningful.
 */

struct llm_tensor_s
{
  char name[NLLM_TENSOR_NAME_MAX];
  uint32_t dtype; /* NLLM_DTYPE_*                  */
  uint32_t ndim;
  uint32_t dims[NLLM_MAX_DIMS]; /* row major, dims[0] = slowest  */
  uint64_t nelem;               /* product of dims               */
  uint32_t group_size;          /* 0 when not quantised          */
  uint32_t ngroups;             /* nelem / group_size            */

  const void *data;       /* whole payload                 */
  size_t size;            /* payload byte count            */
  const uint8_t *qdata;   /* packed quants (quantised only)*/
  const uint16_t *scales; /* fp16 per group                */
  const uint16_t *zeros;  /* fp16 per group, asym only     */
};

/* Tokenizer view.  The .nllm tokenizer section is a byte-level BPE / unigram
 * vocabulary: a fixed table of (offset, length, score) plus a string pool.
 */

struct llm_tokenizer_s
{
  uint32_t n_tokens;
  int32_t unk_id;
  int32_t bos_id;
  int32_t eos_id;
  int32_t pad_id;

  const uint32_t *offsets; /* n_tokens entries, byte offset into pool     */
  const uint32_t *lengths; /* n_tokens entries                            */
  const float *scores;     /* n_tokens entries, may be NULL               */
  const char *pool;        /* raw UTF-8 (or byte-fallback) string pool    */
  size_t pool_size;
};

/* Opaque container context */

struct rkllm_ctx_s;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Name: rkllm_open
 *
 * Description:
 *   Open a model container.  The container kind is detected from the file
 *   magic, not from the file name extension.
 *
 * Input Parameters:
 *   path  - path to a .nllm or .rkllm file
 *   flags - RKLLM_OPEN_PRELOAD to read the whole file into the heap, or
 *           RKLLM_OPEN_LAZY to read tensor payloads on first access
 *   ctx   - receives the new context on success
 *
 * Returned Value:
 *   OK on success, a negated errno value on failure.
 *
 ****************************************************************************/

int rkllm_open(const char *path, uint32_t flags, struct rkllm_ctx_s **ctx);

/****************************************************************************
 * Name: rkllm_close
 *
 * Description:
 *   Release every resource owned by the context.  Pointers previously
 *   handed out by rkllm_get_tensor()/rkllm_get_tokenizer() become invalid.
 *
 ****************************************************************************/

void rkllm_close(struct rkllm_ctx_s *ctx);

/****************************************************************************
 * Name: rkllm_get_config
 *
 * Description:
 *   Return the parsed hyper-parameters, or NULL if ctx is invalid.  The
 *   returned pointer is owned by the context.
 *
 ****************************************************************************/

const struct llm_config_s *rkllm_get_config(struct rkllm_ctx_s *ctx);

/****************************************************************************
 * Name: rkllm_get_tensor
 *
 * Description:
 *   Look a tensor up by name (for example "blk.12.attn_q.weight") and fill
 *   in the caller supplied descriptor.  In lazy mode the payload is read
 *   from the file on the first request and cached for the lifetime of the
 *   context.
 *
 * Returned Value:
 *   OK, -ENOENT when the name is unknown, or another negated errno.
 *
 ****************************************************************************/

int rkllm_get_tensor(struct rkllm_ctx_s *ctx, const char *name,
                     struct llm_tensor_s *tensor);

/****************************************************************************
 * Name: rkllm_get_layer_tensor
 *
 * Description:
 *   Convenience wrapper building the canonical "blk.<layer>.<suffix>" name.
 *
 ****************************************************************************/

int rkllm_get_layer_tensor(struct rkllm_ctx_s *ctx, unsigned int layer,
                           const char *suffix, struct llm_tensor_s *tensor);

/****************************************************************************
 * Name: rkllm_get_tokenizer
 *
 * Description:
 *   Fill in a view of the tokenizer section.  Returns -ENOENT when the
 *   container carries no tokenizer.
 *
 ****************************************************************************/

int rkllm_get_tokenizer(struct rkllm_ctx_s *ctx, struct llm_tokenizer_s *tok);

/****************************************************************************
 * Name: rkllm_tensor_count / rkllm_tensor_at
 *
 * Description:
 *   Enumerate the tensor table without knowing the names in advance.
 *
 ****************************************************************************/

int rkllm_tensor_count(struct rkllm_ctx_s *ctx);
int rkllm_tensor_at(struct rkllm_ctx_s *ctx, int index,
                    struct llm_tensor_s *tensor);

/****************************************************************************
 * Name: rkllm_container_kind
 *
 * Description:
 *   Report which container was detected (enum rkllm_kind_e).
 *
 ****************************************************************************/

int rkllm_container_kind(struct rkllm_ctx_s *ctx);

/****************************************************************************
 * Name: rkllm_fp16_to_fp32 / rkllm_dequant_group
 *
 * Description:
 *   Small numeric helpers shared with the transformer kernels.
 *   rkllm_dequant_group() expands one quantisation group of a
 *   NLLM_DTYPE_I4_* / NLLM_DTYPE_I8 tensor into float.
 *
 ****************************************************************************/

float rkllm_fp16_to_fp32(uint16_t h);
int rkllm_dequant_group(const struct llm_tensor_s *tensor, uint32_t group,
                        float *out, size_t outlen);

#ifdef __cplusplus
}
#endif

#endif /* __APPS_CONTEST2026_062_LLM_RKLLM_FORMAT_H */
