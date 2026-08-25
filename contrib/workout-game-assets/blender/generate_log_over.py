#!/usr/bin/env python3
"""Generate the socketed Workout Game log-over feature as a GLB."""

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


ASSET_NAME = "LogOver_Greybox"
ROOT_NAME = "ROOT_LogOver"
LOG_MESH_NAME = "GEO_LogOverObstacle_LOD0"

SOCKET_HALF_WIDTH_M = 0.68
DEAD_ZONE_M = 0.75
LOG_RADIUS_Z_M = 0.27
LOG_HEIGHT_M = 0.54
LOG_BURY_DEPTH_M = 0.07
LOG_HALF_LENGTH_M = 1.12
CORE_LENGTH_M = LOG_RADIUS_Z_M * 2.0
TILE_LENGTH_M = DEAD_ZONE_M * 2.0 + CORE_LENGTH_M
LOG_CENTER_Z_M = TILE_LENGTH_M * 0.5
RADIAL_SEGMENTS = 16

MAT_BARK = "MAT_LogOverBark_Grey"
MAT_END = "MAT_LogOverEndGrain_Grey"

REQUIRED_NAMES = {
    ROOT_NAME,
    LOG_MESH_NAME,
    "SOCKET_IN",
    "SOCKET_OUT",
    "MARKER_PREPARE",
    "MARKER_DECISION",
    "MARKER_ACTION",
    "MARKER_APEX",
    "MARKER_LAND",
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


def log_surface_height(z_forward: float) -> float:
    local = z_forward - LOG_CENTER_Z_M
    if local <= -LOG_RADIUS_Z_M or local >= LOG_RADIUS_Z_M:
        return 0.0
    for segment in range(RADIAL_SEGMENTS // 2):
        from_angle = math.pi - segment * 2.0 * math.pi / RADIAL_SEGMENTS
        to_angle = math.pi - (segment + 1) * 2.0 * math.pi / RADIAL_SEGMENTS
        from_z = math.cos(from_angle) * LOG_RADIUS_Z_M
        to_z = math.cos(to_angle) * LOG_RADIUS_Z_M
        if local <= to_z + 1.0e-12:
            amount = min(1.0, max(0.0, (local - from_z) / (to_z - from_z)))
            from_y = math.sin(from_angle) * LOG_HEIGHT_M
            to_y = math.sin(to_angle) * LOG_HEIGHT_M
            return from_y + (to_y - from_y) * amount
    return LOG_HEIGHT_M * 0.5


def add_quad(
    faces: list[tuple[int, int, int]],
    materials: list[int],
    previous_left: int,
    previous_right: int,
    current_left: int,
    current_right: int,
    material: int,
) -> None:
    faces.extend(
        (
            (previous_left, current_left, current_right),
            (previous_left, current_right, previous_right),
        )
    )
    materials.extend((material, material))


def create_mesh_object(
    root,
    name: str,
    canonical_vertices: Sequence[Sequence[float]],
    faces: Sequence[Sequence[int]],
    material_indices: Sequence[int],
    materials: Sequence[object],
):
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in canonical_vertices],
        [],
        faces,
    )
    for material in materials:
        mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError(f"Blender repaired generated geometry in {name}")
    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False
    result = bpy.data.objects.new(name=name, object_data=mesh)
    bpy.context.collection.objects.link(result)
    result.parent = root
    result["physics_authority"] = "external"
    return result


