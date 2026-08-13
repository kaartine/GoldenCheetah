#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
from urllib.parse import unquote, urlparse


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
REVISION_RE = re.compile(r"^[0-9a-f]{40}$")
PACKAGE_RE = re.compile(r"^([A-Za-z0-9_.-]+)==([^\s\\]+)\s*\\?$")
HASH_RE = re.compile(r"^--hash=sha256:([0-9a-f]{64})\s*\\?$")
RUNTIME_FILE_RE = re.compile(r"(?:^|/)[^/]+\.so(?:\..*)?$")
SPDX_LICENSE_IDS = {
    "Apache-2.0",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "GPL-2.0-only",
    "GPL-2.0-or-later",
    "GPL-3.0-only",
    "GPL-3.0-or-later",
    "LGPL-2.1-only",
    "LGPL-2.1-or-later",
    "LGPL-3.0-only",
    "LGPL-3.0-or-later",
    "MIT",
    "MPL-2.0",
    "PSF-2.0",
    "Unicode-3.0",
}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_build_manifest(path):
    lines = path.read_text(encoding="ascii").splitlines()
    if len(lines) != 6 or lines[0] != "goldencheetah_appimage_manifest=2":
        raise ValueError("invalid GoldenCheetah build manifest")
    values = {}
    for line in lines[1:]:
        key, separator, value = line.partition("=")
        if not separator or not key or key in values:
            raise ValueError("invalid GoldenCheetah build manifest entry")
        values[key] = value
    if set(values) != {
        "source_revision",
        "build_inputs_sha256",
        "raw_elf_sha256",
        "toolchain",
        "strava_oauth_configured",
    }:
        raise ValueError("unexpected GoldenCheetah build manifest entries")
    if not REVISION_RE.fullmatch(values["source_revision"]):
        raise ValueError("invalid source revision")
    if not SHA256_RE.fullmatch(values["build_inputs_sha256"]):
        raise ValueError("invalid build input identity")
    if not SHA256_RE.fullmatch(values["raw_elf_sha256"]):
        raise ValueError("invalid raw executable digest")
    if values["strava_oauth_configured"] not in {"true", "false"}:
        raise ValueError("invalid Strava OAuth build status")
    return values


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
                "name": package_match.group(1),
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
    return packages


def property_entry(name, value):
    return {"name": name, "value": str(value)}


def digest_component(component_type, name, version, digest, role):
    if not SHA256_RE.fullmatch(digest):
        raise ValueError("invalid component digest")
    return {
        "bom-ref": "goldencheetah:{}:{}:{}".format(role, name, digest),
        "type": component_type,
        "name": name,
        "version": version,
        "hashes": [{"alg": "SHA-256", "content": digest}],
        "properties": [property_entry("goldencheetah:role", role)],
    }


def license_entry(value):
    if value in SPDX_LICENSE_IDS:
        return {"license": {"id": value}}
    return {"license": {"name": value}}


def runtime_library_component(metadata, path, appdir):
    digest = sha256_file(path)
    relative = path.relative_to(appdir).as_posix()
    normalized_name = normalized_python_name(metadata["name"])
    component = digest_component(
        "library", metadata["name"], metadata["version"], digest,
        "identified-runtime-dependency"
    )
    component["bom-ref"] = "goldencheetah:runtime:{}:{}:{}".format(
        normalized_name, digest, hashlib.sha256(relative.encode("utf-8")).hexdigest()
    )
    component["properties"].append(
        property_entry("goldencheetah:runtime-path", relative)
    )
    component["properties"].append(
        property_entry("goldencheetah:provenance", metadata["provenance"])
    )
    component["purl"] = metadata["purl"]
    component["licenses"] = [license_entry(metadata["license"])]
    return component


def normalized_python_name(name):
    return re.sub(r"[-_.]+", "-", name).lower()


