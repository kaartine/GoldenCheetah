#!/usr/bin/env python3
"""Validate committed Workout Game asset manifests and GLB contracts."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct
import sys
from typing import Any
from urllib.parse import urlparse


GLB_HEADER = struct.Struct("<4sII")
GLB_CHUNK_HEADER = struct.Struct("<II")
GLB_MAGIC = b"glTF"
GLB_JSON_CHUNK = 0x4E4F534A
GLB_BINARY_CHUNK = 0x004E4942
SHA256 = re.compile(r"^[0-9a-f]{64}$")
FLOAT_TOLERANCE = 1.0e-5


class AssetValidationError(ValueError):
    pass


def _duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise AssetValidationError(f"duplicate JSON key: {key}")
        result[key] = value
    return result


def _invalid_constant(value: str) -> None:
    raise AssetValidationError(f"non-finite JSON number: {value}")


def load_json_bytes(data: bytes, description: str) -> Any:
    try:
        return json.loads(
            data.decode("utf-8"),
            object_pairs_hook=_duplicate_keys,
            parse_constant=_invalid_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise AssetValidationError(
            f"invalid JSON in {description}: {error}"
        ) from error


def load_json_file(path: Path, maximum_bytes: int = 1024 * 1024) -> Any:
    if path.is_symlink() or not path.is_file():
        raise AssetValidationError(f"JSON file is unavailable: {path}")
    if path.stat().st_size > maximum_bytes:
        raise AssetValidationError(f"JSON file is too large: {path}")
    return load_json_bytes(path.read_bytes(), str(path))


def _matches_type(value: Any, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return (
            isinstance(value, (int, float))
            and not isinstance(value, bool)
            and math.isfinite(float(value))
        )
    if expected == "null":
        return value is None
    raise AssetValidationError(f"unsupported JSON Schema type: {expected}")


def _validate_format(value: str, format_name: str, location: str) -> None:
    if format_name == "uri":
        parsed = urlparse(value)
        if not parsed.scheme or parsed.scheme not in {"https", "http"}:
            raise AssetValidationError(f"{location} is not an HTTP(S) URI")
    elif format_name == "date":
        try:
            if dt.date.fromisoformat(value).isoformat() != value:
                raise ValueError
        except ValueError as error:
            raise AssetValidationError(
                f"{location} is not an ISO date"
            ) from error
    else:
        raise AssetValidationError(
            f"unsupported JSON Schema format at {location}: {format_name}"
        )


def validate_against_schema(
    value: Any, schema: dict[str, Any] | bool, location: str = "$"
) -> None:
    """Evaluate the deliberately small JSON Schema subset used by this repo."""
    if schema is False:
        raise AssetValidationError(f"{location} is not allowed")
    if schema is True:
        return

    if "const" in schema and value != schema["const"]:
        raise AssetValidationError(f"{location} does not match const")
    if "enum" in schema and value not in schema["enum"]:
        raise AssetValidationError(f"{location} is not an allowed value")

    expected_type = schema.get("type")
    if expected_type is not None and not _matches_type(value, expected_type):
        raise AssetValidationError(
            f"{location} must have JSON Schema type {expected_type}"
        )

    if isinstance(value, dict):
        required = schema.get("required", [])
        for key in required:
            if key not in value:
                raise AssetValidationError(f"{location}.{key} is required")
        properties = schema.get("properties", {})
        additional = schema.get("additionalProperties", True)
        for key, child in value.items():
            child_location = f"{location}.{key}"
            if key in properties:
                validate_against_schema(child, properties[key], child_location)
            elif additional is False:
                raise AssetValidationError(
                    f"{child_location} is an unknown property"
                )
            elif isinstance(additional, dict):
                validate_against_schema(child, additional, child_location)

    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            raise AssetValidationError(f"{location} has too few items")
        if schema.get("uniqueItems"):
            encoded = [
                json.dumps(item, sort_keys=True, separators=(",", ":"))
                for item in value
            ]
            if len(encoded) != len(set(encoded)):
                raise AssetValidationError(f"{location} has duplicate items")
        prefixes = schema.get("prefixItems", [])
        for index, child_schema in enumerate(prefixes):
            if index < len(value):
                validate_against_schema(
                    value[index], child_schema, f"{location}[{index}]"
                )
        item_schema = schema.get("items")
        if item_schema is not None:
            for index in range(len(prefixes), len(value)):
                validate_against_schema(
                    value[index], item_schema, f"{location}[{index}]"
                )

    if isinstance(value, str):
        if len(value) < schema.get("minLength", 0):
            raise AssetValidationError(f"{location} is too short")
        pattern = schema.get("pattern")
        if pattern is not None and re.search(pattern, value) is None:
            raise AssetValidationError(f"{location} does not match its pattern")
        if "format" in schema:
            _validate_format(value, schema["format"], location)

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if not math.isfinite(float(value)):
            raise AssetValidationError(f"{location} is not finite")
        if "minimum" in schema and value < schema["minimum"]:
            raise AssetValidationError(f"{location} is below its minimum")
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            raise AssetValidationError(
                f"{location} is not above its exclusive minimum"
            )


def resolve_repository_file(root: Path, relative_path: str) -> Path:
    logical = PurePosixPath(relative_path)
    if (
        logical.is_absolute()
        or not logical.parts
        or logical.as_posix() != relative_path
        or any(part in {"", ".", ".."} for part in logical.parts)
        or "\\" in relative_path
    ):
        raise AssetValidationError(f"unsafe repository path: {relative_path}")
    root = root.resolve(strict=True)
    candidate = root.joinpath(*logical.parts)
    current = root
    for part in logical.parts:
        current = current / part
        if current.is_symlink():
            raise AssetValidationError(f"symlink asset path: {relative_path}")
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise AssetValidationError(
            f"asset file is unavailable: {relative_path}"
        ) from error
    if root not in resolved.parents or not resolved.is_file():
        raise AssetValidationError(f"asset path escapes repository: {relative_path}")
    return resolved


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _vector_close(actual: list[Any], expected: list[Any]) -> bool:
    return (
        isinstance(actual, list)
        and isinstance(expected, list)
        and len(actual) == len(expected)
        and all(
            isinstance(left, (int, float))
            and not isinstance(left, bool)
            and math.isfinite(float(left))
            and math.isclose(
                float(left),
                float(right),
                rel_tol=0.0,
                abs_tol=FLOAT_TOLERANCE,
            )
            for left, right in zip(actual, expected)
        )
    )


def read_glb(path: Path) -> tuple[dict[str, Any], int]:
    data = path.read_bytes()
    if len(data) < GLB_HEADER.size:
        raise AssetValidationError(f"truncated GLB: {path}")
    magic, version, declared_length = GLB_HEADER.unpack_from(data)
    if magic != GLB_MAGIC or version != 2 or declared_length != len(data):
        raise AssetValidationError(f"invalid GLB header: {path}")

    offset = GLB_HEADER.size
    chunks: list[tuple[int, bytes]] = []
    while offset < len(data):
        if offset + GLB_CHUNK_HEADER.size > len(data):
            raise AssetValidationError(f"truncated GLB chunk header: {path}")
        length, chunk_type = GLB_CHUNK_HEADER.unpack_from(data, offset)
        offset += GLB_CHUNK_HEADER.size
        end = offset + length
        if length % 4 or end > len(data):
            raise AssetValidationError(f"invalid GLB chunk length: {path}")
        chunks.append((chunk_type, data[offset:end]))
        offset = end
    if not chunks or chunks[0][0] != GLB_JSON_CHUNK:
        raise AssetValidationError(f"GLB JSON chunk must be first: {path}")
    if len(chunks) > 2 or any(
        chunk_type not in {GLB_JSON_CHUNK, GLB_BINARY_CHUNK}
        for chunk_type, _ in chunks
    ):
        raise AssetValidationError(f"unexpected GLB chunks: {path}")
    if sum(chunk_type == GLB_JSON_CHUNK for chunk_type, _ in chunks) != 1:
        raise AssetValidationError(f"GLB must contain one JSON chunk: {path}")
    if sum(chunk_type == GLB_BINARY_CHUNK for chunk_type, _ in chunks) > 1:
        raise AssetValidationError(f"GLB has multiple binary chunks: {path}")

    json_data = chunks[0][1].rstrip(b" \t\r\n\x00")
    document = load_json_bytes(json_data, f"GLB JSON in {path}")
    if not isinstance(document, dict):
        raise AssetValidationError(f"GLB JSON root is not an object: {path}")
    return document, len(data)


def _node_world_translation(
    nodes: list[dict[str, Any]], parents: dict[int, int], index: int
) -> list[float]:
    result = [0.0, 0.0, 0.0]
    visited: set[int] = set()
    while True:
        if index in visited:
            raise AssetValidationError("GLB node graph has a cycle")
        visited.add(index)
        node = nodes[index]
        if "matrix" in node:
            identity = [
                1, 0, 0, 0,
                0, 1, 0, 0,
                0, 0, 1, 0,
                0, 0, 0, 1,
            ]
            if not _vector_close(node["matrix"], identity):
                raise AssetValidationError("contract node has an applied matrix")
        if not _vector_close(node.get("rotation", [0, 0, 0, 1]), [0, 0, 0, 1]):
            raise AssetValidationError("contract node has a rotation")
        if not _vector_close(node.get("scale", [1, 1, 1]), [1, 1, 1]):
            raise AssetValidationError("contract node has a scale")
        translation = node.get("translation", [0, 0, 0])
        if not isinstance(translation, list) or len(translation) != 3 \
                or not _vector_close(translation, translation):
            raise AssetValidationError("contract node translation is invalid")
        result = [result[axis] + float(translation[axis]) for axis in range(3)]
        if index not in parents:
            return result
        index = parents[index]


def _validate_glb_document(
    document: dict[str, Any], glb_bytes: int, manifest: dict[str, Any]
) -> None:
    technical = manifest["technical"]
    budgets = technical.get("budgets", {})
    if glb_bytes != technical.get("glbBytes"):
        raise AssetValidationError("GLB byte count does not match manifest")
    if glb_bytes > budgets.get("maxGlbBytes", glb_bytes):
        raise AssetValidationError("GLB exceeds byte budget")
    if document.get("asset", {}).get("version") != "2.0":
        raise AssetValidationError("asset is not glTF 2.0")

    allowed_extensions = set(technical.get("allowedExtensions", []))
    used_extensions = set(document.get("extensionsUsed", []))
    required_extensions = set(document.get("extensionsRequired", []))
    if not required_extensions.issubset(used_extensions):
        raise AssetValidationError("required GLB extension is not declared used")
    if not used_extensions.issubset(allowed_extensions):
        raise AssetValidationError("GLB uses a non-allowlisted extension")
    if document.get("cameras") or "KHR_lights_punctual" in document.get(
        "extensions", {}
    ):
        raise AssetValidationError("GLB contains a camera or light")

    for collection in ("buffers", "images"):
        for entry in document.get(collection, []):
            if "uri" in entry:
                raise AssetValidationError(
                    f"GLB {collection} must not contain external or data URIs"
                )

    nodes = document.get("nodes", [])
    if not isinstance(nodes, list) or not all(isinstance(node, dict) for node in nodes):
        raise AssetValidationError("GLB nodes are invalid")
    names = [node.get("name") for node in nodes]
    if any(not isinstance(name, str) or not name for name in names):
        raise AssetValidationError("every GLB node must have a name")
    if len(names) != len(set(names)):
        raise AssetValidationError("GLB node names are not unique")
    if set(names) != set(technical.get("nodes", [])):
        raise AssetValidationError("GLB nodes do not match manifest")
    if any("camera" in node for node in nodes):
        raise AssetValidationError("GLB node references a camera")

    parents: dict[int, int] = {}
    for parent_index, node in enumerate(nodes):
        for child in node.get("children", []):
            if not isinstance(child, int) or child < 0 or child >= len(nodes):
                raise AssetValidationError("GLB child node index is invalid")
            if child in parents:
                raise AssetValidationError("GLB node has multiple parents")
            parents[child] = parent_index

    root_nodes = [node for node in nodes if node["name"].startswith("ROOT_")]
    if len(root_nodes) != 1:
        raise AssetValidationError("GLB must have one named ROOT node")
    root_extras = root_nodes[0].get("extras", {})
    expected_root_extras = {
        "unit_meters": technical["unitMeters"],
        "up_axis": technical["upAxis"],
        "forward_axis": technical["forwardAxis"],
        "physics_authority": "external",
    }
    for key, expected in expected_root_extras.items():
        if root_extras.get(key) != expected:
            raise AssetValidationError(f"GLB root metadata mismatch: {key}")

    accessors = document.get("accessors", [])
    materials = document.get("materials", [])
    triangle_count = 0
    bounds_min = [math.inf, math.inf, math.inf]
    bounds_max = [-math.inf, -math.inf, -math.inf]
    for node_index, node in enumerate(nodes):
        if "mesh" not in node:
            continue
        if not _vector_close(
            _node_world_translation(nodes, parents, node_index), [0, 0, 0]
        ):
            raise AssetValidationError("render mesh transform is not applied")
        if node.get("extras", {}).get("physics_authority") != "external":
            raise AssetValidationError("render mesh claims physics authority")

    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            if primitive.get("mode", 4) != 4:
                raise AssetValidationError("only triangle primitives are allowed")
            accessor_index = primitive.get(
                "indices", primitive.get("attributes", {}).get("POSITION")
            )
            if not isinstance(accessor_index, int) or not (
                0 <= accessor_index < len(accessors)
            ):
                raise AssetValidationError("primitive accessor is invalid")
            count = accessors[accessor_index].get("count")
            if not isinstance(count, int) or count < 0 or count % 3:
                raise AssetValidationError("triangle accessor count is invalid")
            triangle_count += count // 3
            position_index = primitive.get("attributes", {}).get("POSITION")
            if not isinstance(position_index, int) or not (
                0 <= position_index < len(accessors)
            ):
                raise AssetValidationError("POSITION accessor is invalid")
            position = accessors[position_index]
            minimum = position.get("min")
            maximum = position.get("max")
            if not isinstance(minimum, list) or not isinstance(maximum, list):
                raise AssetValidationError("POSITION bounds are missing")
            if len(minimum) != 3 or len(maximum) != 3 \
                    or not _vector_close(minimum, minimum) \
                    or not _vector_close(maximum, maximum):
                raise AssetValidationError("POSITION bounds are invalid")
            bounds_min = [min(bounds_min[i], float(minimum[i])) for i in range(3)]
            bounds_max = [max(bounds_max[i], float(maximum[i])) for i in range(3)]

    if triangle_count != technical.get("trianglesLod0"):
        raise AssetValidationError("GLB triangle count does not match manifest")
    if triangle_count > budgets.get("maxTrianglesLod0", triangle_count):
        raise AssetValidationError("GLB exceeds triangle budget")
    if len(materials) != technical.get("materials"):
        raise AssetValidationError("GLB material count does not match manifest")
    if len(materials) > budgets.get("maxMaterials", len(materials)):
        raise AssetValidationError("GLB exceeds material budget")
    expected_bounds = technical.get("boundsMeters", {})
    if not _vector_close(bounds_min, expected_bounds.get("minimum", [])):
        raise AssetValidationError("GLB minimum bounds do not match manifest")
    if not _vector_close(bounds_max, expected_bounds.get("maximum", [])):
        raise AssetValidationError("GLB maximum bounds do not match manifest")

    buffer_views = document.get("bufferViews", [])
    texture_bytes = 0
    for image in document.get("images", []):
        view_index = image.get("bufferView")
        if not isinstance(view_index, int) or not (0 <= view_index < len(buffer_views)):
            raise AssetValidationError("embedded image bufferView is invalid")
        texture_bytes += int(buffer_views[view_index].get("byteLength", -1))
    if texture_bytes != technical.get("textureBytes"):
        raise AssetValidationError("GLB texture bytes do not match manifest")
    if texture_bytes > budgets.get("maxTextureBytes", texture_bytes):
        raise AssetValidationError("GLB exceeds texture byte budget")

    animation_names = [
        animation.get("name") for animation in document.get("animations", [])
    ]
    if animation_names != technical.get("animations", []):
        raise AssetValidationError("GLB animations do not match manifest")

    node_by_name = {node["name"]: (index, node) for index, node in enumerate(nodes)}
    for socket in technical.get("sockets", []):
        name = socket["name"]
        if name not in node_by_name:
            raise AssetValidationError(f"missing GLB socket: {name}")
        index, node = node_by_name[name]
        actual_position = _node_world_translation(nodes, parents, index)
        if not _vector_close(actual_position, socket["positionMeters"]):
            raise AssetValidationError(f"GLB socket position mismatch: {name}")
        actual_width = node.get("extras", {}).get("socket_half_width_m")
        if not isinstance(actual_width, (int, float)) or not math.isclose(
            float(actual_width),
            float(socket["halfWidthMeters"]),
            rel_tol=0.0,
            abs_tol=FLOAT_TOLERANCE,
        ):
            raise AssetValidationError(f"GLB socket width mismatch: {name}")


def validate_glb_document(
    document: dict[str, Any], glb_bytes: int, manifest: dict[str, Any]
) -> None:
    try:
        _validate_glb_document(document, glb_bytes, manifest)
    except AssetValidationError:
        raise
    except (IndexError, KeyError, OverflowError, TypeError, ValueError) as error:
        raise AssetValidationError(
            f"malformed GLB structure: {error}"
        ) from error


def validate_manifest(
    root: Path, manifest_path: Path, schema: dict[str, Any]
) -> dict[str, Any]:
    manifest = load_json_file(manifest_path)
    validate_against_schema(manifest, schema)

    license_data = manifest["license"]
    review = manifest["review"]
    if license_data["decision"] == "reject" or review["status"] == "rejected":
        raise AssetValidationError("rejected asset is present in source")
    if license_data["decision"] == "conditional" and (
        review["status"] != "approved" or not license_data.get("conditions")
    ):
        raise AssetValidationError("conditional asset is not fully approved")
    if not license_data["modificationAllowed"] or not license_data[
        "redistributionAllowed"
    ]:
        raise AssetValidationError("asset lacks modification or redistribution rights")

    archived_license = license_data.get("archivedLicensePath")
    if archived_license:
        resolve_repository_file(root, archived_license)

    seen_paths: set[str] = set()
    glb_paths: list[Path] = []
    runtime_qml: list[Path] = []
    runtime_mesh_names: list[str] = []
    for entry in manifest["files"]:
        relative_path = entry["path"]
        if relative_path in seen_paths:
            raise AssetValidationError(f"duplicate asset path: {relative_path}")
        seen_paths.add(relative_path)
        path = resolve_repository_file(root, relative_path)
        if path.stat().st_size > 64 * 1024 * 1024:
            raise AssetValidationError(f"asset file is too large: {relative_path}")
        expected_hash = entry["sha256"]
        if not SHA256.fullmatch(expected_hash) or file_sha256(path) != expected_hash:
            raise AssetValidationError(f"asset hash mismatch: {relative_path}")
        if path.suffix.lower() == ".glb":
            glb_paths.append(path)
        if entry["purpose"] == "runtime" and path.suffix.lower() == ".qml":
            runtime_qml.append(path)
        if entry["purpose"] == "runtime" and path.suffix.lower() == ".mesh":
            runtime_mesh_names.append(path.name)

    if manifest["technical"]["format"] == "glb" and len(glb_paths) != 1:
        raise AssetValidationError("GLB asset must list exactly one GLB file")
    for glb_path in glb_paths:
        document, size = read_glb(glb_path)
        validate_glb_document(document, size, manifest)

    qml_text = "\n".join(path.read_text(encoding="utf-8") for path in runtime_qml)
    if re.search(r"\bRuntimeLoader\b|(?:https?|file):|\.\./", qml_text):
        raise AssetValidationError("runtime QML contains dynamic or external loading")
    for mesh_name in runtime_mesh_names:
        if mesh_name not in qml_text:
            raise AssetValidationError(f"runtime mesh is not referenced by QML: {mesh_name}")
    return manifest


def validate_repository(root: Path) -> list[str]:
    root = root.resolve(strict=True)
    schema_path = root / "doc/design/workout_game_asset_manifest.schema.json"
    schema = load_json_file(schema_path)
    manifest_directory = root / "contrib/workout-game-assets/manifests"
    if manifest_directory.is_symlink() or not manifest_directory.is_dir():
        raise AssetValidationError("asset manifest directory is unavailable")
    manifests = sorted(manifest_directory.glob("*.json"))
    if not manifests:
        raise AssetValidationError("no asset manifests found")
    asset_ids: list[str] = []
    for manifest_path in manifests:
        manifest = validate_manifest(root, manifest_path, schema)
        asset_ids.append(manifest["assetId"])
    if len(asset_ids) != len(set(asset_ids)):
        raise AssetValidationError("duplicate asset IDs")
    return asset_ids


def parse_arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="Validate Workout Game asset manifests and GLB files"
    )
    parser.add_argument("--root", type=Path, default=repository)
    return parser.parse_args()


def main() -> None:
    arguments = parse_arguments()
    try:
        asset_ids = validate_repository(arguments.root)
    except (AssetValidationError, OSError) as error:
        print(f"Workout Game asset validation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
    print(
        f"Validated {len(asset_ids)} Workout Game asset manifest(s): "
        + ", ".join(asset_ids)
    )


if __name__ == "__main__":
    main()
