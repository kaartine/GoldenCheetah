#!/usr/bin/env python3

import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
import unicodedata
import zipfile


MAX_MEMBERS = 200000
MAX_UNCOMPRESSED_BYTES = 32 * 1024 * 1024 * 1024
WINDOWS_RESERVED = {
    "con", "prn", "aux", "nul",
    *(f"com{number}" for number in range(1, 10)),
    *(f"lpt{number}" for number in range(1, 10)),
}


def safe_relative(name, strip_components):
    if not isinstance(name, str) or not name or "\\" in name or "\x00" in name:
        raise ValueError("archive member has an unsafe name")
    normalized_name = unicodedata.normalize("NFC", name)
    if normalized_name != name:
        raise ValueError("archive member name is not Unicode-normalized")
    raw_name = normalized_name.rstrip("/")
    raw_parts = raw_name.split("/")
    path = PurePosixPath(raw_name)
    if path.is_absolute() or any(
        part in {"", ".", ".."} for part in raw_parts
    ):
        raise ValueError("archive member escapes extraction root")
    parts = path.parts
    if len(parts) <= strip_components:
        return None
    parts = parts[strip_components:]
    for part in parts:
        stem = part.split(".", 1)[0].casefold()
        if (
            ":" in part
            or part.endswith((" ", "."))
            or stem in WINDOWS_RESERVED
        ):
            raise ValueError("archive member is not portable across target filesystems")
    return PurePosixPath(*parts)


def register_member(seen, relative, is_directory):
    if relative is None:
        return
    folded = tuple(part.casefold() for part in relative.parts)
    missing = object()
    existing = seen.get(folded, missing)
    if existing is not missing:
        if existing is None and is_directory:
            seen[folded] = True
            return
        if not is_directory and existing in {None, True}:
            raise ValueError("archive replaces a directory with a regular file")
        raise ValueError("archive contains duplicate or case-colliding members")
    for count in range(1, len(folded)):
        parent = folded[:count]
        parent_kind = seen.get(parent, missing)
        if parent_kind is False:
            raise ValueError("archive places a member below a regular file")
        if parent_kind is missing:
            seen[parent] = None
    seen[folded] = is_directory


def copy_bounded(source, destination, expected_size):
    copied = 0
    with destination.open("xb") as output:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            copied += len(block)
            if copied > expected_size:
                raise ValueError("archive member exceeds its declared size")
            output.write(block)
    if copied != expected_size:
        raise ValueError("archive member does not match its declared size")


def prepare_target(stage, relative, is_directory, executable=False):
    target = stage.joinpath(*relative.parts)
    target.parent.mkdir(parents=True, exist_ok=True)
    if is_directory:
        target.mkdir(mode=0o755, exist_ok=True)
    else:
        return target, 0o755 if executable else 0o644
    return target, 0o755


def extract_tar(archive_path, stage, strip_components):
    with tarfile.open(archive_path, mode="r:*") as archive:
        members = archive.getmembers()
        if not members or len(members) > MAX_MEMBERS:
            raise ValueError("archive has an invalid member count")
        seen = {}
        planned = []
        total = 0
        for member in members:
            if not (member.isdir() or member.isreg()):
                raise ValueError("archive contains a link or special member")
            relative = safe_relative(member.name, strip_components)
            if relative is None:
                if member.isreg():
                    raise ValueError("strip-components removes a regular member name")
                continue
            register_member(seen, relative, member.isdir())
            total += member.size if member.isreg() else 0
            if total > MAX_UNCOMPRESSED_BYTES:
                raise ValueError("archive is too large")
            planned.append((member, relative))
        for member, relative in planned:
            target, mode = prepare_target(
                stage, relative, member.isdir(), bool(member.mode & 0o111)
            )
            if member.isreg():
                source = archive.extractfile(member)
                if source is None:
                    raise ValueError("archive regular member has no payload")
                with source:
                    copy_bounded(source, target, member.size)
            os.chmod(target, mode)


def zip_member_kind(member):
    mode = member.external_attr >> 16
    if member.is_dir():
        return "directory", False
    if member.flag_bits & 1:
        raise ValueError("encrypted archive members are unsupported")
    if member.create_system == 3 and mode:
        kind = stat.S_IFMT(mode)
        if kind not in {0, stat.S_IFREG}:
            raise ValueError("archive contains a link or special member")
    return "file", bool(mode & 0o111)


def extract_zip(archive_path, stage, strip_components):
    with zipfile.ZipFile(archive_path, mode="r") as archive:
        members = archive.infolist()
        if not members or len(members) > MAX_MEMBERS:
            raise ValueError("archive has an invalid member count")
        seen = {}
        planned = []
        total = 0
        for member in members:
            kind, executable = zip_member_kind(member)
            relative = safe_relative(member.filename, strip_components)
            if relative is None:
                if kind == "file":
                    raise ValueError("strip-components removes a regular member name")
                continue
            register_member(seen, relative, kind == "directory")
            total += member.file_size if kind == "file" else 0
            if total > MAX_UNCOMPRESSED_BYTES:
                raise ValueError("archive is too large")
            planned.append((member, relative, kind, executable))
        for member, relative, kind, executable in planned:
            target, mode = prepare_target(
                stage, relative, kind == "directory", executable
            )
            if kind == "file":
                with archive.open(member, mode="r") as source:
                    copy_bounded(source, target, member.file_size)
            os.chmod(target, mode)


def extract_archive(archive_path, destination, archive_format, strip_components):
    archive_path = archive_path.absolute()
    destination = destination.absolute()
    if archive_path.is_symlink() or not archive_path.is_file():
        raise ValueError("archive must be a regular file")
    if destination.exists() or destination.is_symlink():
        raise ValueError("archive destination must not exist")
    destination.parent.mkdir(parents=True, exist_ok=True)
    parent = destination.parent.resolve(strict=True)
    if not parent.is_dir():
        raise ValueError("archive destination parent is unsafe")
    destination = parent / destination.name
    stage = Path(tempfile.mkdtemp(prefix=destination.name + ".stage.", dir=parent))
    try:
        if archive_format == "tar":
            extract_tar(archive_path, stage, strip_components)
        elif archive_format == "zip":
            extract_zip(archive_path, stage, strip_components)
        else:
            raise ValueError("unsupported archive format")
        os.replace(stage, destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--format", choices=("tar", "zip"), required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--strip-components", type=int, default=0)
    arguments = parser.parse_args()
    if arguments.strip_components < 0 or arguments.strip_components > 32:
        raise ValueError("invalid strip-components value")
    extract_archive(
        arguments.archive,
        arguments.destination,
        arguments.format,
        arguments.strip_components,
    )


if __name__ == "__main__":
    main()
