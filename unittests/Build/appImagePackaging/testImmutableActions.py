#!/usr/bin/env python3

from pathlib import Path
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import yaml
from yaml.nodes import MappingNode, ScalarNode, SequenceNode


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
CHECKER = REPOSITORY_ROOT / ".github" / "scripts" / "check-immutable-actions.py"
PRODUCTION_ALLOWLIST = REPOSITORY_ROOT / ".github" / "actions.lock"
PARSER_LOCK = REPOSITORY_ROOT / ".github" / "scripts" / "immutable-actions-requirements.lock"
POLICY_WORKFLOW = REPOSITORY_ROOT / ".github" / "workflows" / "workflow-policy.yml"
POLICY_CONTRACT = REPOSITORY_ROOT / ".github" / "workflow-policy-contract.json"
CODEOWNERS = REPOSITORY_ROOT / ".github" / "CODEOWNERS"
MASTER_RULESET = REPOSITORY_ROOT / ".github" / "master-ruleset.json"
RELEASE_POLICY_DOCUMENT = (
    REPOSITORY_ROOT / "doc" / "BUILD_ARTIFACT_AUTHENTICITY.md"
)


class ImmutableActionTests(unittest.TestCase):
    BUILD_WORKFLOWS = (
        REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml",
        REPOSITORY_ROOT
        / ".github"
        / "workflows"
        / "ridecache-removal-native.yml",
        REPOSITORY_ROOT
        / ".github"
        / "workflows"
        / "windows-durable-filesystem.yml",
    )

    def run_checker_text(self, workflow_text, allowlist_text):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            workflow = root / "fixture.yml"
            workflow.write_text(workflow_text, encoding="ascii")
            allowlist = root / "actions.lock"
            allowlist.write_text(allowlist_text, encoding="ascii")
            return subprocess.run(
                [
                    sys.executable,
                    str(CHECKER),
                    "--workflows",
                    str(root),
                    "--allowlist",
                    str(allowlist),
                ],
                capture_output=True,
                text=True,
            )

    def run_checker(self, *uses):
        sha = "1" * 40
        return self.run_checker_text(
            "jobs:\n  test:\n    steps:\n"
            + "".join(f"      - uses: {value}\n" for value in uses),
            f"actions/checkout {sha}\n",
        )

    def test_commit_and_container_digest_references_are_accepted(self):
        result = self.run_checker(
            "actions/checkout@" + "1" * 40,
            "docker://ubuntu@sha256:" + "2" * 64,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unchecked_local_actions_are_rejected(self):
        result = self.run_checker(
            "actions/checkout@" + "1" * 40,
            "./.github/actions/local",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("local action", result.stderr)

    def test_job_and_service_images_require_sha256_digests(self):
        sha = "1" * 40
        digest = "2" * 64
        approved = (
            "jobs:\n"
            "  test:\n"
            f"    container: ubuntu@sha256:{digest}\n"
            "    services:\n"
            "      database:\n"
            f"        image: postgres@sha256:{digest}\n"
            "    steps:\n"
            f"      - uses: actions/checkout@{sha}\n"
        )
        result = self.run_checker_text(
            approved, f"actions/checkout {sha}\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        for mutable in (
            approved.replace(f"ubuntu@sha256:{digest}", "ubuntu:24.04"),
            approved.replace(f"postgres@sha256:{digest}", "postgres:17"),
        ):
            with self.subTest(workflow=mutable):
                result = self.run_checker_text(
                    mutable, f"actions/checkout {sha}\n"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("container image is not immutable", result.stderr)

    def test_block_and_flow_yaml_are_parsed_without_matching_strings_or_comments(self):
        sha = "1" * 40
        result = self.run_checker_text(
            "jobs:\n"
            "  block:\n"
            "    steps:\n"
            f"      - uses: 'actions/checkout@{sha}' # reviewed\n"
            "      - run: |-2\n"
            "          printf 'uses: attacker/action@deadbeef'\n"
            f"  flow: {{steps: [{{\"u\\u0073es\": \"actions/checkout@{sha}\"}}, "
            "{uses: docker://ubuntu@sha256:" + "2" * 64 + "}]}\n"
            "# uses: attacker/action@0000000000000000000000000000000000000000\n",
            f"actions/checkout {sha}\n",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_scalar_uses_keys_are_found_through_yaml_syntax_features(self):
        sha = "1" * 40
        allowlist = f"actions/checkout {sha}\n"
        bypasses = {
            "explicit key": (
                "jobs:\n  test:\n    steps:\n"
                "      - ? uses\n"
                "        : attacker/action@v1\n"
                f"      - uses: actions/checkout@{sha}\n"
            ),
            "tagged key": (
                "jobs:\n  test:\n    steps:\n"
                "      - !!map { !!str uses: attacker/action@v1, "
                f"approved: actions/checkout@{sha} }}\n"
            ),
            "aliased key": (
                "uses-key: &uses-key uses\n"
                "jobs:\n  test:\n    steps:\n"
                "      - { *uses-key: attacker/action@v1, "
                f"approved: actions/checkout@{sha} }}\n"
            ),
            "anchored tagged value": (
                "bad: &bad !!str attacker/action@v1\n"
                "jobs:\n  test:\n    steps:\n"
                "      - { !!str uses: *bad, "
                f"approved: actions/checkout@{sha} }} # comment\n"
            ),
        }
        for name, workflow in bypasses.items():
            with self.subTest(name=name):
                result = self.run_checker_text(workflow, allowlist)
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn("attacker/action@v1", result.stderr)

    def test_duplicate_mapping_keys_are_rejected_including_aliases(self):
        sha = "1" * 40
        workflows = {
            "literal": (
                "jobs:\n  test:\n    steps:\n"
                f"      - uses: actions/checkout@{sha}\n"
                f"        uses: actions/checkout@{sha}\n"
            ),
            "alias": (
                "uses-key: &uses-key uses\n"
                "jobs:\n  test:\n    steps:\n"
                f"      - {{ *uses-key: actions/checkout@{sha}, "
                f"uses: actions/checkout@{sha} }}\n"
            ),
        }
        for name, workflow in workflows.items():
            with self.subTest(name=name):
                result = self.run_checker_text(
                    workflow, f"actions/checkout {sha}\n"
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("duplicate mapping key", result.stderr)

    def test_recursive_yaml_graph_is_rejected(self):
        sha = "1" * 40
        result = self.run_checker_text(
            "jobs: &jobs\n"
            "  test:\n"
            f"    uses: actions/checkout@{sha}\n"
            "  recursive: *jobs\n",
            f"actions/checkout {sha}\n",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("recursive YAML graph", result.stderr)

    def test_yaml_tags_are_composed_without_object_construction(self):
        sha = "1" * 40
        with tempfile.TemporaryDirectory() as temporary:
            marker = Path(temporary) / "constructed"
            command = f"touch {marker}"
            result = self.run_checker_text(
                f"probe: !!python/object/apply:os.system ['{command}']\n"
                "jobs:\n  test:\n    steps:\n"
                f"      - uses: actions/checkout@{sha}\n",
                f"actions/checkout {sha}\n",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(marker.exists())

    def test_approved_anchored_key_and_aliased_value_are_accepted(self):
        sha = "1" * 40
        result = self.run_checker_text(
            "uses-key: &uses-key !!str uses\n"
            f"approved: &approved actions/checkout@{sha}\n"
            "jobs:\n  test:\n    steps:\n"
            "      - { *uses-key: *approved } # aliases remain data\n",
            f"actions/checkout {sha}\n",
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_yaml_merge_keys_are_rejected(self):
        sha = "1" * 40
        result = self.run_checker_text(
            f"defaults: &defaults {{uses: actions/checkout@{sha}}}\n"
            "jobs:\n  test:\n    steps:\n"
            f"      - {{<<: *defaults, uses: actions/checkout@{sha}}}\n",
            f"actions/checkout {sha}\n",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("YAML merge keys are forbidden", result.stderr)

    def test_action_repository_and_sha_must_match_reviewed_allowlist(self):
        approved_sha = "1" * 40
        for reference in (
            "unreviewed/action@" + approved_sha,
            "actions/checkout@" + "2" * 40,
        ):
            with self.subTest(reference=reference):
                result = self.run_checker_text(
                    f"jobs: {{test: {{uses: {reference}}}}}\n",
                    f"actions/checkout {approved_sha}\n",
                )
                self.assertNotEqual(result.returncode, 0)

    def test_checked_in_allowlist_accepts_all_repository_workflows(self):
        result = subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--workflows",
                str(REPOSITORY_ROOT / ".github" / "workflows"),
                "--allowlist",
                str(PRODUCTION_ALLOWLIST),
                "--enforce-policy",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def run_repository_policy(self, workflows, repository_root=None):
        if repository_root is None:
            repository_root = REPOSITORY_ROOT
        command = [
            sys.executable,
            str(CHECKER),
            "--workflows",
            str(workflows),
            "--allowlist",
            str(PRODUCTION_ALLOWLIST),
            "--enforce-policy",
            "--repository-root",
            str(repository_root),
        ]
        return subprocess.run(
            command,
            capture_output=True,
            text=True,
        )

    def copy_repository_policy_fixture(self, destination):
        contract = json.loads(POLICY_CONTRACT.read_text(encoding="ascii"))
        shutil.copytree(
            REPOSITORY_ROOT / ".github" / "workflows",
            destination / ".github" / "workflows",
        )
        for relative in contract["protected_files"]:
            source = REPOSITORY_ROOT / relative
            target = destination / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        return contract

    def test_repository_policy_rejects_required_workflow_deletion(self):
        with tempfile.TemporaryDirectory() as temporary:
            workflows = Path(temporary) / "workflows"
            shutil.copytree(
                REPOSITORY_ROOT / ".github" / "workflows", workflows
            )
            (workflows / "ci.yml").unlink()
            result = self.run_repository_policy(workflows)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required workflow", result.stderr)

    def test_repository_policy_rejects_uncontracted_workflow(self):
        with tempfile.TemporaryDirectory() as temporary:
            workflows = Path(temporary) / "workflows"
            shutil.copytree(
                REPOSITORY_ROOT / ".github" / "workflows", workflows
            )
            checkout = "actions/checkout@" + "d23441a48e516b6c34aea4fa41551a30e30af803"
            (workflows / "uncontracted.yml").write_text(
                "name: Uncontracted\n"
                "on: workflow_dispatch\n"
                "permissions:\n  contents: read\n"
                "jobs:\n  inspect:\n    runs-on: ubuntu-24.04\n"
                f"    steps:\n      - uses: {checkout}\n",
                encoding="ascii",
            )
            result = self.run_repository_policy(workflows)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("uncontracted workflow", result.stderr)

    def test_repository_policy_rejects_semantic_weakening(self):
        mutations = {
            "untrusted workflow run": (
                "ci.yml",
                'workflows: ["Workflow policy"]',
                'workflows: ["Candidate policy"]',
            ),
            "candidate status permission": (
                "ci.yml",
                "  macos:\n",
                "  macos:\n    permissions:\n      statuses: write\n",
            ),
            "candidate secret injection": (
                "ci.yml",
                "  macos:\n",
                "  macos:\n    env:\n      LEAK: ${{ secrets.GC_NOKIA_CLIENT_SECRET }}\n",
            ),
            "trusted release event weakening": (
                "ci.yml",
                "    if: ${{ github.event_name == 'push' && github.ref == "
                "'refs/heads/master' && contains(github.event.head_commit.message, "
                "'[publish binaries]') }}\n",
                "    if: ${{ always() }}\n",
            ),
            "build command weakening": (
                "ci.yml",
                "        run: ./.github/scripts/build.sh\n",
                "        run: 'true'\n",
            ),
            "test failure suppression": (
                "ci.yml",
                "      - name: Test\n",
                "      - name: Test\n        continue-on-error: true\n",
            ),
            "build shell replacement": (
                "ci.yml",
                "      - name: Build\n",
                "      - name: Build\n        shell: 'true {0}'\n",
            ),
            "candidate runner weakening": (
                "ci.yml",
                "    runs-on: macos-latest\n",
                "    runs-on: self-hosted\n",
            ),
            "extra bypass step": (
                "ci.yml",
                "      - name: Test\n",
                "      - name: Bypass tests\n"
                "        run: 'true'\n"
                "      - name: Test\n",
            ),
            "policy trigger deletion": (
                "workflow-policy.yml",
                "  pull_request_target:\n",
                "  workflow_dispatch:\n",
            ),
            "policy edited event deletion": (
                "workflow-policy.yml",
                "types: [opened, edited, reopened, synchronize, ready_for_review]",
                "types: [opened, reopened, synchronize, ready_for_review]",
            ),
            "policy base branch widening": (
                "workflow-policy.yml",
                "branches: [master]",
                "branches: ['**']",
            ),
            "policy repository check deletion": (
                "workflow-policy.yml",
                "github.event.pull_request.base.repo.full_name == github.repository && ",
                "",
            ),
            "focused path scope widening": (
                "windows-durable-filesystem.yml",
                "            .github/workflows/windows-durable-filesystem.yml\n",
                "            **\n",
            ),
            "windows full inventory suppression": (
                "ci.yml",
                "python .github/scripts/run-tests.py --build-tool jom --platform windows",
                "Write-Output 'tests skipped'",
            ),
        }
        for name, (filename, original, replacement) in mutations.items():
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                workflows = Path(temporary) / "workflows"
                shutil.copytree(
                    REPOSITORY_ROOT / ".github" / "workflows", workflows
                )
                target = workflows / filename
                contents = target.read_text(encoding="utf-8")
                self.assertIn(original, contents)
                target.write_text(
                    contents.replace(original, replacement, 1),
                    encoding="utf-8",
                )
                result = self.run_repository_policy(workflows)
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn("workflow policy contract", result.stderr)

    def test_repository_policy_binds_every_indirect_ci_input(self):
        contract = json.loads(POLICY_CONTRACT.read_text(encoding="ascii"))
        protected = set(contract["protected_files"])
        required = {
            ".github/CODEOWNERS",
            ".github/actions.lock",
            ".github/master-ruleset.json",
            ".github/scripts/after_build.sh",
            ".github/scripts/before_build.sh",
            ".github/scripts/build.sh",
            ".github/scripts/check-immutable-actions.py",
            ".github/scripts/immutable-actions-requirements.lock",
            ".github/scripts/install-pinned-homebrew.sh",
            ".github/scripts/install.sh",
            ".github/scripts/run-tests.py",
            "appveyor.yml",
            "appveyor/linux/before_build.sh",
            "appveyor/linux/build-appimage-pass.sh",
            "appveyor/linux/build-input-paths.sh",
            "appveyor/linux/install.sh",
            "appveyor/linux/package-appimage-pass.sh",
            "appveyor/linux/reproduce-appimage.sh",
            "appveyor/macos/run-build-regressions.sh",
            "appveyor/safe-extract.py",
            "appveyor/windows/before_build.ps1",
            "appveyor/windows/install.ps1",
            "appveyor/windows/vcpkg.json",
            "src/Python/requirements-appimage.lock",
            "src/Resources/linux/capture-linuxdeployqt-transforms.py",
            "src/Resources/linux/compute-build-input-identity.py",
            "src/Resources/linux/read-appimage-offset.py",
            "src/Resources/linux/verify-appimage-payload.py",
            "unittests/Build/appImagePackaging/testCiReleaseGates.sh",
            "unittests/Build/appImagePackaging/testImmutableActions.py",
            "unittests/Build/appImagePackaging/testMacOSPackaging.py",
            "unittests/Build/appImagePackaging/testReleaseHardening.py",
            "unittests/ci-required-tests.txt",
        }
        self.assertTrue(required <= protected, sorted(required - protected))
        self.assertEqual(list(contract["protected_files"]), sorted(protected))

    def test_repository_policy_rejects_each_protected_ci_input_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repository"
            contract = self.copy_repository_policy_fixture(repository)
            workflows = repository / ".github" / "workflows"
            baseline = self.run_repository_policy(workflows, repository)
            self.assertEqual(baseline.returncode, 0, baseline.stderr)

            for relative in contract["protected_files"]:
                with self.subTest(path=relative):
                    target = repository / relative
                    original = target.read_bytes()
                    target.write_bytes(original + b"\nmutation\n")
                    result = self.run_repository_policy(workflows, repository)
                    self.assertNotEqual(result.returncode, 0, result.stderr)
                    self.assertIn("protected CI file changed", result.stderr)
                    target.write_bytes(original)

    def test_repository_policy_rejects_missing_or_symlinked_ci_inputs(self):
        for mode in ("missing", "symlink"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temporary:
                repository = Path(temporary) / "repository"
                contract = self.copy_repository_policy_fixture(repository)
                relative = next(iter(contract["protected_files"]))
                target = repository / relative
                target.unlink()
                if mode == "symlink":
                    target.symlink_to("/dev/null")
                result = self.run_repository_policy(
                    repository / ".github" / "workflows", repository
                )
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn("protected CI file is unavailable or unsafe", result.stderr)

    def test_mutable_action_and_container_tags_are_rejected(self):
        for reference in (
            "actions/checkout@v6",
            "docker://ubuntu:24.04",
            "docker://ubuntu",
        ):
            with self.subTest(reference=reference):
                result = self.run_checker(reference)
                self.assertNotEqual(result.returncode, 0)

    def test_parser_dependency_is_exactly_versioned_and_hash_locked(self):
        contents = PARSER_LOCK.read_text(encoding="ascii")
        requirements = [
            line.strip()
            for line in contents.splitlines()
            if line.strip() and not line.lstrip().startswith("#")
        ]
        self.assertTrue(requirements)
        self.assertRegex(requirements[0], r"^PyYAML==[0-9]+(?:\.[0-9]+)+ \\$")
        self.assertIn("--hash=sha256:", contents)
        self.assertNotIn("--trusted-host", contents)

    def test_trusted_preflight_treats_candidate_checkout_only_as_data(self):
        contents = POLICY_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("pull_request_target:", contents)
        self.assertIn("branches: [master]", contents)
        self.assertIn("edited", contents)
        self.assertIn(
            "run-name: workflow-policy:${{ github.event.pull_request.number }}:"
            "${{ github.event.pull_request.head.repo.full_name }}:"
            "${{ github.event.pull_request.head.sha }}",
            contents,
        )
        self.assertIn(
            "github.event.pull_request.base.repo.full_name == github.repository",
            contents,
        )
        self.assertIn("github.event.pull_request.base.ref == 'master'", contents)
        self.assertIn(
            "EVENT_REPOSITORY: ${{ github.event.repository.full_name }}", contents
        )
        self.assertIn(
            "BASE_REPOSITORY: ${{ github.event.pull_request.base.repo.full_name }}",
            contents,
        )
        self.assertIn("BASE_REF: ${{ github.event.pull_request.base.ref }}", contents)
        self.assertIn("contents: read", contents)
        self.assertIn("statuses: write", contents)
        self.assertIn("ref: ${{ github.event.pull_request.base.sha }}", contents)
        self.assertIn("ref: ${{ github.event.pull_request.head.sha }}", contents)
        self.assertIn("repository: ${{ github.event.pull_request.head.repo.full_name }}", contents)
        self.assertGreaterEqual(contents.count("persist-credentials: false"), 2)
        self.assertIn("/.github/workflows/", contents)
        self.assertIn("/.github/scripts/run-tests.py", contents)
        self.assertIn("/appveyor/linux/build-appimage-pass.sh", contents)
        self.assertIn("/appveyor/linux/reproduce-appimage.sh", contents)
        self.assertIn(
            "/src/Resources/linux/capture-linuxdeployqt-transforms.py", contents
        )
        self.assertIn(
            "/src/Resources/linux/compute-build-input-identity.py", contents
        )
        self.assertIn(
            "/src/Resources/linux/verify-appimage-payload.py", contents
        )
        self.assertIn("/unittests/ci-required-tests.txt", contents)
        self.assertIn("--require-hashes", contents)
        self.assertIn("--only-binary=:all:", contents)
        self.assertIn("--workflows candidate/.github/workflows", contents)
        self.assertIn("--repository-root candidate", contents)
        self.assertIn("--enforce-policy", contents)
        self.assertIn("Workflow policy / immutable actions", contents)
        self.assertNotIn("run: candidate/", contents)
        self.assertNotIn("working-directory: candidate", contents)

    def test_policy_source_itself_passes_immutable_action_check(self):
        result = subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "--workflows",
                str(POLICY_WORKFLOW),
                "--allowlist",
                str(PRODUCTION_ALLOWLIST),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_pr_build_workflows_run_only_after_trusted_preflight(self):
        for workflow in self.BUILD_WORKFLOWS:
            with self.subTest(workflow=workflow.name):
                contents = workflow.read_text(encoding="utf-8")
                self.assertNotIn("  pull_request:\n", contents)
                self.assertIn("  workflow_run:\n", contents)
                self.assertIn('workflows: ["Workflow policy"]', contents)
                self.assertIn("types: [completed]", contents)
                self.assertIn(
                    "POLICY_EVENT: ${{ github.event.workflow_run.event }}",
                    contents,
                )
                self.assertIn('[ "$POLICY_EVENT" = pull_request_target ]', contents)
                self.assertIn(
                    "POLICY_CONCLUSION: ${{ github.event.workflow_run.conclusion }}",
                    contents,
                )
                self.assertIn('[ "$POLICY_CONCLUSION" = success ]', contents)
                self.assertIn(
                    "POLICY_PATH: ${{ github.event.workflow_run.path }}",
                    contents,
                )
                self.assertIn(
                    '[ "$POLICY_PATH" = .github/workflows/workflow-policy.yml ]',
                    contents,
                )
                self.assertIn(
                    "POLICY_IDENTITY: ${{ github.event.workflow_run.display_title }}",
                    contents,
                )
                self.assertIn(
                    "POLICY_REPOSITORY: ${{ github.event.workflow_run.repository.full_name }}",
                    contents,
                )
                self.assertIn(
                    "POLICY_HEAD_BRANCH: ${{ github.event.workflow_run.head_branch }}",
                    contents,
                )
                self.assertIn("workflow-policy:*:*:[0-9a-f]", contents)
                self.assertIn("needs: validated-candidate", contents)
                self.assertIn(
                    "repository: ${{ needs.validated-candidate.outputs.repository }}",
                    contents,
                )
                self.assertIn(
                    "ref: ${{ needs.validated-candidate.outputs.revision }}",
                    contents,
                )
                self.assertIn("persist-credentials: false", contents)
                self.assertNotIn("cache: true", contents)
                if workflow.name != "ci.yml":
                    self.assertNotIn("secrets.", contents)
                self.assertNotIn("id-token: write", contents)

    def test_trusted_release_is_push_only_and_candidate_jobs_are_secret_free(self):
        document = yaml.safe_load(self.BUILD_WORKFLOWS[0].read_text(encoding="utf-8"))
        jobs = document["jobs"]
        trusted = jobs["trusted-macos-release"]
        self.assertEqual(
            trusted["if"],
            "${{ github.event_name == 'push' && github.ref == "
            "'refs/heads/master' && contains(github.event.head_commit.message, "
            "'[publish binaries]') }}",
        )
        self.assertEqual(trusted["permissions"], {"contents": "write"})
        self.assertEqual(set(trusted["needs"]), {"validated-candidate", "macos"})
        expected_secrets = {
            "GC_CLOUD_OPENDATA_SECRET",
            "GC_NOKIA_CLIENT_SECRET",
            "GC_DROPBOX_CLIENT_SECRET",
            "GC_CYCLINGANALYTICS_CLIENT_SECRET",
            "GC_CLOUD_DB_BASIC_AUTH",
            "GC_CLOUD_DB_APP_NAME",
            "GC_POLARFLOW_CLIENT_SECRET",
            "GC_SPORTTRACKS_CLIENT_SECRET",
            "GC_RWGPS_API_KEY",
            "GC_NOLIO_CLIENT_ID",
            "GC_NOLIO_SECRET",
            "GC_XERT_CLIENT_SECRET",
            "GC_AZUM_CLIENT_SECRET",
            "GC_TRAINERDAY_API_KEY",
        }
        self.assertEqual(set(trusted["env"]), expected_secrets)
        for name in expected_secrets:
            self.assertEqual(trusted["env"][name], f"${{{{ secrets.{name} }}}}")
        self.assertNotIn("GC_STRAVA_CLIENT_SECRET", trusted["env"])

        checkout = next(
            step for step in trusted["steps"]
            if str(step.get("uses", "")).startswith("actions/checkout@")
        )
        self.assertEqual(
            checkout["with"],
            {
                "repository": "${{ github.repository }}",
                "ref": "${{ github.sha }}",
                "persist-credentials": False,
            },
        )
        names = {step.get("name") for step in trusted["steps"]}
        self.assertTrue({"Add Secrets", "Build", "Test", "Release"} <= names)

        for job_name in ("linux", "macos", "windows"):
            candidate = json.dumps(jobs[job_name], sort_keys=True)
            self.assertNotIn("secrets.", candidate)
            self.assertNotIn("contents\": \"write", candidate)

    def test_full_pr_ci_runs_the_complete_inventory_on_every_platform(self):
        workflow = self.BUILD_WORKFLOWS[0].read_text(encoding="utf-8")
        document = yaml.compose(workflow, Loader=yaml.SafeLoader)
        jobs = self.mapping_value(document, "jobs")
        for job_name, runner, platform in (
            ("linux", "ubuntu-22.04", "linux"),
            ("macos", "macos-latest", "macos"),
            ("windows", "windows-2025", "windows"),
        ):
            with self.subTest(job=job_name):
                job = self.mapping_value(jobs, job_name)
                self.assertEqual(self.mapping_value(job, "runs-on").value, runner)
                steps = self.mapping_value(job, "steps")
                scripts = []
                for step in steps.value:
                    try:
                        scripts.append(self.mapping_value(step, "run").value)
                    except AssertionError:
                        pass
                commands = "\n".join(scripts)
                self.assertIn(".github/scripts/run-tests.py", commands)
                self.assertIn(f"--platform {platform}", commands)

        final = self.mapping_value(jobs, "report-final")
        needs = self.mapping_value(final, "needs")
        self.assertIsInstance(needs, SequenceNode)
        self.assertEqual(
            {value.value for value in needs.value},
            {"validated-candidate", "report-pending", "linux", "macos", "windows"},
        )

    def test_focused_pr_workflows_gate_jobs_on_changed_paths(self):
        for workflow in self.BUILD_WORKFLOWS[1:]:
            with self.subTest(workflow=workflow.name):
                contents = workflow.read_text(encoding="utf-8")
                self.assertIn("selected: ${{ steps.resolve.outputs.selected }}", contents)
                self.assertIn("PATH_FILTERS:", contents)
                self.assertIn("pulls/${pull_number}/files?per_page=100", contents)
                self.assertIn(
                    "needs.validated-candidate.outputs.selected == 'true'", contents
                )
                self.assertIn("Candidate tests are not applicable", contents)

    def test_all_bash_workflow_steps_are_syntactically_valid(self):
        for workflow in (*self.BUILD_WORKFLOWS, POLICY_WORKFLOW):
            document = yaml.safe_load(workflow.read_text(encoding="utf-8"))
            for job_name, job in document["jobs"].items():
                windows_job = str(job.get("runs-on", "")).startswith("windows-")
                for index, step in enumerate(job.get("steps", []), start=1):
                    script = step.get("run")
                    shell = step.get("shell")
                    if script is None or shell == "pwsh" or (shell is None and windows_job):
                        continue
                    with self.subTest(
                        workflow=workflow.name, job=job_name, step=index
                    ):
                        result = subprocess.run(
                            ["bash", "-n"],
                            input=script,
                            capture_output=True,
                            text=True,
                        )
                        self.assertEqual(result.returncode, 0, result.stderr)

    def test_workflow_run_builds_report_candidate_sha_with_isolated_permissions(self):
        expected_contexts = {
            "ci.yml": "Candidate CI / build",
            "ridecache-removal-native.yml":
                "Candidate CI / native activity transactions",
            "windows-durable-filesystem.yml":
                "Candidate CI / durable and anchored filesystem",
        }
        for workflow in self.BUILD_WORKFLOWS:
            with self.subTest(workflow=workflow.name):
                contents = workflow.read_text(encoding="utf-8")
                self.assertIn("  report-pending:\n", contents)
                self.assertIn("  report-final:\n", contents)
                self.assertGreaterEqual(contents.count("statuses: write"), 2)
                self.assertIn(
                    "CANDIDATE_SHA: ${{ needs.validated-candidate.outputs.revision }}",
                    contents,
                )
                self.assertIn("state=pending", contents)
                self.assertIn("state=success", contents)
                self.assertIn("state=failure", contents)
                self.assertIn(expected_contexts[workflow.name], contents)

                document = yaml.compose(
                    contents, Loader=yaml.SafeLoader
                )
                jobs = self.mapping_value(document, "jobs")
                for job_name in ("report-pending", "report-final"):
                    reporter = self.mapping_value(jobs, job_name)
                    permissions = self.mapping_value(reporter, "permissions")
                    statuses = self.mapping_value(permissions, "statuses")
                    self.assertEqual(statuses.value, "write")
                    reporter_text = str(reporter.value)
                    self.assertNotIn("actions/checkout", reporter_text)

                candidate_jobs = {
                    "ci.yml": ("linux", "macos", "windows"),
                    "ridecache-removal-native.yml":
                        ("windows", "macos", "linux"),
                    "windows-durable-filesystem.yml": ("windows", "macos"),
                }[workflow.name]
                for job_name in candidate_jobs:
                    candidate = self.mapping_value(jobs, job_name)
                    try:
                        permissions = self.mapping_value(candidate, "permissions")
                    except AssertionError:
                        continue
                    with self.assertRaises(AssertionError):
                        self.mapping_value(permissions, "statuses")

    def test_codeowners_and_branch_rules_document_protected_release_policy(self):
        self.assertTrue(CODEOWNERS.is_file())
        owners = CODEOWNERS.read_text(encoding="ascii")
        self.assertIn("/.github/CODEOWNERS", owners)
        self.assertIn("/.github/workflows/", owners)
        self.assertIn("/.github/scripts/check-immutable-actions.py", owners)
        self.assertIn("/.github/scripts/immutable-actions-requirements.lock", owners)
        self.assertIn("/.github/scripts/run-tests.py", owners)
        self.assertIn("/.github/master-ruleset.json", owners)
        self.assertIn("/appveyor.yml", owners)
        self.assertIn("/src/Core/Secrets.h", owners)
        self.assertIn("/unittests/ci-required-tests.txt", owners)
        self.assertIn("/util/add_secrets.ps1", owners)

        policy = RELEASE_POLICY_DOCUMENT.read_text(encoding="ascii")
        self.assertIn("single-maintainer", policy.lower())
        self.assertIn("second maintainer", policy.lower())
        self.assertIn("Workflow policy / immutable actions", policy)
        self.assertIn("Candidate CI / build", policy)
        self.assertIn("Candidate CI / native activity transactions", policy)
        self.assertIn("Candidate CI / durable and anchored filesystem", policy)

    def test_master_ruleset_is_importable_and_binds_status_sources(self):
        ruleset = json.loads(MASTER_RULESET.read_text(encoding="ascii"))
        self.assertEqual(
            set(ruleset),
            {"name", "target", "enforcement", "bypass_actors", "conditions", "rules"},
        )
        self.assertEqual(ruleset["target"], "branch")
        self.assertEqual(ruleset["enforcement"], "active")
        self.assertEqual(ruleset["bypass_actors"], [])
        self.assertEqual(
            ruleset["conditions"],
            {"ref_name": {"include": ["refs/heads/master"], "exclude": []}},
        )
        rules = {rule["type"]: rule for rule in ruleset["rules"]}
        self.assertTrue({"deletion", "non_fast_forward", "pull_request", "required_status_checks"} <= set(rules))
        pull_request = rules["pull_request"]["parameters"]
        self.assertFalse(pull_request["require_code_owner_review"])
        self.assertTrue(pull_request["dismiss_stale_reviews_on_push"])
        self.assertFalse(pull_request["require_last_push_approval"])
        self.assertTrue(pull_request["required_review_thread_resolution"])
        self.assertEqual(pull_request["required_approving_review_count"], 0)
        status_parameters = rules["required_status_checks"]["parameters"]
        self.assertTrue(status_parameters["strict_required_status_checks_policy"])
        self.assertFalse(status_parameters["do_not_enforce_on_create"])
        checks = status_parameters["required_status_checks"]
        self.assertEqual(
            {check["context"] for check in checks},
            {
                "Workflow policy / immutable actions",
                "Candidate CI / build",
                "Candidate CI / native activity transactions",
                "Candidate CI / durable and anchored filesystem",
            },
        )
        self.assertTrue(checks)
        self.assertEqual({check["integration_id"] for check in checks}, {15368})

    @staticmethod
    def mapping_value(mapping, name):
        if not isinstance(mapping, MappingNode):
            raise AssertionError(f"expected mapping while looking for {name}")
        for key, value in mapping.value:
            if isinstance(key, ScalarNode) and key.value == name:
                return value
        raise AssertionError(f"missing YAML mapping key: {name}")

    def candidate_resolver_script(self, workflow):
        document = yaml.compose(
            workflow.read_text(encoding="utf-8"), Loader=yaml.SafeLoader
        )
        jobs = self.mapping_value(document, "jobs")
        resolver_job = self.mapping_value(jobs, "validated-candidate")
        steps = self.mapping_value(resolver_job, "steps")
        self.assertIsInstance(steps, SequenceNode)
        for step in steps.value:
            try:
                name = self.mapping_value(step, "name")
            except AssertionError:
                continue
            if isinstance(name, ScalarNode) and name.value == \
                    "Resolve trusted candidate identity":
                script = self.mapping_value(step, "run")
                self.assertIsInstance(script, ScalarNode)
                return script.value
        self.fail(f"candidate resolver step is missing from {workflow}")

    def run_candidate_resolver(self, script, **overrides):
        revision = "1" * 40
        policy_revision = "3" * 40
        environment = os.environ.copy()
        environment.update(
            {
                "EVENT_NAME": "workflow_run",
                "CURRENT_REPOSITORY": "kaartine/GoldenCheetah",
                "CURRENT_REVISION": "2" * 40,
                "POLICY_EVENT": "pull_request_target",
                "POLICY_PATH": ".github/workflows/workflow-policy.yml",
                "POLICY_CONCLUSION": "success",
                "POLICY_REPOSITORY": "kaartine/GoldenCheetah",
                "POLICY_HEAD_BRANCH": "master",
                "POLICY_HEAD_SHA": policy_revision,
                "POLICY_IDENTITY": f"workflow-policy:17:fork-owner/repo.name:{revision}",
                "PATH_FILTERS": "**",
            }
        )
        environment.update(overrides)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "github-output"
            environment["GITHUB_OUTPUT"] = str(output)
            pull = root / "pull.json"
            pull.write_text(
                json.dumps(
                    {
                        "state": "open",
                        "base": {
                            "ref": "master",
                            "sha": policy_revision,
                            "repo": {"full_name": "kaartine/GoldenCheetah"},
                        },
                        "head": {
                            "sha": revision,
                            "repo": {"full_name": "fork-owner/repo.name"},
                        },
                    }
                ),
                encoding="ascii",
            )
            files = root / "files.json"
            files.write_text(
                json.dumps([[{"filename": "src/FileIO/AnchoredFileSystem.cpp"}]]),
                encoding="ascii",
            )
            fake_bin = root / "bin"
            fake_bin.mkdir()
            gh = fake_bin / "gh"
            gh.write_text(
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "case \"$*\" in\n"
                "  *'/git/ref/heads/master'*) printf '%s\\n' \"$GH_POLICY_SHA\" ;;\n"
                "  *'/files?per_page=100'*) cat \"$GH_FILES_FIXTURE\" ;;\n"
                "  *) cat \"$GH_PULL_FIXTURE\" ;;\n"
                "esac\n",
                encoding="ascii",
            )
            gh.chmod(0o755)
            environment["GH_PULL_FIXTURE"] = str(pull)
            environment["GH_FILES_FIXTURE"] = str(files)
            environment["GH_POLICY_SHA"] = policy_revision
            environment["PATH"] = str(fake_bin) + os.pathsep + environment["PATH"]
            result = subprocess.run(
                ["bash", "-c", script],
                env=environment,
                capture_output=True,
                text=True,
            )
            contents = output.read_text(encoding="utf-8") if output.exists() else ""
            return result, contents

    def test_candidate_identity_resolver_is_fail_closed(self):
        scripts = {
            self.candidate_resolver_script(workflow)
            for workflow in self.BUILD_WORKFLOWS
        }
        self.assertEqual(len(scripts), 1, "candidate resolver copies diverged")
        script = scripts.pop()

        result, output = self.run_candidate_resolver(script)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            output,
            "authorized=true\n"
            "repository=fork-owner/repo.name\n"
            f"revision={'1' * 40}\n"
            "selected=true\n",
        )

        invalid = {
            "wrong event": {"POLICY_EVENT": "pull_request"},
            "wrong path": {"POLICY_PATH": ".github/workflows/lookalike.yml"},
            "failed policy": {"POLICY_CONCLUSION": "failure"},
            "wrong policy repository": {"POLICY_REPOSITORY": "attacker/repo"},
            "wrong policy branch": {"POLICY_HEAD_BRANCH": "development"},
            "mutable ref": {"POLICY_IDENTITY": "workflow-policy:17:owner/repo:main"},
            "revision suffix": {
                "POLICY_IDENTITY": f"workflow-policy:17:owner/repo:{'1' * 40}x"
            },
            "repository injection": {
                "POLICY_IDENTITY": f"workflow-policy:17:owner/repo\nother=value:{'1' * 40}"
            },
            "invalid pull request": {
                "POLICY_IDENTITY": f"workflow-policy:x:owner/repo:{'1' * 40}"
            },
        }
        for name, environment in invalid.items():
            with self.subTest(name=name):
                result, output = self.run_candidate_resolver(script, **environment)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output, "")

    def test_candidate_identity_resolver_skips_irrelevant_focused_changes(self):
        script = self.candidate_resolver_script(self.BUILD_WORKFLOWS[1])
        result, output = self.run_candidate_resolver(
            script, PATH_FILTERS="doc/**"
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(output.endswith("selected=false\n"), output)


if __name__ == "__main__":
    unittest.main()
