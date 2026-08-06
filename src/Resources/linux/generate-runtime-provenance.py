#!/usr/bin/env python3

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import tarfile
import tempfile
from urllib.parse import quote, unquote, urlparse


VERSION_RE = re.compile(r"^[^\s\x00\r\n]+$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
PACKAGE_RE = re.compile(r"^([A-Za-z0-9_.-]+)==([^\s\\]+)\s*\\?$")
HASH_RE = re.compile(r"^--hash=sha256:([0-9a-f]{64})\s*\\?$")
FFMPEG_LIBRARY_RE = re.compile(
    r"^lib(?:avcodec|avdevice|avfilter|avformat|avutil|postproc|swresample|swscale)"
    r"\.so(?:\..*)?$"
)
ICU_LIBRARY_RE = re.compile(
    r"^libicu(?:data|i18n|io|test|tu|uc)\.so\.(\d+\.\d+)$"
)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def runtime_library_paths(appdir):
    paths = [
        path
        for path in appdir.rglob("*")
        if not path.is_symlink()
        and path.is_file()
        and (path.name.endswith(".so") or ".so." in path.name)
    ]
    return sorted(
        paths,
        key=lambda path: path.relative_to(appdir).as_posix(),
    )


def component(path, appdir, name, version, license, purl, provenance):
    values = (name, version, license, purl, provenance)
    if any(not isinstance(value, str) or not value or "\x00" in value for value in values):
        raise ValueError(f"incomplete runtime provenance for {path}")
    if not VERSION_RE.fullmatch(version):
        raise ValueError(f"invalid runtime version for {path}")
    return {
        "license": license,
        "name": name,
        "path": path.relative_to(appdir).as_posix(),
        "provenance": provenance,
        "purl": purl,
        "version": version,
    }


def checked_appdir_relative_path(relative, appdir, description):
    if (
        not isinstance(relative, str)
        or not relative.isascii()
        or "\\" in relative
        or "\x00" in relative
    ):
        raise ValueError(f"invalid {description} path")
    path_value = PurePosixPath(relative)
    if (
        path_value.is_absolute()
        or any(part in {"", ".", ".."} for part in relative.split("/"))
    ):
        raise ValueError(f"unsafe {description} path")
    candidate = appdir.joinpath(*path_value.parts)
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(appdir)
    except (FileNotFoundError, ValueError) as error:
        raise ValueError(f"{description} library is not present in AppDir: {relative}") from error
    if candidate.is_symlink() or resolved != candidate.absolute() or not resolved.is_file():
        raise ValueError(f"{description} library is not a regular AppDir file: {relative}")
    return resolved


def _load_test_fixture_package_index(path, appdir, expected_sha256):
    source = Path(path)
    try:
        resolved_source = source.resolve(strict=True)
    except FileNotFoundError as error:
        raise ValueError("runtime fixture package index is unavailable") from error
    if (
        not source.is_absolute()
        or source.absolute() != resolved_source
        or source.is_symlink()
        or not resolved_source.is_file()
    ):
        raise ValueError("runtime fixture package index path must be canonical")
    if not isinstance(expected_sha256, str) or not SHA256_RE.fullmatch(
        expected_sha256
    ):
        raise ValueError("runtime fixture package index needs a preauthorized SHA-256")
    if sha256_file(resolved_source) != expected_sha256:
        raise ValueError("runtime fixture package index failed its preauthorized SHA-256")
    source = resolved_source
    if source.is_symlink() or not source.is_file():
        raise ValueError("runtime fixture package index must be a regular file")
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("runtime fixture package index is unexpectedly large")
    with source.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "libraries"}
        or document["format"] != "goldencheetah-runtime-fixture-index-1"
        or not isinstance(document["libraries"], list)
        or len(document["libraries"]) > 100000
    ):
        raise ValueError("invalid runtime fixture package index")

    expected_keys = {
        "license", "name", "path", "provenance", "purl", "sha256", "version"
    }
    entries = {}
    paths = []
    for entry in document["libraries"]:
        if not isinstance(entry, dict) or set(entry) != expected_keys:
            raise ValueError("invalid runtime fixture package index entry")
        relative = entry["path"]
        digest = entry["sha256"]
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise ValueError("invalid runtime fixture package index entry")
        candidate = checked_appdir_relative_path(
            relative, appdir, "fixture index"
        )
        if sha256_file(candidate) != digest:
            raise ValueError(f"fixture index digest mismatch: {relative}")
        paths.append(relative)
        entries[relative] = entry
    if paths != sorted(set(paths)):
        raise ValueError("runtime fixture package index paths are not unique and sorted")
    return entries


def load_transformed_runtime_manifest(path, appdir):
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise ValueError("transformed runtime manifest must be a regular file")
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("transformed runtime manifest is unexpectedly large")
    with source.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "libraries"}
        or document["format"] != "goldencheetah-transformed-runtime-1"
        or not isinstance(document["libraries"], list)
        or len(document["libraries"]) > 100000
    ):
        raise ValueError("invalid transformed runtime manifest")

    expected_keys = {
        "output_sha256",
        "path",
        "source_path",
        "source_sha256",
        "transformation",
    }
    entries = {}
    paths = []
    for entry in document["libraries"]:
        if not isinstance(entry, dict) or set(entry) != expected_keys:
            raise ValueError("invalid transformed runtime manifest entry")
        relative = entry["path"]
        source_path_value = entry["source_path"]
        source_digest = entry["source_sha256"]
        output_digest = entry["output_sha256"]
        if (
            not isinstance(source_path_value, str)
            or not source_path_value
            or "\x00" in source_path_value
            or not isinstance(source_digest, str)
            or not SHA256_RE.fullmatch(source_digest)
            or not isinstance(output_digest, str)
            or not SHA256_RE.fullmatch(output_digest)
            or entry["transformation"] != "patchelf-set-rpath:$ORIGIN"
        ):
            raise ValueError("invalid transformed runtime manifest entry")
        output = checked_appdir_relative_path(
            relative, appdir, "transformed runtime"
        )
        source_path = Path(source_path_value)
        if not source_path.is_absolute() or source_path.is_symlink():
            raise ValueError("transformed runtime source must be an absolute regular file")
        try:
            resolved_source = source_path.resolve(strict=True)
        except FileNotFoundError as error:
            raise ValueError("transformed runtime source is unavailable") from error
        if resolved_source != source_path or not resolved_source.is_file():
            raise ValueError("transformed runtime source must be an absolute regular file")
        if sha256_file(resolved_source) != source_digest:
            raise ValueError(f"transformed runtime source digest mismatch: {relative}")
        if sha256_file(output) != output_digest:
            raise ValueError(f"transformed runtime output digest mismatch: {relative}")
        normalized = dict(entry)
        normalized["source_path"] = resolved_source
        paths.append(relative)
        entries[relative] = normalized
    if paths != sorted(set(paths)):
        raise ValueError("transformed runtime manifest paths are not unique and sorted")
    return entries


