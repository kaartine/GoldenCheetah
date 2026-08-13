#!/usr/bin/env python3

import hashlib
import importlib.util
from pathlib import Path
import sys
from types import SimpleNamespace
from types import ModuleType
import tempfile
import unittest
from unittest import mock


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
INSTALLER_PATH = REPOSITORY_ROOT / ".devcontainer" / "install-verified-qt.py"
QT_LOCK_PATH = (
    REPOSITORY_ROOT / ".devcontainer" / "qt-6.8.3-linux-gcc64.lock"
)


def load_installer():
    spec = importlib.util.spec_from_file_location("install_verified_qt", INSTALLER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load verified Qt installer")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class VerifiedQtArchiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.installer = load_installer()

    def write_lock(self, root, payloads, digest_override=None):
        lock = root / "qt.lock"
        lines = [
            "@updater 6.8.3 desktop linux_gcc_64 linux - "
            "https://download.qt.io\n"
        ]
        for name, payload in payloads.items():
            digest = hashlib.sha256(payload).hexdigest()
            if digest_override and name in digest_override:
                digest = digest_override[name]
            lines.append(
                f"{digest}  6.8.3/gcc_64  "
                f"online/qtsdkrepository/fixture/{name}.7z\n"
            )
        lock.write_text("".join(lines), encoding="ascii")
        return lock

    @staticmethod
    def packages(payloads):
        return [
            SimpleNamespace(
                archive_path=f"online/qtsdkrepository/fixture/{name}.7z",
                archive=f"{name}.7z",
                archive_install_path="6.8.3/gcc_64",
            )
            for name in payloads
        ]

    def test_no_archive_is_extracted_until_every_digest_is_verified(self):
        payloads = {"base": b"verified base", "module": b"tampered module"}
        events = []

        def downloader(url, destination):
            name = Path(url).stem
            events.append(("download", name))
            destination.write_bytes(payloads[name])

        def extractor(archive, destination):
            events.append(("extract", archive.stem))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.write_lock(
                root,
                payloads,
                {"module": hashlib.sha256(b"expected module").hexdigest()},
            )
            with self.assertRaises(ValueError):
                self.installer.install_archives(
                    self.installer.parse_lock(lock).entries,
                    self.packages(payloads),
                    "https://download.qt.io",
                    root / "downloads",
                    root / "Qt",
                    downloader=downloader,
                    extractor=extractor,
                )

        self.assertEqual([event for event in events if event[0] == "extract"], [])

    def test_verified_set_is_extracted_in_lock_order(self):
        payloads = {"base": b"verified base", "module": b"verified module"}
        events = []

        def downloader(url, destination):
            name = Path(url).stem
            events.append(("download", name))
            destination.write_bytes(payloads[name])

        def extractor(archive, destination):
            events.append(("extract", archive.stem))

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.write_lock(root, payloads)
            self.installer.install_archives(
                self.installer.parse_lock(lock).entries,
                self.packages(payloads),
                "https://download.qt.io",
                root / "downloads",
                root / "Qt",
                downloader=downloader,
                extractor=extractor,
            )

        first_extract = next(index for index, event in enumerate(events) if event[0] == "extract")
        self.assertTrue(all(event[0] == "download" for event in events[:first_extract]))
        self.assertEqual(
            [event for event in events if event[0] == "extract"],
            [("extract", "base"), ("extract", "module")],
        )

    def test_lock_and_metadata_paths_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = root / "qt.lock"
            lock.write_text(
                "@updater 6.8.3 desktop linux_gcc_64 linux - "
                "https://download.qt.io\n"
                f"{'0' * 64}  6.8.3/gcc_64  "
                "online/qtsdkrepository/../escape.7z\n",
                encoding="ascii",
            )
            with self.assertRaises(ValueError):
                self.installer.parse_lock(lock)

            valid = self.write_lock(root, {"base": b"payload"})
            packages = self.packages({"different": b"payload"})
            with self.assertRaises(ValueError):
                self.installer.validate_package_plan(
                    self.installer.parse_lock(valid).entries, packages
                )

        self.installer.validate_archive_member("6.8.3/gcc_64/")
        for unsafe in ("../escape", "/absolute", "safe/../../escape", "safe\\escape"):
            with self.assertRaises(ValueError):
                self.installer.validate_archive_member(unsafe)

    def test_install_path_and_updater_inputs_are_locked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock = self.installer.parse_lock(self.write_lock(root, {"base": b"x"}))
            package = self.packages({"base": b"x"})[0]
            package.archive_install_path = "6.8.3/other"
            with self.assertRaisesRegex(ValueError, "installation path"):
                self.installer.validate_package_plan(lock.entries, [package])

            target = SimpleNamespace(
                version="6.8.3",
                target="desktop",
                arch="linux_gcc_64",
                os_name="linux",
            )
            self.installer.validate_updater_target(lock.updater, target)
            target.arch = "linux_gcc_64_repacked"
            with self.assertRaisesRegex(ValueError, "updater target"):
                self.installer.validate_updater_target(lock.updater, target)

    def test_reviewed_lock_matches_real_icu_install_destination(self):
        lock = self.installer.parse_lock(QT_LOCK_PATH)
        icu = [
            entry
            for entry in lock.entries
            if "icu-linux-Rhel8.6-x86_64.7z" in entry.repository_path
        ]
        self.assertEqual(len(icu), 1)
        self.assertEqual(icu[0].install_path, "6.8.3/gcc_64/lib")

    def test_archive_special_members_are_rejected_before_extraction(self):
        class Member:
            filename = "6.8.3/gcc_64/junction"
            is_directory = False
            is_file = False
            is_symlink = False
            is_junction = True
            is_socket = False

        events = []

        class Archive:
            files = [Member()]

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def extractall(self, path):
                events.append(Path(path))

        fake_py7zr = SimpleNamespace(SevenZipFile=lambda *_args, **_kwargs: Archive())
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            "sys.modules", {"py7zr": fake_py7zr}
        ):
            archive = Path(temporary) / "fixture.7z"
            archive.write_bytes(b"fixture")
            with self.assertRaisesRegex(ValueError, "non-regular"):
                self.installer.extract_verified_archive(
                    archive, Path(temporary) / "output"
                )
        self.assertEqual(events, [])

    def test_relative_archive_symlinks_are_staged_and_escape_is_rejected(self):
        class Member:
            def __init__(self, filename, payload, *, symlink=False):
                self.filename = filename
                self.payload = payload
                self.is_directory = False
                self.is_file = not symlink
                self.is_symlink = symlink
                self.is_junction = False
                self.is_socket = False
                self.uncompressed = len(payload)
                self.posix_mode = 0o777 if symlink else 0o755

        target = Member("lib/libfixture.so.1", b"verified payload")
        link = Member("lib/libfixture.so", b"libfixture.so.1", symlink=True)

        class Archive:
            def __init__(self, members):
                self.files = members

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                return False

            def extractall(self, path=None, *, factory=None):
                self.path = path
                for member in self.files:
                    writer = factory.create(
                        (Path(path) / member.filename).as_posix()
                    )
                    writer.write(member.payload)
                    writer.close()

        archives = [Archive([target, link])]
        fake_py7zr = SimpleNamespace(
            SevenZipFile=lambda *_args, **_kwargs: archives.pop(0)
        )
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            "sys.modules", {"py7zr": fake_py7zr}
        ):
            root = Path(temporary)
            archive = root / "fixture.7z"
            archive.write_bytes(b"fixture")
            output = root / "output"
            self.installer.extract_verified_archive(archive, output)
            self.assertEqual(
                (output / "lib/libfixture.so.1").read_bytes(),
                b"verified payload",
            )
            self.assertTrue((output / "lib/libfixture.so").is_symlink())
            self.assertEqual(
                (output / "lib/libfixture.so").readlink(),
                Path("libfixture.so.1"),
            )

        escape = Member("lib/libfixture.so", b"../../escape", symlink=True)
        archives = [Archive([target, escape])]
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(
            "sys.modules", {"py7zr": fake_py7zr}
        ):
            root = Path(temporary)
            archive = root / "fixture.7z"
            archive.write_bytes(b"fixture")
            with self.assertRaisesRegex(ValueError, "symlink"):
                self.installer.extract_verified_archive(
                    archive, root / "output"
                )
            self.assertFalse((root / "escape").exists())

    def test_archive_layout_registration_is_path_bounded(self):
        class NoIterationDict(dict):
            def __iter__(self):
                raise AssertionError("layout validation scanned every member")

        seen = NoIterationDict()
        self.installer.register_archive_layout_member(
            seen, "unrelated", False
        )
        self.installer.register_archive_layout_member(
            seen, "target", False
        )

        descendants = {}
        self.installer.register_archive_layout_member(
            descendants, "node/child", False
        )
        self.installer.register_archive_layout_member(
            descendants, "node", True
        )
        with self.assertRaisesRegex(ValueError, "replaces a directory"):
            self.installer.register_archive_layout_member(
                descendants, "node", False
            )

    def test_nonempty_output_is_rejected_before_any_download(self):
        payloads = {"base": b"verified base"}
        events = []

        def downloader(_url, destination):
            events.append("download")
            destination.write_bytes(payloads["base"])

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "Qt"
            output.mkdir()
            sentinel = output / "unreviewed"
            sentinel.write_bytes(b"pre-existing payload")
            lock = self.installer.parse_lock(self.write_lock(root, payloads))
            with self.assertRaisesRegex(ValueError, "output directory"):
                self.installer.install_archives(
                    lock.entries,
                    self.packages(payloads),
                    lock.updater.base_url,
                    root / "downloads",
                    output,
                    downloader=downloader,
                    extractor=lambda *_args: None,
                )
            self.assertEqual(sentinel.read_bytes(), b"pre-existing payload")
        self.assertEqual(events, [])

    def test_main_does_not_rewrite_aqt_target_before_validation(self):
        target = SimpleNamespace(
            version="6.8.3",
            target="desktop",
            arch="linux_gcc_64",
            os_name="windows",
        )

        class Archives:
            def __init__(self, *_args, **_kwargs):
                pass

            def get_target_config(self):
                return target

            def get_packages(self):
                return []

        updates = []

        class Updater:
            @staticmethod
            def update(*args):
                updates.append(args)

        aqt_module = ModuleType("aqt")
        archives_module = ModuleType("aqt.archives")
        updater_module = ModuleType("aqt.updater")
        archives_module.QtArchives = Archives
        updater_module.Updater = Updater

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            lock_path = self.write_lock(root, {"base": b"verified"})
            arguments = SimpleNamespace(
                lock=lock_path,
                output=root / "Qt",
                base="https://download.qt.io",
                version="6.8.3",
                arch="linux_gcc_64",
                expected_count=1,
                module=[],
            )
            with mock.patch.object(
                self.installer, "parse_arguments", return_value=arguments
            ), mock.patch.object(
                self.installer, "install_archives"
            ) as install_archives, mock.patch.dict(
                sys.modules,
                {
                    "aqt": aqt_module,
                    "aqt.archives": archives_module,
                    "aqt.updater": updater_module,
                },
            ):
                with self.assertRaisesRegex(ValueError, "updater target"):
                    self.installer.main()
            install_archives.assert_not_called()

        self.assertEqual(target.os_name, "windows")
        self.assertEqual(updates, [])


if __name__ == "__main__":
    unittest.main()
