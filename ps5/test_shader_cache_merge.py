# SPDX-License-Identifier: GPL-3.0-or-later
"""Cache validation scheduling and failure handling using synthetic inputs."""
import csv
import json
from pathlib import Path
import subprocess
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from prepare_shader_cache import identity, prepare


class MergeTests(unittest.TestCase):
    def fixture(self, root):
        previous, addition = root / "previous", root / "addition"
        for folder, start in ((previous, 0), (addition, 4)):
            folder.mkdir()
            with (folder / "results.tsv").open("w", newline="") as stream:
                writer = csv.DictWriter(stream, ["hash", "status", "error", "spv_bytes"], delimiter="\t")
                writer.writeheader()
                for number in range(start, start + 4):
                    key = f"{number:016x}"
                    data = bytes([number]) * 32  # Synthetic bytes; subprocess is mocked below.
                    (folder / (key + ".spv")).write_bytes(data)
                    writer.writerow(dict(hash=key, status="ok", error="", spv_bytes=len(data)))
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps({"files": {p.name: identity(p) for p in previous.iterdir()}}))
        selection = root / "selection.json"
        selection.write_text(json.dumps({"scope": "runtime-misses-and-source-entries"}))
        return previous, manifest, addition, selection, root / "output", Path("synthetic-validator")

    def test_parallel_validation_checks_every_input_before_publishing(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            lock = threading.Lock()
            active = peak = 0
            seen = []
            def validate(command, check):
                nonlocal active, peak
                self.assertTrue(check)
                self.assertEqual(command[1:3], ["--target-env", "vulkan1.3"])
                with lock:
                    active += 1
                    peak = max(peak, active)
                    seen.append(Path(command[-1]).name)
                self.assertFalse(args[4].exists())
                time.sleep(.02)
                with lock:
                    active -= 1
            with patch("prepare_shader_cache.subprocess.run", validate):
                prepare(*args, jobs=4)
            self.assertEqual(len(set(seen)), 8)
            self.assertGreater(peak, 1)
            self.assertLessEqual(peak, 4)
            manifest = json.loads((args[4] / "shader-cache-manifest.json").read_text())
            self.assertEqual(manifest["blocks"], 8)

    def test_validator_failure_leaves_no_output_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            def validate(command, check):
                if command[-1].endswith("0000000000000006.spv"):
                    raise subprocess.CalledProcessError(1, command)
            with patch("prepare_shader_cache.subprocess.run", validate):
                with self.assertRaises(subprocess.CalledProcessError):
                    prepare(*args, jobs=4)
            self.assertFalse(args[4].exists())

    def test_tampered_previous_file_fails_before_validation(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            (args[0] / "0000000000000000.spv").write_bytes(b"changed")
            with patch("prepare_shader_cache.subprocess.run") as validator:
                with self.assertRaisesRegex(ValueError, "differs from its manifest"):
                    prepare(*args, jobs=4)
                validator.assert_not_called()
            self.assertFalse(args[4].exists())


if __name__ == "__main__":
    unittest.main()
