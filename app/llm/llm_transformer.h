/****************************************************************************
 * app/llm/llm_transformer.h
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
 * On-device LLM inference engine for KICKPI-K7 (RK3576): public interface.
 *
 * Implements a complete decoder-only transformer of the LLaMA family
 * (LLaMA / TinyLlama / Qwen2 / Qwen2.5 all share the same block layout):
 *
 *   embedding -> N x { RMSNorm, QKV proj, RoPE, GQA attention with KV-cache,
 *                      output proj, RMSNorm, SwiGLU FFN } -> RMSNorm -> head
 *
 * Everything runs on the Cortex-A cores with NEON intrinsics and a pthread
 * worker pool.  The matrix-multiply kernel is the single hot spot and can be
 * switched at run time between the CPU kernel and the RKNPU offload
 * (rk3576_rknn_matmul_run), which lets the two be A/B compared on the same
 * model.  The engine is fully functional with the CPU backend alone, so it
 * does not depend on the NPU driver being present.
 *
 * Weights are int4 block-quantised (q4_0 layout: 32 values share one fp16
 * scale) and are referenced in place inside the single model image buffer;
 * they are never copied.  Activations are fp32 on the CPU path and are
 * converted to fp16 only when handed to the NPU.
 ****************************************************************************/

#ifndef __APP_LLM_LLM_TRANSFORMER_H
#define __APP_LLM_LLM_TRANSFORMER_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <nuttx/compiler.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Model container ---------------------------------------------------------
 *
 * File layout (all little-endian):
 *
 *   [0]                       struct llm_file_header_s  (128 bytes)
 *   [hdr.tensor_table_off]    n_tensors x llm_file_tensor_s (64 bytes each)
 *   [hdr.tokenizer_off]       tokenizer blob (see llm_tokenizer.c)
 *   [tensor.offset]           raw tensor payloads, 64-byte aligned
 */

#define LLM_MODEL_MAGIC       0x4d4c594eu /* "NYLM" */
#define LLM_MODEL_VERSION     1u
#define LLM_TENSOR_NAME_MAX   32
#define LLM_HEADER_BYTES      128
#define LLM_FILE_TENSOR_BYTES 64

/* llm_file_header_s.flags */

#define LLM_FLAG_TIE_EMBED (1u << 0) /* lm_head shares tok_emb        */
#define LLM_FLAG_QKV_BIAS  (1u << 1) /* Q/K/V projections have a bias */
#define LLM_FLAG_ROPE_NEOX (1u << 2) /* HF rotate_half RoPE layout    */

/* int4 block quantisation (q4_0 compatible) -------------------------------
 *
 * One block covers 32 consecutive values of a weight row:
 *
 *   uint16 d       fp16 scale
 *   uint8  qs[16]  low  nibble of qs[j] -> value j
 *                  high nibble of qs[j] -> value j + 16
 *
 * Dequantised value = ((int)nibble - 8) * d.
 */

#define LLM_Q4_BLOCK_SIZE  32
#define LLM_Q4_BLOCK_BYTES 18

/* Worker pool -------------------------------------------------------------*/

#define LLM_MAX_THREADS      8
#define LLM_WORKER_STACKSIZE 8192

/* Tokenizer blob ----------------------------------------------------------
 *
 *   uint32 magic       LLM_TOKENIZER_MAGIC
 *   uint32 vocab_size
 *   uint32 n_merges
 *   uint32 flags       LLM_TOKFLAG_*
 *   vocab_size x { uint16 len; uint8 bytes[len] }   raw byte strings
 *   n_merges   x { uint32 a; uint32 b; uint32 ab }  in ascending rank
 */

#define LLM_TOKENIZER_MAGIC 0x4b54594eu /* "NYTK" */
#define LLM_TOKFLAG_ADD_BOS (1u << 0)

/* NPU matmul operation type, mirrors RKNN_FLOAT16_MM_INT4_TO_FLOAT32. */

#define LLM_NPU_MM_F16_I4_F32 0u

/* NPU tile constraints.  A matmul is only offloaded when it satisfies them;
 * otherwise the CPU kernel runs.  Values are conservative and can be raised
 * once measured on silicon.
 */

#define LLM_NPU_K_ALIGN  32
#define LLM_NPU_N_ALIGN  16
#define LLM_NPU_MIN_WORK (256 * 256)

#define LLM_WEAK         __attribute__((weak))

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Storage format of a tensor. */

enum llm_dtype_e
{
  LLM_DTYPE_F32 = 0,
  LLM_DTYPE_F16 = 1,
  LLM_DTYPE_Q4_0 = 2,
};

/* Matmul backend selection. */

