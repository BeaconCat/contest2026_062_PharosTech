/****************************************************************************
 * app/llm/llm_tokenizer.c
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
 * Byte-level BPE tokenizer.
 *
 * The vocabulary and the ordered merge list are carried inside the model
 * image, so the token strings are referenced in place and never copied.
 * Encoding is the textbook algorithm:
 *
 *   1. Every input byte becomes its single-byte token.
 *   2. The adjacent pair with the lowest merge rank is replaced by the
 *      merged token, repeatedly, until no pair can be merged.
 *
 * The offline converter is expected to have already undone the GPT-2
 * byte-to-unicode mapping, so the vocabulary entries stored in the model
 * are raw bytes and step 1 needs no special alphabet.
 *
 * Note that the HuggingFace pre-tokenizer regular expression is not
 * applied.  It only constrains which merges are legal across word
 * boundaries; skipping it can produce a different but still valid
 * segmentation for unusual inputs such as long digit runs.  For ordinary
 * prompts the output matches the reference tokenizer.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "llm_transformer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LLM_TOK_HEADER_BYTES  16
#define LLM_MERGE_ENTRY_BYTES 12

/* Load factor of the merge hash table stays at or below 50%. */

#define LLM_MERGE_TABLE_SCALE 2

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t llm_rd32(const uint8_t *p);
static uint64_t llm_merge_hash(uint64_t key);
static void     llm_merge_insert(struct llm_tokenizer_s *tok, uint32_t a,
                                 uint32_t b, uint32_t ab, uint32_t rank);
static bool     llm_merge_find(const struct llm_tokenizer_s *tok,
                               uint32_t a, uint32_t b, uint32_t *rank,
                               uint32_t *result);
static int      llm_encode_bytes(struct llm_tokenizer_s *tok,
                                 const uint8_t *bytes, size_t len,
                                 int *out, int max_out, int n);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_rd32
 *
 * Description:
 *   Read a little-endian 32-bit word from a possibly unaligned address.
 *
 ****************************************************************************/

static uint32_t llm_rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/****************************************************************************
 * Name: llm_merge_hash
 *
 * Description:
 *   splitmix64 finaliser, good enough to scatter (a,b) token pairs.
 *
 ****************************************************************************/

static uint64_t llm_merge_hash(uint64_t key)
{
  key += 0x9e3779b97f4a7c15ull;
  key = (key ^ (key >> 30)) * 0xbf58476d1ce4e5b9ull;
  key = (key ^ (key >> 27)) * 0x94d049bb133111ebull;
  return key ^ (key >> 31);
}

/****************************************************************************
 * Name: llm_merge_insert
 ****************************************************************************/

static void llm_merge_insert(struct llm_tokenizer_s *tok, uint32_t a,
                             uint32_t b, uint32_t ab, uint32_t rank)
{
  uint64_t key = ((uint64_t)a << 32) | b;
  uint32_t slot = (uint32_t)(llm_merge_hash(key) & tok->mask);

  while (tok->table[slot].used)
    {
      if (tok->table[slot].key == key)
        {
          /* Duplicate pair: the first occurrence has the lower rank and
           * therefore wins.
           */

          return;
        }

      slot = (slot + 1) & tok->mask;
    }

  tok->table[slot].used = true;
  tok->table[slot].key = key;
  tok->table[slot].rank = rank;
  tok->table[slot].result = ab;
}

/****************************************************************************
 * Name: llm_merge_find
 ****************************************************************************/

static bool llm_merge_find(const struct llm_tokenizer_s *tok, uint32_t a,
                           uint32_t b, uint32_t *rank, uint32_t *result)
{
  uint64_t key = ((uint64_t)a << 32) | b;
  uint32_t slot = (uint32_t)(llm_merge_hash(key) & tok->mask);

  while (tok->table[slot].used)
    {
      if (tok->table[slot].key == key)
        {
          *rank = tok->table[slot].rank;
          *result = tok->table[slot].result;
          return true;
        }

      slot = (slot + 1) & tok->mask;
    }

  return false;
}

/****************************************************************************
 * Name: llm_encode_bytes
 *
 * Description:
 *   Append the BPE encoding of one raw byte run to out.
 *
 * Input Parameters:
 *   tok     - Tokenizer
 *   bytes   - Byte run to encode
 *   len     - Length of the run
 *   out     - Output token buffer
 *   max_out - Capacity of out
 *   n       - Number of tokens already in out
 *
 * Returned Value:
 *   New token count, or a negated errno.
 *
 ****************************************************************************/

