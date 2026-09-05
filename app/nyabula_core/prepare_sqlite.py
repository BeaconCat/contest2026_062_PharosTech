#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare the pinned upstream SQLite amalgamation for Make and CMake."""

import argparse
import hashlib
import io
import os
from pathlib import Path
import tempfile
import urllib.request
import zipfile

URL = "https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip"
ARCHIVE_HASH = "5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4"
FILES = {
    "sqlite3.c": "1a206854aa9fe0ccc1b609f5cfce67eb52ac0a8f5aa2f5e853f7c3ed84c710ab",
    "sqlite3.h": "41e066ccd4f89e938f136ceb48c996c54dc381b59fe566dea48864c3750b779e",
}


def prepare(output: Path) -> None:
    if all((output / name).is_file() and
           hashlib.sha256((output / name).read_bytes()).hexdigest() == digest
           for name, digest in FILES.items()):
        return
    with urllib.request.urlopen(URL, timeout=45) as response:
        archive = response.read(4 * 1024 * 1024)
    if hashlib.sha256(archive).hexdigest() != ARCHIVE_HASH:
        raise ValueError("SQLite archive hash mismatch")
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(archive)) as package:
        for name, digest in FILES.items():
            data = package.read("sqlite-amalgamation-3450100/" + name)
            if hashlib.sha256(data).hexdigest() != digest:
                raise ValueError("SQLite source hash mismatch: " + name)
            descriptor, temporary = tempfile.mkstemp(prefix=name, dir=output)
            try:
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(data)
                os.replace(temporary, output / name)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    prepare(parser.parse_args().output)
