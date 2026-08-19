#!/usr/bin/env python3
"""Regression coverage for the release hardening audit findings."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import tempfile
import textwrap
import unittest


TEST_DIR = Path(__file__).resolve().parent
REPOSITORY_ROOT = TEST_DIR.parents[2]
SUPPORT = REPOSITORY_ROOT / "src/Resources/linux/AppImagePackagingSupport.sh"
OFFSET_READER = REPOSITORY_ROOT / "src/Resources/linux/read-appimage-offset.py"
PAYLOAD_VERIFIER = REPOSITORY_ROOT / "src/Resources/linux/verify-appimage-payload.py"
BUILD_INPUT_IDENTITY = (
    REPOSITORY_ROOT / "src/Resources/linux/compute-build-input-identity.py"
)
APPVEYOR_INPUTS = REPOSITORY_ROOT / "appveyor/linux/build-input-paths.sh"
REPRODUCE_APPIMAGE = REPOSITORY_ROOT / "appveyor/linux/reproduce-appimage.sh"
PACKAGE_APPIMAGE = REPOSITORY_ROOT / "appveyor/linux/package-appimage-pass.sh"


def run(arguments, **kwargs):
    return subprocess.run(arguments, text=True, capture_output=True, **kwargs)


def file_component(appdir, relative):
    path = appdir / relative
    metadata = path.lstat()
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    return {
        "bom-ref": f"goldencheetah:file:{relative}:{digest}",
        "type": "file",
        "name": relative,
        "hashes": [{"alg": "SHA-256", "content": digest}],
        "properties": [
            {"name": "goldencheetah:role", "value": "payload-file"},
            {"name": "goldencheetah:size", "value": str(metadata.st_size)},
            {
                "name": "goldencheetah:mode",
                "value": f"{stat.S_IMODE(metadata.st_mode):04o}",
            },
        ],
    }


def symlink_component(appdir, relative):
    return {
        "bom-ref": f"goldencheetah:symlink:{relative}",
        "type": "file",
        "name": relative,
        "properties": [
            {"name": "goldencheetah:role", "value": "payload-symlink"},
            {
                "name": "goldencheetah:symlink-target",
                "value": os.readlink(appdir / relative),
            },
        ],
    }


class AppImageTrustBoundaryTests(unittest.TestCase):
    def build_type2_fixture(self, directory):
        compiler = shutil.which("cc")
        mksquashfs = shutil.which("mksquashfs")
        if compiler is None or mksquashfs is None:
            self.skipTest("cc and mksquashfs are required")

        source = directory / "runtime.c"
        runtime = directory / "fixture.AppImage"
        marker = directory / "executed"
        source.write_text(
            textwrap.dedent(
                """
                #include <stdio.h>
                #include <stdlib.h>
                int main(void) {
                    const char *path = getenv("GC_EXECUTION_MARKER");
                    if (path) {
                        FILE *stream = fopen(path, "w");
                        if (stream) { fputs("executed", stream); fclose(stream); }
                    }
                    return 71;
                }
                """
            ),
            encoding="ascii",
        )
        subprocess.run(
            [compiler, "-Wl,--build-id=none", "-o", runtime, source], check=True
        )

        data = bytearray(runtime.read_bytes())
        self.assertEqual(data[:4], b"\x7fELF")
        byte_order = "<" if data[5] == 1 else ">"
        if data[4] == 2:
            values = struct.unpack_from(byte_order + "HHIQQQIHHHHHH", data, 16)
        elif data[4] == 1:
            values = struct.unpack_from(byte_order + "HHIIIIIHHHHHH", data, 16)
        else:
            self.fail("unsupported ELF fixture class")
        section_offset = values[5]
        section_entry_size = values[10]
        section_count = values[11]
        squashfs_offset = section_offset + section_entry_size * section_count
        self.assertLessEqual(len(data), squashfs_offset)
        data.extend(b"\0" * (squashfs_offset - len(data)))
        data[8:11] = b"AI\x02"
        runtime.write_bytes(data)
        runtime.chmod(0o700)

        payload = directory / "payload"
        payload.mkdir()
        (payload / "AppRun").write_text("#!/bin/sh\nexit 0\n", encoding="ascii")
        (payload / "AppRun").chmod(0o755)
        (payload / "trusted.txt").write_text("trusted payload\n", encoding="ascii")
        squashfs = directory / "payload.squashfs"
        subprocess.run(
            [
                mksquashfs,
                payload,
                squashfs,
                "-noappend",
                "-all-root",
                "-no-xattrs",
                "-no-progress",
                "-processors",
                "1",
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        with runtime.open("ab") as stream:
            stream.write(squashfs.read_bytes())
        return runtime, marker

    def test_trusted_extraction_does_not_execute_runtime(self):
        self.assertTrue(OFFSET_READER.is_file(), "trusted offset reader is missing")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image, marker = self.build_type2_fixture(root)
            destination = root / "extract"
            destination.mkdir()
            result = run(
                [
                    "bash",
                    "-c",
                    'set -euo pipefail; . "$1"; '
                    'trusted_appimage_extract "$2" "$3"',
                    "bash",
                    str(SUPPORT),
                    str(image),
                    str(destination),
                ],
                env={**os.environ, "GC_EXECUTION_MARKER": str(marker)},
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(marker.exists(), "candidate AppImage runtime was executed")
            self.assertEqual(
                (destination / "squashfs-root/trusted.txt").read_text(encoding="ascii"),
                "trusted payload\n",
            )

    def test_trusted_extraction_rejects_same_size_in_place_mutation(self):
        real_unsquashfs = shutil.which("unsquashfs")
        if real_unsquashfs is None:
            self.skipTest("unsquashfs is required")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            image, _ = self.build_type2_fixture(root)
            destination = root / "extract"
            destination.mkdir()
            fake_bin = root / "bin"
            fake_bin.mkdir()
            mutating_unsquashfs = fake_bin / "unsquashfs"
            mutating_unsquashfs.write_text(
                textwrap.dedent(
                    """#!/bin/sh
                    set -eu
                    "$GC_REAL_UNSQUASHFS" "$@"
                    image=
                    for argument in "$@"; do image=$argument; done
                    python3 -c 'import os, sys