enum llm_backend_e
{
  LLM_BACKEND_CPU = 0,  /* never touch the NPU                           */
  LLM_BACKEND_NPU = 1,  /* prefer the NPU, same fallback as AUTO         */
  LLM_BACKEND_AUTO = 2, /* NPU when the shape is eligible, CPU otherwise */
};

/* On-disk header.  Packed so that the C view matches the file byte for
 * byte on any toolchain.
 */

begin_packed_struct struct llm_file_header_s
{
  uint32_t magic;
  uint32_t version;
  uint32_t arch;  /* 0 = llama-family                        */
  uint32_t flags; /* LLM_FLAG_*                              */
  int32_t hidden_dim;
  int32_t ffn_dim;
  int32_t n_layers;
  int32_t n_heads;
  int32_t n_kv_heads;
  int32_t head_dim;
  int32_t vocab_size;
  int32_t max_seq_len;
  float rope_theta;
  float rms_eps;
  uint32_t n_tensors;
  uint32_t tensor_table_off;
  uint32_t tensor_table_bytes;
  uint32_t tokenizer_off;
  uint32_t tokenizer_bytes;
  uint32_t bos_id;
  uint32_t eos_id;
  uint8_t reserved[44];
} end_packed_struct;

/* On-disk tensor directory entry. */

begin_packed_struct struct llm_file_tensor_s
{
  char name[LLM_TENSOR_NAME_MAX];
  uint32_t dtype; /* enum llm_dtype_e                        */
  uint32_t rows;  /* output features                         */
  uint32_t cols;  /* input features                          */
  uint32_t pad;
  uint64_t offset; /* payload offset from start of file       */
  uint64_t nbytes;
} end_packed_struct;

/* In-memory tensor view.  data points straight into the model image, so a
 * tensor costs nothing beyond this descriptor.  data == NULL means the
 * tensor is absent (optional biases, tied lm_head).
 */

struct llm_tensor_s
{
  const void *data;
  uint32_t rows;
  uint32_t cols;
  uint8_t dtype;
};

/* Hyper-parameters. */

struct llm_config_s
{
  int hidden_dim; /* model width                             */
  int ffn_dim;    /* SwiGLU intermediate width               */
  int n_layers;
  int n_heads;    /* query heads                             */
  int n_kv_heads; /* key/value heads (GQA), <= n_heads       */
  int head_dim;   /* per-head width                          */
  int q_dim;      /* n_heads    * head_dim                   */
  int kv_dim;     /* n_kv_heads * head_dim                   */
  int kv_mul;     /* n_heads / n_kv_heads                    */
  int vocab_size;
  int max_seq_len;
  float rope_theta;
  float rms_eps;
  bool tie_embed;
  bool qkv_bias;
  bool rope_neox;
};

/* One transformer block. */

struct llm_layer_s
{
  struct llm_tensor_s attn_norm; /* [hidden]                              */
  struct llm_tensor_s wq;        /* [q_dim , hidden]                      */
  struct llm_tensor_s wk;        /* [kv_dim, hidden]                      */
  struct llm_tensor_s wv;        /* [kv_dim, hidden]                      */
  struct llm_tensor_s wo;        /* [hidden, q_dim ]                      */
  struct llm_tensor_s bq;        /* [q_dim ] optional                     */
  struct llm_tensor_s bk;        /* [kv_dim] optional                     */
  struct llm_tensor_s bv;        /* [kv_dim] optional                     */
  struct llm_tensor_s ffn_norm;  /* [hidden]                              */
  struct llm_tensor_s w_gate;    /* [ffn , hidden]                        */
  struct llm_tensor_s w_up;      /* [ffn , hidden]                        */
  struct llm_tensor_s w_down;    /* [hidden, ffn ]                        */
};

/* Whole-model weights. */

struct llm_weights_s
{
  struct llm_tensor_s tok_emb;  /* [vocab , hidden]                     */
  struct llm_tensor_s out_norm; /* [hidden]                             */
  struct llm_tensor_s output;   /* [vocab , hidden], tied -> tok_emb    */
  struct llm_layer_s *layers;
};

/* Scratch activations plus the KV-cache.  Everything here is allocated once
 * at load time; the decode loop performs no allocation at all.
 */

struct llm_state_s
{
  float *x;           /* [hidden]  residual stream                       */
  float *xb;          /* [max(hidden,q_dim)] scratch                     */
  float *xb2;         /* [max(hidden,q_dim)] scratch                     */
  float *hb;          /* [ffn]     SwiGLU gate                           */
  float *hb2;         /* [ffn]     SwiGLU up                             */
  float *q;           /* [q_dim]                                         */
  float *att;         /* [n_heads * max_seq_len]                         */
  float *logits;      /* [vocab]                                         */
  float *key_cache;   /* [layers * max_seq_len * kv_dim]                 */
  float *value_cache; /* [layers * max_seq_len * kv_dim]                 */
  float *rope_freq;   /* [head_dim / 2] inverse frequencies              */
  uint16_t *npu_act;  /* [max activation width] fp16 staging, DMA heap   */
  float *npu_out;     /* [max output width]     fp32 staging, DMA heap   */
  size_t npu_act_bytes;
  size_t npu_out_bytes;
};

