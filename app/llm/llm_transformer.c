/****************************************************************************
 * app/llm/llm_transformer.c
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
 * LLaMA-family transformer inference engine, CPU (NEON) kernels with an
 * optional RKNPU matmul offload.
 *
 * Layout conventions used throughout:
 *   - A projection weight is row major [out_features][in_features], exactly
 *     like torch.nn.Linear.weight, so out = W . x is a sequence of dot
 *     products over contiguous rows.  Rows are the unit of work handed to
 *     the thread pool.
 *   - int4 weights use the q4_0 block layout described in
 *     llm_transformer.h: 32 values per block, one fp16 scale per block.
 *   - Activations are fp32.  They are converted to fp16 only in the NPU
 *     staging buffer, because the Cortex-A72/A53 in the RK3576 implement
 *     ARMv8.0 and therefore have fp16 conversion but no fp16 arithmetic;
 *     computing in fp16 on the CPU would be slower, not faster.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef CONFIG_EXAMPLES_LLM_NPU
#  include <nuttx/arch.h>
#endif

#include "llm_transformer.h"

#if defined(__aarch64__) && defined(__ARM_NEON)
#  include <arm_neon.h>
#  define LLM_HAVE_NEON 1
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Below this many multiply-accumulates a matmul is cheaper to run on the
 * calling thread than to hand out to the pool.
 */

#define LLM_THREAD_MIN_WORK   (16 * 1024)

/* Model image is read in chunks so that a partial read or a short file is
 * detected early and the progress indicator stays responsive.
 */

#define LLM_READ_CHUNK        (1024 * 1024)
#define LLM_PROGRESS_STEP     (32 * 1024 * 1024)

/* Model image alignment: matches the D-cache line and keeps every 64-byte
 * aligned tensor payload aligned in memory as well.
 */

#define LLM_IMAGE_ALIGN       64

#define LLM_MAX(a, b)         ((a) > (b) ? (a) : (b))

/* The on-disk structures are parsed by direct memcpy, so a layout change
 * has to be caught at build time rather than by a corrupt model.
 */

_Static_assert(sizeof(struct llm_file_header_s) == LLM_HEADER_BYTES,
               "llm: model header layout changed");
_Static_assert(sizeof(struct llm_file_tensor_s) == LLM_FILE_TENSOR_BYTES,
               "llm: tensor directory entry layout changed");

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint64_t llm_now_ms(void);
static float    llm_dot_f32(const float *a, const float *b, int n);
static float    llm_dot_f16(const uint16_t *w, const float *x, int n);
static float    llm_dot_q4_0(const uint8_t *blk, const float *x, int n);
static void     llm_accum_scaled(float *acc, const float *v, float a, int n);
static void     llm_row_to_f32(float *out, const struct llm_tensor_s *t,
                               int row);
static size_t   llm_row_stride(const struct llm_tensor_s *t);
static void     llm_row_range(int rows, int nthreads, int id, int *begin,
                              int *end);
static void    *llm_worker(void *argp);
static int      llm_pool_start(struct llm_pool_s *pool, int nthreads);
static void     llm_pool_stop(struct llm_pool_s *pool);
static void     llm_pool_run(struct llm_pool_s *pool, float *out,
                             const float *x,
                             const struct llm_tensor_s *w);
#ifdef CONFIG_EXAMPLES_LLM_NPU
static bool     llm_npu_symbol_present(void);
static bool     llm_dma_symbol_present(void);
#endif
static int      llm_matmul_npu(struct llm_model_s *model, float *out,
                               const float *x,
                               const struct llm_tensor_s *w);
static void     llm_bias_add(float *out, const struct llm_tensor_s *b,
                             int n);
static const struct llm_file_tensor_s *
                llm_find_tensor(const struct llm_file_tensor_s *tab,
                                uint32_t n, const char *name);
static int      llm_bind(struct llm_model_s *model,
                         const struct llm_file_tensor_s *tab, uint32_t ntab,
                         struct llm_tensor_s *out, const char *name,
                         uint32_t rows, uint32_t cols, bool required);
static int      llm_read_image(const char *path, void **image,
                               size_t *bytes);
static int      llm_state_alloc(struct llm_model_s *model);
static void     llm_state_free(struct llm_model_s *model);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_now_ms
 *
 * Description:
 *   Monotonic millisecond timestamp used for the throughput report.
 *
 ****************************************************************************/

static uint64_t llm_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Name: llm_dot_f32
 *
 * Description:
 *   Dot product of two fp32 vectors.
 *
 ****************************************************************************/

static float llm_dot_f32(const float *a, const float *b, int n)
{
  float sum = 0.0f;
  int i = 0;

#ifdef LLM_HAVE_NEON
  float32x4_t acc0 = vdupq_n_f32(0.0f);
  float32x4_t acc1 = vdupq_n_f32(0.0f);

  for (; i + 8 <= n; i += 8)
    {
      acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
      acc1 = vfmaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    }

  for (; i + 4 <= n; i += 4)
    {
      acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
    }

  sum = vaddvq_f32(vaddq_f32(acc0, acc1));
#endif

  for (; i < n; i++)
    {
      sum += a[i] * b[i];
    }

  return sum;
}

/****************************************************************************
 * Name: llm_dot_f16
 *
 * Description:
 *   Dot product of an fp16 weight row with an fp32 activation vector.
 *   Scalar: fp16 weights are only used for small tensors, the hot path is
 *   int4.
 *
 ****************************************************************************/

static float llm_dot_f16(const uint16_t *w, const float *x, int n)
{
  float sum = 0.0f;
  int i;

  for (i = 0; i < n; i++)
    {
      sum += llm_fp16_to_fp32(w[i]) * x[i];
    }

  return sum;
}

/****************************************************************************
 * Name: llm_dot_q4_0
 *
 * Description:
 *   Dot product of one int4-quantised weight row with an fp32 activation
 *   vector.  The per-block scale is applied once to the block accumulator
 *   instead of once per element.
 *
 * Input Parameters:
 *   blk - First q4_0 block of the row
 *   x   - Activation vector
 *   n   - Number of values in the row, a multiple of LLM_Q4_BLOCK_SIZE
 *
 ****************************************************************************/

static float llm_dot_q4_0(const uint8_t *blk, const float *x, int n)
{
  const int nblocks = n / LLM_Q4_BLOCK_SIZE;
  float sum = 0.0f;
  int i;

#ifdef LLM_HAVE_NEON
  const uint8x16_t nibble = vdupq_n_u8(0x0f);
  const int8x16_t  zeropt = vdupq_n_s8(8);

  for (i = 0; i < nblocks; i++)
    {
      const uint8_t *p = blk + (size_t)i * LLM_Q4_BLOCK_BYTES;
      const float *xp = x + (size_t)i * LLM_Q4_BLOCK_SIZE;
      float32x4_t acc = vdupq_n_f32(0.0f);
      uint8x16_t packed;
      int8x16_t lo;
      int8x16_t hi;
      int16x8_t w0;
      int16x8_t w1;
      int16x8_t w2;
      int16x8_t w3;
      uint16_t scale;

      memcpy(&scale, p, sizeof(scale));
      packed = vld1q_u8(p + sizeof(scale));

      /* Low nibbles hold values 0..15, high nibbles hold values 16..31. */

      lo = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(packed, nibble)), zeropt);
      hi = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(packed, 4)), zeropt);

      w0 = vmovl_s8(vget_low_s8(lo));
      w1 = vmovl_s8(vget_high_s8(lo));
      w2 = vmovl_s8(vget_low_s8(hi));
      w3 = vmovl_s8(vget_high_s8(hi));

      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16(w0))),
                      vld1q_f32(xp));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(w0))),
                      vld1q_f32(xp + 4));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16(w1))),
                      vld1q_f32(xp + 8));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(w1))),
                      vld1q_f32(xp + 12));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16(w2))),
                      vld1q_f32(xp + 16));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(w2))),
                      vld1q_f32(xp + 20));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_low_s16(w3))),
                      vld1q_f32(xp + 24));
      acc = vfmaq_f32(acc, vcvtq_f32_s32(vmovl_s16(vget_high_s16(w3))),
                      vld1q_f32(xp + 28));

      sum += llm_fp16_to_fp32(scale) * vaddvq_f32(acc);
    }
