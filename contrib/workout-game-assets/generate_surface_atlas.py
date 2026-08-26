#!/usr/bin/env python3
"""Generate the deterministic Workout Game pixel surface atlas and tiles."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path
import zlib


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
TILE_SIZE = 32

PALETTES = {
    "forest": ((226, 236, 221), (218, 231, 214), (234, 240, 227), (212, 226, 207)),
    "dirt": ((244, 232, 210), (236, 218, 193), (249, 238, 218), (229, 207, 181)),
    "stone": ((235, 236, 232), (220, 224, 220), (243, 241, 231), (211, 216, 213)),
    "wood": ((238, 220, 194), (224, 199, 166), (245, 231, 207), (211, 183, 149)),
}


def chunk(kind: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + kind
        + payload
        + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    )


def encode_png(width: int, height: int, pixels: bytes) -> bytes:
    if len(pixels) != width * height * 4:
        raise ValueError("RGBA payload size does not match dimensions")
    rows = b"".join(
        b"\x00" + pixels[y * width * 4:(y + 1) * width * 4]
        for y in range(height)
    )
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(rows, level=9))
        + chunk(b"IEND", b"")
    )


def tile_pixels(name: str) -> bytes:
    palette = PALETTES[name]
    result = bytearray()
    salt = sum(ord(character) for character in name)
    for y_value in range(TILE_SIZE):
        for x_value in range(TILE_SIZE):
            block_x = x_value // 4
            block_y = y_value // 4
            mixed = (
                block_x * 1103515245
                ^ block_y * 12345
                ^ (block_x + 3) * (block_y + 5) * 2654435761
                ^ salt * 2246822519
            ) & 0xFFFFFFFF
            value = ((mixed ^ (mixed >> 13) ^ (mixed >> 23)) >> 5) & 3
            red, green, blue = palette[value]
            result.extend((red, green, blue, 255))
    return bytes(result)


def atlas_pixels(tiles: dict[str, bytes]) -> bytes:
    order = (("forest", "dirt"), ("stone", "wood"))
    size = TILE_SIZE * 2
    result = bytearray(size * size * 4)
    for tile_y, row in enumerate(order):
        for tile_x, name in enumerate(row):
            source = tiles[name]
            for y_value in range(TILE_SIZE):
                source_start = y_value * TILE_SIZE * 4
                target_start = (
                    (tile_y * TILE_SIZE + y_value) * size
                    + tile_x * TILE_SIZE
                ) * 4
                result[target_start:target_start + TILE_SIZE * 4] = (
                    source[source_start:source_start + TILE_SIZE * 4]
                )
    return bytes(result)


def generate(output_directory: Path) -> None:
    output_directory.mkdir(parents=True, exist_ok=True)
    tiles = {name: tile_pixels(name) for name in PALETTES}
    for name, pixels in tiles.items():
        path = output_directory / f"workout-game-surface-{name}.png"
        path.write_bytes(encode_png(TILE_SIZE, TILE_SIZE, pixels))
    atlas = output_directory / "workout-game-surface-atlas.png"
    atlas.write_bytes(encode_png(
        TILE_SIZE * 2, TILE_SIZE * 2, atlas_pixels(tiles)))


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate pixel surface atlas")
    parser.add_argument("--output-dir", required=True, type=Path)
    generate(parser.parse_args().output_dir.resolve())


if __name__ == "__main__":
    main()
