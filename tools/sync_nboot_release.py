#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Validate and install a published N-Boot proper pair."""

import argparse
import hashlib
import json
import pathlib
import re
import struct


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_sums(path):
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        checksum, name = line.split(maxsplit=1)
        result[name.lstrip("* ")] = checksum.lower()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--assets", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--tag", required=True)
    args = parser.parse_args()

    binary = args.assets / "nboot-kickpi-k7.bin"
    dtb = args.assets / "nboot-kickpi-k7.dtb"
    manifest_path = args.assets / "nboot-release.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        raise SystemExit("unsupported N-Boot release manifest schema")
    if manifest.get("tag") != args.tag:
        raise SystemExit("N-Boot release tag does not match the manifest")
    if manifest.get("repository") != "BeaconCat/N-Boot":
        raise SystemExit("unexpected N-Boot source repository")
    if manifest.get("target") != "kickpi-k7-rk3576":
        raise SystemExit("unexpected N-Boot target")
    if not re.fullmatch(r"[0-9a-f]{40}", manifest.get("source_sha", "")):
        raise SystemExit("invalid N-Boot source commit")
    expected_contract = {
        "load_address": "0x40200000",
        "text_offset": "0x200000",
        "vendor_fit_size": 4194304,
        "layout_version": 1,
    }
    if manifest.get("contract") != expected_contract:
        raise SystemExit("unsupported N-Boot board contract")

    if binary.stat().st_size > 2 * 1024 * 1024:
        raise SystemExit("N-Boot proper exceeds the 2 MiB candidate budget")
    if dtb.stat().st_size > 512 * 1024:
        raise SystemExit("N-Boot device tree exceeds the 512 KiB budget")
    sums = load_sums(args.assets / "SHA256SUMS")
    artifacts = manifest.get("artifacts", {})
    expected_artifacts = {
        "proper": binary,
        "dtb": dtb,
    }
    for key, path in expected_artifacts.items():
        record = artifacts.get(key, {})
        if record.get("name") != path.name:
            raise SystemExit(f"manifest artifact name mismatch: {key}")
        if record.get("size") != path.stat().st_size:
            raise SystemExit(f"manifest artifact size mismatch: {path.name}")
        if record.get("sha256") != digest(path):
            raise SystemExit(f"manifest artifact hash mismatch: {path.name}")
        if sums.get(path.name) != digest(path):
            raise SystemExit(f"SHA-256 mismatch: {path.name}")
    if sums.get(manifest_path.name) != digest(manifest_path):
        raise SystemExit("SHA-256 mismatch: nboot-release.json")

    header = binary.read_bytes()[:64]
    if len(header) != 64 or header[56:60] != b"ARMd":
        raise SystemExit("N-Boot ARM64 Image header is invalid")
    if struct.unpack_from("<Q", header, 8)[0] != 0x200000:
        raise SystemExit("N-Boot text offset is not 0x200000")
    if dtb.read_bytes()[:4] != bytes.fromhex("d00dfeed"):
        raise SystemExit("N-Boot device tree header is invalid")

    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / binary.name).write_bytes(binary.read_bytes())
    (args.output / dtb.name).write_bytes(dtb.read_bytes())
    (args.output / manifest_path.name).write_bytes(manifest_path.read_bytes())
    (args.output / "SHA256SUMS").write_text(
        f"{sums[binary.name]}  {binary.name}\n"
        f"{sums[dtb.name]}  {dtb.name}\n"
        f"{sums[manifest_path.name]}  {manifest_path.name}\n",
        encoding="utf-8")
    (args.output / "RELEASE").write_text(args.tag + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