def parse_wheel_artifacts(path, requirements_lock):
    with path.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if (
        not isinstance(document, dict)
        or set(document) != {"format", "packages", "requirements_lock_sha256"}
        or document["format"] != "goldencheetah-python-wheel-records-1"
        or document["requirements_lock_sha256"] != sha256_file(requirements_lock)
        or not isinstance(document["packages"], list)
    ):
        raise ValueError("invalid Python wheel manifest")
    artifacts = {}
    for package in document["packages"]:
        if not isinstance(package, dict):
            raise ValueError("invalid Python wheel manifest package")
        name = normalized_python_name(package.get("name", ""))
        value = {
            "artifact": package.get("artifact"),
            "name": name,
            "sha256": package.get("sha256"),
            "version": package.get("version"),
        }
        if (
            not name
            or name in artifacts
            or not isinstance(value["artifact"], str)
            or not value["artifact"].endswith(".whl")
            or not isinstance(value["sha256"], str)
            or not SHA256_RE.fullmatch(value["sha256"])
            or not isinstance(value["version"], str)
        ):
            raise ValueError("invalid Python wheel manifest package")
        artifacts[name] = value
    return artifacts


def parse_pip_report(path, locked_packages, wheel_artifacts):
    with path.open(encoding="utf-8") as stream:
        report = json.load(stream)
    if report.get("version") != "1" or not isinstance(report.get("install"), list):
        raise ValueError("invalid pip installation report")

    locked = {
        normalized_python_name(package["name"]): package
        for package in locked_packages
    }
    if len(locked) != len(locked_packages):
        raise ValueError("duplicate normalized Python package name")
    selected = []
    selected_names = set()
    for entry in report["install"]:
        metadata = entry.get("metadata", {})
        name = normalized_python_name(metadata.get("name", ""))
        version = metadata.get("version")
        package = locked.get(name)
        if package is None or version != package["version"]:
            raise ValueError("pip installed a package outside the reviewed lock")
        if name in selected_names:
            raise ValueError("pip report contains a duplicate package")

        download = entry.get("download_info", {})
        parsed_url = urlparse(download.get("url", ""))
        if parsed_url.scheme not in {"file", "https"} or parsed_url.query or parsed_url.fragment:
            raise ValueError("pip report contains an unsafe artifact URL")
        artifact = PurePosixPath(unquote(parsed_url.path)).name
        if not artifact or any(character in artifact for character in "\x00\r\n"):
            raise ValueError("pip report contains an invalid artifact filename")
        hashes = download.get("archive_info", {}).get("hashes", {})
        digest = hashes.get("sha256")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise ValueError("pip report has no selected SHA-256 artifact hash")
        if digest not in package["hashes"]:
            raise ValueError("pip selected an artifact outside the reviewed lock")
        selected.append(
            {
                "name": name,
                "version": version,
                "artifact": artifact,
                "sha256": digest,
            }
        )
        if wheel_artifacts.get(name) != selected[-1]:
            raise ValueError("pip report is not tied to its authenticated wheel")
        selected_names.add(name)
    if selected_names != set(locked):
        raise ValueError("pip did not install every reviewed Python package")
    if selected_names != set(wheel_artifacts):
        raise ValueError("wheel manifest does not cover every reviewed Python package")
    return selected


def python_components(packages):
    components = []
    for package in packages:
        purl = "pkg:pypi/{}@{}".format(package["name"], package["version"])
        components.append(
            {
                "bom-ref": purl,
                "type": "library",
                "name": package["name"],
                "version": package["version"],
                "purl": purl,
                "hashes": [
                    {"alg": "SHA-256", "content": package["sha256"]}
                ],
                "properties": [
                    property_entry(
                        "goldencheetah:artifact-filename", package["artifact"]
                    ),
                    property_entry(
                        "goldencheetah:role", "python-runtime-dependency"
                    ),
                ],
            }
        )
    return components


