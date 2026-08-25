#!/usr/bin/env python3
"""Generate the first custom Workout Game tabletop greybox as a GLB.

Run this file with Blender 4.x. Canonical asset coordinates are metres with
+Y up and +Z forward. Blender's glTF exporter maps the generated Blender-space
coordinates to that canonical convention.
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import traceback
from typing import Iterable, Sequence

import bpy


ASSET_NAME = "Tabletop_Greybox"
ROOT_NAME = "ROOT_Tabletop"
MESH_NAME = "GEO_Tabletop_LOD0"

SOCKET_HALF_WIDTH_M = 0.68
DEAD_ZONE_M = 0.75
HEIGHT_M = 0.446
TAKEOFF_RUN_M = 1.87
DECK_LENGTH_M = 1.10
LANDING_RUN_M = 1.87
CORE_LENGTH_M = TAKEOFF_RUN_M + DECK_LENGTH_M + LANDING_RUN_M
TILE_LENGTH_M = DEAD_ZONE_M * 2.0 + CORE_LENGTH_M
TERRAIN_HALF_WIDTH_M = 4.0
SKIRT_BOTTOM_Y_M = -0.60

MAT_TRAIL = "MAT_TabletopTrail_Grey"
MAT_TERRAIN = "MAT_TabletopTerrain_Grey"
MAT_SKIRT = "MAT_TabletopSkirt_Grey"
MAT_BYPASS = "MAT_TabletopBypass_Grey"

BYPASS_HALF_WIDTH_M = 0.44
BYPASS_OFFSET_M = 1.78
BYPASS_RISE_M = 0.025

REQUIRED_NAMES = {
    ROOT_NAME,
    MESH_NAME,
    "SOCKET_IN",
    "SOCKET_OUT",
    "MARKER_PREPARE",
    "MARKER_DECISION",
    "MARKER_ACTION",
    "MARKER_LIP",
    "MARKER_APEX",
    "MARKER_LAND",
}

EPSILON = 1.0e-7
SERIALIZED_FLOAT_EPSILON = 1.0e-6


def canonical_to_blender(point: Sequence[float]) -> tuple[float, float, float]:
    """Map canonical (X, Y-up, Z-forward) to Blender (X, Y, Z-up)."""
    x_value, y_up, z_forward = point
    return (float(x_value), -float(z_forward), float(y_up))


def ramp_rise(progress_value: float) -> float:
    """Match the calibrated eased-entry, planar-ramp tabletop profile."""
    progress = min(1.0, max(0.0, float(progress_value)))
    transition = 1.0 / 6.0
    transition_height = transition * 0.375
    linear_slope = (1.0 - transition_height) / (1.0 - transition)
    if progress >= transition:
        return transition_height + (progress - transition) * linear_slope

    amount = progress / transition
    amount2 = amount * amount
    amount3 = amount2 * amount
    value = (-2.0 * amount3 + 3.0 * amount2) * transition_height
    value += (amount3 - amount2) * transition * linear_slope
    return min(transition_height, max(0.0, value))


def surface_height(z_forward: float) -> float:
    core_start = DEAD_ZONE_M
    takeoff_end = core_start + TAKEOFF_RUN_M
    deck_end = takeoff_end + DECK_LENGTH_M
    core_end = deck_end + LANDING_RUN_M

    if z_forward <= core_start or z_forward >= core_end:
        return 0.0
    if z_forward < takeoff_end:
        return HEIGHT_M * ramp_rise(
            (z_forward - core_start) / TAKEOFF_RUN_M
        )
    if z_forward <= deck_end:
        return HEIGHT_M
    return HEIGHT_M * ramp_rise(
        1.0 - (z_forward - deck_end) / LANDING_RUN_M
    )


def profile_sections() -> list[float]:
    takeoff_start = DEAD_ZONE_M
    takeoff_end = takeoff_start + TAKEOFF_RUN_M
    deck_end = takeoff_end + DECK_LENGTH_M
    landing_end = deck_end + LANDING_RUN_M
    fractions = (1.0 / 6.0, 0.5, 5.0 / 6.0)

    sections = [0.0, takeoff_start]
    sections.extend(takeoff_start + TAKEOFF_RUN_M * value for value in fractions)
    sections.append(takeoff_end)
    sections.append((takeoff_end + deck_end) * 0.5)
    sections.append(deck_end)
    sections.extend(deck_end + LANDING_RUN_M * value for value in fractions)
    sections.extend((landing_end, TILE_LENGTH_M))
    return sections


def outer_terrain_height(z_forward: float, left: bool) -> float:
    core_progress = (z_forward - DEAD_ZONE_M) / CORE_LENGTH_M
    if core_progress <= 0.0 or core_progress >= 1.0:
        return 0.0
    envelope = 4.0 * core_progress * (1.0 - core_progress)
    return (0.10 if left else -0.06) * envelope


def bypass_center_x(z_forward: float) -> float:
    decision_z = DEAD_ZONE_M * 0.5
    landing_z = DEAD_ZONE_M + CORE_LENGTH_M
    merge_z = landing_z + DEAD_ZONE_M * 0.5
    if z_forward <= decision_z or z_forward >= merge_z:
        return 0.0
    if z_forward < DEAD_ZONE_M:
        progress = (z_forward - decision_z) / (DEAD_ZONE_M - decision_z)
        return BYPASS_OFFSET_M * progress * progress * (3.0 - 2.0 * progress)
    if z_forward <= landing_z:
        return BYPASS_OFFSET_M
    progress = (z_forward - landing_z) / (merge_z - landing_z)
    return BYPASS_OFFSET_M * (1.0 - progress * progress * (3.0 - 2.0 * progress))


def terrain_half_width(z_forward: float) -> float:
    progress = min(1.0, max(0.0, z_forward / TILE_LENGTH_M))
    envelope = math.sin(math.pi * progress) ** 2
    tapered = SOCKET_HALF_WIDTH_M + 0.06 \
        + (TERRAIN_HALF_WIDTH_M - SOCKET_HALF_WIDTH_M - 0.06) * envelope
    decision_z = DEAD_ZONE_M * 0.5
    merge_z = DEAD_ZONE_M + CORE_LENGTH_M + DEAD_ZONE_M * 0.5
    if decision_z <= z_forward <= merge_z:
        tapered = max(
            tapered,
            bypass_center_x(z_forward) + BYPASS_HALF_WIDTH_M + 0.35,
        )
    return tapered


def add_triangle(
    faces: list[tuple[int, int, int]],
    material_indices: list[int],
    first: int,
    second: int,
    third: int,
    material_index: int,
) -> None:
    faces.append((first, second, third))
    material_indices.append(material_index)


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


def build_mesh(root) -> tuple[object, list[tuple[float, float, float]]]:
    sections = profile_sections()
    canonical_vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    material_indices: list[int] = []

    # Four joined top vertices per section: outer-left, trail-left,
    # trail-right and outer-right. The trail and terrain therefore cannot gap.
    for z_forward in sections:
        trail_y = surface_height(z_forward)
        outer_width = terrain_half_width(z_forward)
        canonical_vertices.extend(
            (
                (-outer_width,
                 outer_terrain_height(z_forward, True), z_forward),
                (-SOCKET_HALF_WIDTH_M, trail_y, z_forward),
                (SOCKET_HALF_WIDTH_M, trail_y, z_forward),
                (outer_width,
                 outer_terrain_height(z_forward, False), z_forward),
            )
        )

    row_width = 4
    for row in range(len(sections) - 1):
        previous = row * row_width
        current = (row + 1) * row_width
        for strip in range(3):
            material_index = 0 if strip == 1 else 1
            previous_left = previous + strip
            previous_right = previous + strip + 1
            current_left = current + strip
            current_right = current + strip + 1
            add_triangle(
                faces, material_indices,
                previous_left, current_left, current_right, material_index,
            )
            add_triangle(
                faces, material_indices,
                previous_left, current_right, previous_right, material_index,
            )

    left_bottom: list[int] = []
    right_bottom: list[int] = []
    for z_forward in sections:
        outer_width = terrain_half_width(z_forward)
        left_bottom.append(len(canonical_vertices))
        canonical_vertices.append(
            (-outer_width, SKIRT_BOTTOM_Y_M, z_forward)
        )
        right_bottom.append(len(canonical_vertices))
        canonical_vertices.append(
            (outer_width, SKIRT_BOTTOM_Y_M, z_forward)
        )

    for row in range(len(sections) - 1):
        previous_top = row * row_width
        current_top = (row + 1) * row_width

        add_triangle(
            faces, material_indices,
            previous_top, left_bottom[row], left_bottom[row + 1], 2,
        )
        add_triangle(
            faces, material_indices,
            previous_top, left_bottom[row + 1], current_top, 2,
        )

        previous_right_top = previous_top + 3
        current_right_top = current_top + 3
        add_triangle(
            faces, material_indices,
            previous_right_top, current_right_top, right_bottom[row + 1], 2,
        )
        add_triangle(
            faces, material_indices,
            previous_right_top, right_bottom[row + 1], right_bottom[row], 2,
        )

    # Close the full front and back below the socket seam. These faces are
    # hidden when tiles join, but prevent the standalone greybox exposing
    # background below either the trail or its joined side terrain.
    for front in (True, False):
        row = 0 if front else len(sections) - 1
        top = row * row_width
        bottom = [left_bottom[row]]
        for x_value in (-SOCKET_HALF_WIDTH_M, SOCKET_HALF_WIDTH_M):
            bottom.append(len(canonical_vertices))
            canonical_vertices.append(
                (x_value, SKIRT_BOTTOM_Y_M, sections[row])
            )
        bottom.append(right_bottom[row])

        for strip in range(3):
            top_left = top + strip
            top_right = top + strip + 1
            low_left = bottom[strip]
            low_right = bottom[strip + 1]
            if front:
                add_triangle(
                    faces, material_indices,
                    top_left, low_right, low_left, 2,
                )
                add_triangle(
                    faces, material_indices,
                    top_left, top_right, low_right, 2,
                )
            else:
                add_triangle(
                    faces, material_indices,
                    top_left, low_left, low_right, 2,
                )
                add_triangle(
                    faces, material_indices,
                    top_left, low_right, top_right, 2,
                )

    bypass_sections = sorted(set(
        [DEAD_ZONE_M * 0.5,
         DEAD_ZONE_M,
         DEAD_ZONE_M + CORE_LENGTH_M,
         DEAD_ZONE_M + CORE_LENGTH_M + DEAD_ZONE_M * 0.5]
        + [value for value in sections
           if DEAD_ZONE_M * 0.5 < value
           < DEAD_ZONE_M + CORE_LENGTH_M + DEAD_ZONE_M * 0.5]
    ))
    bypass_rows: list[tuple[int, int]] = []
    for z_forward in bypass_sections:
        center_x = bypass_center_x(z_forward)
        surface_y = outer_terrain_height(z_forward, False) + BYPASS_RISE_M
        left = len(canonical_vertices)
        canonical_vertices.append(
            (center_x - BYPASS_HALF_WIDTH_M, surface_y, z_forward)
        )
        right = len(canonical_vertices)
        canonical_vertices.append(
            (center_x + BYPASS_HALF_WIDTH_M, surface_y, z_forward)
        )
        bypass_rows.append((left, right))
    for previous, current in zip(bypass_rows, bypass_rows[1:]):
        add_triangle(
            faces, material_indices,
            previous[0], current[0], current[1], 3,
        )
        add_triangle(
            faces, material_indices,
            previous[0], current[1], previous[1], 3,
        )

    blender_vertices = [canonical_to_blender(point) for point in canonical_vertices]
    mesh = bpy.data.meshes.new(name=MESH_NAME)
    mesh.from_pydata(blender_vertices, [], faces)
    mesh.materials.append(
        make_material(MAT_TRAIL, (0.38, 0.25, 0.12, 1.0))
    )
    mesh.materials.append(
        make_material(MAT_TERRAIN, (0.20, 0.32, 0.17, 1.0))
    )
    mesh.materials.append(
        make_material(MAT_SKIRT, (0.18, 0.19, 0.18, 1.0))
    )
    mesh.materials.append(
        make_material(MAT_BYPASS, (0.47, 0.34, 0.16, 1.0))
    )
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError("Blender had to repair generated tabletop geometry")

    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False

    mesh_object = bpy.data.objects.new(name=MESH_NAME, object_data=mesh)
    bpy.context.collection.objects.link(mesh_object)
    mesh_object.parent = root
    mesh_object["render_layer"] = "trail-and-terrain"
    mesh_object["physics_authority"] = "external"
    return mesh_object, canonical_vertices


def create_empty(
    root,
    name: str,
    canonical_location: tuple[float, float, float],
    display_size: float,
    properties: dict[str, object] | None = None,
):
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


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.35
    root["asset_id"] = "WG-01-tabletop-greybox"
    root["asset_name"] = ASSET_NAME
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["socket_half_width_m"] = SOCKET_HALF_WIDTH_M
    root["physics_authority"] = "external"
    root["profile_height_m"] = HEIGHT_M
    root["takeoff_run_m"] = TAKEOFF_RUN_M
    root["deck_length_m"] = DECK_LENGTH_M
    root["landing_run_m"] = LANDING_RUN_M

    mesh_object, canonical_vertices = build_mesh(root)

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

    lip_z = DEAD_ZONE_M + TAKEOFF_RUN_M
    deck_end_z = lip_z + DECK_LENGTH_M
    landing_z = deck_end_z + LANDING_RUN_M
    marker_properties = {"visual_only": True, "physics_authority": "external"}
    create_empty(root, "MARKER_PREPARE", (0.0, 0.0, 0.0), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_DECISION", (0.0, 0.0, DEAD_ZONE_M * 0.5),
                 0.18, marker_properties)
    create_empty(root, "MARKER_ACTION", (0.0, 0.0, DEAD_ZONE_M), 0.18,
                 marker_properties)
    create_empty(root, "MARKER_LIP", (0.0, HEIGHT_M, lip_z), 0.22,
                 marker_properties)
    create_empty(root, "MARKER_APEX",
                 (0.0, HEIGHT_M, lip_z + DECK_LENGTH_M * 0.5),
                 0.22, marker_properties)
    create_empty(root, "MARKER_LAND", (0.0, 0.0, landing_z), 0.22,
                 marker_properties)

    return root, mesh_object, canonical_vertices


def assert_close(actual: float, expected: float, message: str) -> None:
    # Blender stores transforms as 32-bit floats, so values read back from
    # scene objects need a slightly wider tolerance than geometry calculations.
    if not math.isclose(
        actual, expected, rel_tol=0.0, abs_tol=SERIALIZED_FLOAT_EPSILON
    ):
        raise RuntimeError(f"{message}: expected {expected}, got {actual}")


def assert_finite(values: Iterable[float], message: str) -> None:
    if not all(math.isfinite(float(value)) for value in values):
        raise RuntimeError(message)


def self_check(root, mesh_object, canonical_vertices) -> None:
    if bpy.app.version[0] != 4:
        raise RuntimeError(
            f"Blender 4.x is required, found {bpy.app.version_string}"
        )

    names = {obj.name for obj in bpy.context.scene.objects}
    missing = sorted(REQUIRED_NAMES - names)
    if missing:
        raise RuntimeError(f"Missing required nodes: {', '.join(missing)}")
    unexpected = sorted(names - REQUIRED_NAMES)
    if unexpected:
        raise RuntimeError(f"Unexpected scene nodes: {', '.join(unexpected)}")
    if len(names) != len(bpy.context.scene.objects):
        raise RuntimeError("Duplicate object names detected")
    if mesh_object.data.name != MESH_NAME:
        raise RuntimeError(f"Unexpected mesh data name: {mesh_object.data.name}")

    assert_close(SOCKET_HALF_WIDTH_M, 0.68, "Ordinary socket half-width")
    if DEAD_ZONE_M < 0.5:
        raise RuntimeError("Socket dead zones must be at least 0.5 m")
    assert_close(HEIGHT_M, 0.446, "Tabletop height")
    assert_close(TAKEOFF_RUN_M, 1.87, "Takeoff run")
    assert_close(DECK_LENGTH_M, 1.10, "Deck length")
    assert_close(LANDING_RUN_M, 1.87, "Landing run")

    takeoff_start = DEAD_ZONE_M
    lip_z = takeoff_start + TAKEOFF_RUN_M
    deck_end_z = lip_z + DECK_LENGTH_M
    landing_end_z = deck_end_z + LANDING_RUN_M
    continuity_checks = (
        (0.0, 0.0),
        (takeoff_start, 0.0),
        (lip_z, HEIGHT_M),
        (deck_end_z, HEIGHT_M),
        (landing_end_z, 0.0),
        (TILE_LENGTH_M, 0.0),
    )
    for location, expected in continuity_checks:
        assert_close(surface_height(location), expected,
                     f"Profile continuity at Z={location}")

    for fraction in (0.0, 0.25, 0.5, 0.75, 1.0):
        assert_close(surface_height(DEAD_ZONE_M * fraction), 0.0,
                     "Entry dead zone must be flat")
        exit_z = landing_end_z + DEAD_ZONE_M * fraction
        assert_close(surface_height(exit_z), 0.0,
                     "Exit dead zone must be flat")

    sample_count = 128
    takeoff_values = [
        surface_height(takeoff_start + TAKEOFF_RUN_M * index / sample_count)
        for index in range(sample_count + 1)
    ]
    landing_values = [
        surface_height(deck_end_z + LANDING_RUN_M * index / sample_count)
        for index in range(sample_count + 1)
    ]
    if any(left > right + EPSILON
           for left, right in zip(takeoff_values, takeoff_values[1:])):
        raise RuntimeError("Takeoff profile is not monotonic")
    if any(left < right - EPSILON
           for left, right in zip(landing_values, landing_values[1:])):
        raise RuntimeError("Landing profile is not monotonic")

    for point in canonical_vertices:
        assert_finite(point, "Non-finite canonical mesh coordinate")
    for vertex in mesh_object.data.vertices:
        assert_finite(vertex.co, "Non-finite Blender mesh coordinate")
    if any(polygon.loop_total != 3 for polygon in mesh_object.data.polygons):
        raise RuntimeError("Generated mesh contains a non-triangle face")
    if len(mesh_object.data.vertices) != 108 \
            or len(mesh_object.data.polygons) != 156:
        raise RuntimeError(
            "Unexpected greybox topology: expected 108 vertices and 156 "
            f"triangles, got {len(mesh_object.data.vertices)} vertices and "
            f"{len(mesh_object.data.polygons)} triangles"
        )
    if any(polygon.area <= EPSILON for polygon in mesh_object.data.polygons):
        raise RuntimeError("Generated mesh contains a degenerate triangle")

    x_values = [point[0] for point in canonical_vertices]
    y_values = [point[1] for point in canonical_vertices]
    z_values = [point[2] for point in canonical_vertices]
    assert_close(min(x_values), -TERRAIN_HALF_WIDTH_M,
                 "Mesh left extent")
    assert_close(max(x_values), TERRAIN_HALF_WIDTH_M,
                 "Mesh right extent")
    assert_close(min(y_values), SKIRT_BOTTOM_Y_M,
                 "Mesh skirt depth")
    assert_close(max(y_values), HEIGHT_M, "Mesh profile height")
    assert_close(min(z_values), 0.0, "Mesh start")
    assert_close(max(z_values), TILE_LENGTH_M, "Mesh length")

    for socket_name, socket_z in (
        ("SOCKET_IN", 0.0),
        ("SOCKET_OUT", TILE_LENGTH_M),
    ):
        socket = bpy.data.objects[socket_name]
        assert_close(float(socket["socket_half_width_m"]), 0.68,
                     f"{socket_name} metadata width")
        expected_location = canonical_to_blender((0.0, 0.0, socket_z))
        for actual, expected in zip(socket.location, expected_location):
            assert_close(float(actual), expected,
                         f"{socket_name} location")
        seam_x = {
            round(point[0], 8)
            for point in canonical_vertices
            if math.isclose(point[2], socket_z, abs_tol=EPSILON)
            and math.isclose(point[1], 0.0, abs_tol=EPSILON)
        }
        if round(-SOCKET_HALF_WIDTH_M, 8) not in seam_x \
                or round(SOCKET_HALF_WIDTH_M, 8) not in seam_x:
            raise RuntimeError(f"{socket_name} is missing exact trail seam vertices")

    if root.location.length > EPSILON \
            or any(abs(value) > EPSILON for value in root.rotation_euler):
        raise RuntimeError("Root transform is not identity")
    if any(abs(value - 1.0) > EPSILON for value in root.scale):
        raise RuntimeError("Root scale is not applied")
    if mesh_object.location.length > EPSILON \
            or any(abs(value) > EPSILON
                   for value in mesh_object.rotation_euler):
        raise RuntimeError("Mesh transform is not identity")
    if any(abs(value - 1.0) > EPSILON for value in mesh_object.scale):
        raise RuntimeError("Mesh scale is not applied")
    if root.get("physics_authority") != "external" \
            or mesh_object.get("physics_authority") != "external":
        raise RuntimeError("GLB must not claim physics authority")

    expected_materials = [MAT_TRAIL, MAT_TERRAIN, MAT_SKIRT, MAT_BYPASS]
    actual_materials = [material.name for material in mesh_object.data.materials]
    if actual_materials != expected_materials:
        raise RuntimeError(
            f"Unexpected material order: {actual_materials}"
        )
    for material_name in expected_materials:
        material = bpy.data.materials.get(material_name)
        if material is None or material.node_tree is None:
            raise RuntimeError(f"Missing node material {material_name}")
        expected_nodes = {
            f"SHADER_{material_name}",
            f"OUTPUT_{material_name}",
        }
        actual_nodes = {node.name for node in material.node_tree.nodes}
        if actual_nodes != expected_nodes:
            raise RuntimeError(
                f"Unexpected shader nodes for {material_name}: "
                f"{sorted(actual_nodes)}"
            )


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
    script_arguments = []
    if "--" in sys.argv:
        script_arguments = sys.argv[sys.argv.index("--") + 1:]
    parser = argparse.ArgumentParser(
        description="Generate the Workout Game tabletop greybox GLB"
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output .glb path; relative paths are resolved from the working directory",
    )
    return parser.parse_args(script_arguments)


def main() -> None:
    arguments = parse_arguments()
    output_path = Path(os.path.expanduser(arguments.output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")

    root, mesh_object, canonical_vertices = build_scene()
    self_check(root, mesh_object, canonical_vertices)
    export_glb(output_path)
    print(
        "Generated",
        output_path,
        f"({len(mesh_object.data.vertices)} vertices, "
        f"{len(mesh_object.data.polygons)} triangles)",
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:  # Blender must return a failing process status.
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