def build_log(root, materials):
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    material_indices: list[int] = []
    for side in (-1.0, 1.0):
        for segment in range(RADIAL_SEGMENTS):
            angle = segment * 2.0 * math.pi / RADIAL_SEGMENTS
            sine = math.sin(angle)
            vertices.append(
                (
                    side * LOG_HALF_LENGTH_M,
                    sine * (LOG_HEIGHT_M if sine >= 0.0
                            else LOG_BURY_DEPTH_M),
                    LOG_CENTER_Z_M + math.cos(angle) * LOG_RADIUS_Z_M,
                )
            )
    for segment in range(RADIAL_SEGMENTS):
        next_segment = (segment + 1) % RADIAL_SEGMENTS
        add_quad(
            faces,
            material_indices,
            segment,
            next_segment,
            RADIAL_SEGMENTS + segment,
            RADIAL_SEGMENTS + next_segment,
            0,
        )
    for side_index, reverse in ((0, True), (1, False)):
        center = len(vertices)
        side = -1.0 if side_index == 0 else 1.0
        vertices.append((side * LOG_HALF_LENGTH_M, 0.0, LOG_CENTER_Z_M))
        base = side_index * RADIAL_SEGMENTS
        for segment in range(RADIAL_SEGMENTS):
            next_segment = (segment + 1) % RADIAL_SEGMENTS
            triangle = (center, base + segment, base + next_segment)
            faces.append(tuple(reversed(triangle)) if reverse else triangle)
            material_indices.append(1)
    return create_mesh_object(
        root, LOG_MESH_NAME, vertices, faces, material_indices, materials
    ), vertices


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.35
    root["asset_id"] = "WG-02-log-over-greybox"
    root["asset_name"] = ASSET_NAME
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["socket_half_width_m"] = SOCKET_HALF_WIDTH_M
    root["physics_authority"] = "external"

    materials = [
        make_material(MAT_BARK, (0.28, 0.14, 0.055, 1.0)),
        make_material(MAT_END, (0.52, 0.32, 0.13, 1.0)),
    ]
    log, log_vertices = build_log(root, materials)

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
    create_empty(root, "MARKER_DECISION", (0.0, 0.0, DEAD_ZONE_M * 0.5),
                 0.18, marker_properties)
    create_empty(root, "MARKER_ACTION", (0.0, 0.0, DEAD_ZONE_M), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_APEX",
                 (0.0, LOG_HEIGHT_M, LOG_CENTER_Z_M),
                 0.22, marker_properties)
    create_empty(root, "MARKER_LAND",
                 (0.0, 0.0, TILE_LENGTH_M - DEAD_ZONE_M),
                 0.22, marker_properties)
    return root, log, log_vertices


def self_check(root, log, log_vertices) -> None:
    if bpy.app.version[0] != 4:
        raise RuntimeError(f"Blender 4.x is required, found {bpy.app.version_string}")
    names = {obj.name for obj in bpy.context.scene.objects}
    if names != REQUIRED_NAMES:
        raise RuntimeError(
            f"Scene node mismatch; missing={sorted(REQUIRED_NAMES - names)}, "
            f"unexpected={sorted(names - REQUIRED_NAMES)}"
        )
    assert_close(SOCKET_HALF_WIDTH_M, 0.68, "Ordinary socket half-width")
    assert_close(log_surface_height(DEAD_ZONE_M), 0.0, "Entry dead zone")
    assert_close(log_surface_height(TILE_LENGTH_M - DEAD_ZONE_M), 0.0,
                 "Exit dead zone")
    assert_close(log_surface_height(LOG_CENTER_Z_M), LOG_HEIGHT_M,
                 "Obstacle crest")
    if LOG_BURY_DEPTH_M <= 0.0:
        raise RuntimeError("The log must remain visibly buried in the tread")
    if LOG_HALF_LENGTH_M <= SOCKET_HALF_WIDTH_M + 0.30:
        raise RuntimeError("The log must extend clearly beyond both trail edges")
    for point in log_vertices:
        assert_finite(point, "Non-finite canonical mesh coordinate")
    for mesh_object in (log,):
        if mesh_object.location.length > EPSILON \
                or any(abs(value) > EPSILON for value in mesh_object.rotation_euler) \
                or any(abs(value - 1.0) > EPSILON for value in mesh_object.scale):
            raise RuntimeError(f"{mesh_object.name} transform is not applied")
        if mesh_object.get("physics_authority") != "external":
            raise RuntimeError("Render mesh claims physics authority")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in mesh_object.data.polygons):
            raise RuntimeError(f"{mesh_object.name} has invalid triangles")
    if len(log.data.polygons) != 64:
        raise RuntimeError("Unexpected log-over topology")
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
    parser = argparse.ArgumentParser(description="Generate the log-over GLB")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def main() -> None:
    output_path = Path(os.path.expanduser(parse_arguments().output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root, log, log_vertices = build_scene()
    self_check(root, log, log_vertices)
    export_glb(output_path)
    print(
        "Generated", output_path,
        f"({len(log.data.vertices)} vertices, "
        f"{len(log.data.polygons)} triangles)",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
