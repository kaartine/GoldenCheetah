#!/usr/bin/env python3

import argparse
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import hashlib
import os
from pathlib import Path
import re
import subprocess


MINIMUM_APT_VERSION = (2, 4, 11)
SERIES_RE = re.compile(r"^[a-z][a-z0-9-]*$")
ARCHITECTURE_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
COMPONENTS = "main restricted universe multiverse"
APT_HELPER = Path("/usr/lib/apt/apt-helper")
PACKAGE_INDEX_SUFFIXES = ("", ".gz", ".lz4", ".xz")


def parse_apt_version(value):
    match = re.fullmatch(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:[~+].*)?", value)
    if not match:
        raise ValueError("cannot parse apt version")
    return tuple(int(part) for part in match.groups())


def parse_snapshot(value):
    try:
        parsed = datetime.strptime(value, "%Y%m%dT%H%M%SZ")
    except ValueError as error:
        raise ValueError("invalid Ubuntu snapshot timestamp") from error
    return parsed.replace(tzinfo=timezone.utc)


def expected_sources(series):
    if not SERIES_RE.fullmatch(series):
        raise ValueError("invalid Ubuntu series")
    return (
        ("archive.ubuntu.com", series),
        ("archive.ubuntu.com", f"{series}-updates"),
        ("archive.ubuntu.com", f"{series}-backports"),
        ("security.ubuntu.com", f"{series}-security"),
    )


def expected_source_lines(snapshot, series):
    return {
        f"deb [snapshot={snapshot}] http://{host}/ubuntu {suite} {COMPONENTS}"
        for host, suite in expected_sources(series)
    }


def verify_sources(path, snapshot, series):
    if path.is_symlink() or not path.is_file():
        raise ValueError("APT source list is unsafe")
    lines = {
        line.strip()
        for line in path.read_text(encoding="ascii").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }
    if lines != expected_source_lines(snapshot, series):
        raise ValueError("APT sources differ from the reviewed snapshot set")


def verify_source_parts(path):
    if path.is_symlink():
        raise ValueError("APT source-parts directory is unsafe")
    if not path.exists():
        return
    if not path.is_dir():
        raise ValueError("APT source-parts directory is unsafe")
    for entry in path.iterdir():
        if entry.is_symlink() or not entry.is_file():
            raise ValueError(f"unsafe APT source part: {entry}")
        if entry.stat().st_size != 0:
            raise ValueError(f"nonempty APT source part: {entry}")


def release_metadata(path, architecture):
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "-----BEGIN PGP SIGNED MESSAGE-----":
        raise ValueError(f"APT release index is not an InRelease document: {path}")
    fields = {}
    package_metadata = {}
    in_sha256 = False
    for line in lines:
        if line == "SHA256:":
            in_sha256 = True
            continue
        if in_sha256 and line and not line[0].isspace():
            in_sha256 = False
        if in_sha256:
            record = line.split()
            if len(record) != 3:
                continue
            digest, size, relative_path = record
            match = re.fullmatch(
                rf"({COMPONENTS.replace(' ', '|')})/"
                rf"binary-{re.escape(architecture)}/Packages",
                relative_path,
            )
            if not match:
                continue
            component = match.group(1)
            if (
                component in package_metadata
                or not re.fullmatch(r"[0-9a-f]{64}", digest)
                or not size.isdigit()
            ):
                raise ValueError(
                    f"APT release index has invalid package metadata: {path}"
                )
            package_metadata[component] = (digest, int(size))
        if ": " not in line:
            continue
        name, value = line.split(": ", 1)
        if name in {"Suite", "Date", "Components"} and name not in fields:
            fields[name] = value
    if set(fields) != {"Suite", "Date", "Components"}:
        raise ValueError(f"APT index lacks required release fields: {path}")
    if fields["Components"] != COMPONENTS:
        raise ValueError(f"APT release components differ from the source: {path}")
    if set(package_metadata) != set(COMPONENTS.split()):
        raise ValueError(f"APT release lacks package metadata: {path}")
    return fields, package_metadata


