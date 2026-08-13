#!/usr/bin/env python3

import argparse
import base64
import binascii
import csv
from email.parser import BytesParser
from email.policy import default
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import zipfile


SCRIPT_WRAPPER = (
    b"#!/bin/sh\n"
    b'""":"\n'
    b'exec "$(dirname "$0")/python3.11" "$0" "$@"\n'
    b'":"""\n'
)
MAX_CONSOLE_SCRIPT_BYTES = 4 * 1024 * 1024
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
VERSION_RE = re.compile(r"^[^\s\x00\r\n]+$")
PACKAGE_RE = re.compile(r"^([A-Za-z0-9_.-]+)==([^\s\\]+)\s*\\?$")
HASH_RE = re.compile(r"^--hash=sha256:([0-9a-f]{64})\s*\\?$")
MAX_WHEEL_FILES = 100000
MAX_WHEEL_FILE_BYTES = 1024 * 1024 * 1024


def require_under(path, root):
    try:
        path.relative_to(root)
    except (ValueError, binascii.Error) as error:
        raise ValueError("embedded Python path escapes its root") from error


def atomic_write(path, data, mode):
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".tmp.", dir=str(path.parent)
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def normalize_scripts(root):
    bin_directory = root / "bin"
    if not bin_directory.is_dir() or bin_directory.is_symlink():
        raise ValueError("embedded Python bin directory is unsafe")

    changed = set()
    for path in sorted(bin_directory.iterdir()):
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            require_under(path.resolve(strict=True), root)
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("embedded Python bin contains a special file")
        if metadata.st_size > MAX_CONSOLE_SCRIPT_BYTES:
            continue
        data = path.read_bytes()
        first_line, separator, body = data.partition(b"\n")
        if not separator or not first_line.startswith(b"#!"):
            continue
        if b"python" not in first_line.lower():
            continue
        atomic_write(
            path,
            SCRIPT_WRAPPER + body,
            stat.S_IMODE(metadata.st_mode),
        )
        changed.add(path.resolve(strict=True))
    return changed


def record_digest(path):
    digest = hashlib.sha256(path.read_bytes()).digest()
    encoded = base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")
    return "sha256=" + encoded


def update_records(root, changed):
    changed_records = set()
    for record in sorted(root.rglob("*.dist-info/RECORD")):
        if record.is_symlink() or not record.is_file():
            raise ValueError("embedded Python RECORD is unsafe")
        require_under(record.resolve(strict=True), root)
        site_packages = record.parent.parent
        rows = list(csv.reader(io.StringIO(record.read_text(encoding="utf-8"))))
        updated = False
        for row in rows:
            if len(row) != 3:
                raise ValueError("embedded Python RECORD is malformed")
            candidate = (site_packages / row[0]).resolve(strict=False)
            require_under(candidate, root)
            if candidate in changed:
                row[1] = record_digest(candidate)
                row[2] = str(candidate.stat().st_size)
                updated = True
        if not updated:
            continue
        output = io.StringIO(newline="")
        csv.writer(output, lineterminator="\n").writerows(rows)
        atomic_write(
            record,
            output.getvalue().encode("utf-8"),
            stat.S_IMODE(record.stat().st_mode),
        )
        changed_records.add(record.resolve(strict=True))
    return changed_records


