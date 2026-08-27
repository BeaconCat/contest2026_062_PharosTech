# Nyabula Core NuttX kernel-build patches

These patches capture public NuttX fixes required by the current
`qemu-armv8a` kernel-build validation. They are review artifacts only: no
script applies them automatically and this repository does not submit them
upstream.

Apply from the NuttX repository root in order:

```sh
git apply /path/to/0001-elf-skip-empty-symbol-names.patch
git apply /path/to/0002-arm64-track-selected-addrenv.patch
```

`0001` prevents an ELF symbol-table entry without a string name from leaving
the loader buffer unset before comparison. `0002` makes ARM64 page growth use
the address environment selected on the current CPU. This matters while the
kernel creates the first user task: `this_task()` still refers to the kernel
IDLE task, while `up_addrenv_select()` has already installed the new user
page tables.

The current openvela export/import scripts also omit generated Make metadata.
Until that public build-system issue is fixed, the verified workaround is:

1. After `mkimport.sh`, expose `import/scripts/Make.defs` as
   `import/Make.defs`.
2. Include `$(TOPDIR)/.config` and `$(TOPDIR)/tools/Config.mk`, then recreate
   the standard C/C++/ASM flags from the exported architecture variables.
3. Set `ARCHCRT0OBJ=$(TOPDIR)/startup/crt0.o`,
   `LDSTARTGROUP=--start-group`, and `LDENDGROUP=--end-group`.
4. Build user applications with `apps/staging` temporarily pointing at
   `import/libs`, then restore the normal staging directory before rebuilding
   the kernel.

Verification evidence is recorded in `实测日志.md`: QEMU stopped at Nyabula
Core `main` with `PSTATE=00000040 ---- EL0t`, and the QuickJS lifecycle ran
from the independent user ELF.
