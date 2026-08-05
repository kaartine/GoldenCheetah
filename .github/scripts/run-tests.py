#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


CHECK_TARGET = re.compile(r"^check\s*:", re.MULTILINE)
QTTEST_TOTAL = re.compile(r"Totals:\s+([0-9]+)\s+passed")


def fail(message: str, status: int = 1) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(status)


def count_registered_tests(root: Path) -> int:
    unit_tests = root / "unittests"
    if not unit_tests.is_dir():
        fail("unit-test directory is unavailable")

    registered = 0
    for makefile in unit_tests.rglob("Makefile"):
        if makefile.parent == unit_tests:
            continue
        contents = makefile.read_text(encoding="utf-8", errors="replace")
        if CHECK_TARGET.search(contents):
            registered += 1
    return registered


def run_tests(command: list[str], root: Path) -> tuple[int, int]:
    environment = os.environ.copy()
    environment.setdefault("QT_QPA_PLATFORM", "offscreen")

    suites = 0
    cases = 0
    try:
        process = subprocess.Popen(
            command,
            cwd=root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
    except OSError as error:
        fail(f"cannot start unit-test command: {error}", 127)

    assert process.stdout is not None
    for line in process.stdout:
        print(line, end="", flush=True)
        match = QTTEST_TOTAL.search(line)
        if match:
            suites += 1
            cases += int(match.group(1))

    returncode = process.wait()
    if returncode != 0:
        raise SystemExit(returncode if returncode > 0 else 128 - returncode)
    return suites, cases


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[2]
    default_tool = "jom" if os.name == "nt" else "make"

    parser = argparse.ArgumentParser(description="Run generated GoldenCheetah tests")
    parser.add_argument("--root", type=Path, default=repository)
    parser.add_argument("--build-tool", default=default_tool)
    parser.add_argument("--build-tool-arg", action="append", default=[])
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    root = arguments.root.resolve()

    registered = count_registered_tests(root)
    if registered == 0:
        fail("no unit-test targets were generated")
    print(f"Discovered {registered} generated unit-test targets.", file=sys.stderr)

    command = [
        arguments.build_tool,
        *arguments.build_tool_arg,
        "-j1",
        "check",
    ]
    suites, cases = run_tests(command, root)
    if suites == 0:
        fail("test command completed without a QtTest result")
    if cases == 0:
        fail("test command reported zero executed test cases")
    print(
        f"Executed {cases} QtTest cases across {suites} suites.",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
