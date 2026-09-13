#!/usr/bin/env python3
"""Quarantine and validate an externally generated TripoSR GLB candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import stat
import struct
import sys
from typing import Any, Iterable, NamedTuple
from urllib.parse import urlparse


VERSION = 1
SCRIPT_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY = SCRIPT_DIRECTORY.parents[2]
SHA256_PATTERN = re.compile(r"^[0-9a-f]{64}$")
CANDIDATE_ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9._-]{2,63}$")
REVISION_PATTERN = re.compile(r"^[0-9a-f]{40}$")
GENERATOR_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9 ._+()-]{0,127}$")
AXES = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"}
RIGHTS_BASES = {
    "owned",
    "licensed",
    "permission",
    "public-domain",
    "unverified-review-only",
}
MIME_MAGIC = (
    (b"\x89PNG\r\n\x1a\n", "image/png"),
    (b"\xff\xd8\xff", "image/jpeg"),
    (b"RIFF", "image/webp"),
)
GLB_HEADER = struct.Struct("<4sII")
CHUNK_HEADER = struct.Struct("<II")
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
MIB = 1024 * 1024
HARD_MAX_GLB_BYTES = 64 * MIB
HARD_MAX_COLLISION_GLB_BYTES = 4 * MIB
HARD_MAX_REFERENCE_BYTES = 64 * MIB
HARD_MAX_MODEL_WEIGHTS_BYTES = 2 * 1024 * MIB
HARD_MAX_TRIANGLES_LOD0 = 18_000
HARD_MAX_TRIANGLES_LOD1 = 9_000
HARD_MAX_COLLISION_TRIANGLES = 100
HARD_MAX_MATERIALS = 16
HARD_MAX_TEXTURES = 0
HARD_MAX_TEXTURE_BYTES = 0
HARD_MAX_ACCESSORS = 128
HARD_MAX_BUFFER_VIEWS = 128
HARD_MAX_MESHES = 64
HARD_MAX_NODES = 128
HARD_MAX_PRIMITIVES = 128
HARD_MAX_COMPONENT_VALUES = 2_000_000
HARD_MAX_JSON_VALUES = 1_000_000
COMPONENTS = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_COMPONENTS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
    "MAT2": 4,
    "MAT3": 9,
    "MAT4": 16,
}
CHECKS = [
    "source-reference-sha256",
    "model-weights-sha256",
    "candidate-glb-sha256",
    "glb-container-structure",
    "embedded-resources-only",
    "finite-float-accessors-and-transforms",
    "finite-nonempty-triangle-geometry",
    "valid-position-and-index-ranges",
    "declared-axis-and-unit-metadata",
    "glb-size-budget",
    "triangle-budget",
    "lod1-lighter-than-lod0",
    "material-budget",
    "texture-count-and-byte-budget",
    "optional-collision-proxy-contract",
]

TOP_LEVEL_KEYS = {
    "asset", "buffers", "bufferViews", "accessors", "materials", "meshes",
    "nodes", "scenes", "scene",
}
ATTRIBUTE_FORMATS = {
    "POSITION": {(5126, "VEC3", False)},
    "NORMAL": {(5126, "VEC3", False)},
    "COLOR_0": {
        (5121, "VEC3", True), (5121, "VEC4", True),
        (5123, "VEC3", True), (5123, "VEC4", True),
        (5126, "VEC3", False), (5126, "VEC4", False),
    },
}


class CandidateError(ValueError):
    pass


class FileSnapshot(NamedTuple):
    path: Path
    data: bytes
    sha256: str

    @property
    def size(self) -> int:
        return len(self.data)


class FileDigest(NamedTuple):
    path: Path
    size: int
    sha256: str


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CandidateError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def load_json(data: bytes, description: str) -> Any:
    try:
        document = json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_unique_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                CandidateError(f"non-finite JSON number in {description}: {value}")
            ),
        )
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as error:
        raise CandidateError(f"invalid JSON in {description}: {error}") from error
    _reject_nonfinite(document)
    return document


def _inside(path: Path, directory: Path) -> bool:
    return path == directory or directory in path.parents


def _external_resolved(path: Path, description: str) -> Path:
    expanded = path.expanduser()
    absolute = expanded if expanded.is_absolute() else Path.cwd() / expanded
    for component in (absolute, *absolute.parents):
        if component.is_symlink():
            raise CandidateError(f"{description} may not traverse a symlink")
        if component == component.parent:
            break
    try:
        resolved = absolute.resolve(strict=True)
    except OSError as error:
        raise CandidateError(f"{description} cannot be resolved: {error}") from error
    if _inside(resolved, REPOSITORY.resolve(strict=True)):
        raise CandidateError(f"{description} must remain outside the repository")
    return resolved


def _open_regular(path: Path, description: str) -> tuple[int, Path]:
    resolved = _external_resolved(path, description)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(resolved, flags)
    except OSError as error:
        raise CandidateError(
            f"{description} must be a readable regular, non-symlink file: {error}"
        ) from error
    metadata = os.fstat(descriptor)
    if not stat.S_ISREG(metadata.st_mode):
        os.close(descriptor)
        raise CandidateError(f"{description} must be a regular, non-symlink file")
    return descriptor, resolved


def read_external_snapshot(
    path: Path, description: str, maximum_bytes: int
) -> FileSnapshot:
    descriptor, resolved = _open_regular(path, description)
    try:
        initial = os.fstat(descriptor)
        if initial.st_size <= 0 or initial.st_size > maximum_bytes:
            raise CandidateError(
                f"{description} size {initial.st_size} is outside the allowed "
                f"1..{maximum_bytes} bytes"
            )
        chunks: list[bytes] = []
        remaining = maximum_bytes + 1
        while remaining:
            block = os.read(descriptor, min(MIB, remaining))
            if not block:
                break
            chunks.append(block)
            remaining -= len(block)
        data = b"".join(chunks)
        final = os.fstat(descriptor)
        if len(data) <= 0 or len(data) > maximum_bytes:
            raise CandidateError(f"{description} exceeds its hard byte limit")
        if (
            initial.st_dev != final.st_dev
            or initial.st_ino != final.st_ino
            or initial.st_size != final.st_size
            or final.st_size != len(data)
            or initial.st_mtime_ns != final.st_mtime_ns
        ):
            raise CandidateError(f"{description} changed while it was being read")
        return FileSnapshot(resolved, data, hashlib.sha256(data).hexdigest())
    finally:
        os.close(descriptor)


def digest_external_file(path: Path, description: str, maximum_bytes: int) -> FileDigest:
    descriptor, resolved = _open_regular(path, description)
    try:
        initial = os.fstat(descriptor)
        if initial.st_size <= 0 or initial.st_size > maximum_bytes:
            raise CandidateError(
                f"{description} size {initial.st_size} is outside the allowed "
                f"1..{maximum_bytes} bytes"
            )
        digest = hashlib.sha256()
        size = 0
        while True:
            block = os.read(descriptor, MIB)
            if not block:
                break
            size += len(block)
            if size > maximum_bytes:
                raise CandidateError(f"{description} exceeds its hard byte limit")
            digest.update(block)
        final = os.fstat(descriptor)
        if (
            initial.st_dev != final.st_dev
            or initial.st_ino != final.st_ino
            or initial.st_size != final.st_size
            or final.st_size != size
            or initial.st_mtime_ns != final.st_mtime_ns
        ):
            raise CandidateError(f"{description} changed while it was being read")
        return FileDigest(resolved, size, digest.hexdigest())
    finally:
        os.close(descriptor)


def external_destination(path: Path) -> Path:
    expanded = path.expanduser()
    absolute = expanded if expanded.is_absolute() else Path.cwd() / expanded
    for parent in (absolute, *absolute.parents):
        if parent.is_symlink():
            raise CandidateError("candidate directory may not traverse a symlink")
        if parent == parent.parent:
            break
    resolved = absolute.resolve(strict=False)
    if _inside(resolved, REPOSITORY.resolve(strict=True)):
        raise CandidateError("candidate directory must remain outside the repository")
    return resolved


def require_hash(actual: str, expected: str, description: str) -> None:
    if not isinstance(expected, str) or not SHA256_PATTERN.fullmatch(expected):
        raise CandidateError(f"{description} expected SHA-256 is malformed")
    if actual != expected:
        raise CandidateError(
            f"{description} SHA-256 mismatch: expected {expected}, got {actual}"
        )


def reference_mime(data: bytes) -> str:
    prefix = data[:12]
    for magic, mime_type in MIME_MAGIC:
        if prefix.startswith(magic):
            if mime_type != "image/webp" or prefix[8:12] == b"WEBP":
                return mime_type
    raise CandidateError("reference input is not a supported PNG, JPEG or WebP image")


def _reject_nonfinite(value: Any, location: str = "$") -> None:
    pending = [(value, location)]
    visited = 0
    while pending:
        current, current_location = pending.pop()
        visited += 1
        if visited > HARD_MAX_JSON_VALUES:
            raise CandidateError("JSON document contains too many values")
        if isinstance(current, float) and not math.isfinite(current):
            raise CandidateError(f"non-finite JSON number at {current_location}")
        if isinstance(current, list):
            pending.extend(
                (child, f"{current_location}[{index}]")
                for index, child in enumerate(current)
            )
        elif isinstance(current, dict):
            pending.extend(
                (child, f"{current_location}.{key}")
                for key, child in current.items()
            )


def read_glb(data: bytes) -> tuple[dict[str, Any], bytes]:
    if len(data) > HARD_MAX_GLB_BYTES:
        raise CandidateError("GLB exceeds the 64 MiB hard limit")
    if len(data) < GLB_HEADER.size:
        raise CandidateError("GLB header is truncated")
    magic, version, declared_length = GLB_HEADER.unpack_from(data)
    if magic != b"glTF" or version != 2 or declared_length != len(data):
        raise CandidateError("GLB header is invalid or has a wrong length")

    chunks: list[tuple[int, bytes]] = []
    offset = GLB_HEADER.size
    while offset < len(data):
        if offset + CHUNK_HEADER.size > len(data):
            raise CandidateError("GLB chunk header is truncated")
        length, chunk_type = CHUNK_HEADER.unpack_from(data, offset)
        offset += CHUNK_HEADER.size
        if length % 4 or offset + length > len(data):
            raise CandidateError("GLB chunk length or alignment is invalid")
        chunks.append((chunk_type, data[offset : offset + length]))
        offset += length
    if offset != len(data) or not chunks or chunks[0][0] != JSON_CHUNK:
        raise CandidateError("GLB must start with one JSON chunk")
    if len(chunks) > 2 or (len(chunks) == 2 and chunks[1][0] != BIN_CHUNK):
        raise CandidateError("GLB may contain only one JSON and one BIN chunk")

    document = load_json(chunks[0][1].rstrip(b" \x00"), "GLB JSON chunk")
    asset = document.get("asset") if isinstance(document, dict) else None
    if not isinstance(asset, dict) or asset.get("version") != "2.0":
        raise CandidateError("GLB does not declare glTF 2.0")
    _reject_nonfinite(document)
    binary = chunks[1][1] if len(chunks) == 2 else b""
    return document, binary


def _item(sequence: list[Any], index: Any, description: str) -> Any:
    if not isinstance(sequence, list):
        raise CandidateError(f"{description} collection is not an array")
    if not isinstance(index, int) or isinstance(index, bool):
        raise CandidateError(f"{description} index is not an integer")
    if index < 0 or index >= len(sequence):
        raise CandidateError(f"{description} index {index} is out of range")
    return sequence[index]


def accessor_values(
    document: dict[str, Any], binary: bytes, accessor_index: int
) -> Iterable[tuple[int | float, ...]]:
    accessors = document.get("accessors", [])
    views = document.get("bufferViews", [])
    accessor = _item(accessors, accessor_index, "accessor")
    if not isinstance(accessor, dict) or "sparse" in accessor:
        raise CandidateError("sparse or malformed accessors are not accepted")
    component_type = accessor.get("componentType")
    accessor_type = accessor.get("type")
    if component_type not in COMPONENTS or accessor_type not in TYPE_COMPONENTS:
        raise CandidateError("accessor component or shape is unsupported")
    count = accessor.get("count")
    if not isinstance(count, int) or isinstance(count, bool) or count <= 0:
        raise CandidateError("accessor count must be a positive integer")
    view = _item(views, accessor.get("bufferView"), "bufferView")
    if not isinstance(view, dict) or view.get("buffer") != 0:
        raise CandidateError("accessor must use the embedded GLB buffer")

    fmt, component_bytes = COMPONENTS[component_type]
    component_count = TYPE_COMPONENTS[accessor_type]
    element_bytes = component_bytes * component_count
    stride = view.get("byteStride", element_bytes)
    if (
        not isinstance(stride, int)
        or isinstance(stride, bool)
        or stride < element_bytes
        or stride > 252
        or stride % component_bytes
    ):
        raise CandidateError("accessor byte stride is invalid")
    view_offset = view.get("byteOffset", 0)
    accessor_offset = accessor.get("byteOffset", 0)
    view_length = view.get("byteLength")
    if (
        not all(
            isinstance(value, int) and not isinstance(value, bool)
            for value in (view_offset, accessor_offset, view_length)
        )
        or view_offset < 0
        or accessor_offset < 0
        or view_length < 0
    ):
        raise CandidateError("accessor offsets must be integers")
    if accessor_offset % component_bytes:
        raise CandidateError("accessor byte offset is not component-aligned")
    start = view_offset + accessor_offset
    end = start + stride * (count - 1) + element_bytes
    view_end = view_offset + view_length
    if start < view_offset or end > view_end or view_end > len(binary):
        raise CandidateError("accessor data is outside its buffer view")
    unpacker = struct.Struct("<" + fmt * component_count)
    for index in range(count):
        yield unpacker.unpack_from(binary, start + index * stride)


def _axis_family(axis: str) -> str:
    return axis[-1:] if axis in AXES else ""


def validate_axes(unit_meters: float, up_axis: str, forward_axis: str) -> None:
    if not math.isfinite(unit_meters) or unit_meters <= 0:
        raise CandidateError("unit-meters must be finite and greater than zero")
    if up_axis not in AXES or forward_axis not in AXES:
        raise CandidateError("up and forward axes must use signed X, Y or Z")
    if _axis_family(up_axis) == _axis_family(forward_axis):
        raise CandidateError("up and forward axes must be orthogonal")


def _keys(value: Any, allowed: set[str], required: set[str], description: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise CandidateError(f"{description} must be an object")
    unknown = set(value) - allowed
    missing = required - set(value)
    if unknown or missing:
        raise CandidateError(
            f"{description} has unsupported or missing fields: "
            f"unknown={sorted(unknown)}, missing={sorted(missing)}"
        )
    return value


def _finite_number(value: Any, description: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise CandidateError(f"{description} must be numeric")
    try:
        number = float(value)
    except OverflowError as error:
        raise CandidateError(f"{description} is outside the numeric range") from error
    if not math.isfinite(number):
        raise CandidateError(f"{description} must be finite")
    return number


def _vector(value: Any, length: int, description: str) -> list[float]:
    if not isinstance(value, list) or len(value) != length:
        raise CandidateError(f"{description} must be a vector of length {length}")
    return [_finite_number(component, description) for component in value]


def _identity_transform(node: dict[str, Any], description: str) -> None:
    if "matrix" in node and any(key in node for key in ("translation", "rotation", "scale")):
        raise CandidateError(f"{description} mixes matrix and TRS transforms")
    if "matrix" in node:
        matrix = _vector(node["matrix"], 16, f"{description}.matrix")
        identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
                    0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
        if matrix != identity:
            raise CandidateError("candidate nodes may only use identity transforms")
    for key, identity in (
        ("translation", [0.0, 0.0, 0.0]),
        ("rotation", [0.0, 0.0, 0.0, 1.0]),
        ("scale", [1.0, 1.0, 1.0]),
    ):
        if key in node and _vector(node[key], len(identity), f"{description}.{key}") != identity:
            raise CandidateError("candidate nodes may only use identity transforms")


def _validate_material(material: Any, index: int) -> None:
    value = _keys(
        material,
        {
            "name", "pbrMetallicRoughness", "doubleSided", "alphaMode",
            "alphaCutoff", "emissiveFactor",
        },
        set(),
        f"material {index}",
    )
    if "name" in value and (not isinstance(value["name"], str) or len(value["name"]) > 128):
        raise CandidateError("material name is malformed")
    if "doubleSided" in value and not isinstance(value["doubleSided"], bool):
        raise CandidateError("material doubleSided must be boolean")
    alpha_mode = value.get("alphaMode", "OPAQUE")
    if not isinstance(alpha_mode, str) or alpha_mode not in {"OPAQUE", "MASK", "BLEND"}:
        raise CandidateError("material alphaMode is unsupported")
    if "alphaCutoff" in value:
        _finite_number(value["alphaCutoff"], "material alphaCutoff")
    if "emissiveFactor" in value:
        emissive = _vector(value["emissiveFactor"], 3, "material emissiveFactor")
        if any(component < 0 or component > 1 for component in emissive):
            raise CandidateError("material emissiveFactor is outside 0..1")
    if "pbrMetallicRoughness" in value:
        pbr = _keys(
            value["pbrMetallicRoughness"],
            {"baseColorFactor", "metallicFactor", "roughnessFactor"},
            set(),
            f"material {index} PBR",
        )
        if "baseColorFactor" in pbr:
            color = _vector(pbr["baseColorFactor"], 4, "material baseColorFactor")
            if any(component < 0 or component > 1 for component in color):
                raise CandidateError("material baseColorFactor is outside 0..1")
        for field in ("metallicFactor", "roughnessFactor"):
            if field in pbr:
                factor = _finite_number(pbr[field], f"material {field}")
                if factor < 0 or factor > 1:
                    raise CandidateError(f"material {field} is outside 0..1")


def inspect_glb(data: bytes, budgets: dict[str, int], *, collision: bool = False) -> dict[str, Any]:
    size = len(data)
    if size > HARD_MAX_GLB_BYTES:
        raise CandidateError("GLB exceeds the 64 MiB hard limit")
    if size > budgets["maxGlbBytes"]:
        raise CandidateError("GLB exceeds the byte budget")
    document, binary = read_glb(data)
    if not isinstance(document, dict):
        raise CandidateError("GLB JSON root must be an object")
    unknown_top = set(document) - TOP_LEVEL_KEYS
    required_top = TOP_LEVEL_KEYS - {"materials"}
    if unknown_top or not required_top.issubset(document):
        raise CandidateError(
            "GLB contains unsupported top-level structures or is missing required structures"
        )
    asset = _keys(document["asset"], {"version", "generator"}, {"version"}, "GLB asset")
    if asset["version"] != "2.0":
        raise CandidateError("GLB does not declare glTF 2.0")
    if "generator" in asset and (
        not isinstance(asset["generator"], str) or len(asset["generator"]) > 256
    ):
        raise CandidateError("GLB generator metadata is malformed")
    buffers = document.get("buffers", [])
    if (
        not isinstance(buffers, list)
        or len(buffers) != 1
    ):
        raise CandidateError("GLB must contain exactly one embedded buffer")
    buffer = _keys(buffers[0], {"byteLength"}, {"byteLength"}, "GLB buffer")
    declared_binary = buffer.get("byteLength")
    if (
        not isinstance(declared_binary, int)
        or isinstance(declared_binary, bool)
        or declared_binary < 0
        or declared_binary > len(binary)
        or len(binary) - declared_binary > 3
    ):
        raise CandidateError("embedded buffer length is invalid")
    if any(binary[declared_binary:]):
        raise CandidateError("embedded buffer padding must contain only zero bytes")

    views = document.get("bufferViews", [])
    if not isinstance(views, list) or len(views) > HARD_MAX_BUFFER_VIEWS:
        raise CandidateError("GLB bufferViews must be an array")
    for index, raw_view in enumerate(views):
        view = _keys(
            raw_view,
            {"buffer", "byteOffset", "byteLength", "byteStride", "target", "name"},
            {"buffer", "byteLength"},
            f"bufferView {index}",
        )
        if view.get("buffer") != 0:
            raise CandidateError("buffer view does not use the embedded buffer")
        offset = view.get("byteOffset", 0)
        length = view.get("byteLength")
        if (
            not isinstance(offset, int) or isinstance(offset, bool)
            or not isinstance(length, int) or isinstance(length, bool)
            or offset < 0
            or length < 0
            or offset + length > declared_binary
        ):
            raise CandidateError("buffer view is outside the embedded buffer")
        if "byteStride" in view:
            stride = view["byteStride"]
            if (
                not isinstance(stride, int) or isinstance(stride, bool)
                or stride < 4 or stride > 252 or stride % 4
            ):
                raise CandidateError("buffer view byteStride is invalid")
        if "target" in view and view["target"] not in {34962, 34963}:
            raise CandidateError("buffer view target is unsupported")
        if "name" in view and (not isinstance(view["name"], str) or len(view["name"]) > 128):
            raise CandidateError("buffer view name is malformed")

    materials = document.get("materials", [])
    accessors = document.get("accessors", [])
    meshes = document.get("meshes", [])
    nodes = document.get("nodes", [])
    scenes = document.get("scenes", [])
    if not all(isinstance(value, list) for value in (materials, accessors, meshes, nodes, scenes)):
        raise CandidateError("GLB resource collections must be arrays")
    material_count = len(materials)
    if material_count > HARD_MAX_MATERIALS:
        raise CandidateError("GLB exceeds the hard material limit")
    if collision and material_count:
        raise CandidateError("collision proxy must not contain materials")
    for index, material in enumerate(materials):
        _validate_material(material, index)
    if material_count > budgets["maxMaterials"]:
        raise CandidateError("GLB exceeds the material budget")
    if budgets["maxTextures"] != 0 or budgets["maxTextureBytes"] != 0:
        raise CandidateError("safe candidate profile requires zero texture budgets")

    if len(accessors) > HARD_MAX_ACCESSORS:
        raise CandidateError("GLB exceeds the hard accessor limit")
    accessor_cache: dict[int, list[tuple[int | float, ...]]] = {}
    total_components = 0
    for accessor_index, raw_accessor in enumerate(accessors):
        accessor = _keys(
            raw_accessor,
            {
                "bufferView", "byteOffset", "componentType", "normalized",
                "count", "type", "min", "max", "name",
            },
            {"bufferView", "componentType", "count", "type"},
            f"accessor {accessor_index}",
        )
        if "normalized" in accessor and not isinstance(accessor["normalized"], bool):
            raise CandidateError("accessor normalized must be boolean")
        if "name" in accessor and (
            not isinstance(accessor["name"], str) or len(accessor["name"]) > 128
        ):
            raise CandidateError("accessor name is malformed")
        count = accessor.get("count")
        component_count = TYPE_COMPONENTS.get(accessor.get("type"), 0)
        if (
            not isinstance(count, int) or isinstance(count, bool)
            or count <= 0 or not component_count
        ):
            raise CandidateError("accessor count or type is unsupported")
        total_components += count * component_count
        if total_components > HARD_MAX_COMPONENT_VALUES:
            raise CandidateError("GLB exceeds the decoded accessor component limit")
        values = list(accessor_values(document, binary, accessor_index))
        for components in values:
            if not all(math.isfinite(float(component)) for component in components):
                raise CandidateError("GLB contains non-finite accessor geometry")
        accessor_cache[accessor_index] = values
        for field in ("min", "max"):
            if field in accessor:
                declared = _vector(accessor[field], component_count, f"accessor {field}")
                actual = [
                    (min if field == "min" else max)(float(row[i]) for row in values)
                    for i in range(component_count)
                ]
                if declared != actual:
                    raise CandidateError(f"accessor {field} does not match embedded data")

    triangle_count = 0
    primitive_count = 0
    vertex_count = 0
    bounds_min = [math.inf, math.inf, math.inf]
    bounds_max = [-math.inf, -math.inf, -math.inf]
    if not meshes or len(meshes) > HARD_MAX_MESHES:
        raise CandidateError("GLB mesh inventory is empty or too large")
    referenced_accessors: set[int] = set()
    referenced_views: set[int] = set()
    for mesh_index, raw_mesh in enumerate(meshes):
        mesh = _keys(
            raw_mesh, {"name", "primitives", "extras"}, {"primitives"},
            f"mesh {mesh_index}",
        )
        if "name" in mesh and (not isinstance(mesh["name"], str) or len(mesh["name"]) > 128):
            raise CandidateError("mesh name is malformed")
        if "extras" in mesh and mesh["extras"] != {"processed": True}:
            raise CandidateError("only TripoSR's exact processed mesh marker is accepted")
        if not isinstance(mesh["primitives"], list) or not mesh["primitives"]:
            raise CandidateError("GLB mesh primitives must be a nonempty array")
        for raw_primitive in mesh["primitives"]:
            primitive = _keys(
                raw_primitive,
                {"attributes", "indices", "material", "mode"},
                {"attributes"},
                "mesh primitive",
            )
            primitive_count += 1
            if primitive_count > HARD_MAX_PRIMITIVES:
                raise CandidateError("GLB exceeds the hard primitive limit")
            if primitive.get("mode", 4) != 4:
                raise CandidateError("only triangle primitives are accepted")
            attributes = primitive.get("attributes")
            allowed_attributes = {"POSITION"} if collision else set(ATTRIBUTE_FORMATS)
            if (
                not isinstance(attributes, dict)
                or "POSITION" not in attributes
                or set(attributes) - allowed_attributes
            ):
                raise CandidateError("GLB primitive attributes are malformed")
            position_index = attributes.get("POSITION")
            if "material" in primitive:
                if collision:
                    raise CandidateError("collision proxy primitive must not use a material")
                _item(materials, primitive["material"], "primitive material")
            position_accessor = _item(
                accessors, position_index, "POSITION accessor"
            )
            if (
                position_accessor.get("componentType") != 5126
                or position_accessor.get("type") != "VEC3"
            ):
                raise CandidateError("POSITION must be a float VEC3 accessor")
            positions = accessor_cache[position_index]
            referenced_accessors.add(position_index)
            vertex_count += len(positions)
            for position in positions:
                for axis in range(3):
                    bounds_min[axis] = min(bounds_min[axis], float(position[axis]))
                    bounds_max[axis] = max(bounds_max[axis], float(position[axis]))

            for semantic, accessor_index in attributes.items():
                accessor = _item(accessors, accessor_index, f"{semantic} accessor")
                normalized = accessor.get("normalized", False)
                signature = (accessor.get("componentType"), accessor.get("type"), normalized)
                if signature not in ATTRIBUTE_FORMATS[semantic]:
                    raise CandidateError(f"{semantic} accessor format is unsupported")
                if accessor.get("count") != len(positions):
                    raise CandidateError("primitive vertex attributes have different counts")
                referenced_accessors.add(accessor_index)

            if "indices" in primitive:
                index_accessor = _item(
                    accessors, primitive["indices"], "index accessor"
                )
                if (
                    index_accessor.get("componentType") not in {5121, 5123, 5125}
                    or index_accessor.get("type") != "SCALAR"
                    or index_accessor.get("normalized", False) is not False
                ):
                    raise CandidateError("indices must be unsigned scalar values")
                index_index = primitive["indices"]
                indices = [int(value[0]) for value in accessor_cache[index_index]]
                referenced_accessors.add(index_index)
                if any(index >= len(positions) for index in indices):
                    raise CandidateError("primitive index exceeds POSITION count")
                element_count = len(indices)
            else:
                element_count = len(positions)
            if element_count % 3:
                raise CandidateError("triangle primitive element count is not divisible by 3")
            triangle_count += element_count // 3

    if primitive_count == 0 or triangle_count == 0:
        raise CandidateError("GLB contains no triangle geometry")
    if referenced_accessors != set(range(len(accessors))):
        raise CandidateError("GLB contains unreferenced accessors")
    for accessor_index in referenced_accessors:
        referenced_views.add(accessors[accessor_index]["bufferView"])
    if referenced_views != set(range(len(views))):
        raise CandidateError("GLB contains unreferenced buffer views")
    accessors_by_view: dict[int, list[int]] = {index: [] for index in range(len(views))}
    for accessor_index, accessor in enumerate(accessors):
        accessors_by_view[accessor["bufferView"]].append(accessor_index)
    for view_index, view in enumerate(views):
        intervals: list[tuple[int, int]] = []
        stride = view.get("byteStride")
        counts: set[int] = set()
        for accessor_index in accessors_by_view[view_index]:
            accessor = accessors[accessor_index]
            component_bytes = COMPONENTS[accessor["componentType"]][1]
            element_bytes = component_bytes * TYPE_COMPONENTS[accessor["type"]]
            offset = accessor.get("byteOffset", 0)
            count = accessor["count"]
            counts.add(count)
            if stride is None:
                intervals.append((offset, offset + element_bytes * count))
            else:
                intervals.append((offset, offset + element_bytes))
        intervals.sort()
        cursor = 0
        for start, end in intervals:
            if start != cursor or end <= start:
                raise CandidateError("buffer view contains overlapping or unexplained bytes")
            cursor = end
        if stride is None:
            expected_length = cursor
        else:
            if len(counts) != 1 or cursor != stride:
                raise CandidateError("interleaved buffer view layout is incomplete")
            expected_length = stride * next(iter(counts))
        if expected_length != view["byteLength"]:
            raise CandidateError("buffer view contains unexplained bytes")
    view_regions = sorted(
        (view.get("byteOffset", 0), view.get("byteOffset", 0) + view["byteLength"])
        for view in views
    )
    cursor = 0
    for start, end in view_regions:
        if start != cursor or end <= start:
            raise CandidateError("embedded buffer contains gaps or overlapping views")
        cursor = end
    if cursor != declared_binary:
        raise CandidateError("embedded buffer contains undeclared payload bytes")

    if not nodes or len(nodes) > HARD_MAX_NODES:
        raise CandidateError("GLB node inventory is empty or too large")
    children_by_node: list[list[int]] = []
    mesh_references = [0] * len(meshes)
    parent_counts = [0] * len(nodes)
    for node_index, raw_node in enumerate(nodes):
        node = _keys(
            raw_node,
            {"name", "mesh", "children", "matrix", "translation", "rotation", "scale"},
            set(),
            f"node {node_index}",
        )
        if "name" in node and (not isinstance(node["name"], str) or len(node["name"]) > 128):
            raise CandidateError("node name is malformed")
        _identity_transform(node, f"node {node_index}")
        if "mesh" in node:
            mesh_index = node["mesh"]
            _item(meshes, mesh_index, "node mesh")
            mesh_references[mesh_index] += 1
        children = node.get("children", [])
        if not isinstance(children, list):
            raise CandidateError("node children must be an array")
        seen_children: set[int] = set()
        for child in children:
            _item(nodes, child, "child node")
            if child in seen_children:
                raise CandidateError("node contains duplicate children")
            seen_children.add(child)
            parent_counts[child] += 1
        children_by_node.append(children)
    if any(count != 1 for count in mesh_references):
        raise CandidateError("every mesh must be referenced by exactly one node")
    if len(scenes) != 1 or document.get("scene") != 0:
        raise CandidateError("GLB must contain exactly one default scene")
    scene_index = document.get("scene")
    scene = _item(scenes, scene_index, "default scene")
    scene = _keys(scene, {"name", "nodes"}, {"nodes"}, "default scene")
    roots = scene.get("nodes")
    if not isinstance(roots, list) or not roots:
        raise CandidateError("GLB default scene is malformed")
    seen_roots: set[int] = set()
    for node_index in roots:
        _item(nodes, node_index, "scene node")
        if node_index in seen_roots:
            raise CandidateError("default scene contains duplicate root nodes")
        seen_roots.add(node_index)
        parent_counts[node_index] += 1
    if any(count != 1 for count in parent_counts):
        raise CandidateError("scene graph must reference every node exactly once")
    visited: set[int] = set()
    active: set[int] = set()

    def visit(node_index: int) -> None:
        if node_index in active:
            raise CandidateError("scene graph contains a cycle")
        if node_index in visited:
            raise CandidateError("scene graph instancing is not accepted")
        active.add(node_index)
        for child in children_by_node[node_index]:
            visit(child)
        active.remove(node_index)
        visited.add(node_index)

    for root in roots:
        visit(root)
    if len(visited) != len(nodes):
        raise CandidateError("scene graph contains unreachable nodes")
    if triangle_count > budgets["maxTriangles"]:
        raise CandidateError("GLB exceeds the triangle budget")
    return {
        "format": "glb-2.0",
        "glbBytes": size,
        "triangles": triangle_count,
        "materials": material_count,
        "textures": 0,
        "textureBytes": 0,
        "meshes": len(meshes),
        "primitives": primitive_count,
        "vertices": vertex_count,
        "bounds": {"minimum": bounds_min, "maximum": bounds_max},
    }


def canonical_json(document: dict[str, Any]) -> bytes:
    return (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")


def validate_source_uri(value: str) -> str:
    if not isinstance(value, str):
        raise CandidateError("source-uri must be a string")
    if value == "local-private":
        return value
    if any(ord(character) < 0x20 or character.isspace() for character in value):
        raise CandidateError("source-uri contains whitespace or control characters")
    try:
        parsed = urlparse(value)
        port = parsed.port
    except ValueError as error:
        raise CandidateError(f"source-uri is malformed: {error}") from error
    if (
        parsed.scheme != "https"
        or not parsed.netloc
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
        or parsed.params
        or (port is not None and not 1 <= port <= 65535)
    ):
        raise CandidateError(
            "source-uri must be 'local-private' or an HTTPS URI without "
            "userinfo, query parameters or fragments"
        )
    return value


def build_descriptor(
    args: argparse.Namespace,
    reference: FileSnapshot,
    lod0: FileSnapshot,
    lod1: FileSnapshot,
    technical_lod0: dict[str, Any],
    technical_lod1: dict[str, Any],
    budgets: dict[str, int],
    collision: FileSnapshot | None = None,
    technical_collision: dict[str, Any] | None = None,
) -> dict[str, Any]:
    technical: dict[str, Any] = {
        "lod0": technical_lod0,
        "lod1": technical_lod1,
        "lod1TriangleRatio": technical_lod1["triangles"] / technical_lod0["triangles"],
    }
    files = [
        {
            "role": "review-model-lod0",
            "name": "candidate-lod0.glb",
            "sha256": lod0.sha256,
            "bytes": lod0.size,
        },
        {
            "role": "review-model-lod1",
            "name": "candidate-lod1.glb",
            "sha256": lod1.sha256,
            "bytes": lod1.size,
        },
    ]
    if collision is not None:
        if technical_collision is None:
            raise CandidateError("collision proxy technical metadata is missing")
        technical["collisionProxy"] = technical_collision
        files.append({
            "role": "collision-proxy",
            "name": "candidate-collision.glb",
            "sha256": collision.sha256,
            "bytes": collision.size,
        })
    descriptor = {
        "schemaVersion": VERSION,
        "kind": "workout-game-triposr-candidate",
        "candidateId": args.candidate_id,
        "status": "review-required",
        "sourceReference": {
            "sha256": reference.sha256,
            "bytes": reference.size,
            "mimeType": reference_mime(reference.data),
            "rightsBasis": args.reference_rights,
            "sourceUri": validate_source_uri(args.source_uri),
        },
        "generator": {
            "name": "TripoSR",
            "repository": "https://github.com/VAST-AI-Research/TripoSR",
            "revision": args.triposr_revision,
            "codeLicense": "MIT",
            "modelId": args.model_id,
            "modelWeightsSha256": args.model_weights_sha256,
            "modelLicense": args.model_license,
            "settings": {
                "seed": args.seed,
                "mcResolution": args.mc_resolution,
                "chunkSize": args.chunk_size,
                "foregroundRemoval": args.foreground_removal,
            },
        },
        "coordinateSystem": {
            "unitMeters": args.unit_meters,
            "upAxis": args.up_axis,
            "forwardAxis": args.forward_axis,
        },
        "technical": technical,
        "files": files,
        "validation": {
            "validator": f"triposr_candidate.py/{VERSION}",
            "budgets": budgets,
            "checks": CHECKS,
        },
        "installation": {
            "automatic": False,
            "target": "RB-01",
            "decision": "prohibited-pending-human-review",
        },
    }
    if collision is not None:
        descriptor["collisionProxyProvenance"] = {
            "generator": args.collision_proxy_generator,
            "derivedFromRole": "review-model-lod0",
            "derivedFromSha256": lod0.sha256,
            "sha256": collision.sha256,
            "status": "review-required",
        }
    return descriptor


def _exact_object(value: Any, fields: set[str], description: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise CandidateError(f"{description} has missing or unknown fields")
    return value


def _positive_int(value: Any, description: str, allow_zero: bool = False) -> int:
    minimum = 0 if allow_zero else 1
    if (
        not isinstance(value, int)
        or isinstance(value, bool)
        or value < minimum
    ):
        raise CandidateError(f"{description} must be an integer >= {minimum}")
    return value


def _validate_technical(value: Any, description: str) -> dict[str, Any]:
    technical = _exact_object(value, {
        "format", "glbBytes", "triangles", "materials", "textures",
        "textureBytes", "meshes", "primitives", "vertices", "bounds",
    }, description)
    if technical["format"] != "glb-2.0":
        raise CandidateError(f"{description} format must be glb-2.0")
    for field in ("glbBytes", "triangles", "meshes", "primitives", "vertices"):
        _positive_int(technical[field], f"{description}.{field}")
    for field in ("materials", "textures", "textureBytes"):
        _positive_int(technical[field], f"{description}.{field}", allow_zero=True)
    bounds = _exact_object(
        technical["bounds"], {"minimum", "maximum"}, f"{description}.bounds"
    )
    for field in ("minimum", "maximum"):
        vector = bounds[field]
        if (
            not isinstance(vector, list)
            or len(vector) != 3
            or any(
                not isinstance(component, (int, float))
                or isinstance(component, bool)
                or not math.isfinite(_finite_number(component, f"{description}.bounds.{field}"))
                for component in vector
            )
        ):
            raise CandidateError(f"{description}.bounds.{field} must be a finite vector3")
    if any(
        _finite_number(bounds["minimum"][axis], f"{description}.bounds.minimum")
        > _finite_number(bounds["maximum"][axis], f"{description}.bounds.maximum")
        for axis in range(3)
    ):
        raise CandidateError(f"{description} bounds are inverted")
    return technical


def validate_descriptor(document: Any) -> None:
    files_value = document.get("files") if isinstance(document, dict) else None
    has_collision_hint = isinstance(files_value, list) and len(files_value) == 3
    expected_top = {
        "schemaVersion", "kind", "candidateId", "status", "sourceReference",
        "generator", "coordinateSystem", "technical", "files", "validation",
        "installation",
    }
    if has_collision_hint:
        expected_top.add("collisionProxyProvenance")
    document = _exact_object(document, expected_top, "candidate descriptor")
    if (
        document.get("schemaVersion") != VERSION
        or document.get("kind") != "workout-game-triposr-candidate"
    ):
        raise CandidateError("candidate descriptor version or kind is unsupported")
    if document.get("status") != "review-required":
        raise CandidateError("candidate status must remain review-required")
    candidate_id = document.get("candidateId")
    if not isinstance(candidate_id, str) or not CANDIDATE_ID_PATTERN.fullmatch(candidate_id):
        raise CandidateError("candidate ID is invalid")
    generator = _exact_object(document.get("generator"), {
        "name", "repository", "revision", "codeLicense", "modelId",
        "modelWeightsSha256", "modelLicense", "settings",
    }, "candidate generator")
    if (
        generator.get("name") != "TripoSR"
        or generator.get("repository") != "https://github.com/VAST-AI-Research/TripoSR"
        or generator.get("codeLicense") != "MIT"
        or not isinstance(generator.get("revision"), str)
        or not REVISION_PATTERN.fullmatch(generator["revision"])
        or not isinstance(generator.get("modelWeightsSha256"), str)
        or not SHA256_PATTERN.fullmatch(generator["modelWeightsSha256"])
    ):
        raise CandidateError("candidate generator provenance is invalid")
    if (
        not isinstance(generator["modelId"], str)
        or not generator["modelId"]
        or not isinstance(generator["modelLicense"], str)
        or not generator["modelLicense"]
    ):
        raise CandidateError("candidate model identity or license is invalid")
    settings = _exact_object(generator["settings"], {
        "seed", "mcResolution", "chunkSize", "foregroundRemoval",
    }, "candidate generator settings")
    _positive_int(settings["seed"], "generator.settings.seed", allow_zero=True)
    if _positive_int(settings["mcResolution"], "generator.settings.mcResolution") < 16:
        raise CandidateError("generator.settings.mcResolution must be >= 16")
    _positive_int(settings["chunkSize"], "generator.settings.chunkSize")
    if not isinstance(settings["foregroundRemoval"], bool):
        raise CandidateError("generator.settings.foregroundRemoval must be boolean")

    source = _exact_object(document.get("sourceReference"), {
        "sha256", "bytes", "mimeType", "rightsBasis", "sourceUri",
    }, "candidate source reference")
    if (
        not isinstance(source.get("sha256"), str)
        or not SHA256_PATTERN.fullmatch(source["sha256"])
        or source.get("rightsBasis") not in RIGHTS_BASES
        or source.get("mimeType") not in {"image/png", "image/jpeg", "image/webp"}
    ):
        raise CandidateError("candidate source provenance is invalid")
    if _positive_int(source["bytes"], "sourceReference.bytes") > HARD_MAX_REFERENCE_BYTES:
        raise CandidateError("source reference exceeds its hard byte limit")
    validate_source_uri(source["sourceUri"])
    coordinates = _exact_object(
        document.get("coordinateSystem"),
        {"unitMeters", "upAxis", "forwardAxis"},
        "candidate coordinate system",
    )
    unit_meters = coordinates.get("unitMeters")
    if not isinstance(unit_meters, (int, float)) or isinstance(unit_meters, bool):
        raise CandidateError("coordinateSystem.unitMeters must be numeric")
    validate_axes(
        _finite_number(unit_meters, "coordinateSystem.unitMeters"),
        coordinates.get("upAxis", ""),
        coordinates.get("forwardAxis", ""),
    )
    files = document.get("files")
    has_collision = isinstance(files, list) and len(files) == 3
    expected_files = [
        ("review-model-lod0", "candidate-lod0.glb"),
        ("review-model-lod1", "candidate-lod1.glb"),
    ]
    if has_collision:
        expected_files.append(("collision-proxy", "candidate-collision.glb"))
    if (
        not isinstance(files, list)
        or len(files) not in {2, 3}
        or any(not isinstance(entry, dict) for entry in files)
        or any(set(entry) != {"role", "name", "sha256", "bytes"} for entry in files)
        or [(entry.get("role"), entry.get("name")) for entry in files] != expected_files
        or any(
            not isinstance(entry.get("sha256"), str)
            or not SHA256_PATTERN.fullmatch(entry["sha256"])
            for entry in files
        )
    ):
        raise CandidateError("candidate file inventory is invalid")
    for entry in files:
        _positive_int(entry["bytes"], f"{entry['name']}.bytes")

    expected_technical = {"lod0", "lod1", "lod1TriangleRatio"}
    if has_collision:
        expected_technical.add("collisionProxy")
    technical = _exact_object(
        document.get("technical"), expected_technical,
        "candidate technical metadata",
    )
    lod0 = _validate_technical(technical["lod0"], "technical.lod0")
    lod1 = _validate_technical(technical["lod1"], "technical.lod1")
    ratio = technical["lod1TriangleRatio"]
    ratio_number = (
        _finite_number(ratio, "technical.lod1TriangleRatio")
        if isinstance(ratio, (int, float)) and not isinstance(ratio, bool)
        else math.nan
    )
    if (
        not isinstance(ratio, (int, float))
        or isinstance(ratio, bool)
        or not math.isfinite(ratio_number)
        or not 0 < ratio_number < 1
        or lod1["triangles"] >= lod0["triangles"]
        or ratio_number != lod1["triangles"] / lod0["triangles"]
    ):
        raise CandidateError("candidate LOD1 must be lighter than LOD0")
    if has_collision:
        collision = _validate_technical(
            technical["collisionProxy"], "technical.collisionProxy"
        )
        if (
            collision["triangles"] > HARD_MAX_COLLISION_TRIANGLES
            or collision["materials"] != 0
            or collision["textures"] != 0
            or collision["textureBytes"] != 0
            or collision["glbBytes"] > HARD_MAX_COLLISION_GLB_BYTES
        ):
            raise CandidateError("candidate collision proxy exceeds its safe contract")
        provenance = _exact_object(
            document.get("collisionProxyProvenance"),
            {
                "generator", "derivedFromRole", "derivedFromSha256", "sha256",
                "status",
            },
            "collision proxy provenance",
        )
        if (
            not isinstance(provenance["generator"], str)
            or not GENERATOR_PATTERN.fullmatch(provenance["generator"])
            or provenance["derivedFromRole"] != "review-model-lod0"
            or provenance["derivedFromSha256"] != files[0]["sha256"]
            or provenance["sha256"] != files[2]["sha256"]
            or provenance["status"] != "review-required"
        ):
            raise CandidateError("collision proxy provenance is invalid")

    validation = _exact_object(
        document.get("validation"), {"validator", "budgets", "checks"},
        "candidate validation record",
    )
    if validation["validator"] != f"triposr_candidate.py/{VERSION}":
        raise CandidateError("candidate validator version is unsupported")
    budgets = _exact_object(validation["budgets"], {
        "maxGlbBytes", "maxTrianglesLod0", "maxTrianglesLod1",
        "maxMaterials", "maxTextures", "maxTextureBytes",
        "maxCollisionGlbBytes", "maxCollisionTriangles",
    }, "candidate budgets")
    for field in (
        "maxGlbBytes", "maxTrianglesLod0", "maxTrianglesLod1",
        "maxCollisionGlbBytes", "maxCollisionTriangles",
    ):
        _positive_int(budgets[field], f"validation.budgets.{field}")
    for field in ("maxMaterials", "maxTextures", "maxTextureBytes"):
        _positive_int(
            budgets[field], f"validation.budgets.{field}", allow_zero=True
        )
    validate_budgets(budgets)
    if (
        lod0["glbBytes"] > budgets["maxGlbBytes"]
        or lod1["glbBytes"] > budgets["maxGlbBytes"]
        or lod0["triangles"] > budgets["maxTrianglesLod0"]
        or lod1["triangles"] > budgets["maxTrianglesLod1"]
        or lod0["materials"] > budgets["maxMaterials"]
        or lod1["materials"] > budgets["maxMaterials"]
        or lod0["textures"] != 0
        or lod1["textures"] != 0
        or lod0["textureBytes"] != 0
        or lod1["textureBytes"] != 0
    ):
        raise CandidateError("candidate technical metadata exceeds its declared budgets")
    if files[0]["bytes"] != lod0["glbBytes"] or files[1]["bytes"] != lod1["glbBytes"]:
        raise CandidateError("candidate file sizes do not match technical metadata")
    if has_collision and files[2]["bytes"] != technical["collisionProxy"]["glbBytes"]:
        raise CandidateError("collision file size does not match technical metadata")
    if validation["checks"] != CHECKS:
        raise CandidateError("candidate validation checks are incomplete or reordered")
    installation = document.get("installation", {})
    if installation != {
        "automatic": False,
        "target": "RB-01",
        "decision": "prohibited-pending-human-review",
    }:
        raise CandidateError("candidate must not install or replace RB-01 automatically")


def budgets_from_args(args: argparse.Namespace) -> dict[str, int]:
    return {
        "maxGlbBytes": args.max_glb_bytes,
        "maxTrianglesLod0": args.max_triangles_lod0,
        "maxTrianglesLod1": args.max_triangles_lod1,
        "maxMaterials": args.max_materials,
        "maxTextures": args.max_textures,
        "maxTextureBytes": args.max_texture_bytes,
        "maxCollisionGlbBytes": HARD_MAX_COLLISION_GLB_BYTES,
        "maxCollisionTriangles": HARD_MAX_COLLISION_TRIANGLES,
    }


def validate_budgets(budgets: dict[str, int]) -> None:
    hard_limits = {
        "maxGlbBytes": HARD_MAX_GLB_BYTES,
        "maxTrianglesLod0": HARD_MAX_TRIANGLES_LOD0,
        "maxTrianglesLod1": HARD_MAX_TRIANGLES_LOD1,
        "maxMaterials": HARD_MAX_MATERIALS,
        "maxTextures": HARD_MAX_TEXTURES,
        "maxTextureBytes": HARD_MAX_TEXTURE_BYTES,
        "maxCollisionGlbBytes": HARD_MAX_COLLISION_GLB_BYTES,
        "maxCollisionTriangles": HARD_MAX_COLLISION_TRIANGLES,
    }
    for field, hard_limit in hard_limits.items():
        value = budgets.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise CandidateError(f"{field} must be a non-negative integer")
        if value > hard_limit:
            raise CandidateError(f"{field} exceeds its hard safety limit {hard_limit}")
    if budgets["maxGlbBytes"] < 1:
        raise CandidateError("maxGlbBytes must allow at least one byte")
    if budgets["maxTrianglesLod0"] < 1 or budgets["maxTrianglesLod1"] < 1:
        raise CandidateError("triangle budgets must allow at least one triangle")
    if budgets["maxCollisionGlbBytes"] != HARD_MAX_COLLISION_GLB_BYTES:
        raise CandidateError("collision GLB hard limit may not be changed")
    if budgets["maxCollisionTriangles"] != HARD_MAX_COLLISION_TRIANGLES:
        raise CandidateError("collision triangle hard limit may not be changed")


def _snapshot_at(
    directory_fd: int, name: str, description: str, maximum_bytes: int
) -> FileSnapshot:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(name, flags, dir_fd=directory_fd)
    except OSError as error:
        raise CandidateError(f"cannot open {description}: {error}") from error
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise CandidateError(f"{description} must be a regular non-symlink file")
        if metadata.st_size <= 0 or metadata.st_size > maximum_bytes:
            raise CandidateError(f"{description} exceeds its hard byte limit")
        chunks: list[bytes] = []
        remaining = maximum_bytes + 1
        while remaining:
            block = os.read(descriptor, min(MIB, remaining))
            if not block:
                break
            chunks.append(block)
            remaining -= len(block)
        data = b"".join(chunks)
        final = os.fstat(descriptor)
        if (
            len(data) != metadata.st_size
            or len(data) > maximum_bytes
            or metadata.st_size != final.st_size
            or metadata.st_mtime_ns != final.st_mtime_ns
        ):
            raise CandidateError(f"{description} changed while it was being read")
        return FileSnapshot(Path(name), data, hashlib.sha256(data).hexdigest())
    finally:
        os.close(descriptor)


def _write_at(directory_fd: int, name: str, data: bytes) -> None:
    flags = (
        os.O_WRONLY | os.O_CREAT | os.O_EXCL
        | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(name, flags, 0o600, dir_fd=directory_fd)
    try:
        view = memoryview(data)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise CandidateError(f"short write while creating {name}")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _open_external_directory(path: Path, description: str) -> tuple[int, Path]:
    required_dir_fd_operations = {os.open, os.mkdir, os.rename, os.stat, os.unlink, os.rmdir}
    if not required_dir_fd_operations.issubset(os.supports_dir_fd):
        raise CandidateError(
            "secure candidate directory operations require POSIX dir_fd support"
        )
    resolved = _external_resolved(path, description)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_DIRECTORY", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(resolved, flags)
    except OSError as error:
        raise CandidateError(f"cannot open {description}: {error}") from error
    if not stat.S_ISDIR(os.fstat(descriptor).st_mode):
        os.close(descriptor)
        raise CandidateError(f"{description} must be a directory")
    return descriptor, resolved


def _make_temporary_directory_at(parent_fd: int, prefix: str) -> str:
    for _ in range(100):
        name = f"{prefix}{secrets.token_hex(8)}"
        try:
            os.mkdir(name, 0o700, dir_fd=parent_fd)
            return name
        except FileExistsError:
            continue
    raise CandidateError("could not allocate a unique staging directory")


def _verify_candidate_fd(directory_fd: int) -> dict[str, Any]:
    actual_names = set(os.listdir(directory_fd))
    allowed_names = {
        "candidate.json", "candidate-lod0.glb", "candidate-lod1.glb",
        "candidate-collision.glb",
    }
    if not actual_names.issubset(allowed_names):
        raise CandidateError("candidate directory contains unexpected files")
    descriptor = _snapshot_at(
        directory_fd, "candidate.json", "candidate descriptor", MIB
    )
    document = load_json(descriptor.data, "candidate descriptor")
    validate_descriptor(document)
    expected_names = {"candidate.json"} | {entry["name"] for entry in document["files"]}
    if actual_names != expected_names:
        raise CandidateError("candidate directory inventory does not match its descriptor")

    budgets = document["validation"]["budgets"]
    technical: dict[str, dict[str, Any]] = {}
    levels: list[tuple[str, dict[str, Any], int, int, bool]] = [
        ("lod0", document["files"][0], budgets["maxTrianglesLod0"],
         budgets["maxGlbBytes"], False),
        ("lod1", document["files"][1], budgets["maxTrianglesLod1"],
         budgets["maxGlbBytes"], False),
    ]
    if len(document["files"]) == 3:
        levels.append((
            "collisionProxy", document["files"][2], budgets["maxCollisionTriangles"],
            budgets["maxCollisionGlbBytes"], True,
        ))
    for level, file_entry, triangle_budget, byte_budget, collision in levels:
        snapshot = _snapshot_at(
            directory_fd, file_entry["name"], f"candidate {level} GLB", byte_budget
        )
        require_hash(snapshot.sha256, file_entry["sha256"], f"candidate {level} GLB")
        if snapshot.size != file_entry["bytes"]:
            raise CandidateError(f"candidate {level} GLB byte count does not match descriptor")
        level_budgets = {
            "maxGlbBytes": byte_budget,
            "maxTriangles": triangle_budget,
            "maxMaterials": 0 if collision else budgets["maxMaterials"],
            "maxTextures": 0,
            "maxTextureBytes": 0,
        }
        technical[level] = inspect_glb(snapshot.data, level_budgets, collision=collision)
        if technical[level] != document["technical"][level]:
            raise CandidateError(f"candidate {level} technical metadata does not match its GLB")
    if technical["lod1"]["triangles"] >= technical["lod0"]["triangles"]:
        raise CandidateError("LOD1 must have fewer triangles than LOD0")
    ratio = technical["lod1"]["triangles"] / technical["lod0"]["triangles"]
    if ratio != document["technical"].get("lod1TriangleRatio"):
        raise CandidateError("candidate LOD triangle ratio does not match its GLBs")
    return document


def import_candidate(args: argparse.Namespace) -> None:
    if (
        not isinstance(args.candidate_id, str)
        or not CANDIDATE_ID_PATTERN.fullmatch(args.candidate_id)
    ):
        raise CandidateError("candidate ID is invalid")
    if (
        not isinstance(args.triposr_revision, str)
        or not REVISION_PATTERN.fullmatch(args.triposr_revision)
    ):
        raise CandidateError("TripoSR revision must be a full 40-character commit")
    if (
        not isinstance(args.model_weights_sha256, str)
        or not SHA256_PATTERN.fullmatch(args.model_weights_sha256)
    ):
        raise CandidateError("model weights SHA-256 is malformed")
    if args.reference_rights not in RIGHTS_BASES:
        raise CandidateError("reference rights basis is invalid")
    validate_axes(
        _finite_number(args.unit_meters, "unit-meters"),
        args.up_axis,
        args.forward_axis,
    )
    budgets = budgets_from_args(args)
    validate_budgets(budgets)
    collision_argument = getattr(args, "collision_proxy_glb", None)
    collision_hash_argument = getattr(args, "collision_proxy_glb_sha256", None)
    collision_generator = getattr(args, "collision_proxy_generator", None)
    collision_arguments = (
        collision_argument, collision_hash_argument, collision_generator,
    )
    has_collision_path = collision_argument is not None
    if any(value is None for value in collision_arguments) != all(
        value is None for value in collision_arguments
    ):
        raise CandidateError(
            "collision proxy GLB, SHA-256 and generator must all be provided or all omitted"
        )
    if has_collision_path and (
        not isinstance(collision_generator, str)
        or not GENERATOR_PATTERN.fullmatch(collision_generator)
    ):
        raise CandidateError("collision proxy generator is malformed")
    reference = read_external_snapshot(
        args.reference, "reference input", HARD_MAX_REFERENCE_BYTES
    )
    model_weights = digest_external_file(
        args.model_weights, "TripoSR model weights", HARD_MAX_MODEL_WEIGHTS_BYTES
    )
    lod0 = read_external_snapshot(args.lod0_glb, "TripoSR LOD0 GLB", budgets["maxGlbBytes"])
    lod1 = read_external_snapshot(args.lod1_glb, "TripoSR LOD1 GLB", budgets["maxGlbBytes"])
    collision = None
    if has_collision_path:
        collision = read_external_snapshot(
            collision_argument,
            "collision proxy GLB",
            budgets["maxCollisionGlbBytes"],
        )
    require_hash(reference.sha256, args.reference_sha256, "reference input")
    require_hash(model_weights.sha256, args.model_weights_sha256, "TripoSR model weights")
    require_hash(lod0.sha256, args.lod0_glb_sha256, "TripoSR LOD0 GLB")
    require_hash(lod1.sha256, args.lod1_glb_sha256, "TripoSR LOD1 GLB")
    if collision is not None:
        require_hash(
            collision.sha256, collision_hash_argument, "collision proxy GLB"
        )
    reference_mime(reference.data)
    common_budgets = {
        key: value for key, value in budgets.items()
        if key not in {"maxTrianglesLod0", "maxTrianglesLod1"}
    }
    technical_lod0 = inspect_glb(
        lod0.data, {**common_budgets, "maxTriangles": budgets["maxTrianglesLod0"]}
    )
    technical_lod1 = inspect_glb(
        lod1.data, {**common_budgets, "maxTriangles": budgets["maxTrianglesLod1"]}
    )
    if technical_lod1["triangles"] >= technical_lod0["triangles"]:
        raise CandidateError("LOD1 must have fewer triangles than LOD0")
    technical_collision = None
    if collision is not None:
        technical_collision = inspect_glb(
            collision.data,
            {
                "maxGlbBytes": budgets["maxCollisionGlbBytes"],
                "maxTriangles": budgets["maxCollisionTriangles"],
                "maxMaterials": 0,
                "maxTextures": 0,
                "maxTextureBytes": 0,
            },
            collision=True,
        )
    destination = external_destination(args.candidate_dir)
    if destination.exists():
        raise CandidateError("candidate directory must not already exist")
    destination.parent.mkdir(parents=True, exist_ok=True)

    descriptor = build_descriptor(
        args, reference, lod0, lod1, technical_lod0, technical_lod1, budgets,
        collision, technical_collision,
    )
    descriptor["generator"]["modelWeightsSha256"] = model_weights.sha256
    validate_descriptor(descriptor)
    parent_fd, _ = _open_external_directory(destination.parent, "candidate parent directory")
    temporary_name = ""
    try:
        try:
            os.stat(destination.name, dir_fd=parent_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            raise CandidateError("candidate directory must not already exist")
        temporary_name = _make_temporary_directory_at(
            parent_fd, f".{destination.name}."
        )
        temporary_fd = os.open(
            temporary_name,
            os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
            dir_fd=parent_fd,
        )
        try:
            _write_at(temporary_fd, "candidate-lod0.glb", lod0.data)
            _write_at(temporary_fd, "candidate-lod1.glb", lod1.data)
            if collision is not None:
                _write_at(temporary_fd, "candidate-collision.glb", collision.data)
            _write_at(temporary_fd, "candidate.json", canonical_json(descriptor))
            _verify_candidate_fd(temporary_fd)
            os.fsync(temporary_fd)
        finally:
            os.close(temporary_fd)
        os.rename(
            temporary_name, destination.name,
            src_dir_fd=parent_fd, dst_dir_fd=parent_fd,
        )
        temporary_name = ""
        os.fsync(parent_fd)
    finally:
        if temporary_name:
            cleanup_fd = os.open(
                temporary_name,
                os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_NOFOLLOW", 0),
                dir_fd=parent_fd,
            )
            try:
                for name in os.listdir(cleanup_fd):
                    os.unlink(name, dir_fd=cleanup_fd)
            finally:
                os.close(cleanup_fd)
            os.rmdir(temporary_name, dir_fd=parent_fd)
        os.close(parent_fd)
    print(destination / "candidate.json")


def verify_candidate_directory(candidate_dir: Path) -> dict[str, Any]:
    descriptor, _ = _open_external_directory(candidate_dir, "candidate directory")
    try:
        return _verify_candidate_fd(descriptor)
    finally:
        os.close(descriptor)


def add_budget_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--max-glb-bytes", type=int, default=HARD_MAX_GLB_BYTES)
    parser.add_argument("--max-triangles-lod0", type=int, default=HARD_MAX_TRIANGLES_LOD0)
    parser.add_argument("--max-triangles-lod1", type=int, default=HARD_MAX_TRIANGLES_LOD1)
    parser.add_argument("--max-materials", type=int, default=HARD_MAX_MATERIALS)
    parser.add_argument("--max-textures", type=int, default=HARD_MAX_TEXTURES)
    parser.add_argument("--max-texture-bytes", type=int, default=HARD_MAX_TEXTURE_BYTES)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    create = commands.add_parser("import", help="validate and quarantine a candidate")
    create.add_argument("--candidate-id", required=True)
    create.add_argument("--reference", required=True, type=Path)
    create.add_argument("--reference-sha256", required=True)
    create.add_argument("--reference-rights", required=True, choices=sorted(RIGHTS_BASES))
    create.add_argument("--source-uri", required=True)
    create.add_argument("--lod0-glb", required=True, type=Path)
    create.add_argument("--lod0-glb-sha256", required=True)
    create.add_argument("--lod1-glb", required=True, type=Path)
    create.add_argument("--lod1-glb-sha256", required=True)
    create.add_argument("--collision-proxy-glb", type=Path)
    create.add_argument("--collision-proxy-glb-sha256")
    create.add_argument("--collision-proxy-generator")
    create.add_argument("--candidate-dir", required=True, type=Path)
    create.add_argument("--triposr-revision", required=True)
    create.add_argument("--model-id", required=True)
    create.add_argument("--model-weights", required=True, type=Path)
    create.add_argument("--model-weights-sha256", required=True)
    create.add_argument("--model-license", required=True)
    create.add_argument("--seed", required=True, type=int)
    create.add_argument("--mc-resolution", required=True, type=int)
    create.add_argument("--chunk-size", required=True, type=int)
    create.add_argument(
        "--foreground-removal",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    create.add_argument("--unit-meters", required=True, type=float)
    create.add_argument("--up-axis", required=True, choices=sorted(AXES))
    create.add_argument("--forward-axis", required=True, choices=sorted(AXES))
    add_budget_arguments(create)

    verify = commands.add_parser("verify", help="revalidate a quarantined candidate")
    verify.add_argument("candidate_dir", type=Path)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "import":
            import_candidate(args)
        else:
            document = verify_candidate_directory(args.candidate_dir)
            print(
                f"{document['candidateId']}: valid review candidate "
                f"(LOD0 {document['technical']['lod0']['triangles']}, "
                f"LOD1 {document['technical']['lod1']['triangles']} triangles)"
            )
        return 0
    except (CandidateError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
