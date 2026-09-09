/****************************************************************************
 * tools/amp/protocol/nyamp_protocol.h
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

#ifndef __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H
#define __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NYAMP_WIRE_MAGIC       0x5041594eU /* "NYAP" in little endian */
#define NYAMP_WIRE_VERSION     1U
#define NYAMP_WIRE_HEADER_SIZE 40U
#define NYAMP_RPMSG_MTU        496U
#define NYAMP_INLINE_MAX       (NYAMP_RPMSG_MTU - NYAMP_WIRE_HEADER_SIZE)

#define NYAMP_FLAG_REQUEST     (1U << 0)
#define NYAMP_FLAG_RESPONSE    (1U << 1)
#define NYAMP_FLAG_EVENT       (1U << 2)
#define NYAMP_FLAG_CANCEL      (1U << 3)
#define NYAMP_FLAG_ERROR       (1U << 4)
#define NYAMP_FLAG_KIND_MASK                                     \
  (NYAMP_FLAG_REQUEST | NYAMP_FLAG_RESPONSE | NYAMP_FLAG_EVENT | \
   NYAMP_FLAG_CANCEL)
#define NYAMP_FLAG_ALL     (NYAMP_FLAG_KIND_MASK | NYAMP_FLAG_ERROR)

#define NYAMP_HEALTH_READY 0U

enum nyamp_service_e
{
  NYAMP_SERVICE_HEALTH = 1,
  NYAMP_SERVICE_NPU = 2,
  NYAMP_SERVICE_ASR = 3,
  NYAMP_SERVICE_TTS = 4,
  NYAMP_SERVICE_VISION = 5,
  NYAMP_SERVICE_MEDIA = 6,
  NYAMP_SERVICE_HOME = 7,
};

enum nyamp_result_e
{
  NYAMP_OK = 0,
  NYAMP_EINVAL = -1,
  NYAMP_EMSGSIZE = -2,
  NYAMP_EPROTO = -3,
};

struct nyamp_header_s
{
  uint16_t service;
  uint16_t opcode;
  uint32_t flags;
  uint64_t request_id;
  uint64_t deadline_ms;
  uint32_t generation;
  uint32_t payload_size;
};

int nyamp_header_encode(uint8_t *wire, size_t wire_size,
                        const struct nyamp_header_s *header);
int nyamp_header_decode(struct nyamp_header_s *header, const uint8_t *wire,
                        size_t wire_size);

#ifdef __cplusplus
}
#endif

#endif /* __TOOLS_AMP_PROTOCOL_NYAMP_PROTOCOL_H */
