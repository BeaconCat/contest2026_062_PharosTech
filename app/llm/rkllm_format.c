/****************************************************************************
 * apps/contest2026_062_llm/rkllm_format.c
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
 * Reader for the native ".nllm" model container and for the Rockchip
 * ".rkllm" container.  Only the .nllm layout is authoritative; the .rkllm
 * layout is inferred and every guess carries a TODO with the hexdump check
 * that confirms or kills it.  All multi-byte fields are little endian,
 * which matches both the RK3576 and the x86 host that writes the files, so
 * the on-disk records are read by direct structure overlay.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/compiler.h>

#include <debug.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "rkllm_format.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define RKLLM_NAME_FMT_MAX 96 /* "blk.<n>.<suffix>" scratch buffer */

/* This is an application, not a driver, so it has no CONFIG_DEBUG_<subsys>
 * knob of its own.  Route the diagnostics through syslog at the usual
 * severities.
 */

#define llmerr(fmt, ...)  syslog(LOG_ERR, "llm: " fmt, ##__VA_ARGS__)
#define llmwarn(fmt, ...) syslog(LOG_WARNING, "llm: " fmt, ##__VA_ARGS__)
#define llminfo(fmt, ...) syslog(LOG_INFO, "llm: " fmt, ##__VA_ARGS__)

/* Inferred .rkllm record sizes.  TODO: confirm with a real model, see the
 * verification note at the top of rkllm_parse_rkllm().
 */

#define RKLLM_HDR_SIZE_MIN   sizeof(struct rkllm_disk_hdr_s)
#define RKLLM_ENTRY_NAME_MAX 64
#define RKLLM_ENTRY_SIZE_MIN sizeof(struct rkllm_disk_tensor_s)

/* Inferred .rkllm dtype codes.  They follow the RKNN tensor type ordering
 * used by rknn_matmul_api (float32, float16, int8, int4, uint8), which is
 * the only public enumeration the runtime is known to consume.
 * TODO: confirm; a mis-mapped dtype shows up immediately as garbage logits.
 */

#define RKLLM_DT_FLOAT32 0
#define RKLLM_DT_FLOAT16 1
#define RKLLM_DT_INT8    2
#define RKLLM_DT_INT4    3
#define RKLLM_DT_UINT8   4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* ---- .nllm on-disk records (authoritative, see convert_to_nllm.py) ---- */

begin_packed_struct struct nllm_disk_hdr_s
{
  char magic[NLLM_MAGIC_LEN]; /* "NLLM"                             */
  uint32_t version;
  uint32_t header_size; /* == NLLM_HEADER_SIZE for version 1  */
  uint32_t flags;       /* NLLM_FLAG_*                        */

  uint32_t arch;
  uint32_t n_layer;
  uint32_t n_embd;
  uint32_t n_head;
  uint32_t n_kv_head;
  uint32_t head_dim;
  uint32_t n_ff;
  uint32_t vocab_size;
  uint32_t max_seq_len;

  float rope_theta;
  float rms_eps;

  int32_t bos_token_id;
  int32_t eos_token_id;
  int32_t pad_token_id;

  uint32_t quant_type;
  uint32_t group_size;

  uint32_t n_tensors;
  uint32_t reserved0;

  uint64_t tensor_table_off;
  uint64_t tensor_table_size;
  uint64_t tokenizer_off;
  uint64_t tokenizer_size;
  uint64_t data_off;
  uint64_t data_size;
  uint64_t file_size;

  uint32_t hdr_crc32; /* CRC32 of bytes [0, 144)            */
  uint8_t reserved1[108];
} end_packed_struct;

begin_packed_struct struct nllm_disk_tensor_s
{
  char name[NLLM_TENSOR_NAME_MAX];
  uint32_t dtype;
  uint32_t ndim;
  uint32_t dims[NLLM_MAX_DIMS];
  uint64_t nelem;
  uint32_t group_size;
  uint32_t ngroups;
  uint64_t data_off;  /* absolute file offset               */
  uint64_t data_size; /* quants + scales (+ zeros)          */
  uint32_t scale_off; /* relative to data_off, 0 if none    */
  uint32_t zero_off;  /* relative to data_off, 0 if none    */
} end_packed_struct;

begin_packed_struct struct nllm_disk_tok_s
{
  char magic[NLLM_MAGIC_LEN]; /* "NTOK"                             */
  uint32_t version;
  uint32_t n_tokens;
  uint32_t flags;

  int32_t unk_id;
  int32_t bos_id;
  int32_t eos_id;
  int32_t pad_id;

  uint32_t off_offsets; /* all offsets relative to section    */
  uint32_t off_lengths;
  uint32_t off_scores; /* 0 when the model has no scores     */
  uint32_t off_pool;
  uint32_t pool_size;
  uint32_t reserved0;
  uint64_t reserved1;
} end_packed_struct;

/* ---- .rkllm on-disk records (INFERRED) ---- */

