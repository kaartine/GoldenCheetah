#!/usr/bin/env python3
"""Exercise the UI Memcheck contract using temporary synthetic evidence only."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
DIRECTORY = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("ui_memcheck", DIRECTORY / "ui_memcheck.py")
UI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UI)


@unittest.skipUnless(sys.platform.startswith("linux"), "UI Memcheck requires Linux")
class UiMemcheckTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gc-ui-memcheck-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.runtime = self.root / "AppDir"
        (self.runtime / "lib").mkdir(parents=True)
        (self.runtime / "plugins").mkdir()
        self.binary = self.runtime / "GoldenCheetah"
        shutil.copyfile(Path(sys.executable).resolve(), self.binary)
        self.binary.chmod(0o700)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir()
        self.valgrind = str(Path(sys.executable).resolve())

    def prepare(self, **overrides):
        values = dict(image=self.binary, appdir=str(self.runtime), artifacts=self.artifacts,
                      valgrind=self.valgrind)
        values.update(overrides)
        return UI.prepare(**values)

    def reports(self, state="FINISHED", kind=None, pid=123, names=None):
        output = self.artifacts / "memcheck"
        output.mkdir(exist_ok=True)
        if not (output / "invocation.json").exists():  # prepare() always writes it
            (output / "invocation.json").write_text(json.dumps({"suppression_policy": {
                "defaults": "Valgrind installation defaults; user rc/options ignored",
                "explicit_paths": [], "build_id_pins": []}}))
        root = ET.Element("valgrindoutput")
        ET.SubElement(root, "protocoltool").text = "memcheck"
        ET.SubElement(root, "pid").text = str(pid)
        ET.SubElement(ET.SubElement(root, "status"), "state").text = state
        if kind:
            ET.SubElement(ET.SubElement(root, "error"), "kind").text = kind
        ET.ElementTree(root).write(output / "memcheck-123.xml")
        names = names if names is not None else ["startup_and_main_navigation", "graceful_shutdown_request"]
        suite = ET.Element("testsuite", tests=str(len(names)), failures="0")
        for name in names:
            ET.SubElement(suite, "testcase", name=name)
        ET.ElementTree(suite).write(self.artifacts / "junit.xml")

    def validate(self, app_status=0, ui_status=0):
        with contextlib.redirect_stderr(io.StringIO()):
            return UI.validate(self.artifacts, 123, app_status, ui_status)

    def test_prepare_prefix_and_stale_rejection(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            prefix = self.prepare()
        self.assertEqual(prefix[0], self.valgrind)
        for option in ("--command-line-only=yes", "--tool=memcheck", "--leak-check=full",
                       "--show-leak-kinds=all", "--errors-for-leak-kinds=definite,indirect,possible",
                       "--track-origins=yes", "--num-callers=30", "--smc-check=all", "--keep-debuginfo=yes", "--error-exitcode=97",
                       "--xml=yes", "--trace-children=no", "--child-silent-after-fork=yes"):
            self.assertIn(option, prefix)
        self.assertFalse(any(arg.startswith("--suppressions") for arg in prefix))
        self.assertFalse(any(arg.startswith("--extra-debuginfo-path=") for arg in prefix))
        output = self.artifacts / "memcheck"
        self.assertEqual((output / "command-prefix.nul").read_bytes().split(b"\0")[:-1],
                         [os.fsencode(arg) for arg in prefix])
        invocation = json.loads((output / "invocation.json").read_text())
        self.assertEqual(invocation["binary"], str(self.binary))
        self.assertEqual(invocation["appdir"], str(self.runtime))
        self.assertIsNone(invocation["debuginfo_path"])
        self.assertIsNone(invocation["environment"]["GC_UI_MEMCHECK_DEBUGINFO_PATH"])
        with self.assertRaises(FileExistsError):
            self.prepare()

    def test_explicit_debuginfo_directory_is_forwarded_and_recorded(self):
        directory = self.root / "debug symbols"
        directory.mkdir()
        alias = self.root / "debug-alias"
        alias.symlink_to(directory, target_is_directory=True)
        with mock.patch.dict(os.environ, {"GC_UI_MEMCHECK_DEBUGINFO_PATH": str(alias)}):
            prefix = self.prepare()
            option = f"--extra-debuginfo-path={directory.resolve()}"
            self.assertEqual(prefix.count(option), 1)
            output = self.artifacts / "memcheck"
            self.assertIn(os.fsencode(option), (output / "command-prefix.nul").read_bytes().split(b"\0"))
            invocation = json.loads((output / "invocation.json").read_text())
            self.assertEqual(invocation["prefix"], prefix)
            self.assertEqual(invocation["debuginfo_path"], str(directory.resolve()))
            self.assertEqual(invocation["environment"]["GC_UI_MEMCHECK_DEBUGINFO_PATH"], str(alias))
            self.assertFalse(any(arg.startswith("--suppressions") for arg in prefix))
            self.reports()
            self.assertEqual(self.validate(), 0)
            summary = json.loads((output / "summary.json").read_text())
            self.assertEqual(summary["environment"]["GC_UI_MEMCHECK_DEBUGINFO_PATH"], str(alias))

    def test_missing_file_or_empty_debuginfo_path_rejected_before_preparation(self):
        for path in (self.root / "missing-debug", self.binary, ""):
            with self.subTest(path=path), mock.patch.dict(
                    os.environ, {"GC_UI_MEMCHECK_DEBUGINFO_PATH": str(path)}):
                with self.assertRaises((OSError, ValueError)):
                    self.prepare()
                self.assertFalse((self.artifacts / "memcheck").exists())

    def test_requires_matching_runtime_native_elf(self):
        outside = self.root / "GoldenCheetah"
        shutil.copyfile(self.binary, outside)
        outside.chmod(0o700)
        for values in ({"appdir": ""}, {"appdir": str(self.root)}, {"image": outside},
                       {"valgrind": str(self.root / "unavailable")}):
            with self.subTest(values=values), self.assertRaises((ValueError, OSError)):
                self.prepare(**values)
        for contents in (b"#!/bin/sh\nexit 0\n", b"\x7fELF\x02\x01\x01\0AI\x02\0",
                         b"\x7fELF __asan_init test"):
            with self.subTest(contents=contents):
                self.binary.write_bytes(contents)
                with self.assertRaises(ValueError):
                    self.prepare()
        self.assertFalse((self.artifacts / "memcheck").exists())

    def pinned_suppression(self, pins):
        path = self.root / "reviewed-library.supp"
        path.write_text("# synthetic test fixture\n")
        Path(str(path) + ".buildids").write_text(json.dumps(pins))
        return path

    def test_explicit_suppressions_are_pinned_forwarded_and_disclosed(self):
        libraries = UI.RUNNER.loaded_libraries(self.binary, dict(os.environ))
        expected = UI.RUNNER.elf_build_id(libraries["libc.so.6"])
        suppression = self.pinned_suppression({"libc.so.6": expected})
        with mock.patch.dict(os.environ, {"GC_UI_MEMCHECK_SUPPRESSIONS": str(suppression)}):
            prefix = self.prepare()
            self.assertEqual(prefix.count(f"--suppressions={suppression.resolve()}"), 1)
            invocation = json.loads((self.artifacts / "memcheck/invocation.json").read_text())
            policy = invocation["suppression_policy"]
            self.assertEqual(policy["explicit_paths"], [str(suppression.resolve())])
            self.assertEqual([(p["library"], p["actual"]) for p in policy["build_id_pins"]],
                             [("libc.so.6", expected)])
            self.reports()
            self.assertEqual(self.validate(), 0)
            summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
            self.assertEqual(summary["suppression_policy"], policy)
            self.assertEqual(summary["environment"]["GC_UI_MEMCHECK_SUPPRESSIONS"], str(suppression))

    def test_listed_legacy_suppression_is_recorded_and_warned(self):
        legacy = self.root / "qt-6.8.3-gui-tls.supp"
        legacy.write_text("# synthetic test fixture\n")
        stderr = io.StringIO()
        with mock.patch.dict(os.environ, {"GC_UI_MEMCHECK_SUPPRESSIONS": str(legacy)}), \
                contextlib.redirect_stderr(stderr):
            self.prepare()
        policy = json.loads((self.artifacts / "memcheck/invocation.json").read_text())["suppression_policy"]
        self.assertEqual(policy["unpinned_legacy"], [str(legacy.resolve())])
        self.assertIn("legacy suppression file without Build-ID pins", stderr.getvalue())

    def test_suppression_pin_mismatch_or_missing_file_rejected_before_preparation(self):
        unpinned = self.root / "reviewed-unpinned.supp"
        unpinned.write_text("# synthetic test fixture\n")
        cases = ((str(self.pinned_suppression({"libc.so.6": "0" * 40})), "re-verify"),
                 (str(unpinned), "no Build-ID pin file"),
                 (str(self.root / "missing.supp"), ""), ("", "existing file"))
        for value, message in cases:
            with self.subTest(value=value), mock.patch.dict(
                    os.environ, {"GC_UI_MEMCHECK_SUPPRESSIONS": value}):
                with self.assertRaisesRegex((ValueError, OSError), message):
                    self.prepare()
                self.assertFalse((self.artifacts / "memcheck").exists())

    def test_missing_invocation_record_fails_validation(self):
        self.reports()
        (self.artifacts / "memcheck/invocation.json").unlink()
        self.assertEqual(self.validate(), 1)
        summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
        self.assertTrue(any("invocation record" in p for p in summary["problems"]))

    def test_clean_and_reachable_evidence_pass(self):
        for kind in (None, "Leak_StillReachable"):
            with self.subTest(kind=kind):
                self.reports(kind=kind)
                self.assertEqual(self.validate(), 0)
                summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
                self.assertEqual(summary["status"], "passed")
                self.assertEqual(summary["ui"]["passed"], 2)

    def test_regexp_jit_setting_is_recorded_not_changed(self):
        self.reports()
        for value in (None, "0", "1"):
            environment = {} if value is None else {"QT_ENABLE_REGEXP_JIT": value}
            with self.subTest(value=value), mock.patch.dict(os.environ, environment, clear=True):
                self.assertEqual(self.validate(), 0)
                summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
                self.assertEqual(summary["environment"]["QT_ENABLE_REGEXP_JIT"], value)
                self.assertEqual(os.environ.get("QT_ENABLE_REGEXP_JIT"), value)

    def test_software_gl_driver_is_recorded_not_changed(self):
        self.reports()
        with mock.patch.dict(os.environ, {"GALLIUM_DRIVER": "softpipe", "DRAW_USE_LLVM": "0"}, clear=True):
            self.assertEqual(self.validate(), 0)
        summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
        self.assertEqual(summary["environment"]["GALLIUM_DRIVER"], "softpipe")
        self.assertEqual(summary["environment"]["DRAW_USE_LLVM"], "0")
        runner = (DIRECTORY / "run-pre-release-ui.sh").read_text(encoding="utf-8")
        self.assertNotIn("GALLIUM_DRIVER", runner)
        self.assertNotIn("DRAW_USE_LLVM", runner)

    def test_failed_exit_or_workflow_cannot_pass(self):
        self.reports()
        for app_status, ui_status in ((97, 0), (139, 0), (0, 1), (143, 124)):
            with self.subTest(app_status=app_status, ui_status=ui_status):
                self.assertEqual(self.validate(app_status, ui_status), 1)
                summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
                self.assertEqual(summary["app_returncode"], app_status)
                self.assertEqual(summary["ui_returncode"], ui_status)

    def test_incomplete_or_bad_memcheck_cannot_pass(self):
        for values in ({"state": "RUNNING"}, {"pid": 999}, {"kind": "InvalidRead"},
                       {"kind": "Leak_PossiblyLost"}, {"kind": "Leak_DefinitelyLost"},
                       {"kind": "Leak_IndirectlyLost"}):
            with self.subTest(values=values):
                self.reports(**values)
                self.assertEqual(self.validate(), 1)
        report = self.artifacts / "memcheck/memcheck-123.xml"
        for content in ("", "<valgrindoutput>"):
            report.write_text(content)
            self.assertEqual(self.validate(), 1)
        report.unlink()
        self.assertEqual(self.validate(), 1)

    def test_requires_passed_ui_and_shutdown(self):
        for names in ([], ["startup_and_main_navigation"],
                      ["graceful_shutdown_request", "graceful_shutdown_request"]):
            with self.subTest(names=names):
                self.reports(names=names)
                self.assertEqual(self.validate(), 1)
        for kind in ("failure", "error", "skipped"):
            with self.subTest(kind=kind):
                self.reports()
                path = self.artifacts / "junit.xml"
                tree = ET.parse(path)
                ET.SubElement(tree.getroot().findall("testcase")[-1], kind)
                tree.write(path)
                self.assertEqual(self.validate(), 1)
        self.reports()
        (self.artifacts / "junit.xml").write_text("<testsuite/>")
        self.assertEqual(self.validate(), 1)

    def test_shell_wiring(self):
        script = DIRECTORY / "run-pre-release-ui.sh"
        result = subprocess.run(["bash", "-n", str(script)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        text = script.read_text()
        self.assertIn('setsid "${APP_ENV[@]}" "${MEMCHECK_PREFIX[@]}" "$IMAGE"', text)
        self.assertIn('MEMCHECK_PREFIX=()', text)
        self.assertIn('if wait "$APP_PID"; then', text)
        self.assertIn('--app-status "$APP_EXIT_STATUS" --ui-status "$workflow_status"', text)
        self.assertIn('python3 "$SCRIPT_DIR/ui_memcheck.py" validate', text)
        self.assertIn('python3 "$SCRIPT_DIR/pre_release_ui.py" exercise', text)
        self.assertNotIn('MEMCHECK_PREFIX[@]}" python3', text)
        environment = os.environ.copy()
        environment["GC_UI_MEMCHECK"] = "invalid"
        result = subprocess.run(["bash", str(script), str(self.binary)], env=environment,
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 2)
        self.assertIn("GC_UI_MEMCHECK must be 0 or 1", result.stderr)

    def test_shell_reaps_and_preserves_application_exit_status(self):
        # Execute the real terminal shell block against a synthetic process;
        # neither GoldenCheetah nor the athlete preparation workflow runs here.
        text = (DIRECTORY / "run-pre-release-ui.sh").read_text()
        final_block = text[text.rindex('\nif [ "$MEMCHECK" = 1 ]; then'):]
        final_block = final_block.replace('\nexit "$STATUS"', '\nfinalize_memcheck 55\nexit "$STATUS"')
        functions = text[text.index("stop_app_group()\n"):text.index("cleanup()\n")]
        fake = self.root / "fake-app.py"
        fake.write_text(
            "import os,sys\nfrom pathlib import Path\n"
            "pid = os.getpid()\n"
            "Path(sys.argv[1], 'memcheck', f'memcheck-{pid}.xml').write_text("
            "f'<valgrindoutput><protocoltool>memcheck</protocoltool><pid>{pid}</pid>'"
            "'<status><state>FINISHED</state></status></valgrindoutput>')\n"
            "sys.exit(int(sys.argv[2]))\n", encoding="utf-8")
        harness = '''set -euo pipefail
SCRIPT_DIR=$1
ARTIFACT_DIR=$2
MEMCHECK=1
STATUS=0
APP_EXIT_STATUS=
MEMCHECK_FINALIZED=0
"$3" "$4" "$ARTIFACT_DIR" "$5" &
APP_PID=$!
APP_PGID=$APP_PID
''' + functions + final_block
        self.reports()
        for status in (0, 97):
            with self.subTest(status=status):
                result = subprocess.run(
                    ["bash", "-c", harness, "ui-memcheck-test", str(DIRECTORY),
                     str(self.artifacts), sys.executable, str(fake), str(status)],
                    capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0 if status == 0 else 1, result.stderr)
                summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
                self.assertEqual(summary["app_returncode"], status)
                self.assertEqual(summary["ui_returncode"], 0)

    def test_shell_signal_and_errexit_preserve_failed_evidence(self):
        text = (DIRECTORY / "run-pre-release-ui.sh").read_text()
        cleanup_and_traps = text[text.index("stop_app_group()\n"):
                                 text.index('python3 "$SCRIPT_DIR/pre_release_ui.py" prepare')]
        fake = self.root / "waiting-app.py"
        fake.write_text(
            "import os,signal,sys,time\nfrom pathlib import Path\n"
            "signal.signal(signal.SIGTERM, lambda *_: sys.exit(99))\n"
            "root = Path(sys.argv[1])\npid = os.getpid()\n"
            "(root / 'memcheck' / f'memcheck-{pid}.xml').write_text("
            "f'<valgrindoutput><protocoltool>memcheck</protocoltool><pid>{pid}</pid>'"
            "'<status><state>RUNNING</state></status></valgrindoutput>')\n"
            "(root / 'ready.pid').write_text(str(pid))\ntime.sleep(30)\n", encoding="utf-8")
        harness = '''set -euo pipefail
SCRIPT_DIR=$1
ARTIFACT_DIR=$2
TEST_ROOT=$3
MEMCHECK=1
MEMCHECK_FINALIZED=0
APP_EXIT_STATUS=
APP_PID=
APP_PGID=
VIDEO_PID=
XVFB_PID=
''' + cleanup_and_traps + '''
setsid "$4" "$5" "$ARTIFACT_DIR" &
APP_PID=$!
APP_PGID=$APP_PID
while [ ! -f "$ARTIFACT_DIR/ready.pid" ]; do sleep 0.02; done
if [ "$6" = error ]; then (exit 23); fi
while :; do sleep 0.02; done
'''
        environment = os.environ.copy()
        environment.pop("GC_UI_TRAINING_FAILURE_CASE", None)
        environment["GC_UI_TIMEOUT_SCALE"] = "20"
        environment["QT_ENABLE_REGEXP_JIT"] = "0"
        for event, expected in ((signal.SIGHUP, 129), (signal.SIGINT, 130),
                                (signal.SIGTERM, 143), ("error", 23)):
            with self.subTest(event=event):
                self.artifacts = self.root / f"signal-{event}"
                self.artifacts.mkdir()
                self.reports()
                fixture = self.root / f"fixture-{event}"
                fixture.mkdir()
                process = subprocess.Popen(
                    ["bash", "-c", harness, "ui-memcheck-signal-test", str(DIRECTORY),
                     str(self.artifacts), str(fixture), sys.executable, str(fake), str(event)],
                    env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                app_pid = None
                try:
                    deadline = time.monotonic() + 5
                    ready = self.artifacts / "ready.pid"
                    while not ready.exists() and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertTrue(ready.exists())
                    app_pid = int(ready.read_text())
                    if event != "error":
                        process.send_signal(event)
                    _, stderr = process.communicate(timeout=10)
                    self.assertEqual(process.returncode, expected, stderr.decode())
                    summary = json.loads((self.artifacts / "memcheck/summary.json").read_text())
                    self.assertEqual(summary["status"], "failed")
                    self.assertEqual(summary["ui_returncode"], expected)
                    self.assertEqual(summary["app_returncode"], 99)
                    self.assertEqual(summary["environment"]["GC_UI_TIMEOUT_SCALE"], "20")
                    self.assertEqual(summary["environment"]["QT_ENABLE_REGEXP_JIT"], "0")
                    report = ET.parse(self.artifacts / "memcheck/junit.xml").getroot()
                    self.assertEqual(report.get("failures"), "1")
                    with self.assertRaises(ProcessLookupError):
                        os.killpg(app_pid, 0)
                finally:
                    if app_pid is not None:
                        try:
                            os.killpg(app_pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    if process.poll() is None:
                        process.kill()
                        process.communicate()


if __name__ == "__main__":
    unittest.main()
