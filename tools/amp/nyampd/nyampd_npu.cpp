/****************************************************************************
 * tools/amp/nyampd/nyampd_npu.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ****************************************************************************/

#include "nyampd_npu.h"
#include <rknn_matmul_api.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace nyamp
{
namespace
{
std::uint64_t Microseconds()
{
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::uint64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

bool ReadNpuInterrupts(std::uint64_t &total)
{
  std::FILE *file = std::fopen("/proc/interrupts", "r");
  if (file == nullptr)
    {
      return false;
    }
  char line[512];
  bool found = false;
  total = 0;
  while (std::fgets(line, sizeof(line), file))
    {
      if (std::strstr(line, "27700000.npu") == nullptr &&
          std::strstr(line, "rknpu") == nullptr)
        {
          continue;
        }
      char *cursor = std::strchr(line, ':');
      if (cursor == nullptr)
        {
          continue;
        }
      ++cursor;
      for (;;)
        {
          char *end;
          const auto count = std::strtoull(cursor, &end, 10);
          if (end == cursor)
            {
              break;
            }
          found = true;
          total += count;
          cursor = end;
        }
    }
  std::fclose(file);
  return found;
}
} // namespace

int RunNpuMatmul(std::uint32_t seed, NpuResult &result)
{
  rknn_matmul_ctx context = 0;
  rknn_matmul_info info{};
  rknn_matmul_io_attr attributes{};
  rknn_tensor_mem *a = nullptr;
  rknn_tensor_mem *b = nullptr;
  rknn_tensor_mem *c = nullptr;
  bool created = false;
  std::uint64_t irq_before = 0, irq_after = 0, run_started = 0;
  const std::uint64_t started = Microseconds();
  int ret;
  const auto inputs_intact = [&]() {
    for (unsigned int k = 0; k < NYAMP_NPU_K; ++k)
      {
        if (static_cast<std::int8_t *>(a->virt_addr)[k] !=
            static_cast<int>((k + seed) % 7) - 3)
          return false;
        for (unsigned int n = 0; n < NYAMP_NPU_N; ++n)
          if (static_cast<std::int8_t *>(b->virt_addr)[k * NYAMP_NPU_N + n] !=
              static_cast<int>((3 * k + n + seed % 5) % 9) - 4)
            return false;
      }
    return true;
  };

  info.M = NYAMP_NPU_M;
  info.K = NYAMP_NPU_K;
  info.N = NYAMP_NPU_N;
  info.type = RKNN_INT8_MM_INT8_TO_INT32;
  info.B_layout = RKNN_MM_LAYOUT_NORM;
  info.AC_layout = RKNN_MM_LAYOUT_NORM;

  result.stage = "rknn_matmul_create";
  ret = rknn_matmul_create(&context, &info, &attributes);
  if (ret != 0)
    {
      goto out;
    }
  created = true;

  result.stage = "returned matrix layout";
  if (info.B_layout != RKNN_MM_LAYOUT_NORM ||
      info.AC_layout != RKNN_MM_LAYOUT_NORM)
    {
      ret = -EPROTO;
      goto out;
    }

  result.stage = "tensor attributes";
  if (attributes.A.size < NYAMP_NPU_M * NYAMP_NPU_K ||
      attributes.B.size < NYAMP_NPU_K * NYAMP_NPU_N ||
      attributes.C.size < sizeof(result.values) ||
      attributes.C.type != RKNN_TENSOR_INT32)
    {
      ret = -EPROTO;
      goto out;
    }

  result.stage = "rknn_create_mem";
  a = rknn_create_mem(context, attributes.A.size);
  b = rknn_create_mem(context, attributes.B.size);
  c = rknn_create_mem(context, attributes.C.size);
  if (a == nullptr || b == nullptr || c == nullptr)
    {
      ret = -ENOMEM;
      goto out;
    }
  std::memset(a->virt_addr, 0, attributes.A.size);
  std::memset(b->virt_addr, 0, attributes.B.size);
  for (unsigned int k = 0; k < NYAMP_NPU_K; ++k)
    {
      static_cast<std::int8_t *>(a->virt_addr)[k] =
          static_cast<int>((k + seed) % 7) - 3;
      for (unsigned int n = 0; n < NYAMP_NPU_N; ++n)
        {
          static_cast<std::int8_t *>(b->virt_addr)[k * NYAMP_NPU_N + n] =
              static_cast<int>((3 * k + n + seed % 5) % 9) - 4;
        }
    }

  /* Use the runtime's default synchronization, as in its matmul example. */
  result.stage = "input changed before binding";
  if (!inputs_intact())
    {
      ret = -EBADMSG;
      goto out;
    }
  result.stage = "rknn_matmul_set_io_mem";
  ret = rknn_matmul_set_io_mem(context, a, &attributes.A);
  if (ret == 0)
    {
      ret = rknn_matmul_set_io_mem(context, b, &attributes.B);
    }
  if (ret == 0)
    {
      ret = rknn_matmul_set_io_mem(context, c, &attributes.C);
    }
  if (ret != 0)
    {
      goto out;
    }

  result.stage = "input changed after binding";
  if (!inputs_intact())
    {
      ret = -EBADMSG;
      goto out;
    }
  result.stage = "NPU interrupt counter";
  if (!ReadNpuInterrupts(irq_before))
    {
      ret = -ENODEV;
      goto out;
    }
  run_started = Microseconds();
  result.setup_us = static_cast<std::uint32_t>(run_started - started);
  result.stage = "rknn_matmul_run";
  ret = rknn_matmul_run(context);
  result.run_us = static_cast<std::uint32_t>(Microseconds() - run_started);
  if (ret != 0)
    {
      goto out;
    }
  /* Normal-layout output is CPU-visible when the blocking run returns.
   * Do not invalidate it again after the runtime's output conversion.
   */
  result.stage = "NPU interrupt completion";
  if (!ReadNpuInterrupts(irq_after) || irq_after <= irq_before)
    {
      ret = -EIO;
      goto out;
    }
  result.irq_delta = static_cast<std::uint32_t>(irq_after - irq_before);
  std::memcpy(result.values, c->virt_addr, sizeof(result.values));

out:
  if (c != nullptr)
    {
      rknn_destroy_mem(context, c);
    }
  if (b != nullptr)
    {
      rknn_destroy_mem(context, b);
    }
  if (a != nullptr)
    {
      rknn_destroy_mem(context, a);
    }
  if (created)
    {
      rknn_matmul_destroy(context);
    }
  return ret;
}
} // namespace nyamp
