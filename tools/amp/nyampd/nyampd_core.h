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
};

int Dispatch(const std::uint8_t *request, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics = {});

} // namespace nyamp

#endif /* __TOOLS_AMP_NYAMPD_NYAMPD_CORE_H */
