#!/usr/bin/env python3
# ##############################################################################
# tools/k7_abpack/bootctrl.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

"""Create and inspect redundant KICKPI-K7 A/B boot-control records."""

import argparse
import hashlib
import json
import pathlib
import struct
import sys
import zlib


MAGIC = b"K7ABCTRL"
FORMAT_VERSION = 1
RECORD_SIZE = 4096
COPY_COUNT = 2
CRC_OFFSET = RECORD_SIZE - 4
HEADER = struct.Struct("<8sHHQ")
DOMAIN_HEADER = struct.Struct("<BBBB")
SLOT = struct.Struct("<BBBBQQ32s")
DOMAIN_NAMES = ("nuttx", "amp")


def image_metadata(path):
    if path is None:
        return {"size": 0, "version": 0, "sha256": bytes(32)}

    digest = hashlib.sha256()
    size = 0
    with pathlib.Path(path).open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            size += len(chunk)
            digest.update(chunk)

    return {"size": size, "version": 1, "sha256": digest.digest()}


def initial_slot(metadata, enabled):
    return {
        "priority": 15 if enabled else 0,
        "tries_remaining": 0,
        "successful": 1 if enabled else 0,
        **metadata,
    }


def initial_domain(slot_a, slot_b):
    return {
        "active_slot": 0,
        "slots": [initial_slot(slot_a, True), initial_slot(slot_b, False)],
    }


def encode_record(generation, domains):
    record = bytearray(RECORD_SIZE)
    cursor = 0
    HEADER.pack_into(record, cursor, MAGIC, FORMAT_VERSION, HEADER.size,
                     generation)
    cursor += HEADER.size

    for name in DOMAIN_NAMES:
        domain = domains[name]
        DOMAIN_HEADER.pack_into(record, cursor, domain["active_slot"], 0, 0, 0)
        cursor += DOMAIN_HEADER.size
        for slot in domain["slots"]:
            SLOT.pack_into(record, cursor, slot["priority"],
                           slot["tries_remaining"], slot["successful"], 0,
                           slot["size"], slot["version"], slot["sha256"])
            cursor += SLOT.size

    crc = zlib.crc32(record[:CRC_OFFSET]) & 0xFFFFFFFF
    struct.pack_into("<I", record, CRC_OFFSET, crc)
    return bytes(record)


def decode_record(record, copy_index):
    if len(record) != RECORD_SIZE:
        raise ValueError("record has an invalid size")

    stored_crc = struct.unpack_from("<I", record, CRC_OFFSET)[0]
    calculated_crc = zlib.crc32(record[:CRC_OFFSET]) & 0xFFFFFFFF
    if stored_crc != calculated_crc:
        raise ValueError("CRC32 mismatch")

    magic, version, header_size, generation = HEADER.unpack_from(record, 0)
    if magic != MAGIC:
        raise ValueError("magic mismatch")
    if version != FORMAT_VERSION or header_size != HEADER.size:
        raise ValueError("unsupported format")

    cursor = HEADER.size
    domains = {}
    for name in DOMAIN_NAMES:
        active_slot, _, _, _ = DOMAIN_HEADER.unpack_from(record, cursor)
        cursor += DOMAIN_HEADER.size
        if active_slot > 1:
            raise ValueError(f"{name} active slot is invalid")

        slots = []
        for _ in range(2):
            priority, tries, successful, _, size, image_version, digest = \
                SLOT.unpack_from(record, cursor)
            cursor += SLOT.size
            slots.append({
                "priority": priority,
                "tries_remaining": tries,
                "successful": bool(successful),
                "image_size": size,
                "image_version": image_version,
                "sha256": digest.hex(),
            })
        domains[name] = {"active_slot": "ab"[active_slot], "slots": slots}

    return {
        "copy": copy_index,
        "generation": generation,
        "crc32": f"{stored_crc:08x}",
        "domains": domains,
    }


def read_copies(path):
    data = pathlib.Path(path).read_bytes()
    if len(data) < RECORD_SIZE * COPY_COUNT:
        raise ValueError("bootctrl image is shorter than two records")

    copies = []
    for index in range(COPY_COUNT):
        start = index * RECORD_SIZE
        try:
            copies.append(decode_record(data[start:start + RECORD_SIZE], index))
        except ValueError as error:
            copies.append({"copy": index, "error": str(error)})
    return copies


def command_init(args):
    domains = {
        "nuttx": initial_domain(image_metadata(args.nuttx_a),
                                image_metadata(args.nuttx_b)),
        "amp": initial_domain(image_metadata(args.amp_a),
                              image_metadata(args.amp_b)),
    }
    record = encode_record(args.generation, domains)
    pathlib.Path(args.output).write_bytes(record * COPY_COUNT)


def command_inspect(args):
    copies = read_copies(args.image)
    valid = [copy for copy in copies if "error" not in copy]
    selected = max(valid, key=lambda copy: copy["generation"]) if valid else None
    result = {"copies": copies, "selected_copy": None if selected is None
              else selected["copy"]}
    print(json.dumps(result, indent=2))
    return 0 if selected is not None else 1


def build_parser():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    init = subparsers.add_parser("init")
    init.add_argument("--output", required=True)
    init.add_argument("--generation", type=int, default=1)
    init.add_argument("--nuttx-a", required=True)
    init.add_argument("--nuttx-b")
    init.add_argument("--amp-a")
    init.add_argument("--amp-b")
    init.set_defaults(handler=command_init)

    inspect = subparsers.add_parser("inspect")
    inspect.add_argument("image")
    inspect.set_defaults(handler=command_inspect)
    return parser


def main():
    args = build_parser().parse_args()
    try:
        return args.handler(args) or 0
    except (OSError, ValueError) as error:
        print(f"bootctrl: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
