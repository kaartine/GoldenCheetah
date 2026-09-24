#!/usr/bin/env python3
"""Opt-in Linux Memcheck gate for one explicitly supplied native QtTest binary."""

import argparse
from collections import Counter
import json
import math
import mmap
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET


MAX_REPORT_BYTES = 64 * 1024 * 1024
MAX_MEMCHECK_REPORT_BYTES = 256 * 1024 * 1024


def timeout_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or not 0 < seconds <= 86400:
        raise argparse.ArgumentTypeError("timeout must be greater than 0 and at most 86400 seconds")
    return seconds


def resolve_debuginfo_path(value):
    if value is None:
        return None
    if not value:
        raise ValueError("debuginfo path must be a nonempty existing directory")
    path = Path(value).resolve(strict=True)
    if not path.is_dir():
        raise ValueError(f"debuginfo path must be an existing directory: {path}")
    return str(path)


def read_xml(path, expected_root):
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"missing regular XML report: {path.name}")
    if not 0 < path.stat().st_size <= MAX_REPORT_BYTES:
        raise ValueError(f"empty or oversized XML report: {path.name}")
    root = ET.parse(path).getroot()
    if root.tag != expected_root:
        raise ValueError(f"unexpected XML root in {path.name}: {root.tag}")
    return root


class LimitedXmlReader:
    def __init__(self, stream, limit, name):
        self.stream = stream
        self.limit = limit
        self.name = name
        self.consumed = 0

    def read(self, size=-1):
        remaining = self.limit - self.consumed
        request = remaining + 1 if size < 0 else min(size, remaining + 1)
        data = self.stream.read(request)
        self.consumed += len(data)
        if self.consumed > self.limit:
            raise ValueError(f"oversized XML report while reading: {self.name}")
        return data


def memcheck_evidence(path, pid):
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"missing regular XML report: {path.name}")
    if not 0 < path.stat().st_size <= MAX_MEMCHECK_REPORT_BYTES:
        raise ValueError(f"empty or oversized XML report: {path.name}")
    identifiers = {}
    final_state = None
    fatal_signal = False
    kinds = Counter()
    suppressed = []
    depth = 0
    root = None
    with path.open("rb") as stream:
        reader = LimitedXmlReader(stream, MAX_MEMCHECK_REPORT_BYTES, path.name)
        for event, element in ET.iterparse(reader, events=("start", "end")):
            if event == "start":
                depth += 1
                if depth == 1:
                    root = element
                    if root.tag != "valgrindoutput":
                        raise ValueError(f"unexpected XML root in {path.name}: {root.tag}")
                continue
            if depth == 2:
                if element.tag in {"protocoltool", "pid"}:
                    identifiers.setdefault(element.tag, element.text or "")
                elif element.tag == "status":
                    for state in element.findall("state"):
                        final_state = state.text
                elif element.tag == "fatal_signal":
                    fatal_signal = True
                elif element.tag == "error":
                    kind = element.findtext("kind")
                    if not kind:
                        raise ValueError("Memcheck error is missing its kind")
                    kinds[kind] += 1
                elif element.tag == "suppcounts":
                    for pair in element.findall("pair"):
                        count, name = pair.findtext("count"), pair.findtext("name")
                        if count is None or not count.isdigit() or not name:
                            raise ValueError("malformed Memcheck suppression count")
                        suppressed.append({"name": name, "count": int(count)})
                # Discard processed top-level records instead of retaining their
                # stacks or ignored data. Parsing still runs to EOF.
                root.remove(element)
                element.clear()
            depth -= 1
    if identifiers.get("protocoltool") != "memcheck" or identifiers.get("pid") != str(pid):
        raise ValueError("Memcheck report tool or process ID does not match this run")
    if final_state != "FINISHED":
        raise ValueError("Memcheck report is incomplete (no final FINISHED status)")
    if fatal_signal:
        raise ValueError("Memcheck report contains a fatal signal")
    return {"error_records": dict(kinds), "suppressed": suppressed,
            "blocking_error_records": sum(v for k, v in kinds.items() if k != "Leak_StillReachable")}


