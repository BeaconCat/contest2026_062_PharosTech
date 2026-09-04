#!/usr/bin/env python3
"""Validate the fixed KICKPI-K7 AMP memory and transport contract."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


EXPECTED = {
    "vring": (0x47800000, 0x200000),
    "dma": (0x47A00000, 0x200000),
    "shmem": (0x47C00000, 0x400000),
    "openvela": (0x4A400000, 0x1000000),
}

FORBIDDEN = {
    "bl31": (0x40000000, 0x200000),
    "optee": (0x48400000, 0x1000000),
    "standalone_dma_heap": (0x49400000, 0x1000000),
}


def require(pattern: str, text: str, source: Path) -> re.Match[str]:
    match = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    if match is None:
        raise ValueError(f"{source}: missing pattern: {pattern}")
    return match


def parse_dtsi(path: Path) -> dict[str, tuple[int, int]]:
    text = path.read_text(encoding="utf-8")
    labels = {
        "vring": "rpmsg_reserved",
        "dma": "rpmsg_dma_reserved",
        "shmem": "amp_shmem_reserved",
        "openvela": "openvela_reserved",
    }
    result: dict[str, tuple[int, int]] = {}
    for name, label in labels.items():
        match = require(
            rf"{label}:.*?\{{.*?reg\s*=\s*<0x0\s+(0x[0-9a-fA-F]+)\s+"
            rf"0x0\s+(0x[0-9a-fA-F]+)>;",
            text,
            path,
        )
        result[name] = (int(match.group(1), 16), int(match.group(2), 16))

    require(r"mboxes\s*=\s*<&mailbox0\s+0\s+&mailbox3\s+0>;", text, path)
    require(r"rockchip,link-id\s*=\s*<0x03>;", text, path)
    require(r"GIC_AMP_IRQ_CFG_ROUTE\(174,", text, path)
    require(r"&cpu_l3\s*\{\s*status\s*=\s*\"disabled\";", text, path)
    return result


def parse_defconfig(path: Path) -> tuple[int, int]:
    text = path.read_text(encoding="utf-8")
    start = int(require(r"^CONFIG_RAM_START=(0x[0-9a-fA-F]+)$", text, path).group(1), 16)
    size = int(require(r"^CONFIG_RAM_SIZE=([0-9]+)$", text, path).group(1), 10)
    require(r"^CONFIG_OPENAMP_CACHE=y$", text, path)
    return start, size


def parse_rptun(path: Path) -> tuple[int, int, int, int]:
    text = path.read_text(encoding="utf-8")
    values = []
    for macro in (
        "RK3576_RPMSG_VRING0_DA",
        "RK3576_RPMSG_VRING_SIZE",
        "RK3576_RPMSG_BUFFER_DA",
        "RK3576_RPMSG_BUFFER_LEN",
    ):
        match = require(rf"^#define\s+{macro}\s+(0x[0-9a-fA-F]+)", text, path)
        values.append(int(match.group(1), 16))

    require(
        r"#define\s+RK3576_RPMSG_TX_MBOX\s+0.*?"
        r"#define\s+RK3576_RPMSG_RX_MBOX\s+3.*?"
        r"notifyid\s*==\s*0.*?RK3576_RPMSG_TX_MBOX.*?"
        r"notifyid\s*==\s*1.*?RK3576_RPMSG_RX_MBOX",
        text,
        path,
    )
    return tuple(values)  # type: ignore[return-value]


def overlaps(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] < right[0] + right[1] and right[0] < left[0] + left[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="team repository root",
    )
    args = parser.parse_args()
    repo = args.repo.resolve()

    dtsi_path = repo / "tools/amp/linux/rk3576-kickpi-k7-amp.dtsi"
    defconfig_path = repo / "boards/rk3576/kickpi-k7/configs/amp/defconfig"
    rptun_path = repo / "chips/rk3576/rk3576_rptun.c"

    layout = parse_dtsi(dtsi_path)
    if layout != EXPECTED:
        raise ValueError(f"DTS layout mismatch: {layout!r}")

    if parse_defconfig(defconfig_path) != EXPECTED["openvela"]:
        raise ValueError("openvela defconfig does not match its DTS carveout")

    vring, vring_size, dma, dma_size = parse_rptun(rptun_path)
    if vring != EXPECTED["vring"][0] or vring_size != 0x8000:
        raise ValueError("rptun vring geometry does not match the DTS contract")
    if (dma, dma_size) != EXPECTED["dma"]:
        raise ValueError("rptun buffer pool does not match the DTS contract")

    names = list(EXPECTED)
    for index, name in enumerate(names):
        for other in names[index + 1 :]:
            if overlaps(EXPECTED[name], EXPECTED[other]):
                raise ValueError(f"overlap: {name} and {other}")

    for name, region in EXPECTED.items():
        for reserved_name, reserved in FORBIDDEN.items():
            if overlaps(region, reserved):
                raise ValueError(f"overlap: {name} and {reserved_name}")

    print("AMP layout OK")
    for name, (start, size) in EXPECTED.items():
        print(f"  {name:9s} 0x{start:08x}-0x{start + size:08x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