#else
  for (i = 0; i < nblocks; i++)
    {
      const uint8_t *p = blk + (size_t)i * LLM_Q4_BLOCK_BYTES;
      const float *xp = x + (size_t)i * LLM_Q4_BLOCK_SIZE;
      float bsum = 0.0f;
      uint16_t scale;
      int j;

      memcpy(&scale, p, sizeof(scale));
      p += sizeof(scale);

      for (j = 0; j < LLM_Q4_BLOCK_SIZE / 2; j++)
        {
          int lo = (int)(p[j] & 0x0f) - 8;
          int hi = (int)(p[j] >> 4) - 8;

          bsum += (float)lo * xp[j];
          bsum += (float)hi * xp[j + LLM_Q4_BLOCK_SIZE / 2];
        }

      sum += llm_fp16_to_fp32(scale) * bsum;
    }
#endif

  return sum;
}

/****************************************************************************
 * Name: llm_accum_scaled
 *
 * Description:
 *   acc[i] += a * v[i], used to blend the value vectors in attention.
 *
 ****************************************************************************/

static void llm_accum_scaled(float *acc, const float *v, float a, int n)
{
  int i = 0;

#ifdef LLM_HAVE_NEON
  for (; i + 4 <= n; i += 4)
    {
      vst1q_f32(acc + i, vfmaq_n_f32(vld1q_f32(acc + i), vld1q_f32(v + i),
                                     a));
    }
#endif

  for (; i < n; i++)
    {
      acc[i] += a * v[i];
    }
}

/****************************************************************************
 * Name: llm_row_stride
 *
 * Description:
 *   Size in bytes of one row of a tensor.
 *
 ****************************************************************************/

static size_t llm_row_stride(const struct llm_tensor_s *t)
{
  switch (t->dtype)
    {
      case LLM_DTYPE_F32:
        return (size_t)t->cols * sizeof(float);

      case LLM_DTYPE_F16:
        return (size_t)t->cols * sizeof(uint16_t);

      case LLM_DTYPE_Q4_0:
      default:
        return (size_t)(t->cols / LLM_Q4_BLOCK_SIZE) * LLM_Q4_BLOCK_BYTES;
    }
}

/****************************************************************************
 * Name: llm_row_to_f32
 *
 * Description:
 *   Materialise one row of a tensor as fp32.  Used for the embedding table
 *   lookup, which is the only place a whole row is needed as a vector.
 *
 ****************************************************************************/

static void llm_row_to_f32(float *out, const struct llm_tensor_s *t, int row)
{
  const uint8_t *base = (const uint8_t *)t->data +
                        (size_t)row * llm_row_stride(t);
  int n = (int)t->cols;
  int i;

  switch (t->dtype)
    {
      case LLM_DTYPE_F32:
        memcpy(out, base, (size_t)n * sizeof(float));
        break;

      case LLM_DTYPE_F16:
        {
          const uint16_t *h = (const uint16_t *)base;

          for (i = 0; i < n; i++)
            {
              out[i] = llm_fp16_to_fp32(h[i]);
            }
        }
        break;

      case LLM_DTYPE_Q4_0:
      default:
        {
          int nblocks = n / LLM_Q4_BLOCK_SIZE;
          int b;

          for (b = 0; b < nblocks; b++)
            {
              const uint8_t *p = base + (size_t)b * LLM_Q4_BLOCK_BYTES;
              float *o = out + (size_t)b * LLM_Q4_BLOCK_SIZE;
              uint16_t raw;
              float d;
              int j;

              memcpy(&raw, p, sizeof(raw));
              d = llm_fp16_to_fp32(raw);
              p += sizeof(raw);

              for (j = 0; j < LLM_Q4_BLOCK_SIZE / 2; j++)
                {
                  o[j] = (float)((int)(p[j] & 0x0f) - 8) * d;
                  o[j + LLM_Q4_BLOCK_SIZE / 2] =
                      (float)((int)(p[j] >> 4) - 8) * d;
                }
            }
        }
        break;
    }
}

/****************************************************************************
 * Name: llm_row_range
 *
 * Description:
 *   Split rows into nthreads contiguous chunks and return the chunk owned
 *   by worker id.  The remainder is spread over the first chunks so no
 *   worker is more than one row behind.
 *
 ****************************************************************************/

static void llm_row_range(int rows, int nthreads, int id, int *begin,
                          int *end)
{
  int base = rows / nthreads;
  int rem = rows % nthreads;
  int first = id * base + (id < rem ? id : rem);
  int count = base + (id < rem ? 1 : 0);

  *begin = first;
  *end = first + count;
}

/****************************************************************************
 * Name: llm_worker
 *
 * Description:
 *   Pool worker.  Blocks on its private start semaphore, computes its slice
 *   of the published matmul job, then signals completion.
 *
 ****************************************************************************/

static void *llm_worker(void *argp)
{
  struct llm_worker_arg_s *arg = (struct llm_worker_arg_s *)argp;
  struct llm_pool_s *pool = arg->pool;
  const int id = arg->id;

  for (; ; )
    {
      int begin;
      int end;

      while (sem_wait(&pool->start[id]) < 0)
        {
          if (errno != EINTR)
            {
              return NULL;
            }
        }

      if (pool->quit)
        {
          break;
        }

      llm_row_range((int)pool->job_w->rows, pool->nthreads, id, &begin,
                    &end);
      llm_matmul_cpu_rows(pool->job_out, pool->job_x, pool->job_w, begin,
                          end);

      sem_post(&pool->done);
    }

  return NULL;
}

/****************************************************************************
 * Name: llm_pool_start
 *
 * Description:
 *   Create nthreads-1 worker threads; the caller is worker 0.
 *
 ****************************************************************************/

static int llm_pool_start(struct llm_pool_s *pool, int nthreads)
{
  pthread_attr_t attr;
  int i;
  int ret;

  if (nthreads < 1)
    {
      nthreads = 1;
    }

  if (nthreads > LLM_MAX_THREADS)
    {
      nthreads = LLM_MAX_THREADS;
    }

  pool->nthreads = nthreads;
  pool->quit = false;
  pool->running = false;

  if (nthreads == 1)
    {
      return OK;
    }

  if (sem_init(&pool->done, 0, 0) < 0)
    {
      ret = -errno;
      pool->nthreads = 1;
      return ret;
    }

  for (i = 1; i < nthreads; i++)
    {
      if (sem_init(&pool->start[i], 0, 0) < 0)
        {
          ret = -errno;

          while (--i >= 1)
            {
              sem_destroy(&pool->start[i]);
            }

          sem_destroy(&pool->done);
          pool->nthreads = 1;
          return ret;
        }
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, LLM_WORKER_STACKSIZE);

  for (i = 1; i < nthreads; i++)
    {
#ifdef CONFIG_SMP
      cpu_set_t cpuset;

      /* Spread the workers over the cluster.  Worker 0 is the caller and
       * keeps whatever affinity the shell gave it.
       */

      CPU_ZERO(&cpuset);
      CPU_SET(i % CONFIG_SMP_NCPUS, &cpuset);
      pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
#endif

      ret = pthread_create(&pool->tid[i], &attr, llm_worker,
                           &pool->arg[i]);
      if (ret != 0)
        {
          /* Tear the partially built pool down through the normal path so
           * the already running workers are joined.
           */

          pthread_attr_destroy(&attr);
          pool->nthreads = i;
          pool->running = true;
          llm_pool_stop(pool);

          for (; i < nthreads; i++)
            {
              sem_destroy(&pool->start[i]);
            }

          return -ret;
        }
    }

  pthread_attr_destroy(&attr);
  pool->running = true;
  return OK;
}