def qttest_evidence(path):
    root = read_xml(path, "TestCase")
    if not root.get("name"):
        raise ValueError("QtTest report has no suite name")
    counts = Counter()
    functions = root.findall("TestFunction")
    if not functions:
        raise ValueError("QtTest report contains no test functions")
    for function, expected in ((functions[0], "initTestCase"),
                               (functions[-1], "cleanupTestCase")):
        if function.get("name") != expected:
            raise ValueError(f"QtTest report is incomplete or out of order: expected {expected}")
        if [incident.get("type") for incident in function.findall("Incident")] != ["pass"]:
            raise ValueError(f"QtTest lifecycle function {expected} did not pass")
    for function in functions:
        name = function.get("name")
        incidents = function.findall("Incident")
        if not name or not incidents:
            raise ValueError("QtTest function is missing its name or result")
        for incident in incidents:
            kind = incident.get("type")
            if kind not in {"pass", "fail", "skip", "xfail", "xpass", "bpass", "bfail"}:
                raise ValueError(f"unknown QtTest incident type: {kind}")
            if kind in {"fail", "xpass", "bpass", "bfail"}:
                counts["failed"] += 1
            elif name not in {"initTestCase", "cleanupTestCase"}:
                counts[kind] += 1
    return {"suite": root.get("name"), "passed": counts["pass"],
            "failed": counts["failed"], "skipped": counts["skip"],
            "expected_failures": counts["xfail"]}


