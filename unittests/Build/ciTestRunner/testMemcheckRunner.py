#!/usr/bin/env python3
"""Contract tests using synthetic reports, without requiring Valgrind or Qt."""

import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


RUNNER = Path(__file__).resolve().parents[3] / ".github/scripts/run-memcheck.py"
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("memcheck_runner", RUNNER)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
FAKE_VALGRIND = r'''
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

mode = os.environ.get("GC_FAKE_MEMCHECK", "success")
arguments = sys.argv[1:]
xml_path = Path(next(a.split("=", 1)[1] for a in arguments if a.startswith("--xml-file=")).replace("%p", str(os.getpid())))
output = xml_path.parent
report = Path(arguments[arguments.index("-o") + 1].removesuffix(",xml"))
Path(next(a.split("=", 1)[1] for a in arguments if a.startswith("--log-file=")).replace("%p", str(os.getpid()))).write_text("Memcheck text log\n")
print("application output", flush=True)
(output / "environment.json").write_text(json.dumps({key: os.environ.get(key) for key in (
    "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME",
    "XDG_RUNTIME_DIR", "TMPDIR", "QT_QPA_PLATFORM", "VALGRIND_LIB")}))
(output / "private-fixture.txt").write_text("synthetic test fixture")
(output / "regexp-jit.json").write_text(json.dumps(os.environ.get("QT_ENABLE_REGEXP_JIT")))
(output / "arguments.json").write_text(json.dumps(arguments))
if mode in {"timeout", "interrupt", "orphan"}:
    child = subprocess.Popen([sys.executable, "-c", "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"])
    (output / "child.pid").write_text(str(child.pid))
    if mode != "orphan":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        time.sleep(30)
if mode == "missing":
    sys.exit(0)
pid = os.getpid() + (1 if mode == "wrong-pid" else 0)
tool = "other" if mode == "wrong-tool" else "memcheck"
state = "RUNNING" if mode == "incomplete" else "FINISHED"
error = ""
if mode.startswith("error:"):
    error = "<error><kind>" + mode.split(":", 1)[1] + "</kind></error>"
if mode == "signal-report":
    error = "<fatal_signal><signo>11</signo></fatal_signal>"
xml_path.write_text(f"<valgrindoutput><protocoltool>{tool}</protocoltool><pid>{pid}</pid><status><state>{state}</state></status>{error}<suppcounts><pair><count>2</count><name>known-default</name></pair></suppcounts></valgrindoutput>")
if mode == "malformed":
    xml_path.write_text("<valgrindoutput>")
if mode == "oversized":
    with xml_path.open("r+b") as stream:
        stream.truncate(256 * 1024 * 1024 + 1)
test = '<TestFunction name="actualTest"><Incident type="pass"/></TestFunction>'
if mode == "zero":
    test = ""
if mode == "skip":
    test = '<TestFunction name="actualTest"><Incident type="skip"/></TestFunction>'
if mode == "qt-fail":
    test = '<TestFunction name="actualTest"><Incident type="fail"/></TestFunction>'
if mode == "qt-unknown":
    test = '<TestFunction name="actualTest"><Incident type="unknown"/></TestFunction>'
if mode == "qt-empty-function":
    test = '<TestFunction name="actualTest"/>'
init = '<TestFunction name="initTestCase"><Incident type="pass"/></TestFunction>'
cleanup = '<TestFunction name="cleanupTestCase"><Incident type="pass"/></TestFunction>'
if mode == "qt-no-cleanup":
    cleanup = ""
if mode == "qt-missing-init":
    init = ""
if mode == "qt-cleanup-skip":
    cleanup = cleanup.replace('type="pass"', 'type="skip"')
if mode == "qt-init-skip":
    init = init.replace('type="pass"', 'type="skip"')
body = init + test + cleanup
if mode == "qt-cleanup-not-last":
    body = init + cleanup + test
report.write_text('<TestCase name="FakeTest">' + body + '</TestCase>')
if mode == "qt-malformed":
    report.write_text("<TestCase>")
if mode == "crash":
    os.kill(os.getpid(), signal.SIGKILL)
sys.exit(97 if mode == "nonzero" else 0)
'''


