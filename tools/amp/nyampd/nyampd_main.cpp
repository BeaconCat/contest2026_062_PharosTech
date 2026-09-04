/****************************************************************************
 * tools/amp/nyampd/nyampd_main.cpp
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include "nyampd_core.h"

#include "nyamp_protocol.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/random.h>
#include <unistd.h>

namespace
{

std::uint64_t SharedCounterMilliseconds()
{
#if defined(__aarch64__)
  std::uint64_t counter;
  std::uint64_t frequency;

  asm volatile("mrs %0, cntvct_el0" : "=r"(counter));
  asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  return frequency == 0 ? 0 : counter / (frequency / 1000);
#else
  return 0;
#endif
}

std::uint32_t NewGeneration()
{
  std::uint32_t generation = 0;

  if (getrandom(&generation, sizeof(generation), 0) != sizeof(generation) ||
      generation == 0)
    {
      generation = static_cast<std::uint32_t>(getpid()) ^ 0x4e594150U;
      if (generation == 0)
        {
          generation = 1;
        }
    }

  return generation;
}

int FindDevice(char *path, std::size_t path_size)
{
  DIR *directory = opendir("/sys/class/rpmsg");

  if (directory == nullptr)
    {
      return -errno;
    }

  for (dirent *entry = readdir(directory); entry != nullptr;
       entry = readdir(directory))
    {
      char name_path[PATH_MAX];
      char name[64];
      std::FILE *file;

      if (std::strncmp(entry->d_name, "rpmsg", 5) != 0 ||
          std::strstr(entry->d_name, "ctrl") != nullptr)
        {
          continue;
        }

      std::snprintf(name_path, sizeof(name_path),
                    "/sys/class/rpmsg/%s/name", entry->d_name);
      file = std::fopen(name_path, "r");
      if (file == nullptr)
        {
          continue;
        }

      const char *read = std::fgets(name, sizeof(name), file);
      std::fclose(file);
      if (read == nullptr || std::strncmp(name, "rpmsg-raw", 9) != 0)
        {
          continue;
        }

      std::snprintf(path, path_size, "/dev/%s", entry->d_name);
      closedir(directory);
      return 0;
    }

  closedir(directory);
  return -ENOENT;
}

int Run(const char *requested_device)
{
  std::uint8_t request[NYAMP_RPMSG_MTU];
  std::uint8_t response[NYAMP_RPMSG_MTU];
  const std::uint32_t generation = NewGeneration();
  char discovered[PATH_MAX];
  const char *device = requested_device;
  int fd;

  if (device == nullptr)
    {
      const int result = FindDevice(discovered, sizeof(discovered));
      if (result < 0)
        {
          std::fprintf(stderr, "nyampd: rpmsg-raw endpoint not found: %d\n",
                       -result);
          return 1;
        }

      device = discovered;
    }

  fd = open(device, O_RDWR | O_CLOEXEC);

  if (fd < 0)
    {
      std::fprintf(stderr, "nyampd: open %s failed: %s\n", device,
                   std::strerror(errno));
      return 1;
    }

  std::fprintf(stderr, "nyampd: generation=%u device=%s\n", generation,
               device);

  for (;;)
    {
      const ssize_t received = read(fd, request, sizeof(request));
      std::size_t response_size = 0;

      if (received < 0 && errno == EINTR)
        {
          continue;
        }

      if (received <= 0)
        {
          std::fprintf(stderr, "nyampd: transport disconnected: %s\n",
                       received == 0 ? "end of file" : std::strerror(errno));
          close(fd);
          return 1;
        }

      const int result = nyamp::Dispatch(
        request, static_cast<std::size_t>(received),
        SharedCounterMilliseconds(), generation, response, sizeof(response),
        &response_size);
      if (result != NYAMP_OK)
        {
          std::fprintf(stderr, "nyampd: malformed request: %d\n", result);
          continue;
        }

      ssize_t written;
      do
        {
          written = write(fd, response, response_size);
        }
      while (written < 0 && errno == EINTR);

      if (written != static_cast<ssize_t>(response_size))
        {
          std::fprintf(stderr, "nyampd: transport write failed: %s\n",
                       written < 0 ? std::strerror(errno) : "short write");
          close(fd);
          return 1;
        }
    }
}

} // namespace

int main(int argc, char **argv)
{
  if (argc > 2)
    {
      std::fprintf(stderr, "usage: %s [rpmsg-device]\n", argv[0]);
      return 2;
    }

  return Run(argc == 2 ? argv[1] : nullptr);
}