/****************************************************************************
 * Name: llm_pool_stop
 ****************************************************************************/

static void llm_pool_stop(struct llm_pool_s *pool)
{
  int i;

  if (!pool->running)
    {
      return;
    }

  pool->quit = true;

  for (i = 1; i < pool->nthreads; i++)
    {
      sem_post(&pool->start[i]);
    }

  for (i = 1; i < pool->nthreads; i++)
    {
      pthread_join(pool->tid[i], NULL);
      sem_destroy(&pool->start[i]);
    }

  sem_destroy(&pool->done);
  pool->running = false;
  pool->nthreads = 1;
}

/****************************************************************************
 * Name: llm_pool_run
 *
 * Description:
 *   Publish a matmul job, run chunk 0 on the calling thread and wait for
 *   the workers.  The semaphores order the publication against the worker
 *   reads, so no extra barrier is needed.
 *
 ****************************************************************************/

static void llm_pool_run(struct llm_pool_s *pool, float *out,
                         const float *x, const struct llm_tensor_s *w)
{
  int begin;
  int end;
  int i;

  pool->job_out = out;
  pool->job_x = x;
  pool->job_w = w;

  for (i = 1; i < pool->nthreads; i++)
    {
      sem_post(&pool->start[i]);
    }

  llm_row_range((int)w->rows, pool->nthreads, 0, &begin, &end);
  llm_matmul_cpu_rows(out, x, w, begin, end);

  for (i = 1; i < pool->nthreads; i++)
    {
      while (sem_wait(&pool->done) < 0)
        {
          if (errno != EINTR)
            {
              break;
            }
        }
    }
}

/****************************************************************************
 * Name: llm_npu_symbol_present
 *
 * Description:
 *   True when the RKNPU matmul entry point was linked in.  The address is
 *   taken through a volatile pointer so the compiler cannot fold the weak
 *   symbol test away.
 *
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_LLM_NPU
static bool llm_npu_symbol_present(void)
{
  int (* volatile fn)(uint32_t, const void *, const void *, void *,
                      uint32_t, uint32_t, uint32_t) = rk3576_rknn_matmul_run;

  return fn != NULL;
}

/****************************************************************************
 * Name: llm_dma_symbol_present
 ****************************************************************************/

static bool llm_dma_symbol_present(void)
{
  void *(* volatile fn)(size_t) = rk3576_dma_alloc;

  return fn != NULL;
}
#endif

/****************************************************************************
 * Name: llm_matmul_npu
 *
 * Description:
 *   Offload out = W . x to the NPU.  The activation is staged into the
 *   DMA-safe fp16 buffer and the result into the DMA-safe fp32 buffer; the
 *   weight is passed by reference because it is far too large to copy.
 *
 * Returned Value:
 *   OK when the NPU produced the result, a negated errno when the caller
 *   must fall back to the CPU kernel.
 *
 ****************************************************************************/

static int llm_matmul_npu(struct llm_model_s *model, float *out,
                          const float *x, const struct llm_tensor_s *w)
{
#ifdef CONFIG_EXAMPLES_LLM_NPU
  struct llm_state_s *s = &model->s;
  size_t abytes = (size_t)w->cols * sizeof(uint16_t);
  size_t obytes = (size_t)w->rows * sizeof(float);
  size_t wbytes = (size_t)w->rows * llm_row_stride(w);
  int ret;

  if (!llm_npu_symbol_present() || s->npu_act == NULL ||
      s->npu_out == NULL)
    {
      return -ENODEV;
    }

  if (w->dtype != LLM_DTYPE_Q4_0 ||
      (w->cols % LLM_NPU_K_ALIGN) != 0 ||
      (w->rows % LLM_NPU_N_ALIGN) != 0 ||
      (size_t)w->rows * w->cols < LLM_NPU_MIN_WORK)
    {
      return -EINVAL;
    }

  if (abytes > s->npu_act_bytes || obytes > s->npu_out_bytes)
    {
      return -ENOSPC;
    }

  /* The NPU address generators are 32 bit and Bank2 reaches past the 4GB
   * mark on a 4GB board, so a weight that landed high in the heap cannot
   * be handed over.  NuttX runs flat here, with RAM identity mapped, so
   * the virtual address is the physical one.
   *
   * TODO: route this through up_addrenv_va_to_pa() if an address
   * environment is ever enabled on this platform.
   */

  if ((uint64_t)(uintptr_t)w->data + wbytes > UINT32_MAX)
    {
      return -EFAULT;
    }

  llm_fp32_to_fp16_vec(s->npu_act, x, (int)w->cols);

  up_clean_dcache((uintptr_t)s->npu_act, (uintptr_t)s->npu_act + abytes);
  up_clean_dcache((uintptr_t)w->data, (uintptr_t)w->data + wbytes);

  ret = rk3576_rknn_matmul_run(LLM_NPU_MM_F16_I4_F32, s->npu_act, w->data,
                               s->npu_out, 1, w->cols, w->rows);
  if (ret < 0)
    {
      return ret;
    }

  up_invalidate_dcache((uintptr_t)s->npu_out,
                       (uintptr_t)s->npu_out + obytes);
  memcpy(out, s->npu_out, obytes);
  return OK;
#else
  (void)model;
  (void)out;
  (void)x;
  (void)w;
  return -ENOSYS;
#endif
}

/****************************************************************************
 * Name: llm_bias_add
 ****************************************************************************/

static void llm_bias_add(float *out, const struct llm_tensor_s *b, int n)
{
  const float *v;
  int i = 0;

  if (b->data == NULL)
    {
      return;
    }

  v = (const float *)b->data;

#ifdef LLM_HAVE_NEON
  for (; i + 4 <= n; i += 4)
    {
      vst1q_f32(out + i, vaddq_f32(vld1q_f32(out + i), vld1q_f32(v + i)));
    }
#endif

  for (; i < n; i++)
    {
      out[i] += v[i];
    }
}

/****************************************************************************
 * Name: llm_find_tensor
 ****************************************************************************/

