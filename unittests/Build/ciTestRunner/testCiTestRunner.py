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


def make_fixture(
    root: Path,
    required=("all qt Fake/Test",),
    enabled=("Fake/Test",),
    discovered=(),
    build_root: Path | None = None,
) -> Path:
    source_unit_tests = root / "unittests"
    generated_unit_tests = (build_root or root) / "unittests"
    source_unit_tests.mkdir(parents=True)
    generated_unit_tests.mkdir(parents=True, exist_ok=True)
    (source_unit_tests / "ci-required-tests.txt").write_text(
        "\n".join(required) + "\n", encoding="ascii"
    )
    (generated_unit_tests / "ci-enabled-tests.txt").write_text(
        "\n".join(enabled) + "\n", encoding="ascii"
    )
    project_kinds = {}
    for entry in required:
        _, kind, project = entry.split()
        project_kinds[project] = kind
    for kind, project in discovered:
        project_kinds[project] = kind
    for project, kind in project_kinds.items():
        test_directory = source_unit_tests / project
        test_directory.mkdir(parents=True)
        if kind == "qt":
            project_file = "TEMPLATE = app\nCONFIG += testcase\n"
        else:
            project_file = (
                "TEMPLATE = aux\n"
                "check.commands = python3 test.py\n"
                "QMAKE_EXTRA_TARGETS += check\n"
            )
        test_directory.joinpath(project.split("/")[-1] + ".pro").write_text(
            project_file, encoding="ascii"
        )
    for project in enabled:
        test_directory = generated_unit_tests / project
        test_directory.mkdir(parents=True, exist_ok=True)
        (test_directory / "Makefile").write_text("check:\n", encoding="utf-8")
    return root


