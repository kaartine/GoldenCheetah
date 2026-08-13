#!/usr/bin/env python3
"""Compute a domain-separated identity for ignored AppImage build inputs."""

import hashlib
import os
from pathlib import Path
import re
import stat
import sys


MAX_INPUT_SIZE = 1024 * 1024
LOCAL_INPUT_RE = re.compile(
    r"^\s*(LOCALHEADERS|LOCALSOURCES)\s*(?:\+=|-=|\*=|~=|=)", re.MULTILINE
)
CONFIG_ASSIGNMENT_RE = re.compile(
    r"^\s*CONFIG\s*(\+=|-=|\*=|=)\s*([^\n]*)$", re.MULTILINE
)
BUILD_MODES = {"debug", "release"}


def read_regular(path, required):
    try:
        metadata = path.lstat()
    except FileNotFoundError:
        if required:
            raise ValueError(f"required build input is missing: {path}")
        return None
    if not stat.S_ISREG(metadata.st_mode) or path.is_symlink():
        raise ValueError(f"build input is not a regular file: {path}")
    if metadata.st_size > MAX_INPUT_SIZE:
        raise ValueError(f"build input is too large: {path}")
    data = path.read_bytes()
    if len(data) != metadata.st_size or b"\0" in data:
        raise ValueError(f"invalid build input: {path}")
    return data


def uncommented_configuration(data):
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("gcconfig.pri is not UTF-8") from error
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def effective_build_mode(configuration):
    modes = []
    for operator, value in CONFIG_ASSIGNMENT_RE.findall(configuration):
        assigned_modes = [token for token in value.split() if token in BUILD_MODES]
        if operator == "=":
            modes = assigned_modes
        elif operator == "+=":
            modes.extend(assigned_modes)
        elif operator == "-=":
            modes = [mode for mode in modes if mode not in assigned_modes]
        else:
            for mode in assigned_modes:
                if mode not in modes:
                    modes.append(mode)
    return modes[-1] if modes else None


def update_field(digest, name, data):
    encoded_name = name.encode("ascii")
    digest.update(len(encoded_name).to_bytes(4, "big"))
    digest.update(encoded_name)
    if data is None:
        digest.update(b"\xff" * 8)
    else:
        digest.update(len(data).to_bytes(8, "big"))
        digest.update(data)


def identity(source_root):
    metadata = source_root.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or source_root.is_symlink():
        raise ValueError("source root is not a real directory")
    config = read_regular(source_root / "src/gcconfig.pri", required=True)
    configuration = uncommented_configuration(config)
    if effective_build_mode(configuration) != "release":
        raise ValueError("gcconfig.pri must select the release configuration")
    local_match = LOCAL_INPUT_RE.search(configuration)
    if local_match:
        raise ValueError(
            f"{local_match.group(1)} is not allowed in an authenticated release build"
        )
    generated_secrets = read_regular(
        source_root / "src/Core/GeneratedSecrets.h", required=False
    )
    qwt_config = read_regular(source_root / "qwt/qwtconfig.pri", required=True)
    digest = hashlib.sha256()
    digest.update(b"goldencheetah-build-input-identity-v1\0")
    update_field(digest, "src/gcconfig.pri", config)
    update_field(digest, "src/Core/GeneratedSecrets.h", generated_secrets)
    update_field(digest, "qwt/qwtconfig.pri", qwt_config)
    return digest.hexdigest()


def main(arguments):
    if len(arguments) != 1:
        print("usage: compute-build-input-identity.py SOURCE_ROOT", file=sys.stderr)
        return 2
    try:
        source_root = Path(arguments[0]).resolve(strict=True)
        print(identity(source_root))
    except (OSError, ValueError) as error:
        print(f"invalid release build inputs: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
