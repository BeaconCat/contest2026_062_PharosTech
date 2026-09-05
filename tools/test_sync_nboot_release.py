# SPDX-License-Identifier: Apache-2.0

import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).with_name("sync_nboot_release.py")


class SyncNBootReleaseTest(unittest.TestCase):
    def make_release(self, root, contract=None):
        assets = root / "assets"
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
        manifest = {
            "schema": 1,
            "tag": "v1.2.3",
            "repository": "BeaconCat/N-Boot",
            "source_sha": "1" * 40,
            "target": "kickpi-k7-rk3576",
            "contract": contract or {
                "load_address": "0x40200000",
                "text_offset": "0x200000",
                "vendor_fit_size": 4194304,
                "layout_version": 1,
            },
            "artifacts": {
                "proper": {
                    "name": "nboot-kickpi-k7.bin",
                    "size": len(files["nboot-kickpi-k7.bin"]),
                    "sha256": hashlib.sha256(
                        files["nboot-kickpi-k7.bin"]).hexdigest(),
                },
                "dtb": {
                    "name": "nboot-kickpi-k7.dtb",
                    "size": len(files["nboot-kickpi-k7.dtb"]),
                    "sha256": hashlib.sha256(
                        files["nboot-kickpi-k7.dtb"]).hexdigest(),
                },
            },
        }
        manifest_data = (json.dumps(manifest, indent=2) + "\n").encode()
        (assets / "nboot-release.json").write_bytes(manifest_data)
        files["nboot-release.json"] = manifest_data
        (assets / "SHA256SUMS").write_text("".join(
            f"{hashlib.sha256(data).hexdigest()}  {name}\n"
            for name, data in files.items()), encoding="utf-8")
        return assets, files

    def run_sync(self, assets, output, check=True):
        return subprocess.run([
            sys.executable, str(SCRIPT), "--assets", str(assets),
            "--output", str(output), "--tag", "v1.2.3",
        ], check=check, capture_output=not check, text=True)

    def test_installs_valid_release(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            output = root / "output"
            assets, files = self.make_release(root)
            self.run_sync(assets, output)

            self.assertEqual((output / "RELEASE").read_text().strip(), "v1.2.3")
            self.assertEqual((output / "nboot-kickpi-k7.bin").read_bytes(),
                             files["nboot-kickpi-k7.bin"])
            self.assertEqual(json.loads(
                (output / "nboot-release.json").read_text())["source_sha"],
                "1" * 40)

    def test_rejects_unsupported_board_contract(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            contract = {
                "load_address": "0x40800000",
                "text_offset": "0x200000",
                "vendor_fit_size": 4194304,
                "layout_version": 1,
            }
            assets, _ = self.make_release(root, contract=contract)
            result = self.run_sync(assets, root / "output", check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsupported N-Boot board contract", result.stderr)

    def test_rejects_manifest_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            assets, _ = self.make_release(root)
            manifest = json.loads(
                (assets / "nboot-release.json").read_text())
            manifest["artifacts"]["proper"]["sha256"] = "0" * 64
            (assets / "nboot-release.json").write_text(
                json.dumps(manifest) + "\n", encoding="utf-8")
            result = self.run_sync(assets, root / "output", check=False)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("manifest artifact hash mismatch", result.stderr)


if __name__ == "__main__":
    unittest.main()