def run_runner(
    root: Path,
    fake_build_tool: Path,
    mode: str,
    build_root: Path | None = None,
    platform: str = "linux",
    environment_updates: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["GC_FAKE_TEST_RESULT"] = mode
    if environment_updates:
        environment.update(environment_updates)
    command = [
            sys.executable,
            str(RUNNER),
            "--root",
            str(root),
            "--build-tool",
            sys.executable,
            "--build-tool-arg",
            str(fake_build_tool),
            "--platform",
            platform,
        ]
    if build_root is not None:
        command.extend(("--build-root", str(build_root)))
    return subprocess.run(
        command,
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
    require_literal(
        "jom -j4 sub-unittests",
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

    unit_test_project = REPOSITORY / "unittests" / "unittests.pro"
    require_literal("Build/ciTestRunner", unit_test_project)
    if "CONFIG += ordered" in unit_test_project.read_text(encoding="utf-8"):
        raise AssertionError("unit-test subprojects must build in parallel")
    require_literal(
        "linux:SUBDIRS += Build/appImagePackaging",
        REPOSITORY / "unittests" / "unittests.pro",
    )
    require_literal(
        "Core/linkedActivitySaveCleanup",
        REPOSITORY / "unittests" / "unittests.pro",
    )
    require_literal(
        "all qt Core/linkedActivitySaveCleanup",
        REPOSITORY / "unittests" / "ci-required-tests.txt",
    )
    linux_workflow = REPOSITORY / ".github" / "workflows" / "ridecache-removal-native.yml"
    require_literal("mkdir build-linked-save-cleanup", linux_workflow)
    require_literal(
        "build-linked-save-cleanup/tst_linkedActivitySaveCleanup",
        linux_workflow,
    )

    with tempfile.TemporaryDirectory(prefix="gc-ci-test-runner-") as temporary:
        temporary_path = Path(temporary)
        fake_build_tool = temporary_path / "fake_build_tool.py"
        fake_build_tool.write_text(
            """import os
from pathlib import Path
import sys

mode = os.environ["GC_FAKE_TEST_RESULT"]
expected_tmpdir = os.environ.get("GC_FAKE_EXPECT_TMPDIR")
if expected_tmpdir and os.environ.get("TMPDIR") != expected_tmpdir:
    print("unexpected TMPDIR: " + os.environ.get("TMPDIR", ""), file=sys.stderr)
    sys.exit(8)
diagnostic = os.environ.get("GC_QTTEST_PERSISTENT_LOG")
is_auxiliary = os.path.basename(os.getcwd()) == "Aux"
if is_auxiliary and diagnostic:
    sys.exit(9)
if not is_auxiliary and not diagnostic:
    sys.exit(10)
if mode == "failure":
    Path(diagnostic).write_text(
        "persisted QtTest diagnostic\\n", encoding="utf-8"
    )
    sys.exit(7)
if is_auxiliary:
    sys.exit(0)
if mode == "zero":
    print("Totals: 0 passed, 0 failed, 0 skipped, 0 blacklisted, 0ms")
elif mode == "success":
    print("Totals: 2 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms")
""",
            encoding="utf-8",
        )

        missing = make_fixture(
            temporary_path / "missing",
            required=("all qt Fake/Test", "all qt Fake/Missing"),
            enabled=("Fake/Test",),
        )
        no_targets = run_runner(missing, fake_build_tool, "success")
        expect_failure(no_targets, "eligible project missing from qmake output")
        if "eligible test projects were not generated" not in no_targets.stderr:
            raise AssertionError(f"missing inventory diagnostic: {no_targets.stderr}")

        registered = make_fixture(temporary_path / "registered")
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
        if "persisted QtTest diagnostic" not in build_failure.stdout:
            raise AssertionError(
                "runner did not emit the persisted failure diagnostic"
            )

        success = run_runner(registered, fake_build_tool, "success")
        if success.returncode != 0:
            raise AssertionError(
                f"expected runner success, got {success.returncode}: {success.stderr}"
            )
        if "Executed 2 QtTest cases across 1 suites." not in success.stderr:
            raise AssertionError(f"missing execution summary: {success.stderr}")

        canonical_temp = temporary_path / "canonical-temp"
        canonical_temp.mkdir()
        temp_alias = temporary_path / "temp-alias"
        temp_alias.symlink_to(canonical_temp, target_is_directory=True)
        macos_temp = run_runner(
            registered,
            fake_build_tool,
            "success",
            platform="macos",
            environment_updates={
                "TMPDIR": str(temp_alias),
                "GC_FAKE_EXPECT_TMPDIR": str(canonical_temp.resolve()),
            },
        )
        if macos_temp.returncode != 0:
            raise AssertionError(
                "macOS TMPDIR was not canonicalized: "
                + macos_temp.stderr
            )

        shadow_source = temporary_path / "shadow-source"
        shadow_build = temporary_path / "shadow-build"
        make_fixture(shadow_source, build_root=shadow_build)
        shadow_success = run_runner(
            shadow_source,
            fake_build_tool,
            "success",
            build_root=shadow_build,
        )
        if shadow_success.returncode != 0:
            raise AssertionError(
                "shadow-build test output was not used: " + shadow_success.stderr
            )
        if (shadow_source / "unittests" / "ci-enabled-tests.txt").exists():
            raise AssertionError("shadow-build fixture polluted the source tree")

        with_aux = make_fixture(
            temporary_path / "with-aux",
            required=("all qt Fake/Test", "all aux Fake/Aux"),
            enabled=("Fake/Test", "Fake/Aux"),
        )
        aux_success = run_runner(with_aux, fake_build_tool, "success")
        if aux_success.returncode != 0:
            raise AssertionError(
                f"auxiliary test project was not reconciled: {aux_success.stderr}"
            )

        nested = make_fixture(
            temporary_path / "nested",
            required=("all qt Fake/Nested/Test",),
            enabled=("Fake/Nested/Test",),
        )
        nested_success = run_runner(nested, fake_build_tool, "success")
        if nested_success.returncode != 0:
            raise AssertionError(
                f"nested test project was not reconciled: {nested_success.stderr}"
            )

        excluded = make_fixture(
            temporary_path / "excluded",
            required=("all qt Fake/Test", "windows qt Fake/WindowsOnly"),
            enabled=("Fake/Test",),
        )
        excluded_success = run_runner(excluded, fake_build_tool, "success")
        if excluded_success.returncode != 0:
            raise AssertionError(
                "explicit platform exclusion was not honored: "
                + excluded_success.stderr
            )

        omitted = make_fixture(
            temporary_path / "omitted",
            required=("all qt Fake/Test",),
            enabled=("Fake/Test",),
            discovered=(("qt", "Fake/Omitted"),),
        )
        result = run_runner(omitted, fake_build_tool, "success")
        expect_failure(result, "discovered project absent from inventory")
        if "discovered test projects are absent from inventory" not in result.stderr:
            raise AssertionError(f"missing discovery diagnostic: {result.stderr}")

        nested_omitted = make_fixture(
            temporary_path / "nested-omitted",
            required=("all qt Fake/Test",),
            enabled=("Fake/Test",),
            discovered=(("qt", "Fake/Nested/Omitted"),),
        )
        result = run_runner(nested_omitted, fake_build_tool, "success")
        expect_failure(result, "nested discovered project absent from inventory")
        if "discovered test projects are absent from inventory" not in result.stderr:
            raise AssertionError(f"missing nested discovery diagnostic: {result.stderr}")

        wrong_kind = make_fixture(
            temporary_path / "wrong-kind",
            required=("all aux Fake/Test",),
            enabled=("Fake/Test",),
            discovered=(("qt", "Fake/Test"),),
        )
        result = run_runner(wrong_kind, fake_build_tool, "success")
        expect_failure(result, "inventory kind mismatch")
        if "unit-test inventory kind mismatch" not in result.stderr:
            raise AssertionError(f"missing kind diagnostic: {result.stderr}")

        unexpected = make_fixture(
            temporary_path / "unexpected",
            required=("all qt Fake/Test",),
            enabled=("Fake/Test", "Fake/Unreviewed"),
            discovered=(("qt", "Fake/Unreviewed"),),
        )
        result = run_runner(unexpected, fake_build_tool, "success")
        expect_failure(result, "generated project absent from inventory")
        if "discovered test projects are absent from inventory" not in result.stderr:
            raise AssertionError(f"missing extra-project diagnostic: {result.stderr}")


if __name__ == "__main__":
    main()