static const struct llm_file_tensor_s *
llm_find_tensor(const struct llm_file_tensor_s *tab, uint32_t n,
                const char *name)
{
  uint32_t i;

  for (i = 0; i < n; i++)
    {
      if (strncmp(tab[i].name, name, LLM_TENSOR_NAME_MAX) == 0)
        {
          return &tab[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: llm_bind
 *
 * Description:
 *   Look a tensor up in the directory, validate its shape and payload
 *   extent, and point the in-memory view straight at the model image.
 *
 ****************************************************************************/

static int llm_bind(struct llm_model_s *model,
                    const struct llm_file_tensor_s *tab, uint32_t ntab,
                    struct llm_tensor_s *out, const char *name,
                    uint32_t rows, uint32_t cols, bool required)
{
  const struct llm_file_tensor_s *e = llm_find_tensor(tab, ntab, name);
  size_t expect;

  memset(out, 0, sizeof(*out));

  if (e == NULL)
    {
      if (required)
        {
          fprintf(stderr, "llm: missing tensor '%s'\n", name);
          return -ENOENT;
        }

      return OK;
    }

  if (e->rows != rows || e->cols != cols)
    {
      fprintf(stderr, "llm: tensor '%s' shape %" PRIu32 "x%" PRIu32
              ", expected %" PRIu32 "x%" PRIu32 "\n",
              name, e->rows, e->cols, rows, cols);
      return -EINVAL;
    }

  switch (e->dtype)
    {
      case LLM_DTYPE_F32:
        expect = (size_t)rows * cols * sizeof(float);
        break;

      case LLM_DTYPE_F16:
        expect = (size_t)rows * cols * sizeof(uint16_t);
        break;

      case LLM_DTYPE_Q4_0:
        if ((cols % LLM_Q4_BLOCK_SIZE) != 0)
          {
            fprintf(stderr, "llm: tensor '%s' cols %" PRIu32
                    " not a multiple of %d\n", name, cols,
                    LLM_Q4_BLOCK_SIZE);
            return -EINVAL;
          }

        expect = (size_t)rows * (cols / LLM_Q4_BLOCK_SIZE) *
                 LLM_Q4_BLOCK_BYTES;
        break;

      default:
        fprintf(stderr, "llm: tensor '%s' unknown dtype %" PRIu32 "\n",
                name, e->dtype);
        return -EINVAL;
    }

  if (e->nbytes != expect)
    {
      fprintf(stderr, "llm: tensor '%s' payload %" PRIu64 " bytes, "
              "expected %zu\n", name, (uint64_t)e->nbytes, expect);
      return -EINVAL;
    }

  if (e->offset > model->image_bytes ||
      e->nbytes > model->image_bytes - e->offset)
    {
      fprintf(stderr, "llm: tensor '%s' payload out of file\n", name);
      return -EINVAL;
    }

  out->data = (const uint8_t *)model->image + e->offset;
  out->rows = rows;
  out->cols = cols;
  out->dtype = (uint8_t)e->dtype;
  return OK;
}

/****************************************************************************
 * Name: llm_read_image
 *
 * Description:
 *   Read the whole model file into one 64-byte aligned buffer.  Weights are
 *   then referenced in place, so this is the only copy of the weights that
 *   ever exists.
 *
 ****************************************************************************/

static int llm_read_image(const char *path, void **image, size_t *bytes)
{
  struct stat st;
  uint8_t *buf;
  size_t total;
  size_t got = 0;
  size_t next_report = LLM_PROGRESS_STEP;
  int fd;
  int ret = OK;

  if (stat(path, &st) < 0)
    {
      ret = -errno;
      fprintf(stderr, "llm: stat %s failed: %d\n", path, ret);
      return ret;
    }

  if (st.st_size < (off_t)LLM_HEADER_BYTES)
    {
      fprintf(stderr, "llm: %s is too small to be a model\n", path);
      return -EINVAL;
    }

  total = (size_t)st.st_size;

  buf = (uint8_t *)memalign(LLM_IMAGE_ALIGN, total);
  if (buf == NULL)
    {
      fprintf(stderr, "llm: cannot allocate %zu bytes for the model\n",
              total);
      return -ENOMEM;
    }

  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      ret = -errno;
      fprintf(stderr, "llm: open %s failed: %d\n", path, ret);
      free(buf);
      return ret;
    }

  printf("llm: loading %s (%zu KiB)\n", path, total / 1024);

  while (got < total)
    {
      size_t want = total - got;
      ssize_t n;

      if (want > LLM_READ_CHUNK)
        {
          want = LLM_READ_CHUNK;
        }

      n = read(fd, buf + got, want);
      if (n < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          fprintf(stderr, "llm: read failed at %zu: %d\n", got, ret);
          break;
        }

      if (n == 0)
        {
          fprintf(stderr, "llm: short file, %zu of %zu bytes\n", got,
                  total);
          ret = -EIO;
          break;
        }

      got += (size_t)n;

      if (got >= next_report)
        {
          printf("llm:   %zu / %zu MiB\n", got >> 20, total >> 20);
          next_report += LLM_PROGRESS_STEP;
        }
    }

  close(fd);

  if (ret < 0)
    {
      free(buf);
      return ret;
    }

  *image = buf;
  *bytes = total;
  return OK;
}

/****************************************************************************
 * Name: llm_state_alloc
 *
 * Description:
 *   Allocate every activation buffer and the KV-cache once, so the decode
 *   loop never touches the allocator.
 *
 ****************************************************************************/

static int llm_state_alloc(struct llm_model_s *model)
{
  struct llm_config_s *cfg = &model->cfg;
  struct llm_state_s *s = &model->s;
  int wide = LLM_MAX(cfg->hidden_dim, cfg->q_dim);
  size_t kv_elems = (size_t)cfg->n_layers * cfg->max_seq_len * cfg->kv_dim;
  size_t total;
  int i;

  s->x = (float *)calloc((size_t)cfg->hidden_dim, sizeof(float));
  s->xb = (float *)calloc((size_t)wide, sizeof(float));
  s->xb2 = (float *)calloc((size_t)wide, sizeof(float));
  s->hb = (float *)calloc((size_t)cfg->ffn_dim, sizeof(float));
  s->hb2 = (float *)calloc((size_t)cfg->ffn_dim, sizeof(float));
  s->q = (float *)calloc((size_t)cfg->q_dim, sizeof(float));
  s->att = (float *)calloc((size_t)cfg->n_heads * cfg->max_seq_len,
                           sizeof(float));
  s->logits = (float *)calloc((size_t)cfg->vocab_size, sizeof(float));
  s->key_cache = (float *)calloc(kv_elems, sizeof(float));
  s->value_cache = (float *)calloc(kv_elems, sizeof(float));
  s->rope_freq = (float *)calloc((size_t)(cfg->head_dim / 2),
                                 sizeof(float));

  if (s->x == NULL || s->xb == NULL || s->xb2 == NULL || s->hb == NULL ||
      s->hb2 == NULL || s->q == NULL || s->att == NULL ||
      s->logits == NULL || s->key_cache == NULL ||
      s->value_cache == NULL || s->rope_freq == NULL)
    {
      fprintf(stderr, "llm: out of memory allocating the run state\n");
      return -ENOMEM;
    }

  /* Inverse RoPE frequencies depend only on the head geometry, so they are
   * tabulated once instead of being recomputed for every layer and token.
   */

  for (i = 0; i < cfg->head_dim / 2; i++)
    {
      s->rope_freq[i] = 1.0f / powf(cfg->rope_theta,
                                    (float)(2 * i) / (float)cfg->head_dim);
    }

  total = 2 * kv_elems * sizeof(float);
  printf("llm: kv-cache %zu KiB for %d tokens\n", total / 1024,
         cfg->max_seq_len);

#ifdef CONFIG_EXAMPLES_LLM_NPU
  /* Staging buffers for the NPU path have to come from the DMA heap so the
   * physical address stays below 4GB.  Failure is not fatal: the engine
   * simply keeps using the CPU kernel.
   */

  if (llm_dma_symbol_present())
    {
      int widest_in = LLM_MAX(cfg->hidden_dim, cfg->ffn_dim);
      int widest_out = LLM_MAX(LLM_MAX(cfg->vocab_size, cfg->ffn_dim),
                               LLM_MAX(cfg->hidden_dim, cfg->q_dim));

      s->npu_act_bytes = (size_t)widest_in * sizeof(uint16_t);
      s->npu_out_bytes = (size_t)widest_out * sizeof(float);
      s->npu_act = (uint16_t *)rk3576_dma_alloc(s->npu_act_bytes);
      s->npu_out = (float *)rk3576_dma_alloc(s->npu_out_bytes);

      if (s->npu_act == NULL || s->npu_out == NULL)
        {
          fprintf(stderr, "llm: no DMA memory for the NPU staging "
                  "buffers, CPU only\n");

          if (s->npu_act != NULL)
            {
              rk3576_dma_free(s->npu_act, s->npu_act_bytes);
              s->npu_act = NULL;
            }

          if (s->npu_out != NULL)
            {
              rk3576_dma_free(s->npu_out, s->npu_out_bytes);
              s->npu_out = NULL;
            }

          s->npu_act_bytes = 0;
          s->npu_out_bytes = 0;
        }
    }
#endif

  return OK;
}

/****************************************************************************
 * Name: llm_state_free
 ****************************************************************************/

static void llm_state_free(struct llm_model_s *model)
{
  struct llm_state_s *s = &model->s;

#ifdef CONFIG_EXAMPLES_LLM_NPU
  if (llm_dma_symbol_present())
    {
      if (s->npu_act != NULL)
        {
          rk3576_dma_free(s->npu_act, s->npu_act_bytes);
        }

      if (s->npu_out != NULL)
        {
          rk3576_dma_free(s->npu_out, s->npu_out_bytes);
        }
    }
#endif

  s->npu_act = NULL;
  s->npu_out = NULL;

  free(s->x);
  free(s->xb);
  free(s->xb2);
  free(s->hb);
  free(s->hb2);
  free(s->q);
  free(s->att);
  free(s->logits);
  free(s->key_cache);
  free(s->value_cache);
  free(s->rope_freq);
  memset(s, 0, sizeof(*s));
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_fp16_to_fp32
 ****************************************************************************/

float llm_fp16_to_fp32(uint16_t h)
{
  uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1fu;
  uint32_t man = h & 0x3ffu;
  uint32_t bits;
  float f;

  if (exp == 0)
    {
      if (man == 0)
        {
          bits = sign;
        }
      else
        {
          /* Subnormal fp16: renormalise into an fp32 normal number. */

          exp = 113;
          while ((man & 0x400u) == 0)
            {
              man <<= 1;
              exp--;
            }

          man &= 0x3ffu;
          bits = sign | (exp << 23) | (man << 13);
        }
    }
  else if (exp == 0x1fu)
    {
      bits = sign | 0x7f800000u | (man << 13);
    }
  else
    {
      bits = sign | ((exp + 112u) << 23) | (man << 13);
    }

  memcpy(&f, &bits, sizeof(f));
  return f;
}

/****************************************************************************
 * Name: llm_fp32_to_fp16
 *
 * Description:
 *   Round-half-up fp32 to fp16 conversion.  Used only for the NPU
 *   activation staging buffer, where the extra half ULP of error is far
 *   below the int4 weight quantisation error.
 *
 ****************************************************************************/

uint16_t llm_fp32_to_fp16(float f)
{
  uint32_t x;
  uint32_t sign;
  uint32_t man;
  uint32_t raw_exp;
  int32_t exp;

  memcpy(&x, &f, sizeof(x));
  sign = (x >> 16) & 0x8000u;
  raw_exp = (x >> 23) & 0xffu;
  man = x & 0x7fffffu;

  if (raw_exp == 0xffu)
    {
      return (uint16_t)(sign | 0x7c00u | (man != 0 ? 0x200u : 0u));
    }

  exp = (int32_t)raw_exp - 127 + 15;

  if (exp >= 31)
    {
      return (uint16_t)(sign | 0x7c00u);
    }

  if (exp <= 0)
    {
      uint32_t shift;
      uint32_t h;

      if (exp < -10)
        {
          return (uint16_t)sign;
        }

      man |= 0x800000u;
      shift = (uint32_t)(14 - exp);
      h = man >> shift;

      if (((man >> (shift - 1)) & 1u) != 0)
        {
          h++;
        }

      return (uint16_t)(sign | h);
    }

  {
    uint32_t h = ((uint32_t)exp << 10) | (man >> 13);

    if (((man >> 12) & 1u) != 0)
      {
        h++;
      }

    return (uint16_t)(sign | h);
  }
}

/****************************************************************************
 * Name: llm_fp32_to_fp16_vec
 ****************************************************************************/

void llm_fp32_to_fp16_vec(uint16_t *out, const float *in, int n)
{
  int i = 0;

#if defined(LLM_HAVE_NEON) && defined(__ARM_FP16_FORMAT_IEEE)
  /* vcvt_f16_f32 is part of the base ARMv8-A floating point, so it is
   * available on both the A72 and the A53 clusters.
   */

  for (; i + 4 <= n; i += 4)
    {
      float16x4_t h = vcvt_f16_f32(vld1q_f32(in + i));

      vst1_u16(out + i, vreinterpret_u16_f16(h));
    }
#endif

  for (; i < n; i++)
    {
      out[i] = llm_fp32_to_fp16(in[i]);
    }
}

/****************************************************************************
 * Name: llm_rmsnorm
 ****************************************************************************/

void llm_rmsnorm(float *out, const float *x, const float *weight, int size,
                 float eps)
{
  float ss = 0.0f;
  int i = 0;

#ifdef LLM_HAVE_NEON
  float32x4_t acc = vdupq_n_f32(0.0f);
  float32x4_t scalev;

  for (; i + 4 <= size; i += 4)
    {
      float32x4_t v = vld1q_f32(x + i);

      acc = vfmaq_f32(acc, v, v);
    }

  ss = vaddvq_f32(acc);
#endif

  for (; i < size; i++)
    {
      ss += x[i] * x[i];
    }

  ss = 1.0f / sqrtf(ss / (float)size + eps);
  i = 0;

#ifdef LLM_HAVE_NEON
  scalev = vdupq_n_f32(ss);

  for (; i + 4 <= size; i += 4)
    {
      float32x4_t v = vmulq_f32(vld1q_f32(x + i), scalev);

      vst1q_f32(out + i, vmulq_f32(vld1q_f32(weight + i), v));
    }
#endif

  for (; i < size; i++)
    {
      out[i] = weight[i] * (ss * x[i]);
    }
}

/****************************************************************************
 * Name: llm_softmax
 ****************************************************************************/

void llm_softmax(float *x, int size)
{
  float max = x[0];
  float sum = 0.0f;
  int i;

  for (i = 1; i < size; i++)
    {
      if (x[i] > max)
        {
          max = x[i];
        }
    }

  for (i = 0; i < size; i++)
    {
      x[i] = expf(x[i] - max);
      sum += x[i];
    }

  if (sum <= 0.0f)
    {
      sum = 1.0f;
    }

  sum = 1.0f / sum;

  for (i = 0; i < size; i++)
    {
      x[i] *= sum;
    }
}

/****************************************************************************
 * Name: llm_rope
 *
 * Description:
 *   Rotary position embedding applied in place to the query vector (q_dim
 *   values) and to the key vector that was just written into the cache
 *   (kv_dim values).
 *
 *   Two pairings exist in the wild.  HuggingFace LLaMA/Qwen2 use
 *   rotate_half, which pairs element i with element i + head_dim/2
 *   (LLM_FLAG_ROPE_NEOX).  The original Meta checkpoints and llama.cpp use
 *   the interleaved pairing (2i, 2i+1).  The model header selects one.
 *
 ****************************************************************************/

void llm_rope(struct llm_model_s *model, float *q, float *k, int pos)
{
  const struct llm_config_s *cfg = &model->cfg;
  const struct llm_state_s *s = &model->s;
  const int hd = cfg->head_dim;
  const int half = hd / 2;
  int head;
  int i;

  for (i = 0; i < half; i++)
    {
      float val = (float)pos * s->rope_freq[i];
      float fcr = cosf(val);
      float fci = sinf(val);
      int nvec = cfg->n_heads + cfg->n_kv_heads;

      for (head = 0; head < nvec; head++)
        {
          float *v;
          float x0;
          float x1;
          int a;
          int b;

          if (head < cfg->n_heads)
            {
              v = q + head * hd;
            }
          else
            {
              v = k + (head - cfg->n_heads) * hd;
            }

          if (cfg->rope_neox)
            {
              a = i;
              b = i + half;
            }
          else
            {
              a = 2 * i;
              b = 2 * i + 1;
            }

          x0 = v[a];
          x1 = v[b];
          v[a] = x0 * fcr - x1 * fci;
          v[b] = x1 * fcr + x0 * fci;
        }
    }
}

/****************************************************************************
 * Name: llm_swiglu
 *
 * Description:
 *   gate = silu(gate) * up, in place on gate.
 *
 ****************************************************************************/

void llm_swiglu(float *gate, const float *up, int size)
{
  int i;

  for (i = 0; i < size; i++)
    {
      float g = gate[i];

      g *= 1.0f / (1.0f + expf(-g));
      gate[i] = g * up[i];
    }
}

/****************************************************************************
 * Name: llm_attention
 *
 * Description:
 *   Grouped-query attention over the KV-cache for one layer at position
 *   pos.  Reads the query from state->q and leaves the concatenated head
 *   outputs in state->xb.
 *
 ****************************************************************************/

void llm_attention(struct llm_model_s *model, int layer, int pos)
{
  const struct llm_config_s *cfg = &model->cfg;
  struct llm_state_s *s = &model->s;
  const int hd = cfg->head_dim;
  const float scale = 1.0f / sqrtf((float)hd);
  const size_t loff = (size_t)layer * cfg->max_seq_len * cfg->kv_dim;
  int h;

  for (h = 0; h < cfg->n_heads; h++)
    {
      const float *q = s->q + (size_t)h * hd;
      float *att = s->att + (size_t)h * cfg->max_seq_len;
      const int kvh = h / cfg->kv_mul;
      const size_t hoff = loff + (size_t)kvh * hd;
      float *xb = s->xb + (size_t)h * hd;
      int t;

      for (t = 0; t <= pos; t++)
        {
          const float *k = s->key_cache + hoff +
                           (size_t)t * cfg->kv_dim;

          att[t] = llm_dot_f32(q, k, hd) * scale;
        }

      llm_softmax(att, pos + 1);
      memset(xb, 0, (size_t)hd * sizeof(float));

      for (t = 0; t <= pos; t++)
        {
          const float *v = s->value_cache + hoff +
                           (size_t)t * cfg->kv_dim;

          llm_accum_scaled(xb, v, att[t], hd);
        }
    }
}

/****************************************************************************
 * Name: llm_matmul_cpu_rows
 *
 * Description:
 *   out[r] = dot(W[r], x) for r in [row_begin, row_end).  This is the only
 *   place the weight storage format is interpreted on the CPU path.
 *
 ****************************************************************************/

void llm_matmul_cpu_rows(float *out, const float *x,
                         const struct llm_tensor_s *w, int row_begin,
                         int row_end)
{
  const int cols = (int)w->cols;
  const size_t stride = llm_row_stride(w);
  const uint8_t *base = (const uint8_t *)w->data;
  int r;

  switch (w->dtype)
    {
      case LLM_DTYPE_Q4_0:
        for (r = row_begin; r < row_end; r++)
          {
            out[r] = llm_dot_q4_0(base + (size_t)r * stride, x, cols);
          }
        break;

      case LLM_DTYPE_F32:
        for (r = row_begin; r < row_end; r++)
          {
            out[r] = llm_dot_f32((const float *)(base + (size_t)r * stride),
                                 x, cols);
          }
        break;

      case LLM_DTYPE_F16:
      default:
        for (r = row_begin; r < row_end; r++)
          {
            out[r] = llm_dot_f16((const uint16_t *)(base +
                                                    (size_t)r * stride),
                                 x, cols);
          }
        break;
    }
}

/****************************************************************************
 * Name: llm_matmul
 *
 * Description:
 *   out = W . x with the currently selected backend.  The NPU is tried
 *   first when it is eligible; any refusal falls through to the threaded
 *   CPU kernel, so the result is always produced.
 *
 ****************************************************************************/

void llm_matmul(struct llm_model_s *model, float *out, const float *x,
                const struct llm_tensor_s *w)
{
  /* Strict NPU mode still falls back when the NPU refuses a shape: the
   * result must always be produced.  The per-backend counters below make
   * the split visible so a CPU/NPU A/B run can be interpreted.
   */

  if (model->backend != LLM_BACKEND_CPU &&
      llm_matmul_npu(model, out, x, w) == OK)
    {
      model->npu_calls++;
      return;
    }

  model->cpu_calls++;

  if (model->pool.running &&
      (size_t)w->rows * w->cols >= LLM_THREAD_MIN_WORK &&
      (int)w->rows >= model->pool.nthreads)
    {
      llm_pool_run(&model->pool, out, x, w);
    }
  else
    {
      llm_matmul_cpu_rows(out, x, w, 0, (int)w->rows);
    }
}

/****************************************************************************
 * Name: llm_set_backend
 ****************************************************************************/

void llm_set_backend(struct llm_model_s *model, enum llm_backend_e backend)
{
  model->backend = backend;
}

/****************************************************************************
 * Name: llm_backend_name
 ****************************************************************************/

const char *llm_backend_name(enum llm_backend_e backend)
{
  switch (backend)
    {
      case LLM_BACKEND_CPU:
        return "cpu";

      case LLM_BACKEND_NPU:
        return "npu";

      case LLM_BACKEND_AUTO:
      default:
        return "auto";
    }
}

/****************************************************************************
 * Name: llm_npu_available
 ****************************************************************************/

bool llm_npu_available(void)
{
#ifdef CONFIG_EXAMPLES_LLM_NPU
  return llm_npu_symbol_present();
#else
  return false;
#endif
}

/****************************************************************************
 * Name: llm_forward
 ****************************************************************************/

float *llm_forward(struct llm_model_s *model, int token, int pos)
{
  struct llm_config_s *cfg = &model->cfg;
  struct llm_weights_s *w = &model->w;
  struct llm_state_s *s = &model->s;
  int l;
  int i;

  if (token < 0 || token >= cfg->vocab_size)
    {
      fprintf(stderr, "llm: token %d out of range\n", token);
      return NULL;
    }

  if (pos < 0 || pos >= cfg->max_seq_len)
    {
      fprintf(stderr, "llm: position %d exceeds the context window\n", pos);
      return NULL;
    }

  llm_row_to_f32(s->x, &w->tok_emb, token);

  for (l = 0; l < cfg->n_layers; l++)
    {
      struct llm_layer_s *ly = &w->layers[l];
      const size_t loff = (size_t)l * cfg->max_seq_len * cfg->kv_dim;
      float *kdst = s->key_cache + loff + (size_t)pos * cfg->kv_dim;
      float *vdst = s->value_cache + loff + (size_t)pos * cfg->kv_dim;

      llm_rmsnorm(s->xb, s->x, (const float *)ly->attn_norm.data,
                  cfg->hidden_dim, cfg->rms_eps);

      llm_matmul(model, s->q, s->xb, &ly->wq);
      llm_matmul(model, kdst, s->xb, &ly->wk);
      llm_matmul(model, vdst, s->xb, &ly->wv);

      llm_bias_add(s->q, &ly->bq, cfg->q_dim);
      llm_bias_add(kdst, &ly->bk, cfg->kv_dim);
      llm_bias_add(vdst, &ly->bv, cfg->kv_dim);

      llm_rope(model, s->q, kdst, pos);
      llm_attention(model, l, pos);

      llm_matmul(model, s->xb2, s->xb, &ly->wo);

      for (i = 0; i < cfg->hidden_dim; i++)
        {
          s->x[i] += s->xb2[i];
        }

      llm_rmsnorm(s->xb, s->x, (const float *)ly->ffn_norm.data,
                  cfg->hidden_dim, cfg->rms_eps);

      llm_matmul(model, s->hb, s->xb, &ly->w_gate);
      llm_matmul(model, s->hb2, s->xb, &ly->w_up);
      llm_swiglu(s->hb, s->hb2, cfg->ffn_dim);
      llm_matmul(model, s->xb2, s->hb, &ly->w_down);

      for (i = 0; i < cfg->hidden_dim; i++)
        {
          s->x[i] += s->xb2[i];
        }
    }

  llm_rmsnorm(s->x, s->x, (const float *)w->out_norm.data, cfg->hidden_dim,
              cfg->rms_eps);
  llm_matmul(model, s->logits, s->x, &w->output);

  return s->logits;
}

/****************************************************************************
 * Name: llm_model_load
 ****************************************************************************/

int llm_model_load(struct llm_model_s *model, const char *path,
                   int nthreads, int maxseq)
{
  const struct llm_file_tensor_s *tab;
  struct llm_file_header_s hdr;
  struct llm_config_s *cfg;
  char name[LLM_TENSOR_NAME_MAX];
  size_t table_bytes;
  int ret;
  int l;

  memset(model, 0, sizeof(*model));
  model->backend = LLM_BACKEND_CPU;

  ret = llm_read_image(path, &model->image, &model->image_bytes);
  if (ret < 0)
    {
      return ret;
    }

  memcpy(&hdr, model->image, sizeof(hdr));

  if (hdr.magic != LLM_MODEL_MAGIC)
    {
      fprintf(stderr, "llm: bad magic 0x%08" PRIx32 ", not a .nylm model\n",
              hdr.magic);
      ret = -EINVAL;
      goto err;
    }

  if (hdr.version != LLM_MODEL_VERSION)
    {
      fprintf(stderr, "llm: model version %" PRIu32 ", expected %u\n",
              hdr.version, LLM_MODEL_VERSION);
      ret = -EINVAL;
      goto err;
    }

  cfg = &model->cfg;
  cfg->hidden_dim = hdr.hidden_dim;
  cfg->ffn_dim = hdr.ffn_dim;
  cfg->n_layers = hdr.n_layers;
  cfg->n_heads = hdr.n_heads;
  cfg->n_kv_heads = hdr.n_kv_heads;
  cfg->head_dim = hdr.head_dim;
  cfg->vocab_size = hdr.vocab_size;
  cfg->max_seq_len = hdr.max_seq_len;
  cfg->rope_theta = hdr.rope_theta;
  cfg->rms_eps = hdr.rms_eps;
  cfg->tie_embed = (hdr.flags & LLM_FLAG_TIE_EMBED) != 0;
  cfg->qkv_bias = (hdr.flags & LLM_FLAG_QKV_BIAS) != 0;
  cfg->rope_neox = (hdr.flags & LLM_FLAG_ROPE_NEOX) != 0;

  if (cfg->hidden_dim <= 0 || cfg->ffn_dim <= 0 || cfg->n_layers <= 0 ||
      cfg->n_heads <= 0 || cfg->n_kv_heads <= 0 || cfg->head_dim <= 0 ||
      cfg->vocab_size <= 0 || cfg->max_seq_len <= 0 ||
      (cfg->head_dim % 2) != 0 ||
      (cfg->n_heads % cfg->n_kv_heads) != 0)
    {
      fprintf(stderr, "llm: inconsistent model configuration\n");
      ret = -EINVAL;
      goto err;
    }

  cfg->q_dim = cfg->n_heads * cfg->head_dim;
  cfg->kv_dim = cfg->n_kv_heads * cfg->head_dim;
  cfg->kv_mul = cfg->n_heads / cfg->n_kv_heads;

  if (maxseq > 0 && maxseq < cfg->max_seq_len)
    {
      cfg->max_seq_len = maxseq;
    }

  table_bytes = (size_t)hdr.n_tensors * LLM_FILE_TENSOR_BYTES;
  if (hdr.tensor_table_off > model->image_bytes ||
      table_bytes > model->image_bytes - hdr.tensor_table_off ||
      table_bytes != hdr.tensor_table_bytes)
    {
      fprintf(stderr, "llm: tensor directory out of file\n");
      ret = -EINVAL;
      goto err;
    }

  tab = (const struct llm_file_tensor_s *)((const uint8_t *)model->image +
                                           hdr.tensor_table_off);

  printf("llm: %d layers, hidden %d, ffn %d, heads %d/%d, vocab %d, "
         "ctx %d\n", cfg->n_layers, cfg->hidden_dim, cfg->ffn_dim,
         cfg->n_heads, cfg->n_kv_heads, cfg->vocab_size, cfg->max_seq_len);

  model->w.layers = (struct llm_layer_s *)
                    calloc((size_t)cfg->n_layers,
                           sizeof(struct llm_layer_s));
  if (model->w.layers == NULL)
    {
      ret = -ENOMEM;
      goto err;
    }

  ret = llm_bind(model, tab, hdr.n_tensors, &model->w.tok_emb, "tok_emb",
                 (uint32_t)cfg->vocab_size, (uint32_t)cfg->hidden_dim,
                 true);
  if (ret < 0)
    {
      goto err;
    }

  ret = llm_bind(model, tab, hdr.n_tensors, &model->w.out_norm, "out_norm",
                 1, (uint32_t)cfg->hidden_dim, true);
  if (ret < 0)
    {
      goto err;
    }

  if (model->w.out_norm.dtype != LLM_DTYPE_F32)
    {
      fprintf(stderr, "llm: norm tensors must be fp32\n");
      ret = -EINVAL;
      goto err;
    }

  if (cfg->tie_embed)
    {
      model->w.output = model->w.tok_emb;
    }
  else
    {
      ret = llm_bind(model, tab, hdr.n_tensors, &model->w.output, "output",
                     (uint32_t)cfg->vocab_size, (uint32_t)cfg->hidden_dim,
                     true);
      if (ret < 0)
        {
          goto err;
        }
    }

  for (l = 0; l < cfg->n_layers; l++)
    {
      struct llm_layer_s *ly = &model->w.layers[l];

      snprintf(name, sizeof(name), "blk.%d.attn_norm", l);
      ret = llm_bind(model, tab, hdr.n_tensors, &ly->attn_norm, name, 1,
                     (uint32_t)cfg->hidden_dim, true);
      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_q", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->wq, name,
                         (uint32_t)cfg->q_dim, (uint32_t)cfg->hidden_dim,
                         true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_k", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->wk, name,
                         (uint32_t)cfg->kv_dim, (uint32_t)cfg->hidden_dim,
                         true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_v", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->wv, name,
                         (uint32_t)cfg->kv_dim, (uint32_t)cfg->hidden_dim,
                         true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_o", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->wo, name,
                         (uint32_t)cfg->hidden_dim, (uint32_t)cfg->q_dim,
                         true);
        }

      if (ret == OK && cfg->qkv_bias)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_q_b", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->bq, name, 1,
                         (uint32_t)cfg->q_dim, true);
        }

      if (ret == OK && cfg->qkv_bias)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_k_b", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->bk, name, 1,
                         (uint32_t)cfg->kv_dim, true);
        }

      if (ret == OK && cfg->qkv_bias)
        {
          snprintf(name, sizeof(name), "blk.%d.attn_v_b", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->bv, name, 1,
                         (uint32_t)cfg->kv_dim, true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.ffn_norm", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->ffn_norm, name, 1,
                         (uint32_t)cfg->hidden_dim, true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.ffn_gate", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->w_gate, name,
                         (uint32_t)cfg->ffn_dim, (uint32_t)cfg->hidden_dim,
                         true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.ffn_up", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->w_up, name,
                         (uint32_t)cfg->ffn_dim, (uint32_t)cfg->hidden_dim,
                         true);
        }

      if (ret == OK)
        {
          snprintf(name, sizeof(name), "blk.%d.ffn_down", l);
          ret = llm_bind(model, tab, hdr.n_tensors, &ly->w_down, name,
                         (uint32_t)cfg->hidden_dim, (uint32_t)cfg->ffn_dim,
                         true);
        }

      if (ret < 0)
        {
          goto err;
        }

      if (ly->attn_norm.dtype != LLM_DTYPE_F32 ||
          ly->ffn_norm.dtype != LLM_DTYPE_F32)
        {
          fprintf(stderr, "llm: layer %d norms must be fp32\n", l);
          ret = -EINVAL;
          goto err;
        }
    }

  ret = llm_state_alloc(model);
  if (ret < 0)
    {
      goto err;
    }

  if (hdr.tokenizer_bytes > 0)
    {
      if (hdr.tokenizer_off > model->image_bytes ||
          hdr.tokenizer_bytes > model->image_bytes - hdr.tokenizer_off)
        {
          fprintf(stderr, "llm: tokenizer blob out of file\n");
          ret = -EINVAL;
          goto err;
        }

      ret = llm_tokenizer_init(&model->tok,
                               (const uint8_t *)model->image +
                               hdr.tokenizer_off, hdr.tokenizer_bytes,
                               hdr.bos_id, hdr.eos_id);
      if (ret < 0)
        {
          goto err;
        }
    }

  /* The worker arguments must be valid before any thread starts. */

  for (l = 0; l < LLM_MAX_THREADS; l++)
    {
      model->pool.arg[l].pool = &model->pool;
      model->pool.arg[l].id = l;
    }

  ret = llm_pool_start(&model->pool, nthreads > 0 ? nthreads : 1);
  if (ret < 0)
    {
      fprintf(stderr, "llm: worker pool failed (%d), running "
              "single threaded\n", ret);
    }

  printf("llm: %d thread(s), backend %s, npu %s\n", model->pool.nthreads,
         llm_backend_name(model->backend),
         llm_npu_available() ? "present" : "absent");

  return OK;

