#!/usr/bin/env python3
"""Read a Type 2 AppImage SquashFS offset without executing its runtime."""

import os
from pathlib import Path
import stat
import struct
import sys


ELF32_HEADER = "HHIIIIIHHHHHH"
ELF64_HEADER = "HHIQQQIHHHHHH"
SQUASHFS_HEADER = "<5I6H8Q"
SQUASHFS_SUPERBLOCK_SIZE = struct.calcsize(SQUASHFS_HEADER)


def fail(message):
    raise ValueError(message)


def read_exact(descriptor, size, offset):
    data = os.pread(descriptor, size, offset)
    if len(data) != size:
        fail("truncated AppImage")
    return data


def appimage_offset(path):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(path, flags)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            fail("AppImage is not a regular file")
        if metadata.st_size < 64 + SQUASHFS_SUPERBLOCK_SIZE:
            fail("AppImage is too small")

        ident = read_exact(descriptor, 16, 0)
        if ident[:4] != b"\x7fELF" or ident[6] != 1:
            fail("invalid ELF identification")
        if ident[8:11] != b"AI\x02":
            fail("not a Type 2 AppImage")
        if ident[4] == 1:
            header_format = ELF32_HEADER
            expected_header_size = 52
            expected_section_size = 40
        elif ident[4] == 2:
            header_format = ELF64_HEADER
            expected_header_size = 64
            expected_section_size = 64
        else:
            fail("unsupported ELF class")
        if ident[5] == 1:
            byte_order = "<"
        elif ident[5] == 2:
            byte_order = ">"
        else:
            fail("unsupported ELF byte order")

        header_size = struct.calcsize(byte_order + header_format)
        values = struct.unpack(
            byte_order + header_format,
            read_exact(descriptor, header_size, 16),
        )
        elf_version = values[2]
        section_offset = values[5]
        encoded_header_size = values[7]
        section_entry_size = values[10]
        section_count = values[11]
        section_name_index = values[12]
        if elf_version != 1 or encoded_header_size != expected_header_size:
            fail("invalid ELF header")
        if section_entry_size != expected_section_size:
            fail("unexpected ELF section entry size")
        if section_count in (0, 0xFFFF):
            fail("extended or empty ELF section tables are unsupported")
        if section_name_index == 0xFFFF:
            fail("extended ELF section indexes are unsupported")
        if section_offset < expected_header_size:
            fail("invalid ELF section table offset")

        offset = section_offset + section_entry_size * section_count
        if offset <= section_offset or offset > metadata.st_size - SQUASHFS_SUPERBLOCK_SIZE:
            fail("ELF section table is outside the AppImage")
        superblock = read_exact(descriptor, SQUASHFS_SUPERBLOCK_SIZE, offset)
        if superblock[:4] != b"hsqs":
            fail("SquashFS does not begin at the trusted ELF boundary")
        values = struct.unpack(SQUASHFS_HEADER, superblock)
        inode_count = values[1]
        block_size = values[3]
        compression = values[5]
        block_log = values[6]
        major = values[9]
        minor = values[10]
        bytes_used = values[12]
        if inode_count == 0:
            fail("empty SquashFS payload")
        if block_size < 4096 or block_size > 1048576 or block_size & (block_size - 1):
            fail("invalid SquashFS block size")
        if block_log != block_size.bit_length() - 1:
            fail("invalid SquashFS block logarithm")
        if compression not in range(1, 7) or major != 4 or minor != 0:
            fail("unsupported SquashFS format")
        if bytes_used < SQUASHFS_SUPERBLOCK_SIZE or bytes_used > metadata.st_size - offset:
            fail("SquashFS payload extends outside the AppImage")
        return offset
    finally:
        os.close(descriptor)


def main(arguments):
    if len(arguments) != 1:
        print("usage: read-appimage-offset.py APPIMAGE", file=sys.stderr)
        return 2
    path = Path(arguments[0])
    try:
        if path.is_symlink():
            fail("refusing a linked AppImage")
        print(appimage_offset(path))
    except (OSError, ValueError, struct.error) as error:
        print(f"invalid AppImage: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
