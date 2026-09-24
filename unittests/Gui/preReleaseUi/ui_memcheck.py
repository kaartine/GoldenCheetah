#!/usr/bin/env python3
"""Prepare and validate opt-in Memcheck evidence for the isolated UI fixture."""

import argparse
import importlib.util
import json
import mmap
import os
from pathlib import Path
import shutil
import sys
import xml.etree.ElementTree as ET


sys.dont_write_bytecode = True
RUNNER_PATH = Path(__file__).resolve().parents[3] / ".github/scripts/run-memcheck.py"
SPEC = importlib.util.spec_from_file_location("gc_memcheck_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


def prepare(image, appdir, artifacts, valgrind):
    if not sys.platform.startswith("linux"):
        raise ValueError("UI Memcheck requires Linux")
    if not appdir:
        raise ValueError("GC_UI_MEMCHECK=1 requires GC_UI_APPDIR with its extracted runtime")
    runtime = Path(appdir).resolve(strict=True)
    if not all((runtime / name).is_dir() for name in ("lib", "plugins")):
        raise ValueError("GC_UI_APPDIR must contain the extracted lib and plugins directories")
    binary = Path(image).resolve(strict=True)
    if runtime not in binary.parents or binary.name != "GoldenCheetah":
        raise ValueError("Memcheck requires the native GoldenCheetah binary inside GC_UI_APPDIR")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise ValueError("GoldenCheetah must be an executable regular file")
    with binary.open("rb") as stream:
        header = stream.read(12)
        if not header.startswith(b"\x7fELF") or header[8:11] in (b"AI\x01", b"AI\x02"):
            raise ValueError("Memcheck requires native ELF, not an AppImage launcher or script")
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as contents:
            if any(contents.find(marker) >= 0 for marker in (b"__asan_init", b"libasan.so", b"libclang_rt.asan")):
                raise ValueError("ASan-instrumented binary detected; rebuild without ASan")
    executable = shutil.which(valgrind)
    if not executable:
        raise ValueError(f"Valgrind executable unavailable: {valgrind}")
    output = Path(artifacts).resolve() / "memcheck"
    output.mkdir(mode=0o700, exist_ok=False)
    prefix = [str(Path(executable).resolve()), "--command-line-only=yes", "--tool=memcheck",
              "--leak-check=full", "--show-leak-kinds=all",
              "--errors-for-leak-kinds=definite,indirect,possible", "--track-origins=yes",
              "--num-callers=30", "--error-exitcode=97", "--trace-children=no",
              "--child-silent-after-fork=yes", "--xml=yes",
              f"--xml-file={output / 'memcheck-%p.xml'}",
              f"--log-file={output / 'memcheck-%p.log'}"]
    (output / "command-prefix.nul").write_bytes(b"\0".join(os.fsencode(arg) for arg in prefix) + b"\0")
    (output / "invocation.json").write_text(json.dumps({
        "binary": str(binary), "appdir": str(runtime), "prefix": prefix,
        "child_instrumentation": False,
        "suppression_policy": "Valgrind installation defaults only; user rc/options ignored",
    }, indent=2) + "\n", encoding="utf-8")
    return prefix


def ui_evidence(path):
    root = RUNNER.read_xml(path, "testsuite")
    cases = root.findall("testcase")
    if not cases or root.get("tests") != str(len(cases)):
        raise ValueError("UI JUnit must contain a nonempty matching test count")
    if any(root.get(key, "0") != "0" for key in ("failures", "errors", "skipped")):
        raise ValueError("UI JUnit contains failed, errored, or skipped tests")
    names = [case.get("name") for case in cases]
    if any(not name for name in names) or len(names) != len(set(names)):
        raise ValueError("UI JUnit contains missing or duplicate test names")
    if "graceful_shutdown_request" not in names:
        raise ValueError("UI JUnit is missing graceful_shutdown_request")
    if any(case.find(kind) is not None for case in cases for kind in ("failure", "error", "skipped")):
        raise ValueError("UI JUnit contains a failed, errored, or skipped result")
    return {"passed": len(cases), "tests": names}


def validate(artifacts, pid, app_status, ui_status):
    artifacts = Path(artifacts)
    output = artifacts / "memcheck"
    summary = {"status": "failed", "pid": pid, "app_returncode": app_status,
               "ui_returncode": ui_status, "problems": [], "environment": {
                   name: os.environ.get(name) for name in (
                       "QT_QPA_PLATFORM", "QT_IM_MODULE", "QSG_RHI_BACKEND",
                       "QT_QUICK_BACKEND", "LIBGL_ALWAYS_SOFTWARE", "VALGRIND_LIB",
                       "GC_UI_TIMEOUT_SCALE")}}
    if app_status != 0:
        summary["problems"].append(f"application exited with status {app_status}")
    if ui_status != 0:
        summary["problems"].append(f"UI workflow failed or timed out with status {ui_status}")
    for key, reader, path in (("memcheck", lambda p: RUNNER.memcheck_evidence(p, pid),
                              output / f"memcheck-{pid}.xml"),
                             ("ui", ui_evidence, artifacts / "junit.xml")):
        try:
            summary[key] = reader(path)
        except (OSError, ValueError, ET.ParseError) as error:
            summary["problems"].append(str(error))
    if summary.get("memcheck", {}).get("blocking_error_records", 0):
        summary["problems"].append("Memcheck reported memory errors or blocking leaks")
    if not summary["problems"]:
        summary["status"] = "passed"
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    suite = ET.Element("testsuite", name="ui-memcheck", tests="1",
                       failures=str(int(summary["status"] != "passed")))
    case = ET.SubElement(suite, "testcase", name="native_application_memcheck")
    if summary["problems"]:
        ET.SubElement(case, "failure", message="UI Memcheck gate failed").text = "\n".join(summary["problems"])
    ET.ElementTree(suite).write(output / "junit.xml", encoding="utf-8", xml_declaration=True)
    for problem in summary["problems"]:
        print(problem, file=sys.stderr)
    return 0 if summary["status"] == "passed" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    launch = commands.add_parser("prepare")
    launch.add_argument("--image", required=True, type=Path)
    launch.add_argument("--appdir", required=True)
    launch.add_argument("--artifacts", required=True, type=Path)
    launch.add_argument("--valgrind", default="valgrind")
    finish = commands.add_parser("validate")
    finish.add_argument("--artifacts", required=True, type=Path)
    finish.add_argument("--pid", required=True, type=int)
    finish.add_argument("--app-status", required=True, type=int)
    finish.add_argument("--ui-status", required=True, type=int)
    args = parser.parse_args()
    try:
        if args.action == "prepare":
            prepare(args.image, args.appdir, args.artifacts, args.valgrind)
            return 0
        return validate(args.artifacts, args.pid, args.app_status, args.ui_status)
    except (OSError, ValueError) as error:
        print(f"UI Memcheck: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
