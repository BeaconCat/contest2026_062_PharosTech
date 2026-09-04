/****************************************************************************
 * app/nyampctl/nyampctl_main.c
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

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/rpmsg/rpmsg.h>

#include "nyamp_protocol.h"

#define NYAMPCTL_CTRL_PATH       "/dev/rpmsg/linux"
#define NYAMPCTL_ENDPOINT_NAME   "rpmsg-raw"
#define NYAMPCTL_ENDPOINT_PATH   "/dev/rpmsg-rpmsg-raw"
#define NYAMPCTL_OPEN_RETRIES    50
#define NYAMPCTL_OPEN_DELAY_US   100000
#define NYAMPCTL_RESPONSE_MS     5000
#define NYAMPCTL_HEALTH_OPCODE   1
#define NYAMPCTL_HEALTH_RESPONSE 12

static uint32_t nyampctl_get_le32(const uint8_t *source)
{
  uint32_t value = 0;
  unsigned int index;

  for (index = 0; index < 4; index++)
    {
      value |= (uint32_t)source[index] << (index * 8);
    }

  return value;
}

static int nyampctl_open_endpoint(void)
{
  int retry;
  int fd;

  for (retry = 0; retry < NYAMPCTL_OPEN_RETRIES; retry++)
    {
      fd = open(NYAMPCTL_ENDPOINT_PATH, O_RDWR | O_NONBLOCK);
      if (fd >= 0)
        {
          return fd;
        }

      usleep(NYAMPCTL_OPEN_DELAY_US);
    }

  return -errno;
}

static int nyampctl_health(int fd)
{
  struct nyamp_header_s request = {
    .service = NYAMP_SERVICE_HEALTH,
    .opcode = NYAMPCTL_HEALTH_OPCODE,
    .flags = NYAMP_FLAG_REQUEST,
    .request_id = 1,
    .deadline_ms = 0,
    .generation = 0,
    .payload_size = 0,
  };
  struct nyamp_header_s response;
  struct pollfd pollfd;
  uint8_t wire[NYAMP_RPMSG_MTU];
  ssize_t size;
  int ret;

  ret = nyamp_header_encode(wire, sizeof(wire), &request);
  if (ret != NYAMP_OK)
    {
      return ret;
    }

  size = write(fd, wire, NYAMP_WIRE_HEADER_SIZE);
  if (size != (ssize_t)NYAMP_WIRE_HEADER_SIZE)
    {
      return size < 0 ? -errno : -EIO;
    }

  pollfd.fd = fd;
  pollfd.events = POLLIN;
  pollfd.revents = 0;
  ret = poll(&pollfd, 1, NYAMPCTL_RESPONSE_MS);
  if (ret <= 0)
    {
      return ret == 0 ? -ETIMEDOUT : -errno;
    }

  size = read(fd, wire, sizeof(wire));
  if (size < 0)
    {
      return -errno;
    }

  ret = nyamp_header_decode(&response, wire, (size_t)size);
  if (ret != NYAMP_OK || response.request_id != request.request_id ||
      response.flags != NYAMP_FLAG_RESPONSE ||
      response.payload_size != NYAMPCTL_HEALTH_RESPONSE)
    {
      return -EPROTO;
    }

  if (nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE) != 0)
    {
      return -EREMOTEIO;
    }

  printf("nyamp health ok: generation=%" PRIu32 " capabilities=0x%08" PRIx32
         "\n",
         nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 4),
         nyampctl_get_le32(wire + NYAMP_WIRE_HEADER_SIZE + 8));
  return 0;
}

int main(int argc, char *argv[])
{
  struct rpmsg_endpoint_info info;
  int ctrl;
  int fd;
  int ret;

  if (argc != 2 || strcmp(argv[1], "health") != 0)
    {
      fprintf(stderr, "usage: %s health\n", argv[0]);
      return 2;
    }

  ctrl = open(NYAMPCTL_CTRL_PATH, O_RDWR);
  if (ctrl < 0)
    {
      fprintf(stderr, "nyampctl: open %s failed: %d\n", NYAMPCTL_CTRL_PATH,
              errno);
      return 1;
    }

  memset(&info, 0, sizeof(info));
  strlcpy(info.name, NYAMPCTL_ENDPOINT_NAME, sizeof(info.name));
  info.src = RPMSG_ADDR_ANY;
  info.dst = RPMSG_ADDR_ANY;

  ret = ioctl(ctrl, RPMSG_CREATE_DEV_IOCTL, (unsigned long)&info);
  if (ret < 0 && errno != EEXIST)
    {
      fprintf(stderr, "nyampctl: create endpoint failed: %d\n", errno);
      close(ctrl);
      return 1;
    }

  fd = nyampctl_open_endpoint();
  if (fd < 0)
    {
      fprintf(stderr, "nyampctl: endpoint did not bind: %d\n", -fd);
      ret = fd;
    }
  else
    {
      ret = nyampctl_health(fd);
      close(fd);
    }

  close(ctrl);

  if (ret < 0)
    {
      fprintf(stderr, "nyampctl: health failed: %d\n", ret);
      return 1;
    }

  return 0;
}
