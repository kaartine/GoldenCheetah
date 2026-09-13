#!/usr/bin/env python3
"""Clean a quarantined TripoSR review GLB into bounded review meshes."""

from __future__ import annotations

import argparse
import copy
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import shutil
import struct
import sys
import traceback

import bmesh
import bpy
from mathutils import Matrix, Vector


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
GLB_HEADER = struct.Struct("<4sII")
CHUNK_HEADER = struct.Struct("<II")
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
OUTPUT_NAMES = (
    "candidate-lod0.glb",
    "candidate-lod1.glb",
    "candidate-collision.glb",
)
EPSILON = 1.0e-7
MIN_TRIANGLE_AREA = 1.0e-12
AXES = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"}
TARGET_RIGHT = Vector((1.0, 0.0, 0.0))
TARGET_FORWARD = Vector((0.0, -1.0, 0.0))
TARGET_UP = Vector((0.0, 0.0, 1.0))
REQUIRED_BLENDER_VERSION = (4, 0, 2)


def load_candidate_module():
    module_path = SCRIPT_DIRECTORY.parent / "triposr_candidate.py"
    spec = importlib.util.spec_from_file_location("gc_triposr_candidate", module_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load candidate validator from {module_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


candidate_validator = load_candidate_module()
MAXIMUM_INPUT_BYTES = candidate_validator.HARD_MAX_GLB_BYTES


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(
        description="Clean an external TripoSR GLB into deterministic review LODs"
    )
    parser.add_argument("--input", required=True, help="External raw .glb")
    parser.add_argument("--output-dir", required=True, help="External output directory")
    parser.add_argument(
        "--up",
        choices=sorted(AXES),
        default="+Y",
        help="Raw glTF up axis before Blender import (default: +Y)",
    )
    parser.add_argument(
        "--forward",
        choices=sorted(AXES),
        default="+Z",
        help="Raw glTF forward axis before Blender import (default: +Z)",
    )
    parser.add_argument(
        "--scale",
        type=float,
        default=1.0,
        help="Metres per raw coordinate unit after axis normalization",
    )
    parser.add_argument("--lod0-triangles", type=int, default=18_000)
    parser.add_argument("--lod1-triangles", type=int, default=6_000)
    parser.add_argument(
        "--lod1-ratio",
        type=float,
        default=0.40,
        help="Maximum LOD1 to realized LOD0 triangle ratio",
    )
    parser.add_argument(
        "--min-fragment-triangles",
        type=int,
        default=3,
        help="Loose parts at or below this size may be removed",
    )
    parser.add_argument(
        "--max-fragment-area-ratio",
        type=float,
        default=0.00025,
    )
    parser.add_argument(
        "--max-fragment-extent-ratio",
        type=float,
        default=0.015,
    )
    return parser.parse_args(arguments)


def read_external_input(value: str):
    try:
        snapshot = candidate_validator.read_external_snapshot(
            Path(value), "raw TripoSR input", MAXIMUM_INPUT_BYTES
        )
    except candidate_validator.CandidateError as error:
        raise RuntimeError(str(error)) from error
    if snapshot.path.suffix.lower() != ".glb":
        raise RuntimeError("--input must name an existing GLB file")
    return snapshot


def resolve_external_output(value: str) -> Path:
    try:
        path = candidate_validator.external_destination(Path(value))
    except candidate_validator.CandidateError as error:
        raise RuntimeError(str(error)) from error
    path.mkdir(parents=True, exist_ok=True)
    try:
        path = candidate_validator.external_destination(path)
    except candidate_validator.CandidateError as error:
        raise RuntimeError(str(error)) from error
    if not path.is_dir():
        raise RuntimeError("--output-dir must name a directory")
    return path


def validate_arguments(arguments: argparse.Namespace) -> None:
    if not math.isfinite(arguments.scale) or arguments.scale <= 0.0:
        raise RuntimeError("--scale must be a finite positive number")
    if not 1 <= arguments.lod0_triangles <= 18_000:
        raise RuntimeError("--lod0-triangles must be in 1..18000")
    if not 1 <= arguments.lod1_triangles <= 6_000:
        raise RuntimeError("--lod1-triangles must be in 1..6000")
    if not 0.01 <= arguments.lod1_ratio <= 0.40:
        raise RuntimeError("--lod1-ratio must be in 0.01..0.40")
    if arguments.min_fragment_triangles < 0:
        raise RuntimeError("--min-fragment-triangles must not be negative")
    for name in ("max_fragment_area_ratio", "max_fragment_extent_ratio"):
        value = getattr(arguments, name)
        if not math.isfinite(value) or not 0.0 <= value <= 0.1:
            raise RuntimeError(f"--{name.replace('_', '-')} must be in 0..0.1")


def validate_blender_version() -> None:
    if tuple(bpy.app.version) != REQUIRED_BLENDER_VERSION:
        expected = ".".join(str(value) for value in REQUIRED_BLENDER_VERSION)
        actual = ".".join(str(value) for value in bpy.app.version)
        raise RuntimeError(f"Blender {expected} is required, got {actual}")


def padded(payload: bytes, fill: bytes) -> bytes:
    return payload + fill * ((-len(payload)) % 4)


def encode_glb(document: dict, binary: bytes) -> bytes:
    json_bytes = padded(
        json.dumps(
            document,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("utf-8"),
        b" ",
    )
    binary = padded(binary, b"\0")
    chunks = [CHUNK_HEADER.pack(len(json_bytes), JSON_CHUNK), json_bytes]
    if binary:
        chunks.extend((CHUNK_HEADER.pack(len(binary), BIN_CHUNK), binary))
    body = b"".join(chunks)
    return GLB_HEADER.pack(b"glTF", 2, GLB_HEADER.size + len(body)) + body


def write_glb(path: Path, document: dict, binary: bytes) -> None:
    path.write_bytes(encode_glb(document, binary))


def strip_extensions(value) -> None:
    if isinstance(value, dict):
        value.pop("extensions", None)
        value.pop("extras", None)
        for child in value.values():
            strip_extensions(child)
    elif isinstance(value, list):
        for child in value:
            strip_extensions(child)


def compact_geometry(document: dict, binary: bytes) -> bytes:
    accessors = document.get("accessors")
    if not isinstance(accessors, list) or not isinstance(document.get("bufferViews"), list):
        raise RuntimeError("input GLB geometry collections are malformed")
    if not isinstance(document.get("buffers"), list) or len(document["buffers"]) != 1:
        raise RuntimeError("input GLB must contain one embedded buffer")

    references: list[tuple[str, int]] = []
    retained_semantics: dict[int, str] = {}

    def retain_accessor(value, semantic: str) -> int:
        if not isinstance(value, int) or isinstance(value, bool):
            raise RuntimeError("input contains a non-integer accessor reference")
        if value < 0 or value >= len(accessors):
            raise RuntimeError("input contains an out-of-range accessor reference")
        previous = retained_semantics.get(value)
        if previous is not None and previous != semantic:
            raise RuntimeError("one accessor is reused for incompatible geometry roles")
        if previous is None:
            retained_semantics[value] = semantic
            references.append((semantic, value))
        return value

    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            primitive["attributes"] = {
                name: retain_accessor(index, name)
                for name, index in primitive["attributes"].items()
            }
            if "indices" in primitive:
                primitive["indices"] = retain_accessor(primitive["indices"], "INDICES")

    compact_binary = bytearray()
    compact_accessors: list[dict] = []
    compact_views: list[dict] = []
    accessor_map: dict[int, int] = {}
    component_values = 0
    for semantic, old_index in references:
        source = accessors[old_index]
        if not isinstance(source, dict) or "sparse" in source:
            raise RuntimeError("referenced sparse or malformed accessors are not accepted")
        expected_type = "SCALAR" if semantic == "INDICES" else "VEC3"
        expected_component = None if semantic == "INDICES" else 5126
        if source.get("type") != expected_type or (
            expected_component is not None
            and source.get("componentType") != expected_component
        ):
            raise RuntimeError(f"{semantic} accessor format is unsupported")
        try:
            values = list(
                candidate_validator.accessor_values(document, binary, old_index)
            )
        except candidate_validator.CandidateError as error:
            raise RuntimeError(f"cannot decode {semantic} accessor: {error}") from error
        component_values += sum(len(row) for row in values)
        if component_values > candidate_validator.HARD_MAX_COMPONENT_VALUES:
            raise RuntimeError("input exceeds the decoded geometry component limit")
        if semantic == "INDICES":
            if any(value[0] < 0 or value[0] > 0xFFFFFFFF for value in values):
                raise RuntimeError("input index is outside the unsigned 32-bit range")
            payload = b"".join(struct.pack("<I", int(value[0])) for value in values)
            component_type = 5125
            target = 34963
        else:
            payload = b"".join(struct.pack("<fff", *map(float, value)) for value in values)
            component_type = 5126
            target = 34962
        view_index = len(compact_views)
        compact_views.append(
            {
                "buffer": 0,
                "byteOffset": len(compact_binary),
                "byteLength": len(payload),
                "target": target,
            }
        )
        compact_binary.extend(payload)
        compact_accessor = {
            "bufferView": view_index,
            "componentType": component_type,
            "count": len(values),
            "type": expected_type,
        }
        if semantic != "INDICES":
            compact_accessor["min"] = [min(row[i] for row in values) for i in range(3)]
            compact_accessor["max"] = [max(row[i] for row in values) for i in range(3)]
        accessor_map[old_index] = len(compact_accessors)
        compact_accessors.append(compact_accessor)

    for mesh in document["meshes"]:
        for primitive in mesh["primitives"]:
            primitive["attributes"] = {
                name: accessor_map[index]
                for name, index in primitive["attributes"].items()
            }
            if "indices" in primitive:
                primitive["indices"] = accessor_map[primitive["indices"]]

    document["accessors"] = compact_accessors
    document["bufferViews"] = compact_views
    document["buffers"] = [{"byteLength": len(compact_binary)}]
    return bytes(compact_binary)


def make_sanitized_import(source_data: bytes, destination: Path) -> dict[str, int]:
    try:
        document, binary = candidate_validator.read_glb(source_data)
    except candidate_validator.CandidateError as error:
        raise RuntimeError(str(error)) from error
    buffers = document.get("buffers", [])
    if not isinstance(buffers, list) or not buffers:
        raise RuntimeError("input GLB has no buffer")
    if any(isinstance(item, dict) and item.get("uri") for item in buffers):
        raise RuntimeError("external geometry buffers are not accepted")
    meshes = document.get("meshes", [])
    if not isinstance(meshes, list) or not meshes:
        raise RuntimeError("input GLB has no meshes")

    removed = {
        "animations": len(document.get("animations", [])),
        "cameras": len(document.get("cameras", [])),
        "images": len(document.get("images", [])),
        "lights": len(
            document.get("extensions", {})
            .get("KHR_lights_punctual", {})
            .get("lights", [])
        )
        if isinstance(document.get("extensions"), dict)
        else 0,
        "materials": len(document.get("materials", [])),
        "skins": len(document.get("skins", [])),
        "textures": len(document.get("textures", [])),
    }
    for key in (
        "animations",
        "cameras",
        "images",
        "materials",
        "samplers",
        "skins",
        "textures",
        "extensionsUsed",
        "extensionsRequired",
    ):
        document.pop(key, None)
    strip_extensions(document)

    for mesh in meshes:
        if not isinstance(mesh, dict):
            raise RuntimeError("input contains an invalid mesh entry")
        mesh.pop("weights", None)
        primitives = mesh.get("primitives", [])
        if not isinstance(primitives, list) or not primitives:
            raise RuntimeError("input contains an empty mesh")
        for primitive in primitives:
            if not isinstance(primitive, dict):
                raise RuntimeError("input contains an invalid mesh primitive")
            primitive.pop("material", None)
            primitive.pop("targets", None)
            attributes = primitive.get("attributes")
            if not isinstance(attributes, dict) or "POSITION" not in attributes:
                raise RuntimeError("every primitive must contain POSITION")
            primitive["attributes"] = {
                key: value
                for key, value in attributes.items()
                if key in {"POSITION", "NORMAL"}
            }
    for node in document.get("nodes", []):
        if isinstance(node, dict):
            node.pop("camera", None)
            node.pop("skin", None)
            node.pop("weights", None)

    binary = compact_geometry(document, binary)

    validation_document = copy.deepcopy(document)
    for node in validation_document.get("nodes", []):
        if isinstance(node, dict):
            for transform in ("matrix", "translation", "rotation", "scale"):
                node.pop(transform, None)
    validation_data = encode_glb(validation_document, binary)
    try:
        candidate_validator.inspect_glb(
            validation_data,
            {
                "maxGlbBytes": MAXIMUM_INPUT_BYTES,
                "maxTriangles": 1_000_000,
                "maxMaterials": 0,
                "maxTextures": 0,
                "maxTextureBytes": 0,
            },
        )
    except candidate_validator.CandidateError as error:
        raise RuntimeError(f"sanitized input failed structural validation: {error}") from error

    write_glb(destination, document, binary)
    return removed


def axis_vector(axis: str) -> Vector:
    sign = 1.0 if axis[0] == "+" else -1.0
    values = {
        "X": Vector((1.0, 0.0, 0.0)),
        "Y": Vector((0.0, 1.0, 0.0)),
        "Z": Vector((0.0, 0.0, 1.0)),
    }
    return values[axis[1]] * sign


def gltf_to_blender(vector: Vector) -> Vector:
    return Vector((vector.x, -vector.z, vector.y))


def axis_normalization(up_axis: str, forward_axis: str) -> Matrix:
    source_up = gltf_to_blender(axis_vector(up_axis))
    source_forward = gltf_to_blender(axis_vector(forward_axis))
    if abs(source_up.dot(source_forward)) > EPSILON:
        raise RuntimeError("--up and --forward must be perpendicular")
    source_right = source_up.cross(source_forward)
    if source_right.length < EPSILON:
        raise RuntimeError("--up and --forward do not define an orientation")
    source = Matrix((source_right, source_forward, source_up)).transposed()
    target = Matrix((TARGET_RIGHT, TARGET_FORWARD, TARGET_UP)).transposed()
    transform = target @ source.inverted()
    if not math.isclose(transform.determinant(), 1.0, abs_tol=1.0e-6):
        raise RuntimeError("axis normalization would mirror the mesh")
    return transform


def reset_and_import(path: Path) -> list[bpy.types.Object]:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(path), import_pack_images=False)
    mesh_objects = sorted(
        (obj for obj in bpy.context.scene.objects if obj.type == "MESH"),
        key=lambda obj: obj.name,
    )
    if not mesh_objects:
        raise RuntimeError("sanitized GLB imported no mesh objects")
    return mesh_objects


def bake_mesh_object(obj: bpy.types.Object, transform: Matrix, scale: float) -> None:
    if obj.data.shape_keys is not None:
        obj.shape_key_clear()
    obj.animation_data_clear()
    for modifier in list(obj.modifiers):
        obj.modifiers.remove(modifier)
    obj.vertex_groups.clear()
    world = obj.matrix_world.copy()
    for vertex in obj.data.vertices:
        vertex.co = transform @ (world @ vertex.co) * scale
    obj.parent = None
    obj.matrix_world = Matrix.Identity(4)
    obj.data.materials.clear()


def select_only(objects: list[bpy.types.Object]) -> None:
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]


def join_objects(objects: list[bpy.types.Object]) -> bpy.types.Object:
    if not objects:
        raise RuntimeError("cannot join an empty object list")
    select_only(objects)
    if len(objects) > 1:
        bpy.ops.object.join()
    return bpy.context.active_object


def triangulate_and_weld(obj: bpy.types.Object) -> None:
    mesh = obj.data
    editable = bmesh.new()
    editable.from_mesh(mesh)
    bmesh.ops.remove_doubles(editable, verts=editable.verts, dist=1.0e-7)
    bmesh.ops.dissolve_degenerate(editable, edges=editable.edges, dist=1.0e-9)
    bmesh.ops.triangulate(editable, faces=editable.faces)
    invalid_faces = [
        face for face in editable.faces if face.calc_area() <= MIN_TRIANGLE_AREA
    ]
    if invalid_faces:
        bmesh.ops.delete(editable, geom=invalid_faces, context="FACES_ONLY")
        orphan_vertices = [vertex for vertex in editable.verts if not vertex.link_faces]
        if orphan_vertices:
            bmesh.ops.delete(editable, geom=orphan_vertices, context="VERTS")
    bmesh.ops.recalc_face_normals(editable, faces=editable.faces)
    editable.to_mesh(mesh)
    editable.free()
    mesh.validate(clean_customdata=True)
    mesh.update(calc_edges=True)


def object_bounds(obj: bpy.types.Object) -> tuple[Vector, Vector]:
    if not obj.data.vertices:
        raise RuntimeError(f"{obj.name} contains no vertices")
    minimum = Vector((math.inf, math.inf, math.inf))
    maximum = Vector((-math.inf, -math.inf, -math.inf))
    for vertex in obj.data.vertices:
        for index in range(3):
            minimum[index] = min(minimum[index], vertex.co[index])
            maximum[index] = max(maximum[index], vertex.co[index])
    return minimum, maximum


def object_area(obj: bpy.types.Object) -> float:
    return sum(polygon.area for polygon in obj.data.polygons)


def split_loose_parts(obj: bpy.types.Object) -> list[bpy.types.Object]:
    select_only([obj])
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.separate(type="LOOSE")
    bpy.ops.object.mode_set(mode="OBJECT")
    parts = sorted(
        (
            candidate
            for candidate in bpy.context.selected_objects
            if candidate.type == "MESH"
        ),
        key=lambda candidate: (
            -len(candidate.data.polygons),
            tuple(round(value, 9) for value in object_bounds(candidate)[0]),
            tuple(round(value, 9) for value in object_bounds(candidate)[1]),
        ),
    )
    for index, part in enumerate(parts):
        part.name = f"SOURCE_PART_{index:04d}"
        part.data.name = f"SOURCE_PART_{index:04d}_MESH"
    return parts


def clean_imported_meshes(
    mesh_objects: list[bpy.types.Object],
    transform: Matrix,
    arguments: argparse.Namespace,
) -> tuple[list[bpy.types.Object], dict[str, int]]:
    for obj in mesh_objects:
        bake_mesh_object(obj, transform, arguments.scale)
    joined = join_objects(mesh_objects)
    joined.name = "SOURCE_JOINED"
    triangulate_and_weld(joined)
    parts = split_loose_parts(joined)
    if not parts:
        raise RuntimeError("input contains no usable loose mesh parts")

    total_area = sum(object_area(part) for part in parts)
    global_minimum = Vector((math.inf, math.inf, math.inf))
    global_maximum = Vector((-math.inf, -math.inf, -math.inf))
    for part in parts:
        minimum, maximum = object_bounds(part)
        for index in range(3):
            global_minimum[index] = min(global_minimum[index], minimum[index])
            global_maximum[index] = max(global_maximum[index], maximum[index])
    global_extent = max((global_maximum - global_minimum).length, EPSILON)
    largest = max(parts, key=lambda part: len(part.data.polygons))
    removed_fragments = 0
    kept = []
    for part in parts:
        triangles = len(part.data.polygons)
        minimum, maximum = object_bounds(part)
        removable = (
            part is not largest
            and triangles <= arguments.min_fragment_triangles
            and object_area(part) <= total_area * arguments.max_fragment_area_ratio
            and (maximum - minimum).length
            <= global_extent * arguments.max_fragment_extent_ratio
        )
        if removable:
            bpy.data.objects.remove(part, do_unlink=True)
            removed_fragments += 1
        else:
            kept.append(part)
    if not kept:
        raise RuntimeError("fragment cleanup removed all geometry")
    return kept, {
        "importedMeshObjects": len(mesh_objects),
        "looseParts": len(parts),
        "removedFragments": removed_fragments,
    }


def duplicate_parts(
    parts: list[bpy.types.Object], prefix: str
) -> list[bpy.types.Object]:
    duplicates = []
    for index, source in enumerate(parts):
        duplicate = source.copy()
        duplicate.data = source.data.copy()
        duplicate.name = f"{prefix}_PART_{index:04d}"
        duplicate.data.name = f"{prefix}_PART_{index:04d}_MESH"
        bpy.context.collection.objects.link(duplicate)
        duplicates.append(duplicate)
    return duplicates


def allocate_targets(parts: list[bpy.types.Object], budget: int) -> list[int]:
    counts = [len(part.data.polygons) for part in parts]
    total = sum(counts)
    if total <= budget:
        return counts
    if budget < len(parts):
        raise RuntimeError(
            f"triangle budget {budget} cannot retain {len(parts)} loose parts"
        )
    ratio = budget / total
    targets = [min(count, max(1, int(math.floor(count * ratio)))) for count in counts]
    while sum(targets) > budget:
        index = max(
            (item for item in range(len(parts)) if targets[item] > 1),
            key=lambda item: (targets[item], counts[item], -item),
            default=None,
        )
        if index is None:
            raise RuntimeError("cannot allocate the requested triangle budget")
        targets[index] -= 1
    return targets


def decimate_object(obj: bpy.types.Object, target: int) -> None:
    for _attempt in range(5):
        current = len(obj.data.polygons)
        if current <= target:
            return
        modifier = obj.modifiers.new(name="DeterministicDecimate", type="DECIMATE")
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = max(0.0001, min(1.0, (target / current) * 0.995))
        modifier.use_collapse_triangulate = True
        select_only([obj])
        bpy.ops.object.modifier_apply(modifier=modifier.name)
        triangulate_and_weld(obj)
    if len(obj.data.polygons) > target:
        raise RuntimeError(
            f"could not reduce {obj.name} to {target} triangles "
            f"(got {len(obj.data.polygons)})"
        )


def center_and_ground(obj: bpy.types.Object) -> None:
    minimum, maximum = object_bounds(obj)
    offset = Vector(
        (
            -(minimum.x + maximum.x) * 0.5,
            -(minimum.y + maximum.y) * 0.5,
            -minimum.z,
        )
    )
    for vertex in obj.data.vertices:
        vertex.co += offset
    obj.data.update()


def review_material() -> bpy.types.Material:
    material = bpy.data.materials.get("MAT_CandidateReview")
    if material is None:
        material = bpy.data.materials.new("MAT_CandidateReview")
        material.diffuse_color = (0.62, 0.27, 0.055, 1.0)
        material.use_nodes = True
        principled = material.node_tree.nodes.get("Principled BSDF")
        principled.inputs["Base Color"].default_value = (0.62, 0.27, 0.055, 1.0)
        principled.inputs["Roughness"].default_value = 0.72
        principled.inputs["Metallic"].default_value = 0.0
    return material


def join_level(
    parts: list[bpy.types.Object], name: str, budget: int
) -> bpy.types.Object:
    targets = allocate_targets(parts, budget)
    for part, target in zip(parts, targets):
        decimate_object(part, target)
    result = join_objects(parts)
    result.name = name
    result.data.name = f"{name}_MESH"
    triangulate_and_weld(result)
    center_and_ground(result)
    result.data.materials.clear()
    result.data.materials.append(review_material())
    result.location = (0.0, 0.0, 0.0)
    result.rotation_euler = (0.0, 0.0, 0.0)
    result.scale = (1.0, 1.0, 1.0)
    result["source_role"] = "triposr-review-mesh"
    result["runtime_rig"] = False
    result["physics_authority"] = "external"
    triangles = len(result.data.polygons)
    if triangles <= 0 or triangles > budget:
        raise RuntimeError(f"{name} triangle count {triangles} exceeds {budget}")
    return result


def build_collision(lod0: bpy.types.Object) -> bpy.types.Object:
    minimum, maximum = object_bounds(lod0)
    vertices = [
        (x_value, y_value, z_value)
        for x_value in (minimum.x, maximum.x)
        for y_value in (minimum.y, maximum.y)
        for z_value in (minimum.z, maximum.z)
    ]
    faces = [
        (0, 1, 3), (0, 3, 2),
        (4, 6, 7), (4, 7, 5),
        (0, 4, 5), (0, 5, 1),
        (2, 3, 7), (2, 7, 6),
        (0, 2, 6), (0, 6, 4),
        (1, 5, 7), (1, 7, 3),
    ]
    mesh = bpy.data.meshes.new("CandidateCollision_MESH")
    mesh.from_pydata(vertices, [], faces)
    mesh.update(calc_edges=True)
    collision = bpy.data.objects.new("CandidateCollision", mesh)
    bpy.context.collection.objects.link(collision)
    collision["collision_role"] = "review-proxy"
    collision["physics_authority"] = "external"
    if len(mesh.polygons) > 100:
        raise RuntimeError("collision mesh exceeds 100 triangles")
    return collision


def assert_identity_and_finite(obj: bpy.types.Object, budget: int) -> dict:
    if obj.location.length > EPSILON:
        raise RuntimeError(f"{obj.name} location is not identity")
    if any(abs(value) > EPSILON for value in obj.rotation_euler):
        raise RuntimeError(f"{obj.name} rotation is not identity")
    if any(abs(value - 1.0) > EPSILON for value in obj.scale):
        raise RuntimeError(f"{obj.name} scale is not identity")
    if any(
        not all(math.isfinite(value) for value in vertex.co)
        for vertex in obj.data.vertices
    ):
        raise RuntimeError(f"{obj.name} contains non-finite vertices")
    if any(
        polygon.loop_total != 3 or polygon.area <= MIN_TRIANGLE_AREA
        for polygon in obj.data.polygons
    ):
        raise RuntimeError(f"{obj.name} contains invalid triangles")
    triangles = len(obj.data.polygons)
    if triangles <= 0 or triangles > budget:
        raise RuntimeError(f"{obj.name} exceeds its triangle budget")
    minimum, maximum = object_bounds(obj)
    if not math.isclose(minimum.z, 0.0, abs_tol=1.0e-6):
        raise RuntimeError(f"{obj.name} is not grounded at Z=0")
    if not math.isclose(minimum.x + maximum.x, 0.0, abs_tol=1.0e-6):
        raise RuntimeError(f"{obj.name} is not centered on X")
    if not math.isclose(minimum.y + maximum.y, 0.0, abs_tol=1.0e-6):
        raise RuntimeError(f"{obj.name} is not centered on Y")
    return {
        "vertices": len(obj.data.vertices),
        "triangles": triangles,
        "boundsMinBlender": [round(value, 9) for value in minimum],
        "boundsMaxBlender": [round(value, 9) for value in maximum],
    }


def export_glb(obj: bpy.types.Object, path: Path) -> None:
    select_only([obj])
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        check_existing=False,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_extras=False,
        export_cameras=False,
        export_lights=False,
        export_animations=False,
    )
    if not path.is_file() or path.stat().st_size <= 0:
        raise RuntimeError(f"GLB export did not produce {path}")


