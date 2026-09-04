/****************************************************************************
 * tools/amp/protocol/nyamp_protocol_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyamp_protocol.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                            \
  do                                                                 \
    {                                                                \
      if (!(expression))                                             \
        {                                                            \
          fprintf(stderr, "check failed at line %d: %s\n", __LINE__, \
                  #expression);                                      \
          return 1;                                                  \
        }                                                            \
    }                                                                \
  while (0)

static int test_round_trip(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s input = {
    .service = NYAMP_SERVICE_NPU,
    .opcode = 3,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 0x1122334455667788ULL,
    .deadline_ms = 1234567,
    .generation = 9,
    .payload_size = 32,
  };
  struct nyamp_header_s output;

  memset(wire, 0xa5, sizeof(wire));
  CHECK(nyamp_header_encode(wire, sizeof(wire), &input) == NYAMP_OK);
  CHECK(nyamp_header_decode(&output, wire,
                            NYAMP_WIRE_HEADER_SIZE + input.payload_size) ==
        NYAMP_OK);
  CHECK(memcmp(&input, &output, sizeof(input)) == 0);
  return 0;
}

static int test_rejections(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU] = { 0 };
  struct nyamp_header_s header = {
    .service = NYAMP_SERVICE_HEALTH,
    .opcode = 1,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 1,
    .deadline_ms = 100,
    .generation = 1,
    .payload_size = 0,
  };
  struct nyamp_header_s decoded;

  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_OK);
  CHECK(nyamp_header_decode(&decoded, wire, NYAMP_WIRE_HEADER_SIZE - 1) ==
        NYAMP_EMSGSIZE);

  wire[0] ^= 1;
  CHECK(nyamp_header_decode(&decoded, wire, sizeof(wire)) == NYAMP_EPROTO);
  wire[0] ^= 1;

  header.flags = NYAMP_FLAG_REQUEST | NYAMP_FLAG_RESPONSE;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_EVENT | NYAMP_FLAG_ERROR;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_CANCEL;
  header.payload_size = 1;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EPROTO);

  header.flags = NYAMP_FLAG_REQUEST;
  header.payload_size = NYAMP_INLINE_MAX + 1;
  CHECK(nyamp_header_encode(wire, sizeof(wire), &header) == NYAMP_EMSGSIZE);
  return 0;
}

static int test_malformed_inputs(void)
{
  uint8_t wire[NYAMP_RPMSG_MTU];
  struct nyamp_header_s decoded;
  uint32_t state = 0x9e3779b9U;
  unsigned int iteration;
  unsigned int index;

  for (iteration = 0; iteration < 10000; iteration++)
    {
      for (index = 0; index < sizeof(wire); index++)
        {
          state ^= state << 13;
          state ^= state >> 17;
          state ^= state << 5;
          wire[index] = (uint8_t)state;
        }

      CHECK(nyamp_header_decode(&decoded, wire, sizeof(wire)) != NYAMP_OK);
    }

  return 0;
}

int main(void)
{
  if (test_round_trip() != 0 || test_rejections() != 0 ||
      test_malformed_inputs() != 0)
    {
      return 1;
    }
  puts("nyamp protocol tests passed");
  return 0;
}