def reject_build_path(root, forbidden_prefix):
    forbidden = forbidden_prefix.encode("utf-8")
    if not forbidden:
        raise ValueError("empty forbidden build path")
    for path in sorted(root.rglob("*")):
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            require_under(path.resolve(strict=True), root)
            continue
        if not stat.S_ISREG(metadata.st_mode):
            if not stat.S_ISDIR(metadata.st_mode):
                raise ValueError("embedded Python contains a special file")
            continue
        with path.open("rb") as stream:
            overlap = b""
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                searchable = overlap + block
                if forbidden in searchable:
                    relative = path.relative_to(root).as_posix()
                    raise ValueError(
                        "embedded Python retains its build path in " + relative
                    )
                overlap_size = max(len(forbidden) - 1, 0)
                overlap = (
                    searchable[-overlap_size:] if overlap_size else b""
                )


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def normalized_python_name(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_requirements_lock(path):
    packages = []
    current = None

    def finish_package():
        if current is None:
            return
        if not current["hashes"]:
            raise ValueError("locked Python package has no artifact hashes")
        packages.append(current.copy())

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        package_match = PACKAGE_RE.fullmatch(line)
        if package_match:
            finish_package()
            current = {
                "name": normalized_python_name(package_match.group(1)),
                "version": package_match.group(2),
                "hashes": [],
            }
            continue
        hash_match = HASH_RE.fullmatch(line)
        if hash_match and current is not None:
            current["hashes"].append(hash_match.group(1))
            continue
        raise ValueError("unsupported entry in Python requirements lock")
    finish_package()
    if not packages:
        raise ValueError("Python requirements lock is empty")
    by_name = {package["name"]: package for package in packages}
    if len(by_name) != len(packages):
        raise ValueError("duplicate normalized Python package name")
    return by_name


def remove_locked_source_distributions(python_root, requirements_lock):
    if python_root.is_symlink():
        raise ValueError("embedded Python root is a symlink")
    root = python_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("embedded Python root is not a directory")
    if requirements_lock.is_symlink() or not requirements_lock.is_file():
        raise ValueError("Python requirements lock is unsafe")
    locked = parse_requirements_lock(requirements_lock.resolve(strict=True))
    candidates = sorted(root.glob("lib/python*/site-packages"))
    if len(candidates) != 1 or candidates[0].is_symlink():
        raise ValueError("embedded Python site-packages directory is ambiguous")
    site_packages = candidates[0].resolve(strict=True)
    require_under(site_packages, root)
    if not site_packages.is_dir():
        raise ValueError("embedded Python site-packages directory is unsafe")

    claimed = {}
    replaced = {}
    for dist_info in sorted(site_packages.glob("*.dist-info")):
        if dist_info.is_symlink() or not dist_info.is_dir():
            raise ValueError("embedded Python distribution metadata is unsafe")
        metadata_path = dist_info / "METADATA"
        record_path = dist_info / "RECORD"
        for path in (metadata_path, record_path):
            if (
                path.is_symlink()
                or not path.is_file()
                or path.stat().st_size > 16 * 1024 * 1024
            ):
                raise ValueError("embedded Python distribution metadata is unsafe")
        metadata = BytesParser(policy=default).parsebytes(metadata_path.read_bytes())
        name = normalized_python_name((metadata.get("Name") or "").strip())
        if name not in locked:
            continue
        if not name or name in replaced:
            raise ValueError("locked Python source distribution is ambiguous")
        replaced[name] = dist_info

        rows = csv.reader(io.StringIO(record_path.read_text(encoding="utf-8")))
        record_claims = set()
        for row in rows:
            if len(row) != 3 or not row[0] or "\\" in row[0] or "\x00" in row[0]:
                raise ValueError("embedded Python source RECORD is malformed")
            relative = PurePosixPath(row[0])
            if relative.is_absolute():
                raise ValueError("embedded Python source RECORD path escapes its root")
            lexical = Path(
                os.path.abspath(site_packages.joinpath(*relative.parts))
            )
            try:
                lexical.relative_to(root)
            except ValueError as error:
                raise ValueError(
                    "embedded Python source RECORD path escapes its root"
                ) from error
            if lexical == root:
                raise ValueError(
                    "embedded Python source RECORD path escapes its root"
                )
            if not os.path.lexists(lexical):
                continue
            try:
                parent = lexical.parent.resolve(strict=True)
            except FileNotFoundError as error:
                raise ValueError(
                    "embedded Python source RECORD path is unavailable"
                ) from error
            require_under(parent, root)
            candidate = parent / lexical.name
            if candidate in record_claims:
                raise ValueError("embedded Python source RECORD has duplicate paths")
            record_claims.add(candidate)
            previous = claimed.setdefault(candidate, name)
            if previous != name:
                raise ValueError(
                    "embedded Python source distributions have overlapping paths"
                )

    for path in sorted(claimed, key=lambda item: (len(item.parts), str(item)), reverse=True):
        try:
            metadata = path.lstat()
        except FileNotFoundError as error:
            raise ValueError(
                "embedded Python source RECORD path is unavailable"
            ) from error
        if not (stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)):
            raise ValueError("embedded Python source RECORD names a non-file")

    removable_directories = set()
    for path in claimed:
        path.unlink()
        parent = path.parent
        while parent != root and parent != site_packages:
            removable_directories.add(parent)
            parent = parent.parent
    for directory in sorted(
        removable_directories,
        key=lambda item: (len(item.parts), str(item)),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass
    for name, dist_info in replaced.items():
        if dist_info.exists() or dist_info.is_symlink():
            raise ValueError(
                f"locked Python source distribution has unclaimed files: {name}"
            )


def package_license(metadata):
    license_name = (metadata.get("License-Expression") or "").strip()
    if not license_name:
        declared = (metadata.get("License") or "").strip().splitlines()
        if declared and len(declared[0]) <= 256:
            license_name = declared[0]
    if not license_name:
        classifiers = metadata.get_all("Classifier", [])
        licenses = [
            value.rsplit(" :: ", 1)[-1]
            for value in classifiers
            if value.startswith("License ::")
        ]
        license_name = ", ".join(sorted(set(licenses)))
    if not license_name:
        license_name = "LicenseRef-python-package-metadata-undeclared"
    return license_name


def checked_wheel_member(name):
    if not name or "\\" in name or "\x00" in name:
        raise ValueError("wheel contains an invalid member path")
    relative = PurePosixPath(name)
    if relative.is_absolute() or any(
        part in {"", ".", ".."} for part in name.split("/")
    ):
        raise ValueError("wheel contains an unsafe member path")
    return relative


def record_sha256(value, description):
    if not value.startswith("sha256="):
        raise ValueError(f"{description} has no SHA-256 identity")
    encoded = value.partition("=")[2]
    try:
        digest = base64.urlsafe_b64decode(
            encoded + "=" * (-len(encoded) % 4)
        ).hex()
    except ValueError as error:
        raise ValueError(f"{description} has malformed SHA-256 identity") from error
    if not SHA256_RE.fullmatch(digest):
        raise ValueError(f"{description} has malformed SHA-256 identity")
    return digest


def wheel_install_path(record_path, dist_info, site_packages, python_root):
    parts = record_path.parts
    if parts[0].endswith(".data"):
        if len(parts) < 3:
            raise ValueError("wheel contains an invalid data-scheme path")
        scheme = parts[1]
        remainder = parts[2:]
        if scheme in {"purelib", "platlib"}:
            destination = site_packages.joinpath(*remainder)
        elif scheme == "scripts":
            destination = python_root.joinpath("bin", *remainder)
        elif scheme == "data":
            destination = python_root.joinpath(*remainder)
        else:
            raise ValueError(
                "wheel runtime library uses an unsupported install scheme"
            )
    else:
        destination = site_packages.joinpath(*parts)
    return destination


def wheel_package(path, locked, payload_root, python_root, site_packages):
    if path.is_symlink() or not path.is_file() or path.suffix != ".whl":
        raise ValueError("wheelhouse contains a non-wheel payload")
    digest = file_sha256(path)
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) > MAX_WHEEL_FILES:
            raise ValueError("wheel contains too many members")
        members = {}
        for info in infos:
            if info.flag_bits & 0x1:
                raise ValueError("wheel contains an encrypted member")
            relative = checked_wheel_member(info.filename.rstrip("/"))
            if info.is_dir():
                continue
            file_type = stat.S_IFMT((info.external_attr >> 16) & 0xFFFF)
            if file_type not in {0, stat.S_IFREG}:
                raise ValueError("wheel contains a non-regular member")
            if info.file_size > MAX_WHEEL_FILE_BYTES:
                raise ValueError("wheel member is unexpectedly large")
            name = relative.as_posix()
            if name in members:
                raise ValueError("wheel contains duplicate members")
            members[name] = info

        metadata_names = sorted(
            name
            for name in members
            if len(PurePosixPath(name).parts) == 2
            and PurePosixPath(name).parts[0].endswith(".dist-info")
            and PurePosixPath(name).name == "METADATA"
        )
        if len(metadata_names) != 1:
            raise ValueError("wheel must contain one top-level METADATA file")
        metadata_name = metadata_names[0]
        dist_info = PurePosixPath(metadata_name).parts[0]
        record_name = f"{dist_info}/RECORD"
        if record_name not in members:
            raise ValueError("wheel has no authenticated RECORD")
        metadata_bytes = archive.read(metadata_name)
        metadata = BytesParser(policy=default).parsebytes(metadata_bytes)
        name = normalized_python_name(metadata.get("Name", "").strip())
        version = metadata.get("Version", "").strip()
        package = locked.get(name)
        if (
            package is None
            or version != package["version"]
            or digest not in package["hashes"]
            or not VERSION_RE.fullmatch(version)
        ):
            raise ValueError("wheel artifact is outside the reviewed lock")

        record_bytes = archive.read(record_name)
        rows = list(csv.reader(io.StringIO(record_bytes.decode("utf-8"))))
        identities = {}
        for row in rows:
            if len(row) != 3 or not row[0]:
                raise ValueError("wheel RECORD is malformed")
            record_path = checked_wheel_member(row[0])
            record_value = record_path.as_posix()
            if record_value in identities:
                raise ValueError("wheel RECORD contains duplicate paths")
            if record_value == record_name:
                if row[1:] != ["", ""]:
                    raise ValueError("wheel RECORD self-entry is malformed")
                identities[record_value] = None
                continue
            info = members.get(record_value)
            if info is None or not row[2].isdigit():
                raise ValueError("wheel RECORD references a missing member")
            expected_digest = record_sha256(row[1], "wheel RECORD entry")
            data = archive.read(info)
            if (
                len(data) != int(row[2])
                or hashlib.sha256(data).hexdigest() != expected_digest
            ):
                raise ValueError("wheel RECORD identity does not match its artifact")
            identities[record_value] = (expected_digest, len(data))

        unsigned = {
            f"{dist_info}/RECORD.jws",
            f"{dist_info}/RECORD.p7s",
        }
        if set(members) - set(identities) - unsigned:
            raise ValueError("wheel contains files absent from its RECORD")
        metadata_identity = identities.get(metadata_name)
        if metadata_identity is None:
            raise ValueError("wheel METADATA has no RECORD identity")

        libraries = []
        for record_name_value, identity in identities.items():
            if identity is None:
                continue
            record_path = PurePosixPath(record_name_value)
            if not (
                record_path.name.endswith(".so") or ".so." in record_path.name
            ):
                continue
            installed = wheel_install_path(
                record_path, dist_info, site_packages, python_root
            )
            try:
                relative = installed.relative_to(payload_root).as_posix()
            except ValueError as error:
                raise ValueError("wheel library installs outside the payload") from error
            libraries.append(
                {
                    "path": relative,
                    "sha256": identity[0],
                    "size": identity[1],
                }
            )
        libraries.sort(key=lambda entry: entry["path"])
        if len({entry["path"] for entry in libraries}) != len(libraries):
            raise ValueError("wheel installs duplicate runtime library paths")

        metadata_path = site_packages / metadata_name
        return {
            "artifact": path.name,
            "license": package_license(metadata),
            "metadata_path": metadata_path.relative_to(payload_root).as_posix(),
            "metadata_sha256": metadata_identity[0],
            "name": name,
            "record_sha256": hashlib.sha256(record_bytes).hexdigest(),
            "runtime_libraries": libraries,
            "sha256": digest,
            "version": version,
        }


