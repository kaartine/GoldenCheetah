#!/usr/bin/env python3
"""Bind linuxdeployqt-modified libraries to pre-deployment system sources."""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
BUILD_ID_RE = re.compile(r"^[0-9a-f]{16,128}$")
LDCONFIG_LINE_RE = re.compile(r"^\s*(\S+)\s+\([^)]*\)\s+=>\s+(\S+)\s*$")
SNAPSHOT_FORMAT = "goldencheetah-linuxdeployqt-source-snapshot-1"
MANIFEST_FORMAT = "goldencheetah-transformed-runtime-1"
MAX_SNAPSHOT_ENTRIES = 100000


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def stable_sha256(path):
    before = path.stat()
    digest = sha256_file(path)
    after = path.stat()
    identity = lambda value: (
        value.st_dev,
        value.st_ino,
        value.st_mode,
        value.st_size,
        value.st_mtime_ns,
        value.st_ctime_ns,
    )
    if identity(before) != identity(after):
        raise ValueError(f"runtime source changed while hashing: {path}")
    return digest


def atomic_json(path, document, mode=0o600):
    output = path.absolute()
    if output.exists() or output.is_symlink():
        raise ValueError(f"output already exists: {output}")
    parent = output.parent.resolve(strict=True)
    output = parent / output.name
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=output.name + ".tmp.", dir=parent
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(document, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, output)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def canonical_regular_file(path, description):
    candidate = Path(path)
    if not candidate.is_absolute() or candidate.is_symlink():
        raise ValueError(f"{description} must be a canonical regular file")
    resolved = candidate.resolve(strict=True)
    if resolved != candidate or not resolved.is_file():
        raise ValueError(f"{description} must be a canonical regular file")
    return resolved


def ldconfig_source_entries():
    ldconfig = shutil.which("ldconfig")
    if ldconfig is None:
        raise ValueError("ldconfig is unavailable")
    result = subprocess.run(
        [ldconfig, "-p"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        env={"LC_ALL": "C", "PATH": os.environ.get("PATH", "")},
    )
    entries = {}
    digests = {}

    def add_entry(soname, source):
        key = (soname, str(source))
        digest = digests.get(source)
        if digest is None:
            digest = stable_sha256(source)
            digests[source] = digest
        entries[key] = {
            "path": str(source),
            "sha256": digest,
            "soname": soname,
        }
        if len(entries) > MAX_SNAPSHOT_ENTRIES:
            raise ValueError("runtime source snapshot is unexpectedly large")

    for line in result.stdout.splitlines():
        match = LDCONFIG_LINE_RE.fullmatch(line)
        if match is None:
            continue
        soname, raw_path = match.groups()
        if (
            not soname.isascii()
            or "/" in soname
            or "\\" in soname
            or "\x00" in soname
        ):
            raise ValueError("ldconfig returned an unsafe SONAME")
        source_argument = Path(raw_path)
        try:
            source = source_argument.resolve(strict=True)
        except FileNotFoundError as error:
            raise ValueError("ldconfig source is unavailable") from error
        if not source.is_absolute() or source.is_symlink() or not source.is_file():
            raise ValueError("ldconfig source is not a regular file")
        add_entry(soname, source)

    dpkg_query = shutil.which("dpkg-query")
    if dpkg_query is None:
        raise ValueError("dpkg-query is unavailable")
    package_files = subprocess.run(
        [dpkg_query, "-S", "*.so", "*.so.*"],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        env={"LC_ALL": "C", "PATH": os.environ.get("PATH", "")},
    )
    for line in package_files.stdout.splitlines():
        _owner, separator, raw_path = line.partition(": ")
        if not separator:
            raise ValueError("dpkg-query returned an invalid library path")
        source_argument = Path(raw_path)
        soname = source_argument.name
        if not (soname.endswith(".so") or ".so." in soname):
            continue
        if (
            not source_argument.is_absolute()
            or not soname.isascii()
            or "/" in soname
            or "\\" in soname
            or "\x00" in soname
        ):
            raise ValueError("dpkg-query returned an unsafe library path")
        try:
            source = source_argument.resolve(strict=True)
        except FileNotFoundError:
            continue
        if source.is_symlink() or not source.is_file():
            continue
        add_entry(soname, source)

    if not entries:
        raise ValueError("runtime source snapshot is empty")
    return [entries[key] for key in sorted(entries)]


def snapshot_document(entries):
    return {"format": SNAPSHOT_FORMAT, "libraries": entries}


def load_snapshot(document):
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "libraries"}
        or document["format"] != SNAPSHOT_FORMAT
        or not isinstance(document["libraries"], list)
        or len(document["libraries"]) > MAX_SNAPSHOT_ENTRIES
    ):
        raise ValueError("invalid linuxdeployqt source snapshot")
    entries = []
    keys = []
    for entry in document["libraries"]:
        if not isinstance(entry, dict) or set(entry) != {
            "path", "sha256", "soname"
        }:
            raise ValueError("invalid linuxdeployqt source snapshot entry")
        source = canonical_regular_file(entry["path"], "runtime source")
        digest = entry["sha256"]
        soname = entry["soname"]
        if (
            not isinstance(digest, str)
            or not SHA256_RE.fullmatch(digest)
            or not isinstance(soname, str)
            or not soname.isascii()
            or not soname
            or "/" in soname
            or "\\" in soname
            or "\x00" in soname
        ):
            raise ValueError("invalid linuxdeployqt source snapshot entry")
        key = (soname, str(source))
        keys.append(key)
        entries.append({"path": source, "sha256": digest, "soname": soname})
    if keys != sorted(set(keys)):
        raise ValueError("linuxdeployqt source snapshot is not unique and sorted")
    return entries


