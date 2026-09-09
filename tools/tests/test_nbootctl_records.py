# SPDX-License-Identifier: Apache-2.0
"""Exercise the actual record writer with read-back corruption and I/O faults."""
from pathlib import Path
import subprocess
import sys
import re
import tempfile

source_path = (Path(sys.argv[1]) if len(sys.argv) > 1 else
               Path(__file__).resolve().parents[2] / 'app/nbootctl/nbootctl_bootctrl.c')
source = source_path.read_text()
types = source[source.index('struct nbootctl_slot_s'):source.index('_Static_assert')]
match = re.search(r'static int nbootctl_write_records\([^;]*?\)\n\{', source)
assert match
end = match.end()
depth = 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
writer = source[match.start():end]
program = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <zlib.h>
#define NBOOTCTL_RECORD_SIZE 4096
#define NBOOTCTL_SHA256_SIZE 32
#define NBOOTCTL_COPY_COUNT 2
#define NBOOTCTL_RECORD_SECTORS 8
''' + types + r'''
struct inode;
struct ops {
  ssize_t (*write)(struct inode *, const uint8_t *, int, int);
  ssize_t (*read)(struct inode *, uint8_t *, int, int);
};
struct inode { struct { struct ops *i_bops; } u; };
static uint8_t disk[8192];
static int writes, order[2], fault;
static ssize_t wr(struct inode *n, const uint8_t *b, int s, int count)
{
  (void)n; assert(count == 8 && (s == 0 || s == 8));
  order[writes++] = s;
  if (fault == 1) return -EIO;
  memcpy(disk + s * 512, b, 4096);
  return 8;
}
static ssize_t rd(struct inode *n, uint8_t *b, int s, int count)
{
  (void)n; assert(count == 8 && (s == 0 || s == 8));
  memcpy(b, disk + s * 512, 4096);
  if (fault == 2 || (fault == 3 && writes == 2)) b[100] ^= 1;
  return 8;
}
static uint32_t nbootctl_crc32(const void *p, size_t n)
{ return (uint32_t)crc32(0, p, n); }
''' + writer + r'''
int main(void)
{
  struct ops ops = {wr, rd};
  struct inode node = {{&ops}};
  struct nbootctl_record_s *records = calloc(2, sizeof(*records));
  assert(records);
  for (int selected = 0; selected < 2; selected++) {
    for (fault = 0; fault < 4; fault++) {
      memset(records, 0, 8192); memset(disk, 0x5a, 8192); writes = 0;
      records[selected].generation = 7;
      int ret = nbootctl_write_records(&node, records, selected);
      assert(ret == (fault ? -EIO : 0));
      assert(order[0] == (1-selected)*8);
      if (fault == 1 || fault == 2) {
        assert(writes == 1);
        assert(disk[selected*4096] == 0x5a);
      } else {
        assert(writes == 2 && order[1] == selected*8);
      }
      if (!fault) {
        assert(memcmp(disk, disk+4096, 4096) == 0);
        struct nbootctl_record_s *r = (void *)disk;
        assert(r->generation == 8 && r->crc32 == crc32(0,disk,4092));
      }
    }
  }
  free(records);
  return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'test.c'
    exe = Path(tmp) / 'test'
    path.write_text(program)
    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(path), '-lz', '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('RECORD_WRITE_PASS selected=0/1, write-error, both-readback-corruptions')
