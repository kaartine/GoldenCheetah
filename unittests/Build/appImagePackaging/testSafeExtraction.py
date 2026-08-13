#!/usr/bin/env python3

import io
import importlib.util
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import unicodedata
import zipfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
EXTRACTOR = REPOSITORY_ROOT / "appveyor" / "safe-extract.py"


def load_extractor():
    spec = importlib.util.spec_from_file_location("safe_extract", EXTRACTOR)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load safe archive extractor")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class SafeExtractionTests(unittest.TestCase):
    def run_extract(self, archive, destination, archive_format, *extra):
        return subprocess.run(
            [
                sys.executable,
                str(EXTRACTOR),
                "--format",
                archive_format,
                "--archive",
                str(archive),
                "--destination",
                str(destination),
                *extra,
            ],
            capture_output=True,
            text=True,
        )

    def test_tar_rejects_links_special_files_and_traversal_without_publication(self):
        cases = (("../escape", tarfile.REGTYPE), ("link", tarfile.SYMTYPE),
                 ("hard", tarfile.LNKTYPE), ("fifo", tarfile.FIFOTYPE))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, (name, kind) in enumerate(cases):
                archive = root / f"hostile-{index}.tar"
                with tarfile.open(archive, "w") as stream:
                    member = tarfile.TarInfo(name)
                    member.type = kind
                    member.linkname = "target"
                    member.size = 0
                    stream.addfile(member, io.BytesIO())
                destination = root / f"output-{index}"
                result = self.run_extract(archive, destination, "tar")
                self.assertNotEqual(result.returncode, 0, name)
                self.assertFalse(destination.exists(), name)
            self.assertFalse((root / "escape").exists())

    def test_tar_can_skip_one_named_safe_symlink_without_publishing_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "vendor.tar"
            with tarfile.open(archive, "w") as stream:
                payload = b"library"
                library = tarfile.TarInfo("vendor/lib.so")
                library.size = len(payload)
                stream.addfile(library, io.BytesIO(payload))
                link = tarfile.TarInfo("vendor/lib.so.1")
                link.type = tarfile.SYMTYPE
                link.linkname = "lib.so"
                stream.addfile(link)

            destination = root / "output"
            result = self.run_extract(
                archive, destination, "tar",
                "--strip-components", "1",
                "--skip-link", "vendor/lib.so.1",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((destination / "lib.so").read_bytes(), payload)
            self.assertFalse((destination / "lib.so.1").exists())

            missing = self.run_extract(
                archive, root / "missing", "tar",
                "--skip-link", "vendor/not-present",
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertFalse((root / "missing").exists())

            unsafe_archive = root / "unsafe-vendor.tar"
            with tarfile.open(unsafe_archive, "w") as stream:
                link = tarfile.TarInfo("vendor/lib.so.1")
                link.type = tarfile.SYMTYPE
                link.linkname = "../outside"
                stream.addfile(link)
            unsafe = self.run_extract(
                unsafe_archive, root / "unsafe", "tar",
                "--skip-link", "vendor/lib.so.1",
            )
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertFalse((root / "unsafe").exists())

    def test_zip_rejects_symlinks_and_case_collisions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            symlink_archive = root / "symlink.zip"
            with zipfile.ZipFile(symlink_archive, "w") as stream:
                member = zipfile.ZipInfo("link")
                member.create_system = 3
                member.external_attr = (stat.S_IFLNK | 0o777) << 16
                stream.writestr(member, "target")
            result = self.run_extract(symlink_archive, root / "symlink-out", "zip")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "symlink-out").exists())

            collision_archive = root / "collision.zip"
            with zipfile.ZipFile(collision_archive, "w") as stream:
                stream.writestr("Bin/Tool.exe", b"one")
                stream.writestr("bin/tool.exe", b"two")
            result = self.run_extract(
                collision_archive, root / "collision-out", "zip"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "collision-out").exists())

            unicode_archive = root / "unicode.zip"
            decomposed = unicodedata.normalize("NFD", "caf\N{LATIN SMALL LETTER E WITH ACUTE}.txt")
            with zipfile.ZipFile(unicode_archive, "w") as stream:
                stream.writestr(decomposed, b"ambiguous")
            result = self.run_extract(
                unicode_archive, root / "unicode-out", "zip"
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((root / "unicode-out").exists())

    def test_valid_archive_is_stripped_and_published_as_one_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "valid.tar"
            with tarfile.open(archive, "w") as stream:
                payload = b"reviewed"
                member = tarfile.TarInfo("source/include/header.h")
                member.size = len(payload)
                member.mode = 0o644
                stream.addfile(member, io.BytesIO(payload))
            destination = root / "output"
            result = self.run_extract(
                archive, destination, "tar", "--strip-components", "1"
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(
                (destination / "include/header.h").read_bytes(), b"reviewed"
            )

    def test_absolute_dot_segment_and_windows_names_are_rejected(self):
        names = (
            "/absolute",
            "safe/./hidden",
            "CON",
            "safe/name:stream",
            "safe/trailing.",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, name in enumerate(names):
                archive = root / f"unsafe-name-{index}.zip"
                with zipfile.ZipFile(archive, "w") as stream:
                    stream.writestr(name, b"unsafe")
                destination = root / f"unsafe-name-{index}"
                result = self.run_extract(archive, destination, "zip")
                self.assertNotEqual(result.returncode, 0, name)
                self.assertFalse(destination.exists(), name)

    def test_file_directory_collisions_are_rejected_in_both_orders(self):
        cases = (
            (("node", b"file"), ("node/child", b"child")),
            (("node/child", b"child"), ("node", b"file")),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for index, members in enumerate(cases):
                archive = root / f"collision-{index}.zip"
                with zipfile.ZipFile(archive, "w") as stream:
                    for name, payload in members:
                        stream.writestr(name, payload)
                destination = root / f"collision-{index}"
                result = self.run_extract(archive, destination, "zip")
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(destination.exists())

    def test_layout_registration_does_not_scan_unrelated_members(self):
        extractor = load_extractor()

        class NoIterationDict(dict):
            def __iter__(self):
                raise AssertionError("layout validation scanned every member")

        seen = NoIterationDict()
        extractor.register_member(seen, PurePosixPath("unrelated"), False)
        extractor.register_member(seen, PurePosixPath("target"), False)

        descendants = {}
        extractor.register_member(
            descendants, PurePosixPath("node/child"), False
        )
        extractor.register_member(
            descendants, PurePosixPath("node"), True
        )
        with self.assertRaisesRegex(ValueError, "replaces a directory"):
            extractor.register_member(
                descendants, PurePosixPath("node"), False
            )

    def test_encrypted_zip_member_is_rejected_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "encrypted.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("payload", b"encrypted fixture")
            payload = bytearray(archive.read_bytes())
            local = payload.index(b"PK\x03\x04")
            central = payload.index(b"PK\x01\x02")
            payload[local + 6] |= 1
            payload[central + 8] |= 1
            archive.write_bytes(payload)
            destination = root / "encrypted"
            result = self.run_extract(archive, destination, "zip")
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(destination.exists())

    def test_member_and_uncompressed_size_limits_fail_closed(self):
        extractor = load_extractor()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "bounded.tar"
            with tarfile.open(archive, "w") as stream:
                for name in ("one", "two"):
                    member = tarfile.TarInfo(name)
                    member.size = 2
                    stream.addfile(member, io.BytesIO(b"xx"))

            member_destination = root / "member-limit"
            with mock.patch.object(extractor, "MAX_MEMBERS", 1):
                with self.assertRaisesRegex(ValueError, "member count"):
                    extractor.extract_archive(
                        archive, member_destination, "tar", 0
                    )
            self.assertFalse(member_destination.exists())

            size_destination = root / "size-limit"
            with mock.patch.object(extractor, "MAX_UNCOMPRESSED_BYTES", 3):
                with self.assertRaisesRegex(ValueError, "too large"):
                    extractor.extract_archive(archive, size_destination, "tar", 0)
            self.assertFalse(size_destination.exists())


if __name__ == "__main__":
    unittest.main()