path = sys.argv[1]
with open(path, "r+b") as stream:
    stream.seek(32)
    value = stream.read(1)
    stream.seek(32)
    stream.write(bytes((value[0] ^ 1,)))
    stream.flush()
    os.fsync(stream.fileno())' "$image"
                    """
                ),
                encoding="ascii",
            )
            mutating_unsquashfs.chmod(0o700)
            environment = {
                **os.environ,
                "GC_REAL_UNSQUASHFS": real_unsquashfs,
                "PATH": f"{fake_bin}:{os.environ['PATH']}",
            }
            result = run(
                [
                    "bash",
                    "-c",
                    'set -euo pipefail; . "$1"; '
                    'trusted_appimage_extract "$2" "$3"',
                    "bash",
                    str(SUPPORT),
                    str(image),
                    str(destination),
                ],
                env=environment,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse((destination / "squashfs-root").exists())

    def test_candidate_verifiers_never_use_appimage_extract(self):
        support = SUPPORT.read_text(encoding="utf-8")
        self.assertNotIn('"$image_path" --appimage-extract', support)
        self.assertNotIn('"$image" --appimage-extract', support)
        for function in (
            "verify_appimage_manifest",
            "verify_appimage_sbom",
            "strava_oauth_appimage_status",
            "linux_keychain_appimage_status",
        ):
            self.assertIn("trusted_appimage_extract", support[support.index(function) :])


class PayloadSbomTests(unittest.TestCase):
    def write_sbom(self, appdir, components):
        sbom = appdir / "usr/share/goldencheetah/goldencheetah.cdx.json"
        sbom.parent.mkdir(parents=True, exist_ok=True)
        document = {
            "bomFormat": "CycloneDX",
            "specVersion": "1.5",
            "serialNumber": "urn:uuid:00000000-0000-4000-8000-000000000000",
            "version": 1,
            "metadata": {},
            "components": components,
        }
        sbom.write_text(json.dumps(document) + "\n", encoding="utf-8")
        return sbom

    def verify(self, appdir, sbom):
        return run(["python3", str(PAYLOAD_VERIFIER), str(appdir), str(sbom)])

    def test_payload_sbom_requires_exact_coverage_and_content(self):
        self.assertTrue(PAYLOAD_VERIFIER.is_file(), "payload verifier is missing")
        with tempfile.TemporaryDirectory() as temporary:
            appdir = Path(temporary) / "AppDir"
            (appdir / "bin").mkdir(parents=True)
            executable = appdir / "bin/gc"
            executable.write_text("release payload\n", encoding="ascii")
            executable.chmod(0o755)
            os.symlink("bin/gc", appdir / "gc")
            components = [
                file_component(appdir, "bin/gc"),
                symlink_component(appdir, "gc"),
            ]
            sbom = self.write_sbom(appdir, components)
            self.assertEqual(self.verify(appdir, sbom).returncode, 0)

            sbom = self.write_sbom(appdir, components[1:])
            self.assertNotEqual(self.verify(appdir, sbom).returncode, 0)

            sbom = self.write_sbom(appdir, components)
            executable.write_text("modified payload\n", encoding="ascii")
            self.assertNotEqual(self.verify(appdir, sbom).returncode, 0)

            executable.write_text("release payload\n", encoding="ascii")
            (appdir / "gc").unlink()
            os.symlink("missing", appdir / "gc")
            self.assertNotEqual(self.verify(appdir, sbom).returncode, 0)

    def test_payload_mode_mismatch_identifies_file_and_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            appdir = Path(temporary) / "AppDir"
            executable = appdir / "bin/gc"
            executable.parent.mkdir(parents=True)
            executable.write_text("release payload\n", encoding="ascii")
            executable.chmod(0o755)
            sbom = self.write_sbom(appdir, [file_component(appdir, "bin/gc")])

            executable.chmod(0o700)
            result = self.verify(appdir, sbom)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "payload file mode mismatch for bin/gc: expected 0755, got 0700",
                result.stderr,
            )


class BuildInputTests(unittest.TestCase):
    def identity(self, source_root):
        return run(["python3", str(BUILD_INPUT_IDENTITY), str(source_root)])

    def test_identity_binds_ignored_inputs_and_rejects_local_sources(self):
        self.assertTrue(BUILD_INPUT_IDENTITY.is_file(), "identity tool is missing")
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary)
            (source_root / "src/Core").mkdir(parents=True)
            (source_root / "qwt").mkdir()
            (source_root / "qwt/qwtconfig.pri").write_text(
                "QWT_CONFIG += QwtPlot\n", encoding="ascii"
            )
            config = source_root / "src/gcconfig.pri"
            config.write_text("CONFIG += release\n", encoding="ascii")
            secrets = source_root / "src/Core/GeneratedSecrets.h"
            secrets.write_text("#define GC_TEST_SECRET \"one\"\n", encoding="ascii")

            first = self.identity(source_root)
            self.assertEqual(first.returncode, 0, first.stderr)
            self.assertRegex(first.stdout.strip(), r"^[0-9a-f]{64}$")
            config.write_text("CONFIG += release static\n", encoding="ascii")
            second = self.identity(source_root)
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertNotEqual(first.stdout, second.stdout)
            secrets.write_text("#define GC_TEST_SECRET \"two\"\n", encoding="ascii")
            third = self.identity(source_root)
            self.assertEqual(third.returncode, 0, third.stderr)
            self.assertNotEqual(second.stdout, third.stdout)

            (source_root / "qwt/qwtconfig.pri").write_text(
                "QWT_CONFIG += QwtPlot QwtSvg\n", encoding="ascii"
            )
            fourth = self.identity(source_root)
            self.assertEqual(fourth.returncode, 0, fourth.stderr)
            self.assertNotEqual(third.stdout, fourth.stdout)

            config.write_text(
                "CONFIG += release\nLOCALHEADERS += /tmp/injected.h\n",
                encoding="ascii",
            )
            rejected = self.identity(source_root)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("LOCALHEADERS", rejected.stderr)

    def test_identity_rejects_non_release_configuration(self):
        with tempfile.TemporaryDirectory() as temporary:
            source_root = Path(temporary)
            (source_root / "src/Core").mkdir(parents=True)
            (source_root / "qwt").mkdir()
            (source_root / "qwt/qwtconfig.pri").write_text(
                "QWT_CONFIG += QwtPlot\n", encoding="ascii"
            )
            config = source_root / "src/gcconfig.pri"

            for contents in (
                "CONFIG += debug\n",
                "CONFIG += release debug\n",
                "DEFINES += GC_VIDEO_NONE\n",
            ):
                with self.subTest(contents=contents):
                    config.write_text(contents, encoding="ascii")
                    rejected = self.identity(source_root)
                    self.assertNotEqual(rejected.returncode, 0)
                    self.assertIn("release configuration", rejected.stderr)

            config.write_text(
                "CONFIG += debug\nCONFIG -= debug\nCONFIG += release static\n",
                encoding="ascii",
            )
            accepted = self.identity(source_root)
            self.assertEqual(accepted.returncode, 0, accepted.stderr)

    def test_compiled_report_and_manifest_bind_build_inputs(self):
        project = (REPOSITORY_ROOT / "src/src.pro").read_text(encoding="utf-8")
        main = (REPOSITORY_ROOT / "src/Core/main.cpp").read_text(encoding="utf-8")
        support = SUPPORT.read_text(encoding="utf-8")
        self.assertIn("compute-build-input-identity.py", project)
        self.assertIn("GC_BUILD_INPUTS_SHA256", project)
        self.assertIn("build_inputs_sha256=", main)
        self.assertIn("build_inputs_sha256=", support)

    def test_input_installer_rejects_source_and_destination_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            input_source = root / "input"
            source_tree = root / "source"
            for tree in (input_source, source_tree):
                (tree / "src/Core").mkdir(parents=True)
                (tree / "qwt").mkdir()
            config = input_source / "src/gcconfig.pri"
            config.write_text("CONFIG += release\n", encoding="ascii")
            (source_tree / "qwt/qwtconfig.pri.in").write_text(
                "QWT_CONFIG += QwtPlot\n", encoding="ascii"
            )
            outside = root / "outside"
            outside.write_text("unchanged\n", encoding="ascii")

            def install_inputs():
                return run(
                    [
                        "bash", "-c",
                        'set -euo pipefail; . "$1"; '
                        'install_reproducible_build_inputs "$2" "$3"',
                        "bash", str(SUPPORT), str(input_source), str(source_tree),
                    ]
                )

            os.symlink(outside, source_tree / "src/gcconfig.pri")
            destination_link = install_inputs()
            self.assertNotEqual(destination_link.returncode, 0)
            self.assertEqual(outside.read_text(encoding="ascii"), "unchanged\n")

            (source_tree / "src/gcconfig.pri").unlink()
            config.unlink()
            os.symlink(outside, config)
            source_link = install_inputs()
            self.assertNotEqual(source_link.returncode, 0)
            self.assertEqual(outside.read_text(encoding="ascii"), "unchanged\n")


class PipelineIsolationTests(unittest.TestCase):
    def test_appveyor_inputs_are_external_and_extracted_trees_are_not_cached(self):
        self.assertTrue(APPVEYOR_INPUTS.is_file(), "AppVeyor input helper is missing")
        configuration = (REPOSITORY_ROOT / "appveyor.yml").read_text(encoding="utf-8")
        cache_block = configuration.split("cache:", 1)[1].split("install:", 1)[0]
        for name in ("D2XX", "srmio", "python-source"):
            self.assertNotIn(name, cache_block)

        with tempfile.TemporaryDirectory() as temporary:
            input_root = Path(temporary) / "inputs"
            result = run(
                [
                    "bash",
                    "-c",
                    'set -euo pipefail; REPOSITORY_ROOT="$1"; '
                    'GC_APPVEYOR_INPUT_ROOT="$2"; . "$3"; '
                    "prepare_appveyor_build_inputs; "
                    "printf '%s\\n' \"$GC_D2XX_ROOT\" \"$GC_SRMIO_ROOT\" "
                    '"$GC_PYTHON_SOURCE_ROOT"',
                    "bash",
                    str(REPOSITORY_ROOT),
                    str(input_root),
                    str(APPVEYOR_INPUTS),
                ]
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            repository = REPOSITORY_ROOT.resolve()
            for raw_path in result.stdout.splitlines():
                path = Path(raw_path).resolve()
                self.assertTrue(path.is_relative_to(input_root.resolve()))
                self.assertFalse(path.is_relative_to(repository))

    def test_packaging_environment_is_allowlisted(self):
        with tempfile.TemporaryDirectory() as temporary:
            probe = Path(temporary) / "probe"
            probe.write_text(
                textwrap.dedent(
                    """#!/bin/sh
                    set -eu
                    test "$ARCH" = x86_64
                    test "$LC_ALL" = C
                    test "$TZ" = UTC
                    test "$SOURCE_DATE_EPOCH" = 1234567890
                    test -d "$HOME"
                    case "$PATH" in *poison*) exit 72;; esac
                    test -z "${VERSION+x}"
                    test -z "${UPDATE_INFORMATION+x}"
                    test -z "${APPIMAGETOOL_SIGN_PASSPHRASE+x}"
                    test -z "${QT_PLUGIN_PATH+x}"
                    test -z "${QT_QPA_PLATFORM_PLUGIN_PATH+x}"
                    test -z "${PYTHONHOME+x}"
                    test -z "${PYTHONPATH+x}"
                    """
                ),
                encoding="ascii",
            )
            probe.chmod(0o700)
            environment = {
                **os.environ,
                "SOURCE_DATE_EPOCH": "1234567890",
                "VERSION": "poison",
                "UPDATE_INFORMATION": "poison",
                "APPIMAGETOOL_SIGN_PASSPHRASE": "poison",
                "QT_PLUGIN_PATH": "/poison",
                "QT_QPA_PLATFORM_PLUGIN_PATH": "/poison",
                "PYTHONHOME": "/poison",
                "PYTHONPATH": "/poison",
                "HOME": "/poison",
                "PATH": "/poison:/usr/local/bin:/usr/bin:/bin",
            }
            result = run(
                [
                    "/bin/bash",
                    "-c",
                    'set -euo pipefail; . "$1"; run_packaging_appimage "$2"',
                    "bash",
                    str(SUPPORT),
                    str(probe),
                ],
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_package_python_version_is_pinned_and_conflicts_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            binary = root / "GoldenCheetah"
            binary.write_text("#!/bin/sh\nexit 1\n", encoding="ascii")
            binary.chmod(0o700)

            def package(version):
                output = root / ("output-" + (version or "unset"))
                output.mkdir()
                environment = {
                    **os.environ,
                    "GC_APPIMAGE_BINARY": str(binary),
                    "GC_APPIMAGE_OAUTH_POLICY": "unconfigured",
                    "GC_APPIMAGE_REPOSITORY_ROOT": str(REPOSITORY_ROOT),
                }
                if version is None:
                    environment.pop("PYTHON_VERSION", None)
                else:
                    environment["PYTHON_VERSION"] = version
                return run([str(PACKAGE_APPIMAGE), str(output)], env=environment)

            defaulted = package(None)
            self.assertNotEqual(defaulted.returncode, 0)
            self.assertNotIn("Python unset does not match", defaulted.stderr)
            self.assertIn(
                "Strava OAuth build status requires an ELF executable",
                defaulted.stderr,
            )

            conflict = package("3.12")
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn("Python 3.12 does not match", conflict.stderr)

    def test_linuxdeployqt_transform_capture_surrounds_deployment(self):
        package = PACKAGE_APPIMAGE.read_text(encoding="utf-8")
        self.assertIn(
            "Resources/linux/capture-linuxdeployqt-transforms.py",
            package,
        )
        snapshot = package.index('"$LINUXDEPLOYQT_CAPTURE" snapshot')
        deployment = package.index("run_linuxdeployqt_with_keychain_probe")
        finalize = package.index('"$LINUXDEPLOYQT_CAPTURE" finalize')
        keychain = package.index("install_linux_keychain_runtime")
        sbom = package.index("create_appimage_sbom")
        self.assertLess(snapshot, deployment)
        self.assertLess(deployment, finalize)
        self.assertLess(finalize, keychain)
        self.assertLess(keychain, sbom)
        self.assertIn("-no-strip", package[deployment:finalize])

    def test_compiler_environment_is_allowlisted(self):
        build_pass = REPOSITORY_ROOT / "appveyor/linux/build-appimage-pass.sh"
        with tempfile.TemporaryDirectory() as temporary:
            temporary_root = Path(temporary)
            physical_root = temporary_root / "physical"
            physical_root.mkdir()
            root = temporary_root / "alias"
            root.symlink_to(physical_root, target_is_directory=True)
            qt_root = root / "Qt"
            home = root / "home"
            temporary_build = root / "tmp"
            (qt_root / "bin").mkdir(parents=True)
            home.mkdir()
            temporary_build.mkdir()
            probe = root / "probe"
            probe.write_text(
                textwrap.dedent(
                    """#!/bin/sh
                    set -eu
                    test "$QTDIR" = "$1"
                    test "$HOME" = "$2"
                    test "$TMPDIR" = "$3"
                    test "$LC_ALL" = C
                    test "$TZ" = UTC
                    test "$SOURCE_DATE_EPOCH" = 1234567890
                    test "$GC_SOURCE_REVISION" = 1111111111111111111111111111111111111111
                    case "$PATH" in *poison*) exit 72;; esac
                    for variable in CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS \
                        ARFLAGS MAKEFLAGS QMAKEFLAGS QMAKEFEATURES QMAKEPATH \
                        PKG_CONFIG_PATH PYTHONHOME PYTHONPATH CCACHE_DIR \
                        CCACHE_COMPILERCHECK CCACHE_IGNOREOPTIONS CCACHE_MAXSIZE \
                        CCACHE_NOHASHDIR CCACHE_SLOPPINESS; do
                        eval 'test -z "${'"$variable"'+x}"'
                    done
                    """
                ),
                encoding="ascii",
            )
            probe.chmod(0o700)
            environment = {
                **os.environ,
                "SOURCE_DATE_EPOCH": "1234567890",
                "GC_SOURCE_REVISION": "1" * 40,
                "PATH": f"/poison:{os.environ['PATH']}",
                "CC": "poison",
                "CXX": "poison",
                "CFLAGS": "poison",
                "CXXFLAGS": "poison",
                "CPPFLAGS": "poison",
                "LDFLAGS": "poison",
                "ARFLAGS": "poison",
                "MAKEFLAGS": "poison",
                "QMAKEFLAGS": "poison",
                "QMAKEFEATURES": "poison",
                "QMAKEPATH": "poison",
                "PKG_CONFIG_PATH": "poison",
                "PYTHONHOME": "poison",
                "PYTHONPATH": "poison",
                "CCACHE_DIR": "poison",
                "CCACHE_COMPILERCHECK": "poison",
                "CCACHE_IGNOREOPTIONS": "poison",
                "CCACHE_MAXSIZE": "poison",
                "CCACHE_NOHASHDIR": "poison",
                "CCACHE_SLOPPINESS": "poison",
            }
            result = run(
                [
                    "bash",
                    "-c",
                    'set -euo pipefail; . "$1"; '
                    'run_reproducible_build_tool "$2" "$3" "$4" '
                    '"$5" "$2" "$3" "$4"',
                    "bash",
                    str(SUPPORT),
                    str(qt_root.resolve()),
                    str(home.resolve()),
                    str(temporary_build.resolve()),
                    str(probe),
                ],
                env=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("run_reproducible_build_tool", build_pass.read_text(
                encoding="utf-8"
            ))

    def test_reproducible_release_bypasses_shared_compiler_caches(self):
        for relative in (
            "appveyor/linux/build-appimage-pass.sh",
            "appveyor/linux/reproduce-appimage.sh",
            "src/Resources/linux/AppImagePackagingSupport.sh",
        ):
            source = (REPOSITORY_ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn("GC_APPIMAGE_CCACHE", source, relative)
            self.assertNotIn("QMAKE_CC=$CCACHE", source, relative)

    def test_reproduction_builds_and_packages_two_independent_trees(self):
        self.assertTrue(REPRODUCE_APPIMAGE.is_file(), "reproduction driver is missing")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            subprocess.run(["git", "init", "-q", source], check=True)
            subprocess.run(["git", "-C", source, "config", "user.name", "Test"], check=True)
            subprocess.run(
                ["git", "-C", source, "config", "user.email", "test@example.invalid"],
                check=True,
            )
            (source / "src/Core").mkdir(parents=True)
            (source / "qwt").mkdir()
            (source / ".gitignore").write_text(
                "/src/gcconfig.pri\n"
                "/src/Core/GeneratedSecrets.h\n"
                "/qwt/qwtconfig.pri\n",
                encoding="ascii",
            )
            (source / "tracked").write_text("source\n", encoding="ascii")
            (source / "src/Core/fixture").write_text("core\n", encoding="ascii")
            (source / "qwt/qwtconfig.pri.in").write_text(
                "QWT_CONFIG += QwtPlot\n", encoding="ascii"
            )
            subprocess.run(
                [
                    "git", "-C", source, "add", "tracked", ".gitignore",
                    "src/Core/fixture", "qwt/qwtconfig.pri.in",
                ],
                check=True,
            )
            subprocess.run(["git", "-C", source, "commit", "-qm", "fixture"], check=True)
            config = source / "src/gcconfig.pri"
            secrets = source / "src/Core/GeneratedSecrets.h"
            qwt_config = source / "qwt/qwtconfig.pri"
            config.write_text("CONFIG += release\n", encoding="ascii")
            secrets.write_text(
                '#define GC_TEST_SECRET "fixture"\n', encoding="ascii"
            )
            qwt_config.write_text(
                "QWT_CONFIG += QwtPlot QwtSvg\n", encoding="ascii"
            )
            build_log = root / "build.log"
            package_log = root / "package.log"
            build_probe = root / "build-pass"
            package_probe = root / "package-pass"
            build_probe.write_text(
                "#!/bin/sh\nset -eu\n"
                "test ! -e \"$1/.reproduction-pass\"\n"
                "printf build >\"$1/.reproduction-pass\"\n"
                "mkdir -p \"$1/src/Core\" \"$1/qwt\"\n"
                "cp \"$3/src/gcconfig.pri\" \"$1/src/gcconfig.pri\"\n"
                "cp \"$3/src/Core/GeneratedSecrets.h\" "
                "\"$1/src/Core/GeneratedSecrets.h\"\n"
                "cp \"$3/qwt/qwtconfig.pri\" \"$1/qwt/qwtconfig.pri\"\n"
                "printf '%s|%s\\n' \"$1\" \"$2\" >>\"$GC_BUILD_LOG\"\n"
                "mkdir -p \"$2/src\"\n"
                "printf 'same independently built elf\\n' >\"$2/src/GoldenCheetah\"\n"
                "chmod 700 \"$2/src/GoldenCheetah\"\n",
                encoding="ascii",
            )
            package_probe.write_text(
                "#!/bin/sh\nset -eu\n"
                "test ! -e \"$GC_APPIMAGE_REPOSITORY_ROOT/.reproduction-pass\"\n"
                "printf package >"
                "\"$GC_APPIMAGE_REPOSITORY_ROOT/.reproduction-pass\"\n"
                "cmp \"$GC_EFFECTIVE_CONFIG\" "
                "\"$GC_APPIMAGE_REPOSITORY_ROOT/src/gcconfig.pri\"\n"
                "cmp \"$GC_EFFECTIVE_SECRETS\" "
                "\"$GC_APPIMAGE_REPOSITORY_ROOT/src/Core/GeneratedSecrets.h\"\n"
                "cmp \"$GC_EFFECTIVE_QWT_CONFIG\" "
                "\"$GC_APPIMAGE_REPOSITORY_ROOT/qwt/qwtconfig.pri\"\n"
                "printf '%s|%s\\n' \"$GC_APPIMAGE_REPOSITORY_ROOT\" "
                "\"$GC_APPIMAGE_BINARY\" >>\"$GC_PACKAGE_LOG\"\n"
                "test -x \"$GC_APPIMAGE_BINARY\"\n"
                "mkdir -p \"$1\"\n"
                "printf appimage >\"$1/GoldenCheetah.AppImage\"\n"
                "printf build >\"$1/build.manifest\"\n"
                "hash=$(sha256sum \"$1/GoldenCheetah.AppImage\" | cut -d' ' -f1)\n"
                "printf 'appimage_sha256=%s\\n' \"$hash\" "
                ">\"$1/GoldenCheetah.AppImage.manifest\"\n"
                "printf sbom >\"$1/GoldenCheetah.AppImage.sbom.cdx.json\"\n",
                encoding="ascii",
            )
            build_probe.chmod(0o700)
            package_probe.chmod(0o700)
            output = root / "output"
            result = run(
                [str(REPRODUCE_APPIMAGE), str(source), str(output)],
                env={
                    **os.environ,
                    "GC_APPIMAGE_BUILD_PASS_SCRIPT": str(build_probe),
                    "GC_APPIMAGE_PACKAGE_PASS_SCRIPT": str(package_probe),
                    "GC_BUILD_LOG": str(build_log),
                    "GC_PACKAGE_LOG": str(package_log),
                    "GC_EFFECTIVE_CONFIG": str(config),
                    "GC_EFFECTIVE_SECRETS": str(secrets),
                    "GC_EFFECTIVE_QWT_CONFIG": str(qwt_config),
                },
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            builds = build_log.read_text(encoding="utf-8").splitlines()
            packages = package_log.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(builds), 2)
            self.assertEqual(len(packages), 2)
            self.assertEqual(
                len({line.split("|", 1)[0] for line in builds}),
                1,
                "independent checkouts must reuse one canonical source path",
            )
            self.assertEqual(len({line.split("|", 1)[1] for line in builds}), 2)
            for build, package in zip(builds, packages):
                source_tree, build_tree = build.split("|", 1)
                package_source, package_binary = package.split("|", 1)
                self.assertEqual(package_source, source_tree)
                self.assertEqual(package_binary, f"{build_tree}/src/GoldenCheetah")

    def test_all_linux_release_entrypoints_use_the_reproduction_driver(self):
        for relative in (
            "appveyor/linux/after_build.sh",
            ".devcontainer/package-appimage.sh",
            "src/Resources/linux/MakeAppImageQt6.sh",
        ):
            source = (REPOSITORY_ROOT / relative).read_text(encoding="utf-8")
            self.assertIn("reproduce-appimage.sh", source, relative)
            self.assertNotIn("APPIMAGETOOL_FILE", source, relative)
        build_pass = (
            REPOSITORY_ROOT / "appveyor/linux/build-appimage-pass.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("-ffile-prefix-map=", build_pass)
        self.assertIn("-fdebug-prefix-map=", build_pass)

    def test_mismatched_independent_elf_stops_before_packaging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source"
            source.mkdir()
            subprocess.run(["git", "init", "-q", source], check=True)
            subprocess.run(["git", "-C", source, "config", "user.name", "Test"], check=True)
            subprocess.run(
                ["git", "-C", source, "config", "user.email", "test@example.invalid"],
                check=True,
            )
            (source / "src/Core").mkdir(parents=True)
            (source / "qwt").mkdir()
            (source / ".gitignore").write_text(
                "/src/gcconfig.pri\n/qwt/qwtconfig.pri\n", encoding="ascii"
            )
            (source / "tracked").write_text("source\n", encoding="ascii")
            (source / "src/Core/fixture").write_text("core\n", encoding="ascii")
            (source / "qwt/qwtconfig.pri.in").write_text(
                "QWT_CONFIG += QwtPlot\n", encoding="ascii"
            )
            subprocess.run(
                [
                    "git", "-C", source, "add", "tracked", ".gitignore",
                    "src/Core/fixture", "qwt/qwtconfig.pri.in",
                ],
                check=True,
            )
            subprocess.run(["git", "-C", source, "commit", "-qm", "fixture"], check=True)
            (source / "src/gcconfig.pri").write_text(
                "CONFIG += release\n", encoding="ascii"
            )

            build_probe = root / "build-pass"
            package_probe = root / "package-pass"
            package_log = root / "package.log"
            build_probe.write_text(
                "#!/bin/sh\nset -eu\nmkdir -p \"$2/src\"\n"
                "printf '%s\\n' \"$(basename \"$2\")\" >\"$2/src/GoldenCheetah\"\n"
                "chmod 700 \"$2/src/GoldenCheetah\"\n",
                encoding="ascii",
            )
            package_probe.write_text(
                "#!/bin/sh\nset -eu\nprintf 'packaged\\n' >>\"$GC_PACKAGE_LOG\"\n",
                encoding="ascii",
            )
            build_probe.chmod(0o700)
            package_probe.chmod(0o700)
            result = run(
                [str(REPRODUCE_APPIMAGE), str(source), str(root / "output")],
                env={
                    **os.environ,
                    "GC_APPIMAGE_BUILD_PASS_SCRIPT": str(build_probe),
                    "GC_APPIMAGE_PACKAGE_PASS_SCRIPT": str(package_probe),
                    "GC_PACKAGE_LOG": str(package_log),
                },
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("independent GoldenCheetah ELF mismatch", result.stderr)
            self.assertFalse(
                package_log.exists(),
                "packaging ran before independently built ELF files matched",
            )


class PlatformGateTests(unittest.TestCase):
    def test_devcontainer_uses_a_persistent_compiler_cache(self):
        dockerfile = (REPOSITORY_ROOT / ".devcontainer/Dockerfile").read_text(
            encoding="utf-8"
        )
        build_script = (REPOSITORY_ROOT / ".devcontainer/build.sh").read_text(
            encoding="utf-8"
        )
        configuration = json.loads(
            (REPOSITORY_ROOT / ".devcontainer/devcontainer.json").read_text(
                encoding="utf-8"
            )
        )

        self.assertIn("/usr/lib/ccache", dockerfile)
        self.assertIn("ccache --show-stats", build_script)
        self.assertIn("CCACHE_BASEDIR", build_script)
        self.assertIn(
            'ccache --set-config="sloppiness=pch_defines,time_macros"',
            build_script,
        )
        self.assertIn(
            'qmake_arguments+=("QMAKE_CXXFLAGS+=-fpch-preprocess")',
            build_script,
        )
        self.assertIn('mkdir -p "${build_dir}"', build_script)
        pch_cleanup = build_script.index(
            "find -P \"${build_dir}\" -type d -name 'GoldenCheetah.gch'"
        )
        self.assertLess(
            build_script.index('mkdir -p "${build_dir}"'),
            pch_cleanup,
        )
        self.assertLess(
            pch_cleanup,
            build_script.index('qmake "${qmake_arguments[@]}"'),
        )
        self.assertIn(
            "source=goldencheetah-ccache,"
            "target=/home/ubuntu/.cache/ccache,type=volume",
            configuration["mounts"],
        )
        self.assertEqual(
            configuration["containerEnv"]["CCACHE_DIR"],
            "/home/ubuntu/.cache/ccache",
        )
        self.assertIn("chown -R", configuration["postCreateCommand"])

    def test_gui_smoke_marks_initialized_runtime(self):
        main = (REPOSITORY_ROOT / "src/Core/main.cpp").read_text(encoding="utf-8")
        marker = main.index("goldencheetah_gui_smoke=main-window-ready")
        initialization = main.index("LocalFileStoreProcess::initializeReaper")
        athlete = main.index("new MainWindow")
        completion = main.index(
            "scheduleGuiSmokeCompletion(mainWindow)", athlete
        )
        disable_auto_quit = main.index("setQuitOnLastWindowClosed(false)")
        coordinated_close = main.index("GuiSmokeShutdown::complete")
        close_window = main.index("mainWindow->close()", coordinated_close)
        event_loop = main.rindex("application->exec()")
        shutdown = main.index("LocalFileStoreProcess::shutdownReaper", completion)
        self.assertLess(initialization, completion)
        self.assertLess(athlete, completion)
        self.assertLess(disable_auto_quit, athlete)
        self.assertLess(coordinated_close, close_window)
        self.assertLess(close_window, event_loop)
        self.assertLess(completion, shutdown)
        self.assertLess(completion, event_loop)

        support = SUPPORT.read_text(encoding="utf-8")
        self.assertIn("smoke_athlete/config/athlete-general.ini", support)
        self.assertIn('"$smoke_library" SmokeAthlete', support)

        package = PACKAGE_APPIMAGE.read_text(encoding="utf-8")
        self.assertIn(
            'require_qt_offscreen_appimage_on_glibc 2.35 "$IMAGE" 30s',
            package,
        )

    def test_apt_integration_builds_the_real_bootstrap_stage(self):
        dockerfile = (REPOSITORY_ROOT / ".devcontainer/Dockerfile").read_text(
            encoding="utf-8"
        )
        test_source = (TEST_DIR / "testAptSnapshot.py").read_text(encoding="utf-8")
        self.assertIn("AS apt-snapshot-bootstrap", dockerfile)
        self.assertIn('"build"', test_source)
        self.assertIn('"--target"', test_source)
        self.assertIn('"apt-snapshot-bootstrap"', test_source)
        integration = test_source.split(
            "def test_real_apt_update_indices_match_signed_metadata", 1
        )[1]
        self.assertNotIn("apt-get install --no-install-recommends -y -qq ca-certificates python3", integration)

    def test_release_hosts_refresh_transformed_runtime_packages(self):
        dockerfile = (REPOSITORY_ROOT / ".devcontainer/Dockerfile").read_text(
            encoding="utf-8"
        )
        linux_install = (
            REPOSITORY_ROOT / "appveyor/linux/install.sh"
        ).read_text(encoding="utf-8")
        self.assertRegex(dockerfile, r"(?m)^\s+libcap2 \\$")
        self.assertRegex(dockerfile, r"(?m)^\s+libgnutls30t64 \\$")
        self.assertIn(
            '"$APT_GET" install -qq libcap2 libgnutls30',
            linux_install,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