def decompressed_package_metadata(path):
    if (
        APT_HELPER.is_symlink()
        or not APT_HELPER.is_file()
        or not os.access(APT_HELPER, os.X_OK)
    ):
        raise ValueError("trusted apt-helper is unavailable")
    try:
        process = subprocess.Popen(
            [str(APT_HELPER), "cat-file", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
        )
    except OSError as error:
        raise ValueError("cannot start trusted apt-helper") from error

    digest = hashlib.sha256()
    size = 0
    assert process.stdout is not None
    with process.stdout:
        for chunk in iter(lambda: process.stdout.read(1024 * 1024), b""):
            digest.update(chunk)
            size += len(chunk)
    if process.wait() != 0:
        raise ValueError(f"apt-helper could not read package index: {path}")
    return digest.hexdigest(), size


def verify_indices(path, snapshot, snapshot_time, series, architecture):
    if path.is_symlink() or not path.is_dir():
        raise ValueError("APT index directory is unsafe")
    if not ARCHITECTURE_RE.fullmatch(architecture):
        raise ValueError("invalid Debian architecture")
    prefix = f"snapshot.ubuntu.com_ubuntu_{snapshot}_dists_"
    expected = {
        f"{prefix}{suite}_InRelease": suite
        for _host, suite in expected_sources(series)
    }
    entries = {entry.name: entry for entry in path.iterdir()}
    release_names = {
        name for name in entries if name.endswith("_InRelease")
    }
    if release_names != set(expected):
        raise ValueError("APT snapshot release index set is not exact")
    snapshot_releases = [entries[name] for name in sorted(expected)]

    allowed_package_names = set()
    for _host, suite in expected_sources(series):
        for component in COMPONENTS.split():
            stem = (
                f"{prefix}{suite}_{component}_binary-{architecture}_Packages"
            )
            allowed_package_names.update(
                f"{stem}{suffix}" for suffix in PACKAGE_INDEX_SUFFIXES
            )
    observed_package_names = {
        name for name in entries if "_Packages" in name
    }
    if not observed_package_names.issubset(allowed_package_names):
        raise ValueError("APT package index set contains unexpected entries")

    observed = set()
    package_metadata_by_suite = {}
    for release in snapshot_releases:
        if release.is_symlink() or not release.is_file():
            raise ValueError(f"unsafe APT release index: {release}")
        fields, package_metadata = release_metadata(release, architecture)
        suite = fields["Suite"]
        if suite != expected[release.name] or suite in observed:
            raise ValueError("APT release index set contains an unexpected suite")
        release_time = parsedate_to_datetime(fields["Date"])
        if release_time.tzinfo is None:
            raise ValueError("APT release date has no timezone")
        if release_time.astimezone(timezone.utc) > snapshot_time:
            raise ValueError("APT release index is newer than the requested snapshot")
        observed.add(suite)
        package_metadata_by_suite[suite] = package_metadata

    for _host, suite in expected_sources(series):
        package_metadata = package_metadata_by_suite[suite]
        for component in COMPONENTS.split():
            expected_digest, expected_size = package_metadata[component]
            stem = (
                f"{prefix}{suite}_{component}_binary-{architecture}_Packages"
            )
            matches = [
                entries[f"{stem}{suffix}"]
                for suffix in PACKAGE_INDEX_SUFFIXES
                if f"{stem}{suffix}" in entries
            ]
            if len(matches) > 1:
                raise ValueError(f"APT package index is duplicated: {stem}")
            if expected_size == 0 and not matches:
                continue
            if len(matches) != 1:
                raise ValueError(f"APT package index is incomplete: {stem}")
            package_index = matches[0]
            if package_index.is_symlink() or not package_index.is_file():
                raise ValueError(f"unsafe APT package index: {package_index}")
            actual_digest, actual_size = decompressed_package_metadata(
                package_index
            )
            if actual_size != expected_size or actual_digest != expected_digest:
                raise ValueError(
                    "APT package index differs from signed metadata: "
                    f"{package_index}"
                )


def verify(sources, lists, snapshot, apt_version, series, architecture):
    if parse_apt_version(apt_version) < MINIMUM_APT_VERSION:
        raise ValueError("apt does not support fail-closed Ubuntu snapshots")
    snapshot_time = parse_snapshot(snapshot)
    verify_sources(sources, snapshot, series)
    verify_source_parts(sources.parent / "sources.list.d")
    verify_indices(lists, snapshot, snapshot_time, series, architecture)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sources", type=Path, required=True)
    parser.add_argument("--lists", type=Path, required=True)
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--apt-version", required=True)
    parser.add_argument("--series", required=True)
    parser.add_argument("--architecture", required=True)
    arguments = parser.parse_args()
    verify(
        arguments.sources,
        arguments.lists,
        arguments.snapshot,
        arguments.apt_version,
        arguments.series,
        arguments.architecture,
    )


if __name__ == "__main__":
    main()