class MemcheckEvidenceTests(unittest.TestCase):
    HEADER = b'<valgrindoutput><protocoltool>memcheck</protocoltool><pid>123</pid>'
    FINISHED = b'<status><state>FINISHED</state></status>'
    FOOTER = b'</valgrindoutput>'

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gc-memcheck-evidence-")
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "report.xml"

    def test_aggregate_matches_report_records_and_ignores_nested_lookalikes(self):
        body = (b'<status><state>RUNNING</state></status>'
                b'<error><kind>InvalidRead</kind><stack><frame><fn>example</fn></frame></stack></error>'
                b'<error><kind>Leak_StillReachable</kind></error>'
                b'<ignored><error><kind>InvalidWrite</kind></error><status><state>OTHER</state></status></ignored>'
                + self.FINISHED
                + b'<error><kind>InvalidRead</kind></error><error><kind>FutureKind</kind></error>'
                b'<suppcounts><pair><count>2</count><name>first</name></pair>'
                b'<pair><count>0</count><name>second</name></pair></suppcounts>')
        self.path.write_bytes(self.HEADER + body + self.FOOTER)
        self.assertEqual(MODULE.memcheck_evidence(self.path, 123), {
            "error_records": {"InvalidRead": 2, "Leak_StillReachable": 1, "FutureKind": 1},
            "blocking_error_records": 3,
            "suppressed": [{"name": "first", "count": 2}, {"name": "second", "count": 0}],
        })

    def test_valid_memcheck_report_larger_than_64_mib(self):
        record = b'<ignored>' + b'x' * 4096 + b'</ignored>'
        with self.path.open("wb") as stream:
            stream.write(self.HEADER)
            for _ in range(17 * 1024):
                stream.write(record)
            stream.write(b'<error><kind>Leak_StillReachable</kind></error>')
            stream.write(self.FINISHED + self.FOOTER)
        self.assertGreater(self.path.stat().st_size, 64 * 1024 * 1024)
        evidence = MODULE.memcheck_evidence(self.path, 123)
        self.assertEqual(evidence["error_records"], {"Leak_StillReachable": 1})
        self.assertEqual(evidence["blocking_error_records"], 0)

    def test_256_mib_limit_rejects_sparse_oversized_report(self):
        with self.path.open("wb") as stream:
            stream.truncate(256 * 1024 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, "oversized"):
            MODULE.memcheck_evidence(self.path, 123)

    def test_consumed_bytes_limit_catches_growth_after_stat(self):
        self.path.write_bytes(self.HEADER + b'<ignored>' + b'x' * 1024
                              + b'</ignored>' + self.FINISHED + self.FOOTER)
        stale_stat = mock.Mock(st_size=1, st_mode=self.path.stat().st_mode)
        with mock.patch.object(MODULE, "MAX_MEMCHECK_REPORT_BYTES", 256), \
                mock.patch.object(Path, "stat", return_value=stale_stat):
            with self.assertRaisesRegex(ValueError, "oversized XML report while reading"):
                MODULE.memcheck_evidence(self.path, 123)

    def test_finished_status_does_not_skip_trailing_xml_validation(self):
        for suffix in (b'<unclosed>', self.FOOTER + b'not XML'):
            with self.subTest(suffix=suffix):
                self.path.write_bytes(self.HEADER + self.FINISHED + suffix)
                with self.assertRaises(ET.ParseError):
                    MODULE.memcheck_evidence(self.path, 123)
        self.path.write_bytes(self.HEADER + self.FINISHED
                              + b'<status><state>RUNNING</state></status>' + self.FOOTER)
        with self.assertRaisesRegex(ValueError, "no final FINISHED"):
            MODULE.memcheck_evidence(self.path, 123)

    def test_small_report_validation_contracts_remain_strict(self):
        for body in (b'<error/>', b'<fatal_signal/>',
                     b'<suppcounts><pair><count>-1</count><name>invalid</name></pair></suppcounts>'):
            with self.subTest(body=body):
                self.path.write_bytes(self.HEADER + self.FINISHED + body + self.FOOTER)
                with self.assertRaises(ValueError):
                    MODULE.memcheck_evidence(self.path, 123)
        self.path.write_bytes(b'<other><protocoltool>memcheck</protocoltool><pid>123</pid>'
                              + self.FINISHED + b'</other>')
        with self.assertRaisesRegex(ValueError, "unexpected XML root"):
            MODULE.memcheck_evidence(self.path, 123)

    def test_qttest_report_limit_is_still_64_mib(self):
        with self.path.open("wb") as stream:
            stream.truncate(64 * 1024 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, "oversized"):
            MODULE.qttest_evidence(self.path)


