#!/usr/bin/env python3
"""Generate the deterministic three-line Workout Game gap-jump GLB."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import traceback
import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_tabletop import canonical_to_blender, create_empty, make_material


ASSET_NAME = "GapJumpThreeLine"
ROOT_NAME = "ROOT_GapJumpThreeLine"
GROUND_NAME = "GEO_GapJumpGround_LOD0"
TREAD_NAME = "GEO_GapJumpTread_LOD0"
ACCENT_NAME = "GEO_GapJumpAccents_LOD0"

SOCKET_HALF_WIDTH_M = 0.68
TAKEOFF_Z_M = 12.0
MERGE_START_Z_M = 22.7
MERGE_END_Z_M = 40.7
TREAD_HALF_WIDTH_M = 0.58
SHOULDER_WIDTH_M = 0.46

LINES = (
    ("SHORT", -2.3, 1.8, 0.48, 0.14, 3.0, 4.8),
    ("MEDIUM", 0.0, 3.2, 0.62, 0.22, 3.8, 5.6),
    ("LONG", 2.3, 4.7, 0.78, 0.32, 4.6, 6.4),
)

MAT_DIRT = "MAT_GapJumpPackedDirt"
MAT_EARTH = "MAT_GapJumpCutEarth"
MAT_FOREST = "MAT_GapJumpForestFloor"
EPSILON = 1.0e-7
SERIALIZED_FLOAT_EPSILON = 1.0e-6

REQUIRED_NAMES = {
    ROOT_NAME,
    GROUND_NAME,
    TREAD_NAME,
    ACCENT_NAME,
    "SOCKET_IN",
    "SOCKET_OUT",
    "MARKER_DECISION",
    "MARKER_MERGE_START",
    "MARKER_RECOVERY",
    *(f"MARKER_{line}_{marker}" for line, *_ in LINES
      for marker in ("LIP", "APEX", "LAND")),
}


def assert_close(actual: float, expected: float, message: str) -> None:
    if not math.isclose(actual, expected, rel_tol=0.0,
                        abs_tol=SERIALIZED_FLOAT_EPSILON):
        raise RuntimeError(f"{message}: expected {expected}, got {actual}")


def smoother(value: float) -> float:
    value = max(0.0, min(1.0, value))
    return value * value * value * (value * (value * 6.0 - 15.0) + 10.0)


def line_center(lateral: float, z_value: float) -> float:
    if z_value <= TAKEOFF_Z_M:
        return lateral * smoother((z_value - 4.0) / 7.0)
    if z_value < MERGE_START_Z_M:
        return lateral
    return lateral * (1.0 - smoother(
        (z_value - MERGE_START_Z_M) / (MERGE_END_Z_M - MERGE_START_Z_M)
    ))


def ground_height(x_value: float, z_value: float) -> float:
    if (abs(z_value) <= EPSILON
            or abs(z_value - MERGE_END_Z_M) <= EPSILON):
        return 0.0
    base = -0.16 + 0.055 * math.sin(0.47 * z_value + 0.31 * x_value)
    edge_relief = 0.11 * smoother((abs(x_value) - 3.8) / 2.0)
    bowl = 0.0
    for _, lateral, gap, *_ in LINES:
        if TAKEOFF_Z_M < z_value < TAKEOFF_Z_M + gap:
            longitudinal = math.sin(
                math.pi * (z_value - TAKEOFF_Z_M) / gap
            ) ** 2
            lateral_weight = max(0.0, 1.0 - abs(x_value - lateral) / 1.12)
            bowl = max(bowl, longitudinal * lateral_weight)
    return base + edge_relief - 0.42 * bowl


def make_mesh_object(root, name: str, materials, vertices, faces,
                     material_indices):
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in vertices], [], faces
    )
    for material in materials:
        mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError(f"Blender repaired generated geometry: {name}")
    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False
    obj = bpy.data.objects.new(name=name, object_data=mesh)
    bpy.context.collection.objects.link(obj)
    obj.parent = root
    obj["physics_authority"] = "external"
    obj["surface_provenance"] = "project-authored deterministic profile"
    return obj


def append_strip(vertices, faces, materials, rows) -> None:
    start = len(vertices)
    for center, height, z_value, half_width, shoulder in rows:
        if shoulder <= EPSILON:
            columns = tuple(
                (center + fraction * half_width, height)
                for fraction in (-1.0, -0.5, 0.0, 0.5, 1.0)
            )
        else:
            columns = (
                (center - half_width - shoulder,
                 ground_height(center - half_width - shoulder, z_value)),
                (center - half_width, height - 0.025),
                (center, height + 0.012),
                (center + half_width, height - 0.018),
                (center + half_width + shoulder,
                 ground_height(center + half_width + shoulder, z_value)),
            )
        vertices.extend((x_value, y_value, z_value)
                        for x_value, y_value in columns)
    for row in range(1, len(rows)):
        previous = start + (row - 1) * 5
        current = start + row * 5
        for column in range(4):
            faces.extend((
                (previous + column, current + column,
                 previous + column + 1),
                (previous + column + 1, current + column,
                 current + column + 1),
            ))
            material = 0 if column in (1, 2) else 1
            materials.extend((material, material))


def create_ground(root, materials):
    x_fractions = (-1.0, -0.833, -0.667, -0.517, -0.383, -0.25,
                   -0.125, 0.0, 0.125, 0.25, 0.383, 0.517, 0.667,
                   0.833, 1.0)
    z_values = sorted({
        0.0, 2.0, 4.0, 6.0, 8.0, 10.0, 11.4, TAKEOFF_Z_M,
        MERGE_START_Z_M, 26.0, 30.0, 34.0, 38.0, MERGE_END_Z_M,
        *(TAKEOFF_Z_M + gap * fraction
          for _, _, gap, *_ in LINES for fraction in (0.25, 0.5, 0.75, 1.0)),
    })
    vertices = []
    socket_ground_half_width = SOCKET_HALF_WIDTH_M
    for z_value in z_values:
        entry_blend = smoother(z_value / 6.0)
        exit_blend = 1.0 - smoother((z_value - 34.7) / 6.0)
        half_width = socket_ground_half_width + (
            6.0 - socket_ground_half_width
        ) * min(entry_blend, exit_blend)
        for fraction in x_fractions:
            x_value = fraction * half_width
            vertices.append(
                (x_value, ground_height(x_value, z_value), z_value)
            )
    faces = []
    material_indices = []
    columns = len(x_fractions)
    for row in range(len(z_values) - 1):
        for column in range(columns - 1):
            lower = row * columns + column
            upper = lower + columns
            faces.extend(((lower, upper, lower + 1),
                          (lower + 1, upper, upper + 1)))
            material_indices.extend((2, 2))
    return make_mesh_object(root, GROUND_NAME, materials, vertices, faces,
                            material_indices), vertices


def create_tread(root, materials):
    vertices = []
    faces = []
    material_indices = []

    approach_rows = []
    for z_value in (0.0, 1.5, 3.0, 4.5, 6.0, 7.0, 8.0):
        half_width = SOCKET_HALF_WIDTH_M + 2.32 * smoother(z_value / 8.0)
        approach_rows.append((0.0, 0.0 if z_value == 0.0 else 0.012,
                              z_value, half_width,
                              SHOULDER_WIDTH_M * smoother(z_value / 8.0)))
    append_strip(vertices, faces, material_indices, approach_rows)

    segment_ranges = []
    for line_index, (line_name, lateral, gap, lip_height, landing_drop,
                     takeoff_run, landing_run) in enumerate(LINES):
        takeoff_rows = []
        start = TAKEOFF_Z_M - takeoff_run
        for z_value in sorted({8.0, start, start + takeoff_run * 0.25,
                               start + takeoff_run * 0.5,
                               start + takeoff_run * 0.75, TAKEOFF_Z_M}):
            if z_value < 8.0 - EPSILON:
                continue
            phase = smoother((z_value - start) / takeoff_run)
            takeoff_rows.append((line_center(lateral, z_value),
                                 lip_height * phase, z_value,
                                 TREAD_HALF_WIDTH_M, SHOULDER_WIDTH_M))
        append_strip(vertices, faces, material_indices, takeoff_rows)

        landing_z = TAKEOFF_Z_M + gap
        landing_height = max(0.18, lip_height - landing_drop)
        landing_rows = []
        for z_value in (landing_z, landing_z + landing_run * 0.2,
                        landing_z + landing_run * 0.45,
                        landing_z + landing_run * 0.72,
                        landing_z + landing_run, MERGE_START_Z_M):
            if z_value > MERGE_START_Z_M + EPSILON:
                continue
            phase = max(0.0, min(1.0, (z_value - landing_z) / landing_run))
            height = landing_height * (1.0 - smoother(phase))
            landing_rows.append((lateral, height, z_value,
                                 0.76 + 0.08 * line_index,
                                 SHOULDER_WIDTH_M))
        if landing_rows[-1][2] < MERGE_START_Z_M - EPSILON:
            landing_rows.append((lateral, 0.0, MERGE_START_Z_M,
                                 TREAD_HALF_WIDTH_M, SHOULDER_WIDTH_M))
        append_strip(vertices, faces, material_indices, landing_rows)
        segment_ranges.append((line_name, TAKEOFF_Z_M, landing_z))

    merge_rows = []
    for z_value in (MERGE_START_Z_M, 25.0, 28.0, 31.0, 34.0, 36.5):
        half_width = 3.35 * (1.0 - smoother(
            (z_value - MERGE_START_Z_M) / (36.5 - MERGE_START_Z_M)
        )) + SOCKET_HALF_WIDTH_M * smoother(
            (z_value - MERGE_START_Z_M) / (36.5 - MERGE_START_Z_M)
        )
        merge_rows.append((0.0, 0.008, z_value, half_width,
                           SHOULDER_WIDTH_M))
    append_strip(vertices, faces, material_indices, merge_rows)
    append_strip(vertices, faces, material_indices, (
        (0.0, 0.008, 36.5, SOCKET_HALF_WIDTH_M, SHOULDER_WIDTH_M),
        (0.0, 0.008, 38.6, SOCKET_HALF_WIDTH_M, 0.22),
        (0.0, 0.0, MERGE_END_Z_M, SOCKET_HALF_WIDTH_M, 0.0),
    ))

    return (make_mesh_object(root, TREAD_NAME, materials, vertices, faces,
                             material_indices), vertices, segment_ranges)


def append_face(vertices, faces, materials, lateral, z_value,
                top_height, bottom_height, half_width, landing=False):
    start = len(vertices)
    bottom_z = z_value - 0.13 if landing else z_value + 0.13
    x_values = (lateral - half_width - 0.22, lateral - half_width,
                lateral, lateral + half_width,
                lateral + half_width + 0.22)
    crown = (top_height - 0.12, top_height - 0.025,
             top_height + 0.012, top_height - 0.02,
             top_height - 0.13)
    for x_value, y_value in zip(x_values, crown):
        vertices.append((x_value, y_value, z_value))
    for x_value in x_values:
        vertices.append((x_value, bottom_height, bottom_z))
    for index in range(4):
        faces.extend(((start + index, start + 5 + index,
                       start + index + 1),
                      (start + index + 1, start + 5 + index,
                       start + 6 + index)))
        materials.extend((1, 1))


def append_marker_tab(vertices, faces, materials, center_x, z_value, notches):
    width = 0.22 + 0.08 * notches
    height = 0.20 + 0.06 * notches
    depth = 0.10
    start = len(vertices)
    for y_value in (0.0, height):
        for x_value in (center_x - width, center_x + width):
            for z_offset in (-depth, depth):
                vertices.append((x_value, y_value, z_value + z_offset))
    quads = ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1),
             (2, 3, 7, 6), (1, 5, 7, 3), (0, 2, 6, 4))
    for a, b, c, d in quads:
        faces.extend(((start + a, start + b, start + c),
                      (start + a, start + c, start + d)))
        materials.extend((1, 1))


def create_accents(root, materials):
    vertices = []
    faces = []
    material_indices = []
    for index, (_, lateral, gap, lip_height, landing_drop, _, _) in enumerate(LINES):
        landing_height = max(0.18, lip_height - landing_drop)
        append_face(vertices, faces, material_indices, lateral, TAKEOFF_Z_M,
                    lip_height, ground_height(lateral, TAKEOFF_Z_M + 0.1),
                    TREAD_HALF_WIDTH_M)
        append_face(vertices, faces, material_indices, lateral,
                    TAKEOFF_Z_M + gap, landing_height,
                    ground_height(lateral, TAKEOFF_Z_M + gap - 0.1),
                    0.76 + 0.08 * index, landing=True)
        tab_z = 6.3 + 0.35 * index
        tab_x = line_center(lateral, tab_z) + (0.95 if lateral >= 0.0 else -0.95)
        append_marker_tab(vertices, faces, material_indices, tab_x, tab_z,
                          index + 1)
    return make_mesh_object(root, ACCENT_NAME, materials, vertices, faces,
                            material_indices), vertices


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.45
    root["asset_id"] = "FT-12-gap-jump-three-line"
    root["asset_name"] = ASSET_NAME
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["socket_half_width_m"] = SOCKET_HALF_WIDTH_M
    root["tile_length_m"] = MERGE_END_Z_M
    root["physics_authority"] = "external"
    root["profile_contract"] = "WorkoutGameGapJumpGeometry difficulty=0.5"
    for line_name, lateral, gap, lip_height, landing_drop, *_ in LINES:
        prefix = line_name.lower()
        root[f"{prefix}_lateral_m"] = lateral
        root[f"{prefix}_gap_length_m"] = gap
        root[f"{prefix}_lip_height_m"] = lip_height
        root[f"{prefix}_landing_drop_m"] = landing_drop

    materials = [
        make_material(MAT_DIRT, (0.568, 0.361, 0.204, 1.0)),
        make_material(MAT_EARTH, (0.420, 0.294, 0.192, 1.0)),
        make_material(MAT_FOREST, (0.149, 0.333, 0.239, 1.0)),
    ]
    ground, ground_vertices = create_ground(root, materials)
    tread, tread_vertices, gaps = create_tread(root, materials)
    accents, accent_vertices = create_accents(root, materials)

    socket_properties = {
        "socket_half_width_m": SOCKET_HALF_WIDTH_M,
        "surface_class": "ordinary-trail",
        "grade_percent": 0.0,
        "visual_only": True,
    }
    create_empty(root, "SOCKET_IN", (0.0, 0.0, 0.0), 0.28,
                 {**socket_properties, "socket_role": "in"})
    create_empty(root, "SOCKET_OUT", (0.0, 0.0, MERGE_END_Z_M), 0.28,
                 {**socket_properties, "socket_role": "out"})
    marker_properties = {"visual_only": True,
                         "physics_authority": "external"}
    create_empty(root, "MARKER_DECISION", (0.0, 0.0, 9.0), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_MERGE_START",
                 (0.0, 0.0, MERGE_START_Z_M), 0.18, marker_properties)
    create_empty(root, "MARKER_RECOVERY", (0.0, 0.0, 36.5), 0.18,
                 marker_properties)
    for line_name, lateral, gap, lip_height, landing_drop, *_ in LINES:
        landing_height = max(0.18, lip_height - landing_drop)
        create_empty(root, f"MARKER_{line_name}_LIP",
                     (lateral, lip_height, TAKEOFF_Z_M), 0.16,
                     marker_properties)
        create_empty(root, f"MARKER_{line_name}_APEX",
                     (lateral, lip_height + 0.65 + gap * 0.08,
                      TAKEOFF_Z_M + gap * 0.52), 0.16, marker_properties)
        create_empty(root, f"MARKER_{line_name}_LAND",
                     (lateral, landing_height, TAKEOFF_Z_M + gap), 0.16,
                     marker_properties)
    return root, (ground, tread, accents), (
        ground_vertices, tread_vertices, accent_vertices), gaps


def self_check(root, objects, vertex_sets, gaps) -> None:
    if bpy.app.version[:3] != (4, 0, 2):
        raise RuntimeError(
            f"Blender 4.0.2 is required, found {bpy.app.version_string}"
        )
    names = {obj.name for obj in bpy.context.scene.objects}
    if names != REQUIRED_NAMES:
        raise RuntimeError(
            f"Scene node mismatch; missing={sorted(REQUIRED_NAMES - names)}, "
            f"unexpected={sorted(names - REQUIRED_NAMES)}"
        )
    assert_close(MERGE_END_Z_M, 40.7, "Runtime socket length")
    if tuple(gap for _, _, gap, *_ in LINES) != (1.8, 3.2, 4.7):
        raise RuntimeError("Gap progression changed")
    for vertex_set in vertex_sets:
        for point in vertex_set:
            if not all(math.isfinite(value) for value in point):
                raise RuntimeError("Non-finite canonical mesh coordinate")
    triangles = 0
    for obj in objects:
        if (obj.location.length > EPSILON
                or any(abs(value) > EPSILON for value in obj.rotation_euler)
                or any(abs(value - 1.0) > EPSILON for value in obj.scale)):
            raise RuntimeError(f"Mesh transform is not applied: {obj.name}")
        if obj.get("physics_authority") != "external":
            raise RuntimeError("Render mesh claims physics authority")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in obj.data.polygons):
            raise RuntimeError(f"Invalid triangle in {obj.name}")
        triangles += len(obj.data.polygons)
    if triangles > 3600:
        raise RuntimeError(f"LOD0 triangle budget exceeded: {triangles}")
    for line_name, lip_z, landing_z in gaps:
        if landing_z <= lip_z:
            raise RuntimeError(f"Closed or reversed gap: {line_name}")
    for socket_name, socket_z in (("SOCKET_IN", 0.0),
                                  ("SOCKET_OUT", MERGE_END_Z_M)):
        socket = bpy.data.objects[socket_name]
        expected = canonical_to_blender((0.0, 0.0, socket_z))
        for actual, target in zip(socket.location, expected):
            assert_close(float(actual), target, f"{socket_name} location")
        assert_close(float(socket["socket_half_width_m"]),
                     SOCKET_HALF_WIDTH_M, f"{socket_name} width")
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
        filepath=str(output_path), check_existing=False, export_format="GLB",
        use_selection=True, export_yup=True, export_extras=True,
        export_cameras=False, export_lights=False, export_animations=False,
    )
    if not output_path.is_file() or output_path.stat().st_size <= 0:
        raise RuntimeError(f"GLB export did not produce {output_path}")


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Generate three-line gap jump")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def main() -> None:
    output_path = Path(os.path.expanduser(parse_arguments().output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root, objects, vertex_sets, gaps = build_scene()
    self_check(root, objects, vertex_sets, gaps)
    export_glb(output_path)
    print("Generated", output_path,
          f"({sum(len(obj.data.polygons) for obj in objects)} triangles)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
