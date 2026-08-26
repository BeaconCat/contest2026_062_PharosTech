#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

import importlib.util
import pathlib
import tempfile
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name("bootctrl.py")
SPEC = importlib.util.spec_from_file_location("bootctrl", MODULE_PATH)
BOOTCTRL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOOTCTRL)


class BootctrlTest(unittest.TestCase):
    def test_redundant_records_survive_one_corrupt_copy(self):
        metadata = {"size": 3, "version": 1, "sha256": bytes.fromhex("11" * 32)}
        empty = {"size": 0, "version": 0, "sha256": bytes(32)}
        domains = {
            "nuttx": BOOTCTRL.initial_domain(metadata, empty),
            "amp": BOOTCTRL.initial_domain(empty, empty),
        }
        record = BOOTCTRL.encode_record(7, domains)

        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / "bootctrl.bin"
            data = bytearray(record * 2)
            data[32] ^= 0x80
            image.write_bytes(data)
            copies = BOOTCTRL.read_copies(image)

        self.assertIn("error", copies[0])
        self.assertEqual(copies[1]["generation"], 7)
        self.assertEqual(copies[1]["domains"]["nuttx"]["active_slot"], "a")

    def test_newer_valid_generation_wins(self):
        empty = {"size": 0, "version": 0, "sha256": bytes(32)}
        domains = {
            "nuttx": BOOTCTRL.initial_domain(empty, empty),
            "amp": BOOTCTRL.initial_domain(empty, empty),
        }
        with tempfile.TemporaryDirectory() as directory:
            image = pathlib.Path(directory) / "bootctrl.bin"
            image.write_bytes(BOOTCTRL.encode_record(4, domains) +
                              BOOTCTRL.encode_record(5, domains))
            copies = BOOTCTRL.read_copies(image)
        selected = max(copies, key=lambda copy: copy["generation"])
        self.assertEqual(selected["copy"], 1)


if __name__ == "__main__":
    unittest.main()
