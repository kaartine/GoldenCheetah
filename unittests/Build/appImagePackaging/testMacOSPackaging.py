#!/usr/bin/env python3

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
VALIDATOR = REPOSITORY_ROOT / "appveyor" / "macos" / "validate-payload.py"
HOMEBREW_INSTALLER = REPOSITORY_ROOT / ".github" / "scripts" / "install-pinned-homebrew.sh"
MACOS_BUILD = REPOSITORY_ROOT / ".github" / "scripts" / "build.sh"
MACOS_REGRESSION_RUNNER = (
    REPOSITORY_ROOT / "appveyor" / "macos" / "run-build-regressions.sh"
)
CI_RELEASE_GATES = (
    REPOSITORY_ROOT
    / "unittests"
    / "Build"
    / "appImagePackaging"
    / "testCiReleaseGates.sh"
)
GITHUB_PACKAGER = REPOSITORY_ROOT / ".github" / "scripts" / "after_build.sh"
CORE_REVISION = "1" * 40
BREW_REVISION = "67658c8cf6ee685420c531ed94ed46b6e7ba5b2a"
PINNED_CORE_REVISION = "2602f7f80784581466deb491f09bf734174ac772"


class MacOSPayloadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.bundle = self.root / "GoldenCheetah.app"
        (self.bundle / "Contents" / "MacOS").mkdir(parents=True)
        (self.bundle / "Contents" / "MacOS" / "GoldenCheetah").write_bytes(
            b"relocatable fixture"
        )
        framework = self.bundle / "Contents" / "Frameworks" / "Fixture.framework"
        (framework / "Versions" / "A").mkdir(parents=True)
        (framework / "Versions" / "A" / "Fixture").write_bytes(b"framework")
        (framework / "Versions" / "Current").symlink_to("A")
        self.receipt = self.root / "INSTALL_RECEIPT.json"
        self.receipt.write_text(
            json.dumps(
                {
                    "source": {"tap_git_head": CORE_REVISION},
                    "poured_from_bottle": True,
                }
            ),
            encoding="utf-8",
        )

    def tearDown(self):
        self.temporary.cleanup()

    def run_validator(self, output, *extra):
        command = [
            sys.executable,
            str(VALIDATOR),
            "--bundle",
            str(self.bundle),
            "--output",
            str(output),
            "--homebrew-core-commit",
            CORE_REVISION,
            "--qt-version",
            "6.5.3",
            "--formula",
            f"fixture=1.2.3={self.receipt}",
        ]
        command.extend(extra)
        return subprocess.run(command, capture_output=True, text=True)

    def test_manifest_is_deterministic_and_hashes_final_payload(self):
        first = self.root / "first.json"
        second = self.root / "second.json"
        first_result = self.run_validator(first)
        self.assertEqual(first_result.returncode, 0, first_result.stderr)
        second_result = self.run_validator(second)
        self.assertEqual(second_result.returncode, 0, second_result.stderr)
        self.assertEqual(first.read_bytes(), second.read_bytes())

        document = json.loads(first.read_text(encoding="utf-8"))
        payload = document["payload"]
        executable = next(
            entry
            for entry in payload
            if entry["path"] == "Contents/MacOS/GoldenCheetah"
        )
        self.assertEqual(
            executable["sha256"], hashlib.sha256(b"relocatable fixture").hexdigest()
        )
        formula = next(
            component
            for component in document["components"]
            if component["name"] == "fixture"
        )
        self.assertEqual(formula["version"], "1.2.3")
        self.assertEqual(formula["license"], "NOASSERTION")

    def test_forbidden_build_path_in_payload_is_rejected(self):
        forbidden = "/Users/appveyor/build-agent/work"
        (self.bundle / "Contents" / "MacOS" / "GoldenCheetah").write_bytes(
            b"prefix=" + forbidden.encode("utf-8")
        )
        result = self.run_validator(
            self.root / "forbidden.json", "--forbidden-prefix", forbidden
        )
        self.assertNotEqual(result.returncode, 0)

    def test_symlink_outside_bundle_is_rejected(self):
        outside = self.root / "outside"
        outside.write_text("outside", encoding="ascii")
        (self.bundle / "Contents" / "Frameworks" / "escape").symlink_to(outside)
        result = self.run_validator(self.root / "escape.json")
        self.assertNotEqual(result.returncode, 0)

    def test_formula_receipt_must_match_pinned_core(self):
        self.receipt.write_text(
            json.dumps({"source": {"tap_git_head": "2" * 40}}),
            encoding="utf-8",
        )
        result = self.run_validator(self.root / "bad-receipt.json")
        self.assertNotEqual(result.returncode, 0)

    def test_pinned_homebrew_reinstalls_a_matching_but_tampered_keg(self):
        fake_bin = self.root / "fake-bin"
        fake_bin.mkdir()
        repository = self.root / "brew"
        core_repository = repository / "Library/Taps/homebrew/homebrew-core"
        (repository / ".git").mkdir(parents=True)
        (core_repository / ".git").mkdir(parents=True)
        keg = self.root / "keg"
        (keg / "bin").mkdir(parents=True)
        tool = keg / "bin" / "fixture"
        tool.write_text("tampered", encoding="ascii")
        receipt = keg / "INSTALL_RECEIPT.json"
        receipt.write_text(
            json.dumps({"source": {"tap_git_head": PINNED_CORE_REVISION}}),
            encoding="utf-8",
        )
        log = self.root / "brew.log"

        fake_bin.joinpath("brew").write_text(
            """#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

root = Path(os.environ["GC_FAKE_BREW_ROOT"])
args = sys.argv[1:]
if args == ["--repository"]:
    print(root / "brew")
elif args == ["list", "--versions", "--formula", "fixture"]:
    print("fixture 1.2.3")
elif args == ["--prefix", "fixture"]:
    print(root / "keg")
elif args[:2] in (["reinstall", "--formula"], ["install", "--formula"]):
    (root / "keg/bin/fixture").write_text("trusted", encoding="ascii")
    (root / "keg/INSTALL_RECEIPT.json").write_text(
        json.dumps({"source": {"tap_git_head": os.environ["HOMEBREW_CORE_COMMIT"]}}),
        encoding="utf-8",
    )
    with (root / "brew.log").open("a", encoding="ascii") as stream:
        stream.write(args[0] + "\\n")
else:
    raise SystemExit("unsupported fake brew command: " + repr(args))
""",
            encoding="utf-8",
        )
        fake_bin.joinpath("git").write_text(
            """#!/usr/bin/env python3
import os
import sys

args = sys.argv[1:]
if "rev-parse" in args:
    repository = args[args.index("-C") + 1]
    if repository.endswith("homebrew-core"):
        print(os.environ["HOMEBREW_CORE_COMMIT"])
    else:
        print(os.environ["HOMEBREW_BREW_COMMIT"])
""",
            encoding="utf-8",
        )
        for executable in (fake_bin / "brew", fake_bin / "git"):
            executable.chmod(0o755)

        environment = os.environ.copy()
        environment.update(
            {
                "PATH": f"{fake_bin}:/usr/bin:/bin",
                "GC_FAKE_BREW_ROOT": str(self.root),
                "HOMEBREW_BREW_COMMIT": BREW_REVISION,
                "HOMEBREW_CORE_COMMIT": PINNED_CORE_REVISION,
            }
        )
        result = subprocess.run(
            ["bash", str(HOMEBREW_INSTALLER), "fixture=1.2.3"],
            capture_output=True,
            text=True,
            env=environment,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(tool.read_text(encoding="ascii"), "trusted")
        self.assertEqual(log.read_text(encoding="ascii"), "reinstall\n")

    def test_homebrew_formula_revision_is_part_of_expected_version(self):
        build = MACOS_BUILD.read_text(encoding="utf-8")
        self.assertIn("automake=1.18.1_1", build)
        self.assertIn("dbus=1.16.2_1", build)

    def test_native_regressions_skip_linux_only_release_gates(self):
        runner = MACOS_REGRESSION_RUNNER.read_text(encoding="utf-8")
        release_gates = CI_RELEASE_GATES.read_text(encoding="utf-8")

        self.assertIn(
            'bash "$TEST_ROOT/testCiReleaseGates.sh" --portable', runner
        )
        self.assertIn('MODE=${1:-full}', release_gates)
        self.assertRegex(
            release_gates,
            r'if \[ "\$MODE" = "full" \]; then\s+'
            r'python3 "\$APT_SNAPSHOT_TEST"\s+fi',
        )

    def test_github_bundle_scan_recognizes_mach_o_payloads(self):
        packager = GITHUB_PACKAGER.read_text(encoding="utf-8")

        self.assertIn("grep -q Mach-O", packager)
        self.assertNotIn("grep -q Match-O", packager)


if __name__ == "__main__":
    unittest.main()
