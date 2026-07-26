/****************************************************************************
 * app/llm/llm_main.c
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
 * NSH front end for the on-device LLM engine.
 *
 *   nsh> llm /data/qwen2.5-0.5b-q4.nylm "Tell me about openvela"
 *   nsh> llm -j 8 -t 0 -b auto /data/model.nylm "2 + 2 ="
 *
 * With -t 0 the decode is deterministic, which is what the CPU/NPU A/B
 * comparison needs: run the same prompt with -b cpu and -b auto and the
 * generated text must be identical while the tok/s differ.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "llm_transformer.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LLM_DEFAULT_STEPS    128
#define LLM_DEFAULT_TEMP     0.8f
#define LLM_DEFAULT_TOPK     40
#define LLM_DEFAULT_TOPP     0.9f
#define LLM_DEFAULT_THREADS  4
#define LLM_PROMPT_MAX       4096

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static void llm_usage(const char *progname);
static int  llm_parse_backend(const char *name, enum llm_backend_e *out);
static int  llm_join_prompt(char *dst, size_t dstlen, int argc,
                            char *argv[], int first);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: llm_usage
 ****************************************************************************/

static void llm_usage(const char *progname)
{
  fprintf(stderr,
          "Usage: %s [options] <model.nylm> [prompt words...]\n"
          "  -n <int>    tokens to generate      (default %d)\n"
          "  -t <float>  temperature, 0 = greedy (default %.2f)\n"
          "  -k <int>    top-k, 0 = disabled     (default %d)\n"
          "  -p <float>  top-p nucleus mass      (default %.2f)\n"
          "  -s <int>    random seed             (default from the clock)\n"
          "  -j <int>    worker threads 1..%d    (default %d)\n"
          "  -c <int>    cap the context window  (default from the model)\n"
          "  -b <name>   matmul backend cpu|npu|auto (default cpu)\n"
          "  -C          wrap the prompt in the ChatML chat template\n"
          "  -q          do not stream the generated tokens\n",
          progname, LLM_DEFAULT_STEPS, (double)LLM_DEFAULT_TEMP,
          LLM_DEFAULT_TOPK, (double)LLM_DEFAULT_TOPP, LLM_MAX_THREADS,
          LLM_DEFAULT_THREADS);
}

/****************************************************************************
 * Name: llm_parse_backend
 ****************************************************************************/

