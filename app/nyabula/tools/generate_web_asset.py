#!/usr/bin/env python3
"""Embed the Nyabula debug console in a C translation unit."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "web" / "console.html"
OUTPUT = ROOT / "src" / "generated" / "nyabula_console_html.c"

BANNER = """/****************************************************************************
 * app/nyabula/src/generated/nyabula_console_html.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * \"License\"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an \"AS IS\" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#include \"nyabula_console_html.h\"

"""


def main() -> None:
    # Embed a stable payload even when Git checks the HTML out with CRLF.

    data = SOURCE.read_bytes().replace(b"\r\n", b"\n")
    lines = []
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append("  " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")

    output = BANNER
    output += "const uint8_t g_nyabula_console_html[] =\n{\n"
    output += "\n".join(lines)
    output += "\n};\n\n"
    output += f"const size_t g_nyabula_console_html_size = {len(data)};\n"
    OUTPUT.write_text(output, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
