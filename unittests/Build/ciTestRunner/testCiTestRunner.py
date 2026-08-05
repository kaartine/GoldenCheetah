#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile


TEST_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY = TEST_DIRECTORY.parents[2]
RUNNER = REPOSITORY / ".github" / "scripts" / "run-tests.py"


def require_literal(text: str, path: Path) -> None:
    if text not in path.read_text(encoding="utf-8"):
        raise AssertionError(f"missing CI test wiring {text!r} in {path}")


def make_fixture(root: Path, registered: bool) -> Path:
    (root / "unittests").mkdir(parents=True)
    if registered:
        test_directory = root / "unittests" / "Fake" / "Test"
        test_directory.mkdir(parents=True)
        (test_directory / "Makefile").write_text("check:\n", encoding="utf-8")
    return root


def run_runner(root: Path, fake_build_tool: Path, mode: str) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["GC_FAKE_TEST_RESULT"] = mode
    return subprocess.run(
        [
            sys.executable,
            str(RUNNER),
            "--root",
            str(root),
            "--build-tool",
            sys.executable,
            "--build-tool-arg",
            str(fake_build_tool),
        ],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )


def expect_failure(result: subprocess.CompletedProcess[str], description: str) -> None:
    if result.returncode == 0:
        raise AssertionError(f"expected runner failure for {description}")


def main() -> None:
    require_literal(
        "cp unittests/unittests.pri.in unittests/unittests.pri",
        REPOSITORY / ".github" / "scripts" / "build.sh",
    )
    require_literal(
        "make -j2 sub-unittests",
        REPOSITORY / ".github" / "scripts" / "build.sh",
    )
    require_literal(
        "python3 ./.github/scripts/run-tests.py",
        REPOSITORY / ".github" / "workflows" / "ci.yml",
    )

    appveyor = REPOSITORY / "appveyor.yml"
    for wiring in (
        r"copy /Y unittests\unittests.pri.in unittests\unittests.pri",
        "cp unittests/unittests.pri.in unittests/unittests.pri",
        "jom -j2 sub-unittests",
        "make -j2 sub-unittests",
        r"python .github\scripts\run-tests.py --build-tool jom",
        "python3 .github/scripts/run-tests.py --build-tool make",
    ):
        require_literal(wiring, appveyor)

    require_literal(
        "Build/ciTestRunner", REPOSITORY / "unittests" / "unittests.pro"
    )
    require_literal(
        "linux:SUBDIRS += Build/appImagePackaging",
        REPOSITORY / "unittests" / "unittests.pro",
    )

    with tempfile.TemporaryDirectory(prefix="gc-ci-test-runner-") as temporary:
        temporary_path = Path(temporary)
        fake_build_tool = temporary_path / "fake_build_tool.py"
        fake_build_tool.write_text(
            """import os
import sys

mode = os.environ["GC_FAKE_TEST_RESULT"]
if mode == "failure":
    sys.exit(7)
if mode == "zero":
    print("Totals: 0 passed, 0 failed, 0 skipped, 0 blacklisted, 0ms")
elif mode == "success":
    print("Totals: 2 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms")
""",
            encoding="utf-8",
        )

        missing = make_fixture(temporary_path / "missing", registered=False)
        no_targets = run_runner(missing, fake_build_tool, "success")
        expect_failure(no_targets, "no targets")
        if "no unit-test targets were generated" not in no_targets.stderr:
            raise AssertionError(f"missing no-target diagnostic: {no_targets.stderr}")

        registered = make_fixture(temporary_path / "registered", registered=True)
        expect_failure(run_runner(registered, fake_build_tool, "none"), "no results")
        zero_cases = run_runner(registered, fake_build_tool, "zero")
        expect_failure(zero_cases, "zero cases")
        if "zero executed test cases" not in zero_cases.stderr:
            raise AssertionError(f"missing zero-case diagnostic: {zero_cases.stderr}")

        build_failure = run_runner(registered, fake_build_tool, "failure")
        if build_failure.returncode != 7:
            raise AssertionError(
                f"expected build-tool status 7, got {build_failure.returncode}"
            )

        success = run_runner(registered, fake_build_tool, "success")
        if success.returncode != 0:
            raise AssertionError(
                f"expected runner success, got {success.returncode}: {success.stderr}"
            )
        if "Executed 2 QtTest cases across 1 suites." not in success.stderr:
            raise AssertionError(f"missing execution summary: {success.stderr}")


if __name__ == "__main__":
    main()