static int llm_parse_backend(const char *name, enum llm_backend_e *out)
{
  if (strcmp(name, "cpu") == 0)
    {
      *out = LLM_BACKEND_CPU;
    }
  else if (strcmp(name, "npu") == 0)
    {
      *out = LLM_BACKEND_NPU;
    }
  else if (strcmp(name, "auto") == 0)
    {
      *out = LLM_BACKEND_AUTO;
    }
  else
    {
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: llm_join_prompt
 *
 * Description:
 *   Join the remaining command line words into a single space separated
 *   prompt.  NSH has already removed the quotes, so this restores the
 *   spacing the user typed.
 *
 ****************************************************************************/

static int llm_join_prompt(char *dst, size_t dstlen, int argc,
                           char *argv[], int first)
{
  size_t used = 0;
  int i;

  dst[0] = '\0';

  for (i = first; i < argc; i++)
    {
      size_t need = strlen(argv[i]) + (used > 0 ? 1 : 0);

      if (used + need + 1 > dstlen)
        {
          return -E2BIG;
        }

      if (used > 0)
        {
          dst[used++] = ' ';
        }

      strcpy(dst + used, argv[i]);
      used += strlen(argv[i]);
    }

  return (int)used;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct llm_model_s model;
  struct llm_sampler_s sampler;
  struct llm_genopt_s opt;
  enum llm_backend_e backend = LLM_BACKEND_CPU;
  const char *path = NULL;
  char *prompt;
  float temperature = LLM_DEFAULT_TEMP;
  float topp = LLM_DEFAULT_TOPP;
  uint64_t seed = 0;
  int steps = LLM_DEFAULT_STEPS;
  int topk = LLM_DEFAULT_TOPK;
  int threads = LLM_DEFAULT_THREADS;
  int maxseq = 0;
  bool chat = false;
  bool quiet = false;
  int i;
  int ret;

  for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != '\0'; i++)
    {
      const char *opt_arg = NULL;
      char flag = argv[i][1];

      if (strchr("ntkpsjcb", flag) != NULL)
        {
          if (i + 1 >= argc)
            {
              fprintf(stderr, "llm: -%c needs an argument\n", flag);
              llm_usage(argv[0]);
              return EXIT_FAILURE;
            }

          opt_arg = argv[++i];
        }

      switch (flag)
        {
          case 'n':
            steps = atoi(opt_arg);
            break;

          case 't':
            temperature = (float)strtod(opt_arg, NULL);
            break;

          case 'k':
            topk = atoi(opt_arg);
            break;

          case 'p':
            topp = (float)strtod(opt_arg, NULL);
            break;

          case 's':
            seed = (uint64_t)strtoull(opt_arg, NULL, 0);
            break;

          case 'j':
            threads = atoi(opt_arg);
            break;

          case 'c':
            maxseq = atoi(opt_arg);
            break;

          case 'b':
            if (llm_parse_backend(opt_arg, &backend) < 0)
              {
                fprintf(stderr, "llm: unknown backend '%s'\n", opt_arg);
                llm_usage(argv[0]);
                return EXIT_FAILURE;
              }
            break;

          case 'C':
            chat = true;
            break;

          case 'q':
            quiet = true;
            break;

          case 'h':
            llm_usage(argv[0]);
            return EXIT_SUCCESS;

          default:
            fprintf(stderr, "llm: unknown option -%c\n", flag);
            llm_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

  if (i >= argc)
    {
      llm_usage(argv[0]);
      return EXIT_FAILURE;
    }

  path = argv[i++];

  if (steps <= 0)
    {
      steps = LLM_DEFAULT_STEPS;
    }

  if (threads < 1 || threads > LLM_MAX_THREADS)
    {
      threads = LLM_DEFAULT_THREADS;
    }

  if (seed == 0)
    {
      struct timespec ts;

      clock_gettime(CLOCK_REALTIME, &ts);
      seed = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }

  prompt = (char *)malloc(LLM_PROMPT_MAX);
  if (prompt == NULL)
    {
      fprintf(stderr, "llm: out of memory\n");
      return EXIT_FAILURE;
    }

  if (llm_join_prompt(prompt, LLM_PROMPT_MAX, argc, argv, i) < 0)
    {
      fprintf(stderr, "llm: the prompt exceeds %d bytes\n",
              LLM_PROMPT_MAX);
      free(prompt);
      return EXIT_FAILURE;
    }

  if (prompt[0] == '\0')
    {
      strcpy(prompt, "Hello");
    }

  ret = llm_model_load(&model, path, threads, maxseq);
  if (ret < 0)
    {
      fprintf(stderr, "llm: cannot load %s: %d\n", path, ret);
      free(prompt);
      return EXIT_FAILURE;
    }

  if (backend != LLM_BACKEND_CPU && !llm_npu_available())
    {
      printf("llm: no NPU matmul backend linked in, using the CPU "
             "kernels\n");
      backend = LLM_BACKEND_CPU;
    }

  llm_set_backend(&model, backend);

  ret = llm_sampler_init(&sampler, model.cfg.vocab_size, temperature, topk,
                         topp, seed);
  if (ret < 0)
    {
      fprintf(stderr, "llm: sampler init failed: %d\n", ret);
      llm_model_free(&model);
      free(prompt);
      return EXIT_FAILURE;
    }

  memset(&opt, 0, sizeof(opt));
  opt.prompt = prompt;
  opt.max_new = steps;
  opt.chat = chat;
  opt.quiet = quiet;

  printf("llm: backend %s, temp %.2f, top-k %d, top-p %.2f, seed "
         "%llu\n", llm_backend_name(backend), (double)temperature, topk,
         (double)topp, (unsigned long long)seed);

  ret = llm_generate(&model, &sampler, &opt);
  if (ret < 0)
    {
      fprintf(stderr, "llm: generation failed: %d\n", ret);
    }

  llm_sampler_free(&sampler);
  llm_model_free(&model);
  free(prompt);

  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
