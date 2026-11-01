#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Filesystem safety checks for source archive ingestion, without network."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/fetch_llvm.py"
SPEC = importlib.util.spec_from_file_location("fetch_llvm", SCRIPT)
FETCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FETCH)


class SourceArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.version = b"set(LLVM_VERSION_MAJOR 24)\n"

    def tearDown(self):
        self.temporary.cleanup()

    def archive(self, extra=None):
        path = self.root / "source.tar.gz"
        with tarfile.open(path, "w:gz") as tar:
            for name, content in [("source/cmake/Modules/LLVMVersion.cmake", self.version),
                                  ("source/llvm/CMakeLists.txt", b"cmake_minimum_required(VERSION 3.20)\n")]:
                info = tarfile.TarInfo(name)
                info.size = len(content)
                tar.addfile(info, io.BytesIO(content))
            if extra:
                extra(tar)
        return path, {"archive_sha256": FETCH.sha256(path),
                      "version_file_sha256": hashlib.sha256(self.version).hexdigest(),
                      "commit": "test-only-snapshot", "version": "test"}

    def test_valid_archive_records_provenance(self):
        archive, pin = self.archive()
        destination = self.root / "llvm"
        result = FETCH.extract(archive, destination, pin)
        self.assertEqual(result["commit"], pin["commit"])
        self.assertEqual(json.loads((destination / ".llvm-vulkan-source.json").read_text())["archive_sha256"], pin["archive_sha256"])

    def test_bad_checksum_does_not_create_destination(self):
        archive, pin = self.archive()
        pin["archive_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            FETCH.extract(archive, self.root / "llvm", pin)
        self.assertFalse((self.root / "llvm").exists())

    def test_wrong_version_file_does_not_publish_tree(self):
        archive, pin = self.archive()
        pin["version_file_sha256"] = "0" * 64
        with self.assertRaises(ValueError):
            FETCH.extract(archive, self.root / "llvm", pin)
        self.assertFalse((self.root / "llvm").exists())

    def test_existing_tree_is_preserved(self):
        archive, pin = self.archive()
        destination = self.root / "llvm"
        destination.mkdir()
        (destination / "user-work").write_text("preserve")
        with self.assertRaises(FileExistsError):
            FETCH.extract(archive, destination, pin)
        self.assertEqual((destination / "user-work").read_text(), "preserve")

    def test_parent_escape_is_rejected(self):
        def malicious(tar):
            info = tarfile.TarInfo("source/../../outside")
            info.size = 1
            tar.addfile(info, io.BytesIO(b"x"))
        archive, pin = self.archive(malicious)
        with self.assertRaises(ValueError):
            FETCH.extract(archive, self.root / "llvm", pin)
        self.assertFalse((self.root / "outside").exists())

    def test_symlink_escape_is_rejected(self):
        def malicious(tar):
            info = tarfile.TarInfo("source/escape")
            info.type = tarfile.SYMTYPE
            info.linkname = "../../outside"
            tar.addfile(info)
        archive, pin = self.archive(malicious)
        with self.assertRaises(tarfile.TarError):
            FETCH.extract(archive, self.root / "llvm", pin)
        self.assertFalse((self.root / "llvm").exists())


if __name__ == "__main__":
    unittest.main()
