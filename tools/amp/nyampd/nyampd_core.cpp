/****************************************************************************
 * tools/amp/nyampd/nyampd_core.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyampd_core.h"

#include "nyamp_protocol.h"
#include <cstring>

namespace nyamp
{
namespace
{

constexpr std::size_t kStatusPayloadSize = 4;
constexpr std::size_t kHealthPayloadSize = 12;
constexpr std::uint32_t kCapabilityHealth = 1U << 0;

void PutLe32(std::uint8_t *dest, std::uint32_t value)
{
  for (unsigned int index = 0; index < 4; ++index)
    {
      dest[index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

int EncodeResponse(const nyamp_header_s &request, Status status,
                   std::uint32_t generation, std::uint8_t *response,
                   std::size_t response_capacity, std::size_t *response_size,
                   std::string_view diagnostics = {})
{
  const bool health = status == Status::kOk &&
                      request.service == NYAMP_SERVICE_HEALTH &&
                      request.opcode == kHealthQuery;
  if (diagnostics.size() > NYAMP_INLINE_MAX - kStatusPayloadSize)
    {
      return NYAMP_EMSGSIZE;
    }

  const std::uint32_t payload_size = static_cast<std::uint32_t>(
      health ? kHealthPayloadSize : kStatusPayloadSize + diagnostics.size());
  nyamp_header_s header = {
    request.service,
    request.opcode,
    NYAMP_FLAG_RESPONSE | (status == Status::kOk ? 0U : NYAMP_FLAG_ERROR),
    request.request_id,
    request.deadline_ms,
    generation,
    payload_size,
  };

  if (response_size == nullptr)
    {
      return NYAMP_EINVAL;
    }

  const int result = nyamp_header_encode(response, response_capacity, &header);
  if (result != NYAMP_OK)
    {
      return result;
    }

  PutLe32(response + NYAMP_WIRE_HEADER_SIZE,
          static_cast<std::uint32_t>(status));
  if (health)
    {
      PutLe32(response + NYAMP_WIRE_HEADER_SIZE + 4, generation);
      PutLe32(response + NYAMP_WIRE_HEADER_SIZE + 8, kCapabilityHealth);
    }
  else if (!diagnostics.empty())
    {
      std::memcpy(response + NYAMP_WIRE_HEADER_SIZE + kStatusPayloadSize,
                  diagnostics.data(), diagnostics.size());
    }

  *response_size = NYAMP_WIRE_HEADER_SIZE + payload_size;
  return NYAMP_OK;
}

} // namespace

int Dispatch(const std::uint8_t *request_wire, std::size_t request_size,
             std::uint64_t now_ms, std::uint32_t generation,
             std::uint8_t *response, std::size_t response_capacity,
             std::size_t *response_size, std::string_view diagnostics)
{
  nyamp_header_s request;
  int result;

  if (request_wire == nullptr || response == nullptr ||
      response_size == nullptr)
    {
      return NYAMP_EINVAL;
    }

  result = nyamp_header_decode(&request, request_wire, request_size);
  if (result != NYAMP_OK)
    {
      return result;
    }

  if (request_size != NYAMP_WIRE_HEADER_SIZE + request.payload_size ||
      request.flags != NYAMP_FLAG_REQUEST)
    {
      return EncodeResponse(request, Status::kProtocol, generation, response,
                            response_capacity, response_size);
    }

  if (request.deadline_ms != 0 && now_ms > request.deadline_ms)
    {
      return EncodeResponse(request, Status::kDeadline, generation, response,
                            response_capacity, response_size);
    }

  if (request.generation != 0 && request.generation != generation)
    {
      return EncodeResponse(request, Status::kGeneration, generation, response,
                            response_capacity, response_size);
    }

  if (request.service != NYAMP_SERVICE_HEALTH ||
      (request.opcode != kHealthQuery && request.opcode != kInfoQuery) ||
      request.payload_size != 0 ||
      (request.opcode == kInfoQuery && diagnostics.empty()))
    {
      return EncodeResponse(request, Status::kUnsupported, generation,
                            response, response_capacity, response_size);
    }

  return EncodeResponse(request, Status::kOk, generation, response,
                        response_capacity, response_size,
                        request.opcode == kInfoQuery ? diagnostics
                                                     : std::string_view{});
}

} // namespace nyamp