def load_python_runtime_manifest(path, appdir, expected_source_sha256):
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise ValueError("Python runtime manifest must be a regular file")
    if source.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Python runtime manifest is unexpectedly large")
    with source.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if (
        not SHA256_RE.fullmatch(expected_source_sha256)
        or set(document) != {
            "format", "source_sha256", "distributions", "files", "symlinks"
        }
        or document["format"] != "goldencheetah-python-source-runtime-2"
        or document["source_sha256"] != expected_source_sha256
        or not isinstance(document["distributions"], list)
        or not isinstance(document["files"], list)
        or not isinstance(document["symlinks"], list)
        or len(document["distributions"]) > 100000
        or len(document["files"]) > 1000000
        or len(document["symlinks"]) > 100000
    ):
        raise ValueError("invalid Python runtime manifest")

    def checked_relative(relative, description):
        if (
            not isinstance(relative, str)
            or not relative.isascii()
            or "\\" in relative
            or "\x00" in relative
        ):
            raise ValueError(f"invalid Python {description} manifest entry")
        path_value = PurePosixPath(relative)
        if path_value.is_absolute() or any(
            part in {"", ".", ".."} for part in relative.split("/")
        ):
            raise ValueError(f"Python {description} manifest path is unsafe")
        candidate = appdir.joinpath(*path_value.parts)
        try:
            candidate.absolute().relative_to(appdir)
        except ValueError as error:
            raise ValueError(
                f"Python {description} manifest path escapes AppDir"
            ) from error
        return candidate

    files = []
    files_by_path = {}
    file_paths = []
    allowed_transformations = {
        "identity",
        "python-console-script-wrapper-v1",
        "python-wheel-record-refresh-v1",
    }
    for entry in document["files"]:
        if not isinstance(entry, dict) or set(entry) != {
            "path", "source_sha256", "output_sha256", "transformation"
        }:
            raise ValueError("invalid Python runtime file manifest entry")
        relative = entry["path"]
        candidate = checked_relative(relative, "runtime file")
        source_digest = entry["source_sha256"]
        output_digest = entry["output_sha256"]
        transformation = entry["transformation"]
        if (
            not isinstance(source_digest, str)
            or not SHA256_RE.fullmatch(source_digest)
            or not isinstance(output_digest, str)
            or not SHA256_RE.fullmatch(output_digest)
            or transformation not in allowed_transformations
        ):
            raise ValueError("invalid Python runtime file manifest entry")
        if transformation == "identity" and source_digest != output_digest:
            raise ValueError(f"undeclared Python transformation: {relative}")
        if transformation != "identity" and source_digest == output_digest:
            raise ValueError(f"empty Python transformation: {relative}")
        if candidate.is_symlink() or not candidate.is_file():
            raise ValueError(
                f"Python runtime file is not present in AppDir: {relative}"
            )
        if sha256_file(candidate) != output_digest:
            raise ValueError(f"Python runtime manifest digest mismatch: {relative}")
        normalized = dict(entry)
        files.append(normalized)
        files_by_path[relative] = normalized
        file_paths.append(relative)
    if file_paths != sorted(set(file_paths)):
        raise ValueError("Python runtime file manifest paths are not unique and sorted")

    symlinks = []
    symlink_paths = []
    for entry in document["symlinks"]:
        if not isinstance(entry, dict) or set(entry) != {"path", "target"}:
            raise ValueError("invalid Python runtime symlink manifest entry")
        relative = entry["path"]
        target = entry["target"]
        candidate = checked_relative(relative, "runtime symlink")
        if (
            not isinstance(target, str)
            or not target
            or "\x00" in target
            or "\r" in target
            or "\n" in target
            or PurePosixPath(target).is_absolute()
        ):
            raise ValueError("invalid Python runtime symlink manifest entry")
        if not candidate.is_symlink() or os.readlink(candidate) != target:
            raise ValueError(f"Python runtime symlink target mismatch: {relative}")
        try:
            candidate.resolve(strict=True).relative_to(appdir)
        except (FileNotFoundError, RuntimeError, ValueError) as error:
            raise ValueError(f"Python runtime symlink is unsafe: {relative}") from error
        symlinks.append(dict(entry))
        symlink_paths.append(relative)
    if symlink_paths != sorted(set(symlink_paths)):
        raise ValueError("Python runtime symlink paths are not unique and sorted")
    if set(file_paths) & set(symlink_paths):
        raise ValueError("Python runtime manifest paths have conflicting types")

    def load_distributions(entries):
        expected = {}
        paths = []
        for entry in entries:
            if not isinstance(entry, dict) or set(entry) != {"path", "sha256"}:
                raise ValueError("invalid Python source distribution manifest entry")
            relative = entry["path"]
            digest = entry["sha256"]
            checked_relative(relative, "source distribution")
            if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
                raise ValueError("invalid Python source distribution manifest entry")
            runtime_file = files_by_path.get(relative)
            if runtime_file is None or runtime_file["output_sha256"] != digest:
                raise ValueError(
                    "Python source distribution is not bound to a runtime file"
                )
            paths.append(relative)
            expected[relative] = digest
        if paths != sorted(set(paths)):
            raise ValueError("Python source distribution paths are not unique and sorted")
        return expected

    libraries = {
        entry["path"]: entry["output_sha256"]
        for entry in files
        if entry["path"].endswith(".so") or ".so." in PurePosixPath(entry["path"]).name
    }
    return {
        "distributions": load_distributions(document["distributions"]),
        "files": files,
        "libraries": libraries,
        "symlinks": symlinks,
    }


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
    locked = {package["name"]: package for package in packages}
    if len(locked) != len(packages):
        raise ValueError("duplicate normalized Python package name")
    return locked


