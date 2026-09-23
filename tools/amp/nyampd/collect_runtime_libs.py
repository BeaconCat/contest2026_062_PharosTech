#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Collect the dynamic ELF dependencies needed by the RAM-only Linux image."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys

binary, vendor, output = map(Path, sys.argv[1:4])
compiler = sys.argv[4]
environment = dict(os.environ, LC_ALL="C")


def readelf(path, option):
    return subprocess.check_output(["readelf", option, str(path)], text=True,
                                   env=environment)


output.mkdir(parents=True, exist_ok=True)
queue = [binary.resolve()]
copied = set()
interpreter = re.search(r"Requesting program interpreter: (.*?)\]",
                        readelf(binary, "-l"))
extra = [Path(interpreter.group(1)).name] if interpreter else []
while queue:
    path = queue.pop()
    names = re.findall(r"\(NEEDED\).*?\[(.*?)\]", readelf(path, "-d")) + extra
    extra = []
    for name in names:
        if name in copied:
            continue
        if Path(name).name != name:
            raise SystemExit(f"dependency is not a library name: {name}")
        library = vendor / name
        if not library.is_file():
            library = Path(subprocess.check_output(
                [compiler, f"-print-file-name={name}"], text=True).strip())
        if not library.is_file():
            raise SystemExit(f"missing runtime dependency: {name}")
        if "AArch64" not in readelf(library, "-h"):
            raise SystemExit(f"not an AArch64 library: {library}")
        shutil.copy2(library.resolve(), output / name)
        copied.add(name)
        queue.append(library.resolve())
print("runtime libraries:", ", ".join(sorted(copied)))
