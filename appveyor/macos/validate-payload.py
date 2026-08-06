#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
VERSION_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.+-]*$")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def contains_bytes(path, needle):
    overlap = max(len(needle) - 1, 0)
    previous = b""
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            data = previous + block
            if needle in data:
                return True
            previous = data[-overlap:] if overlap else b""
    return False


def safe_symlink(path, root):
    target = os.readlink(path)
    if not target or "\x00" in target or "\r" in target or "\n" in target:
        raise ValueError(f"invalid bundle symlink target: {path}")
    if Path(target).is_absolute():
        raise ValueError(f"absolute bundle symlink: {path}")
    try:
        path.resolve(strict=True).relative_to(root)
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        raise ValueError(f"bundle symlink escapes or is dangling: {path}") from error
    return target


def payload_entries(bundle, forbidden_prefixes):
    entries = []
    for directory, directory_names, file_names in os.walk(bundle, followlinks=False):
        directory_names.sort()
        file_names.sort()
        directory_path = Path(directory)
        for name in list(directory_names):
            path = directory_path / name
            if path.is_symlink():
                entries.append(
                    {
                        "path": path.relative_to(bundle).as_posix(),
                        "target": safe_symlink(path, bundle),
                        "type": "symlink",
                    }
                )
                directory_names.remove(name)
        for name in file_names:
            path = directory_path / name
            relative = path.relative_to(bundle).as_posix()
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                entries.append(
                    {
                        "path": relative,
                        "target": safe_symlink(path, bundle),
                        "type": "symlink",
                    }
                )
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError(f"unsupported special file in bundle: {relative}")
            for forbidden in forbidden_prefixes:
                if contains_bytes(path, forbidden):
                    raise ValueError(
                        f"bundle file retains forbidden build path: {relative}"
                    )
            entries.append(
                {
                    "mode": "{:04o}".format(stat.S_IMODE(metadata.st_mode)),
                    "path": relative,
                    "sha256": sha256_file(path),
                    "size": metadata.st_size,
                    "type": "file",
                }
            )
    entries.sort(key=lambda entry: entry["path"])
    return entries


def parse_formula(value, core_commit):
    fields = value.split("=", 3)
    if len(fields) == 3:
        name, version, receipt_name = fields
        license_name = "NOASSERTION"
    elif len(fields) == 4:
        name, version, license_name, receipt_name = fields
    else:
        raise ValueError("formula must be NAME=VERSION[=LICENSE]=RECEIPT")
    if (
        not re.fullmatch(r"[A-Za-z0-9@+_.-]+", name)
        or not VERSION_RE.fullmatch(version)
        or not license_name
    ):
        raise ValueError("invalid formula provenance value")
    receipt = Path(receipt_name)
    if receipt.is_symlink() or not receipt.is_file():
        raise ValueError(f"missing or linked Homebrew receipt: {receipt}")
    if receipt.stat().st_size > 1024 * 1024:
        raise ValueError("Homebrew receipt is unexpectedly large")
    with receipt.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if document.get("source", {}).get("tap_git_head") != core_commit:
        raise ValueError(f"Homebrew receipt is not from the pinned core: {name}")
    return {
        "license": license_name,
        "name": name,
        "provenance": {
            "homebrew_core_commit": core_commit,
            "poured_from_bottle": bool(document.get("poured_from_bottle")),
            "receipt_sha256": sha256_file(receipt),
        },
        "version": version,
    }


def atomic_json_write(path, document):
    path = path.absolute()
    if path.is_symlink():
        raise ValueError("refusing linked macOS provenance output")
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".tmp.", dir=path.parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, 0o644)
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


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--homebrew-core-commit", required=True)
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--formula", action="append", default=[])
    parser.add_argument("--forbidden-prefix", action="append", default=[])
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    bundle = arguments.bundle.resolve(strict=True)
    if arguments.bundle.is_symlink() or not bundle.is_dir():
        raise ValueError("macOS bundle must be a real directory")
    output = arguments.output.absolute()
    try:
        output.relative_to(bundle)
    except ValueError:
        pass
    else:
        raise ValueError("macOS provenance output must be outside the bundle")
    if not REVISION_RE.fullmatch(arguments.homebrew_core_commit):
        raise ValueError("invalid Homebrew core revision")
    if not VERSION_RE.fullmatch(arguments.qt_version):
        raise ValueError("invalid Qt version")

    forbidden_prefixes = []
    for value in arguments.forbidden_prefix:
        if not value or "\x00" in value or "\r" in value or "\n" in value:
            raise ValueError("invalid forbidden path prefix")
        encoded = value.encode("utf-8")
        if encoded not in forbidden_prefixes:
            forbidden_prefixes.append(encoded)
    formulas = [
        parse_formula(value, arguments.homebrew_core_commit)
        for value in arguments.formula
    ]
    if len({entry["name"] for entry in formulas}) != len(formulas):
        raise ValueError("duplicate Homebrew formula provenance")
    formulas.sort(key=lambda entry: entry["name"])
    components = formulas + [
        {
            "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
            "name": "Qt",
            "provenance": {
                "kind": "CI-provided SDK",
                "release_inputs_verified": False,
            },
            "version": arguments.qt_version,
        }
    ]
    components.sort(key=lambda entry: entry["name"])
    atomic_json_write(
        output,
        {
            "components": components,
            "format": "goldencheetah-macos-payload-provenance-1",
            "homebrew_core_commit": arguments.homebrew_core_commit,
            "payload": payload_entries(bundle, forbidden_prefixes),
        },
    )


if __name__ == "__main__":
    main()
