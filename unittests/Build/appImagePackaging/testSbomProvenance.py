#!/usr/bin/env python3

import json
import hashlib
import base64
import csv
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
SBOM_GENERATOR = REPOSITORY_ROOT / "src/Resources/linux/generate-appimage-sbom.py"
RUNTIME_GENERATOR = REPOSITORY_ROOT / "src/Resources/linux/generate-runtime-provenance.py"
PACKAGING_SUPPORT = REPOSITORY_ROOT / "src/Resources/linux/AppImagePackagingSupport.sh"
PYTHON_NORMALIZER = REPOSITORY_ROOT / "src/Resources/linux/normalize-embedded-python.py"
LINUXDEPLOYQT_CAPTURE = (
    REPOSITORY_ROOT
    / "src/Resources/linux/capture-linuxdeployqt-transforms.py"
)


def load_python_module(name, path):
    spec = importlib.util.spec_from_file_location(
        name, path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("cannot load runtime provenance generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runtime_generator():
    return load_python_module("generate_runtime_provenance", RUNTIME_GENERATOR)


def create_test_wheel(path, name, version, files, record_override=None):
    distribution = name.replace("-", "_")
    dist_info = f"{distribution}-{version}.dist-info"
    metadata = (
        "Metadata-Version: 2.1\n"
        f"Name: {name}\n"
        f"Version: {version}\n"
        "License-Expression: MIT\n"
    ).encode("utf-8")
    members = dict(files)
    members[f"{dist_info}/METADATA"] = metadata
    members[f"{dist_info}/WHEEL"] = (
        b"Wheel-Version: 1.0\nGenerator: GoldenCheetah-test\n"
        b"Root-Is-Purelib: false\nTag: py3-none-any\n"
    )
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    for member_name in sorted(members):
        payload = members[member_name]
        digest = base64.urlsafe_b64encode(hashlib.sha256(payload).digest())
        writer.writerow(
            [
                member_name,
                "sha256=" + digest.decode("ascii").rstrip("="),
                str(len(payload)),
            ]
        )
    writer.writerow([f"{dist_info}/RECORD", "", ""])
    members[f"{dist_info}/RECORD"] = (
        record_override if record_override is not None else output.getvalue().encode()
    )
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for member_name in sorted(members):
            info = zipfile.ZipInfo(member_name, (2024, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, members[member_name])
    return metadata


class SbomProvenanceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runtime_generator = load_runtime_generator()
        cls.python_normalizer = load_python_module(
            "normalize_embedded_python", PYTHON_NORMALIZER
        )

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.appdir = self.root / "appdir"
        (self.appdir / "lib").mkdir(parents=True)
        (self.appdir / "lib" / "libQt6Core.so.6.8.3").write_bytes(b"qt")
        (self.appdir / "lib" / "libfixture.so.1").write_bytes(b"fixture")
        (self.appdir / "lib" / "libavcodec.so.61").write_bytes(b"qt ffmpeg")
        self.manifest = self.root / "manifest"
        self.manifest.write_text(
            "goldencheetah_appimage_manifest=2\n"
            f"source_revision={'1' * 40}\n"
            f"build_inputs_sha256={'3' * 64}\n"
            f"raw_elf_sha256={'2' * 64}\n"
            "toolchain=gcc-13.2.0_qt-6.8.3_cxx-17\n"
            "strava_oauth_configured=false\n",
            encoding="ascii",
        )
        python_root = self.appdir / "opt/python3.11"
        site_packages = python_root / "lib/python3.11/site-packages"
        site_packages.mkdir(parents=True)
        self.wheelhouse = self.root / "wheelhouse"
        self.wheelhouse.mkdir()
        self.wheel = self.wheelhouse / "lockedfixture-1.0-py3-none-any.whl"
        wheel_metadata = create_test_wheel(
            self.wheel,
            "lockedfixture",
            "1.0",
            {
                "lockedfixture/__init__.py": b"",
                "lockedfixture/native.so": b"authenticated wheel library",
            },
        )
        self.wheel_sha256 = hashlib.sha256(self.wheel.read_bytes()).hexdigest()
        self.lock = self.root / "requirements.lock"
        self.lock.write_text(
            "lockedfixture==1.0 \\\n"
            f"    --hash=sha256:{self.wheel_sha256}\n",
            encoding="ascii",
        )
        self.wheel_manifest = self.root / "python-wheels.json"
        self.python_normalizer.write_wheel_manifest(
            self.appdir,
            python_root,
            self.wheelhouse,
            self.lock,
            self.wheel_manifest,
        )
        locked_package = site_packages / "lockedfixture"
        locked_metadata = site_packages / "lockedfixture-1.0.dist-info"
        locked_package.mkdir()
        locked_metadata.mkdir()
        locked_package.joinpath("__init__.py").write_bytes(b"")
        locked_package.joinpath("native.so").write_bytes(
            b"authenticated wheel library"
        )
        locked_metadata.joinpath("METADATA").write_bytes(wheel_metadata)
        self.report = self.root / "pip-report.json"
        self.report.write_text(
            json.dumps(
                {
                    "version": "1",
                    "install": [
                        {
                            "download_info": {
                                "url": self.wheel.as_uri(),
                                "archive_info": {
                                    "hashes": {"sha256": self.wheel_sha256}
                                },
                            },
                            "metadata": {
                                "name": "lockedfixture",
                                "version": "1.0",
                            },
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        self.package_index = self.root / "package-index.json"
        self.package_index.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-runtime-fixture-index-1",
                    "libraries": [
                        {
                            "license": "MIT",
                            "name": "fixture-runtime",
                            "path": "lib/libfixture.so.1",
                            "provenance": "dpkg:fixture-runtime=9.1",
                            "purl": "pkg:deb/ubuntu/fixture-runtime@9.1",
                            "sha256": hashlib.sha256(b"fixture").hexdigest(),
                            "version": "9.1",
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )
        self.transformed_runtime_manifest = self.root / "transformed-runtime.json"
        self.transformed_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-transformed-runtime-1",
                    "libraries": [],
                }
            ),
            encoding="utf-8",
        )
        self.runtime = self.root / "runtime.json"
        self.python_runtime_sha256 = "a" * 64
        self.python_runtime_manifest = self.root / "python-runtime.json"
        self.python_interpreter = python_root / "bin/python3.11"
        self.python_interpreter.parent.mkdir(parents=True)
        self.python_interpreter.write_bytes(b"authenticated Python interpreter")
        interpreter_digest = hashlib.sha256(
            self.python_interpreter.read_bytes()
        ).hexdigest()
        self.python_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-python-source-runtime-2",
                    "source_sha256": self.python_runtime_sha256,
                    "distributions": [],
                    "files": [
                        {
                            "path": "opt/python3.11/bin/python3.11",
                            "source_sha256": interpreter_digest,
                            "output_sha256": interpreter_digest,
                            "transformation": "identity",
                        }
                    ],
                    "symlinks": [],
                }
            ),
            encoding="utf-8",
        )
        self.qt_root = self.root / "Qt"
        (self.qt_root / "lib").mkdir(parents=True)
        (self.qt_root / "lib" / "libQt6Core.so.6.8.3").write_bytes(b"qt")
        (self.qt_root / "lib" / "libavcodec.so.61.1").write_bytes(b"qt ffmpeg")
        (self.qt_root / "lib" / "libavcodec.so.61").symlink_to(
            "libavcodec.so.61.1"
        )
        (self.qt_root / "lib" / "libavutil.so.59.39.100").write_bytes(
            b"FFmpeg version 7.1\0"
            b"libavutil license: LGPL version 2.1 or later\0"
        )
        qt_sbom = self.qt_root / "sbom" / "qtmultimedia-6.8.3.spdx.json"
        qt_sbom.parent.mkdir()
        qt_sbom.write_text(
            json.dumps(
                {
                    "spdxVersion": "SPDX-2.3",
                    "name": "qtmultimedia-6.8.3",
                    "packages": [
                        {
                            "SPDXID": "SPDXRef-Package-Core",
                            "name": "Core",
                            "versionInfo": "6.8.3",
                            "licenseConcluded": (
                                "LicenseRef-Qt-Commercial OR LGPL-3.0-only OR "
                                "GPL-2.0-only OR GPL-3.0-only"
                            ),
                        },
                        {
                            "SPDXID": "SPDXRef-Package-FFmpeg",
                            "name": "FFmpeg",
                            "versionInfo": "unknown",
                            "licenseConcluded": "NOASSERTION",
                        },
                    ],
                    "files": [
                        {
                            "SPDXID": "SPDXRef-File-Core",
                            "fileName": "./lib/libQt6Core.so.6.8.3",
                        }
                    ],
                    "relationships": [
                        {
                            "spdxElementId": "SPDXRef-Package-Core",
                            "relatedSpdxElement": "SPDXRef-File-Core",
                            "relationshipType": "CONTAINS",
                        },
                        {
                            "spdxElementId": "SPDXRef-DOCUMENT",
                            "relatedSpdxElement": "SPDXRef-Package-FFmpeg",
                            "relationshipType": "DESCRIBES",
                        },
                    ],
                }
            ),
            encoding="utf-8",
        )
        self.runtime_generator.atomic_write(
            self.runtime,
            self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            ),
        )

    def tearDown(self):
        self.temporary.cleanup()

    def generate(self, config, output, check=True):
        return subprocess.run(
            [
                sys.executable,
                str(SBOM_GENERATOR),
                "--appdir",
                str(self.appdir),
                "--build-manifest",
                str(self.manifest),
                "--build-config",
                str(config),
                "--requirements-lock",
                str(self.lock),
                "--python-install-report",
                str(self.report),
                "--python-wheel-manifest",
                str(self.wheel_manifest),
                "--runtime-provenance",
                str(self.runtime),
                "--output",
                str(output),
                "--linuxdeployqt-file",
                "linuxdeployqt-fixture",
                "--linuxdeployqt-sha256",
                "4" * 64,
                "--appimagetool-file",
                "appimagetool-fixture",
                "--appimagetool-sha256",
                "5" * 64,
                "--appimage-runtime-file",
                "runtime-fixture",
                "--appimage-runtime-sha256",
                "a" * 64,
                "--python-runtime-file",
                "python-fixture",
                "--python-runtime-sha256",
                "6" * 64,
                "--srmio-revision",
                "7" * 40,
                "--srmio-source-sha256",
                "8" * 64,
                "--d2xx-linux-version",
                "1.4.27",
                "--d2xx-linux-sha256",
                "9" * 64,
            ],
            check=check,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def record_identity(payload):
        digest = hashlib.sha256(payload).digest()
        encoded = base64.urlsafe_b64encode(digest).decode("ascii").rstrip("=")
        return f"sha256={encoded},{len(payload)}"

    def runtime_arguments(self):
        return SimpleNamespace(
            appdir=self.appdir,
            d2xx_provenance="reviewed-d2xx-archive",
            d2xx_version="1.4.27",
            python_install_report=self.report,
            python_provenance="reviewed-python-runtime",
            python_runtime_manifest=self.python_runtime_manifest,
            python_runtime_sha256=self.python_runtime_sha256,
            python_version="3.11.15",
            python_wheel_manifest=self.wheel_manifest,
            qt_provenance="reviewed-qt-lock",
            qt_root=self.qt_root,
            qt_version="6.8.3",
            requirements_lock=self.lock,
            linuxdeployqt_sha256="d" * 64,
            transformed_runtime_manifest=self.transformed_runtime_manifest,
        )

    def fixture_entries(self):
        return self.runtime_generator._load_test_fixture_package_index(
            self.package_index,
            self.appdir,
            hashlib.sha256(self.package_index.read_bytes()).hexdigest(),
        )

    def test_features_and_runtime_provenance_follow_actual_inputs(self):
        disabled = self.root / "disabled.pri"
        disabled.write_text(
            "GSL_LIBS = -lgsl\n"
            "SRMIO_INSTALL = /stale/srmio\n"
            "SRMIO_INSTALL =\n"
            "D2XX_INCLUDE += ../stale-d2xx\n"
            "D2XX_INCLUDE -= ../stale-d2xx\n",
            encoding="ascii",
        )
        disabled_output = self.root / "disabled.json"
        self.generate(disabled, disabled_output)
        document = json.loads(disabled_output.read_text(encoding="utf-8"))
        subprocess.run(
            [
                "bash",
                "-c",
                'source "$1"; validate_appimage_sbom "$2"',
                "sbom-validator",
                str(PACKAGING_SUPPORT),
                str(disabled_output),
            ],
            check=True,
        )
        names = {entry["name"] for entry in document["components"]}
        self.assertIn("appimage-runtime", names)
        self.assertNotIn("srmio", names)
        self.assertNotIn("d2xx-linux", names)
        interpreter = next(
            entry
            for entry in document["components"]
            if entry["name"] == "opt/python3.11/bin/python3.11"
        )
        properties = {
            prop["name"]: prop["value"]
            for prop in interpreter["properties"]
        }
        self.assertEqual(
            properties["goldencheetah:role"],
            "payload-file",
        )
        self.assertEqual(
            properties["goldencheetah:python-runtime-role"],
            "authenticated-python-runtime-file",
        )
        self.assertEqual(
            properties["goldencheetah:python-source-artifact-sha256"],
            self.python_runtime_sha256,
        )
        self.assertEqual(
            properties["goldencheetah:python-transformation"], "identity"
        )

        runtime = [
            entry
            for entry in document["components"]
            if any(
                prop == {
                    "name": "goldencheetah:role",
                    "value": "identified-runtime-dependency",
                }
                for prop in entry.get("properties", [])
            )
        ]
        self.assertEqual(
            {entry["name"] for entry in runtime},
            {"Qt6Core", "FFmpeg", "fixture-runtime", "lockedfixture"},
        )
        wheel_runtime = next(
            entry for entry in runtime if entry["name"] == "lockedfixture"
        )
        wheel_provenance = next(
            prop["value"]
            for prop in wheel_runtime["properties"]
            if prop["name"] == "goldencheetah:provenance"
        )
        self.assertIn(f"wheel-sha256={self.wheel_sha256}", wheel_provenance)
        ffmpeg = next(entry for entry in runtime if entry["name"] == "FFmpeg")
        self.assertEqual(ffmpeg["version"], "7.1")
        self.assertEqual(
            ffmpeg["licenses"], [{"license": {"id": "LGPL-2.1-or-later"}}]
        )
        ffmpeg_provenance = next(
            prop["value"]
            for prop in ffmpeg["properties"]
            if prop["name"] == "goldencheetah:provenance"
        )
        self.assertIn("qtmultimedia-6.8.3.spdx.json", ffmpeg_provenance)
        self.assertIn("qt-source-sha256=", ffmpeg_provenance)
        for entry in runtime:
            self.assertTrue(entry["version"])
            self.assertTrue(entry["licenses"])
            self.assertTrue(
                any(
                    prop["name"] == "goldencheetah:provenance" and prop["value"]
                    for prop in entry["properties"]
                )
            )

        enabled = self.root / "enabled.pri"
        enabled.write_text(
            "SRMIO_INSTALL = /usr/local\nD2XX_INCLUDE = ../D2XX\n",
            encoding="ascii",
        )
        enabled_output = self.root / "enabled.json"
        self.generate(enabled, enabled_output)
        enabled_document = json.loads(enabled_output.read_text(encoding="utf-8"))
        enabled_names = {entry["name"] for entry in enabled_document["components"]}
        self.assertIn("srmio", enabled_names)
        self.assertIn("d2xx-linux", enabled_names)

    def test_incomplete_runtime_provenance_is_rejected(self):
        document = json.loads(self.runtime.read_text(encoding="utf-8"))
        document["libraries"].pop()
        self.runtime.write_text(json.dumps(document), encoding="utf-8")
        config = self.root / "config.pri"
        config.write_text("GSL_LIBS = -lgsl\n", encoding="ascii")
        result = self.generate(config, self.root / "invalid.json", check=False)
        self.assertNotEqual(result.returncode, 0)

    def test_debian_provenance_requires_exact_content_identity(self):
        payload = self.appdir / "lib" / "libuntrusted.so.1"
        payload.write_bytes(b"packaged bytes")
        installed = self.root / "installed" / "libuntrusted.so.1"
        installed.parent.mkdir()
        installed.write_bytes(b"different installed bytes")

        with mock.patch.object(
            self.runtime_generator,
            "command_output",
            return_value=f"libuntrusted: {installed}\n",
        ), mock.patch.object(
            self.runtime_generator, "candidate_score", return_value=20
        ), mock.patch.object(
            self.runtime_generator,
            "installed_package_metadata",
            return_value={
                "license": "MIT",
                "name": "libuntrusted",
                "provenance": "fixture",
                "purl": "pkg:deb/ubuntu/libuntrusted@1",
                "version": "1",
            },
        ):
            with self.assertRaises(ValueError):
                self.runtime_generator.resolve_debian_package(payload)

    def test_debian_provenance_requires_authenticated_deb_payload(self):
        payload = self.appdir / "lib" / "libauthenticated.so.1"
        payload.write_bytes(b"installed package bytes")
        installed = self.root / "installed" / "libauthenticated.so.1"
        installed.parent.mkdir()
        installed.write_bytes(payload.read_bytes())

        def dpkg_output(arguments):
            if arguments[:2] == ["dpkg-query", "-S"]:
                return f"libauthenticated: {installed}\n"
            if arguments[:2] == ["dpkg-query", "-W"]:
                return "libauthenticated\tlibauthenticated\t1.0\t1.0\tamd64"
            raise subprocess.CalledProcessError(100, arguments)

        with mock.patch.object(
            self.runtime_generator, "command_output", side_effect=dpkg_output
        ), mock.patch.object(
            self.runtime_generator,
            "package_license",
            return_value=("MIT", "f" * 64),
        ):
            with self.assertRaisesRegex(ValueError, "authenticated .deb"):
                self.runtime_generator.resolve_debian_package(payload)

    def test_debian_provenance_records_authenticated_apt_and_deb_digest(self):
        payload = self.appdir / "lib" / "libauthenticated.so.1"
        payload.write_bytes(b"authenticated package bytes")
        installed = self.root / "installed" / "libauthenticated.so.1"
        installed.parent.mkdir()
        installed.write_bytes(payload.read_bytes())
        artifact_digest = "d" * 64
        member_name = installed.as_posix().lstrip("/")

        def dpkg_output(arguments):
            if arguments[:2] == ["dpkg-query", "-S"]:
                return f"libauthenticated: {installed}\n"
            if arguments[:2] == ["dpkg-query", "-W"]:
                return "libauthenticated\tlibauthenticated\t1.0\t1.0\tamd64"
            raise AssertionError(f"unexpected command: {arguments}")

        with mock.patch.object(
            self.runtime_generator, "command_output", side_effect=dpkg_output
        ), mock.patch.object(
            self.runtime_generator,
            "package_license",
            return_value=("MIT", "f" * 64),
        ), mock.patch.object(
            self.runtime_generator,
            "authenticated_debian_artifact",
            return_value={
                "files": {
                    member_name: hashlib.sha256(payload.read_bytes()).hexdigest()
                },
                "sha256": artifact_digest,
            },
        ):
            metadata = self.runtime_generator.resolve_debian_package(payload)

        self.assertIn(
            f"apt-metadata-sha256={artifact_digest}", metadata["provenance"]
        )
        self.assertIn(f"deb-sha256={artifact_digest}", metadata["provenance"])
        self.assertIn(f"deb-member={member_name}", metadata["provenance"])

    def test_qt_spdx_paths_cannot_escape_the_sdk(self):
        document_path = self.qt_root / "sbom" / "qtmultimedia-6.8.3.spdx.json"
        document = json.loads(document_path.read_text(encoding="utf-8"))
        document["files"][0]["fileName"] = "./../outside/libQt6Core.so"
        document_path.write_text(json.dumps(document), encoding="utf-8")

        with self.assertRaises(ValueError):
            self.runtime_generator.load_qt_spdx_evidence(self.qt_root)

    def test_vendored_dist_info_is_not_a_separate_payload_owner(self):
        site_packages = (
            self.appdir
            / "opt/python3.11/lib/python3.11/site-packages"
        )
        vendored_metadata = (
            site_packages
            / "lockedfixture/_vendor/autocommand-2.2.2.dist-info"
        )
        vendored_metadata.mkdir(parents=True)
        vendored_metadata.joinpath("METADATA").write_text(
            "Metadata-Version: 2.1\n"
            "Name: autocommand\n"
            "Version: 2.2.2\n"
            "License-Expression: LGPL-3.0-only\n",
            encoding="utf-8",
        )
        installer = vendored_metadata / "INSTALLER"
        installer.write_text("pip\n", encoding="ascii")
        vendored_metadata.joinpath("RECORD").write_text(
            "autocommand-2.2.2.dist-info/INSTALLER,,\n",
            encoding="utf-8",
        )

        owners = self.runtime_generator.python_distribution_files(
            self.appdir,
            self.wheel_manifest,
            self.lock,
            self.report,
        )

        self.assertNotIn(installer.resolve(), owners)
        native = site_packages / "lockedfixture/native.so"
        self.assertEqual(owners[native.resolve()]["name"], "lockedfixture")

    def test_python_record_hash_size_and_scope_are_enforced(self):
        python_root = self.appdir / "opt/python3.11"
        wheelhouse = self.root / "bad-wheelhouse"
        wheelhouse.mkdir()
        wheel = wheelhouse / "badfixture-1.0-py3-none-any.whl"
        create_test_wheel(
            wheel,
            "badfixture",
            "1.0",
            {"badfixture/native.so": b"actual"},
            b"badfixture/native.so,sha256=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA,6\n",
        )
        lock = self.root / "bad-requirements.lock"
        lock.write_text(
            "badfixture==1.0 \\\n"
            f"    --hash=sha256:{hashlib.sha256(wheel.read_bytes()).hexdigest()}\n",
            encoding="ascii",
        )
        with self.assertRaisesRegex(ValueError, "RECORD identity"):
            self.python_normalizer.write_wheel_manifest(
                self.appdir,
                python_root,
                wheelhouse,
                lock,
                self.root / "bad-wheel-manifest.json",
            )

    def test_forged_top_level_dist_info_is_not_a_python_owner(self):
        site_packages = self.appdir / "opt/python3.11/lib/python3.11/site-packages"
        metadata = site_packages / "forged-1.0.dist-info"
        metadata.mkdir(parents=True)
        metadata.joinpath("METADATA").write_text(
            "Metadata-Version: 2.1\nName: forged\nVersion: 1.0\nLicense: MIT\n",
            encoding="utf-8",
        )
        payload = site_packages / "forged.so"
        payload.write_bytes(b"forged payload")
        metadata.joinpath("RECORD").write_text(
            "forged.so," + self.record_identity(payload.read_bytes()) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "authenticated wheel"):
            self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            )

    def test_bundled_python_library_uses_pinned_runtime_manifest(self):
        library = self.appdir / "usr/lib/libXft.so.2"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"python appimage libXft")
        digest = hashlib.sha256(library.read_bytes()).hexdigest()
        self.python_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-python-source-runtime-2",
                    "source_sha256": self.python_runtime_sha256,
                    "distributions": [],
                    "files": [
                        {
                            "path": "usr/lib/libXft.so.2",
                            "source_sha256": digest,
                            "output_sha256": digest,
                            "transformation": "identity",
                        }
                    ],
                    "symlinks": [],
                }
            ),
            encoding="utf-8",
        )
        output = self.root / "python-runtime-output.json"
        self.runtime_generator.atomic_write(
            output,
            self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            ),
        )

        document = json.loads(output.read_text(encoding="utf-8"))
        entry = next(
            item
            for item in document["libraries"]
            if item["path"] == "usr/lib/libXft.so.2"
        )
        self.assertEqual(entry["name"], "python-appimage-runtime")
        self.assertEqual(
            entry["license"],
            "LicenseRef-python-appimage-bundled-library",
        )

    def test_python_runtime_manifest_is_exactly_consumed_and_hashed(self):
        config = self.root / "runtime-config.pri"
        config.write_text("GSL_LIBS = -lgsl\n", encoding="ascii")
        library = self.appdir / "usr/lib/libRuntimeFixture.so.1"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"actual runtime")

        self.python_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-python-source-runtime-2",
                    "source_sha256": self.python_runtime_sha256,
                    "distributions": [],
                    "files": [
                        {
                            "path": "usr/lib/libRuntimeFixture.so.1",
                            "source_sha256": hashlib.sha256(
                                b"other runtime"
                            ).hexdigest(),
                            "output_sha256": hashlib.sha256(
                                b"other runtime"
                            ).hexdigest(),
                            "transformation": "identity",
                        }
                    ],
                    "symlinks": [],
                }
            ),
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ValueError, "manifest digest mismatch"):
            self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            )

        library.unlink()
        with self.assertRaisesRegex(ValueError, "not present in AppDir"):
            self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            )

    def runtime_command(self, output):
        return [
            sys.executable,
            str(RUNTIME_GENERATOR),
            "--appdir", str(self.appdir),
            "--output", str(output),
            "--qt-version", "6.8.3",
            "--qt-root", str(self.qt_root),
            "--qt-provenance", "reviewed-qt-lock",
            "--python-version", "3.11.15",
            "--python-provenance", "reviewed-python-runtime",
            "--python-runtime-manifest", str(self.python_runtime_manifest),
            "--python-runtime-sha256", self.python_runtime_sha256,
            "--python-wheel-manifest", str(self.wheel_manifest),
            "--requirements-lock", str(self.lock),
            "--python-install-report", str(self.report),
            "--d2xx-version", "1.4.27",
            "--d2xx-provenance", "reviewed-d2xx-archive",
            "--linuxdeployqt-sha256", "d" * 64,
            "--transformed-runtime-manifest", str(self.transformed_runtime_manifest),
            "--test-mode",
            "--fixture-package-index", str(self.package_index),
            "--fixture-package-index-sha256",
            hashlib.sha256(self.package_index.read_bytes()).hexdigest(),
        ]

    def test_production_cli_cannot_activate_runtime_fixtures(self):
        result = subprocess.run(
            self.runtime_command(self.root / "production-fixture.json"),
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unrecognized arguments", result.stderr)

    def test_fixture_index_requires_canonical_path_and_preauthorized_digest(self):
        approved_digest = hashlib.sha256(self.package_index.read_bytes()).hexdigest()
        entries = self.runtime_generator._load_test_fixture_package_index(
            self.package_index,
            self.appdir,
            approved_digest,
        )
        self.assertIn("lib/libfixture.so.1", entries)

        noncanonical_parent = self.root / "fixture-parent"
        noncanonical_parent.mkdir()
        noncanonical = noncanonical_parent / ".." / self.package_index.name
        with self.assertRaisesRegex(ValueError, "canonical"):
            self.runtime_generator._load_test_fixture_package_index(
                noncanonical,
                self.appdir,
                approved_digest,
            )

        (self.appdir / "lib" / "libfixture.so.1").write_bytes(b"forged fixture")
        document = json.loads(self.package_index.read_text(encoding="utf-8"))
        document["libraries"][0]["sha256"] = hashlib.sha256(
            b"forged fixture"
        ).hexdigest()
        self.package_index.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "preauthorized SHA-256"):
            self.runtime_generator._load_test_fixture_package_index(
                self.package_index,
                self.appdir,
                approved_digest,
            )

    def test_source_python_manifest_authenticates_all_payload_categories(self):
        payload = self.root / "python-source"
        python_root = payload / "opt/python3.11"
        site_packages = python_root / "lib/python3.11/site-packages"
        dist_info = site_packages / "sourcefixture-1.0.dist-info"
        (python_root / "bin").mkdir(parents=True)
        dist_info.mkdir(parents=True)
        (payload / "usr/share/python-runtime").mkdir(parents=True)
        interpreter = python_root / "bin/python3.11"
        interpreter.write_bytes(b"authenticated interpreter")
        console = python_root / "bin/source-tool"
        console.write_bytes(b"#!/tmp/build/python3.11\nprint('ok')\n")
        stdlib = python_root / "lib/python3.11/os.py"
        stdlib.parent.mkdir(parents=True, exist_ok=True)
        stdlib.write_bytes(b"authenticated stdlib")
        config = python_root / "pyvenv.cfg"
        config.write_bytes(b"home = /opt/python3.11\n")
        data = payload / "usr/share/python-runtime/data.bin"
        data.write_bytes(b"authenticated data")
        link = python_root / "bin/python3"
        link.symlink_to("python3.11")
        record = dist_info / "RECORD"
        record.write_text(
            "../../../bin/source-tool,sha256=obsolete,1\n"
            "sourcefixture-1.0.dist-info/RECORD,,\n",
            encoding="utf-8",
        )
        manifest = self.root / "complete-python-runtime.json"
        source_state = self.python_normalizer.capture_runtime_source(payload)
        changed_scripts = self.python_normalizer.normalize_scripts(python_root)
        changed_records = self.python_normalizer.update_records(
            python_root, changed_scripts
        )
        self.python_normalizer.write_runtime_manifest(
            payload,
            manifest,
            self.python_runtime_sha256,
            source_state,
            changed_scripts,
            changed_records,
        )

        loaded = self.runtime_generator.load_python_runtime_manifest(
            manifest, payload, self.python_runtime_sha256
        )
        authenticated = {entry["path"] for entry in loaded["files"]}
        for expected in (interpreter, stdlib, config, data, console, record):
            self.assertIn(expected.relative_to(payload).as_posix(), authenticated)
        transformations = {
            entry["path"]: entry["transformation"]
            for entry in loaded["files"]
        }
        self.assertEqual(
            transformations[console.relative_to(payload).as_posix()],
            "python-console-script-wrapper-v1",
        )
        self.assertEqual(
            transformations[record.relative_to(payload).as_posix()],
            "python-wheel-record-refresh-v1",
        )

        for target in (interpreter, stdlib, config, data, console, record):
            original = target.read_bytes()
            target.write_bytes(b"substituted")
            with self.subTest(path=target.relative_to(payload)):
                with self.assertRaisesRegex(ValueError, "manifest digest mismatch"):
                    self.runtime_generator.load_python_runtime_manifest(
                        manifest, payload, self.python_runtime_sha256
                    )
                target.write_bytes(original)

        link.unlink()
        link.symlink_to("source-tool")
        with self.assertRaisesRegex(ValueError, "symlink target mismatch"):
            self.runtime_generator.load_python_runtime_manifest(
                manifest, payload, self.python_runtime_sha256
            )

        document = json.loads(manifest.read_text(encoding="utf-8"))
        console_entry = next(
            entry
            for entry in document["files"]
            if entry["path"] == console.relative_to(payload).as_posix()
        )
        console_entry["transformation"] = "identity"
        manifest.write_text(json.dumps(document), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "undeclared Python transformation"):
            self.runtime_generator.load_python_runtime_manifest(
                manifest, payload, self.python_runtime_sha256
            )

    def test_python_source_capture_uses_manifest_path_order(self):
        payload = self.root / "python-path-order"
        nested = payload / "usr/foo/bar"
        sibling = payload / "usr/foo-bar"
        nested.parent.mkdir(parents=True)
        nested.write_bytes(b"nested")
        sibling.write_bytes(b"sibling")

        entries = self.python_normalizer.capture_runtime_source(payload)
        paths = [entry["path"] for entry in entries]
        self.assertEqual(paths, ["usr/foo-bar", "usr/foo/bar"])

    def test_locked_wheel_replacement_removes_source_distribution_first(self):
        payload = self.root / "python-wheel-overlap"
        python_root = payload / "opt/python3.11"
        site_packages = python_root / "lib/python3.11/site-packages"
        replaced = site_packages / "packaging"
        dist_info = site_packages / "packaging-1.0.dist-info"
        unrelated = site_packages / "pip"
        script = python_root / "bin/packaging-tool"
        dist_info.mkdir(parents=True)
        replaced.mkdir()
        unrelated.mkdir()
        script.parent.mkdir()
        module = replaced / "__init__.py"
        module.write_bytes(b"source runtime package")
        metadata = dist_info / "METADATA"
        metadata.write_text(
            "Metadata-Version: 2.1\nName: packaging\nVersion: 1.0\n",
            encoding="ascii",
        )
        record = dist_info / "RECORD"
        record.write_text(
            "packaging/__init__.py,,\n"
            "packaging/__pycache__/missing.pyc,,\n"
            "packaging-1.0.dist-info/METADATA,,\n"
            "packaging-1.0.dist-info/RECORD,,\n"
            "../../../bin/packaging-tool,,\n",
            encoding="ascii",
        )
        script.write_bytes(b"#!/tmp/python\n")
        (unrelated / "__init__.py").write_bytes(b"pip remains")
        lock = self.root / "overlap-requirements.lock"
        lock.write_text(
            "packaging==1.0 \\\n"
            f"    --hash=sha256:{'1' * 64}\n",
            encoding="ascii",
        )

        self.python_normalizer.remove_locked_source_distributions(
            python_root, lock
        )
        self.assertFalse(replaced.exists())
        self.assertFalse(dist_info.exists())
        self.assertFalse(script.exists())
        self.assertTrue((unrelated / "__init__.py").is_file())

        escaped = self.root / "must-remain"
        escaped.write_bytes(b"outside")
        dist_info.mkdir()
        metadata.write_text(
            "Metadata-Version: 2.1\nName: packaging\nVersion: 1.0\n",
            encoding="ascii",
        )
        record.write_text(
            f"{'../' * 12}{escaped.name},,\n",
            encoding="ascii",
        )
        with self.assertRaisesRegex(ValueError, "escapes"):
            self.python_normalizer.remove_locked_source_distributions(
                python_root, lock
            )
        self.assertEqual(escaped.read_bytes(), b"outside")

    def test_base_python_runtime_manifest_is_created_before_pip_install(self):
        support = PACKAGING_SUPPORT.read_text(encoding="utf-8")
        function = support.split("install_embedded_python()", 1)[1]
        function = function.split("\n)", 1)[0]
        self.assertLess(
            function.index("--runtime-manifest"),
            function.index("pip install"),
        )
        self.assertLess(
            function.index("--remove-locked-source-distributions"),
            function.index("pip install"),
        )

    def test_transformed_runtime_binds_source_output_and_relative_path(self):
        source = self.root / "source" / "libtransformed.so.1"
        source.parent.mkdir()
        source.write_bytes(b"authenticated package payload")
        output = self.appdir / "lib" / "libtransformed.so.1"
        output.write_bytes(b"post-patchelf payload")
        source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
        output_digest = hashlib.sha256(output.read_bytes()).hexdigest()
        self.transformed_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-transformed-runtime-1",
                    "libraries": [
                        {
                            "output_sha256": output_digest,
                            "path": "lib/libtransformed.so.1",
                            "source_path": str(source),
                            "source_sha256": source_digest,
                            "transformation": "patchelf-set-rpath:$ORIGIN",
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

        entries = self.runtime_generator.load_transformed_runtime_manifest(
            self.transformed_runtime_manifest, self.appdir
        )
        package_metadata = {
            "license": "MIT",
            "name": "transformed-runtime",
            "provenance": (
                "pkg:deb/ubuntu/transformed-runtime@1"
                f";apt-metadata-sha256={'d' * 64}"
                f";deb-sha256={'d' * 64}"
            ),
            "purl": "pkg:deb/ubuntu/transformed-runtime@1",
            "version": "1",
        }
        with mock.patch.object(
            self.runtime_generator,
            "resolve_debian_package",
            return_value=package_metadata,
        ) as resolver:
            entry = self.runtime_generator.transformed_runtime_component(
                output,
                self.appdir,
                entries["lib/libtransformed.so.1"],
            )
        resolver.assert_called_once_with(source)
        self.assertEqual(entry["path"], "lib/libtransformed.so.1")
        self.assertIn(f"source-sha256={source_digest}", entry["provenance"])
        self.assertIn(f"deb-sha256={'d' * 64}", entry["provenance"])
        self.assertIn(f"output-sha256={output_digest}", entry["provenance"])
        self.assertIn("runtime-path=lib/libtransformed.so.1", entry["provenance"])
        self.assertNotIn(str(self.root), entry["provenance"])

        arguments = self.runtime_arguments()
        with mock.patch.object(
            self.runtime_generator,
            "resolve_debian_package",
            return_value=package_metadata,
        ):
            document = self.runtime_generator.build_document(
                arguments, fixture_index=self.fixture_entries()
            )
        transformed = next(
            item
            for item in document["libraries"]
            if item["path"] == "lib/libtransformed.so.1"
        )
        self.assertEqual(transformed["name"], "transformed-runtime")
        self.assertIn(f"output-sha256={output_digest}", transformed["provenance"])

        output.write_bytes(b"changed transformed payload")
        with self.assertRaisesRegex(ValueError, "output digest mismatch"):
            self.runtime_generator.transformed_runtime_component(
                output,
                self.appdir,
                entries["lib/libtransformed.so.1"],
            )

        output.write_bytes(b"post-patchelf payload")
        source.write_bytes(b"changed source payload")
        with self.assertRaisesRegex(ValueError, "source digest mismatch"):
            self.runtime_generator.transformed_runtime_component(
                output,
                self.appdir,
                entries["lib/libtransformed.so.1"],
            )

    def test_linuxdeployqt_capture_binds_source_tool_and_elf_identity(self):
        capture = load_python_module(
            "capture_linuxdeployqt_transforms", LINUXDEPLOYQT_CAPTURE
        )
        source = self.root / "system/libaudio.so.1.2.3"
        source.parent.mkdir()
        source.write_bytes(b"authenticated source library")
        output = self.appdir / "lib/libaudio.so.1"
        output.write_bytes(b"linuxdeployqt transformed library")
        tool_digest = "d" * 64
        snapshot = {
            "format": "goldencheetah-linuxdeployqt-source-snapshot-1",
            "libraries": [
                {
                    "path": str(source.resolve()),
                    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "soname": "libaudio.so.1",
                }
            ],
        }
        authenticated = []

        def elf_identity(path):
            return {
                "build_id": "0123456789abcdef",
                "rpath": "$ORIGIN" if path == output else "",
                "soname": "libaudio.so.1",
            }

        entries = capture.build_transformed_entries(
            self.appdir,
            snapshot,
            tool_digest,
            elf_identity=elf_identity,
            authenticate_source=lambda path: authenticated.append(path),
        )
        self.assertEqual(authenticated, [source.resolve()])
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["path"], "lib/libaudio.so.1")
        self.assertEqual(
            entries[0]["transformation"],
            f"linuxdeployqt-no-strip:{tool_digest}:rpath=$ORIGIN",
        )

        manifest = self.root / "linuxdeployqt-transforms.json"
        manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-transformed-runtime-1",
                    "libraries": entries,
                }
            ),
            encoding="utf-8",
        )
        loaded = self.runtime_generator.load_transformed_runtime_manifest(
            manifest, self.appdir, tool_digest
        )
        self.assertIn("lib/libaudio.so.1", loaded)
        with self.assertRaisesRegex(ValueError, "transformation"):
            self.runtime_generator.load_transformed_runtime_manifest(
                manifest, self.appdir, "e" * 64
            )

        def wrong_output_identity(path):
            identity = elf_identity(path)
            if path == output:
                identity["rpath"] = "/untrusted"
            return identity

        unauthenticated = []
        self.assertEqual(
            capture.build_transformed_entries(
                self.appdir,
                snapshot,
                tool_digest,
                elf_identity=wrong_output_identity,
                authenticate_source=lambda path: unauthenticated.append(path),
            ),
            [],
        )
        self.assertEqual(unauthenticated, [])

        source.write_bytes(b"changed after snapshot")
        with self.assertRaisesRegex(ValueError, "source changed"):
            capture.build_transformed_entries(
                self.appdir,
                snapshot,
                tool_digest,
                elf_identity=elf_identity,
                authenticate_source=lambda path: None,
            )

    def test_linuxdeployqt_capture_binds_transformed_qml_plugin_to_qt_sdk(self):
        capture = load_python_module(
            "capture_linuxdeployqt_qml_plugin", LINUXDEPLOYQT_CAPTURE
        )
        source = (
            self.qt_root
            / "qml/QtQml/Models/libmodelsplugin.so"
        )
        source.parent.mkdir(parents=True)
        source.write_bytes(b"authenticated Qt QML plugin")
        output = (
            self.appdir
            / "qml/QtQml/Models/libmodelsplugin.so"
        )
        output.parent.mkdir(parents=True)
        output.write_bytes(b"linuxdeployqt transformed Qt QML plugin")
        snapshot = {
            "format": "goldencheetah-linuxdeployqt-source-snapshot-1",
            "libraries": [],
        }

        def elf_identity(path):
            return {
                "build_id": "0123456789abcdef",
                "rpath": (
                    "$ORIGIN:$ORIGIN/../../../lib:$ORIGIN/../../../lib"
                    if path == output
                    else "$ORIGIN/../../../lib"
                ),
                "soname": "",
            }

        entries = capture.build_transformed_entries(
            self.appdir,
            snapshot,
            "d" * 64,
            qt_root=self.qt_root,
            elf_identity=elf_identity,
            authenticate_source=lambda path: self.fail(
                f"Qt SDK source was treated as a Debian payload: {path}"
            ),
        )

        self.assertEqual(len(entries), 1)
        self.assertEqual(
            entries[0]["path"],
            "qml/QtQml/Models/libmodelsplugin.so",
        )
        self.assertEqual(entries[0]["source_path"], str(source.resolve()))
        self.assertEqual(
            entries[0]["transformation"],
            f"linuxdeployqt-no-strip:{'d' * 64}:rpath=relative-lib",
        )

        def untrusted_rpath_identity(path):
            identity = elf_identity(path)
            if path == output:
                identity["rpath"] += ":/tmp/untrusted"
            return identity

        self.assertEqual(
            capture.build_transformed_entries(
                self.appdir,
                snapshot,
                "d" * 64,
                qt_root=self.qt_root,
                elf_identity=untrusted_rpath_identity,
                authenticate_source=lambda path: None,
            ),
            [],
        )

    def test_linuxdeployqt_capture_accepts_plugin_without_soname(self):
        capture = load_python_module(
            "capture_linuxdeployqt_sonameless_plugin", LINUXDEPLOYQT_CAPTURE
        )
        plugin = self.root / "libmodelsplugin.so"
        plugin.write_bytes(b"ELF fixture")

        def command_text(arguments):
            if arguments[0].endswith("readelf"):
                return "Build ID: 0123456789abcdef"
            if "--print-soname" in arguments:
                return ""
            if "--print-rpath" in arguments:
                return "$ORIGIN/../../../lib"
            self.fail(f"unexpected ELF identity command: {arguments}")

        with mock.patch.object(capture.shutil, "which", side_effect=lambda name: name), \
             mock.patch.object(capture, "command_text", side_effect=command_text):
            identity = capture.default_elf_identity(plugin)

        self.assertEqual(identity["soname"], "")
        self.assertEqual(identity["build_id"], "0123456789abcdef")

    def test_transformed_qml_plugin_keeps_qt_provenance(self):
        source = self.qt_root / "qml/QtQml/Models/libmodelsplugin.so"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"authenticated Qt QML plugin")
        output = self.appdir / "qml/QtQml/Models/libmodelsplugin.so"
        output.parent.mkdir(parents=True)
        output.write_bytes(b"linuxdeployqt transformed Qt QML plugin")
        source_digest = hashlib.sha256(source.read_bytes()).hexdigest()
        output_digest = hashlib.sha256(output.read_bytes()).hexdigest()
        transformation = (
            f"linuxdeployqt-no-strip:{'d' * 64}:rpath=relative-lib"
        )
        self.transformed_runtime_manifest.write_text(
            json.dumps(
                {
                    "format": "goldencheetah-transformed-runtime-1",
                    "libraries": [
                        {
                            "output_sha256": output_digest,
                            "path": "qml/QtQml/Models/libmodelsplugin.so",
                            "source_path": str(source.resolve()),
                            "source_sha256": source_digest,
                            "transformation": transformation,
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

        with mock.patch.object(
            self.runtime_generator,
            "resolve_debian_package",
            side_effect=AssertionError("Qt QML plugin used Debian provenance"),
        ):
            document = self.runtime_generator.build_document(
                self.runtime_arguments(), fixture_index=self.fixture_entries()
            )

        plugin = next(
            entry
            for entry in document["libraries"]
            if entry["path"] == "qml/QtQml/Models/libmodelsplugin.so"
        )
        self.assertEqual(plugin["name"], "Qt-distributed-libmodelsplugin")
        self.assertIn(f"qt-source-sha256={source_digest}", plugin["provenance"])
        self.assertIn(f"transformation={transformation}", plugin["provenance"])
        self.assertIn(f"output-sha256={output_digest}", plugin["provenance"])

    def test_linuxdeployqt_capture_accepts_soname_alias_output(self):
        capture = load_python_module(
            "capture_linuxdeployqt_soname_alias", LINUXDEPLOYQT_CAPTURE
        )
        source = self.root / "system/libbz2.so.1.0.4"
        source.parent.mkdir()
        source.write_bytes(b"authenticated libbz2 package payload")
        output = self.appdir / "lib/libbz2.so.1"
        output.write_bytes(b"linuxdeployqt transformed libbz2 payload")
        snapshot = {
            "format": "goldencheetah-linuxdeployqt-source-snapshot-1",
            "libraries": [
                {
                    "path": str(source.resolve()),
                    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                    "soname": "libbz2.so.1.0",
                }
            ],
        }

        def elf_identity(path):
            return {
                "build_id": "0123456789abcdef",
                "rpath": "$ORIGIN" if path == output else "",
                "soname": "libbz2.so.1.0",
            }

        authenticated = []
        entries = capture.build_transformed_entries(
            self.appdir,
            snapshot,
            "d" * 64,
            elf_identity=elf_identity,
            authenticate_source=lambda path: authenticated.append(path),
        )

        self.assertEqual(authenticated, [source.resolve()])
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0]["path"], "lib/libbz2.so.1")

    def test_linuxdeployqt_snapshot_includes_package_private_libraries(self):
        capture = load_python_module(
            "capture_linuxdeployqt_private_library", LINUXDEPLOYQT_CAPTURE
        )
        public = self.root / "system/libpublic.so.1"
        private = self.root / "system/pulseaudio/libprivate.so.1"
        private.parent.mkdir(parents=True)
        public.write_bytes(b"public library")
        private.write_bytes(b"private package library")

        def run(command, **_kwargs):
            if command[0].endswith("ldconfig"):
                return SimpleNamespace(
                    stdout=(
                        f"libpublic.so.1 (libc6,x86-64) => {public}\n"
                    )
                )
            if command[:2] == ["/usr/bin/dpkg-query", "-S"]:
                return SimpleNamespace(
                    stdout=f"private-package:amd64: {private}\n"
                )
            self.fail(f"unexpected source-discovery command: {command}")

        with mock.patch.object(
            capture.shutil,
            "which",
            side_effect=lambda name: {
                "ldconfig": "/usr/sbin/ldconfig",
                "dpkg-query": "/usr/bin/dpkg-query",
            }.get(name),
        ), mock.patch.object(capture.subprocess, "run", side_effect=run):
            entries = capture.ldconfig_source_entries()

        by_soname = {entry["soname"]: entry for entry in entries}
        self.assertEqual(
            by_soname["libprivate.so.1"]["path"],
            str(private.resolve()),
        )
        self.assertEqual(
            by_soname["libprivate.so.1"]["sha256"],
            hashlib.sha256(private.read_bytes()).hexdigest(),
        )

    def test_runtime_library_paths_follow_serialized_posix_order(self):
        nested = self.appdir / "opt/python/site-packages/numpy/random"
        sibling = self.appdir / "opt/python/site-packages/numpy.libs"
        nested.mkdir(parents=True)
        sibling.mkdir(parents=True)
        nested.joinpath("mtrand.so").write_bytes(b"nested")
        sibling.joinpath("libfixture.so.1").write_bytes(b"sibling")

        paths = [
            path.relative_to(self.appdir).as_posix()
            for path in self.runtime_generator.runtime_library_paths(
                self.appdir
            )
        ]

        self.assertEqual(paths, sorted(paths))


if __name__ == "__main__":
    unittest.main()
