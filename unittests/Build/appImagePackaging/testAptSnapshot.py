#!/usr/bin/env python3

from email.utils import format_datetime
from datetime import datetime, timezone
import gzip
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
VERIFIER = REPOSITORY_ROOT / "appveyor" / "linux" / "verify-apt-snapshot.py"
BOOTSTRAP_VERIFIER = (
    REPOSITORY_ROOT / "appveyor" / "linux" / "verify-apt-snapshot-bootstrap.sh"
)
INSTALLER = REPOSITORY_ROOT / "appveyor" / "linux" / "install.sh"
APT_WRAPPER = (
    REPOSITORY_ROOT / "appveyor" / "linux" / "apt-get-fail-closed.sh"
)
DEV_DOCKERFILE = REPOSITORY_ROOT / ".devcontainer" / "Dockerfile"
SNAPSHOT = "20260801T000000Z"
PACKAGE_CONTENT = b"Package: authenticated-fixture\nVersion: 1\n"
UBUNTU_IMAGE = (
    "ubuntu:24.04@sha256:"
    "c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b"
)
CA_CERTIFICATES_BOOTSTRAP_URL = (
    "https://snapshot.ubuntu.com/ubuntu/20260801T000000Z/pool/main/c/"
    "ca-certificates/ca-certificates_20260601~24.04.1_all.deb"
)
CA_CERTIFICATES_BOOTSTRAP_SHA256 = (
    "6bac2a01979e210d9eac1d4d56747ec709ea60654744d66705dc3c36e7629e50"
)


def suites(series):
    return (series, f"{series}-updates", f"{series}-backports", f"{series}-security")


class AptSnapshotTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.sources = self.root / "sources.list"
        self.source_parts = self.root / "sources.list.d"
        self.lists = self.root / "lists"
        self.source_parts.mkdir()
        self.lists.mkdir()
        self.write_sources(SNAPSHOT)
        self.write_indices(datetime(2026, 7, 31, 12, tzinfo=timezone.utc))

    def tearDown(self):
        self.temporary.cleanup()

    def write_sources(self, snapshot, series="jammy"):
        lines = []
        for suite in suites(series):
            host = (
                "security.ubuntu.com"
                if suite == f"{series}-security"
                else "archive.ubuntu.com"
            )
            lines.append(
                f"deb [snapshot={snapshot}] http://{host}/ubuntu "
                f"{suite} main restricted universe multiverse\n"
            )
        self.sources.write_text("".join(lines), encoding="ascii")

    def write_indices(
        self,
        date,
        series="jammy",
        empty_components=(),
        omit_empty_indices=False,
    ):
        empty_components = set(empty_components)
        for old in self.lists.iterdir():
            old.unlink()
        for suite in suites(series):
            release = (
                "-----BEGIN PGP SIGNED MESSAGE-----\n"
                "Hash: SHA256\n\n"
                f"Suite: {suite}\n"
                f"Date: {format_datetime(date)}\n"
                "Components: main restricted universe multiverse\n"
                "SHA256:\n"
            )
            for component in ("main", "restricted", "universe", "multiverse"):
                empty = (suite, component) in empty_components
                package_content = b"" if empty else PACKAGE_CONTENT
                digest = hashlib.sha256(package_content).hexdigest()
                size = len(package_content)
                release += (
                    f" {digest} {size:16d} "
                    f"{component}/binary-amd64/Packages\n"
                )
                if empty and omit_empty_indices:
                    continue
                self.lists.joinpath(
                    f"snapshot.ubuntu.com_ubuntu_{SNAPSHOT}_dists_"
                    f"{suite}_{component}_binary-amd64_Packages"
                ).write_bytes(package_content)
            self.lists.joinpath(
                f"snapshot.ubuntu.com_ubuntu_{SNAPSHOT}_dists_"
                f"{suite}_InRelease"
            ).write_text(release, encoding="ascii")

    def run_verifier(self, version="2.4.11", series="jammy"):
        return subprocess.run(
            [
                sys.executable,
                str(VERIFIER),
                "--sources",
                str(self.sources),
                "--lists",
                str(self.lists),
                "--snapshot",
                SNAPSHOT,
                "--apt-version",
                version,
                "--series",
                series,
                "--architecture",
                "amd64",
            ],
            capture_output=True,
            text=True,
        )

    def run_bootstrap_verifier(self, version="2.4.11", series="jammy"):
        return subprocess.run(
            [
                "sh",
                str(BOOTSTRAP_VERIFIER),
                str(self.sources),
                str(self.lists),
                SNAPSHOT,
                version,
                series,
                "amd64",
            ],
            capture_output=True,
            text=True,
        )

    def assert_rejected_by_both_verifiers(self):
        for verifier, result in (
            ("Python", self.run_verifier()),
            ("bootstrap", self.run_bootstrap_verifier()),
        ):
            with self.subTest(verifier=verifier):
                self.assertNotEqual(
                    result.returncode,
                    0,
                    f"{verifier} verifier accepted invalid APT state",
                )

    def assert_accepted_by_both_verifiers(self):
        for verifier, result in (
            ("Python", self.run_verifier()),
            ("bootstrap", self.run_bootstrap_verifier()),
        ):
            with self.subTest(verifier=verifier):
                self.assertEqual(
                    result.returncode,
                    0,
                    f"{verifier}: {result.stderr}",
                )

    def test_reviewed_snapshot_sources_and_indices_are_accepted(self):
        result = self.run_verifier()
        self.assertEqual(result.returncode, 0, result.stderr)

        self.write_sources(SNAPSHOT, "noble")
        self.write_indices(
            datetime(2026, 7, 31, 12, tzinfo=timezone.utc), "noble"
        )
        result = self.run_verifier(series="noble")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_old_apt_wrong_source_future_index_and_missing_suite_are_rejected(self):
        self.assertNotEqual(self.run_verifier("2.4.10").returncode, 0)

        self.write_sources("20260802T000000Z")
        self.assertNotEqual(self.run_verifier().returncode, 0)
        self.write_sources(SNAPSHOT)

        self.write_indices(datetime(2026, 8, 2, tzinfo=timezone.utc))
        self.assertNotEqual(self.run_verifier().returncode, 0)
        self.write_indices(datetime(2026, 7, 31, tzinfo=timezone.utc))

        next(
            self.lists.glob(
                f"snapshot.ubuntu.com_ubuntu_{SNAPSHOT}_dists_"
                "jammy-backports_InRelease"
            )
        ).unlink()
        self.assertNotEqual(self.run_verifier().returncode, 0)

    def test_package_metadata_is_required_by_both_snapshot_verifiers(self):
        package_index = next(self.lists.glob("*_main_binary-amd64_Packages"))
        package_index.unlink()
        result = self.run_verifier()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("package index", result.stderr.lower())

        result = subprocess.run(
            [
                "sh",
                str(BOOTSTRAP_VERIFIER),
                str(self.sources),
                str(self.lists),
                SNAPSHOT,
                "2.4.11",
                "jammy",
                "amd64",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)

        self.write_indices(datetime(2026, 7, 31, 12, tzinfo=timezone.utc))
        result = subprocess.run(
            [
                "sh",
                str(BOOTSTRAP_VERIFIER),
                str(self.sources),
                str(self.lists),
                SNAPSHOT,
                "2.4.11",
                "jammy",
                "amd64",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_signed_empty_component_may_be_absent_but_nonempty_may_not(self):
        empty_component = {("jammy-backports", "restricted")}
        self.write_indices(
            datetime(2026, 7, 31, 12, tzinfo=timezone.utc),
            empty_components=empty_component,
            omit_empty_indices=True,
        )
        result = self.run_verifier()
        self.assertEqual(result.returncode, 0, result.stderr)
        result = subprocess.run(
            [
                "sh",
                str(BOOTSTRAP_VERIFIER),
                str(self.sources),
                str(self.lists),
                SNAPSHOT,
                "2.4.11",
                "jammy",
                "amd64",
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

        release = self.lists / (
            f"snapshot.ubuntu.com_ubuntu_{SNAPSHOT}_dists_"
            "jammy-backports_InRelease"
        )
        contents = release.read_text(encoding="ascii").replace(
            "e3b0c44298fc1c149afbf4c8996fb924"
            "27ae41e4649b934ca495991b7852b855                0 "
            "restricted/binary-amd64/Packages",
            f"{'b' * 64}                1 "
            "restricted/binary-amd64/Packages",
        )
        release.write_text(contents, encoding="ascii")
        self.assertNotEqual(self.run_verifier().returncode, 0)

    def test_stale_package_content_is_rejected_by_both_verifiers(self):
        package_index = next(self.lists.glob("*_main_binary-amd64_Packages"))
        package_index.unlink()
        compressed_index = Path(f"{package_index}.gz")
        compressed_index.write_bytes(gzip.compress(b"X" * len(PACKAGE_CONTENT)))

        self.assert_rejected_by_both_verifiers()

    def test_compressed_package_content_is_verified_after_decompression(self):
        package_index = next(self.lists.glob("*_main_binary-amd64_Packages"))
        package_content = package_index.read_bytes()
        package_index.unlink()
        Path(f"{package_index}.gz").write_bytes(gzip.compress(package_content))

        self.assert_accepted_by_both_verifiers()

    def test_signed_zero_size_with_local_content_is_rejected_by_both_verifiers(self):
        empty_component = {("jammy-backports", "restricted")}
        self.write_indices(
            datetime(2026, 7, 31, 12, tzinfo=timezone.utc),
            empty_components=empty_component,
        )
        package_index = self.lists / (
            f"snapshot.ubuntu.com_ubuntu_{SNAPSHOT}_dists_"
            "jammy-backports_restricted_binary-amd64_Packages"
        )
        package_index.write_bytes(b"unexpected package data\n")

        self.assert_rejected_by_both_verifiers()

    def test_extra_release_and_package_indices_are_rejected_by_both_verifiers(self):
        extra_release = self.lists / "archive.ubuntu.com_ubuntu_dists_jammy_InRelease"
        extra_release.write_text("stale release metadata\n", encoding="ascii")
        self.assert_rejected_by_both_verifiers()

        extra_release.unlink()
        extra_packages = self.lists / (
            "archive.ubuntu.com_ubuntu_dists_"
            "jammy_main_binary-amd64_Packages"
        )
        extra_packages.write_bytes(PACKAGE_CONTENT)
        self.assert_rejected_by_both_verifiers()

    def test_nonempty_source_parts_are_rejected_by_both_verifiers(self):
        source_part = self.source_parts / "unexpected.sources"
        source_part.write_bytes(b"")
        self.assert_accepted_by_both_verifiers()

        source_part.write_text(
            "Types: deb\nURIs: http://archive.ubuntu.com/ubuntu\n",
            encoding="ascii",
        )

        self.assert_rejected_by_both_verifiers()

    def test_release_installer_runs_fail_closed_snapshot_verification(self):
        contents = INSTALLER.read_text(encoding="utf-8")
        self.assertIn("verify-apt-snapshot.py", contents)
        self.assertIn("--apt-version", contents)
        self.assertIn("--snapshot \"$UBUNTU_SNAPSHOT\"", contents)
        flattened_installer = " ".join(contents.replace("\\\n", " ").split())
        self.assertIn(
            "sudo find /etc/apt/sources.list.d -mindepth 1 -maxdepth 1 "
            "-exec rm -rf -- {} +",
            flattened_installer,
        )
        prune_indices = (
            "find /var/lib/apt/lists -maxdepth 1 \\( "
            "-name '*_InRelease' -o -name '*_Packages*' \\) "
            "! -name \"snapshot.ubuntu.com_ubuntu_${UBUNTU_SNAPSHOT}_*\" "
            "-delete"
        )
        self.assertIn(prune_indices, flattened_installer)

        dockerfile = DEV_DOCKERFILE.read_text(encoding="utf-8")
        self.assertIn("verify-apt-snapshot.py", dockerfile)
        self.assertIn("verify-apt-snapshot-bootstrap.sh", dockerfile)
        self.assertIn("--series noble", dockerfile)
        self.assertIn("--architecture amd64", dockerfile)
        self.assertNotIn('Acquire::https::Verify-Peer "false"', dockerfile)

        flattened = " ".join(dockerfile.replace("\\\n", " ").split())
        self.assertIn(
            "find /etc/apt/sources.list.d -mindepth 1 -maxdepth 1 "
            "-exec rm -rf -- {} +",
            flattened,
        )
        self.assertEqual(flattened.count(prune_indices), 2)
        activate_snapshot = flattened.index(
            "install -m 0644 /usr/local/share/goldencheetah/"
            "ubuntu-snapshot.sources.list /etc/apt/sources.list"
        )
        snapshot_update = flattened.index("apt-get update", activate_snapshot)
        snapshot_prune = flattened.index(prune_indices, snapshot_update)
        bootstrap_verify = flattened.index(
            "verify-apt-snapshot-bootstrap.sh", snapshot_prune
        )
        bootstrap_packages = flattened.index(
            'ca-certificates="${CA_CERTIFICATES_VERSION}"',
            bootstrap_verify,
        )
        main_install = flattened.index("apt-get install", bootstrap_packages)
        python_verify = flattened.index(
            "verify-apt-snapshot.py", main_install
        )
        self.assertLess(snapshot_update, bootstrap_verify)
        self.assertLess(snapshot_update, snapshot_prune)
        self.assertLess(snapshot_prune, bootstrap_verify)
        self.assertLess(bootstrap_verify, main_install)
        self.assertLess(main_install, python_verify)

        final_update = flattened.rindex("RUN apt-get update")
        final_prune = flattened.index(prune_indices, final_update)
        final_verify = flattened.index("verify-apt-snapshot.py", final_prune)
        self.assertLess(final_update, final_prune)
        self.assertLess(final_prune, final_verify)

        final_stage = dockerfile.rsplit("RUN apt-get update", 1)[1]
        self.assertNotIn("rm -rf /var/lib/apt/lists", final_stage)

    def test_appveyor_apt_wrapper_overrides_hostile_configuration(self):
        self.assertTrue(APT_WRAPPER.is_file())
        self.assertTrue(os.access(APT_WRAPPER, os.X_OK))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            hostile_config = root / "apt.conf"
            hostile_config.write_text(
                'Acquire::AllowInsecureRepositories "true";\n'
                'Acquire::AllowDowngradeToInsecureRepositories "true";\n'
                'APT::Get::AllowUnauthenticated "true";\n',
                encoding="ascii",
            )
            fake_bin = root / "bin"
            fake_bin.mkdir()
            arguments = root / "arguments"
            fake_apt = fake_bin / "apt-get"
            fake_apt.write_text(
                "#!/bin/sh\n"
                "set -eu\n"
                "test -s \"$APT_CONFIG\"\n"
                "printf '%s\\n' \"$@\" >\"$GC_APT_ARGUMENTS\"\n",
                encoding="ascii",
            )
            fake_apt.chmod(0o755)
            environment = os.environ.copy()
            environment.update(
                {
                    "APT_CONFIG": str(hostile_config),
                    "GC_APT_ARGUMENTS": str(arguments),
                    "PATH": f"{fake_bin}:{environment['PATH']}",
                }
            )
            result = subprocess.run(
                [str(APT_WRAPPER), "install", "-qq", "fixture"],
                env=environment,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            observed = arguments.read_text(encoding="ascii").splitlines()
            for option in (
                "Acquire::AllowInsecureRepositories=false",
                "Acquire::AllowDowngradeToInsecureRepositories=false",
                "APT::Get::AllowUnauthenticated=false",
                "--allow-downgrades",
            ):
                self.assertIn(option, observed)

        installer = INSTALLER.read_text(encoding="utf-8")
        self.assertIn('APT_GET="$REPOSITORY_ROOT/appveyor/linux/apt-get-fail-closed.sh"', installer)
        self.assertIn("/etc/apt/preferences.d/goldencheetah-snapshot", installer)
        self.assertIn('Pin: origin "snapshot.ubuntu.com"', installer)
        self.assertIn("Pin-Priority: 1001", installer)
        self.assertNotIn("sudo apt-get ", installer)

    def test_docker_bootstraps_pinned_ca_from_authenticated_snapshot(self):
        dockerfile = DEV_DOCKERFILE.read_text(encoding="utf-8")
        flattened = " ".join(dockerfile.replace("\\\n", " ").split())
        self.assertIn(
            f"ADD --checksum=sha256:{CA_CERTIFICATES_BOOTSTRAP_SHA256}",
            flattened,
        )
        self.assertIn(CA_CERTIFICATES_BOOTSTRAP_URL, flattened)
        self.assertIn(f"'{CA_CERTIFICATES_BOOTSTRAP_SHA256}'", dockerfile)
        self.assertIn("/tmp/ca-certificates-bootstrap.deb", flattened)
        self.assertIn("sha256sum --check --strict", flattened)
        self.assertIn(
            "dpkg-deb --extract /tmp/ca-certificates-bootstrap.deb",
            flattened,
        )
        self.assertIn(
            "find /tmp/ca-certificates-bootstrap/usr/share/ca-certificates "
            "-type f -name '*.crt' -print0 | sort -z | xargs -0 cat",
            flattened,
        )
        self.assertIn("ARG CA_CERTIFICATES_VERSION=", dockerfile)
        self.assertIn("ARG OPENSSL_VERSION=", dockerfile)
        self.assertIn(
            'ca-certificates="${CA_CERTIFICATES_VERSION}"', flattened
        )
        self.assertIn('openssl="${OPENSSL_VERSION}"', flattened)
        self.assertIn('libssl3t64="${OPENSSL_VERSION}"', flattened)
        self.assertIn("Acquire::AllowInsecureRepositories=false", flattened)
        self.assertIn("APT::Get::AllowUnauthenticated=false", flattened)
        self.assertNotIn("--allow-unauthenticated", flattened)
        self.assertNotIn("Acquire::https::Verify-Peer=false", flattened)
        self.assertIn(
            'test "$(dpkg-query -W -f=\'${Version}\' ca-certificates)" '
            '= "${CA_CERTIFICATES_VERSION}"',
            flattened,
        )
        self.assertIn(
            'test "$(dpkg-query -W -f=\'${Version}\' openssl)" '
            '= "${OPENSSL_VERSION}"',
            flattened,
        )

        activate_snapshot = flattened.index(
            "install -m 0644 /usr/local/share/goldencheetah/"
            "ubuntu-snapshot.sources.list /etc/apt/sources.list"
        )
        snapshot_update = flattened.index("apt-get update", activate_snapshot)
        bootstrap_verify = flattened.index(
            "verify-apt-snapshot-bootstrap.sh", snapshot_update
        )
        bootstrap_install = flattened.index(
            'ca-certificates="${CA_CERTIFICATES_VERSION}"',
            bootstrap_verify,
        )
        self.assertNotIn("apt-get update", flattened[:activate_snapshot])
        self.assertLess(activate_snapshot, snapshot_update)
        self.assertLess(snapshot_update, bootstrap_verify)
        self.assertLess(bootstrap_verify, bootstrap_install)

    @unittest.skipUnless(
        os.environ.get("GC_RUN_APT_SNAPSHOT_INTEGRATION") == "1",
        "requires an explicit networked Docker integration run",
    )
    def test_real_apt_update_indices_match_signed_metadata(self):
        docker = shutil.which("docker")
        self.assertIsNotNone(docker, "docker is required for this integration test")
        result = subprocess.run(
            [
                docker,
                "build",
                "--pull=false",
                "--no-cache",
                "--target",
                "apt-snapshot-bootstrap",
                "--file",
                str(DEV_DOCKERFILE),
                str(REPOSITORY_ROOT),
            ],
            capture_output=True,
            text=True,
            timeout=600,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"{result.stderr}\nAPT lists:\n{result.stdout}",
        )


if __name__ == "__main__":
    unittest.main()