def python_runtime_properties(entry):
    properties = [
        property_entry(
            "goldencheetah:python-runtime-role",
            "authenticated-python-runtime-file",
        ),
        property_entry(
            "goldencheetah:python-source-artifact-sha256",
            entry["source_artifact_sha256"],
        ),
        property_entry(
            "goldencheetah:python-transformation",
            entry.get("transformation", "identity-symlink"),
        ),
        property_entry("goldencheetah:provenance", entry["provenance"]),
    ]
    if "source_sha256" in entry:
        properties.append(
            property_entry(
                "goldencheetah:python-source-file-sha256",
                entry["source_sha256"],
            )
        )
    return properties


def symlink_component(path, appdir, python_entry=None):
    relative = path.relative_to(appdir).as_posix()
    target = os.readlink(path)
    if not target or "\x00" in target or "\r" in target or "\n" in target:
        raise ValueError("AppDir contains an invalid symlink target")
    if PurePosixPath(target).is_absolute():
        raise ValueError("AppDir contains an absolute symlink target")
    try:
        resolved_target = path.resolve(strict=True)
        resolved_target.relative_to(appdir)
    except (FileNotFoundError, RuntimeError, ValueError):
        raise ValueError("AppDir symlink escapes its payload or is dangling")
    component = {
        "bom-ref": "goldencheetah:symlink:{}".format(relative),
        "type": "file",
        "name": relative,
        "properties": [
            property_entry("goldencheetah:role", "payload-symlink"),
            property_entry("goldencheetah:symlink-target", target),
        ],
    }
    if python_entry is not None:
        if python_entry["target"] != target:
            raise ValueError("Python runtime SBOM symlink target mismatch")
        component["properties"].extend(python_runtime_properties(python_entry))
    return component


def appdir_components(
    appdir, excluded_path, python_runtime_files, python_runtime_symlinks
):
    components = []
    for root, directory_names, file_names in os.walk(appdir, followlinks=False):
        directory_names.sort()
        file_names.sort()
        root_path = Path(root)
        linked_directories = [
            name for name in directory_names if (root_path / name).is_symlink()
        ]
        for name in linked_directories:
            path = root_path / name
            relative = path.relative_to(appdir).as_posix()
            components.append(
                symlink_component(
                    path, appdir, python_runtime_symlinks.pop(relative, None)
                )
            )
            directory_names.remove(name)
        for name in file_names:
            path = root_path / name
            if path == excluded_path:
                continue
            relative = path.relative_to(appdir).as_posix()
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                components.append(
                    symlink_component(
                        path, appdir, python_runtime_symlinks.pop(relative, None)
                    )
                )
                continue
            if not stat.S_ISREG(metadata.st_mode):
                raise ValueError("AppDir contains an unsupported special file")
            digest = sha256_file(path)
            component = {
                    "bom-ref": "goldencheetah:file:{}:{}".format(
                        relative, digest
                    ),
                    "type": "file",
                    "name": relative,
                    "hashes": [{"alg": "SHA-256", "content": digest}],
                    "properties": [
                        property_entry("goldencheetah:role", "payload-file"),
                        property_entry("goldencheetah:size", metadata.st_size),
                        property_entry(
                            "goldencheetah:mode",
                            "{:04o}".format(stat.S_IMODE(metadata.st_mode)),
                        ),
                    ],
                }
            python_entry = python_runtime_files.pop(relative, None)
            if python_entry is not None:
                if python_entry["output_sha256"] != digest:
                    raise ValueError("Python runtime SBOM digest mismatch")
                component["properties"].extend(
                    python_runtime_properties(python_entry)
                )
            components.append(component)
    if python_runtime_files or python_runtime_symlinks:
        missing = sorted((*python_runtime_files, *python_runtime_symlinks))
        raise ValueError(
            "Python runtime provenance is absent from AppDir: "
            + ", ".join(missing[:10])
        )
    return components


