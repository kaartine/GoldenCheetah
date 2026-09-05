#!/usr/bin/env python3
"""Generate four low-poly Finnish forest silhouettes for Workout Game."""

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


ROOT_NAME = "ROOT_ConiferSet"
MESH_NAMES = {
    "GEO_ConiferTrunk_LOD0",
    "GEO_ConiferNarrow_LOD0",
    "GEO_ConiferLayered_LOD0",
    "GEO_ConiferBrokenTop_LOD0",
    "GEO_ScotsPineTrunk_LOD0",
    "GEO_ScotsPineCrown_LOD0",
}
REQUIRED_NAMES = {ROOT_NAME, "PIVOT_BASE", *MESH_NAMES}
EPSILON = 1.0e-7
MAXIMUM_CROWN_RADIUS_METERS = 1.35
TRIANGLE_BUDGET = 420
EXPECTED_TRIANGLES = {
    "GEO_ConiferTrunk_LOD0": 16,
    "GEO_ConiferNarrow_LOD0": 80,
    "GEO_ConiferLayered_LOD0": 72,
    "GEO_ConiferBrokenTop_LOD0": 96,
    "GEO_ScotsPineTrunk_LOD0": 40,
    "GEO_ScotsPineCrown_LOD0": 96,
}
RUNTIME_VARIANT_PAIRS = (
    ("GEO_ConiferTrunk_LOD0", "GEO_ConiferNarrow_LOD0"),
    ("GEO_ConiferTrunk_LOD0", "GEO_ConiferLayered_LOD0"),
    ("GEO_ConiferTrunk_LOD0", "GEO_ConiferBrokenTop_LOD0"),
    ("GEO_ScotsPineTrunk_LOD0", "GEO_ScotsPineCrown_LOD0"),
)
MAXIMUM_RUNTIME_VARIANT_TRIANGLES = 136
SCOTS_PINE_CLUMP_TRIANGLES = 72
SCOTS_PINE_FACES_PER_SIDE = 4


def append_ring(
    vertices, y_value, radius, offset_x=0.0, offset_z=0.0, sides=8
):
    start = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        vertices.append((
            offset_x + radius * math.cos(angle),
            y_value,
            offset_z + radius * math.sin(angle),
        ))
    return start


def append_join(faces, lower, upper, sides=8):
    for side in range(sides):
        following = (side + 1) % sides
        faces.extend((
            (lower + side, upper + side, upper + following),
            (lower + side, upper + following, lower + following),
        ))


def tapered_trunk():
    vertices = []
    faces = []
    lower = append_ring(vertices, 0.0, 0.19, sides=6)
    upper = append_ring(vertices, 2.35, 0.105, sides=6)
    append_join(faces, lower, upper, sides=6)
    faces.extend((
        (lower, lower + 2, lower + 1),
        (lower, lower + 3, lower + 2),
        (lower, lower + 4, lower + 3),
        (lower, lower + 5, lower + 4),
    ))
    return vertices, faces


def scots_pine_trunk():
    """Return a tall, subtly bent six-sided trunk with a readable bare bole."""
    vertices = []
    faces = []
    rings = (
        append_ring(vertices, 0.0, 0.20, 0.0, 0.0, sides=6),
        append_ring(vertices, 1.55, 0.16, 0.03, -0.02, sides=6),
        append_ring(vertices, 3.20, 0.12, -0.05, 0.03, sides=6),
        append_ring(vertices, 4.55, 0.075, 0.07, 0.00, sides=6),
    )
    for lower, upper in zip(rings, rings[1:]):
        append_join(faces, lower, upper, sides=6)
    faces.extend((
        (rings[0], rings[0] + 2, rings[0] + 1),
        (rings[0], rings[0] + 3, rings[0] + 2),
        (rings[0], rings[0] + 4, rings[0] + 3),
        (rings[0], rings[0] + 5, rings[0] + 4),
    ))
    return vertices, faces


def append_low_poly_clump(
    vertices, faces, center, radius_x, radius_z, height, sides=6
):
    """Append a faceted asymmetric needle clump without overlapping faces."""
    center_x, center_y, center_z = center
    lower = len(vertices)
    vertices.append((center_x, center_y - height * 0.50, center_z))
    lower_ring = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        vertices.append((
            center_x + radius_x * math.cos(angle),
            center_y - height * 0.12,
            center_z + radius_z * math.sin(angle),
        ))
    upper_ring = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        vertices.append((
            center_x + radius_x * 0.72 * math.cos(angle),
            center_y + height * 0.20,
            center_z + radius_z * 0.72 * math.sin(angle),
        ))
    top = len(vertices)
    vertices.append((
        center_x - radius_x * 0.10,
        center_y + height * 0.50,
        center_z + radius_z * 0.06,
    ))
    for side in range(sides):
        following = (side + 1) % sides
        faces.append((lower, lower_ring + side, lower_ring + following))
        faces.extend((
            (lower_ring + side, upper_ring + side, upper_ring + following),
            (lower_ring + side, upper_ring + following,
             lower_ring + following),
        ))
        faces.append((upper_ring + side, top, upper_ring + following))