begin_packed_struct struct rkllm_disk_hdr_s
{
  char magic[4]; /* "RKLL" or "RKNN", see TODO         */
  uint32_t version;
  uint32_t header_size;
  uint32_t model_type;

  uint64_t config_off;
  uint64_t config_size;
  uint64_t tensor_table_off;
  uint64_t tensor_table_size;
  uint32_t n_tensors;
  uint32_t tensor_entry_size;
  uint64_t tokenizer_off;
  uint64_t tokenizer_size;
  uint64_t data_off;
  uint64_t data_size;
} end_packed_struct;

begin_packed_struct struct rkllm_disk_tensor_s
{
  char name[RKLLM_ENTRY_NAME_MAX];
  uint32_t dtype;
  uint32_t ndim;
  uint32_t dims[NLLM_MAX_DIMS];
  uint64_t data_off;
  uint64_t data_size;
} end_packed_struct;

/* Inferred .rkllm hyper-parameter block.  The field set is the minimum a
 * decoder-only transformer runtime must know and matches the knobs the
 * public rkllm.h exposes (context length, layer/head geometry, RoPE).
 * TODO: confirm ordering; a wrong order is obvious because n_layer and
 * n_embd land on implausible values.
 */

begin_packed_struct struct rkllm_disk_cfg_s
{
  uint32_t n_layer;
  uint32_t n_embd;
  uint32_t n_head;
  uint32_t n_kv_head;
  uint32_t head_dim;
  uint32_t n_ff;
  uint32_t vocab_size;
  uint32_t max_context_len;
  float rope_theta;
  float rms_eps;
  int32_t bos_token_id;
  int32_t eos_token_id;
  uint32_t quant_bits;
  uint32_t group_size;
} end_packed_struct;

/* ---- runtime state ---- */

struct rkllm_slot_s
{
  struct nllm_disk_tensor_s rec; /* normalised tensor record           */
  uint32_t scale_off;
  uint32_t zero_off;
  void *cache; /* lazily read payload, may be NULL   */
};

struct rkllm_ctx_s
{
  int fd;
  int kind; /* enum rkllm_kind_e                  */
  off_t filesize;

  uint8_t *image; /* whole file, preload mode only      */

  struct llm_config_s config;

  int ntensors;
  struct rkllm_slot_s *slots;

