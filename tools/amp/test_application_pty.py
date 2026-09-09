#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the real client query and daemon over a raw PTY, not RPMsg HW."""
import os
import pathlib
import pty
import subprocess
import sys
import tempfile
import tty

repo=pathlib.Path(sys.argv[1]).resolve()
daemon=pathlib.Path(sys.argv[2]).resolve()
with tempfile.TemporaryDirectory(prefix="amp-app-pty-") as directory:
    root=pathlib.Path(directory)
    (root/'nuttx/rpmsg').mkdir(parents=True)
    (root/'nuttx/config.h').write_text('')
    (root/'nuttx/rpmsg/rpmsg.h').write_text('''#include <stdint.h>
#define RPMSG_ADDR_ANY 0xffffffffU
#define RPMSG_CREATE_DEV_IOCTL 1
struct rpmsg_endpoint_info {char name[32]; uint32_t src,dst;};
''')
    source=repo/'app/nyampctl/nyampctl_main.c'
    (root/'client.c').write_text(f'''#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include "nyamp_protocol.h"
/* A PTY is a stream; preserve the message boundaries provided by RPMsg. */
static ssize_t nyamp_test_read(int fd,void *buffer,size_t capacity) {{
 uint8_t *wire=buffer;
 size_t used=0, needed=NYAMP_WIRE_HEADER_SIZE;
 while(used<needed) {{
  ssize_t got=read(fd,wire+used,needed-used);
  if(got<=0)return got;
  used+=(size_t)got;
  if(used==NYAMP_WIRE_HEADER_SIZE) {{
   needed+=wire[36]|((uint32_t)wire[37]<<8)|((uint32_t)wire[38]<<16)|((uint32_t)wire[39]<<24);
   if(needed>capacity){{errno=EMSGSIZE;return -1;}}
  }}
 }}
 return (ssize_t)used;
}}
#define read nyamp_test_read
#define main unused_nyampctl_main
#include "{source}"
#undef main
int main(int argc,char **argv) {{
 if(argc!=3)return 2;
 return nyampctl_query(atoi(argv[1]),atoi(argv[2]))<0?1:0;
}}
''')
    protocol=repo/'tools/amp/protocol'
    subprocess.run(['cc','-std=gnu11','-D_GNU_SOURCE','-Wall','-Werror',
                    '-I',str(root),'-I',str(protocol),str(root/'client.c'),
                    str(protocol/'nyamp_protocol.c'),'-o',str(root/'client')],check=True)
    master,slave=pty.openpty()
    tty.setraw(slave)
    process=subprocess.Popen([str(daemon),os.ttyname(slave)],stderr=subprocess.PIPE,text=True)
    try:
        for opcode in (1,2):
            result=subprocess.run([str(root/'client'),str(master),str(opcode)],
                                  pass_fds=(master,),capture_output=True,text=True,timeout=10)
            assert result.returncode==0, result
            assert ('nyamp health ok:' if opcode==1 else 'online=') in result.stdout
            print(result.stdout,flush=True)
        print('NYAMP_APPLICATION_PTY_PASS')
    finally:
        process.terminate()
        process.communicate(timeout=5)
        os.close(master)
        os.close(slave)
