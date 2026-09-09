/****************************************************************************
 * tools/amp/nyampd/nyampd_core_test.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyampd_core.h"

#include "nyamp_protocol.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#define CHECK(expression)                                                 \
  do                                                                      \
    {                                                                     \
      if (!(expression))                                                  \
        {                                                                 \
          std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                       #expression);                                      \
          return 1;                                                       \
        }                                                                 \
    }                                                                     \
  while (0)

namespace
{

std::int32_t GetLe32(const std::uint8_t *source)
{
  std::uint32_t value = 0;
  for (unsigned int index = 0; index < 4; ++index)
    {
      value |= static_cast<std::uint32_t>(source[index]) << (index * 8);
    }

  return static_cast<std::int32_t>(value);
}

int Exchange(nyamp_header_s request, std::uint64_t now,
             std::uint32_t generation, nyamp_header_s *response_header,
             std::int32_t *status)
{
  std::uint8_t request_wire[NYAMP_RPMSG_MTU] = {};
  std::uint8_t response_wire[NYAMP_RPMSG_MTU] = {};
  std::size_t response_size = 0;

  CHECK(nyamp_header_encode(request_wire, sizeof(request_wire), &request) ==
        NYAMP_OK);
  CHECK(nyamp::Dispatch(request_wire,
                        NYAMP_WIRE_HEADER_SIZE + request.payload_size, now,
                        generation, response_wire, sizeof(response_wire),
                        &response_size) == NYAMP_OK);
  CHECK(nyamp_header_decode(response_header, response_wire, response_size) ==
        NYAMP_OK);
  *status = GetLe32(response_wire + NYAMP_WIRE_HEADER_SIZE);
  return 0;
}
} // namespace

int main()
{
  constexpr std::uint32_t generation = 17;
  nyamp_header_s request = {
    NYAMP_SERVICE_HEALTH,
    nyamp::kHealthQuery,
    NYAMP_FLAG_REQUEST,
    42,
    1000,
    0,
    0,
  };
  nyamp_header_s response;
  std::int32_t status;

  CHECK(Exchange(request, 500, generation, &response, &status) == 0);
  CHECK(status == static_cast<std::int32_t>(nyamp::Status::kOk));
  CHECK(response.flags == NYAMP_FLAG_RESPONSE);
  CHECK(response.generation == generation);
  CHECK(response.payload_size == 12);

  request.deadline_ms = 499;
  CHECK(Exchange(request, 500, generation, &response, &status) == 0);
  CHECK(status == static_cast<std::int32_t>(nyamp::Status::kDeadline));
  CHECK((response.flags & NYAMP_FLAG_ERROR) != 0);

  request.deadline_ms = 1000;
  request.generation = generation - 1;
  CHECK(Exchange(request, 500, generation, &response, &status) == 0);
  CHECK(status == static_cast<std::int32_t>(nyamp::Status::kGeneration));

  request.generation = generation;
  request.service = NYAMP_SERVICE_NPU;
  CHECK(Exchange(request, 500, generation, &response, &status) == 0);
  CHECK(status == static_cast<std::int32_t>(nyamp::Status::kUnsupported));

  request.service = NYAMP_SERVICE_HEALTH;
  request.opcode = nyamp::kInfoQuery;
  request.generation = generation;
  {
    std::uint8_t input[NYAMP_RPMSG_MTU] = {};
    std::uint8_t output[NYAMP_RPMSG_MTU] = {};
    std::size_t size = 0;
    constexpr char info[] = "online=4\ncpu0 part=0xd08\n";
    CHECK(nyamp_header_encode(input, sizeof(input), &request) == NYAMP_OK);
    CHECK(nyamp::Dispatch(input, NYAMP_WIRE_HEADER_SIZE, 500, generation,
                          output, sizeof(output), &size, info) == NYAMP_OK);
    CHECK(size == NYAMP_WIRE_HEADER_SIZE + 4 + std::strlen(info));
    CHECK(std::memcmp(output + NYAMP_WIRE_HEADER_SIZE + 4, info,
                      std::strlen(info)) == 0);
    CHECK(nyamp::Dispatch(input, NYAMP_WIRE_HEADER_SIZE, 500, generation,
                          output, NYAMP_WIRE_HEADER_SIZE + 4, &size,
                          info) == NYAMP_EMSGSIZE);
  }

  std::puts("nyampd core tests passed");
  return 0;
}
