#!/usr/bin/env python3
"""Generate the Workout Game bunny-hop hurdle as a deterministic GLB."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import traceback
from typing import Iterable, Sequence

import bpy

ASSET_NAME = "BunnyHop_Greybox"
ROOT_NAME = "ROOT_BunnyHop"
MESH_NAME = "GEO_BunnyHopHurdle_LOD0"

SOCKET_HALF_WIDTH_M = 0.68
DEAD_ZONE_M = 1.68
CORE_LENGTH_M = 0.22
TILE_LENGTH_M = DEAD_ZONE_M * 2.0 + CORE_LENGTH_M
HURDLE_CENTER_Z_M = TILE_LENGTH_M * 0.5
HURDLE_HEIGHT_M = 0.20
BAR_HALF_LENGTH_M = 1.02
BAR_HALF_DEPTH_M = 0.12
BAR_HALF_HEIGHT_M = 0.05
SUPPORT_CENTER_X_M = 0.84
SUPPORT_HALF_WIDTH_M = 0.10
SUPPORT_HALF_DEPTH_M = 0.38

MAT_BAR = "MAT_BunnyHopBar_Grey"
MAT_SUPPORT = "MAT_BunnyHopSupport_Grey"
REQUIRED_NAMES = {
    ROOT_NAME,
    MESH_NAME,
    "SOCKET_IN",
    "SOCKET_OUT",
    "MARKER_PREPARE",
    "MARKER_DECISION",
    "MARKER_ACTION",
    "MARKER_PRELOAD",
    "MARKER_TAKEOFF",
    "MARKER_APEX",
    "MARKER_LAND",
}
EPSILON = 1.0e-7
SERIALIZED_FLOAT_EPSILON = 1.0e-6


def canonical_to_blender(point: Sequence[float]) -> tuple[float, float, float]:
    """Map canonical (X, Y-up, Z-forward) to Blender (X, Y, Z-up)."""
    x_value, y_up, z_forward = point
    return (float(x_value), -float(z_forward), float(y_up))


def make_material(name: str, color: tuple[float, float, float, float]):
    material = bpy.data.materials.new(name=name)
    material.diffuse_color = color
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    if principled is None:
        raise RuntimeError(f"{name} has no Principled BSDF node")
    principled.name = f"SHADER_{name}"
    principled.inputs["Base Color"].default_value = color
    principled.inputs["Roughness"].default_value = 1.0
    output = material.node_tree.nodes.get("Material Output")
    if output is None:
        raise RuntimeError(f"{name} has no Material Output node")
    output.name = f"OUTPUT_{name}"
    return material


def create_empty(root, name: str,
                 canonical_location: tuple[float, float, float],
                 display_size: float,
                 properties: dict[str, object] | None = None):
    empty = bpy.data.objects.new(name=name, object_data=None)
    bpy.context.collection.objects.link(empty)
    empty.parent = root
    empty.location = canonical_to_blender(canonical_location)
    empty.empty_display_type = "PLAIN_AXES"
    empty.empty_display_size = display_size
    if properties:
        for key, value in properties.items():
            empty[key] = value
    return empty


def assert_close(actual: float, expected: float, message: str) -> None:
    if not math.isclose(
        actual, expected, rel_tol=0.0, abs_tol=SERIALIZED_FLOAT_EPSILON
    ):
        raise RuntimeError(f"{message}: expected {expected}, got {actual}")


def assert_finite(values: Iterable[float], message: str) -> None:
    if not all(math.isfinite(float(value)) for value in values):
        raise RuntimeError(message)


def add_box(
    vertices: list[tuple[float, float, float]],
    faces: list[tuple[int, int, int]],
    material_indices: list[int],
    center: tuple[float, float, float],
    half_size: tuple[float, float, float],
) -> None:
    start = len(vertices)
    cx, cy, cz = center
    hx, hy, hz = half_size
    vertices.extend(
        (cx + sx * hx, cy + sy * hy, cz + sz * hz)
        for sy in (-1.0, 1.0)
        for sx in (-1.0, 1.0)
        for sz in (-1.0, 1.0)
    )

    def index(vertical: int, lateral: int, forward: int) -> int:
        return start + vertical * 4 + lateral * 2 + forward

    quads = (
        ((1, 0, 0), (1, 0, 1), (1, 1, 1), (1, 1, 0), 0),
        ((0, 1, 0), (0, 1, 1), (0, 0, 1), (0, 0, 0), 1),
        ((0, 0, 0), (0, 0, 1), (1, 0, 1), (1, 0, 0), 1),
        ((0, 1, 1), (0, 1, 0), (1, 1, 0), (1, 1, 1), 1),
        ((0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1), 0),
        ((0, 1, 0), (0, 0, 0), (1, 0, 0), (1, 1, 0), 0),
    )
    for a, b, c, d, material in quads:
        ia, ib, ic, id_ = (index(*corner) for corner in (a, b, c, d))
        faces.extend(((ia, ib, ic), (ia, ic, id_)))
        material_indices.extend((material, material))


def add_support_prism(
    vertices: list[tuple[float, float, float]],
    faces: list[tuple[int, int, int]],
    material_indices: list[int],
    center_x: float,
) -> None:
    start = len(vertices)
    for x in (center_x - SUPPORT_HALF_WIDTH_M,
              center_x + SUPPORT_HALF_WIDTH_M):
        vertices.extend((
            (x, 0.015, HURDLE_CENTER_Z_M - SUPPORT_HALF_DEPTH_M),
            (x, 0.015, HURDLE_CENTER_Z_M + SUPPORT_HALF_DEPTH_M),
            (x, HURDLE_HEIGHT_M - 2.0 * BAR_HALF_HEIGHT_M,
             HURDLE_CENTER_Z_M),
        ))
    faces.extend((
        (start, start + 2, start + 1),
        (start + 3, start + 4, start + 5),
        (start, start + 3, start + 5),
        (start, start + 5, start + 2),
        (start + 1, start + 2, start + 5),
        (start + 1, start + 5, start + 4),
        (start, start + 1, start + 4),
        (start, start + 4, start + 3),
    ))
    material_indices.extend((1,) * 8)


def create_hurdle(root, materials):
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    material_indices: list[int] = []
    add_box(
        vertices,
        faces,
        material_indices,
        (0.0, HURDLE_HEIGHT_M - BAR_HALF_HEIGHT_M, HURDLE_CENTER_Z_M),
        (BAR_HALF_LENGTH_M, BAR_HALF_HEIGHT_M, BAR_HALF_DEPTH_M),
    )
    add_support_prism(vertices, faces, material_indices, -SUPPORT_CENTER_X_M)
    add_support_prism(vertices, faces, material_indices, SUPPORT_CENTER_X_M)

    mesh = bpy.data.meshes.new(name=MESH_NAME)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in vertices], [], faces
    )
    for material in materials:
        mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError("Blender repaired generated bunny-hop geometry")
    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False
    hurdle = bpy.data.objects.new(name=MESH_NAME, object_data=mesh)
    bpy.context.collection.objects.link(hurdle)
    hurdle.parent = root
    hurdle["physics_authority"] = "external"
    return hurdle, vertices


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.35
    root["asset_id"] = "WG-03-bunny-hop-greybox"
    root["asset_name"] = ASSET_NAME
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["socket_half_width_m"] = SOCKET_HALF_WIDTH_M
    root["physics_authority"] = "external"

    materials = [
        make_material(MAT_BAR, (0.88, 0.67, 0.25, 1.0)),
        make_material(MAT_SUPPORT, (0.24, 0.12, 0.045, 1.0)),
    ]
    hurdle, vertices = create_hurdle(root, materials)

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
    create_empty(root, "MARKER_PREPARE", (0.0, 0.0, 0.0), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_DECISION", (0.0, 0.0, 0.375),
                 0.18, marker_properties)
    create_empty(root, "MARKER_ACTION", (0.0, 0.0, 0.75), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_PRELOAD", (0.0, 0.0, 0.75), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_TAKEOFF", (0.0, 0.0, 1.20), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_APEX",
                 (0.0, HURDLE_HEIGHT_M, HURDLE_CENTER_Z_M),
                 0.22, marker_properties)
    create_empty(root, "MARKER_LAND",
                 (0.0, 0.0, 2.83),
                 0.22, marker_properties)
    return root, hurdle, vertices


def self_check(root, hurdle, vertices: Sequence[Sequence[float]]) -> None:
    if bpy.app.version[0] != 4:
        raise RuntimeError(f"Blender 4.x is required, found {bpy.app.version_string}")
    names = {obj.name for obj in bpy.context.scene.objects}
    if names != REQUIRED_NAMES:
        raise RuntimeError(
            f"Scene node mismatch; missing={sorted(REQUIRED_NAMES - names)}, "
            f"unexpected={sorted(names - REQUIRED_NAMES)}"
        )
    assert_close(SOCKET_HALF_WIDTH_M, 0.68, "Ordinary socket half-width")
    assert_close(TILE_LENGTH_M, 3.58, "Bunny-hop tile length")
    if HURDLE_HEIGHT_M >= 0.54:
        raise RuntimeError("Bunny-hop hurdle must remain lower than the log")
    if BAR_HALF_LENGTH_M <= SOCKET_HALF_WIDTH_M + 0.25:
        raise RuntimeError("Hurdle must extend beyond both trail edges")
    for point in vertices:
        assert_finite(point, "Non-finite canonical mesh coordinate")
    if hurdle.location.length > EPSILON \
            or any(abs(value) > EPSILON for value in hurdle.rotation_euler) \
            or any(abs(value - 1.0) > EPSILON for value in hurdle.scale):
        raise RuntimeError("Hurdle transform is not applied")
    if hurdle.get("physics_authority") != "external":
        raise RuntimeError("Render mesh claims physics authority")
    if any(polygon.loop_total != 3 or polygon.area <= EPSILON
           for polygon in hurdle.data.polygons):
        raise RuntimeError("Hurdle contains invalid triangles")
    if len(hurdle.data.vertices) != 20 or len(hurdle.data.polygons) != 28:
        raise RuntimeError("Unexpected bunny-hop topology")
    for socket_name, socket_z in (("SOCKET_IN", 0.0),
                                  ("SOCKET_OUT", TILE_LENGTH_M)):
        socket = bpy.data.objects[socket_name]
        assert_close(float(socket["socket_half_width_m"]), 0.68,
                     f"{socket_name} width")
        expected = canonical_to_blender((0.0, 0.0, socket_z))
        for actual, target in zip(socket.location, expected):
            assert_close(float(actual), target, f"{socket_name} location")
    if root.location.length > EPSILON \
            or any(abs(value) > EPSILON for value in root.rotation_euler) \
            or any(abs(value - 1.0) > EPSILON for value in root.scale):
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
    parser = argparse.ArgumentParser(description="Generate the bunny-hop GLB")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def main() -> None:
    output_path = Path(os.path.expanduser(parse_arguments().output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root, hurdle, vertices = build_scene()
    self_check(root, hurdle, vertices)
    export_glb(output_path)
    print(
        "Generated", output_path,
        f"({len(hurdle.data.vertices)} vertices, "
        f"{len(hurdle.data.polygons)} triangles)",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
