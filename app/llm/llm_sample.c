/****************************************************************************
 * app/llm/llm_sample.c
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
 * Token sampling: greedy, temperature, top-k and top-p (nucleus).
 *
 * The chain is temperature -> softmax -> top-k -> top-p -> multinomial.
 * temperature == 0 short-circuits to argmax, which is the deterministic
 * mode used when comparing the CPU and NPU matmul backends.
 *
 * Sorting the whole vocabulary would dominate the per-token cost on a
 * 150k-entry vocabulary, so candidates whose probability cannot possibly
 * enter the nucleus are discarded before the sort.  Any token with
 * p < (1 - topp) / (vocab_size - 1) can never be part of the smallest set
 * whose mass reaches topp, so dropping it does not change the result.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "llm_transformer.h"

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t llm_rand_u32(struct llm_sampler_s *sampler);
static float llm_rand_f32(struct llm_sampler_s *sampler);
static int llm_argmax(const float *v, int n);
static int llm_prob_compare(const void *a, const void *b);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_rand_u32
 *
 * Description:
 *   xorshift64* pseudo random generator.  Self-contained so that a run is
 *   reproducible from the seed regardless of what else uses rand().
 *
 ****************************************************************************/

static uint32_t llm_rand_u32(struct llm_sampler_s *sampler)
{
  uint64_t x = sampler->rng;

  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  sampler->rng = x;
  return (uint32_t)((x * 0x2545f4914f6cdd1dull) >> 32);
}

/****************************************************************************
 * Name: llm_rand_f32
 *
 * Description:
 *   Uniform float in [0, 1).
 *
 ****************************************************************************/

static float llm_rand_f32(struct llm_sampler_s *sampler)
{
  return (float)(llm_rand_u32(sampler) >> 8) / 16777216.0f;
}

/****************************************************************************
 * Name: llm_argmax
 ****************************************************************************/

static int llm_argmax(const float *v, int n)
{
  int best = 0;
  int i;

  for (i = 1; i < n; i++)
    {
      if (v[i] > v[best])
        {
          best = i;
        }
    }

  return best;
}

/****************************************************************************
 * Name: llm_prob_compare
 *
 * Description:
 *   qsort comparator giving descending probability order.
 *
 ****************************************************************************/

static int llm_prob_compare(const void *a, const void *b)
{
  const struct llm_prob_s *pa = (const struct llm_prob_s *)a;
  const struct llm_prob_s *pb = (const struct llm_prob_s *)b;

  if (pa->p > pb->p)
    {
      return -1;
    }

  if (pa->p < pb->p)
    {
      return 1;
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_sampler_init
 *
 * Description:
 *   Set up the sampler and allocate the candidate scratch buffer.
 *
 * Input Parameters:
 *   sampler     - Caller-provided sampler structure
 *   vocab_size  - Number of logits produced by the model
 *   temperature - 0 selects greedy decoding
 *   topk        - Keep at most this many candidates, 0 disables
 *   topp        - Nucleus mass in (0, 1], 1 disables
 *   seed        - PRNG seed, 0 is replaced by a fixed non-zero constant
 *
 * Returned Value:
 *   OK on success, a negated errno on failure.
 *
 ****************************************************************************/

int llm_sampler_init(struct llm_sampler_s *sampler, int vocab_size,
                     float temperature, int topk, float topp, uint64_t seed)
{
  memset(sampler, 0, sizeof(*sampler));

  if (vocab_size <= 0)
    {
      return -EINVAL;
    }

  if (temperature < 0.0f)
    {
      temperature = 0.0f;
    }

  if (topp <= 0.0f || topp > 1.0f)
    {
      topp = 1.0f;
    }

  if (topk < 0 || topk > vocab_size)
    {
      topk = vocab_size;
    }

  sampler->vocab_size = vocab_size;
  sampler->temperature = temperature;
  sampler->topk = topk;
  sampler->topp = topp;

  /* xorshift64* degenerates to a constant zero stream from a zero state. */

  sampler->rng = seed != 0 ? seed : 0x853c49e6748fea9bull;

  sampler->cand = (struct llm_prob_s *)malloc((size_t)vocab_size *
                                              sizeof(struct llm_prob_s));
  if (sampler->cand == NULL)
    {
      return -ENOMEM;
    }

  return OK;
}

/****************************************************************************
 * Name: llm_sampler_free
 ****************************************************************************/

void llm_sampler_free(struct llm_sampler_s *sampler)
{
  free(sampler->cand);
  sampler->cand = NULL;
}

/****************************************************************************
 * Name: llm_sample
 *
 * Description:
 *   Draw one token from the logits.  The logits array is modified in place
 *   (it holds the softmax result on return).
 *
 * Returned Value:
 *   The sampled token id.
 *
 ****************************************************************************/

int llm_sample(struct llm_sampler_s *sampler, float *logits)
{
  const int n = sampler->vocab_size;
  struct llm_prob_s *cand = sampler->cand;
  float cutoff;
  float mass = 0.0f;
  float r;
  int ncand = 0;
  int keep;
  int i;

  if (sampler->temperature == 0.0f || cand == NULL)
    {
      return llm_argmax(logits, n);
    }

  if (sampler->temperature != 1.0f)
    {
      float inv = 1.0f / sampler->temperature;

      for (i = 0; i < n; i++)
        {
          logits[i] *= inv;
        }
    }

  llm_softmax(logits, n);

  /* Plain temperature sampling: no truncation, walk the CDF directly. */

  if (sampler->topp >= 1.0f && sampler->topk >= n)
    {
      r = llm_rand_f32(sampler);

      for (i = 0; i < n; i++)
        {
          mass += logits[i];
          if (r < mass)
            {
              return i;
            }
        }

      return n - 1;
    }

  cutoff = n > 1 ? (1.0f - sampler->topp) / (float)(n - 1) : 0.0f;

  for (i = 0; i < n; i++)
    {
      if (logits[i] >= cutoff)
        {
          cand[ncand].p = logits[i];
          cand[ncand].index = (uint32_t)i;
          ncand++;
        }
    }

  if (ncand == 0)
    {
      return llm_argmax(logits, n);
    }

  qsort(cand, (size_t)ncand, sizeof(struct llm_prob_s), llm_prob_compare);

  keep = ncand;

  if (sampler->topk > 0 && sampler->topk < keep)
    {
      keep = sampler->topk;
    }

  /* Nucleus: shortest prefix whose mass reaches topp. */

  if (sampler->topp < 1.0f)
    {
      float acc = 0.0f;

      for (i = 0; i < keep; i++)
        {
          acc += cand[i].p;
          if (acc >= sampler->topp)
            {
              keep = i + 1;
              break;
            }
        }
    }

  for (i = 0; i < keep; i++)
    {
      mass += cand[i].p;
    }

  if (mass <= 0.0f)
    {
      return (int)cand[0].index;
    }

  r = llm_rand_f32(sampler) * mass;
  mass = 0.0f;

  for (i = 0; i < keep; i++)
    {
      mass += cand[i].p;
      if (r < mass)
        {
          return (int)cand[i].index;
        }
    }

  return (int)cand[keep - 1].index;
}
