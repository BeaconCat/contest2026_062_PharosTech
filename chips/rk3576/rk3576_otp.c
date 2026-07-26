/****************************************************************************
 * chips/rk3576/rk3576_otp.c
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
 * RK3576 OTP (eFuse) read-only driver.
 *
 * The controller is a Rockchip OTP v2 block.  Fuses are read one 16-bit
 * word at a time through the "user mode" path:
 *
 *   1. put the array into user mode        (USER_CTRL)
 *   2. write the word address              (USER_ADDR)
 *   3. kick the read state machine         (USER_ENABLE)
 *   4. poll for the done bit               (INT_STATUS)
 *   5. check the ECC status and take data  (USER_QP / USER_Q)
 *   6. acknowledge the interrupt and leave user mode
 *
 * This driver is deliberately read-only: blowing a fuse is irreversible and
 * a mistake would permanently damage the board, so no programming path is
 * implemented and none of the programming registers are even defined.
 *
 * The exported surface is a /dev/otp character device supporting read(),
 * pread() and lseek(), plus in-kernel helpers - most importantly
 * rk3576_otp_get_mac(), which turns the factory chip ID into a stable
 * locally administered station address so that boards no longer need a
 * hard-coded (and therefore colliding) WLAN MAC.
 *
 * Clocking: the OTP controller runs off "otpc" and "apb" gates which the
 * boot loader already opens (the BootROM and the MiniLoader both read
 * fuses before handing over).  No CRU call is made here because the CRU
 * driver currently exposes only per-module I2C/PWM helpers.
 * TODO: gate the clocks explicitly once rk3576_cru.h grows a generic
 * gate API, so that the driver no longer depends on loader state.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>

#include "arm64_internal.h"
#include "hardware/rk3576_otp.h"
#include "rk3576_otp.h"

#ifdef CONFIG_RK3576_OTP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Device node created by rk3576_otp_initialize(). */

#define RK3576_OTP_DEVPATH "/dev/otp"
#define RK3576_OTP_DEVMODE 0444 /* World readable, never writable      */

/* The read state machine needs a short settling delay after the user mode
 * bit is toggled, and completes a word in well under a microsecond.  Poll
 * in 1us steps with a generous ceiling.
 */

#define RK3576_OTP_SETTLE_US  5
#define RK3576_OTP_POLL_US    1
#define RK3576_OTP_POLL_LIMIT 1000

/* FNV-1a parameters, used to fold the 16-byte chip ID into the 32 host
 * specific bits of a derived MAC address.
 */

#define RK3576_OTP_FNV_OFFSET 0x811c9dc5
#define RK3576_OTP_FNV_PRIME  0x01000193

/* Locally administered, unicast OUI byte (bit 1 set, bit 0 clear). */