def canonicalize_export(path: Path, *, collision: bool) -> None:
    try:
        document, binary = candidate_validator.read_glb(path.read_bytes())
    except candidate_validator.CandidateError as error:
        raise RuntimeError(f"cannot validate exported GLB framing: {error}") from error
    strip_extensions(document)
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            if collision:
                primitive["attributes"] = {
                    "POSITION": primitive.get("attributes", {}).get("POSITION")
                }
                primitive.pop("material", None)
    if collision:
        document.pop("materials", None)
    binary = compact_geometry(document, binary)
    write_glb(path, document, binary)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> None:
    validate_blender_version()
    arguments = parse_arguments()
    validate_arguments(arguments)
    source = read_external_input(arguments.input)
    output_directory = resolve_external_output(arguments.output_dir)
    for name in (*OUTPUT_NAMES, "cleanup-report.json", ".triposr-cleanup-work"):
        if (output_directory / name).is_symlink():
            raise RuntimeError(f"output path may not be a symlink: {name}")
    for name in OUTPUT_NAMES:
        (output_directory / name).unlink(missing_ok=True)
    report_path = output_directory / "cleanup-report.json"
    report_path.unlink(missing_ok=True)
    work_directory = output_directory / ".triposr-cleanup-work"
    if work_directory.exists():
        shutil.rmtree(work_directory)
    work_directory.mkdir()
    sanitized = work_directory / "sanitized-input.glb"

    try:
        removed = make_sanitized_import(source.data, sanitized)
        mesh_objects = reset_and_import(sanitized)
        transform = axis_normalization(arguments.up, arguments.forward)
        source_parts, cleanup = clean_imported_meshes(
            mesh_objects, transform, arguments
        )
        raw_triangles = sum(len(part.data.polygons) for part in source_parts)
        if raw_triangles < 3:
            raise RuntimeError("input needs at least three triangles for two LODs")

        lod0_parts = duplicate_parts(source_parts, "LOD0")
        lod0 = join_level(lod0_parts, "CandidateLOD0", arguments.lod0_triangles)
        lod0_triangles = len(lod0.data.polygons)
        lod1_budget = min(
            arguments.lod1_triangles,
            int(math.floor(lod0_triangles * arguments.lod1_ratio + EPSILON)),
        )
        if lod1_budget < 1:
            raise RuntimeError("realized LOD0 is too small to produce a valid LOD1")
        lod1_parts = duplicate_parts(source_parts, "LOD1")
        lod1 = join_level(lod1_parts, "CandidateLOD1", lod1_budget)
        collision = build_collision(lod0)

        technical = {
            "lod0": assert_identity_and_finite(lod0, arguments.lod0_triangles),
            "lod1": assert_identity_and_finite(lod1, lod1_budget),
            "collision": assert_identity_and_finite(collision, 100),
        }
        if technical["lod1"]["triangles"] > technical["lod0"]["triangles"] * 0.40:
            raise RuntimeError("LOD1 exceeds 40 percent of realized LOD0")

        outputs = {}
        for obj, name in zip((lod0, lod1, collision), OUTPUT_NAMES):
            path = output_directory / name
            export_glb(obj, path)
            canonicalize_export(path, collision=obj is collision)
            outputs[name] = {
                "bytes": path.stat().st_size,
                "sha256": sha256(path),
            }
        report = {
            "contractVersion": 1,
            "source": {
                "bytes": source.size,
                "sha256": source.sha256,
            },
            "normalization": {
                "up": arguments.up,
                "forward": arguments.forward,
                "metresPerUnit": arguments.scale,
                "centeredOnGround": True,
            },
            "removed": removed,
            "cleanup": {**cleanup, "retainedTriangles": raw_triangles},
            "technical": technical,
            "outputs": outputs,
            "limitations": [
                "review-mesh-only",
                "monolithic-not-runtime-rig",
                "collision-not-physics-authority",
            ],
        }
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True, ensure_ascii=True) + "\n",
            encoding="ascii",
        )
        print(
            "Generated",
            output_directory,
            f"(LOD0 {technical['lod0']['triangles']}, "
            f"LOD1 {technical['lod1']['triangles']}, "
            f"collision {technical['collision']['triangles']} triangles)",
        )
    finally:
        shutil.rmtree(work_directory, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