static int llm_encode_bytes(struct llm_tokenizer_s *tok,
                            const uint8_t *bytes, size_t len, int *out,
                            int max_out, int n)
{
  int start = n;
  size_t i;

  /* Step 1: one token per byte. */

  for (i = 0; i < len; i++)
    {
      int32_t id = tok->byte_tok[bytes[i]];

      if (id < 0)
        {
          /* No single-byte token covers this value.  Dropping it keeps the
           * rest of the prompt usable, which is preferable to failing.
           */

          continue;
        }

      if (n >= max_out)
        {
          return -E2BIG;
        }

      out[n++] = (int)id;
    }

  /* Step 2: greedily apply the lowest-ranked adjacent merge. */

  for (; ; )
    {
      uint32_t best_rank = UINT32_MAX;
      uint32_t best_result = 0;
      int best_at = -1;
      int j;

      for (j = start; j + 1 < n; j++)
        {
          uint32_t rank;
          uint32_t result;

          if (llm_merge_find(tok, (uint32_t)out[j], (uint32_t)out[j + 1],
                             &rank, &result) && rank < best_rank)
            {
              best_rank = rank;
              best_result = result;
              best_at = j;
            }
        }

      if (best_at < 0)
        {
          break;
        }

      out[best_at] = (int)best_result;
      memmove(&out[best_at + 1], &out[best_at + 2],
              (size_t)(n - best_at - 2) * sizeof(int));
      n--;
    }

  return n;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_tokenizer_init
 ****************************************************************************/

int llm_tokenizer_init(struct llm_tokenizer_s *tok, const uint8_t *blob,
                       size_t blob_bytes, uint32_t bos_id, uint32_t eos_id)
{
  const uint8_t *p = blob;
  const uint8_t *end = blob + blob_bytes;
  uint32_t flags;
  uint32_t table_size;
  uint32_t i;

  memset(tok, 0, sizeof(*tok));

  if (blob_bytes < LLM_TOK_HEADER_BYTES ||
      llm_rd32(p) != LLM_TOKENIZER_MAGIC)
    {
      fprintf(stderr, "llm: tokenizer blob has a bad magic\n");
      return -EINVAL;
    }

  tok->vocab_size = llm_rd32(p + 4);
  tok->n_merges = llm_rd32(p + 8);
  flags = llm_rd32(p + 12);
  tok->add_bos = (flags & LLM_TOKFLAG_ADD_BOS) != 0;
  tok->bos_id = bos_id;
  tok->eos_id = eos_id;
  p += LLM_TOK_HEADER_BYTES;

  if (tok->vocab_size == 0)
    {
      fprintf(stderr, "llm: tokenizer has an empty vocabulary\n");
      return -EINVAL;
    }

  tok->vocab = (struct llm_bpe_entry_s *)
               calloc(tok->vocab_size, sizeof(struct llm_bpe_entry_s));
  if (tok->vocab == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < 256; i++)
    {
      tok->byte_tok[i] = -1;
    }

  for (i = 0; i < tok->vocab_size; i++)
    {
      uint16_t len;

      if ((size_t)(end - p) < sizeof(len))
        {
          goto truncated;
        }

      len = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
      p += sizeof(len);

      if ((size_t)(end - p) < len)
        {
          goto truncated;
        }

      tok->vocab[i].bytes = p;
      tok->vocab[i].len = len;

      if (len == 1 && tok->byte_tok[p[0]] < 0)
        {
          tok->byte_tok[p[0]] = (int32_t)i;
        }

      p += len;
    }

  /* Merge hash: power-of-two capacity so the mask can replace a modulo. */

  table_size = 16;
  while (table_size < tok->n_merges * LLM_MERGE_TABLE_SCALE)
    {
      table_size <<= 1;
    }

  tok->table = (struct llm_merge_slot_s *)
               calloc(table_size, sizeof(struct llm_merge_slot_s));
  if (tok->table == NULL)
    {
      free(tok->vocab);
      tok->vocab = NULL;
      return -ENOMEM;
    }

  tok->mask = table_size - 1;

  for (i = 0; i < tok->n_merges; i++)
    {
      uint32_t a;
      uint32_t b;
      uint32_t ab;

      if ((size_t)(end - p) < LLM_MERGE_ENTRY_BYTES)
        {
          goto truncated;
        }

      a = llm_rd32(p);
      b = llm_rd32(p + 4);
      ab = llm_rd32(p + 8);
      p += LLM_MERGE_ENTRY_BYTES;

      if (a >= tok->vocab_size || b >= tok->vocab_size ||
          ab >= tok->vocab_size)
        {
          fprintf(stderr, "llm: merge %" PRIu32 " references a token "
                  "outside the vocabulary\n", i);
          llm_tokenizer_free(tok);
          return -EINVAL;
        }

      llm_merge_insert(tok, a, b, ab, i);
    }

  printf("llm: tokenizer %" PRIu32 " tokens, %" PRIu32 " merges\n",
         tok->vocab_size, tok->n_merges);
  return OK;

truncated:
  fprintf(stderr, "llm: tokenizer blob is truncated\n");
  llm_tokenizer_free(tok);
  return -EINVAL;
}

/****************************************************************************
 * Name: llm_tokenizer_free
 ****************************************************************************/

void llm_tokenizer_free(struct llm_tokenizer_s *tok)
{
  free(tok->vocab);
  free(tok->table);
  tok->vocab = NULL;
  tok->table = NULL;
  tok->vocab_size = 0;
  tok->n_merges = 0;
  tok->mask = 0;
}

/****************************************************************************
 * Name: llm_tokenizer_lookup
 *
 * Description:
 *   Exact vocabulary match, used for special tokens such as "<|im_start|>".
 *
 * Returned Value:
 *   Token id, or -1 when the string is not a whole vocabulary entry.
 *
 ****************************************************************************/

int llm_tokenizer_lookup(struct llm_tokenizer_s *tok, const char *s,
                         int len)
{
  uint32_t i;

  if (len <= 0 || tok->vocab == NULL)
    {
      return -1;
    }

  for (i = 0; i < tok->vocab_size; i++)
    {
      if (tok->vocab[i].len == (uint16_t)len &&
          memcmp(tok->vocab[i].bytes, s, (size_t)len) == 0)
        {
          return (int)i;
        }
    }

  return -1;
}

/****************************************************************************
 * Name: llm_tokenizer_encode
 ****************************************************************************/

int llm_tokenizer_encode(struct llm_tokenizer_s *tok, const char *text,
                         bool parse_special, int *out, int max_out)
{
  const char *cursor = text;
  int n = 0;

  if (tok->vocab == NULL)
    {
      return -ENODEV;
    }

  if (tok->add_bos)
    {
      if (max_out < 1)
        {
          return -E2BIG;
        }

      out[n++] = (int)tok->bos_id;
    }

  while (*cursor != '\0')
    {
      const char *marker = NULL;
      const char *marker_end = NULL;
      int marker_id = -1;
      size_t run_len;

      if (parse_special)
        {
          /* Find the next "<|...|>" that is a whole vocabulary entry.  A
           * marker that is not in the vocabulary is ordinary text.
           */

          const char *scan = cursor;

          while ((scan = strstr(scan, "<|")) != NULL)
            {
              const char *close = strstr(scan + 2, "|>");
              int id;

              if (close == NULL)
                {
                  break;
                }

              id = llm_tokenizer_lookup(tok, scan, (int)(close + 2 - scan));
              if (id >= 0)
                {
                  marker = scan;
                  marker_end = close + 2;
                  marker_id = id;
                  break;
                }

              scan = close + 2;
            }
        }

      if (marker == cursor)
        {
          if (n >= max_out)
            {
              return -E2BIG;
            }

          out[n++] = marker_id;
          cursor = marker_end;
          continue;
        }

      run_len = marker != NULL ? (size_t)(marker - cursor)
                               : strlen(cursor);

      n = llm_encode_bytes(tok, (const uint8_t *)cursor, run_len, out,
                           max_out, n);
      if (n < 0)
        {
          return n;
        }

      cursor += run_len;
    }

  return n;
}

/****************************************************************************
 * Name: llm_tokenizer_decode
 ****************************************************************************/

const uint8_t *llm_tokenizer_decode(struct llm_tokenizer_s *tok, int token,
                                    int *len)
{
  if (tok->vocab == NULL || token < 0 ||
      (uint32_t)token >= tok->vocab_size)
    {
      *len = 0;
      return NULL;
    }

  *len = (int)tok->vocab[token].len;
  return tok->vocab[token].bytes;
}
