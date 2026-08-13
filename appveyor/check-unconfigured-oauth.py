#!/usr/bin/env python3

import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


EXPECTED = (
    "goldencheetah_build_status=1\n"
    "application=GoldenCheetah\n"
    "strava_support=enabled\n"
    "strava_oauth=runtime_credentials\n"
    "strava_compile_fallback=unavailable\n"
)

MACH_O_32_MAGICS = {b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe"}
MACH_O_64_MAGICS = {b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"}
MACH_O_FAT_MAGICS = {
    b"\xca\xfe\xba\xbe",
    b"\xbe\xba\xfe\xca",
    b"\xca\xfe\xba\xbf",
    b"\xbf\xba\xfe\xca",
}


def binary_format(path):
    with path.open("rb") as stream:
        header = stream.read(4096)
        if len(header) >= 20 and header[:4] == b"\x7fELF":
            if header[4] not in {1, 2} or header[5] not in {1, 2}:
                raise ValueError("invalid ELF executable header")
            byte_order = "little" if header[5] == 1 else "big"
            if int.from_bytes(header[16:18], byte_order) not in {2, 3}:
                raise ValueError("ELF artifact is not executable")
            return "elf"

        if len(header) >= 64 and header[:2] == b"MZ":
            pe_offset = int.from_bytes(header[0x3C:0x40], "little")
            if pe_offset < 64 or pe_offset > 16 * 1024 * 1024:
                raise ValueError("invalid PE executable header offset")
            stream.seek(pe_offset)
            if stream.read(4) != b"PE\x00\x00":
                raise ValueError("invalid PE executable signature")
            return "pe"

        magic = header[:4]
        if magic in MACH_O_32_MAGICS | MACH_O_64_MAGICS:
            minimum = 32 if magic in MACH_O_64_MAGICS else 28
            if len(header) < minimum:
                raise ValueError("truncated Mach-O executable header")
            byte_order = ">" if magic in {
                b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf"
            } else "<"
            if struct.unpack_from(byte_order + "I", header, 12)[0] != 2:
                raise ValueError("Mach-O artifact is not executable")
            return "mach-o"

        if magic in MACH_O_FAT_MAGICS:
            if len(header) < 8:
                raise ValueError("truncated universal Mach-O header")
            byte_order = ">" if magic in {
                b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"
            } else "<"
            architectures = struct.unpack_from(byte_order + "I", header, 4)[0]
            if architectures < 1 or architectures > 64:
                raise ValueError("invalid universal Mach-O architecture count")
            return "mach-o"

    raise ValueError("artifact is not a native executable")


def expected_binary_format():
    if os.name == "nt":
        return "pe"
    if sys.platform == "darwin":
        return "mach-o"
    return "elf"


def build_environment(home, source=None):
    environment = dict(os.environ if source is None else source)
    environment.update(
        HOME=str(home),
        XDG_CONFIG_HOME=str(Path(home) / ".config"),
        LC_ALL="C",
    )
    for name in tuple(environment):
        if name.startswith("DYLD_") or name in {
            "LD_AUDIT",
            "LD_LIBRARY_PATH",
            "LD_PRELOAD",
            "PYTHONHOME",
            "PYTHONPATH",
            "QML2_IMPORT_PATH",
            "QML_IMPORT_PATH",
            "QT_PLUGIN_PATH",
            "QT_QPA_PLATFORM_PLUGIN_PATH",
        }:
            environment.pop(name, None)
    return environment


def check_binary(binary):
    if binary.is_symlink() or not binary.is_file():
        raise ValueError("GoldenCheetah build-status target is not a regular file")
    binary = binary.resolve(strict=True)
    if binary_format(binary) != expected_binary_format():
        raise ValueError("GoldenCheetah build-status target is not native for this host")
    with tempfile.TemporaryDirectory(prefix="gc-build-status-") as home:
        environment = build_environment(home)
        result = subprocess.run(
            [str(binary), "--goldencheetah-build-status"],
            env=environment,
            capture_output=True,
            timeout=15,
        )
    if result.returncode != 0:
        raise ValueError("GoldenCheetah build-status command failed")
    try:
        report = result.stdout.decode("ascii").replace("\r\n", "\n")
    except UnicodeDecodeError as error:
        raise ValueError("GoldenCheetah build-status output is not ASCII") from error
    if report != EXPECTED:
        raise ValueError("public artifact contains OAuth credentials or invalid status")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    arguments = parser.parse_args()
    check_binary(arguments.binary)


if __name__ == "__main__":
    main()
