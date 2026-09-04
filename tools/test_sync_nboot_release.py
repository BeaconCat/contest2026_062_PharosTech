# SPDX-License-Identifier: Apache-2.0

import hashlib
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("sync_nboot_release.py")


class SyncNBootReleaseTest(unittest.TestCase):
    def test_installs_valid_release_pair(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            assets = root / "assets"
            output = root / "output"
            assets.mkdir()
            binary = bytearray(64)
            binary[8:16] = (0x200000).to_bytes(8, "little")
            binary[56:60] = b"ARMd"
            files = {
                "nboot-kickpi-k7.bin": bytes(binary),
                "nboot-kickpi-k7.dtb": bytes.fromhex("d00dfeed") + bytes(60),
            }
            for name, data in files.items():
                (assets / name).write_bytes(data)
            (assets / "SHA256SUMS").write_text("".join(
                f"{hashlib.sha256(data).hexdigest()}  {name}\n"
                for name, data in files.items()), encoding="utf-8")

            subprocess.run([
                sys.executable, str(SCRIPT), "--assets", str(assets),
                "--output", str(output), "--tag", "v1.2.3",
            ], check=True)

            self.assertEqual((output / "RELEASE").read_text().strip(), "v1.2.3")
            self.assertEqual((output / "nboot-kickpi-k7.bin").read_bytes(),
                             files["nboot-kickpi-k7.bin"])


if __name__ == "__main__":
    unittest.main()