@unittest.skipUnless(sys.platform.startswith("linux"), "Memcheck runner is Linux-only")
class MemcheckRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gc-memcheck-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.fake = self.root / "fake-valgrind"
        self.fake.write_text(f"#!{sys.executable}\n" + FAKE_VALGRIND, encoding="utf-8")
        self.fake.chmod(0o700)
        self.output = self.root / "result"

    def invocation(self, mode="success", timeout="5", binary=None, extra=(), valgrind=None,
                   suppressions=(), debuginfo_path=None):
        environment = os.environ.copy()
        environment.pop("QT_QPA_PLATFORM", None)
        environment["GC_FAKE_MEMCHECK"] = mode
        environment["VALGRIND_LIB"] = "/fixture/valgrind-lib"
        environment["QT_IM_MODULE"] = "compose"
        environment["QT_ENABLE_REGEXP_JIT"] = "0"
        environment["QSG_RHI_BACKEND"] = "opengl"
        environment["QT_QUICK_BACKEND"] = "software"
        environment["LIBGL_ALWAYS_SOFTWARE"] = "1"
        environment["GC_FAKE_PRIVATE_VALUE"] = "do-not-record-this-value"
        suppression_options = [value for path in suppressions for value in ("--suppressions", str(path))]
        debuginfo_options = [] if debuginfo_path is None else ["--debuginfo-path", str(debuginfo_path)]
        return ([sys.executable, str(RUNNER), "--valgrind", str(valgrind or self.fake),
                 "--output", str(self.output), "--timeout", timeout, *suppression_options,
                 *debuginfo_options, "--",
                 str(binary or Path(sys.executable).resolve()), *extra], environment)

    def run_fixture(self, mode="success", **kwargs):
        command, environment = self.invocation(mode, **kwargs)
        return subprocess.run(command, env=environment, capture_output=True, text=True, timeout=10)

    def summary(self):
        return json.loads((self.output / "summary.json").read_text())

    def assert_child_stopped(self):
        pid = int((self.output / "child.pid").read_text())
        for _ in range(50):
            status = Path(f"/proc/{pid}/stat")
            if not status.exists() or status.read_text().split(")", 1)[1].strip().startswith("Z"):
                return
            time.sleep(0.02)
        self.fail(f"runner left child {pid} alive")

    def test_success_artifacts_options_and_isolation(self):
        result = self.run_fixture(extra=("actualTest:data",))
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = self.summary()
        self.assertEqual(summary["status"], "passed")
        self.assertEqual(summary["qttest"]["passed"], 1)
        self.assertEqual(summary["environment"], {
            "QT_QPA_PLATFORM": "offscreen", "QT_IM_MODULE": "compose",
            "QSG_RHI_BACKEND": "opengl", "QT_QUICK_BACKEND": "software",
            "LIBGL_ALWAYS_SOFTWARE": "1", "GALLIUM_DRIVER": os.environ.get("GALLIUM_DRIVER"),
            "DRAW_USE_LLVM": os.environ.get("DRAW_USE_LLVM"), "VALGRIND_LIB": "/fixture/valgrind-lib",
            "GC_TEST_TIMEOUT_SCALE": os.environ.get("GC_TEST_TIMEOUT_SCALE"),
            "QT_ENABLE_REGEXP_JIT": "0",
        })
        self.assertNotIn("do-not-record-this-value", json.dumps(summary))
        self.assertEqual((self.output / "private-fixture.txt").stat().st_mode & 0o777, 0o600)
        self.assertEqual(json.loads((self.output / "regexp-jit.json").read_text()), "0")
        self.assertEqual(summary["memcheck"]["suppressed"], [{"name": "known-default", "count": 2}])
        self.assertIn("application output", (self.output / "application.log").read_text())
        self.assertEqual(ET.parse(self.output / "junit.xml").getroot().get("failures"), "0")
        options = json.loads((self.output / "arguments.json").read_text())
        self.assertFalse(any(option.startswith("--suppressions=") for option in options))
        self.assertFalse(any(option.startswith("--extra-debuginfo-path=") for option in options))
        self.assertIsNone(summary["debuginfo_path"])
        self.assertEqual(summary["suppression_policy"]["explicit_paths"], [])
        for option in ("--command-line-only=yes", "--tool=memcheck", "--leak-check=full",
                       "--show-leak-kinds=all", "--errors-for-leak-kinds=definite,indirect,possible",
                       "--error-exitcode=97", "--track-origins=yes", "--num-callers=30", "--smc-check=all", "--keep-debuginfo=yes", "--trace-children=no",
                       "--child-silent-after-fork=yes", "actualTest:data"):
            self.assertIn(option, options)
        environment = json.loads((self.output / "environment.json").read_text())
        self.assertEqual(environment.pop("QT_QPA_PLATFORM"), "offscreen")
        self.assertEqual(environment.pop("VALGRIND_LIB"), "/fixture/valgrind-lib")
        for directory in environment.values():
            self.assertEqual(Path(directory).parent, self.output)
            self.assertEqual(Path(directory).stat().st_mode & 0o777, 0o700)

    def test_regexp_jit_setting_is_preserved_not_defaulted(self):
        for index, value in enumerate((None, "0", "1")):
            with self.subTest(value=value):
                self.output = self.root / f"jit-{index}"
                command, environment = self.invocation()
                if value is None:
                    environment.pop("QT_ENABLE_REGEXP_JIT", None)
                else:
                    environment["QT_ENABLE_REGEXP_JIT"] = value
                result = subprocess.run(command, env=environment, capture_output=True,
                                        text=True, timeout=10, umask=0o002)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.summary()["environment"]["QT_ENABLE_REGEXP_JIT"], value)
                self.assertEqual(json.loads((self.output / "regexp-jit.json").read_text()), value)
                self.assertEqual((self.output / "private-fixture.txt").stat().st_mode & 0o777, 0o600)

    def test_failure_evidence(self):
        modes = ("missing", "malformed", "incomplete", "oversized", "wrong-pid", "wrong-tool",
                 "signal-report", "zero", "skip", "qt-fail", "qt-unknown", "qt-empty-function",
                 "qt-malformed", "nonzero", "crash", "error:InvalidRead", "error:Leak_DefinitelyLost",
                 "error:Leak_IndirectlyLost", "error:Leak_PossiblyLost", "error:UninitCondition",
                 "qt-no-cleanup", "qt-cleanup-not-last", "qt-missing-init",
                 "qt-cleanup-skip", "qt-init-skip")
        for index, mode in enumerate(modes):
            with self.subTest(mode=mode):
                self.output = self.root / f"result-{index}"
                result = self.run_fixture(mode)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(self.summary()["status"], "failed")
                self.assertTrue(self.summary()["problems"])
                if mode == "nonzero":
                    self.assertEqual(self.summary()["returncode"], 97)
                self.assertEqual(ET.parse(self.output / "junit.xml").getroot().get("failures"), "1")

    def test_reachable_is_informational(self):
        result = self.run_fixture("error:Leak_StillReachable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.summary()["memcheck"]["error_records"], {"Leak_StillReachable": 1})

    def test_explicit_suppressions_are_forwarded_and_disclosed(self):
        libraries = MODULE.loaded_libraries(Path(sys.executable).resolve(), os.environ.copy())
        pin = json.dumps({"libc.so.6": MODULE.elf_build_id(libraries["libc.so.6"])})
        paths = [self.root / "reviewed-one.supp", self.root / "reviewed-two.supp"]
        for path in paths:
            path.write_text("# synthetic test fixture\n")
            Path(str(path) + ".buildids").write_text(pin)
        result = self.run_fixture(suppressions=paths)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Memcheck passed with explicit suppressions:", result.stdout)
        self.assertEqual(self.summary()["suppression_policy"]["explicit_paths"],
                         [str(path.resolve()) for path in paths])
        self.assertEqual(len(self.summary()["suppression_policy"]["build_id_pins"]), 2)
        self.assertEqual(self.summary()["suppression_policy"]["unpinned_legacy"], [])
        arguments = json.loads((self.output / "arguments.json").read_text())
        for path in paths:
            self.assertIn(f"--suppressions={path.resolve()}", arguments)

    @staticmethod
    def synthetic_elf(path, notes):
        """Write a minimal ELF64 LE file with one PT_NOTE holding (type, name, desc) notes."""
        blob = b""
        for kind, name, desc in notes:
            blob += len(name).to_bytes(4, "little") + len(desc).to_bytes(4, "little")
            blob += kind.to_bytes(4, "little")
            blob += name + b"\0" * (-len(name) % 4) + desc + b"\0" * (-len(desc) % 4)
        header = bytearray(64)
        header[:7] = b"\x7fELF\x02\x01\x01"
        header[0x20:0x28] = (64).to_bytes(8, "little")
        header[0x36:0x38] = (56).to_bytes(2, "little")
        header[0x38:0x3a] = (1).to_bytes(2, "little")
        program = bytearray(56)
        program[0:4] = (4).to_bytes(4, "little")
        program[8:16] = (120).to_bytes(8, "little")
        program[32:40] = len(blob).to_bytes(8, "little")
        program[48:56] = (4).to_bytes(8, "little")
        path.write_bytes(bytes(header) + bytes(program) + blob)
        return path

    def test_elf_build_id_reads_gnu_note_after_other_notes(self):
        build_id = bytes(range(20))
        path = self.synthetic_elf(self.root / "libnote.so", [
            (1, b"GNU\0", b"\x01\x02\x03\x04"), (3, b"XYZ\0", b"\xff" * 8), (3, b"GNU\0", build_id)])
        self.assertEqual(MODULE.elf_build_id(path), build_id.hex())

    def test_elf_build_id_rejects_non_elf_and_missing_note(self):
        (self.root / "text.so").write_text("not an ELF\n")
        without = self.synthetic_elf(self.root / "libnone.so", [(1, b"GNU\0", b"\x00" * 4)])
        for path, message in ((self.root / "text.so", "not a 64-bit"), (without, "no GNU Build-ID")):
            with self.subTest(path=path.name), self.assertRaisesRegex(ValueError, message):
                MODULE.elf_build_id(path)

    def pinned_suppression(self, pins):
        path = self.root / "reviewed-library.supp"
        path.write_text("# synthetic test fixture\n")
        Path(str(path) + ".buildids").write_text(json.dumps(pins))
        return path

    def test_build_id_pins_are_verified_and_recorded(self):
        libraries = MODULE.loaded_libraries(Path(sys.executable).resolve(), os.environ.copy())
        self.assertIn("libc.so.6", libraries)
        expected = MODULE.elf_build_id(libraries["libc.so.6"])
        suppression = self.pinned_suppression({"libc.so.6": expected.upper()})
        result = self.run_fixture(suppressions=(suppression,))
        self.assertEqual(result.returncode, 0, result.stderr)
        pins = {pin["library"]: pin for pin in self.summary()["suppression_policy"]["build_id_pins"]}
        self.assertEqual(pins["libc.so.6"]["actual"], expected)
        self.assertEqual(pins["libc.so.6"]["expected"], expected)
        self.assertEqual(pins["libc.so.6"]["resolved_by"], "ldd")

    def test_unpinned_suppressions_fail_unless_listed_legacy(self):
        unpinned = self.root / "reviewed-unpinned.supp"
        unpinned.write_text("# synthetic test fixture\n")
        result = self.run_fixture(suppressions=(unpinned,))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any("has no Build-ID pin file" in p for p in self.summary()["problems"]),
                        self.summary()["problems"])
        self.assertFalse((self.output / "arguments.json").exists())
        self.output = self.root / "legacy-run"
        legacy = self.root / "qt-6.8.3-gui-tls.supp"
        legacy.write_text("# synthetic test fixture\n")
        result = self.run_fixture(suppressions=(legacy,))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.summary()["suppression_policy"]["unpinned_legacy"], [str(legacy.resolve())])
        self.assertIn("legacy suppression file without Build-ID pins", result.stderr)

    def test_unresolved_build_id_pin_fails_before_launch(self):
        libraries = MODULE.loaded_libraries(Path(sys.executable).resolve(), os.environ.copy())
        suppression = self.pinned_suppression({"libc.so.6": MODULE.elf_build_id(libraries["libc.so.6"]),
                                               "libgc-not-loaded.so.1": "00"})
        result = self.run_fixture(suppressions=(suppression,))
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any("pinned to libgc-not-loaded.so.1" in problem and "cannot verify" in problem
                            for problem in self.summary()["problems"]), self.summary()["problems"])
        self.assertFalse((self.output / "arguments.json").exists())

    def test_build_id_pin_mismatch_fails_before_launch(self):
        suppression = self.pinned_suppression({"libc.so.6": "0" * 40})
        result = self.run_fixture(suppressions=(suppression,))
        self.assertNotEqual(result.returncode, 0)
        summary = self.summary()
        self.assertEqual(summary["status"], "failed")
        self.assertTrue(any("is pinned to libc.so.6 Build-ID" in problem and "re-verify" in problem
                            for problem in summary["problems"]), summary["problems"])
        self.assertFalse((self.output / "arguments.json").exists())

    def test_plugin_pins_resolve_from_qt_plugin_path_and_qt_installation(self):
        build_id = bytes(range(20))
        installed = self.root / "qt" / "plugins" / "platforms"
        installed.mkdir(parents=True)
        self.synthetic_elf(installed / "libqxcb.so", [(3, b"GNU\0", build_id)])
        libraries = {"libQt6Core.so.6": str(self.root / "qt" / "lib" / "libQt6Core.so.6")}
        path, resolved_by = MODULE.resolve_pinned_library("libqxcb.so", libraries, {})
        self.assertEqual((Path(path), resolved_by), ((installed / "libqxcb.so").resolve(), "plugin"))
        bundled = self.root / "appdir" / "plugins" / "platforms"
        bundled.mkdir(parents=True)
        self.synthetic_elf(bundled / "libqxcb.so", [(3, b"GNU\0", build_id[::-1])])
        environment = {"QT_PLUGIN_PATH": str(self.root / "appdir" / "plugins")}
        path, resolved_by = MODULE.resolve_pinned_library("libqxcb.so", libraries, environment)
        self.assertEqual(MODULE.elf_build_id(path), build_id[::-1].hex())
        self.assertEqual(MODULE.resolve_pinned_library("libqnone.so", libraries, environment), (None, None))

    @staticmethod
    def synthetic_debuglink_elf(path, debuglink):
        """Minimal ELF64 LE with section headers: null, .shstrtab, .gnu_debuglink."""
        shstrtab = b"\0.shstrtab\0.gnu_debuglink\0"
        link = debuglink.encode() + b"\0" * (4 - len(debuglink) % 4) + b"\x01\x02\x03\x04"
        data_offset = 64
        link_offset = data_offset + len(shstrtab)
        shoff = link_offset + len(link)
        header = bytearray(64)
        header[:7] = b"\x7fELF\x02\x01\x01"
        header[0x28:0x30] = shoff.to_bytes(8, "little")
        header[0x3A:0x3C] = (64).to_bytes(2, "little")
        header[0x3C:0x3E] = (3).to_bytes(2, "little")
        header[0x3E:0x40] = (1).to_bytes(2, "little")

        def section(name, offset, size):
            entry = bytearray(64)
            entry[0:4] = name.to_bytes(4, "little")
            entry[0x18:0x20] = offset.to_bytes(8, "little")
            entry[0x20:0x28] = size.to_bytes(8, "little")
            return bytes(entry)
        sections = bytes(64) + section(1, data_offset, len(shstrtab)) + section(11, link_offset, len(link))
        path.write_bytes(bytes(header) + shstrtab + link + sections)
        return path

    def test_elf_debuglink_reads_section_name(self):
        path = self.synthetic_debuglink_elf(self.root / "libQt6Fake.so.6", "Qt6Fake.debug")
        self.assertEqual(MODULE.elf_debuglink(path), "Qt6Fake.debug")
        self.assertIsNone(MODULE.elf_debuglink(self.synthetic_elf(self.root / "libnolink.so", [])))

    def test_debuginfo_coverage_requires_debuglink_mirror_of_object_directory(self):
        library = self.root / "appdir" / "lib"
        library.mkdir(parents=True)
        path = self.synthetic_debuglink_elf(library / "libQt6Fake.so.6", "Qt6Fake.debug")
        debuginfo = self.root / "debuginfo"
        debuginfo.mkdir()
        with mock.patch.object(MODULE, "loaded_libraries", return_value={"libQt6Fake.so.6": str(path),
                                                                         "libc.so.6": "/lib/libc.so.6"}):
            self.assertEqual(MODULE.debuginfo_coverage("binary", {}, str(debuginfo), set()), [])
            self.assertEqual(MODULE.debuginfo_coverage("binary", {}, str(debuginfo), {"libQt6Fake.so.6"}),
                             ["libQt6Fake.so.6 (Qt6Fake.debug)"])
            mirror = debuginfo / library.resolve().relative_to("/")
            mirror.mkdir(parents=True)
            (mirror / "Qt6Fake.debug").write_text("synthetic test fixture")
            self.assertEqual(MODULE.debuginfo_coverage("binary", {}, str(debuginfo), {"libQt6Fake.so.6"}), [])

    def test_debuginfo_coverage_includes_pinned_libraries_found_outside_ldd(self):
        library = self.root / "appdir" / "lib"
        library.mkdir(parents=True)
        self.synthetic_debuglink_elf(library / "libQt6XcbFake.so.6", "Qt6XcbFake.debug")
        debuginfo = self.root / "debuginfo"
        debuginfo.mkdir()
        environment = {"LD_LIBRARY_PATH": str(library)}
        with mock.patch.object(MODULE, "loaded_libraries", return_value={}):
            self.assertEqual(MODULE.debuginfo_coverage("binary", environment, str(debuginfo),
                                                       {"libQt6XcbFake.so.6"}),
                             ["libQt6XcbFake.so.6 (Qt6XcbFake.debug)"])

    def test_dri_driver_pins_resolve_from_libgl_drivers_path(self):
        drivers = self.root / "dri"
        drivers.mkdir()
        self.synthetic_elf(drivers / "swrast_dri.so", [(3, b"GNU\0", bytes(range(20)))])
        path, resolved_by = MODULE.resolve_pinned_library(
            "swrast_dri.so", {}, {"LIBGL_DRIVERS_PATH": str(drivers)})
        self.assertEqual((Path(path), resolved_by), ((drivers / "swrast_dri.so").resolve(), "dri"))

    def test_invalid_build_id_pin_file_fails_before_launch(self):
        for index, pins in enumerate(({}, ["libc.so.6"], {"libc.so.6": ""})):
            with self.subTest(pins=pins):
                self.output = self.root / f"invalid-pins-{index}"
                result = self.run_fixture(suppressions=(self.pinned_suppression(pins),))
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue(any("Build-ID pin file" in p for p in self.summary()["problems"]))
                self.assertFalse((self.output / "arguments.json").exists())

    def test_explicit_debuginfo_directory_is_forwarded_and_recorded(self):
        directory = self.root / "debug symbols"
        directory.mkdir()
        alias = self.root / "debug-alias"
        alias.symlink_to(directory, target_is_directory=True)
        result = self.run_fixture(debuginfo_path=alias)
        self.assertEqual(result.returncode, 0, result.stderr)
        option = f"--extra-debuginfo-path={directory.resolve()}"
        arguments = json.loads((self.output / "arguments.json").read_text())
        self.assertEqual(arguments.count(option), 1)
        self.assertIn(option, self.summary()["command"])
        self.assertEqual(self.summary()["debuginfo_path"], str(directory.resolve()))
        self.assertEqual(self.summary()["suppression_policy"]["explicit_paths"], [])

    def test_missing_file_or_empty_debuginfo_path_fails_before_launch(self):
        for index, path in enumerate((self.root / "missing-debug", self.fake, "")):
            with self.subTest(path=path):
                self.output = self.root / f"invalid-debuginfo-{index}"
                result = self.run_fixture(debuginfo_path=path)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.summary()["status"], "failed")
                self.assertFalse((self.output / "arguments.json").exists())

    def test_missing_or_nonfile_suppression_fails_before_launch(self):
        for index, path in enumerate((self.root / "missing.supp", self.root)):
            with self.subTest(path=path):
                self.output = self.root / f"invalid-suppression-{index}"
                result = self.run_fixture(suppressions=(path,))
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.summary()["status"], "failed")
                self.assertFalse((self.output / "arguments.json").exists())

    def test_existing_directory_is_not_reused(self):
        self.output.mkdir()
        sentinel = self.output / "summary.json"
        sentinel.write_text("old result")
        result = self.run_fixture()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sentinel.read_text(), "old result")
        self.assertEqual(list(self.output.iterdir()), [sentinel])

    def test_invalid_inputs(self):
        script = self.root / "test-script"
        script.write_text("#!/bin/sh\nexit 0\n")
        script.chmod(0o700)
        asan = self.root / "asan-binary"
        asan.write_bytes(b"\x7fELFfake ELF with __asan_init symbol")
        asan.chmod(0o700)
        cases = ({"binary": script}, {"binary": asan}, {"valgrind": self.root / "missing"},
                 {"binary": self.root / "missing"}, {"extra": ("-o", "other.xml,xml")},
                 {"extra": ("-xml",)}, {"timeout": "nan"}, {"timeout": "0"}, {"timeout": "86401"})
        for index, arguments in enumerate(cases):
            with self.subTest(arguments=arguments):
                self.output = self.root / f"invalid-{index}"
                result = self.run_fixture(**arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.output / "arguments.json").exists())

    def test_timeout_stops_entire_group(self):
        result = self.run_fixture("timeout", timeout="0.5")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("test run timed out", self.summary()["problems"])
        self.assert_child_stopped()

    def test_interrupt_stops_entire_group(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signum):
                self.output = self.root / f"signal-{signum}"
                command, environment = self.invocation("interrupt")
                process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    deadline = time.monotonic() + 5
                    while not (self.output / "child.pid").exists() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertTrue((self.output / "child.pid").exists())
                    process.send_signal(signum)
                    process.communicate(timeout=5)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn(f"interrupted by signal {signum}", self.summary()["problems"])
                    self.assert_child_stopped()
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.communicate()

    def test_child_is_stopped_after_leader_exits(self):
        result = self.run_fixture("orphan")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_child_stopped()


if __name__ == "__main__":
    unittest.main()