def write_wheel_manifest(
    payload_root, python_root, wheelhouse, requirements_lock, output
):
    root = payload_root.resolve(strict=True)
    python = python_root.resolve(strict=True)
    require_under(python, root)
    candidates = sorted(python.glob("lib/python*/site-packages"))
    if len(candidates) != 1 or not candidates[0].is_dir():
        raise ValueError("embedded Python site-packages directory is ambiguous")
    site_packages = candidates[0].resolve(strict=True)
    require_under(site_packages, python)
    wheel_root = wheelhouse.resolve(strict=True)
    if wheelhouse.is_symlink() or not wheel_root.is_dir():
        raise ValueError("Python wheelhouse is unsafe")
    lock = requirements_lock.resolve(strict=True)
    if requirements_lock.is_symlink() or not lock.is_file():
        raise ValueError("Python requirements lock is unsafe")
    locked = parse_requirements_lock(lock)
    packages = [
        wheel_package(path, locked, root, python, site_packages)
        for path in sorted(wheel_root.iterdir(), key=lambda item: item.name)
    ]
    packages.sort(key=lambda package: package["name"])
    names = [package["name"] for package in packages]
    if names != sorted(locked):
        raise ValueError("wheelhouse does not exactly cover the reviewed lock")
    document = {
        "format": "goldencheetah-python-wheel-records-1",
        "requirements_lock_sha256": file_sha256(lock),
        "packages": packages,
    }
    data = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    atomic_write(output.absolute(), data, 0o600)