err:
  llm_state_free(model);
  free(model->w.layers);
  model->w.layers = NULL;
  free(model->image);
  model->image = NULL;
  model->image_bytes = 0;
  return ret;
}

/****************************************************************************
 * Name: llm_model_free
 ****************************************************************************/

void llm_model_free(struct llm_model_s *model)
{
  llm_pool_stop(&model->pool);
  llm_tokenizer_free(&model->tok);
  llm_state_free(model);
  free(model->w.layers);
  model->w.layers = NULL;
  free(model->image);
  model->image = NULL;
  model->image_bytes = 0;
}

/****************************************************************************
 * Name: llm_generate
 ****************************************************************************/

int llm_generate(struct llm_model_s *model, struct llm_sampler_s *sampler,
                 const struct llm_genopt_s *opt)
{
  struct llm_config_s *cfg = &model->cfg;
  char *text = NULL;
  int *tokens;
  uint64_t t_start;
  uint64_t t_prefill;
  uint64_t t_end;
  int n_prompt;
  int generated = 0;
  int token;
  int pos = 0;
  int ret = OK;

  if (model->tok.vocab == NULL)
    {
      fprintf(stderr, "llm: the model image carries no tokenizer\n");
      return -ENOENT;
    }

  tokens = (int *)malloc((size_t)cfg->max_seq_len * sizeof(int));
  if (tokens == NULL)
    {
      return -ENOMEM;
    }

  if (opt->chat)
    {
      static const char fmt[] =
        "<|im_start|>system\nYou are Nyabula, a helpful assistant running "
        "on openvela.<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n"
        "<|im_start|>assistant\n";
      size_t len = sizeof(fmt) + strlen(opt->prompt);

      text = (char *)malloc(len);
      if (text == NULL)
        {
          free(tokens);
          return -ENOMEM;
        }

      snprintf(text, len, fmt, opt->prompt);
    }

  n_prompt = llm_tokenizer_encode(&model->tok,
                                  text != NULL ? text : opt->prompt,
                                  opt->chat, tokens, cfg->max_seq_len);
  free(text);

  if (n_prompt <= 0)
    {
      fprintf(stderr, "llm: the prompt encoded to no tokens\n");
      free(tokens);
      return n_prompt < 0 ? n_prompt : -EINVAL;
    }

  model->npu_calls = 0;
  model->cpu_calls = 0;

  /* Prefill: run every prompt token through the network to fill the
   * KV-cache.  Only the logits of the last one are used.
   */

  t_start = llm_now_ms();

  for (pos = 0; pos < n_prompt; pos++)
    {
      if (llm_forward(model, tokens[pos], pos) == NULL)
        {
          free(tokens);
          return -EIO;
        }
    }

  t_prefill = llm_now_ms();

  token = llm_sample(sampler, model->s.logits);

  while (generated < opt->max_new && pos < cfg->max_seq_len)
    {
      const uint8_t *piece;
      int piece_len;

      if ((uint32_t)token == model->tok.eos_id)
        {
          break;
        }

      piece = llm_tokenizer_decode(&model->tok, token, &piece_len);
      if (piece != NULL && piece_len > 0 && !opt->quiet)
        {
          fwrite(piece, 1, (size_t)piece_len, stdout);
          fflush(stdout);
        }

      generated++;

      if (llm_forward(model, token, pos) == NULL)
        {
          ret = -EIO;
          break;
        }

      pos++;
      token = llm_sample(sampler, model->s.logits);
    }

  t_end = llm_now_ms();

  if (!opt->quiet)
    {
      printf("\n");
    }

  {
    uint64_t pre_ms = t_prefill - t_start;
    uint64_t dec_ms = t_end - t_prefill;

    printf("llm: prefill %d tok in %" PRIu64 " ms (%.2f tok/s)\n",
           n_prompt, pre_ms,
           pre_ms ? (double)n_prompt * 1000.0 / (double)pre_ms : 0.0);
    printf("llm: decode  %d tok in %" PRIu64 " ms (%.2f tok/s)\n",
           generated, dec_ms,
           dec_ms ? (double)generated * 1000.0 / (double)dec_ms : 0.0);
    printf("llm: matmul  npu %" PRIu32 " / cpu %" PRIu32 "\n",
           model->npu_calls, model->cpu_calls);
  }

  free(tokens);
  return ret < 0 ? ret : generated;
}
