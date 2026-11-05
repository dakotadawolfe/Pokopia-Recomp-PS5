# SPDX-License-Identifier: GPL-3.0-or-later
"""Host checks for private shader selection and verified cache replacement."""
import csv
import hashlib
from pathlib import Path
import tempfile
import unittest

from install_shader_cache import DESTINATION, activate, verify_remote
from prepare_shader_cache import checked_name
from prepare_startup_shader_corpus import prepare


class SelectionTests(unittest.TestCase):
    def test_observed_entry_and_shared_occurrence(self):
        with tempfile.TemporaryDirectory(prefix="pokopia-shader-test-") as directory:
            root = Path(directory)
            corpus = root / "corpus"
            corpus.mkdir()
            hashes = []
            for value in (b"observed", b"related", b"unrelated"):
                key = hashlib.sha1(value).hexdigest()[:16]
                hashes.append(key)
                for folder in ("code", "ctrl", "glslc"):
                    (corpus / folder).mkdir(exist_ok=True)
                    (corpus / folder / (key + ".bin")).write_bytes(value)
            with (corpus / "unique.tsv").open("w", newline="") as stream:
                writer = csv.writer(stream, delimiter="\t")
                writer.writerow(("code_sha1_16", "first_source", "entry_off", "first_ctrl", "first_glslc"))
                for index, key in enumerate(hashes):
                    writer.writerow((key, "material", "0" if index < 2 else "100", key, key))
            with (corpus / "manifest.tsv").open("w", newline="") as stream:
                writer = csv.writer(stream, delimiter="\t")
                writer.writerow(("code_sha1_16", "source", "entry_off", "container"))
                writer.writerows(((hashes[0], "material", "0", "romfs"),
                                 (hashes[1], "material", "0", "romfs"),
                                 (hashes[0], "other", "100", "romfs"),
                                 (hashes[2], "other", "100", "romfs")))
            log = root / "runtime.log"
            log.write_text(f"shader: no usable /cache/{hashes[0]}.spv\n")
            prepare(corpus, root / "selected", log)
            self.assertEqual({p.stem for p in (root / "selected" / "code").iterdir()}, set(hashes[:2]))
            with self.assertRaises(ValueError):
                prepare(corpus, root / "selected", log)
            (corpus / "code" / (hashes[1] + ".bin")).write_bytes(b"corrupt")
            with self.assertRaises(ValueError):
                prepare(corpus, root / "corrupt", log)
            self.assertFalse((root / "corrupt").exists())
            log.write_text("shader: no usable /cache/0000000000000000.spv\n")
            with self.assertRaises(ValueError):
                prepare(corpus, root / "absent", log)


class CacheTests(unittest.TestCase):
    def test_names_cannot_escape_cache(self):
        for name in ("../save", "/save", "a/b", "a\\b", ".", "..", "x\r\nDELE file"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                checked_name(name)

    def test_full_readback_and_unexpected_files(self):
        class FTP:
            data = {"results.tsv": b"bindings"}
            def mlsd(self, _):
                return [(name, {"type": "file"}) for name in self.data]
            def retrbinary(self, command, receive, **_):
                receive(self.data[command.rsplit("/", 1)[1]])
        ftp = FTP()
        files = {"results.tsv": {"bytes": 8, "sha256": hashlib.sha256(b"bindings").hexdigest()}}
        verify_remote(ftp, DESTINATION, files)
        ftp.data["results.tsv"] = b"tampered"
        with self.assertRaises(ValueError):
            verify_remote(ftp, DESTINATION, files)
        ftp.data["extra"] = b""
        with self.assertRaises(ValueError):
            verify_remote(ftp, DESTINATION, files)

    def test_failed_activation_restores_previous_cache(self):
        class FTP:
            calls = []
            def rename(self, source, destination):
                self.calls.append((source, destination))
                if source == "staging":
                    raise OSError("rename failed")
        ftp = FTP()
        with self.assertRaises(OSError):
            activate(ftp, "staging", "backup")
        self.assertEqual(ftp.calls, [(DESTINATION, "backup"), ("staging", DESTINATION),
                                     ("backup", DESTINATION)])


if __name__ == "__main__":
    unittest.main()
