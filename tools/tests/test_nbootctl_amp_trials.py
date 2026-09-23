#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check AMP trial policy in the actual staging and activation functions."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "app/nbootctl/nbootctl_bootctrl.c").read_text()
types = source[source.index("struct nbootctl_slot_s"):source.index("_Static_assert")]
defines = "\n".join(re.findall(r"^#define NBOOTCTL_.*$", source, re.M))


def function(name):
    match = re.search(r"(?:static )?int " + name + r"\([^;]*?\)\n\{", source)
    assert match
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
''' + defines + "\n" + types + r'''
struct geometry { bool geo_available,geo_writeenabled; unsigned int geo_sectorsize; size_t geo_nsectors; };
struct inode;
struct ops {
 ssize_t (*read)(struct inode *,uint8_t *,size_t,size_t);
 ssize_t (*write)(struct inode *,const uint8_t *,size_t,size_t);
 int (*geometry)(struct inode *,struct geometry *);
};
struct inode { struct {struct ops *i_bops;} u; };
static struct nbootctl_record_s saved;
static struct inode control, payload;
static uint8_t disk[1024];
static ssize_t payload_read(struct inode *n,uint8_t *data,size_t start,size_t count)
{ assert(n==&payload && !start && count==2);memcpy(data,disk,1024);return count; }
static ssize_t payload_write(struct inode *n,const uint8_t *data,size_t start,size_t count)
{ assert(n==&payload && !start && count==2);memcpy(disk,data,1024);return count; }
static int payload_geometry(struct inode *n,struct geometry *g)
{ assert(n==&payload);*g=(struct geometry){true,true,512,2};return 0; }
static struct ops payload_ops={payload_read,payload_write,payload_geometry};
static const char *nbootctl_slot_path(unsigned int medium,int domain,unsigned int slot)
{ assert(medium==1 && domain<=1 && slot==1);return "slot"; }
static int open_blockdriver(const char *path,int flags,struct inode **node)
{ assert(!strcmp(path,"slot") && !flags);*node=&payload;return 0; }
static void close_blockdriver(struct inode *node) {(void)node;}
static int nbootctl_read_records(unsigned int medium,struct inode **node,
                                 struct nbootctl_record_s *records,int *selected)
{ assert(medium==1);*node=&control;*selected=1;records[1]=saved;return 0; }
static int nbootctl_write_records(struct inode *node,struct nbootctl_record_s *records,int selected)
{ assert(node==&control && selected==1);saved=records[1];return 0; }
/* Hashing is outside this policy test; preserve matching write/read digests. */
typedef struct {uint32_t sum;} SHA2_CTX;
static void sha256init(SHA2_CTX *c) {c->sum=0;}
static void sha256update(SHA2_CTX *c,const void *data,size_t size)
{ const uint8_t *p=data;while(size--)c->sum+=*p++; }
static void sha256final(uint8_t *digest,SHA2_CTX *c)
{ memset(digest,0,32);memcpy(digest,&c->sum,sizeof(c->sum)); }
''' + function("nbootctl_domain_index") + "\n" + function("nbootctl_bootctrl_update") + "\n" + function("nbootctl_bootctrl_stage") + r'''
int main(void)
{
 char name[]="/tmp/nbootctl-trial-XXXXXX";
 uint8_t input[513];memset(input,0x5a,sizeof(input));
 int fd=mkstemp(name);assert(fd>=0);
 assert(write(fd,input,sizeof(input))==sizeof(input));close(fd);
 payload.u.i_bops=&payload_ops;
 assert(nbootctl_bootctrl_stage(1,"amp",0,name)==0);
 assert(saved.domains[1].active_slot==1);
 assert(saved.domains[1].slots[1].tries_remaining==1);
 assert(saved.domains[1].slots[1].successful==0);
 saved.domains[1].slots[1].tries_remaining=0;
 assert(nbootctl_bootctrl_update(1,"amp",1,false)==0);
 assert(saved.domains[1].slots[1].tries_remaining==1);
 assert(nbootctl_bootctrl_update(1,"amp",1,true)==0);
 assert(saved.domains[1].slots[1].successful==1);
 assert(saved.domains[1].slots[1].tries_remaining==0);
 assert(nbootctl_bootctrl_update(1,"amp",1,false)==0);
 assert(saved.domains[1].slots[1].tries_remaining==0);
 assert(nbootctl_bootctrl_stage(1,"nuttx",0,name)==0);
 assert(saved.domains[0].slots[1].tries_remaining==0);
 assert(nbootctl_bootctrl_update(1,"nuttx",1,false)==0);
 assert(saved.domains[0].slots[1].tries_remaining==0);
 unlink(name);
 puts("AMP_TRIAL_POLICY_PASS stage activate confirm; NuttX unchanged");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nbootctl-amp-policy-") as directory:
    path = Path(directory) / "test.c"
    binary = Path(directory) / "test"
    path.write_text(program)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
