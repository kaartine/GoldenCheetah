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
                   suppressions=()):
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
        return ([sys.executable, str(RUNNER), "--valgrind", str(valgrind or self.fake),
                 "--output", str(self.output), "--timeout", timeout, *suppression_options, "--",
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
            "LIBGL_ALWAYS_SOFTWARE": "1", "VALGRIND_LIB": "/fixture/valgrind-lib",
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
        self.assertEqual(summary["suppression_policy"]["explicit_paths"], [])
        for option in ("--command-line-only=yes", "--tool=memcheck", "--leak-check=full",
                       "--show-leak-kinds=all", "--errors-for-leak-kinds=definite,indirect,possible",
                       "--error-exitcode=97", "--track-origins=yes", "--num-callers=30", "--smc-check=all", "--trace-children=no",
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
        paths = [self.root / "reviewed-one.supp", self.root / "reviewed-two.supp"]
        for path in paths:
            path.write_text("# synthetic test fixture\n")
        result = self.run_fixture(suppressions=paths)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Memcheck passed with explicit suppressions:", result.stdout)
        self.assertEqual(self.summary()["suppression_policy"]["explicit_paths"],
                         [str(path.resolve()) for path in paths])
        arguments = json.loads((self.output / "arguments.json").read_text())
        for path in paths:
            self.assertIn(f"--suppressions={path.resolve()}", arguments)

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