def parse_pip_report(path, locked):
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise ValueError("pip installation report must be a regular file")
    with source.open(encoding="utf-8") as stream:
        report = json.load(stream)
    if report.get("version") != "1" or not isinstance(report.get("install"), list):
        raise ValueError("invalid pip installation report")
    selected = {}
    for entry in report["install"]:
        metadata = entry.get("metadata", {})
        name = normalized_python_name(metadata.get("name", ""))
        version = metadata.get("version")
        package = locked.get(name)
        if package is None or version != package["version"] or name in selected:
            raise ValueError("pip installed a package outside the reviewed lock")
        download = entry.get("download_info", {})
        parsed_url = urlparse(download.get("url", ""))
        if parsed_url.scheme not in {"file", "https"} or parsed_url.query or parsed_url.fragment:
            raise ValueError("pip report contains an unsafe artifact URL")
        artifact = PurePosixPath(unquote(parsed_url.path)).name
        digest = download.get("archive_info", {}).get("hashes", {}).get("sha256")
        if (
            not artifact
            or any(character in artifact for character in "\x00\r\n")
            or not isinstance(digest, str)
            or digest not in package["hashes"]
        ):
            raise ValueError("pip selected an artifact outside the reviewed lock")
        selected[name] = {
            "artifact": artifact,
            "name": name,
            "sha256": digest,
            "version": version,
        }
    if set(selected) != set(locked):
        raise ValueError("pip did not install every reviewed Python package")
    return selected


def checked_manifest_file(appdir, relative, digest, description, size=None):
    candidate = checked_appdir_relative_path(relative, appdir, description)
    if sha256_file(candidate) != digest or (
        size is not None and candidate.stat().st_size != size
    ):
        raise ValueError(f"{description} identity does not match payload: {relative}")
    return candidate


def load_python_wheel_manifest(path, appdir, requirements_lock, pip_report):
    source = Path(path)
    if source.is_symlink() or not source.is_file():
        raise ValueError("Python wheel manifest must be a regular file")
    if source.stat().st_size > 64 * 1024 * 1024:
        raise ValueError("Python wheel manifest is unexpectedly large")
    lock = Path(requirements_lock)
    if lock.is_symlink() or not lock.is_file():
        raise ValueError("Python requirements lock must be a regular file")
    locked = parse_requirements_lock(lock)
    selected = parse_pip_report(pip_report, locked)
    with source.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "packages", "requirements_lock_sha256"}
        or document["format"] != "goldencheetah-python-wheel-records-1"
        or document["requirements_lock_sha256"] != sha256_file(lock)
        or not isinstance(document["packages"], list)
        or len(document["packages"]) > 100000
    ):
        raise ValueError("invalid Python wheel manifest")

    expected_package_keys = {
        "artifact", "license", "metadata_path", "metadata_sha256", "name",
        "record_sha256", "runtime_libraries", "sha256", "version",
    }
    owners = {}
    metadata_paths = {}
    packages = []
    for package in document["packages"]:
        if not isinstance(package, dict) or set(package) != expected_package_keys:
            raise ValueError("invalid Python wheel manifest package")
        name = package["name"]
        version = package["version"]
        artifact = package["artifact"]
        digest = package["sha256"]
        metadata_relative = package["metadata_path"]
        metadata_digest = package["metadata_sha256"]
        record_digest = package["record_sha256"]
        license_name = package["license"]
        selected_package = selected.get(name)
        if (
            not isinstance(name, str)
            or normalized_python_name(name) != name
            or not isinstance(version, str)
            or not VERSION_RE.fullmatch(version)
            or not isinstance(artifact, str)
            or not artifact.endswith(".whl")
            or "/" in artifact
            or "\\" in artifact
            or "\x00" in artifact
            or not isinstance(license_name, str)
            or not license_name
            or "\x00" in license_name
            or not isinstance(digest, str)
            or not SHA256_RE.fullmatch(digest)
            or not isinstance(metadata_digest, str)
            or not SHA256_RE.fullmatch(metadata_digest)
            or not isinstance(record_digest, str)
            or not SHA256_RE.fullmatch(record_digest)
            or selected_package is None
            or selected_package != {
                "artifact": artifact,
                "name": name,
                "sha256": digest,
                "version": version,
            }
            or not isinstance(package["runtime_libraries"], list)
        ):
            raise ValueError("invalid Python wheel manifest package")
        metadata_value = PurePosixPath(metadata_relative)
        if (
            metadata_value.name != "METADATA"
            or not metadata_value.parent.name.endswith(".dist-info")
            or metadata_value.parent.parent.name
            not in {"site-packages", "dist-packages"}
        ):
            raise ValueError("Python wheel metadata path is not top-level")
        metadata_file = checked_manifest_file(
            appdir,
            metadata_relative,
            metadata_digest,
            "authenticated wheel metadata",
        )
        if metadata_relative in metadata_paths:
            raise ValueError("Python wheel metadata path has multiple owners")
        metadata_paths[metadata_relative] = metadata_digest
        component_metadata = {
            "license": license_name,
            "name": name,
            "provenance": (
                f"pkg:pypi/{name}@{version}"
                f"#wheel={artifact};wheel-sha256={digest}"
                f";wheel-record-sha256={record_digest}"
            ),
            "purl": f"pkg:pypi/{name}@{version}",
            "version": version,
        }
        library_paths = []
        for library in package["runtime_libraries"]:
            if not isinstance(library, dict) or set(library) != {
                "path", "sha256", "size"
            }:
                raise ValueError("invalid Python wheel runtime library")
            relative = library["path"]
            library_digest = library["sha256"]
            size = library["size"]
            if (
                not isinstance(relative, str)
                or not relative.isascii()
                or not isinstance(library_digest, str)
                or not SHA256_RE.fullmatch(library_digest)
                or not isinstance(size, int)
                or isinstance(size, bool)
                or size < 0
            ):
                raise ValueError("invalid Python wheel runtime library")
            library_file = checked_manifest_file(
                appdir,
                relative,
                library_digest,
                "authenticated wheel runtime library",
                size,
            )
            previous = owners.setdefault(library_file, component_metadata)
            if previous != component_metadata:
                raise ValueError("Python wheel library has multiple owners")
            library_paths.append(relative)
        if library_paths != sorted(set(library_paths)):
            raise ValueError("Python wheel runtime paths are not unique and sorted")
        packages.append(name)
    if packages != sorted(set(packages)) or set(packages) != set(selected):
        raise ValueError("Python wheel manifest does not exactly cover pip report")
    return {"metadata": metadata_paths, "owners": owners}


