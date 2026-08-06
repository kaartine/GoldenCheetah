#!/usr/bin/env python3

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
from typing import NamedTuple
import unicodedata
from urllib.parse import urljoin, urlparse


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class LockedArchive(NamedTuple):
    sha256: str
    install_path: str
    repository_path: str


class UpdaterConfig(NamedTuple):
    version: str
    target: str
    arch: str
    os_name: str
    installed_desktop_arch_dir: str | None
    base_url: str


class QtLock(NamedTuple):
    updater: UpdaterConfig
    entries: list[LockedArchive]


def validate_repository_path(value):
    if not isinstance(value, str) or not value.isascii():
        raise ValueError("Qt archive path must be ASCII")
    if "\\" in value or "\x00" in value:
        raise ValueError("Qt archive path contains an unsafe character")
    path = PurePosixPath(value)
    if (
        path.is_absolute()
        or any(part in {"", ".", ".."} for part in value.split("/"))
        or not value.startswith("online/qtsdkrepository/")
        or path.suffix != ".7z"
        or path.name != value.rsplit("/", 1)[-1]
    ):
        raise ValueError("Qt archive path is not a safe repository path")
    return value


def parse_lock(path):
    entries = []
    updater = None
    repository_paths = set()
    archive_names = set()
    for line_number, raw_line in enumerate(
        Path(path).read_text(encoding="ascii").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("@updater "):
            fields = line.split()
            if updater is not None or len(fields) != 7:
                raise ValueError(f"invalid Qt updater lock on line {line_number}")
            version, target, arch, os_name, installed, base_url = fields[1:]
            if any(
                not value.isascii() or not value or value in {".", ".."}
                for value in (version, target, arch, os_name)
            ):
                raise ValueError("invalid Qt updater lock")
            parsed_base = urlparse(base_url)
            if (
                parsed_base.scheme != "https"
                or parsed_base.username
                or parsed_base.password
                or parsed_base.query
                or parsed_base.fragment
                or parsed_base.path not in {"", "/"}
            ):
                raise ValueError("invalid Qt updater repository base")
            updater = UpdaterConfig(
                version, target, arch, os_name,
                None if installed == "-" else installed,
                base_url.rstrip("/"),
            )
            continue
        fields = line.split()
        if len(fields) != 3 or not SHA256_RE.fullmatch(fields[0]):
            raise ValueError(f"invalid Qt lock entry on line {line_number}")
        install_path = fields[1]
        safe_install_destination(Path("/opt/Qt"), install_path)
        repository_path = validate_repository_path(fields[2])
        archive_name = PurePosixPath(repository_path).name
        if repository_path in repository_paths or archive_name in archive_names:
            raise ValueError("duplicate Qt archive path or filename")
        repository_paths.add(repository_path)
        archive_names.add(archive_name)
        entries.append(LockedArchive(fields[0], install_path, repository_path))
    if updater is None or not entries:
        raise ValueError("Qt archive lock is empty")
    return QtLock(updater, entries)


def safe_install_destination(output_dir, relative_path):
    if not isinstance(relative_path, str) or "\\" in relative_path:
        raise ValueError("unsafe Qt archive installation path")
    if relative_path:
        parts = relative_path.split("/")
        if any(part in {"", ".", ".."} for part in parts):
            raise ValueError("unsafe Qt archive installation path")
        relative = PurePosixPath(relative_path)
        if relative.is_absolute():
            raise ValueError("unsafe Qt archive installation path")
    else:
        relative = PurePosixPath()
    root = Path(output_dir).resolve()
    destination = (root / Path(*relative.parts)).resolve()
    try:
        destination.relative_to(root)
    except ValueError as error:
        raise ValueError("Qt archive installation path escapes output") from error
    return destination


def validate_package_plan(entries, packages, output_dir=Path("/opt/Qt")):
    planned = {}
    for package in packages:
        repository_path = validate_repository_path(package.archive_path)
        if repository_path in planned:
            raise ValueError("aqt metadata contains a duplicate Qt archive")
        safe_install_destination(output_dir, package.archive_install_path)
        planned[repository_path] = package
    locked_paths = {entry.repository_path for entry in entries}
    if set(planned) != locked_paths:
        missing = sorted(locked_paths - set(planned))
        unexpected = sorted(set(planned) - locked_paths)
        raise ValueError(
            f"aqt metadata differs from the reviewed lock; "
            f"missing={missing}, unexpected={unexpected}"
        )
    for entry in entries:
        package = planned[entry.repository_path]
        if package.archive_install_path != entry.install_path:
            raise ValueError(
                "aqt metadata archive installation path differs from the reviewed lock"
            )
    return planned


def validate_updater_target(expected, target):
    actual = (
        str(getattr(target, "version", "")),
        str(getattr(target, "target", "")),
        str(getattr(target, "arch", "")),
        str(getattr(target, "os_name", "")),
    )
    locked = (expected.version, expected.target, expected.arch, expected.os_name)
    if actual != locked:
        raise ValueError(f"aqt updater target differs from lock: {actual!r}")


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def download_https(url, destination):
    import requests

    parsed = urlparse(url)
    if (
        parsed.scheme != "https"
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError("Qt archive URL is not an uncredentialed HTTPS URL")
    with requests.get(
        url, allow_redirects=True, stream=True, timeout=(15, 180)
    ) as response:
        response.raise_for_status()
        final = urlparse(response.url)
        if (
            final.scheme != "https"
            or final.username
            or final.password
            or final.query
            or final.fragment
        ):
            raise ValueError("Qt archive download redirected to an unsafe URL")
        descriptor = os.open(
            destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600
        )
        try:
            with os.fdopen(descriptor, "wb") as stream:
                for block in response.iter_content(chunk_size=1024 * 1024):
                    if block:
                        stream.write(block)
                stream.flush()
                os.fsync(stream.fileno())
        except BaseException:
            try:
                os.close(descriptor)
            except OSError:
                pass
            raise


def validate_archive_member(name):
    if not isinstance(name, str) or not name or "\\" in name or "\x00" in name:
        raise ValueError("Qt archive contains an unsafe member name")
    if unicodedata.normalize("NFC", name) != name:
        raise ValueError("Qt archive member is not Unicode-normalized")
    normalized = name[:-1] if name.endswith("/") else name
    if not normalized:
        raise ValueError("Qt archive contains an unsafe member name")
    path = PurePosixPath(normalized)
    if path.is_absolute() or any(
        part in {"", ".", ".."} for part in normalized.split("/")
    ):
        raise ValueError("Qt archive member escapes its extraction root")


def register_archive_layout_member(seen, normalized, is_directory):
    missing = object()
    existing = seen.get(normalized, missing)
    if existing is not missing:
        if existing is None and is_directory:
            seen[normalized] = True
            return
        if not is_directory and existing in {None, True}:
            raise ValueError("Qt archive replaces a directory with a file")
        raise ValueError("Qt archive contains duplicate members")

    parts = normalized.split("/")
    for count in range(1, len(parts)):
        parent = "/".join(parts[:count])
        parent_kind = seen.get(parent, missing)
        if parent_kind is False:
            raise ValueError("Qt archive places a member below a file")
        if parent_kind is missing:
            seen[parent] = None
    seen[normalized] = is_directory


def resolve_archive_symlink_target(link_name, target, archive_payloads):
    if (
        not isinstance(target, str)
        or not target
        or "\\" in target
        or "\x00" in target
        or unicodedata.normalize("NFC", target) != target
    ):
        raise ValueError("Qt archive symlink has an unsafe target")
    target_path = PurePosixPath(target)
    if target_path.is_absolute():
        raise ValueError("Qt archive symlink escapes its extraction root")

    resolved = list(PurePosixPath(link_name).parent.parts)
    for part in target.split("/"):
        if part in {"", "."}:
            if part == "":
                raise ValueError("Qt archive symlink has an unsafe target")
            continue
        if part == "..":
            if not resolved:
                raise ValueError("Qt archive symlink escapes its extraction root")
            resolved.pop()
            continue
        resolved.append(part)
    resolved_name = PurePosixPath(*resolved).as_posix()
    if resolved_name not in archive_payloads:
        raise ValueError("Qt archive symlink target is not in the archive")
    return resolved_name


def extract_verified_archive(archive_path, destination):
    import py7zr

    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        raise ValueError("unsafe Qt archive destination")
    with py7zr.SevenZipFile(archive_path, mode="r") as archive:
        members = list(archive.files)
        if not members or len(members) > 200000:
            raise ValueError("Qt archive is empty")
        expected_files = set()
        expected_file_names = set()
        expected_directories = set()
        directory_spellings = {}
        expected_sizes = {}
        executable_files = set()
        symlink_members = {}
        path_spellings = {}
        seen = {}
        total_size = 0
        for member in members:
            name = member.filename
            validate_archive_member(name)
            member_name = name.rstrip("/")
            normalized = member_name.casefold()
            is_symlink = bool(getattr(member, "is_symlink", False))
            if bool(getattr(member, "is_junction", False)) or bool(
                getattr(member, "is_socket", False)
            ):
                raise ValueError("Qt archive contains a non-regular member")
            if not (member.is_directory or member.is_file or is_symlink):
                raise ValueError("Qt archive contains a non-regular member")

            parts = member_name.split("/")
            for count in range(1, len(parts) + 1):
                spelling = "/".join(parts[:count])
                folded = spelling.casefold()
                previous = path_spellings.get(folded)
                if previous is not None and previous != spelling:
                    raise ValueError("Qt archive contains case-colliding members")
                path_spellings[folded] = spelling
                if count < len(parts) or member.is_directory:
                    directory_spellings[folded] = spelling
            register_archive_layout_member(
                seen, normalized, member.is_directory
            )
            if member.is_directory:
                expected_directories.add(normalized)
            else:
                size = int(getattr(member, "uncompressed", 0) or 0)
                if size < 0:
                    raise ValueError("Qt archive has an invalid member size")
                expected_sizes[member_name] = size
                if is_symlink:
                    if size > 4096:
                        raise ValueError("Qt archive symlink target is too large")
                    symlink_members[member_name] = None
                else:
                    expected_files.add(normalized)
                    expected_file_names.add(member_name)
                    if int(getattr(member, "posix_mode", 0) or 0) & 0o111:
                        executable_files.add(member_name)
                total_size += size
                if total_size > 32 * 1024 * 1024 * 1024:
                    raise ValueError("Qt archive is unexpectedly large")
        allowed_directories = {
            path for path, kind in seen.items() if kind is not False
        }

        with tempfile.TemporaryDirectory(
            prefix="qt-extract-", dir=destination.parent
        ) as temporary:
            stage = Path(temporary).resolve(strict=True)

            class StagedFile:
                def __init__(self, path, expected_size):
                    self.path = path
                    self.expected_size = expected_size
                    descriptor = os.open(
                        path,
                        os.O_RDWR | os.O_CREAT | os.O_EXCL
                        | getattr(os, "O_NOFOLLOW", 0),
                        0o600,
                    )
                    self.stream = os.fdopen(descriptor, "w+b")

                def write(self, data):
                    if self.stream.tell() + len(data) > self.expected_size:
                        raise ValueError("Qt archive member exceeds its declared size")
                    return self.stream.write(data)

                def read(self, size=None):
                    return self.stream.read(-1 if size is None else size)

                def seek(self, offset, whence=0):
                    return self.stream.seek(offset, whence)

                def flush(self):
                    self.stream.flush()

                def size(self):
                    position = self.stream.tell()
                    self.stream.seek(0, os.SEEK_END)
                    size = self.stream.tell()
                    self.stream.seek(position)
                    return size

                def close(self):
                    if self.stream.closed:
                        return
                    self.stream.flush()
                    if self.size() != self.expected_size:
                        self.stream.close()
                        raise ValueError(
                            "Qt archive member differs from its declared size"
                        )
                    self.stream.close()

            class StagedFileFactory:
                def __init__(self):
                    self.products = {}

                def create(self, filename):
                    requested = Path(filename)
                    if requested.is_absolute():
                        try:
                            requested = requested.relative_to(stage)
                        except ValueError as error:
                            raise ValueError(
                                "Qt extractor requested a member outside its staging root"
                            ) from error
                    relative_name = requested.as_posix()
                    if (
                        relative_name not in expected_sizes
                        or relative_name in self.products
                    ):
                        raise ValueError(
                            "Qt extractor requested an unexpected member: "
                            f"{relative_name!r}"
                        )
                    relative = PurePosixPath(relative_name)
                    path = stage.joinpath(*relative.parts)
                    path.parent.mkdir(parents=True, exist_ok=True)
                    product = StagedFile(path, expected_sizes[relative_name])
                    self.products[relative_name] = product
                    return product

                def close_all(self):
                    for product in self.products.values():
                        if not product.stream.closed:
                            product.stream.close()

            for spelling in sorted(
                directory_spellings.values(),
                key=lambda value: (value.count("/"), value),
            ):
                stage.joinpath(*PurePosixPath(spelling).parts).mkdir(
                    parents=True, exist_ok=True
                )
            factory = StagedFileFactory()
            try:
                archive.extractall(path=stage, factory=factory)
            finally:
                factory.close_all()
            if set(factory.products) != set(expected_sizes):
                raise ValueError("Qt extraction omitted an archive member")

            extracted_files = set()
            extracted_directories = set()
            for root, directories, files in os.walk(stage, followlinks=False):
                root_path = Path(root)
                for name in directories:
                    path = root_path / name
                    if path.is_symlink() or not path.is_dir():
                        raise ValueError("Qt extraction produced an unsafe directory")
                    extracted_directories.add(
                        path.relative_to(stage).as_posix().casefold()
                    )
                for name in files:
                    path = root_path / name
                    info = path.lstat()
                    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                        raise ValueError("Qt extraction produced a link or special file")
                    extracted_files.add(
                        path.relative_to(stage).as_posix().casefold()
                    )
            if (
                extracted_files
                != expected_files
                | {name.casefold() for name in symlink_members}
                or not expected_directories.issubset(extracted_directories)
                or not extracted_directories.issubset(allowed_directories)
            ):
                raise ValueError("Qt extraction output differs from its member table")

            archive_payloads = expected_file_names | set(symlink_members)
            resolved_symlinks = {}
            for link_name in symlink_members:
                link_payload = stage.joinpath(
                    *PurePosixPath(link_name).parts
                ).read_bytes()
                try:
                    target = link_payload.decode("utf-8")
                except UnicodeDecodeError as error:
                    raise ValueError(
                        "Qt archive symlink target is not UTF-8"
                    ) from error
                resolved_symlinks[link_name] = (
                    target,
                    resolve_archive_symlink_target(
                        link_name, target, archive_payloads
                    ),
                )
            for link_name in resolved_symlinks:
                visited = set()
                current = link_name
                while current in resolved_symlinks:
                    if current in visited:
                        raise ValueError("Qt archive contains a symlink cycle")
                    visited.add(current)
                    current = resolved_symlinks[current][1]
                if current not in expected_file_names:
                    raise ValueError("Qt archive symlink does not resolve to a file")

            destination.mkdir(parents=True, exist_ok=True)
            destination = destination.resolve(strict=True)

            def ensure_directory(relative):
                current = destination
                for part in relative.parts:
                    current = current / part
                    if current.exists() or current.is_symlink():
                        if current.is_symlink() or not current.is_dir():
                            raise ValueError("Qt archive merge has an unsafe ancestor")
                    else:
                        current.mkdir(mode=0o755)
                return current

            for source in sorted(stage.rglob("*")):
                relative = source.relative_to(stage)
                if source.is_dir():
                    ensure_directory(relative)
                    continue
                if relative.as_posix() in symlink_members:
                    continue
                target_parent = ensure_directory(relative.parent)
                target = target_parent / relative.name
                if target.exists() or target.is_symlink():
                    raise ValueError("Qt archives contain colliding files")
                with source.open("rb") as input_stream, target.open("xb") as output_stream:
                    shutil.copyfileobj(input_stream, output_stream, 1024 * 1024)
                os.chmod(
                    target,
                    0o755 if relative.as_posix() in executable_files else 0o644,
                )

            for link_name, (raw_target, _resolved) in sorted(
                resolved_symlinks.items()
            ):
                relative = PurePosixPath(link_name)
                target_parent = ensure_directory(Path(*relative.parent.parts))
                target = target_parent / relative.name
                if target.exists() or target.is_symlink():
                    raise ValueError("Qt archives contain colliding symlinks")
                target.symlink_to(raw_target)
            for link_name in resolved_symlinks:
                link = destination.joinpath(*PurePosixPath(link_name).parts)
                try:
                    link.resolve(strict=True).relative_to(destination)
                except (OSError, ValueError) as error:
                    raise ValueError(
                        "Qt archive symlink escapes the installed Qt root"
                    ) from error


def install_archives(
    entries,
    packages,
    base_url,
    archive_dir,
    output_dir,
    *,
    downloader=download_https,
    extractor=extract_verified_archive,
):
    parsed_base = urlparse(base_url)
    if (
        parsed_base.scheme != "https"
        or parsed_base.username
        or parsed_base.password
        or parsed_base.query
        or parsed_base.fragment
    ):
        raise ValueError("Qt repository base must be uncredentialed HTTPS")
    output_dir = Path(output_dir)
    archive_dir = Path(archive_dir)
    if archive_dir.exists() and (archive_dir.is_symlink() or not archive_dir.is_dir()):
        raise ValueError("unsafe Qt archive staging directory")
    archive_dir.mkdir(parents=True, mode=0o700, exist_ok=True)
    os.chmod(archive_dir, 0o700)
    if output_dir.is_symlink() or (
        output_dir.exists() and not output_dir.is_dir()
    ):
        raise ValueError("unsafe Qt output directory")
    if output_dir.exists():
        try:
            next(output_dir.iterdir())
        except StopIteration:
            pass
        else:
            raise ValueError("Qt output directory is not empty")
    else:
        output_dir.mkdir(parents=True)

    planned = validate_package_plan(entries, packages, output_dir)
    verified = []
    try:
        for entry in entries:
            name = PurePosixPath(entry.repository_path).name
            final_path = archive_dir / name
            temporary = archive_dir / (name + ".part")
            if final_path.exists() or final_path.is_symlink() or temporary.exists():
                raise ValueError("Qt archive staging path is not clean")
            url = urljoin(base_url.rstrip("/") + "/", entry.repository_path)
            downloader(url, temporary)
            if temporary.is_symlink() or not temporary.is_file():
                raise ValueError("Qt downloader did not create a regular file")
            if sha256_file(temporary) != entry.sha256:
                raise ValueError(f"Qt archive digest mismatch: {name}")
            os.replace(temporary, final_path)
            verified.append((entry, final_path))

        # This loop is deliberately separate: no extraction is reachable until
        # every archive in the reviewed set has passed its pinned digest.
        for entry, archive_path in verified:
            package = planned[entry.repository_path]
            destination = safe_install_destination(output_dir, entry.install_path)
            extractor(archive_path, destination)
    except BaseException:
        for path in archive_dir.glob("*.part"):
            if path.is_file() and not path.is_symlink():
                path.unlink()
        raise


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--base", default="https://download.qt.io")
    parser.add_argument("--version", required=True)
    parser.add_argument("--arch", required=True)
    parser.add_argument("--expected-count", required=True, type=int)
    parser.add_argument("--module", action="append", default=[])
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    lock = parse_lock(arguments.lock)
    if len(lock.entries) != arguments.expected_count:
        raise ValueError("Qt archive lock has an unexpected number of entries")
    if (
        arguments.version != lock.updater.version
        or arguments.arch != lock.updater.arch
        or arguments.base.rstrip("/") != lock.updater.base_url
    ):
        raise ValueError("Qt installer arguments differ from the reviewed lock")

    from aqt.archives import QtArchives
    from aqt.updater import Updater

    archives = QtArchives(
        lock.updater.os_name,
        lock.updater.target,
        lock.updater.version,
        lock.updater.arch,
        base=arguments.base,
        modules=arguments.module,
        timeout=(15, 180),
    )
    target = archives.get_target_config()
    validate_updater_target(lock.updater, target)
    with tempfile.TemporaryDirectory(prefix="verified-qt-") as temporary:
        install_archives(
            lock.entries,
            archives.get_packages(),
            arguments.base,
            Path(temporary),
            arguments.output,
        )
    Updater.update(
        target, arguments.output, lock.updater.installed_desktop_arch_dir
    )


if __name__ == "__main__":
    main()