  uint8_t *tokblob; /* tokenizer section, always in RAM   */
  size_t toksize;
  bool has_tok;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int rkllm_read_at(int fd, void *dst, size_t len, off_t off);
static uint64_t rkllm_dims_product(const uint32_t *dims, uint32_t ndim);
static size_t rkllm_dtype_bits(uint32_t dtype);
static int rkllm_parse_nllm(struct rkllm_ctx_s *ctx);
static int rkllm_parse_rkllm(struct rkllm_ctx_s *ctx);
static int rkllm_map_rkllm_dtype(uint32_t dtype, uint32_t group_size);
static int rkllm_slot_payload(struct rkllm_ctx_s *ctx,
                              struct rkllm_slot_s *slot, const void **payload);
static int rkllm_fill_tensor(struct rkllm_ctx_s *ctx, int index,
                             struct llm_tensor_s *tensor);
static void rkllm_free_slots(struct rkllm_ctx_s *ctx);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rkllm_read_at
 *
 * Description:
 *   Positioned full read.  Short reads are retried; EOF is an error.
 *
 ****************************************************************************/

static int rkllm_read_at(int fd, void *dst, size_t len, off_t off)
{
  uint8_t *p = (uint8_t *)dst;
  size_t done = 0;

  if (lseek(fd, off, SEEK_SET) != off)
    {
      return -errno;
    }

  while (done < len)
    {
      ssize_t n = read(fd, p + done, len - done);

      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      if (n == 0)
        {
          return -EIO;
        }

      done += (size_t)n;
    }

  return OK;
}

/****************************************************************************
 * Name: rkllm_dims_product
 ****************************************************************************/

static uint64_t rkllm_dims_product(const uint32_t *dims, uint32_t ndim)
{
  uint64_t n = 1;
  uint32_t i;

  if (ndim == 0 || ndim > NLLM_MAX_DIMS)
    {
      return 0;
    }

  for (i = 0; i < ndim; i++)
    {
      if (dims[i] == 0)
        {
          return 0;
        }

      n *= (uint64_t)dims[i];
    }

  return n;
}

/****************************************************************************
 * Name: rkllm_dtype_bits
 *
 * Description:
 *   Bits per element of the packed quant/float payload, excluding the
 *   per-group scale and zero arrays.  Returns 0 for unknown types.
 *
 ****************************************************************************/

static size_t rkllm_dtype_bits(uint32_t dtype)
{
  switch (dtype)
    {
      case NLLM_DTYPE_F32:
      case NLLM_DTYPE_I32:
        return 32;

      case NLLM_DTYPE_F16:
        return 16;

      case NLLM_DTYPE_I8:
      case NLLM_DTYPE_U8:
        return 8;

      case NLLM_DTYPE_I4_SYM:
      case NLLM_DTYPE_I4_ASYM:
        return 4;

      default:
        return 0;
    }
}

/****************************************************************************
 * Name: rkllm_parse_nllm
 *
 * Description:
 *   Parse the native container.  Header, tensor table and tokenizer are
 *   always brought into RAM; the weight payloads follow the open mode.
 *
 ****************************************************************************/

static int rkllm_parse_nllm(struct rkllm_ctx_s *ctx)
{
  struct nllm_disk_hdr_s hdr;
  struct nllm_disk_tensor_s *table = NULL;
  size_t tablebytes;
  int ret;
  int i;

  ret = rkllm_read_at(ctx->fd, &hdr, sizeof(hdr), 0);
  if (ret < 0)
    {
      return ret;
    }

  if (hdr.version != NLLM_VERSION)
    {
      llmerr("ERROR: unsupported .nllm version %" PRIu32 "\n", hdr.version);
      return -ENOTSUP;
    }

  if (hdr.header_size != NLLM_HEADER_SIZE)
    {
      llmerr("ERROR: bad .nllm header size %" PRIu32 "\n", hdr.header_size);
      return -EINVAL;
    }

  if (hdr.n_tensors == 0 || hdr.n_tensors > RKLLM_MAX_TENSORS)
    {
      llmerr("ERROR: implausible tensor count %" PRIu32 "\n", hdr.n_tensors);
      return -EINVAL;
    }

  tablebytes = (size_t)hdr.n_tensors * NLLM_TENSOR_ENTRY_SIZE;
  if (hdr.tensor_table_size < tablebytes ||
      (off_t)(hdr.tensor_table_off + tablebytes) > ctx->filesize)
    {
      llmerr("ERROR: tensor table out of file bounds\n");
      return -EINVAL;
    }

  ctx->config.arch = hdr.arch;
  ctx->config.n_layer = hdr.n_layer;
  ctx->config.n_embd = hdr.n_embd;
  ctx->config.n_head = hdr.n_head;
  ctx->config.n_kv_head = hdr.n_kv_head != 0 ? hdr.n_kv_head : hdr.n_head;
  ctx->config.head_dim = hdr.head_dim != 0
                             ? hdr.head_dim
                             : (hdr.n_head != 0 ? hdr.n_embd / hdr.n_head : 0);
  ctx->config.n_ff = hdr.n_ff;
  ctx->config.vocab_size = hdr.vocab_size;
  ctx->config.max_seq_len = hdr.max_seq_len;
  ctx->config.rope_theta = hdr.rope_theta;
  ctx->config.rms_eps = hdr.rms_eps;
  ctx->config.bos_token_id = hdr.bos_token_id;
  ctx->config.eos_token_id = hdr.eos_token_id;
  ctx->config.pad_token_id = hdr.pad_token_id;
  ctx->config.quant_type = hdr.quant_type;
  ctx->config.group_size = hdr.group_size;
  ctx->config.flags = hdr.flags;

  table = (struct nllm_disk_tensor_s *)malloc(tablebytes);
  if (table == NULL)
    {
      return -ENOMEM;
    }

  ret = rkllm_read_at(ctx->fd, table, tablebytes, (off_t)hdr.tensor_table_off);
  if (ret < 0)
    {
      free(table);
      return ret;
    }

  ctx->slots = (struct rkllm_slot_s *)calloc(hdr.n_tensors,
                                             sizeof(struct rkllm_slot_s));
  if (ctx->slots == NULL)
    {
      free(table);
      return -ENOMEM;
    }

  for (i = 0; i < (int)hdr.n_tensors; i++)
    {
      struct nllm_disk_tensor_s *src = &table[i];

      if ((off_t)(src->data_off + src->data_size) > ctx->filesize)
        {
          llmerr("ERROR: tensor %d payload out of bounds\n", i);
          free(table);
          return -EINVAL;
        }

      if (rkllm_dtype_bits(src->dtype) == 0)
        {
          llmerr("ERROR: tensor %d unknown dtype %" PRIu32 "\n", i,
                 src->dtype);
          free(table);
          return -ENOTSUP;
        }

      memcpy(&ctx->slots[i].rec, src, sizeof(*src));
      ctx->slots[i].rec.name[NLLM_TENSOR_NAME_MAX - 1] = '\0';
      ctx->slots[i].scale_off = src->scale_off;
      ctx->slots[i].zero_off = src->zero_off;
    }

  ctx->ntensors = (int)hdr.n_tensors;
  free(table);

  /* Tokenizer section, if present */

  if (hdr.tokenizer_size >= sizeof(struct nllm_disk_tok_s) &&
      (off_t)(hdr.tokenizer_off + hdr.tokenizer_size) <= ctx->filesize)
    {
      ctx->tokblob = (uint8_t *)malloc((size_t)hdr.tokenizer_size);
      if (ctx->tokblob == NULL)
        {
          return -ENOMEM;
        }

      ret = rkllm_read_at(ctx->fd, ctx->tokblob, (size_t)hdr.tokenizer_size,
                          (off_t)hdr.tokenizer_off);
      if (ret < 0)
        {
          return ret;
        }

      ctx->toksize = (size_t)hdr.tokenizer_size;
      ctx->has_tok =
          memcmp(ctx->tokblob, NLLM_TOKENIZER_MAGIC, NLLM_MAGIC_LEN) == 0;
      if (!ctx->has_tok)
        {
          llmwarn("WARNING: tokenizer section magic mismatch, ignored\n");
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rkllm_map_rkllm_dtype
 *
 * Description:
 *   Translate an inferred RKLLM dtype code into an NLLM dtype.  RKLLM int4
 *   weights are per-group affine quantised, so they map onto the
 *   asymmetric variant unless the file says the group size is zero.
 *
 ****************************************************************************/

static int rkllm_map_rkllm_dtype(uint32_t dtype, uint32_t group_size)
{
  switch (dtype)
    {
      case RKLLM_DT_FLOAT32:
        return NLLM_DTYPE_F32;

      case RKLLM_DT_FLOAT16:
        return NLLM_DTYPE_F16;

      case RKLLM_DT_INT8:
        return NLLM_DTYPE_I8;

      case RKLLM_DT_INT4:
        return group_size != 0 ? NLLM_DTYPE_I4_ASYM : NLLM_DTYPE_I4_SYM;

      case RKLLM_DT_UINT8:
        return NLLM_DTYPE_U8;

      default:
        return -ENOTSUP;
    }
}

/****************************************************************************
 * Name: rkllm_parse_rkllm
 *
 * Description:
 *   Parse the Rockchip container using the inferred layout above.
 *
 *   VERIFICATION RECIPE (do this before trusting a single weight):
 *     1. hexdump -C model.rkllm | head -16
 *        Confirm the 4 byte magic and note whether the next word looks like
 *        a small version number.
 *     2. Check that header_size (offset 8) is a small multiple of 16 and
 *        that the offsets at 16..79 are monotonically increasing and land
 *        inside the file.  If they do not, the field order below is wrong.
 *     3. Dump the first tensor name at tensor_table_off: it must be
 *        printable ASCII such as "model.layers.0.self_attn.q_proj.weight".
 *     4. Cross-check n_tensors against the layer count reported by the
 *        RKLLM-Toolkit export log for the same model.
 *   Until all four pass, the .nllm path is the one to use.
 *
 ****************************************************************************/

static int rkllm_parse_rkllm(struct rkllm_ctx_s *ctx)
{
  struct rkllm_disk_hdr_s hdr;
  struct rkllm_disk_cfg_s cfg;
  uint8_t *table = NULL;
  size_t stride;
  size_t tablebytes;
  int ret;
  int i;

  ret = rkllm_read_at(ctx->fd, &hdr, sizeof(hdr), 0);
  if (ret < 0)
    {
      return ret;
    }

  if ((size_t)hdr.header_size < RKLLM_HDR_SIZE_MIN ||
      hdr.header_size > RKLLM_HEADER_SIZE_MAX)
    {
      llmerr("ERROR: .rkllm header_size %" PRIu32 " rejected; the inferred "
             "layout does not match this file, use .nllm\n",
             hdr.header_size);
      return -ENOTSUP;
    }

  if (hdr.n_tensors == 0 || hdr.n_tensors > RKLLM_MAX_TENSORS)
    {
      llmerr("ERROR: .rkllm tensor count %" PRIu32 " implausible\n",
             hdr.n_tensors);
      return -ENOTSUP;
    }

  stride = hdr.tensor_entry_size != 0 ? hdr.tensor_entry_size
                                      : RKLLM_ENTRY_SIZE_MIN;
  if (stride < RKLLM_ENTRY_SIZE_MIN)
    {
      llmerr("ERROR: .rkllm entry stride %zu below inferred record\n", stride);
      return -ENOTSUP;
    }

  tablebytes = stride * hdr.n_tensors;
  if ((off_t)(hdr.tensor_table_off + tablebytes) > ctx->filesize)
    {
      llmerr("ERROR: .rkllm tensor table out of bounds\n");
      return -ENOTSUP;
    }

  /* Hyper-parameters.  A short or missing block is not fatal: the caller
   * can still enumerate tensors, it just has to supply the geometry.
   */

  memset(&cfg, 0, sizeof(cfg));
  if (hdr.config_size >= sizeof(cfg) &&
      (off_t)(hdr.config_off + sizeof(cfg)) <= ctx->filesize)
    {
      ret = rkllm_read_at(ctx->fd, &cfg, sizeof(cfg), (off_t)hdr.config_off);
      if (ret < 0)
        {
          return ret;
        }
    }
  else
    {
      llmwarn("WARNING: .rkllm config block absent or short\n");
    }

  ctx->config.arch = NLLM_ARCH_UNKNOWN;
  ctx->config.n_layer = cfg.n_layer;
  ctx->config.n_embd = cfg.n_embd;
  ctx->config.n_head = cfg.n_head;
  ctx->config.n_kv_head = cfg.n_kv_head != 0 ? cfg.n_kv_head : cfg.n_head;
  ctx->config.head_dim = cfg.head_dim != 0
                             ? cfg.head_dim
                             : (cfg.n_head != 0 ? cfg.n_embd / cfg.n_head : 0);
  ctx->config.n_ff = cfg.n_ff;
  ctx->config.vocab_size = cfg.vocab_size;
  ctx->config.max_seq_len = cfg.max_context_len;
  ctx->config.rope_theta = cfg.rope_theta;
  ctx->config.rms_eps = cfg.rms_eps;
  ctx->config.bos_token_id = cfg.bos_token_id;
  ctx->config.eos_token_id = cfg.eos_token_id;
  ctx->config.pad_token_id = -1;
  ctx->config.group_size = cfg.group_size;
  ctx->config.quant_type =
      cfg.quant_bits == 4 ? NLLM_DTYPE_I4_ASYM : NLLM_DTYPE_I8;

  table = (uint8_t *)malloc(tablebytes);
  if (table == NULL)
    {
      return -ENOMEM;
    }

  ret = rkllm_read_at(ctx->fd, table, tablebytes, (off_t)hdr.tensor_table_off);
  if (ret < 0)
    {
      free(table);
      return ret;
    }

  ctx->slots = (struct rkllm_slot_s *)calloc(hdr.n_tensors,
                                             sizeof(struct rkllm_slot_s));
  if (ctx->slots == NULL)
    {
      free(table);
      return -ENOMEM;
    }

  for (i = 0; i < (int)hdr.n_tensors; i++)
    {
      const struct rkllm_disk_tensor_s *src =
          (const struct rkllm_disk_tensor_s *)(table + (size_t)i * stride);
      struct nllm_disk_tensor_s *dst = &ctx->slots[i].rec;
      int dtype;

      dtype = rkllm_map_rkllm_dtype(src->dtype, cfg.group_size);
      if (dtype < 0)
        {
          llmerr("ERROR: .rkllm tensor %d dtype %" PRIu32 " unsupported\n", i,
                 src->dtype);
          free(table);
          return dtype;
        }

      if ((off_t)(src->data_off + src->data_size) > ctx->filesize)
        {
          llmerr("ERROR: .rkllm tensor %d payload out of bounds\n", i);
          free(table);
          return -ENOTSUP;
        }

      memset(dst, 0, sizeof(*dst));

      /* The on-disk name field is not guaranteed to be terminated, so copy
       * the fixed width and terminate by hand.
       */

      memcpy(dst->name, src->name, RKLLM_ENTRY_NAME_MAX);
      dst->name[NLLM_TENSOR_NAME_MAX - 1] = '\0';
      dst->dtype = (uint32_t)dtype;
      dst->ndim = src->ndim > NLLM_MAX_DIMS ? NLLM_MAX_DIMS : src->ndim;
      memcpy(dst->dims, src->dims, sizeof(dst->dims));
      dst->nelem = rkllm_dims_product(dst->dims, dst->ndim);
      dst->group_size = cfg.group_size;
      dst->ngroups =
          cfg.group_size != 0 ? (uint32_t)(dst->nelem / cfg.group_size) : 0;
      dst->data_off = src->data_off;
      dst->data_size = src->data_size;

      /* TODO: the placement of the per-group scale and zero arrays inside
       * an RKLLM payload is unknown.  Assuming "quants first, then scales,
       * then zeros" (the layout our own writer uses) lets the accessors
       * work if the guess is right and produces obviously wrong numbers if
       * it is not.  Confirm by dumping one small tensor and checking that
       * the tail bytes decode as plausible fp16 magnitudes.
       */

      if (dst->ngroups != 0 && dst->nelem != 0)
        {
          size_t qbytes =
              (size_t)((dst->nelem * rkllm_dtype_bits(dst->dtype) + 7) / 8);

          ctx->slots[i].scale_off = (uint32_t)qbytes;
          if (dtype == NLLM_DTYPE_I4_ASYM)
            {
              ctx->slots[i].zero_off =
                  (uint32_t)(qbytes + dst->ngroups * sizeof(uint16_t));
            }

          ctx->slots[i].rec.scale_off = ctx->slots[i].scale_off;
          ctx->slots[i].rec.zero_off = ctx->slots[i].zero_off;
        }
    }

  ctx->ntensors = (int)hdr.n_tensors;
  free(table);

  /* The RKLLM tokenizer blob is a HuggingFace tokenizer.json payload rather
   * than our fixed table, so it is exposed as raw bytes only.
   * TODO: confirm it is JSON (first non-space byte '{') and, if so, add a
   * JSON vocabulary loader instead of the table view.
   */

  if (hdr.tokenizer_size != 0 &&
      (off_t)(hdr.tokenizer_off + hdr.tokenizer_size) <= ctx->filesize)
    {
      llminfo("rkllm: tokenizer blob %" PRIu64 " bytes at %" PRIu64
              " (raw, not parsed)\n",
              hdr.tokenizer_size, hdr.tokenizer_off);
    }

  return OK;
}

/****************************************************************************
 * Name: rkllm_slot_payload
 *
 * Description:
 *   Return a pointer to the tensor payload, reading and caching it first
 *   when the container was opened lazily.
 *
 ****************************************************************************/

static int rkllm_slot_payload(struct rkllm_ctx_s *ctx,
                              struct rkllm_slot_s *slot, const void **payload)
{
  int ret;

  if (slot->rec.data_size == 0)
    {
      *payload = NULL;
      return OK;
    }

  if (ctx->image != NULL)
    {
      *payload = ctx->image + slot->rec.data_off;
      return OK;
    }

  if (slot->cache == NULL)
    {
      /* Weight payloads are handed to the NPU, so they must be 64 byte
       * aligned.  aligned_alloc() requires the size to be a multiple of the
       * alignment, hence the round up.
       */

      size_t len = ((size_t)slot->rec.data_size + NLLM_DATA_ALIGN - 1) &
                   ~((size_t)NLLM_DATA_ALIGN - 1);

      slot->cache = memalign(NLLM_DATA_ALIGN, len);
      if (slot->cache == NULL)
        {
          return -ENOMEM;
        }

      ret = rkllm_read_at(ctx->fd, slot->cache, (size_t)slot->rec.data_size,
                          (off_t)slot->rec.data_off);
      if (ret < 0)
        {
          free(slot->cache);
          slot->cache = NULL;
          return ret;
        }
    }

  *payload = slot->cache;
  return OK;
}

/****************************************************************************
 * Name: rkllm_fill_tensor
 ****************************************************************************/

static int rkllm_fill_tensor(struct rkllm_ctx_s *ctx, int index,
                             struct llm_tensor_s *tensor)
{
  struct rkllm_slot_s *slot = &ctx->slots[index];
  const uint8_t *base;
  const void *payload;
  int ret;

  ret = rkllm_slot_payload(ctx, slot, &payload);
  if (ret < 0)
    {
      return ret;
    }

  base = (const uint8_t *)payload;

  memset(tensor, 0, sizeof(*tensor));
  memcpy(tensor->name, slot->rec.name, NLLM_TENSOR_NAME_MAX);
  tensor->name[NLLM_TENSOR_NAME_MAX - 1] = '\0';
  tensor->dtype = slot->rec.dtype;
  tensor->ndim = slot->rec.ndim;
  memcpy(tensor->dims, slot->rec.dims, sizeof(tensor->dims));
  tensor->nelem = slot->rec.nelem;
  tensor->group_size = slot->rec.group_size;
  tensor->ngroups = slot->rec.ngroups;
  tensor->data = payload;
  tensor->size = (size_t)slot->rec.data_size;

  if (base != NULL && tensor->ngroups != 0)
    {
      tensor->qdata = base;
      tensor->scales = (const uint16_t *)(base + slot->scale_off);
      if (tensor->dtype == NLLM_DTYPE_I4_ASYM && slot->zero_off != 0)
        {
          tensor->zeros = (const uint16_t *)(base + slot->zero_off);
        }
    }

  return OK;
}

/****************************************************************************
 * Name: rkllm_free_slots
 ****************************************************************************/

static void rkllm_free_slots(struct rkllm_ctx_s *ctx)
{
  int i;

  if (ctx->slots == NULL)
    {
      return;
    }

  for (i = 0; i < ctx->ntensors; i++)
    {
      if (ctx->slots[i].cache != NULL)
        {
          free(ctx->slots[i].cache);
        }
    }

  free(ctx->slots);
  ctx->slots = NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rkllm_open
 ****************************************************************************/

int rkllm_open(const char *path, uint32_t flags, struct rkllm_ctx_s **ctxp)
{
  struct rkllm_ctx_s *ctx;
  struct stat sb;
  char magic[NLLM_MAGIC_LEN];
  int ret;

  if (path == NULL || ctxp == NULL)
    {
      return -EINVAL;
    }

  ctx = (struct rkllm_ctx_s *)calloc(1, sizeof(struct rkllm_ctx_s));
  if (ctx == NULL)
    {
      return -ENOMEM;
    }

  ctx->fd = open(path, O_RDONLY);
  if (ctx->fd < 0)
    {
      ret = -errno;
      free(ctx);
      return ret;
    }

  if (fstat(ctx->fd, &sb) < 0)
    {
      ret = -errno;
      goto errout;
    }

  ctx->filesize = sb.st_size;
  if (ctx->filesize < (off_t)NLLM_HEADER_SIZE)
    {
      ret = -EINVAL;
      goto errout;
    }

  ret = rkllm_read_at(ctx->fd, magic, sizeof(magic), 0);
  if (ret < 0)
    {
      goto errout;
    }

  if (memcmp(magic, NLLM_MAGIC, NLLM_MAGIC_LEN) == 0)
    {
      ctx->kind = RKLLM_KIND_NLLM;
    }
  else if (memcmp(magic, RKLLM_MAGIC_A, NLLM_MAGIC_LEN) == 0 ||
           memcmp(magic, RKLLM_MAGIC_B, NLLM_MAGIC_LEN) == 0)
    {
      ctx->kind = RKLLM_KIND_RKLLM;
    }
  else
    {
      llmerr("ERROR: unknown model magic %02x %02x %02x %02x\n", magic[0],
             magic[1], magic[2], magic[3]);
      ret = -EINVAL;
      goto errout;
    }

  if ((flags & RKLLM_OPEN_PRELOAD) != 0)
    {
      ctx->image = (uint8_t *)memalign(
          NLLM_DATA_ALIGN, ((size_t)ctx->filesize + NLLM_DATA_ALIGN - 1) &
                               ~((size_t)NLLM_DATA_ALIGN - 1));
      if (ctx->image == NULL)
        {
          ret = -ENOMEM;
          goto errout;
        }

      ret = rkllm_read_at(ctx->fd, ctx->image, (size_t)ctx->filesize, 0);
      if (ret < 0)
        {
          goto errout;
        }
    }

  ret = ctx->kind == RKLLM_KIND_NLLM ? rkllm_parse_nllm(ctx)
                                     : rkllm_parse_rkllm(ctx);
  if (ret < 0)
    {
      goto errout;
    }

  *ctxp = ctx;
  return OK;

errout:
  rkllm_close(ctx);
  return ret;
}

/****************************************************************************
 * Name: rkllm_close
 ****************************************************************************/

void rkllm_close(struct rkllm_ctx_s *ctx)
{
  if (ctx == NULL)
    {
      return;
    }

  rkllm_free_slots(ctx);

  if (ctx->tokblob != NULL)
    {
      free(ctx->tokblob);
    }

  if (ctx->image != NULL)
    {
      free(ctx->image);
    }

  if (ctx->fd >= 0)
    {
      close(ctx->fd);
    }

  free(ctx);
}

/****************************************************************************
 * Name: rkllm_get_config
 ****************************************************************************/

const struct llm_config_s *rkllm_get_config(struct rkllm_ctx_s *ctx)
{
  return ctx != NULL ? &ctx->config : NULL;
}

/****************************************************************************
 * Name: rkllm_get_tensor
 ****************************************************************************/

int rkllm_get_tensor(struct rkllm_ctx_s *ctx, const char *name,
                     struct llm_tensor_s *tensor)
{
  int i;

  if (ctx == NULL || name == NULL || tensor == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < ctx->ntensors; i++)
    {
      if (strncmp(ctx->slots[i].rec.name, name, NLLM_TENSOR_NAME_MAX) == 0)
        {
          return rkllm_fill_tensor(ctx, i, tensor);
        }
    }

  return -ENOENT;
}

/****************************************************************************
 * Name: rkllm_get_layer_tensor
 ****************************************************************************/

int rkllm_get_layer_tensor(struct rkllm_ctx_s *ctx, unsigned int layer,
                           const char *suffix, struct llm_tensor_s *tensor)
{
  char name[RKLLM_NAME_FMT_MAX];

  if (suffix == NULL)
    {
      return -EINVAL;
    }

  snprintf(name, sizeof(name), "blk.%u.%s", layer, suffix);
  return rkllm_get_tensor(ctx, name, tensor);
}

/****************************************************************************
 * Name: rkllm_get_tokenizer
 ****************************************************************************/

int rkllm_get_tokenizer(struct rkllm_ctx_s *ctx, struct llm_tokenizer_s *tok)
{
  const struct nllm_disk_tok_s *hdr;

  if (ctx == NULL || tok == NULL)
    {
      return -EINVAL;
    }

  if (!ctx->has_tok)
    {
      return -ENOENT;
    }

  hdr = (const struct nllm_disk_tok_s *)ctx->tokblob;

  if (hdr->off_pool + hdr->pool_size > ctx->toksize ||
      hdr->off_offsets + hdr->n_tokens * sizeof(uint32_t) > ctx->toksize ||
      hdr->off_lengths + hdr->n_tokens * sizeof(uint32_t) > ctx->toksize)
    {
      llmerr("ERROR: tokenizer section internally inconsistent\n");
      return -EINVAL;
    }

  memset(tok, 0, sizeof(*tok));
  tok->n_tokens = hdr->n_tokens;
  tok->unk_id = hdr->unk_id;
  tok->bos_id = hdr->bos_id;
  tok->eos_id = hdr->eos_id;
  tok->pad_id = hdr->pad_id;
  tok->offsets = (const uint32_t *)(ctx->tokblob + hdr->off_offsets);
  tok->lengths = (const uint32_t *)(ctx->tokblob + hdr->off_lengths);
  tok->pool = (const char *)(ctx->tokblob + hdr->off_pool);
  tok->pool_size = hdr->pool_size;

  if (hdr->off_scores != 0 &&
      hdr->off_scores + hdr->n_tokens * sizeof(float) <= ctx->toksize)
    {
      tok->scores = (const float *)(ctx->tokblob + hdr->off_scores);
    }

  return OK;
}

/****************************************************************************
 * Name: rkllm_tensor_count
 ****************************************************************************/

int rkllm_tensor_count(struct rkllm_ctx_s *ctx)
{
  return ctx != NULL ? ctx->ntensors : -EINVAL;
}

/****************************************************************************
 * Name: rkllm_tensor_at
 ****************************************************************************/

int rkllm_tensor_at(struct rkllm_ctx_s *ctx, int index,
                    struct llm_tensor_s *tensor)
{
  if (ctx == NULL || tensor == NULL)
    {
      return -EINVAL;
    }

  if (index < 0 || index >= ctx->ntensors)
    {
      return -ERANGE;
    }

  return rkllm_fill_tensor(ctx, index, tensor);
}

/****************************************************************************
 * Name: rkllm_container_kind
 ****************************************************************************/

int rkllm_container_kind(struct rkllm_ctx_s *ctx)
{
  return ctx != NULL ? ctx->kind : RKLLM_KIND_NONE;
}

/****************************************************************************
 * Name: rkllm_fp16_to_fp32
 *
 * Description:
 *   Software half to single conversion.  Kept in integer arithmetic so it
 *   does not depend on __fp16 support in the toolchain.
 *
 ****************************************************************************/

float rkllm_fp16_to_fp32(uint16_t h)
{
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1f;
  uint32_t man = h & 0x3ff;
  uint32_t bits;
  float f;

  if (exp == 0)
    {
      if (man == 0)
        {
          bits = sign; /* signed zero               */
        }
      else
        {
          /* Subnormal half: renormalise into a single precision normal */

          exp = 127 - 15 + 1;
          while ((man & 0x400) == 0)
            {
              man <<= 1;
              exp--;
            }

          man &= 0x3ff;
          bits = sign | (exp << 23) | (man << 13);
        }
    }
  else if (exp == 0x1f)
    {
      bits = sign | 0x7f800000 | (man << 13); /* inf / NaN                */
    }
  else
    {
      bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }

  memcpy(&f, &bits, sizeof(f));
  return f;
}

/****************************************************************************
 * Name: rkllm_dequant_group
 *
 * Description:
 *   Expand one quantisation group into float.  'out' must hold at least
 *   tensor->group_size elements.
 *
 ****************************************************************************/

int rkllm_dequant_group(const struct llm_tensor_s *tensor, uint32_t group,
                        float *out, size_t outlen)
{
  uint32_t gs;
  uint32_t i;
  float scale;
  float zero = 0.0f;

  if (tensor == NULL || out == NULL || tensor->qdata == NULL ||
      tensor->scales == NULL)
    {
      return -EINVAL;
    }

  gs = tensor->group_size;
  if (gs == 0 || group >= tensor->ngroups || outlen < gs)
    {
      return -EINVAL;
    }

  scale = rkllm_fp16_to_fp32(tensor->scales[group]);
  if (tensor->zeros != NULL)
    {
      zero = rkllm_fp16_to_fp32(tensor->zeros[group]);
    }

  switch (tensor->dtype)
    {
      case NLLM_DTYPE_I8:
        {
          const int8_t *q = (const int8_t *)tensor->qdata + (size_t)group * gs;

          for (i = 0; i < gs; i++)
            {
              out[i] = (float)q[i] * scale + zero;
            }
        }
        break;

      case NLLM_DTYPE_I4_SYM:
      case NLLM_DTYPE_I4_ASYM:
        {
          /* Two nibbles per byte, element 2n in the low nibble.  The
           * symmetric variant stores values biased by 8 so the stored
           * nibble is always in [0, 15].
           */

          const uint8_t *q = tensor->qdata + ((size_t)group * gs) / 2;

          for (i = 0; i < gs; i++)
            {
              uint8_t byte = q[i >> 1];
              int nib = (i & 1) != 0 ? (byte >> 4) : (byte & 0x0f);

              if (tensor->dtype == NLLM_DTYPE_I4_SYM)
                {
                  out[i] = (float)(nib - 8) * scale;
                }
              else
                {
                  out[i] = (float)nib * scale + zero;
                }
            }
        }
        break;

      default:
        return -ENOTSUP;
    }

  return OK;
}