def stop_group(process):
    # Kill descendants even if the group leader already exited. A child holding
    # logs open must not survive a failed or interrupted test run.
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        if sig == signal.SIGTERM:
            time.sleep(0.15)
    process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--valgrind", default="valgrind", help="Valgrind executable (VALGRIND_LIB is inherited)")
    parser.add_argument("--suppressions", action="append", default=[], type=Path,
                        help="explicit reviewed suppression file; repeat for additional files")
    parser.add_argument("--debuginfo-path", help="existing directory for Valgrind's extra debug-symbol lookup")
    parser.add_argument("--output", required=True, type=Path, help="new directory; existing paths are never reused")
    parser.add_argument("--timeout", type=timeout_seconds, default=600.0)
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- BINARY [TEST_FUNCTION[:DATA_TAG] ...]")
    args = parser.parse_args()
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("supply -- BINARY [TEST_FUNCTION ...]")
    output = args.output.absolute()
    try:
        output.mkdir(mode=0o700, parents=True, exist_ok=False)
    except OSError as error:
        print(f"cannot create new output directory: {error}", file=sys.stderr)
        return 1
    summary = {"status": "failed", "problems": [], "command": [],
               "timeout_seconds": args.timeout, "child_instrumentation": False,
               "debuginfo_path": None,
               "suppression_policy": {
                   "defaults": "Valgrind installation defaults; user rc/options ignored",
                   "explicit_paths": []}}
    started = time.monotonic()
    process = None
    interrupted = []
    previous_handlers = {}

    def receive_signal(signum, _frame):
        interrupted.append(signum)

    try:
        if not sys.platform.startswith("linux"):
            raise ValueError("this opt-in runner requires Linux")
        if any(not arg or arg.startswith("-") for arg in command[1:]):
            raise ValueError("only selected QtTest functions are accepted; output and other QtTest flags are managed")
        binary = Path(command[0]).resolve(strict=True)
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError("test binary must be an executable regular file")
        with binary.open("rb") as stream:
            if stream.read(4) != b"\x7fELF":
                raise ValueError("test binary must be a native ELF executable, not a script")
            with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as contents:
                if any(contents.find(marker) >= 0 for marker in (b"__asan_init", b"libasan.so", b"libclang_rt.asan")):
                    raise ValueError("ASan-instrumented binary detected; rebuild without ASan before using Memcheck")
        valgrind = shutil.which(args.valgrind)
        if not valgrind:
            raise ValueError(f"Valgrind executable unavailable: {args.valgrind}")
        valgrind = str(Path(valgrind).resolve())
        suppression_paths = []
        for requested in args.suppressions:
            path = requested.resolve(strict=True)
            if not path.is_file():
                raise ValueError(f"suppression path must be a regular file: {path}")
            suppression_paths.append(str(path))
        summary["suppression_policy"]["explicit_paths"] = suppression_paths
        debuginfo_path = resolve_debuginfo_path(args.debuginfo_path)
        summary["debuginfo_path"] = debuginfo_path
        environment = os.environ.copy()
        environment.setdefault("QT_QPA_PLATFORM", "offscreen")
        summary["environment"] = {
            name: environment.get(name)
            for name in ("QT_QPA_PLATFORM", "QT_IM_MODULE", "QSG_RHI_BACKEND",
                         "QT_QUICK_BACKEND", "LIBGL_ALWAYS_SOFTWARE", "VALGRIND_LIB",
                         "GC_TEST_TIMEOUT_SCALE", "QT_ENABLE_REGEXP_JIT")
        }
        for variable, directory in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                                    ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"),
                                    ("XDG_RUNTIME_DIR", "runtime"), ("TMPDIR", "tmp")):
            path = output / directory
            path.mkdir(mode=0o700)
            environment[variable] = str(path)
        environment["GC_QTTEST_PERSISTENT_LOG"] = str(output / "qttest-diagnostic.log")
        work = output / "work"
        work.mkdir(mode=0o700)
        invocation = [valgrind, "--command-line-only=yes", "--tool=memcheck",
                      "--leak-check=full", "--show-leak-kinds=all",
                      "--errors-for-leak-kinds=definite,indirect,possible", "--track-origins=yes",
                      "--num-callers=30", "--smc-check=all",
                      "--error-exitcode=97", "--trace-children=no", "--child-silent-after-fork=yes",
                      "--xml=yes", f"--xml-file={output / 'memcheck-%p.xml'}",
                      f"--log-file={output / 'memcheck-%p.log'}",
                      *(f"--suppressions={path}" for path in suppression_paths),
                      *([f"--extra-debuginfo-path={debuginfo_path}"] if debuginfo_path is not None else []),
                      str(binary), *command[1:],
                      "-o", f"{output / 'qttest.xml'},xml"]
        summary["command"] = invocation
        summary["binary"] = str(binary)
        summary["valgrind_lib"] = environment.get("VALGRIND_LIB")
        for sig in (signal.SIGINT, signal.SIGTERM):
            previous_handlers[sig] = signal.signal(sig, receive_signal)
        with (output / "application.log").open("wb") as application_log:
            process = subprocess.Popen(invocation, cwd=work, env=environment,
                                       stdout=application_log, stderr=subprocess.STDOUT,
                                       start_new_session=True, umask=0o077)
            summary["pid"] = process.pid
            deadline = time.monotonic() + args.timeout
            while process.poll() is None and not interrupted and time.monotonic() < deadline:
                time.sleep(min(0.05, max(0, deadline - time.monotonic())))
            if interrupted:
                summary["problems"].append(f"interrupted by signal {interrupted[0]}")
            elif process.poll() is None:
                summary["problems"].append("test run timed out")
            stop_group(process)
        summary["returncode"] = process.returncode
        if process.returncode != 0:
            summary["problems"].append(f"Valgrind/test process exited with status {process.returncode}")
        for key, reader, path in (("memcheck", lambda p: memcheck_evidence(p, process.pid),
                                  output / f"memcheck-{process.pid}.xml"),
                                 ("qttest", qttest_evidence, output / "qttest.xml")):
            try:
                summary[key] = reader(path)
            except (OSError, ValueError, ET.ParseError) as error:
                summary["problems"].append(str(error))
        if summary.get("memcheck", {}).get("blocking_error_records", 0):
            summary["problems"].append("Memcheck reported memory errors or blocking leaks")
        if "qttest" in summary:
            if not summary["qttest"]["passed"]:
                summary["problems"].append("QtTest reported zero passed test cases (lifecycle functions excluded)")
            if summary["qttest"]["failed"]:
                summary["problems"].append("QtTest reported failed test cases")
        if not summary["problems"]:
            summary["status"] = "passed"
    except (OSError, ValueError) as error:
        summary["problems"].append(str(error))
    finally:
        if process is not None and process.poll() is None:
            stop_group(process)
        if interrupted:
            problem = f"interrupted by signal {interrupted[0]}"
            if problem not in summary["problems"]:
                summary["problems"].append(problem)
            summary["status"] = "failed"
        for sig, handler in previous_handlers.items():
            signal.signal(sig, handler)
        summary["elapsed_seconds"] = round(time.monotonic() - started, 3)
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        suite = ET.Element("testsuite", name="memcheck", tests="1",
                           failures=str(int(summary["status"] != "passed")),
                           time=str(summary["elapsed_seconds"]))
        case = ET.SubElement(suite, "testcase", name=Path(command[0]).name, classname="memcheck")
        if summary["status"] != "passed":
            ET.SubElement(case, "failure", message="Memcheck gate failed").text = "\n".join(summary["problems"])
        ET.ElementTree(suite).write(output / "junit.xml", encoding="utf-8", xml_declaration=True)
    suppression_label = " with explicit suppressions" if args.suppressions else ""
    print(f"Memcheck {summary['status']}{suppression_label}: {output}")
    for problem in summary["problems"]:
        print(problem, file=sys.stderr)
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