/* Worker pool used to split a matmul row range across CPU cores. */

struct llm_pool_s;

struct llm_worker_arg_s
{
  struct llm_pool_s *pool;
  int id;
};

struct llm_pool_s
{
  int nthreads; /* including the calling thread    */
  bool running;
  volatile bool quit;
  pthread_t tid[LLM_MAX_THREADS];
  sem_t start[LLM_MAX_THREADS];
  sem_t done;
  struct llm_worker_arg_s arg[LLM_MAX_THREADS];

  /* Current job, published before the start semaphores are posted. */

  float *job_out;
  const float *job_x;
  const struct llm_tensor_s *job_w;
};

/* Byte-level BPE tokenizer. */

struct llm_bpe_entry_s
{
  const uint8_t *bytes; /* points into the model image                  */
  uint16_t len;
};

struct llm_merge_slot_s
{
  uint64_t key; /* (a << 32) | b, 0 = empty                     */
  uint32_t rank;
  uint32_t result;
  bool used;
};

struct llm_tokenizer_s
{
  struct llm_bpe_entry_s *vocab;
  uint32_t vocab_size;
  uint32_t n_merges;
  struct llm_merge_slot_s *table; /* open-addressed merge hash          */
  uint32_t mask;                  /* table size - 1                     */
  int32_t byte_tok[256];
  uint32_t bos_id;
  uint32_t eos_id;
  bool add_bos;
};

/* Sampler. */

struct llm_prob_s
{
  float p;
  uint32_t index;
};

struct llm_sampler_s
{
  float temperature;
  float topp;
  int topk;
  uint64_t rng;
  int vocab_size;
  struct llm_prob_s *cand;
};

/* Everything needed to run the model. */

struct llm_model_s
{
  struct llm_config_s cfg;
  struct llm_weights_s w;
  struct llm_state_s s;
  struct llm_pool_s pool;
  struct llm_tokenizer_s tok;

  void *image; /* whole model file, weights point into it   */
  size_t image_bytes;

  enum llm_backend_e backend;
  uint32_t npu_calls; /* matmuls actually run on the NPU           */
  uint32_t cpu_calls; /* matmuls run on the CPU kernel             */
};

/* Generation parameters for llm_generate(). */