def command_text(arguments):
    result = subprocess.run(
        arguments,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        env={"LC_ALL": "C", "PATH": os.environ.get("PATH", "")},
    )
    return result.stdout.strip()


def default_elf_identity(path):
    readelf = shutil.which("readelf")
    patchelf = shutil.which("patchelf")
    if readelf is None or patchelf is None:
        raise ValueError("ELF provenance tools are unavailable")
    notes = command_text([readelf, "-n", str(path)])
    build_ids = re.findall(r"(?m)^\s*Build ID: ([0-9a-f]+)\s*$", notes)
    if len(build_ids) != 1 or not BUILD_ID_RE.fullmatch(build_ids[0]):
        raise ValueError(f"ELF build-id is unavailable: {path}")
    soname = command_text([patchelf, "--print-soname", str(path)])
    rpath = command_text([patchelf, "--print-rpath", str(path)])
    if (
        not soname.isascii()
        or "/" in soname
        or "\\" in soname
        or "\x00" in soname
        or not rpath.isascii()
        or "\x00" in rpath
    ):
        raise ValueError(f"ELF dynamic identity is invalid: {path}")
    return {"build_id": build_ids[0], "rpath": rpath, "soname": soname}


def appdir_libraries(appdir):
    root_argument = Path(appdir)
    if root_argument.is_symlink():
        raise ValueError("AppDir is a symlink")
    root = root_argument.resolve(strict=True)
    library_root = root / "lib"
    if library_root.is_symlink() or not library_root.is_dir():
        raise ValueError("AppDir library root is unsafe")
    libraries = []
    for relative_root in ("lib", "plugins", "qml"):
        payload_root = root / relative_root
        if not payload_root.exists():
            continue
        if payload_root.is_symlink() or not payload_root.is_dir():
            raise ValueError("AppDir library root is unsafe")
        for path in payload_root.rglob("*"):
            if path.is_symlink() or not path.is_file():
                continue
            if not (path.name.endswith(".so") or ".so." in path.name):
                continue
            resolved = path.resolve(strict=True)
            if resolved != path.absolute():
                raise ValueError("AppDir library path is not canonical")
            libraries.append(resolved)
    return root, sorted(
        libraries, key=lambda item: item.relative_to(root).as_posix()
    )


def qt_source_for_output(qt_root, appdir, output):
    if qt_root is None:
        return None
    root_argument = Path(qt_root)
    if root_argument.is_symlink():
        raise ValueError("Qt SDK root is a symlink")
    root = root_argument.resolve(strict=True)
    if not root.is_dir():
        raise ValueError("Qt SDK root is not a directory")
    relative = output.relative_to(appdir)
    if relative.parts[0] not in {"plugins", "qml"}:
        return None
    candidate = root / relative
    if candidate.is_symlink() or not candidate.is_file():
        return None
    source = candidate.resolve(strict=True)
    try:
        source.relative_to(root)
    except ValueError as error:
        raise ValueError("Qt SDK runtime path escapes its root") from error
    if source != candidate.absolute():
        raise ValueError("Qt SDK runtime path is not canonical")
    return source


def appdir_relative_lib_rpath(appdir, output):
    relative = os.path.relpath(appdir / "lib", output.parent)
    if relative == ".":
        return "$ORIGIN"
    if relative.startswith("/") or "\\" in relative:
        raise ValueError("cannot derive AppDir-relative library RPATH")
    return "$ORIGIN/" + relative


