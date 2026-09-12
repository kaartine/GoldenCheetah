#!/usr/bin/env python3
"""Generate the socketed Workout Game drop-face asset as a GLB."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import traceback
from typing import Iterable, Sequence

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_tabletop import canonical_to_blender, create_empty, make_material


ASSET_NAME = "Drop_Greybox"
ROOT_NAME = "ROOT_Drop"
MESH_NAME = "GEO_DropFace_LOD0"

SOCKET_HALF_WIDTH_M = 0.68
TILE_LENGTH_M = 24.0
LIP_Z_M = 10.0
SOURCE_HEIGHT_M = 0.70
FACE_HALF_WIDTH_M = 1.70

MAT_FACE = "MAT_DropFace_Grey"
MAT_EDGE = "MAT_DropEdge_Grey"
REQUIRED_NAMES = {
    ROOT_NAME,
    MESH_NAME,
    "SOCKET_IN",
    "SOCKET_OUT",
    "MARKER_PREPARE",
    "MARKER_DECISION",
    "MARKER_ACTION",
    "MARKER_LIP",
    "MARKER_AIR",
    "MARKER_LAND",
    "MARKER_RECOVERY",
}
EPSILON = 1.0e-7
SERIALIZED_FLOAT_EPSILON = 1.0e-6


def assert_close(actual: float, expected: float, message: str) -> None:
    if not math.isclose(
        actual, expected, rel_tol=0.0, abs_tol=SERIALIZED_FLOAT_EPSILON
    ):
        raise RuntimeError(f"{message}: expected {expected}, got {actual}")


def assert_finite(values: Iterable[float], message: str) -> None:
    if not all(math.isfinite(float(value)) for value in values):
        raise RuntimeError(message)


def create_drop_face(root, materials):
    x_values = (-FACE_HALF_WIDTH_M, -0.90, -0.35, 0.35, 0.90,
                FACE_HALF_WIDTH_M)
    top_z = (9.98, 10.03, 9.99, 10.02, 9.97, 10.01)
    middle_z = (10.16, 10.21, 10.18, 10.23, 10.17, 10.20)
    bottom_z = (10.35, 10.42, 10.38, 10.44, 10.36, 10.40)
    vertices: list[tuple[float, float, float]] = []
    for row_y, row_z in (
        (0.02, top_z),
        (-0.31, middle_z),
        (-SOURCE_HEIGHT_M, bottom_z),
    ):
        vertices.extend(
            (x_value, row_y, z_value)
            for x_value, z_value in zip(x_values, row_z)
        )

    faces: list[tuple[int, int, int]] = []
    material_indices: list[int] = []
    columns = len(x_values)
    for row in range(2):
        for column in range(columns - 1):
            upper_left = row * columns + column
            upper_right = upper_left + 1
            lower_left = (row + 1) * columns + column
            lower_right = lower_left + 1
            faces.extend((
                (upper_left, lower_left, lower_right),
                (upper_left, lower_right, upper_right),
            ))
            material_indices.extend((1 if row == 0 else 0,) * 2)

    def append_quad(a, b, c, d, material_index):
        start = len(vertices)
        vertices.extend((a, b, c, d))
        faces.extend(((start, start + 1, start + 2),
                      (start, start + 2, start + 3)))
        material_indices.extend((material_index, material_index))

    # A long flush cap makes the cutoff legible from the 10 m chase view. It
    # remains visual-only and does not replace the authoritative tread.
    append_quad(
        (-SOCKET_HALF_WIDTH_M, 0.022, 9.18),
        (SOCKET_HALF_WIDTH_M, 0.022, 9.18),
        (SOCKET_HALF_WIDTH_M, 0.018, 10.10),
        (-SOCKET_HALF_WIDTH_M, 0.018, 10.10),
        1,
    )
    append_quad(
        (-SOCKET_HALF_WIDTH_M, 0.018, 10.10),
        (SOCKET_HALF_WIDTH_M, 0.018, 10.10),
        (SOCKET_HALF_WIDTH_M, -0.10, 10.16),
        (-SOCKET_HALF_WIDTH_M, -0.10, 10.16),
        1,
    )

    # Low faceted boulders frame the lip outside the 1.36 m tread. Their long
    # approach points and forward noses form a recognizable natural drop gate
    # without reading as rectangular barriers from the chase camera.
    for side in (-1.0, 1.0):
        top = (
            (side * 0.74, 0.08, 9.18),
            (side * FACE_HALF_WIDTH_M, 0.26, 9.28),
            (side * 1.58, 0.30, 9.68),
            (side * 1.56, 0.22, 10.55),
            (side * 0.79, 0.12, 10.28),
            (side * 0.82, 0.25, 9.62),
        )
        bottom = tuple(
            (x_value, -0.38, z_value)
            for x_value, _, z_value in top
        )
        start = len(vertices)
        vertices.extend(top + bottom)
        for index in range(1, len(top) - 1):
            faces.append((start, start + index, start + index + 1))
            material_indices.append(1)
            faces.append((
                start + len(top),
                start + len(top) + index + 1,
                start + len(top) + index,
            ))
            material_indices.append(0)
        for index in range(len(top)):
            next_index = (index + 1) % len(top)
            faces.extend((
                (start + index, start + len(top) + index,
                 start + len(top) + next_index),
                (start + index, start + len(top) + next_index,
                 start + next_index),
            ))
            material_indices.extend((1 if index in (1, 4) else 0,) * 2)

    mesh = bpy.data.meshes.new(name=MESH_NAME)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in vertices], [], faces
    )
    for material in materials:
        mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError("Blender repaired generated drop geometry")
    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False
    result = bpy.data.objects.new(name=MESH_NAME, object_data=mesh)
    bpy.context.collection.objects.link(result)
    result.parent = root
    result["physics_authority"] = "external"
    return result, vertices


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.35
    root["asset_id"] = "WG-04-drop-greybox"
    root["asset_name"] = ASSET_NAME
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["socket_half_width_m"] = SOCKET_HALF_WIDTH_M
    root["physics_authority"] = "external"

    materials = [
        make_material(MAT_FACE, (0.12, 0.105, 0.09, 1.0)),
        make_material(MAT_EDGE, (0.56, 0.50, 0.38, 1.0)),
    ]
    face, vertices = create_drop_face(root, materials)

    socket_properties = {
        "socket_half_width_m": SOCKET_HALF_WIDTH_M,
        "surface_class": "ordinary-trail",
        "grade_percent": 0.0,
        "visual_only": True,
    }
    create_empty(root, "SOCKET_IN", (0.0, 0.0, 0.0), 0.28,
                 {**socket_properties, "socket_role": "in"})
    create_empty(root, "SOCKET_OUT", (0.0, 0.0, TILE_LENGTH_M), 0.28,
                 {**socket_properties, "socket_role": "out"})
    marker_properties = {"visual_only": True, "physics_authority": "external"}
    for name, location in (
        ("MARKER_PREPARE", (0.0, 0.0, 0.0)),
        ("MARKER_DECISION", (0.0, 0.0, 6.0)),
        ("MARKER_ACTION", (0.0, 0.0, 9.5)),
        ("MARKER_LIP", (0.0, 0.0, LIP_Z_M)),
        ("MARKER_AIR", (0.0, -0.20, 10.65)),
        ("MARKER_LAND", (0.0, -SOURCE_HEIGHT_M, 12.5)),
        ("MARKER_RECOVERY", (0.0, -SOURCE_HEIGHT_M, 16.0)),
    ):
        create_empty(root, name, location, 0.18, marker_properties)
    return root, face, vertices


def self_check(root, face, vertices) -> None:
    if bpy.app.version[0] != 4:
        raise RuntimeError(
            f"Blender 4.x is required, found {bpy.app.version_string}"
        )
    names = {obj.name for obj in bpy.context.scene.objects}
    if names != REQUIRED_NAMES:
        raise RuntimeError(
            f"Scene node mismatch; missing={sorted(REQUIRED_NAMES - names)}, "
            f"unexpected={sorted(names - REQUIRED_NAMES)}"
        )
    assert_close(SOCKET_HALF_WIDTH_M, 0.68, "Ordinary socket half-width")
    assert_close(TILE_LENGTH_M, 24.0, "Drop tile length")
    assert_close(SOURCE_HEIGHT_M, 0.70, "Drop source height")
    if FACE_HALF_WIDTH_M <= SOCKET_HALF_WIDTH_M + 0.95:
        raise RuntimeError("Drop face must extend beyond both trail edges")
    if max(point[1] for point in vertices[:18]) > 0.03:
        raise RuntimeError("Drop face must not rise above the upper tread")
    if max(point[1] for point in vertices[18:]) > 0.30:
        raise RuntimeError("Drop shoulders exceed the low rock profile")
    assert_close(min(point[1] for point in vertices), -SOURCE_HEIGHT_M,
                 "Drop face depth")
    if (min(point[2] for point in vertices) < LIP_Z_M - 0.82
            or max(point[2] for point in vertices) > LIP_Z_M + 0.55):
        raise RuntimeError("Drop face escaped its bounded lip envelope")
    for point in vertices:
        assert_finite(point, "Non-finite canonical mesh coordinate")
    if (face.location.length > EPSILON
            or any(abs(value) > EPSILON for value in face.rotation_euler)
            or any(abs(value - 1.0) > EPSILON for value in face.scale)):
        raise RuntimeError("Drop face transform is not applied")
    if face.get("physics_authority") != "external":
        raise RuntimeError("Render mesh claims physics authority")
    if any(polygon.loop_total != 3 or polygon.area <= EPSILON
           for polygon in face.data.polygons):
        raise RuntimeError("Drop face has invalid triangles")
    if len(face.data.vertices) != 50 or len(face.data.polygons) != 64:
        raise RuntimeError("Unexpected drop-face topology")
    if {polygon.material_index for polygon in face.data.polygons} != {0, 1}:
        raise RuntimeError("Both opaque drop materials must be used")
    for socket_name, socket_z in (("SOCKET_IN", 0.0),
                                  ("SOCKET_OUT", TILE_LENGTH_M)):
        socket = bpy.data.objects[socket_name]
        assert_close(float(socket["socket_half_width_m"]), 0.68,
                     f"{socket_name} width")
        expected = canonical_to_blender((0.0, 0.0, socket_z))
        for actual, target in zip(socket.location, expected):
            assert_close(float(actual), target, f"{socket_name} location")
    if (root.location.length > EPSILON
            or any(abs(value) > EPSILON for value in root.rotation_euler)
            or any(abs(value - 1.0) > EPSILON for value in root.scale)):
        raise RuntimeError("Root transform is not identity")


def export_glb(output_path: Path) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    for obj in bpy.context.scene.objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = bpy.data.objects[ROOT_NAME]
    bpy.ops.export_scene.gltf(
        filepath=str(output_path),
        check_existing=False,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_extras=True,
        export_cameras=False,
        export_lights=False,
        export_animations=False,
    )
    if not output_path.is_file() or output_path.stat().st_size <= 0:
        raise RuntimeError(f"GLB export did not produce {output_path}")


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Generate the drop-face GLB")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def main() -> None:
    output_path = Path(os.path.expanduser(parse_arguments().output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root, face, vertices = build_scene()
    self_check(root, face, vertices)
    export_glb(output_path)
    print(
        "Generated", output_path,
        f"({len(face.data.vertices)} vertices, "
        f"{len(face.data.polygons)} triangles)",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