def parse_build_features(path):
    features = {"srmio": False, "d2xx": False}
    assignments = {"SRMIO_INSTALL": [], "SRMIO_LIBS": [], "D2XX_INCLUDE": [], "D2XX_LIBS": []}
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0]
        match = re.match(
            r"^\s*([A-Z0-9_]+)\s*(\+=|-=|=)\s*(.*?)\s*$", line
        )
        if match is None or match.group(1) not in assignments:
            continue
        name, operation, value = match.groups()
        tokens = value.split()
        if operation == "=":
            assignments[name] = tokens
        elif operation == "+=":
            assignments[name].extend(tokens)
        else:
            removals = set(tokens)
            assignments[name] = [
                token for token in assignments[name] if token not in removals
            ]
    features["srmio"] = bool(assignments["SRMIO_INSTALL"] or assignments["SRMIO_LIBS"])
    features["d2xx"] = bool(assignments["D2XX_INCLUDE"] or assignments["D2XX_LIBS"])
    return features


def parse_runtime_provenance(path, appdir):
    with path.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if set(document) != {
        "format", "libraries", "python_runtime_files", "python_runtime_symlinks"
    } or document["format"] != "goldencheetah-runtime-provenance-2":
        raise ValueError("invalid runtime provenance document")
    libraries = document["libraries"]
    if (
        not isinstance(libraries, list)
        or not isinstance(document["python_runtime_files"], list)
        or not isinstance(document["python_runtime_symlinks"], list)
    ):
        raise ValueError("runtime provenance collections must be lists")
    expected_paths = sorted(
        item.relative_to(appdir).as_posix()
        for item in appdir.rglob("*")
        if not item.is_symlink() and item.is_file()
        and RUNTIME_FILE_RE.search(item.relative_to(appdir).as_posix())
    )
    actual_paths = [entry.get("path") for entry in libraries]
    if actual_paths != expected_paths or len(actual_paths) != len(set(actual_paths)):
        raise ValueError("runtime provenance does not exactly cover packaged libraries")
    components = []
    required = {"license", "name", "path", "provenance", "purl", "version"}
    for entry in libraries:
        if not isinstance(entry, dict) or set(entry) != required:
            raise ValueError("invalid runtime provenance entry")
        if any(not isinstance(entry[key], str) or not entry[key]
               for key in required):
            raise ValueError("incomplete runtime provenance entry")
        payload = appdir / PurePosixPath(entry["path"])
        if payload.is_symlink() or not payload.is_file():
            raise ValueError("runtime provenance references a non-regular payload")
        components.append(runtime_library_component(entry, payload, appdir))
    sha256_re = re.compile(r"^[0-9a-f]{64}$")
    file_keys = {
        "path", "source_sha256", "output_sha256", "transformation",
        "provenance", "source_artifact_sha256",
    }
    python_files = {}
    paths = []
    transformations = {
        "identity",
        "python-console-script-wrapper-v1",
        "python-wheel-record-refresh-v1",
    }
    for entry in document["python_runtime_files"]:
        if not isinstance(entry, dict) or set(entry) != file_keys:
            raise ValueError("invalid Python runtime file provenance entry")
        relative = entry["path"]
        if (
            not isinstance(relative, str)
            or not relative.isascii()
            or not sha256_re.fullmatch(entry["source_sha256"])
            or not sha256_re.fullmatch(entry["output_sha256"])
            or not sha256_re.fullmatch(entry["source_artifact_sha256"])
            or not isinstance(entry["provenance"], str)
            or not entry["provenance"]
            or entry["transformation"] not in transformations
        ):
            raise ValueError("invalid Python runtime file provenance entry")
        if (
            entry["transformation"] == "identity"
            and entry["source_sha256"] != entry["output_sha256"]
        ):
            raise ValueError("undeclared Python runtime transformation")
        if (
            entry["transformation"] != "identity"
            and entry["source_sha256"] == entry["output_sha256"]
        ):
            raise ValueError("empty Python runtime transformation")
        paths.append(relative)
        python_files[relative] = entry
    if paths != sorted(set(paths)):
        raise ValueError("Python runtime file provenance is not unique and sorted")

    symlink_keys = {"path", "target", "provenance", "source_artifact_sha256"}
    python_symlinks = {}
    paths = []
    for entry in document["python_runtime_symlinks"]:
        if not isinstance(entry, dict) or set(entry) != symlink_keys:
            raise ValueError("invalid Python runtime symlink provenance entry")
        relative = entry["path"]
        if (
            not isinstance(relative, str)
            or not relative.isascii()
            or not isinstance(entry["target"], str)
            or not entry["target"]
            or not sha256_re.fullmatch(entry["source_artifact_sha256"])
            or not isinstance(entry["provenance"], str)
            or not entry["provenance"]
        ):
            raise ValueError("invalid Python runtime symlink provenance entry")
        paths.append(relative)
        python_symlinks[relative] = entry
    if paths != sorted(set(paths)):
        raise ValueError("Python runtime symlink provenance is not unique and sorted")
    return components, python_files, python_symlinks