def build_transformed_entries(
    appdir,
    snapshot,
    linuxdeployqt_sha256,
    *,
    qt_root=None,
    elf_identity=default_elf_identity,
    authenticate_source,
):
    if (
        not isinstance(linuxdeployqt_sha256, str)
        or not SHA256_RE.fullmatch(linuxdeployqt_sha256)
    ):
        raise ValueError("invalid linuxdeployqt SHA-256")
    sources = load_snapshot(snapshot)
    for entry in sources:
        if stable_sha256(entry["path"]) != entry["sha256"]:
            raise ValueError(f"linuxdeployqt source changed: {entry['path']}")
    by_soname = {}
    for entry in sources:
        by_soname.setdefault(entry["soname"], []).append(entry)

    root, outputs = appdir_libraries(appdir)
    transformed = []
    for output in outputs:
        output_identity = elf_identity(output)
        output_soname = output_identity.get("soname")
        candidates = by_soname.get(output_soname, [])
        output_digest = stable_sha256(output)
        if any(entry["sha256"] == output_digest for entry in candidates):
            continue
        matching = []
        for entry in candidates:
            source_identity = elf_identity(entry["path"])
            if (
                source_identity.get("build_id") == output_identity.get("build_id")
                and source_identity.get("soname") == entry["soname"]
                and output_soname == entry["soname"]
                and output_identity.get("rpath") == "$ORIGIN"
            ):
                matching.append(entry)
        digests = {entry["sha256"] for entry in matching}
        if matching:
            if len(digests) != 1:
                raise ValueError(
                    f"linuxdeployqt source identity is ambiguous: {output.name}"
                )
            source = sorted(matching, key=lambda entry: str(entry["path"]))[0]
            authenticate_source(source["path"])
            transformed.append(
                {
                    "output_sha256": output_digest,
                    "path": output.relative_to(root).as_posix(),
                    "source_path": str(source["path"]),
                    "source_sha256": source["sha256"],
                    "transformation": (
                        f"linuxdeployqt-no-strip:{linuxdeployqt_sha256}:"
                        "rpath=$ORIGIN"
                    ),
                }
            )
            continue

        qt_source = qt_source_for_output(qt_root, root, output)
        if qt_source is None:
            continue
        source_digest = stable_sha256(qt_source)
        if source_digest == output_digest:
            continue
        source_identity = elf_identity(qt_source)
        if (
            source_identity.get("build_id") != output_identity.get("build_id")
            or source_identity.get("soname") != output_soname
            or output_identity.get("rpath")
            != appdir_relative_lib_rpath(root, output)
        ):
            continue
        transformed.append(
            {
                "output_sha256": output_digest,
                "path": output.relative_to(root).as_posix(),
                "source_path": str(qt_source),
                "source_sha256": source_digest,
                "transformation": (
                    f"linuxdeployqt-no-strip:{linuxdeployqt_sha256}:"
                    "rpath=relative-lib"
                ),
            }
        )
    paths = [entry["path"] for entry in transformed]
    if paths != sorted(set(paths)):
        raise ValueError("linuxdeployqt transformed paths are not unique and sorted")
    return transformed


def load_python_module(path):
    source = canonical_regular_file(path, "runtime provenance tool")
    spec = importlib.util.spec_from_file_location("runtime_provenance", source)
    if spec is None or spec.loader is None:
        raise ValueError("cannot load runtime provenance tool")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_json(path, description):
    source = canonical_regular_file(path, description)
    if source.stat().st_size > 32 * 1024 * 1024:
        raise ValueError(f"{description} is unexpectedly large")
    with source.open(encoding="utf-8") as stream:
        return json.load(stream)


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    snapshot_parser = subparsers.add_parser("snapshot")
    snapshot_parser.add_argument("--output", required=True, type=Path)
    finalize_parser = subparsers.add_parser("finalize")
    finalize_parser.add_argument("--appdir", required=True, type=Path)
    finalize_parser.add_argument("--snapshot", required=True, type=Path)
    finalize_parser.add_argument("--output", required=True, type=Path)
    finalize_parser.add_argument("--linuxdeployqt-sha256", required=True)
    finalize_parser.add_argument("--provenance-tool", required=True, type=Path)
    finalize_parser.add_argument("--qt-root", required=True, type=Path)
    arguments = parser.parse_args()

    if arguments.command == "snapshot":
        atomic_json(
            arguments.output,
            snapshot_document(ldconfig_source_entries()),
        )
        return

    provenance = load_python_module(arguments.provenance_tool)
    snapshot = load_json(arguments.snapshot, "linuxdeployqt source snapshot")
    entries = build_transformed_entries(
        arguments.appdir,
        snapshot,
        arguments.linuxdeployqt_sha256,
        qt_root=arguments.qt_root,
        authenticate_source=provenance.resolve_debian_package,
    )
    atomic_json(
        arguments.output,
        {"format": MANIFEST_FORMAT, "libraries": entries},
    )


if __name__ == "__main__":
    main()