def scots_pine_crown():
    """Return an open, windswept crown characteristic of a mature pine."""
    vertices = []
    faces = []
    append_low_poly_clump(
        vertices, faces, (-0.34, 4.55, -0.04), 0.82, 0.60, 0.92
    )
    append_low_poly_clump(
        vertices, faces, (0.43, 4.67, 0.08), 0.70, 0.56, 0.82
    )
    append_low_poly_clump(
        vertices, faces, (0.08, 5.08, -0.08), 0.55, 0.46, 0.78
    )
    return vertices, faces


def with_understory(geometry, mirrored=False):
    """Add two small ground-level conifers to turn one anchor into a grove."""
    vertices, faces = geometry
    if mirrored:
        positions = ((0.73, 0.07, 0.43), (-0.62, 0.06, -0.49))
    else:
        positions = ((-0.73, 0.07, 0.43), (0.62, 0.06, -0.49))
    for index, (offset_x, base_y, offset_z) in enumerate(positions):
        ring = append_ring(
            vertices, base_y, 0.27 - index * 0.03,
            offset_x, offset_z, sides=6
        )
        center = len(vertices)
        vertices.append((offset_x, base_y, offset_z))
        tip = len(vertices)
        vertices.append((
            offset_x + (0.05 if index == 0 else -0.04),
            1.18 - index * 0.23,
            offset_z,
        ))
        for side in range(6):
            following = (side + 1) % 6
            faces.append((center, ring + following, ring + side))
            faces.append((ring + side, tip, ring + following))
    return vertices, faces


def continuous_crown(rings):
    vertices = []
    faces = []
    starts = [append_ring(vertices, *ring) for ring in rings]
    for lower, upper in zip(starts, starts[1:]):
        append_join(faces, lower, upper)
    top = len(vertices)
    last_y, _, last_offset = rings[-1]
    vertices.append((last_offset, last_y + 0.22, 0.0))
    for side in range(8):
        faces.append((starts[-1] + side,
                      top, starts[-1] + (side + 1) % 8))
    return vertices, faces


def layered_crown():
    vertices = []
    faces = []
    for base_y, radius, height in (
        (1.05, 1.28, 1.30),
        (2.02, 1.00, 1.18),
        (2.94, 0.68, 1.02),
    ):
        ring = append_ring(vertices, base_y, radius)
        tip = len(vertices)
        vertices.append((0.0, base_y + height, 0.0))
        center = len(vertices)
        vertices.append((0.0, base_y, 0.0))
        for side in range(8):
            following = (side + 1) % 8
            faces.append((ring + side, tip, ring + following))
            faces.append((center, ring + following, ring + side))
    return vertices, faces


def create_mesh(root, name, geometry, material):
    vertices, faces = geometry
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata([canonical_to_blender(point) for point in vertices], [], faces)
    mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError(f"Blender repaired generated geometry in {name}")
    for polygon in mesh.polygons:
        polygon.use_smooth = False
    result = bpy.data.objects.new(name=name, object_data=mesh)
    bpy.context.collection.objects.link(result)
    result.parent = root
    result["physics_authority"] = "external"
    return result


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.30
    root["asset_id"] = "EN-01-conifer-set"
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["physics_authority"] = "external"

    bark = make_material("MAT_ConiferBark", (0.24, 0.12, 0.045, 1.0))
    pine_bark = make_material("MAT_ScotsPineBark", (0.46, 0.22, 0.07, 1.0))
    dark = make_material("MAT_ConiferDark", (0.035, 0.25, 0.11, 1.0))
    light = make_material("MAT_ConiferLight", (0.07, 0.36, 0.16, 1.0))
    create_mesh(root, "GEO_ConiferTrunk_LOD0", tapered_trunk(), bark)
    create_mesh(root, "GEO_ConiferNarrow_LOD0", with_understory(continuous_crown((
        (1.00, 0.82, 0.0),
        (2.15, 0.60, 0.0),
        (3.35, 0.34, 0.0),
        (4.55, 0.06, 0.0),
    ))), dark)
    create_mesh(root, "GEO_ConiferLayered_LOD0",
                with_understory(layered_crown(), mirrored=True), light)
    create_mesh(root, "GEO_ConiferBrokenTop_LOD0", with_understory(continuous_crown((
        (0.95, 1.10, 0.0),
        (2.05, 0.82, 0.10),
        (3.05, 0.54, -0.08),
        (3.82, 0.25, 0.10),
        (4.25, 0.12, 0.14),
    )), mirrored=True), dark)
    pine_trunk = create_mesh(
        root, "GEO_ScotsPineTrunk_LOD0", scots_pine_trunk(), pine_bark
    )
    pine_trunk["species"] = "Pinus sylvestris-inspired original silhouette"
    pine_crown = create_mesh(
        root, "GEO_ScotsPineCrown_LOD0",
        with_understory(scots_pine_crown()), light
    )
    pine_crown["species"] = "Pinus sylvestris-inspired original silhouette"
    create_empty(root, "PIVOT_BASE", (0.0, 0.0, 0.0), 0.12,
                 {"visual_only": True, "physics_authority": "external"})
    return root