def command_output(arguments):
    result = subprocess.run(
        arguments,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    return result.stdout


def package_license(package):
    copyright_path = Path("/usr/share/doc") / package.split(":", 1)[0] / "copyright"
    try:
        text = copyright_path.resolve(strict=True).read_text(
            encoding="utf-8", errors="replace"
        )
    except (FileNotFoundError, OSError):
        raise ValueError(f"Debian package has no license evidence: {package}")
    licenses = []
    for match in re.finditer(r"(?m)^License:\s*([^\r\n]+)", text):
        value = match.group(1).strip()
        if value and value not in licenses:
            licenses.append(value)
    evidence_digest = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if licenses:
        license_name = "Debian declared: " + ", ".join(licenses)
    else:
        # Older Debian copyright files are free-form. A content-addressed
        # LicenseRef preserves exact, auditable license evidence without
        # guessing an SPDX identifier from prose.
        license_name = "LicenseRef-debian-copyright-" + evidence_digest
    return license_name, evidence_digest


def installed_package_identity(package):
    fields = command_output(
        [
            "dpkg-query",
            "-W",
            "-f=${binary:Package}\t${source:Package}\t${source:Version}\t${Version}\t${Architecture}",
            package,
        ]
    ).strip().split("\t")
    if len(fields) != 5:
        raise ValueError(f"invalid dpkg metadata for {package}")
    binary, source, source_version, binary_version, architecture = fields
    source = source or binary.split(":", 1)[0]
    source_version = source_version or binary_version
    purl = "pkg:deb/ubuntu/{}@{}?arch={}".format(
        quote(source, safe=".+-"),
        quote(source_version, safe=".+-~:"),
        quote(architecture, safe="_-"),
    )
    license_name, license_digest = package_license(binary)
    metadata = {
        "license": license_name,
        "name": source,
        "provenance": purl + "#debian-copyright-sha256=" + license_digest,
        "purl": purl,
        "version": source_version,
    }
    identity = {
        "architecture": architecture,
        "binary": binary,
        "binary_version": binary_version,
    }
    return metadata, identity


def installed_package_metadata(package):
    return installed_package_identity(package)[0]


def parse_deb822(text):
    paragraphs = []
    for block in re.split(r"\n[ \t]*\n", text.strip()):
        if not block:
            continue
        fields = {}
        current = None
        for line in block.splitlines():
            if line.startswith((" ", "\t")):
                if current is None:
                    raise ValueError("invalid APT package metadata")
                fields[current] += "\n" + line[1:]
                continue
            key, separator, value = line.partition(":")
            if not separator or not key or key in fields:
                raise ValueError("invalid APT package metadata")
            current = key
            fields[key] = value.strip()
        paragraphs.append(fields)
    return paragraphs


def apt_package_record(identity):
    package_spec = "{}={}".format(
        identity["binary"], identity["binary_version"]
    )
    try:
        output = command_output(
            ["apt-cache", "show", "--no-all-versions", package_spec]
        )
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        raise ValueError(
            f"no authenticated .deb metadata for {package_spec}"
        ) from error
    binary_name = identity["binary"].split(":", 1)[0]
    records = []
    for fields in parse_deb822(output):
        if (
            fields.get("Package") != binary_name
            or fields.get("Version") != identity["binary_version"]
            or fields.get("Architecture") != identity["architecture"]
        ):
            continue
        digest = fields.get("SHA256", "")
        filename = fields.get("Filename", "")
        size = fields.get("Size", "")
        if (
            not SHA256_RE.fullmatch(digest)
            or not filename
            or filename.startswith("/")
            or "\\" in filename
            or "\x00" in filename
            or any(part in {"", ".", ".."} for part in filename.split("/"))
            or not size.isdigit()
        ):
            raise ValueError(f"invalid authenticated .deb metadata for {package_spec}")
        records.append((digest, filename, int(size)))
    records = sorted(set(records))
    if len(records) != 1:
        raise ValueError(f"ambiguous authenticated .deb metadata for {package_spec}")
    digest, filename, size = records[0]
    return {
        "filename": filename,
        "sha256": digest,
        "size": size,
        "spec": package_spec,
    }


def deb_regular_file_hashes(path):
    try:
        process = subprocess.Popen(
            ["dpkg-deb", "--fsys-tarfile", str(path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except FileNotFoundError as error:
        raise ValueError("cannot inspect authenticated .deb payload") from error
    hashes = {}
    links = {}
    try:
        if process.stdout is None:
            raise ValueError("cannot inspect authenticated .deb payload")
        with tarfile.open(fileobj=process.stdout, mode="r|*") as archive:
            for member in archive:
                name = member.name
                while name.startswith("./"):
                    name = name[2:]
                name = name.rstrip("/")
                if name in {"", "."} and member.isdir():
                    continue
                value = PurePosixPath(name)
                if (
                    value.is_absolute()
                    or any(part in {"", ".", ".."} for part in name.split("/"))
                ):
                    raise ValueError("authenticated .deb contains an unsafe path")
                if member.issym() or member.islnk():
                    links[name] = member.linkname
                    continue
                if not member.isfile():
                    continue
                stream = archive.extractfile(member)
                if stream is None:
                    raise ValueError("cannot read authenticated .deb member")
                digest = hashlib.sha256()
                size = 0
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
                    size += len(block)
                if size != member.size or name in hashes:
                    raise ValueError("authenticated .deb payload is malformed")
                hashes[name] = digest.hexdigest()
        if process.stdout is not None:
            process.stdout.close()
        stderr = process.stderr.read() if process.stderr is not None else b""
        if process.wait() != 0:
            raise ValueError(
                "cannot inspect authenticated .deb payload: "
                + stderr.decode("utf-8", errors="replace").strip()
            )
    except Exception:
        process.kill()
        process.wait()
        raise
    unresolved = dict(links)
    for _ in range(len(unresolved) + 1):
        progress = False
        for name, target_value in list(unresolved.items()):
            if member_path := target_value.lstrip("/"):
                if not target_value.startswith("/"):
                    member_path = posixpath.join(posixpath.dirname(name), target_value)
                member_path = posixpath.normpath(member_path)
            if (
                not member_path
                or member_path.startswith("../")
                or member_path == ".."
                or "\\" in member_path
            ):
                raise ValueError("authenticated .deb contains an unsafe link")
            digest = hashes.get(member_path)
            if digest is None:
                continue
            hashes[name] = digest
            del unresolved[name]
            progress = True
        if not unresolved or not progress:
            break
    return hashes


DEBIAN_ARTIFACT_CACHE = {}


def authenticated_debian_artifact(identity):
    record = apt_package_record(identity)
    cache_key = (
        identity["binary"],
        identity["binary_version"],
        identity["architecture"],
        record["sha256"],
    )
    if cache_key in DEBIAN_ARTIFACT_CACHE:
        return DEBIAN_ARTIFACT_CACHE[cache_key]
    with tempfile.TemporaryDirectory(prefix="gc-debian-provenance-") as directory:
        try:
            subprocess.run(
                [
                    "apt-get",
                    "-o", "APT::Get::AllowUnauthenticated=false",
                    "-o", "Acquire::AllowInsecureRepositories=false",
                    "-o", "Acquire::AllowDowngradeToInsecureRepositories=false",
                    "download",
                    record["spec"],
                ],
                check=True,
                cwd=directory,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
        except (FileNotFoundError, subprocess.CalledProcessError) as error:
            raise ValueError(
                f"cannot acquire authenticated .deb for {record['spec']}"
            ) from error
        artifacts = sorted(Path(directory).glob("*.deb"))
        if len(artifacts) != 1 or artifacts[0].is_symlink():
            raise ValueError(f"invalid authenticated .deb download for {record['spec']}")
        artifact = artifacts[0]
        if (
            artifact.stat().st_size != record["size"]
            or sha256_file(artifact) != record["sha256"]
        ):
            raise ValueError(f"authenticated .deb digest mismatch for {record['spec']}")
        result = {
            "files": deb_regular_file_hashes(artifact),
            "sha256": record["sha256"],
        }
    DEBIAN_ARTIFACT_CACHE[cache_key] = result
    return result


def candidate_score(payload, installed_path):
    try:
        if sha256_file(payload) == sha256_file(installed_path):
            return 100
    except OSError:
        return -1
    return -1


def resolve_debian_package(path):
    try:
        ownership = command_output(["dpkg-query", "-S", "*/" + path.name])
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        raise ValueError(f"no Debian provenance for runtime library {path.name}") from error
    candidates = []
    for line in ownership.splitlines():
        package, separator, installed_name = line.partition(": ")
        if not separator or Path(installed_name).name != path.name:
            continue
        installed_path = Path(installed_name)
        if not installed_path.is_file():
            continue
        candidates.append((candidate_score(path, installed_path), package, installed_path))
    if not candidates:
        raise ValueError(f"no installed package payload matches {path.name}")
    candidates.sort(reverse=True)
    best_score = candidates[0][0]
    if best_score != 100:
        raise ValueError(
            f"no installed package payload has identical content: {path.name}"
        )
    best = sorted({package for score, package, _ in candidates if score == best_score})
    if len(best) != 1:
        raise ValueError(f"ambiguous Debian provenance for {path.name}: {best}")
    installed_paths = sorted(
        installed_path
        for score, package, installed_path in candidates
        if score == best_score and package == best[0]
    )
    metadata, identity = installed_package_identity(best[0])
    artifact = authenticated_debian_artifact(identity)
    matches = []
    source_digest = sha256_file(path)
    for installed_path in installed_paths:
        member_name = installed_path.as_posix().lstrip("/")
        if artifact["files"].get(member_name) == source_digest:
            matches.append(member_name)
    if not matches:
        raise ValueError(
            f"authenticated .deb payload does not contain {path.name}"
        )
    metadata["provenance"] += (
        f";apt-metadata-sha256={artifact['sha256']}"
        f";deb-sha256={artifact['sha256']}"
        f";deb-member={quote(matches[0], safe='/_.+-')}"
    )
    return metadata


def python_distribution_files(
    appdir,
    wheel_manifest,
    requirements_lock,
    pip_report,
    source_distributions=None,
):
    wheel = load_python_wheel_manifest(
        wheel_manifest, appdir, requirements_lock, pip_report
    )
    source_distributions = source_distributions or {}
    authenticated = dict(source_distributions)
    for relative, digest in wheel["metadata"].items():
        previous = authenticated.setdefault(relative, digest)
        if previous != digest:
            raise ValueError(
                f"Python distribution metadata has conflicting provenance: {relative}"
            )

    observed = set()
    for metadata_path in sorted(appdir.rglob("*.dist-info/METADATA")):
        distribution = metadata_path.parent
        packages_root = distribution.parent
        if packages_root.name not in {"site-packages", "dist-packages"}:
            continue
        if (
            metadata_path.is_symlink()
            or not metadata_path.is_file()
            or distribution.is_symlink()
            or packages_root.is_symlink()
        ):
            raise ValueError(
                f"Python distribution metadata is not regular: {distribution}"
            )
        relative = metadata_path.relative_to(appdir).as_posix()
        digest = authenticated.get(relative)
        if digest is None:
            raise ValueError(
                "Python top-level distribution has no authenticated wheel "
                f"or source-runtime owner: {relative}"
            )
        if sha256_file(metadata_path) != digest:
            raise ValueError(
                f"Python distribution metadata identity mismatch: {relative}"
            )
        observed.add(relative)

    missing_wheel_metadata = set(wheel["metadata"]) - observed
    if missing_wheel_metadata:
        raise ValueError(
            "authenticated wheel metadata is absent from AppDir: "
            + ", ".join(sorted(missing_wheel_metadata)[:10])
        )
    return wheel["owners"]


def load_qt_spdx_evidence(qt_root):
    sbom_root = qt_root / "sbom"
    if sbom_root.is_symlink() or not sbom_root.is_dir():
        raise ValueError("Qt SDK has no real SPDX evidence directory")
    documents = sorted(sbom_root.glob("*.spdx.json"))
    if not documents:
        raise ValueError("Qt SDK has no installed SPDX JSON documents")

    packages_by_name = {}
    files_by_path = {}
    for document_path in documents:
        if document_path.is_symlink() or not document_path.is_file():
            raise ValueError(f"Qt SPDX evidence is not a regular file: {document_path}")
        if document_path.stat().st_size > 32 * 1024 * 1024:
            raise ValueError(f"Qt SPDX evidence is unexpectedly large: {document_path}")
        with document_path.open(encoding="utf-8") as stream:
            document = json.load(stream)
        if document.get("spdxVersion") != "SPDX-2.3":
            raise ValueError(f"unsupported Qt SPDX document: {document_path}")
        packages = document.get("packages")
        files = document.get("files")
        relationships = document.get("relationships")
        if not isinstance(packages, list) or not isinstance(files, list) or not isinstance(relationships, list):
            raise ValueError(f"incomplete Qt SPDX document: {document_path}")

        document_relative = document_path.relative_to(qt_root).as_posix()
        document_digest = sha256_file(document_path)
        package_ids = {}
        for package in packages:
            if not isinstance(package, dict):
                raise ValueError(f"invalid Qt SPDX package: {document_path}")
            package_id = package.get("SPDXID")
            name = package.get("name")
            if not isinstance(package_id, str) or not package_id.startswith("SPDXRef-"):
                raise ValueError(f"invalid Qt SPDX package identifier: {document_path}")
            if not isinstance(name, str) or not name or "\x00" in name:
                raise ValueError(f"invalid Qt SPDX package name: {document_path}")
            if package_id in package_ids:
                raise ValueError(f"duplicate Qt SPDX package identifier: {document_path}")
            evidence = {
                "document": document_relative,
                "document_sha256": document_digest,
                "license": package.get("licenseConcluded", ""),
                "name": name,
                "spdx_id": package_id,
                "version": package.get("versionInfo", ""),
            }
            package_ids[package_id] = evidence
            packages_by_name.setdefault(name.casefold(), []).append(evidence)

        file_ids = {}
        for file_entry in files:
            if not isinstance(file_entry, dict):
                raise ValueError(f"invalid Qt SPDX file: {document_path}")
            file_id = file_entry.get("SPDXID")
            file_name = file_entry.get("fileName")
            if not isinstance(file_id, str) or not file_id.startswith("SPDXRef-"):
                raise ValueError(f"invalid Qt SPDX file identifier: {document_path}")
            if not isinstance(file_name, str) or not file_name.startswith("./"):
                raise ValueError(f"invalid Qt SPDX file path: {document_path}")
            relative = PurePosixPath(file_name[2:])
            if relative.is_absolute() or any(part in {"", ".", ".."} for part in relative.parts):
                raise ValueError(f"unsafe Qt SPDX file path: {document_path}")
            if file_id in file_ids:
                raise ValueError(f"duplicate Qt SPDX file identifier: {document_path}")
            file_ids[file_id] = relative.as_posix()

        for relationship in relationships:
            if not isinstance(relationship, dict) or relationship.get("relationshipType") != "CONTAINS":
                continue
            package = package_ids.get(relationship.get("spdxElementId"))
            relative = file_ids.get(relationship.get("relatedSpdxElement"))
            if package is None or relative is None:
                continue
            evidence = {
                **package,
                "file_spdx_id": relationship["relatedSpdxElement"],
            }
            entries = files_by_path.setdefault(relative, [])
            if evidence not in entries:
                entries.append(evidence)

    for entries in packages_by_name.values():
        entries.sort(key=lambda item: (item["document"], item["spdx_id"]))
    for entries in files_by_path.values():
        entries.sort(key=lambda item: (item["document"], item["spdx_id"]))
    return {"files": files_by_path, "packages": packages_by_name}


def qt_package_provenance(evidence, name, version=None):
    candidates = evidence["packages"].get(name.casefold(), [])
    if version is not None:
        candidates = [entry for entry in candidates if entry["version"] == version]
    if not candidates:
        qualifier = f" {version}" if version else ""
        raise ValueError(f"Qt SPDX evidence does not identify {name}{qualifier}")
    return ";".join(
        "qt-spdx={document}#sha256={document_sha256},package={spdx_id}".format(**entry)
        for entry in candidates
    )


def qt_runtime_source_index(qt_root):
    by_name = {}
    for path in sorted(qt_root.rglob("*")):
        if not path.is_file() or not (
            path.name.endswith(".so") or ".so." in path.name
        ):
            continue
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(qt_root)
        except (FileNotFoundError, RuntimeError, ValueError) as error:
            raise ValueError(f"Qt SDK runtime path escapes its root: {path}") from error
        if not resolved.is_file() or resolved.is_symlink():
            raise ValueError(f"Qt SDK runtime target is not a regular file: {path}")
        by_name.setdefault(path.name, set()).add(resolved)
    return by_name


def qt_source_for_payload(path, relative, qt_root, source_index):
    candidates = set()
    exact = qt_root / PurePosixPath(relative)
    if exact.is_file():
        resolved = exact.resolve(strict=True)
        resolved.relative_to(qt_root)
        candidates.add(resolved)
    candidates.update(source_index.get(path.name, set()))
    if not candidates:
        return None
    scored = [(candidate_score(path, candidate), candidate) for candidate in candidates]
    best_score = max(score for score, _ in scored)
    best = sorted(candidate for score, candidate in scored if score == best_score)
    if best_score != 100:
        return None
    if len(best) != 1:
        raise ValueError(f"ambiguous Qt SDK provenance for {relative}")
    return best[0]


def qt_source_provenance(qt_root, source, evidence):
    relative = source.relative_to(qt_root).as_posix()
    parts = [
        f"qt-source={relative}",
        f"qt-source-sha256={sha256_file(source)}",
    ]
    file_evidence = evidence["files"].get(relative, [])
    parts.extend(
        "qt-spdx={document}#sha256={document_sha256},package={spdx_id},file={file_spdx_id}".format(
            **entry
        )
        for entry in file_evidence
    )
    return ";".join(parts)


def ffmpeg_metadata(qt_root, source_index, evidence):
    avutil_targets = {
        target
        for name, targets in source_index.items()
        if name.startswith("libavutil.so.")
        for target in targets
    }
    if len(avutil_targets) != 1:
        raise ValueError("Qt SDK has ambiguous FFmpeg runtime metadata")
    avutil = next(iter(avutil_targets))
    if avutil.stat().st_size > 32 * 1024 * 1024:
        raise ValueError("Qt FFmpeg metadata library is unexpectedly large")
    contents = avutil.read_bytes()
    version_match = re.search(
        rb"(?:^|\x00)FFmpeg version ([0-9A-Za-z.+_-]{1,64})(?:\x00|$)",
        contents,
    )
    license_match = re.search(
        rb"(?:^|\x00)libavutil license: ([^\x00\r\n]{1,128})(?:\x00|$)",
        contents,
    )
    if version_match is None or license_match is None:
        raise ValueError("Qt FFmpeg runtime does not report its version and license")
    version = version_match.group(1).decode("ascii")
    reported_license = license_match.group(1).decode("ascii")
    licenses = {
        "LGPL version 2.1 or later": "LGPL-2.1-or-later",
        "LGPL version 3 or later": "LGPL-3.0-or-later",
        "GPL version 2 or later": "GPL-2.0-or-later",
        "GPL version 3 or later": "GPL-3.0-or-later",
    }
    license_name = licenses.get(reported_license)
    if license_name is None or not VERSION_RE.fullmatch(version):
        raise ValueError("Qt FFmpeg runtime reports unsupported metadata")
    provenance = qt_package_provenance(evidence, "FFmpeg")
    provenance += ";" + qt_source_provenance(qt_root, avutil, evidence)
    return {
        "license": license_name,
        "name": "FFmpeg",
        "provenance": provenance,
        "purl": f"pkg:generic/ffmpeg@{version}",
        "version": version,
    }


def icu_metadata(qt_root, source, evidence):
    match = ICU_LIBRARY_RE.fullmatch(source.name)
    if match is None:
        raise ValueError(f"cannot derive ICU version from {source.name}")
    version = match.group(1)
    provenance = qt_package_provenance(evidence, "ICU", version)
    provenance += ";" + qt_source_provenance(qt_root, source, evidence)
    return {
        "license": "Unicode-3.0",
        "name": "ICU",
        "provenance": provenance,
        "purl": f"pkg:generic/icu@{version}",
        "version": version,
    }


def fixture_package_component(path, appdir, entry):
    if not isinstance(entry, dict) or set(entry) != {
        "license", "name", "path", "provenance", "purl", "sha256", "version"
    }:
        raise ValueError(f"invalid fixture package provenance for {path.name}")
    relative = path.relative_to(appdir).as_posix()
    if entry["path"] != relative or sha256_file(path) != entry["sha256"]:
        raise ValueError(f"fixture index identity mismatch: {relative}")
    metadata = {
        key: entry[key]
        for key in ("license", "name", "provenance", "purl", "version")
    }
    return component(path, appdir, **metadata)


def transformed_runtime_component(path, appdir, entry):
    relative = path.relative_to(appdir).as_posix()
    source = entry["source_path"]
    if sha256_file(source) != entry["source_sha256"]:
        raise ValueError(f"transformed runtime source digest mismatch: {relative}")
    if sha256_file(path) != entry["output_sha256"]:
        raise ValueError(f"transformed runtime output digest mismatch: {relative}")
    metadata = dict(resolve_debian_package(source))
    metadata["provenance"] += (
        f";source-sha256={entry['source_sha256']}"
        f";transformation={entry['transformation']}"
        f";runtime-path={relative}"
        f";output-sha256={entry['output_sha256']}"
    )
    return component(path, appdir, **metadata)


def build_document(arguments, fixture_index=None):
    appdir = arguments.appdir.resolve(strict=True)
    if arguments.appdir.is_symlink() or not appdir.is_dir():
        raise ValueError("AppDir must be a real directory")
    fixture_index = dict(fixture_index or {})
    transformed_runtime = load_transformed_runtime_manifest(
        arguments.transformed_runtime_manifest, appdir
    )
    python_source = load_python_runtime_manifest(
        arguments.python_runtime_manifest,
        appdir,
        arguments.python_runtime_sha256,
    )
    python_runtime = python_source["libraries"]
    python_owners = python_distribution_files(
        appdir,
        arguments.python_wheel_manifest,
        arguments.requirements_lock,
        arguments.python_install_report,
        python_source["distributions"],
    )
    qt_root = arguments.qt_root.resolve(strict=True)
    if arguments.qt_root.is_symlink() or not qt_root.is_dir():
        raise ValueError("Qt provenance root must be a real directory")
    qt_evidence = load_qt_spdx_evidence(qt_root)
    qt_sources = qt_runtime_source_index(qt_root)
    ffmpeg = None
    libraries = []
    for path in runtime_library_paths(appdir):
        relative = path.relative_to(appdir).as_posix()
        fixture_metadata = fixture_index.pop(relative, None)
        transformed_metadata = transformed_runtime.pop(relative, None)
        if fixture_metadata is not None and transformed_metadata is not None:
            raise ValueError(
                f"runtime library has conflicting provenance manifests: {relative}"
            )
        runtime_digest = python_runtime.pop(relative, None)
        if runtime_digest is not None and sha256_file(path) != runtime_digest:
            raise ValueError(
                f"Python runtime manifest digest mismatch: {relative}"
            )
        if runtime_digest is not None and path in python_owners:
            raise ValueError(
                f"Python library has source-runtime and wheel ownership: {relative}"
            )
        qt_source = qt_source_for_payload(
            path, relative, qt_root, qt_sources
        )
        if fixture_metadata is not None:
            entry = fixture_package_component(path, appdir, fixture_metadata)
        elif transformed_metadata is not None:
            if path in python_owners or runtime_digest is not None:
                raise ValueError(
                    f"transformed runtime conflicts with Python ownership: {relative}"
                )
            entry = transformed_runtime_component(
                path, appdir, transformed_metadata
            )
        elif path in python_owners:
            entry = component(path, appdir, **python_owners[path])
        elif "site-packages/" in relative:
            raise ValueError(f"Python runtime library has no RECORD owner: {relative}")
        elif runtime_digest is not None:
            entry = component(
                path,
                appdir,
                "python-appimage-runtime",
                arguments.python_version,
                "LicenseRef-python-appimage-bundled-library",
                f"pkg:generic/python-appimage-runtime@{arguments.python_version}",
                arguments.python_provenance,
            )
        elif relative.startswith("opt/python"):
            raise ValueError(
                f"Python runtime library is absent from its manifest: {relative}"
            )
        elif path.name.startswith("libftd2xx.so"):
            entry = component(
                path,
                appdir,
                "d2xx-linux",
                arguments.d2xx_version,
                "FTDI D2XX Driver License",
                f"pkg:generic/d2xx-linux@{arguments.d2xx_version}",
                arguments.d2xx_provenance,
            )
        elif qt_source is not None and FFMPEG_LIBRARY_RE.fullmatch(path.name):
            if ffmpeg is None:
                ffmpeg = ffmpeg_metadata(qt_root, qt_sources, qt_evidence)
            entry = component(path, appdir, **ffmpeg)
        elif qt_source is not None and ICU_LIBRARY_RE.fullmatch(qt_source.name):
            entry = component(path, appdir, **icu_metadata(qt_root, qt_source, qt_evidence))
        elif qt_source is not None and (
            path.name.startswith("libQt6")
            or relative.startswith("plugins/")
            or relative.startswith("qml/")
        ):
            qt_match = re.match(r"lib(Qt6[A-Za-z0-9]+)\.so", path.name)
            qt_name = (
                qt_match.group(1)
                if qt_match
                else "Qt-distributed-" + re.sub(r"\.so(?:\..*)?$", "", path.name)
            )
            qt_purl_name = re.sub(r"[^a-z0-9.+-]+", "-", qt_name.lower())
            entry = component(
                path,
                appdir,
                qt_name,
                arguments.qt_version,
                "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
                f"pkg:generic/{qt_purl_name}@{arguments.qt_version}",
                arguments.qt_provenance + ";" + qt_source_provenance(
                    qt_root, qt_source, qt_evidence
                ),
            )
        elif qt_source is not None:
            raise ValueError(
                f"Qt SDK runtime library has no supported provenance: {relative}"
            )
        else:
            metadata = resolve_debian_package(path)
            entry = component(path, appdir, **metadata)
        libraries.append(entry)
    if python_runtime:
        missing = sorted(python_runtime)
        raise ValueError(
            "Python runtime manifest libraries are not present in AppDir: "
            + ", ".join(missing[:10])
        )
    if fixture_index:
        raise ValueError(
            "runtime fixture index libraries were not consumed: "
            + ", ".join(sorted(fixture_index)[:10])
        )
    if transformed_runtime:
        raise ValueError(
            "transformed runtime libraries were not consumed: "
            + ", ".join(sorted(transformed_runtime)[:10])
        )
    paths = [entry["path"] for entry in libraries]
    if paths != sorted(set(paths)):
        raise ValueError("duplicate or nondeterministic runtime provenance path")
    python_runtime_files = [
        {
            **entry,
            "provenance": arguments.python_provenance,
            "source_artifact_sha256": arguments.python_runtime_sha256,
        }
        for entry in python_source["files"]
    ]
    python_runtime_symlinks = [
        {
            **entry,
            "provenance": arguments.python_provenance,
            "source_artifact_sha256": arguments.python_runtime_sha256,
        }
        for entry in python_source["symlinks"]
    ]
    return {
        "format": "goldencheetah-runtime-provenance-2",
        "libraries": libraries,
        "python_runtime_files": python_runtime_files,
        "python_runtime_symlinks": python_runtime_symlinks,
    }


def atomic_write(path, document):
    path = path.absolute()
    if path.is_symlink():
        raise ValueError("runtime provenance output is a symlink")
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
        os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--appdir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qt-version", required=True)
    parser.add_argument("--qt-root", required=True, type=Path)
    parser.add_argument("--qt-provenance", required=True)
    parser.add_argument("--python-version", required=True)
    parser.add_argument("--python-provenance", required=True)
    parser.add_argument("--python-runtime-manifest", required=True, type=Path)
    parser.add_argument("--python-runtime-sha256", required=True)
    parser.add_argument("--python-wheel-manifest", required=True, type=Path)
    parser.add_argument("--requirements-lock", required=True, type=Path)
    parser.add_argument("--python-install-report", required=True, type=Path)
    parser.add_argument("--d2xx-version", required=True)
    parser.add_argument("--d2xx-provenance", required=True)
    parser.add_argument("--transformed-runtime-manifest", required=True, type=Path)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    atomic_write(arguments.output, build_document(arguments))


if __name__ == "__main__":
    main()
