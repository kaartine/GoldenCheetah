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
TERRAIN_MESH_NAME = "GEO_LogOverTile_LOD0"
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
BYPASS_HALF_WIDTH_M = 0.42
BYPASS_OFFSET_M = 1.68
BYPASS_RISE_M = 0.02
RADIAL_SEGMENTS = 16

MAT_BYPASS = "MAT_LogOverBypass_Grey"
MAT_BARK = "MAT_LogOverBark_Grey"
MAT_END = "MAT_LogOverEndGrain_Grey"

REQUIRED_NAMES = {
    ROOT_NAME,
    TERRAIN_MESH_NAME,
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


def profile_sections() -> list[float]:
    sections = [0.0, DEAD_ZONE_M]
    for segment in range(RADIAL_SEGMENTS // 2 + 1):
        angle = math.pi - segment * 2.0 * math.pi / RADIAL_SEGMENTS
        sections.append(LOG_CENTER_Z_M + math.cos(angle) * LOG_RADIUS_Z_M)
    sections.extend((TILE_LENGTH_M - DEAD_ZONE_M, TILE_LENGTH_M))
    return sorted(set(sections))


def smooth_step(progress: float) -> float:
    amount = min(1.0, max(0.0, progress))
    return amount * amount * (3.0 - 2.0 * amount)


def bypass_center_x(z_forward: float) -> float:
    decision_z = DEAD_ZONE_M * 0.5
    split_z = DEAD_ZONE_M
    merge_z = TILE_LENGTH_M - DEAD_ZONE_M * 0.5
    join_z = TILE_LENGTH_M - DEAD_ZONE_M
    if z_forward <= decision_z or z_forward >= merge_z:
        return 0.0
    if z_forward < split_z:
        return BYPASS_OFFSET_M * smooth_step(
            (z_forward - decision_z) / (split_z - decision_z)
        )
    if z_forward <= join_z:
        return BYPASS_OFFSET_M
    return BYPASS_OFFSET_M * (1.0 - smooth_step(
        (z_forward - join_z) / (merge_z - join_z)
    ))


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


def build_tile(root, materials):
    sections = profile_sections()
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    material_indices: list[int] = []
    bypass_sections = sorted(set(
        [DEAD_ZONE_M * 0.5, DEAD_ZONE_M,
         TILE_LENGTH_M - DEAD_ZONE_M,
         TILE_LENGTH_M - DEAD_ZONE_M * 0.5]
        + [value for value in sections
           if DEAD_ZONE_M * 0.5 < value
           < TILE_LENGTH_M - DEAD_ZONE_M * 0.5]
    ))
    bypass_rows: list[tuple[int, int]] = []
    for z_forward in bypass_sections:
        center = bypass_center_x(z_forward)
        base = len(vertices)
        vertices.extend(
            (
                (center - BYPASS_HALF_WIDTH_M, BYPASS_RISE_M, z_forward),
                (center + BYPASS_HALF_WIDTH_M, BYPASS_RISE_M, z_forward),
            )
        )
        bypass_rows.append((base, base + 1))
    for previous, current in zip(bypass_rows, bypass_rows[1:]):
        add_quad(
            faces,
            material_indices,
            previous[0], previous[1], current[0], current[1], 0,
        )
    return create_mesh_object(
        root, TERRAIN_MESH_NAME, vertices, faces, material_indices,
        [materials[0]]
    ), vertices


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
        root, LOG_MESH_NAME, vertices, faces, material_indices, materials[1:]
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
        make_material(MAT_BYPASS, (0.47, 0.34, 0.16, 1.0)),
        make_material(MAT_BARK, (0.28, 0.14, 0.055, 1.0)),
        make_material(MAT_END, (0.52, 0.32, 0.13, 1.0)),
    ]
    tile, tile_vertices = build_tile(root, materials)
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
    return root, tile, log, tile_vertices, log_vertices


def self_check(root, tile, log, tile_vertices, log_vertices) -> None:
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
    for point in [*tile_vertices, *log_vertices]:
        assert_finite(point, "Non-finite canonical mesh coordinate")
    for mesh_object in (tile, log):
        if mesh_object.location.length > EPSILON \
                or any(abs(value) > EPSILON for value in mesh_object.rotation_euler) \
                or any(abs(value - 1.0) > EPSILON for value in mesh_object.scale):
            raise RuntimeError(f"{mesh_object.name} transform is not applied")
        if mesh_object.get("physics_authority") != "external":
            raise RuntimeError("Render mesh claims physics authority")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in mesh_object.data.polygons):
            raise RuntimeError(f"{mesh_object.name} has invalid triangles")
    if len(tile.data.polygons) > 160 or len(log.data.polygons) != 64:
        raise RuntimeError("Unexpected log-over topology")
    for socket_name, socket_z in (("SOCKET_IN", 0.0),
                                  ("SOCKET_OUT", TILE_LENGTH_M)):
        socket = bpy.data.objects[socket_name]
        assert_close(float(socket["socket_half_width_m"]), 0.68,
                     f"{socket_name} width")
        expected = canonical_to_blender((0.0, 0.0, socket_z))
        for actual, target in zip(socket.location, expected):
            assert_close(float(actual), target, f"{socket_name} location")
    bypass_min_z = min(point[2] for point in tile_vertices)
    bypass_max_z = max(point[2] for point in tile_vertices)
    assert_close(bypass_min_z, DEAD_ZONE_M * 0.5,
                 "Bypass visual entry")
    assert_close(bypass_max_z, TILE_LENGTH_M - DEAD_ZONE_M * 0.5,
                 "Bypass visual exit")
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
    root, tile, log, tile_vertices, log_vertices = build_scene()
    self_check(root, tile, log, tile_vertices, log_vertices)
    export_glb(output_path)
    print(
        "Generated", output_path,
        f"({len(tile.data.vertices) + len(log.data.vertices)} vertices, "
        f"{len(tile.data.polygons) + len(log.data.polygons)} triangles)",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