def self_check(root):
    if bpy.app.version[0] != 4:
        raise RuntimeError(f"Blender 4.x is required, found {bpy.app.version_string}")
    objects = {obj.name: obj for obj in bpy.context.scene.objects}
    if set(objects) != REQUIRED_NAMES:
        raise RuntimeError("Conifer scene node inventory changed")
    triangles = 0
    triangle_counts = {}
    for name in MESH_NAMES:
        obj = objects[name]
        if obj.location.length > EPSILON or any(
            abs(value) > EPSILON for value in obj.rotation_euler
        ) or any(abs(value - 1.0) > EPSILON for value in obj.scale):
            raise RuntimeError(f"{name} transform is not applied")
        if obj.get("physics_authority") != "external":
            raise RuntimeError(f"{name} claims physics authority")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in obj.data.polygons):
            raise RuntimeError(f"{name} has invalid triangles")
        for vertex in obj.data.vertices:
            canonical = vertex.co.x, vertex.co.z, -vertex.co.y
            if canonical[1] < -EPSILON:
                raise RuntimeError(f"{name} extends below its terrain anchor")
            if math.hypot(canonical[0], canonical[2]) \
                    > MAXIMUM_CROWN_RADIUS_METERS + EPSILON:
                raise RuntimeError(f"{name} exceeds camera-clearance radius")
        triangle_counts[name] = len(obj.data.polygons)
        if triangle_counts[name] != EXPECTED_TRIANGLES[name]:
            raise RuntimeError(
                f"{name} triangle contract changed: {triangle_counts[name]}"
            )
        if name == "GEO_ScotsPineCrown_LOD0":
            lower_clump_faces = (
                obj.data.polygons[index]
                for index in range(
                    0, SCOTS_PINE_CLUMP_TRIANGLES,
                    SCOTS_PINE_FACES_PER_SIDE
                )
            )
            if any(face.normal.z >= -EPSILON for face in lower_clump_faces):
                raise RuntimeError("Scots-pine lower clump winding faces inward")
        triangles += triangle_counts[name]
    if triangles > TRIANGLE_BUDGET:
        raise RuntimeError(f"Conifer set exceeds triangle budget: {triangles}")
    for trunk_name, crown_name in RUNTIME_VARIANT_PAIRS:
        runtime_triangles = (
            triangle_counts[trunk_name] + triangle_counts[crown_name]
        )
        if runtime_triangles > MAXIMUM_RUNTIME_VARIANT_TRIANGLES:
            raise RuntimeError(
                f"Runtime grove exceeds triangle budget: {runtime_triangles}"
            )
    return triangles


def export_glb(output_path):
    output_path.parent.mkdir(parents=True, exist_ok=True)
    for obj in bpy.context.scene.objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = bpy.data.objects[ROOT_NAME]
    bpy.ops.export_scene.gltf(
        filepath=str(output_path), check_existing=False, export_format="GLB",
        use_selection=True, export_yup=True, export_extras=True,
        export_cameras=False, export_lights=False, export_animations=False,
    )


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Generate conifer GLB")
    parser.add_argument("--output", required=True)
    output = Path(os.path.expanduser(parser.parse_args(arguments).output)).resolve()
    if output.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root = build_scene()
    triangles = self_check(root)
    export_glb(output)
    print("Generated", output, f"({triangles} triangles)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