struct llm_genopt_s
{
  const char *prompt;
  int max_new; /* number of tokens to decode                */
  bool chat;   /* wrap the prompt in the ChatML template    */
  bool quiet;  /* suppress token streaming                  */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* NPU offload -------------------------------------------------------------
 *
 * Provided by the RKNPU driver (chips/rk3576).  Declared weak so that the
 * engine links and runs without it; the pointer is NULL-tested before use.
 *
 *   C[m][n] = A[m][k] x B[k][n]
 *   type    - LLM_NPU_MM_F16_I4_F32
 *   a       - fp16 activations, m*k elements, row major, DMA-safe
 *   b       - int4 weights in q4_0 blocks laid out as n rows of k values
 *             (i.e. the transpose of the mathematical B, which is exactly
 *             the row-major nn.Linear weight and the layout RKNN calls the
 *             native/perf layout), DMA-safe
 *   c       - fp32 result, m*n elements, row major, DMA-safe
 *
 * The driver owns cache maintenance of any buffer it programs into the
 * hardware; the engine cleans/invalidates its own staging buffers as well.
 */

int rk3576_rknn_matmul_run(uint32_t type, const void *a, const void *b,
                           void *c, uint32_t m, uint32_t k,
                           uint32_t n) LLM_WEAK;

/* DMA-safe allocator (chips/rk3576/rk3576_dma_alloc.h).  The chip header is
 * not on the application include path, so the prototypes are mirrored here;
 * they must stay identical to the ones in that header.  Weak, because the
 * CPU-only build does not need them.
 */

void *rk3576_dma_alloc(size_t size) LLM_WEAK;
void rk3576_dma_free(void *memory, size_t size) LLM_WEAK;

/* Model lifecycle ---------------------------------------------------------*/

/****************************************************************************
 * Name: llm_model_load
 *
 * Description:
 *   Read a .nylm model image from path, wire up the tensor views, allocate
 *   the activation scratch and the KV-cache, build the tokenizer and start
 *   the worker pool.
 *
 * Input Parameters:
 *   model    - Caller-provided, zero-initialised model structure
 *   path     - Path of the model image
 *   nthreads - Worker threads (1..LLM_MAX_THREADS), 0 selects the default
 *   maxseq   - Clamp the KV-cache to this many tokens, 0 uses the model max
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int llm_model_load(struct llm_model_s *model, const char *path, int nthreads,
                   int maxseq);

/****************************************************************************
 * Name: llm_model_free
 *
 * Description:
 *   Stop the worker pool and release every buffer owned by the model.
 *
 ****************************************************************************/

void llm_model_free(struct llm_model_s *model);

/****************************************************************************
 * Name: llm_forward
 *
 * Description:
 *   Run one token through the whole network and leave the logits in
 *   model->s.logits.
 *
 * Input Parameters:
 *   model - Loaded model
 *   token - Input token id
 *   pos   - Absolute position of this token, 0-based
 *
 * Returned Value:
 *   Pointer to the logits array, or NULL on error.
 *
 ****************************************************************************/

float *llm_forward(struct llm_model_s *model, int token, int pos);

/****************************************************************************
 * Name: llm_generate
 *
 * Description:
 *   Tokenize the prompt, run the prefill, then decode autoregressively,
 *   streaming the detokenised text to stdout and printing throughput.
 *
 * Returned Value:
 *   Number of tokens generated, or a negated errno on failure.
 *
 ****************************************************************************/

int llm_generate(struct llm_model_s *model, struct llm_sampler_s *sampler,
                 const struct llm_genopt_s *opt);

/* Backend selection -------------------------------------------------------*/

void llm_set_backend(struct llm_model_s *model, enum llm_backend_e backend);
const char *llm_backend_name(enum llm_backend_e backend);
bool llm_npu_available(void);

/* Kernels -----------------------------------------------------------------*/

void llm_rmsnorm(float *out, const float *x, const float *weight, int size,
                 float eps);
void llm_softmax(float *x, int size);
void llm_rope(struct llm_model_s *model, float *q, float *k, int pos);
void llm_swiglu(float *gate, const float *up, int size);
void llm_attention(struct llm_model_s *model, int layer, int pos);
void llm_matmul(struct llm_model_s *model, float *out, const float *x,
                const struct llm_tensor_s *w);
void llm_matmul_cpu_rows(float *out, const float *x,
                         const struct llm_tensor_s *w, int row_begin,
                         int row_end);

/* Numeric helpers ---------------------------------------------------------*/

float llm_fp16_to_fp32(uint16_t h);
uint16_t llm_fp32_to_fp16(float f);
void llm_fp32_to_fp16_vec(uint16_t *out, const float *in, int n);

/* Tokenizer (llm_tokenizer.c) ---------------------------------------------*/

int llm_tokenizer_init(struct llm_tokenizer_s *tok, const uint8_t *blob,
                       size_t blob_bytes, uint32_t bos_id, uint32_t eos_id);
void llm_tokenizer_free(struct llm_tokenizer_s *tok);

/****************************************************************************
 * Name: llm_tokenizer_encode
 *
 * Description:
 *   Byte-level BPE encode.  When parse_special is true, "<|...|>" markers in
 *   the text are looked up as whole vocabulary entries instead of being
 *   split, which is what the ChatML template needs.
 *
 * Input Parameters:
 *   tok           - Initialised tokenizer
 *   text          - NUL-terminated UTF-8 input
 *   parse_special - Honour "<|...|>" special tokens
 *   out           - Caller buffer for the token ids
 *   max_out       - Capacity of out
 *
 * Returned Value:
 *   Number of tokens written, or a negated errno.
 *
 ****************************************************************************/

int llm_tokenizer_encode(struct llm_tokenizer_s *tok, const char *text,
                         bool parse_special, int *out, int max_out);

/****************************************************************************
 * Name: llm_tokenizer_decode
 *
 * Description:
 *   Return the raw byte string of one token.  The pointer aliases the model
 *   image and stays valid for the lifetime of the model.
 *
 ****************************************************************************/

const uint8_t *llm_tokenizer_decode(struct llm_tokenizer_s *tok, int token,
                                    int *len);

int llm_tokenizer_lookup(struct llm_tokenizer_s *tok, const char *s, int len);

/* Sampler (llm_sample.c) --------------------------------------------------*/

int llm_sampler_init(struct llm_sampler_s *sampler, int vocab_size,
                     float temperature, int topk, float topp, uint64_t seed);
void llm_sampler_free(struct llm_sampler_s *sampler);
int llm_sample(struct llm_sampler_s *sampler, float *logits);

#endif /* __APP_LLM_LLM_TRANSFORMER_H */