def capture_runtime_source(payload_root):
    if payload_root.is_symlink():
        raise ValueError("embedded Python payload root is a symlink")
    root = payload_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("embedded Python payload root is not a directory")
    entries = []
    for top_level in (root / "opt", root / "usr"):
        if not top_level.exists():
            continue
        if top_level.is_symlink() or not top_level.is_dir():
            raise ValueError("embedded Python payload directory is unsafe")
        for path in sorted(top_level.rglob("*")):
            metadata = path.lstat()
            relative = path.relative_to(root).as_posix()
            if stat.S_ISLNK(metadata.st_mode):
                require_under(path.resolve(strict=True), root)
                target = os.readlink(path)
                if not target or "\x00" in target or "\r" in target or "\n" in target:
                    raise ValueError("embedded Python symlink target is invalid")
                entries.append(
                    {"kind": "symlink", "path": relative, "target": target}
                )
            elif stat.S_ISREG(metadata.st_mode):
                entries.append(
                    {
                        "kind": "file",
                        "path": relative,
                        "source_sha256": file_sha256(path),
                    }
                )
            elif not stat.S_ISDIR(metadata.st_mode):
                raise ValueError("embedded Python contains a special file")
    if not entries:
        raise ValueError("embedded Python payload is empty")
    paths = [entry["path"] for entry in entries]
    if len(paths) != len(set(paths)):
        raise ValueError("embedded Python source paths are not unique")
    entries.sort(key=lambda entry: entry["path"])
    return entries


