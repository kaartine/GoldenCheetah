#!/usr/bin/env python3
"""Inspect cleanup GLBs without loading untrusted data into Blender."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import sys


GLB_HEADER = struct.Struct("<4sII")
CHUNK_HEADER = struct.Struct("<II")
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
FORBIDDEN = {
    "animations",
    "cameras",
    "images",
    "samplers",
    "skins",
    "textures",
    "extensionsRequired",
}
IDENTITY_MATRIX = (
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
)


def read_glb(path: Path) -> tuple[dict, bytes]:
    data = path.read_bytes()
    magic, version, length = GLB_HEADER.unpack_from(data)
    assert magic == b"glTF" and version == 2 and length == len(data)
    document = None
    binary = b""
    offset = GLB_HEADER.size
    while offset < len(data):
        chunk_length, chunk_type = CHUNK_HEADER.unpack_from(data, offset)
        offset += CHUNK_HEADER.size
        payload = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == JSON_CHUNK:
            document = json.loads(payload.rstrip(b" \t\r\n\0"))
        elif chunk_type == BIN_CHUNK:
            binary = payload
    assert isinstance(document, dict)
    return document, binary


def iter_values(value):
    if isinstance(value, dict):
        for key, child in value.items():
            yield key, child
            yield from iter_values(child)
    elif isinstance(value, list):
        for child in value:
            yield from iter_values(child)


def accessor_bytes(document: dict, binary: bytes, accessor_index: int) -> bytes:
    accessor = document["accessors"][accessor_index]
    view = document["bufferViews"][accessor["bufferView"]]
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    components = {5126: 4}[accessor["componentType"]]
    widths = {"VEC3": 3}[accessor["type"]]
    length = accessor["count"] * components * widths
    return binary[start:start + length]


def position_bounds(document: dict, binary: bytes) -> tuple[list[float], list[float]]:
    positions = []
    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            index = primitive["attributes"]["POSITION"]
            accessor = document["accessors"][index]
            assert accessor["componentType"] == 5126 and accessor["type"] == "VEC3"
            payload = accessor_bytes(document, binary, index)
            positions.extend(struct.iter_unpack("<fff", payload))
    assert positions
    minimum = [min(point[index] for point in positions) for index in range(3)]
    maximum = [max(point[index] for point in positions) for index in range(3)]
    return minimum, maximum


def triangle_count(document: dict) -> int:
    result = 0
    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            assert primitive.get("mode", 4) == 4
            accessor_index = primitive.get("indices")
            if accessor_index is None:
                position = primitive["attributes"]["POSITION"]
                count = document["accessors"][position]["count"]
            else:
                count = document["accessors"][accessor_index]["count"]
            assert count % 3 == 0
            result += count // 3
    return result


def assert_identity_nodes(document: dict) -> None:
    for node in document.get("nodes", []):
        assert node.get("translation", [0.0, 0.0, 0.0]) == [0.0, 0.0, 0.0]
        assert node.get("rotation", [0.0, 0.0, 0.0, 1.0]) == [0.0, 0.0, 0.0, 1.0]
        assert node.get("scale", [1.0, 1.0, 1.0]) == [1.0, 1.0, 1.0]
        matrix = node.get("matrix")
        if matrix is not None:
            assert all(math.isclose(value, expected, abs_tol=1.0e-7)
                       for value, expected in zip(matrix, IDENTITY_MATRIX))


def inspect(
    path: Path, expected_name: str, budget: int
) -> tuple[int, list[float], list[float]]:
    document, binary = read_glb(path)
    assert not (FORBIDDEN & document.keys())
    assert len(document.get("materials", [])) <= 1
    assert len(document.get("meshes", [])) == 1
    assert len(document.get("nodes", [])) == 1
    assert document["nodes"][0]["name"] == expected_name
    for key, value in iter_values(document):
        assert key != "uri", f"external URI survived in {path}: {value}"
        assert key not in {"camera", "skin", "weights", "targets"}
    assert_identity_nodes(document)
    triangles = triangle_count(document)
    assert 0 < triangles <= budget
    minimum, maximum = position_bounds(document, binary)
    assert math.isclose(minimum[1], 0.0, abs_tol=1.0e-5)
    assert math.isclose(minimum[0] + maximum[0], 0.0, abs_tol=1.0e-5)
    assert math.isclose(minimum[2] + maximum[2], 0.0, abs_tol=1.0e-5)
    return triangles, minimum, maximum


def inspect_raw(path: Path) -> int:
    document, _binary = read_glb(path)
    assert document.get("animations")
    assert document.get("cameras")
    assert document.get("images")
    assert document.get("skins")
    assert document.get("textures")
    assert "KHR_lights_punctual" in document.get("extensionsUsed", [])
    assert any(key == "uri" for key, _value in iter_values(document))
    return triangle_count(document)


def assert_matching_bounds(
    reference: tuple[list[float], list[float]],
    candidate: tuple[list[float], list[float]],
    relative_tolerance: float,
) -> None:
    reference_minimum, reference_maximum = reference
    candidate_minimum, candidate_maximum = candidate
    for index in range(3):
        extent = reference_maximum[index] - reference_minimum[index]
        tolerance = max(1.0e-5, extent * relative_tolerance)
        assert math.isclose(
            candidate_minimum[index], reference_minimum[index], abs_tol=tolerance
        )
        assert math.isclose(
            candidate_maximum[index], reference_maximum[index], abs_tol=tolerance
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--expected-up", default="+Y")
    parser.add_argument("--expected-forward", default="+Z")
    parser.add_argument("--expected-scale", default=1.0, type=float)
    parser.add_argument("--max-lod-bound-error", default=0.05, type=float)
    arguments = parser.parse_args()
    if arguments.raw is not None:
        assert inspect_raw(arguments.raw) > 18_000
    lod0, lod0_minimum, lod0_maximum = inspect(
        arguments.output_dir / "candidate-lod0.glb", "CandidateLOD0", 18_000
    )
    lod1, lod1_minimum, lod1_maximum = inspect(
        arguments.output_dir / "candidate-lod1.glb", "CandidateLOD1", 6_000
    )
    collision, collision_minimum, collision_maximum = inspect(
        arguments.output_dir / "candidate-collision.glb", "CandidateCollision", 100
    )
    assert lod1 <= math.floor(lod0 * 0.40)
    assert collision <= 100
    assert_matching_bounds(
        (lod0_minimum, lod0_maximum),
        (lod1_minimum, lod1_maximum),
        arguments.max_lod_bound_error,
    )
    assert_matching_bounds(
        (lod0_minimum, lod0_maximum),
        (collision_minimum, collision_maximum),
        1.0e-6,
    )
    report = json.loads((arguments.output_dir / "cleanup-report.json").read_text())
    assert report["normalization"]["up"] == arguments.expected_up
    assert report["normalization"]["forward"] == arguments.expected_forward
    assert math.isclose(
        report["normalization"]["metresPerUnit"], arguments.expected_scale
    )
    assert report["technical"]["lod0"]["triangles"] == lod0
    assert report["technical"]["lod1"]["triangles"] == lod1
    assert report["technical"]["collision"]["triangles"] == collision
    if arguments.raw is not None:
        assert report["removed"]["animations"] > 0
        assert report["removed"]["cameras"] > 0
        assert report["removed"]["images"] > 0
        assert report["removed"]["lights"] > 0
        assert report["removed"]["materials"] > 0
        assert report["removed"]["skins"] > 0
        assert report["removed"]["textures"] > 0
        assert report["cleanup"]["removedFragments"] > 0
    print(f"Validated cleanup output: LOD0={lod0}, LOD1={lod1}, collision={collision}")


if __name__ == "__main__":
    main()
