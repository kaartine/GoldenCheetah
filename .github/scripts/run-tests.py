#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


QTTEST_TOTAL = re.compile(r"Totals:\s+([0-9]+)\s+passed")
PROJECT_RE = re.compile(r"^[A-Za-z0-9_-]+(?:/[A-Za-z0-9_-]+)+$")
TESTCASE_CONFIG = re.compile(
    r"^\s*CONFIG\s*\+=\s*[^#\n]*\btestcase\b", re.MULTILINE
)
COMMON_TEST_CONFIG = re.compile(
    r"^\s*include\s*\(\s*\.\./\.\./unittests\.pri\s*\)", re.MULTILINE
)
AUX_TEMPLATE = re.compile(
    r"^\s*TEMPLATE\s*=\s*aux(?:\s|$)", re.MULTILINE
)
AUX_CHECK_TARGET = re.compile(
    r"^\s*QMAKE_EXTRA_TARGETS\s*\+=\s*[^#\n]*\bcheck\b", re.MULTILINE
)
PLATFORM_SELECTORS = {"all", "linux", "macos", "windows", "nonwindows"}


def fail(message: str, status: int = 1) -> None:
    print(message, file=sys.stderr)
    raise SystemExit(status)


def read_inventory(unit_tests: Path) -> dict[str, tuple[str, str]]:
    inventory_path = unit_tests / "ci-required-tests.txt"
    if inventory_path.is_symlink() or not inventory_path.is_file():
        fail("unit-test inventory is unavailable")
    inventory = {}
    for line_number, raw in enumerate(
        inventory_path.read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if (
            len(fields) != 3
            or fields[0] not in PLATFORM_SELECTORS
            or fields[1] not in {"qt", "aux"}
            or not PROJECT_RE.fullmatch(fields[2])
        ):
            fail(f"invalid unit-test inventory line {line_number}")
        if fields[2] in inventory:
            fail(f"duplicate unit-test inventory project: {fields[2]}")
        inventory[fields[2]] = (fields[0], fields[1])
    if not inventory:
        fail("unit-test inventory is empty")
    return inventory


def discover_projects(unit_tests: Path) -> dict[str, str]:
    project_files = {}
    for project_file in sorted(unit_tests.rglob("*.pro")):
        directory = project_file.parent
        project = directory.relative_to(unit_tests).as_posix()
        if not PROJECT_RE.fullmatch(project):
            continue
        current = directory
        while current != unit_tests:
            if current.is_symlink() or not current.is_dir():
                fail(f"unit-test project directory is unsafe: {current}")
            current = current.parent
        if project_file.is_symlink() or not project_file.is_file():
            fail(f"unit-test project file is unsafe: {project_file}")
        project_files.setdefault(project, []).append(project_file)

    discovered = {}
    for project in sorted(project_files):
        contents = [
            project_file.read_text(encoding="utf-8")
            for project_file in project_files[project]
        ]
        qt_test = any(
            TESTCASE_CONFIG.search(text) or COMMON_TEST_CONFIG.search(text)
            for text in contents
        )
        auxiliary = any(
            AUX_TEMPLATE.search(text) and AUX_CHECK_TARGET.search(text)
            for text in contents
        )
        if qt_test and auxiliary:
            fail(f"unit-test project has ambiguous kind: {project}")
        if qt_test or auxiliary:
            discovered[project] = "qt" if qt_test else "aux"
    if not discovered:
        fail("no testcase or auxiliary projects were discovered")
    return discovered


def selector_matches(selector: str, platform: str) -> bool:
    return (
        selector == "all"
        or selector == platform
        or (selector == "nonwindows" and platform != "windows")
    )


def read_enabled(unit_tests: Path) -> set[str]:
    enabled_path = unit_tests / "ci-enabled-tests.txt"
    if enabled_path.is_symlink() or not enabled_path.is_file():
        fail("qmake unit-test project manifest is unavailable")
    projects = []
    for raw in enabled_path.read_text(encoding="ascii").splitlines():
        project = raw.strip()
        if project and not PROJECT_RE.fullmatch(project):
            fail(f"qmake generated an invalid unit-test project: {project!r}")
        if project:
            projects.append(project)
    if len(projects) != len(set(projects)):
        fail("qmake unit-test project manifest contains duplicates")
    return set(projects)


def run_tests(
    command: list[str],
    working_directory: Path,
    platform: str,
    persistent_output: bool,
) -> tuple[int, int]:
    environment = os.environ.copy()
    environment.setdefault("QT_QPA_PLATFORM", "offscreen")
    if platform == "macos":
        temporary = Path(environment.get("TMPDIR", "/private/tmp"))
        try:
            canonical_temporary = temporary.resolve(strict=True)
        except OSError as error:
            fail(f"cannot resolve macOS temporary directory: {error}")
        if not canonical_temporary.is_dir():
            fail("macOS temporary directory is not a directory")
        environment["TMPDIR"] = str(canonical_temporary)
    diagnostic_directory = (
        tempfile.TemporaryDirectory(prefix="gc-qtest-output-")
        if persistent_output
        else None
    )
    diagnostic_path = (
        Path(diagnostic_directory.name) / "result.txt"
        if diagnostic_directory is not None
        else None
    )
    if diagnostic_path is not None:
        environment["GC_QTTEST_PERSISTENT_LOG"] = str(diagnostic_path)

    suites = 0
    cases = 0
    try:
        try:
            process = subprocess.Popen(
                command,
                cwd=working_directory,
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
        if diagnostic_path is not None and diagnostic_path.exists():
            if (
                diagnostic_path.is_symlink()
                or not diagnostic_path.is_file()
                or diagnostic_path.stat().st_size > 16 * 1024 * 1024
            ):
                fail("persisted QtTest diagnostic is unsafe")
            diagnostic = diagnostic_path.read_text(
                encoding="utf-8", errors="replace"
            )
            print(diagnostic, end="", flush=True)
            for line in diagnostic.splitlines():
                match = QTTEST_TOTAL.search(line)
                if match:
                    suites += 1
                    cases += int(match.group(1))

        if returncode != 0:
            raise SystemExit(
                returncode if returncode > 0 else 128 - returncode
            )
        return suites, cases
    finally:
        if diagnostic_directory is not None:
            diagnostic_directory.cleanup()


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[2]
    default_tool = "jom" if os.name == "nt" else "make"

    parser = argparse.ArgumentParser(description="Run generated GoldenCheetah tests")
    parser.add_argument("--root", type=Path, default=repository)
    parser.add_argument(
        "--build-root",
        type=Path,
        help="qmake output root; defaults to the source root",
    )
    parser.add_argument("--build-tool", default=default_tool)
    parser.add_argument("--build-tool-arg", action="append", default=[])
    parser.add_argument(
        "--platform",
        choices=("linux", "macos", "windows"),
        default="windows" if os.name == "nt" else ("macos" if sys.platform == "darwin" else "linux"),
    )
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    source_root = arguments.root.resolve()
    build_root = (
        arguments.build_root.resolve()
        if arguments.build_root is not None
        else source_root
    )

    source_unit_tests = source_root / "unittests"
    build_unit_tests = build_root / "unittests"
    if not source_unit_tests.is_dir():
        fail("unit-test directory is unavailable")
    if not build_unit_tests.is_dir():
        fail("generated unit-test directory is unavailable")
    inventory = read_inventory(source_unit_tests)
    discovered = discover_projects(source_unit_tests)
    absent = sorted(set(discovered) - set(inventory))
    stale = sorted(set(inventory) - set(discovered))
    if absent:
        fail(
            "discovered test projects are absent from inventory: "
            + ", ".join(absent)
        )
    if stale:
        fail("unit-test inventory has no discovered project: " + ", ".join(stale))
    mismatched = sorted(
        project
        for project, kind in discovered.items()
        if inventory[project][1] != kind
    )
    if mismatched:
        fail("unit-test inventory kind mismatch: " + ", ".join(mismatched))
    eligible = {
        project: kind
        for project, (selector, kind) in inventory.items()
        if selector_matches(selector, arguments.platform)
    }
    if not eligible:
        fail("unit-test inventory has no eligible projects")
    enabled = read_enabled(build_unit_tests)
    missing = sorted(set(eligible) - enabled)
    unexpected = sorted(enabled - set(eligible))
    if missing:
        fail("eligible test projects were not generated: " + ", ".join(missing))
    if unexpected:
        fail("generated test projects are not in inventory: " + ", ".join(unexpected))
    print(f"Reconciled {len(eligible)} eligible unit-test projects.", file=sys.stderr)

    command = [
        arguments.build_tool,
        *arguments.build_tool_arg,
        "-j1",
        "check",
    ]
    suites = 0
    cases = 0
    for project, kind in eligible.items():
        project_directory = build_unit_tests / project
        makefile = project_directory / "Makefile"
        if makefile.is_symlink() or not makefile.is_file():
            fail(f"eligible test project has no generated Makefile: {project}")
        project_suites, project_cases = run_tests(
            command,
            project_directory,
            arguments.platform,
            kind == "qt",
        )
        if kind == "qt" and project_suites == 0:
            fail(f"test project completed without a QtTest result: {project}")
        if kind == "qt" and project_cases == 0:
            fail(f"test project reported zero executed test cases: {project}")
        suites += project_suites
        cases += project_cases
    print(
        f"Executed {cases} QtTest cases across {suites} suites.",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