def write_runtime_manifest(
    payload_root,
    output,
    source_sha256,
    source_entries,
    changed_scripts,
    changed_records,
):
    if not SHA256_RE.fullmatch(source_sha256):
        raise ValueError("embedded Python source digest is invalid")
    if payload_root.is_symlink():
        raise ValueError("embedded Python payload root is a symlink")
    root = payload_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("embedded Python payload root is not a directory")
    output = output.absolute()
    try:
        output.resolve(strict=False).relative_to(root)
    except ValueError:
        pass
    else:
        raise ValueError("runtime manifest must be outside the Python payload")

    script_paths = {
        path.relative_to(root).as_posix() for path in changed_scripts
    }
    record_paths = {
        path.relative_to(root).as_posix() for path in changed_records
    }
    if script_paths & record_paths:
        raise ValueError("embedded Python path has conflicting transformations")

    files = []
    symlinks = []
    for source_entry in source_entries:
        if not isinstance(source_entry, dict) or "path" not in source_entry:
            raise ValueError("invalid embedded Python source entry")
        relative = source_entry["path"]
        path = root.joinpath(*PurePosixPath(relative).parts)
        if source_entry.get("kind") == "symlink":
            if not path.is_symlink() or os.readlink(path) != source_entry.get("target"):
                raise ValueError(f"embedded Python symlink changed: {relative}")
            require_under(path.resolve(strict=True), root)
            symlinks.append(
                {"path": relative, "target": source_entry["target"]}
            )
            continue
        if set(source_entry) != {"kind", "path", "source_sha256"} or \
                source_entry["kind"] != "file":
            raise ValueError("invalid embedded Python source entry")
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"embedded Python source file changed type: {relative}")
        output_sha256 = file_sha256(path)
        transformation = "identity"
        if relative in script_paths:
            transformation = "python-console-script-wrapper-v1"
        elif relative in record_paths:
            transformation = "python-wheel-record-refresh-v1"
        if transformation == "identity":
            if source_entry["source_sha256"] != output_sha256:
                raise ValueError(
                    f"undeclared embedded Python transformation: {relative}"
                )
        elif source_entry["source_sha256"] == output_sha256:
            raise ValueError(f"empty embedded Python transformation: {relative}")
        files.append(
            {
                "path": relative,
                "source_sha256": source_entry["source_sha256"],
                "output_sha256": output_sha256,
                "transformation": transformation,
            }
        )
    files.sort(key=lambda entry: entry["path"])
    symlinks.sort(key=lambda entry: entry["path"])

    distributions = []
    for metadata_path in sorted(root.rglob("*.dist-info/METADATA")):
        if metadata_path.parent.parent.name not in {"site-packages", "dist-packages"}:
            continue
        metadata = metadata_path.lstat()
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError("source Python distribution metadata is unsafe")
        distributions.append(
            {
                "path": metadata_path.relative_to(root).as_posix(),
                "sha256": file_sha256(metadata_path),
            }
        )
    distributions.sort(key=lambda entry: entry["path"])

    document = {
        "format": "goldencheetah-python-source-runtime-2",
        "source_sha256": source_sha256,
        "distributions": distributions,
        "files": files,
        "symlinks": symlinks,
    }
    data = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )
    atomic_write(output, data, 0o600)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--python-root", required=True, type=Path)
    parser.add_argument("--forbidden-prefix", required=True)
    parser.add_argument("--payload-root", type=Path)
    parser.add_argument("--runtime-manifest", type=Path)
    parser.add_argument("--runtime-sha256")
    parser.add_argument("--wheelhouse", type=Path)
    parser.add_argument("--requirements-lock", type=Path)
    parser.add_argument("--wheel-manifest", type=Path)
    parser.add_argument(
        "--remove-locked-source-distributions", action="store_true"
    )
    arguments = parser.parse_args()

    runtime_arguments = (
        arguments.payload_root,
        arguments.runtime_manifest,
        arguments.runtime_sha256,
    )
    if any(value is not None for value in runtime_arguments) and not all(
        value is not None for value in runtime_arguments
    ):
        raise ValueError("runtime manifest arguments must be provided together")
    wheel_arguments = (
        arguments.payload_root,
        arguments.wheelhouse,
        arguments.requirements_lock,
        arguments.wheel_manifest,
    )
    if any(value is not None for value in wheel_arguments[1:]) and not all(
        value is not None for value in wheel_arguments
    ):
        raise ValueError("wheel manifest arguments must be provided together")

    if arguments.python_root.is_symlink():
        raise ValueError("embedded Python root is a symlink")
    root = arguments.python_root.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("embedded Python root is not a directory")
    if arguments.remove_locked_source_distributions:
        if arguments.requirements_lock is None:
            raise ValueError(
                "removing locked source distributions requires the lock"
            )
        remove_locked_source_distributions(root, arguments.requirements_lock)
    source_entries = (
        capture_runtime_source(arguments.payload_root)
        if arguments.runtime_manifest is not None
        else None
    )
    changed = normalize_scripts(root)
    changed_records = update_records(root, changed)
    reject_build_path(root, arguments.forbidden_prefix)
    if arguments.payload_root is not None:
        if arguments.runtime_manifest is not None:
            write_runtime_manifest(
                arguments.payload_root,
                arguments.runtime_manifest,
                arguments.runtime_sha256,
                source_entries,
                changed,
                changed_records,
            )
        if arguments.wheel_manifest is not None:
            write_wheel_manifest(
                arguments.payload_root,
                root,
                arguments.wheelhouse,
                arguments.requirements_lock,
                arguments.wheel_manifest,
            )


if __name__ == "__main__":
    main()
