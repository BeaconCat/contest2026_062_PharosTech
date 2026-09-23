/****************************************************************************
 * tools/amp/nyampd/nyampd_core.h
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H
#define __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "nyamp_protocol.h"

namespace nyamp
{

constexpr std::uint16_t kHealthQuery = 1;
constexpr std::uint16_t kInfoQuery = 2;

enum class Status : std::int32_t
{
  kOk = 0,
  kProtocol = -1,
  kDeadline = -2,
  kGeneration = -3,
  kUnsupported = -4,
  kBackend = -5,
};

struct NpuResult
{
  const char *stage = "backend";
  std::uint32_t setup_us = 0;
  std::uint32_t run_us = 0;
  std::uint32_t irq_delta = 0;
  std::int32_t values[NYAMP_NPU_N]{};
};

using NpuMatmul = int (*)(std::uint32_t seed, NpuResult &result);

int Dispatch(const std::uint8_t *request, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics = {},
             NpuMatmul npu = nullptr);

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H */