#define RK3576_OTP_MAC_LOCAL_OUI 0x02

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct rk3576_otp_dev_s
{
  mutex_t lock;     /* Serialises access to the single controller      */
  bool initialized; /* True once /dev/otp has been registered          */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static uint32_t rk3576_otp_getreg(unsigned int offset);
static void rk3576_otp_putreg(unsigned int offset, uint32_t value);
static int rk3576_otp_wait_done(uint32_t flag);
static int rk3576_otp_read_word(uint32_t wordaddr, uint16_t *value);

static ssize_t rk3576_otp_file_read(struct file *filep, char *buffer,
                                    size_t buflen);
static off_t rk3576_otp_file_seek(struct file *filep, off_t offset,
                                  int whence);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct rk3576_otp_dev_s g_rk3576_otp =
{
  .lock        = NXMUTEX_INITIALIZER,
  .initialized = false,
};

static const struct file_operations g_rk3576_otp_fops =
{
  .read = rk3576_otp_file_read,
  .seek = rk3576_otp_file_seek,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_otp_getreg
 *
 * Description:
 *   Read one controller register.
 *
 ****************************************************************************/

static uint32_t rk3576_otp_getreg(unsigned int offset)
{
  return getreg32(RK3576_OTP_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_otp_putreg
 *
 * Description:
 *   Write one controller register.
 *
 ****************************************************************************/

static void rk3576_otp_putreg(unsigned int offset, uint32_t value)
{
  putreg32(value, RK3576_OTP_ADDR + offset);
}

/****************************************************************************
 * Name: rk3576_otp_wait_done
 *
 * Description:
 *   Poll INT_STATUS until the requested flag is raised, then acknowledge
 *   it (the register is write-one-to-clear).
 *
 * Input Parameters:
 *   flag - Status bit to wait for.
 *
 * Returned Value:
 *   OK (0) when the flag was seen; -ETIMEDOUT otherwise.
 *
 ****************************************************************************/

static int rk3576_otp_wait_done(uint32_t flag)
{
  unsigned int retries;

  for (retries = 0; retries < RK3576_OTP_POLL_LIMIT; retries++)
    {
      if ((rk3576_otp_getreg(RK3576_OTP_INT_STATUS) & flag) != 0)
        {
          /* Acknowledge just this source. */

          rk3576_otp_putreg(RK3576_OTP_INT_STATUS, flag);
          return OK;
        }

      up_udelay(RK3576_OTP_POLL_US);
    }

  return -ETIMEDOUT;
}

/****************************************************************************
 * Name: rk3576_otp_read_word
 *
 * Description:
 *   Read a single 16-bit OTP word through the user mode path.  The caller
 *   must hold the device lock.
 *
 * Input Parameters:
 *   wordaddr - Address of the word inside the fuse array.
 *   value    - Receives the fuse contents.
 *
 * Returned Value:
 *   OK (0) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int rk3576_otp_read_word(uint32_t wordaddr, uint16_t *value)
{
  uint32_t status;
  int ret;

  /* Enter user mode.  Both value and write-enable mask are required. */

  rk3576_otp_putreg(RK3576_OTP_USER_CTRL,
                    OTP_USER_CTRL_USE_USER | OTP_USER_CTRL_USE_USER_MASK);
  up_udelay(RK3576_OTP_SETTLE_US);

  /* Select the word and start the read state machine. */

  rk3576_otp_putreg(RK3576_OTP_USER_ADDR,
                    (wordaddr & OTP_USER_ADDR_VAL_MASK) |
                    OTP_USER_ADDR_MASK);
  rk3576_otp_putreg(RK3576_OTP_USER_ENABLE,
                    OTP_USER_ENABLE_FSM | OTP_USER_ENABLE_FSM_MASK);

  ret = rk3576_otp_wait_done(OTP_INT_STATUS_USER_DONE);
  if (ret == OK)
    {
      /* The two ECC status bits must agree: 0b00 means the word carries no
       * ECC, 0b11 means it was checked and is correct.  Anything else is an
       * uncorrectable read.
       */

      status = rk3576_otp_getreg(RK3576_OTP_USER_QP) & OTP_USER_QP_ECC_MASK;
      if (status != 0 && status != OTP_USER_QP_ECC_MASK)
        {
          _err("ERROR: OTP: word %" PRIu32 " ECC failure, QP=0x%08" PRIx32
               "\n", wordaddr, status);
          ret = -EIO;
        }
      else
        {
          *value = (uint16_t)rk3576_otp_getreg(RK3576_OTP_USER_Q);
        }
    }
  else
    {
      _err("ERROR: OTP: word %" PRIu32 " read timed out\n", wordaddr);
    }

  /* Always leave user mode again, even on error. */

  rk3576_otp_putreg(RK3576_OTP_USER_CTRL, OTP_USER_CTRL_USE_USER_MASK);
  up_udelay(RK3576_OTP_SETTLE_US);

  return ret;
}

/****************************************************************************
 * Name: rk3576_otp_file_read
 *
 * Description:
 *   Character device read handler.  Serves both read() and pread(): the
 *   VFS pread() implementation adjusts filep->f_pos around this call.
 *
 ****************************************************************************/

static ssize_t rk3576_otp_file_read(struct file *filep, char *buffer,
                                    size_t buflen)
{
  off_t pos = filep->f_pos;
  ssize_t nread;

  if (pos < 0 || pos >= RK3576_OTP_SIZE)
    {
      return 0; /* End of the fuse array */
    }

  /* Clamp the request to the end of the array. */

  if (buflen > (size_t)(RK3576_OTP_SIZE - pos))
    {
      buflen = (size_t)(RK3576_OTP_SIZE - pos);
    }

  nread = rk3576_otp_read((uint32_t)pos, buffer, buflen);
  if (nread > 0)
    {
      filep->f_pos = pos + nread;
    }

  return nread;
}

/****************************************************************************
 * Name: rk3576_otp_file_seek
 *
 * Description:
 *   Character device seek handler over the fixed-size fuse array.
 *
 ****************************************************************************/

static off_t rk3576_otp_file_seek(struct file *filep, off_t offset,
                                  int whence)
{
  off_t pos;

  switch (whence)
    {
      case SEEK_SET:
        pos = offset;
        break;

      case SEEK_CUR:
        pos = filep->f_pos + offset;
        break;

      case SEEK_END:
        pos = RK3576_OTP_SIZE + offset;
        break;

      default:
        return -EINVAL;
    }

  if (pos < 0)
    {
      return -EINVAL;
    }

  filep->f_pos = pos;
  return pos;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rk3576_otp_read
 *
 * Description:
 *   Read a range of raw bytes out of the fuse array.  See rk3576_otp.h.
 *
 ****************************************************************************/

ssize_t rk3576_otp_read(uint32_t offset, void *buf, size_t len)
{
  uint8_t *dest = (uint8_t *)buf;
  uint32_t wordaddr;
  unsigned int skip;
  size_t done = 0;
  uint16_t word;
  int ret;

  if (buf == NULL)
    {
      return -EINVAL;
    }

  if (len == 0)
    {
      return 0;
    }

  if (offset >= RK3576_OTP_SIZE || len > RK3576_OTP_SIZE - offset)
    {
      return -EINVAL;
    }

  /* Reads are word granular in hardware; remember how many bytes of the
   * first word have to be discarded.
   */

  wordaddr = offset / RK3576_OTP_NBYTES;
  skip     = offset % RK3576_OTP_NBYTES;

  ret = nxmutex_lock(&g_rk3576_otp.lock);
  if (ret < 0)
    {
      return ret;
    }

  while (done < len)
    {
      unsigned int chunk;
      uint8_t bytes[RK3576_OTP_NBYTES];

      ret = rk3576_otp_read_word(wordaddr, &word);
      if (ret < 0)
        {
          break;
        }

      /* The fuse array is little endian: the low byte of a word carries the
       * lower byte offset.
       */

      bytes[0] = (uint8_t)(word & 0xff);
      bytes[1] = (uint8_t)((word >> 8) & 0xff);

      chunk = RK3576_OTP_NBYTES - skip;
      if (chunk > len - done)
        {
          chunk = (unsigned int)(len - done);
        }

      memcpy(&dest[done], &bytes[skip], chunk);

      done += chunk;
      skip  = 0;
      wordaddr++;
    }

  nxmutex_unlock(&g_rk3576_otp.lock);

  return ret < 0 ? (ssize_t)ret : (ssize_t)done;
}

/****************************************************************************
 * Name: rk3576_otp_read_cpu_code
 *
 * Description:
 *   Read the 16-bit CPU code.  See rk3576_otp.h.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_code(uint16_t *code)
{
  uint8_t raw[RK3576_OTP_CELL_CPU_CODE_SIZE];
  ssize_t nread;

  if (code == NULL)
    {
      return -EINVAL;
    }

  nread = rk3576_otp_read(RK3576_OTP_CELL_CPU_CODE_OFFSET, raw, sizeof(raw));
  if (nread < 0)
    {
      return (int)nread;
    }

  if (nread != (ssize_t)sizeof(raw))
    {
      return -EIO;
    }

  *code = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
  return OK;
}

/****************************************************************************
 * Name: rk3576_otp_read_cpu_version
 *
 * Description:
 *   Read the 3-bit silicon revision.  See rk3576_otp.h.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_version(uint8_t *version)
{
  uint8_t raw;
  ssize_t nread;

  if (version == NULL)
    {
      return -EINVAL;
    }

  nread = rk3576_otp_read(RK3576_OTP_CELL_CPU_VERSION_OFFSET, &raw,
                          RK3576_OTP_CELL_CPU_VERSION_SIZE);
  if (nread < 0)
    {
      return (int)nread;
    }

  if (nread != RK3576_OTP_CELL_CPU_VERSION_SIZE)
    {
      return -EIO;
    }

  *version = (raw >> RK3576_OTP_CPU_VERSION_SHIFT) &
             RK3576_OTP_CPU_VERSION_MASK;
  return OK;
}

/****************************************************************************
 * Name: rk3576_otp_read_cpu_id
 *
 * Description:
 *   Read the 16-byte factory chip ID.  See rk3576_otp.h.
 *
 ****************************************************************************/

int rk3576_otp_read_cpu_id(uint8_t id[RK3576_OTP_CELL_CPU_ID_SIZE])
{
  ssize_t nread;

  if (id == NULL)
    {
      return -EINVAL;
    }

  nread = rk3576_otp_read(RK3576_OTP_CELL_CPU_ID_OFFSET, id,
                          RK3576_OTP_CELL_CPU_ID_SIZE);
  if (nread < 0)
    {
      return (int)nread;
    }

  if (nread != RK3576_OTP_CELL_CPU_ID_SIZE)
    {
      return -EIO;
    }

  return OK;
}

/****************************************************************************
 * Name: rk3576_otp_get_mac
 *
 * Description:
 *   Derive a stable station address from the factory chip ID.  See
 *   rk3576_otp.h.
 *
 ****************************************************************************/

int rk3576_otp_get_mac(uint8_t index, uint8_t mac[RK3576_OTP_MAC_SIZE])
{
  uint8_t id[RK3576_OTP_CELL_CPU_ID_SIZE];
  uint32_t hash = RK3576_OTP_FNV_OFFSET;
  unsigned int i;
  int ret;

  if (mac == NULL)
    {
      return -EINVAL;
    }

  ret = rk3576_otp_read_cpu_id(id);
  if (ret < 0)
    {
      return ret;
    }

  /* Fold the 128-bit die ID down to 32 bits (FNV-1a). */

  for (i = 0; i < RK3576_OTP_CELL_CPU_ID_SIZE; i++)
    {
      hash = (hash ^ id[i]) * RK3576_OTP_FNV_PRIME;
    }

  /* A blank (never fused) chip ID would hash to a constant and defeat the
   * whole point, so refuse it rather than silently handing out a MAC every
   * board would share.
   */

  for (i = 0; i < RK3576_OTP_CELL_CPU_ID_SIZE; i++)
    {
      if (id[i] != 0x00 && id[i] != 0xff)
        {
          break;
        }
    }

  if (i == RK3576_OTP_CELL_CPU_ID_SIZE)
    {
      _err("ERROR: OTP: chip ID is blank, cannot derive a MAC address\n");
      return -ENODATA;
    }

  mac[0] = RK3576_OTP_MAC_LOCAL_OUI;
  mac[1] = (uint8_t)(hash >> 24);
  mac[2] = (uint8_t)(hash >> 16);
  mac[3] = (uint8_t)(hash >> 8);
  mac[4] = (uint8_t)hash;
  mac[5] = index;

  return OK;
}

/****************************************************************************
 * Name: rk3576_otp_initialize
 *
 * Description:
 *   Register the read-only /dev/otp character device.  See rk3576_otp.h.
 *
 ****************************************************************************/

int rk3576_otp_initialize(void)
{
  uint8_t version = 0;
  uint16_t code = 0;
  int ret;

  if (g_rk3576_otp.initialized)
    {
      return OK;
    }

  ret = register_driver(RK3576_OTP_DEVPATH, &g_rk3576_otp_fops,
                        RK3576_OTP_DEVMODE, NULL);
  if (ret < 0)
    {
      _err("ERROR: OTP: Failed to register %s: %d\n", RK3576_OTP_DEVPATH, ret);
      return ret;
    }

  g_rk3576_otp.initialized = true;

  /* Reading the CPU code back is a cheap end-to-end self test of the user
   * mode path and gives a useful boot log line.
   */

  if (rk3576_otp_read_cpu_code(&code) == OK &&
      rk3576_otp_read_cpu_version(&version) == OK)
    {
      _info("OTP ready: cpu code 0x%04x, version %u\n", code, version);
    }

  return OK;
}

#endif /* CONFIG_RK3576_OTP */