def atomic_json_write(path, document):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".tmp.", dir=str(path.parent)
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
        directory_descriptor = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory_descriptor)
        finally:
            os.close(directory_descriptor)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def build_document(arguments):
    appdir = arguments.appdir.resolve(strict=True)
    if not appdir.is_dir():
        raise ValueError("AppDir is not a directory")
    output = arguments.output.absolute()
    manifest = parse_build_manifest(arguments.build_manifest)
    locked_packages = parse_requirements_lock(arguments.requirements_lock)
    wheel_artifacts = parse_wheel_artifacts(
        arguments.python_wheel_manifest, arguments.requirements_lock
    )
    packages = parse_pip_report(
        arguments.python_install_report, locked_packages, wheel_artifacts
    )
    requirements_digest = sha256_file(arguments.requirements_lock)
    build_config_digest = sha256_file(arguments.build_config)
    features = parse_build_features(arguments.build_config)

    application_ref = "pkg:generic/goldencheetah@{}".format(
        manifest["source_revision"]
    )
    runtime_components, python_files, python_symlinks = parse_runtime_provenance(
        arguments.runtime_provenance, appdir
    )
    components = python_components(packages)
    components.extend(
        appdir_components(appdir, output, python_files, python_symlinks)
    )
    components.extend(runtime_components)
    components.extend(
        [
            digest_component(
                "application",
                "linuxdeployqt",
                arguments.linuxdeployqt_file,
                arguments.linuxdeployqt_sha256,
                "build-tool",
            ),
            digest_component(
                "application",
                "appimagetool",
                arguments.appimagetool_file,
                arguments.appimagetool_sha256,
                "build-tool",
            ),
            digest_component(
                "application",
                "appimage-runtime",
                arguments.appimage_runtime_file,
                arguments.appimage_runtime_sha256,
                "runtime",
            ),
            digest_component(
                "framework",
                "python-runtime",
                arguments.python_runtime_file,
                arguments.python_runtime_sha256,
                "runtime",
            ),
        ]
    )
    if features["srmio"]:
        components.append({
                "bom-ref": "pkg:github/rclasen/srmio@{}".format(
                    arguments.srmio_revision
                ),
                "type": "library",
                "name": "srmio",
                "version": arguments.srmio_revision,
                "purl": "pkg:github/rclasen/srmio@{}".format(
                    arguments.srmio_revision
                ),
                "hashes": [
                    {
                        "alg": "SHA-256",
                        "content": arguments.srmio_source_sha256,
                    }
                ],
                "licenses": [{"license": {"id": "MIT"}}],
                "properties": [
                    property_entry("goldencheetah:role", "linked-dependency"),
                    property_entry(
                        "goldencheetah:provenance",
                        "https://github.com/rclasen/srmio/tree/{}".format(
                            arguments.srmio_revision
                        ),
                    ),
                ],
            })
    if features["d2xx"]:
        d2xx = digest_component(
            "library", "d2xx-linux", arguments.d2xx_linux_version,
            arguments.d2xx_linux_sha256, "linked-dependency-source"
        )
        d2xx["licenses"] = [
            {"license": {"name": "FTDI D2XX Driver License"}}
        ]
        d2xx["properties"].append(
            property_entry(
                "goldencheetah:provenance",
                "https://ftdichip.com/drivers/d2xx-drivers/",
            )
        )
        components.append(d2xx)
    components.sort(key=lambda component: component["bom-ref"])
    dependency_refs = sorted(
        component["bom-ref"]
        for component in components
        if component.get("properties")
        and any(
            entry == property_entry("goldencheetah:role", role)
            for entry in component["properties"]
            for role in (
                "linked-dependency",
                "linked-dependency-source",
                "identified-runtime-dependency",
                "python-runtime-dependency",
                "runtime",
                "payload-file",
                "payload-symlink",
            )
        )
    )
    return {
        "$schema": "http://cyclonedx.org/schema/bom-1.5.schema.json",
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "bom-ref": application_ref,
                "type": "application",
                "name": "GoldenCheetah",
                "version": manifest["source_revision"],
                "licenses": [{"license": {"id": "GPL-2.0-or-later"}}],
                "properties": [
                    property_entry(
                        "goldencheetah:raw-elf-sha256",
                        manifest["raw_elf_sha256"],
                    ),
                    property_entry(
                        "goldencheetah:build-inputs-sha256",
                        manifest["build_inputs_sha256"],
                    ),
                    property_entry(
                        "goldencheetah:build-config-sha256",
                        build_config_digest,
                    ),
                    property_entry(
                        "goldencheetah:requirements-lock-sha256",
                        requirements_digest,
                    ),
                    property_entry(
                        "goldencheetah:strava-oauth-configured",
                        manifest["strava_oauth_configured"],
                    ),
                    property_entry(
                        "goldencheetah:toolchain", manifest["toolchain"]
                    ),
                ],
            }
        },
        "components": components,
        "dependencies": [{"ref": application_ref, "dependsOn": dependency_refs}],
    }


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--appdir", required=True, type=Path)
    parser.add_argument("--build-manifest", required=True, type=Path)
    parser.add_argument("--requirements-lock", required=True, type=Path)
    parser.add_argument("--python-install-report", required=True, type=Path)
    parser.add_argument("--python-wheel-manifest", required=True, type=Path)
    parser.add_argument("--build-config", required=True, type=Path)
    parser.add_argument("--runtime-provenance", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--linuxdeployqt-file", required=True)
    parser.add_argument("--linuxdeployqt-sha256", required=True)
    parser.add_argument("--appimagetool-file", required=True)
    parser.add_argument("--appimagetool-sha256", required=True)
    parser.add_argument("--appimage-runtime-file", required=True)
    parser.add_argument("--appimage-runtime-sha256", required=True)
    parser.add_argument("--python-runtime-file", required=True)
    parser.add_argument("--python-runtime-sha256", required=True)
    parser.add_argument("--srmio-revision", required=True)
    parser.add_argument("--srmio-source-sha256", required=True)
    parser.add_argument("--d2xx-linux-version", required=True)
    parser.add_argument("--d2xx-linux-sha256", required=True)
    return parser.parse_args()


def main():
    arguments = parse_arguments()
    if not REVISION_RE.fullmatch(arguments.srmio_revision):
        raise ValueError("invalid SRMIO revision")
    if not SHA256_RE.fullmatch(arguments.srmio_source_sha256):
        raise ValueError("invalid SRMIO source digest")
    document = build_document(arguments)
    atomic_json_write(arguments.output, document)


if __name__ == "__main__":
    main()
