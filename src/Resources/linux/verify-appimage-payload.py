#!/usr/bin/env python3
"""Verify that CycloneDX payload-file entries exactly cover an AppDir."""

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys


EMBEDDED_SBOM = PurePosixPath(
    "usr/share/goldencheetah/goldencheetah.cdx.json"
)


def properties(component):
    values = {}
    for entry in component.get("properties", []):
        if not isinstance(entry, dict) or set(entry) != {"name", "value"}:
            raise ValueError("invalid payload component property")
        name = entry["name"]
        value = entry["value"]
        if not isinstance(name, str) or not isinstance(value, str) or name in values:
            raise ValueError("duplicate or invalid payload component property")
        values[name] = value
    return values


def relative_name(value):
    if not isinstance(value, str) or not value or not value.isascii():
        raise ValueError("invalid payload component path")
    path = PurePosixPath(value)
    if path.is_absolute() or value != path.as_posix() or any(part in ("", ".", "..") for part in path.parts):
        raise ValueError("unsafe payload component path")
    return path


def payload_components(document):
    result = {}
    components = document.get("components")
    if not isinstance(components, list):
        raise ValueError("SBOM components are missing")
    for component in components:
        if not isinstance(component, dict) or component.get("type") != "file":
            continue
        component_properties = properties(component)
        role = component_properties.get("goldencheetah:role")
        if role not in {"payload-file", "payload-symlink"}:
            continue
        name = relative_name(component.get("name"))
        if name == EMBEDDED_SBOM or name in result:
            raise ValueError("duplicate or excluded payload component")
        result[name] = (component, component_properties, role)
    return result


def enumerate_payload(appdir):
    result = {}
    for root, directory_names, file_names in os.walk(appdir, followlinks=False):
        directory_names.sort()
        file_names.sort()
        root_path = Path(root)
        linked_directories = [
            name for name in directory_names if (root_path / name).is_symlink()
        ]
        for name in linked_directories:
            directory_names.remove(name)
            path = root_path / name
            result[PurePosixPath(path.relative_to(appdir).as_posix())] = path
        for name in file_names:
            path = root_path / name
            relative = PurePosixPath(path.relative_to(appdir).as_posix())
            if relative == EMBEDDED_SBOM:
                continue
            metadata = path.lstat()
            if not (stat.S_ISREG(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode)):
                raise ValueError("payload contains an unsupported special file")
            result[relative] = path
    return result


def one_sha256(component):
    hashes = component.get("hashes")
    if not isinstance(hashes, list) or len(hashes) != 1:
        raise ValueError("payload file must have one SHA-256 hash")
    entry = hashes[0]
    if not isinstance(entry, dict) or set(entry) != {"alg", "content"}:
        raise ValueError("invalid payload file hash")
    digest = entry.get("content")
    if entry.get("alg") != "SHA-256" or not isinstance(digest, str):
        raise ValueError("invalid payload file hash")
    if len(digest) != 64 or any(character not in "0123456789abcdef" for character in digest):
        raise ValueError("invalid payload file hash")
    return digest


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def resolve_symlink(appdir, path):
    target = os.readlink(path)
    if not target or "\0" in target or "\r" in target or "\n" in target:
        raise ValueError("invalid payload symlink target")
    if PurePosixPath(target).is_absolute():
        raise ValueError("absolute payload symlink target")
    try:
        resolved = path.resolve(strict=True)
        resolved.relative_to(appdir)
    except (FileNotFoundError, RuntimeError, ValueError):
        raise ValueError("payload symlink escapes or is dangling")
    return target


def verify(appdir, sbom):
    if appdir.is_symlink() or not appdir.is_dir():
        raise ValueError("invalid AppDir")
    if sbom.is_symlink() or not sbom.is_file():
        raise ValueError("invalid SBOM")
    with sbom.open(encoding="utf-8") as stream:
        document = json.load(stream)
    expected = payload_components(document)
    actual = enumerate_payload(appdir)
    if set(expected) != set(actual):
        missing = sorted(str(path) for path in set(actual) - set(expected))
        stale = sorted(str(path) for path in set(expected) - set(actual))
        raise ValueError(
            "payload coverage mismatch; missing={} stale={}".format(
                ",".join(missing[:5]), ",".join(stale[:5])
            )
        )
    for relative in sorted(actual, key=str):
        path = actual[relative]
        component, component_properties, role = expected[relative]
        metadata = path.lstat()
        if stat.S_ISREG(metadata.st_mode):
            if role != "payload-file":
                raise ValueError("regular payload represented as a symlink")
            if one_sha256(component) != sha256(path):
                raise ValueError("payload file digest mismatch")
            if component_properties.get("goldencheetah:size") != str(metadata.st_size):
                raise ValueError("payload file size mismatch")
            if component_properties.get("goldencheetah:mode") != f"{stat.S_IMODE(metadata.st_mode):04o}":
                raise ValueError("payload file mode mismatch")
        elif stat.S_ISLNK(metadata.st_mode):
            if role != "payload-symlink" or "hashes" in component:
                raise ValueError("symlink payload represented as a file")
            if component_properties.get("goldencheetah:symlink-target") != resolve_symlink(appdir, path):
                raise ValueError("payload symlink target mismatch")
        else:
            raise ValueError("unsupported payload file type")


def main(arguments):
    if len(arguments) != 2:
        print("usage: verify-appimage-payload.py APPDIR SBOM", file=sys.stderr)
        return 2
    try:
        verify(Path(arguments[0]).resolve(strict=True), Path(arguments[1]))
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"invalid AppImage payload: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
