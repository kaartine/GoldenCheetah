#!/usr/bin/env python3
"""Generate three low-poly conifer silhouettes for Workout Game."""

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
}
REQUIRED_NAMES = {ROOT_NAME, "PIVOT_BASE", *MESH_NAMES}
EPSILON = 1.0e-7


def append_ring(vertices, y_value, radius, offset_x=0.0, sides=8):
    start = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        vertices.append((
            offset_x + radius * math.cos(angle),
            y_value,
            radius * math.sin(angle),
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
    dark = make_material("MAT_ConiferDark", (0.035, 0.25, 0.11, 1.0))
    light = make_material("MAT_ConiferLight", (0.07, 0.36, 0.16, 1.0))
    create_mesh(root, "GEO_ConiferTrunk_LOD0", tapered_trunk(), bark)
    create_mesh(root, "GEO_ConiferNarrow_LOD0", continuous_crown((
        (1.00, 0.82, 0.0),
        (2.15, 0.60, 0.0),
        (3.35, 0.34, 0.0),
        (4.55, 0.06, 0.0),
    )), dark)
    create_mesh(root, "GEO_ConiferLayered_LOD0", layered_crown(), light)
    create_mesh(root, "GEO_ConiferBrokenTop_LOD0", continuous_crown((
        (0.95, 1.10, 0.0),
        (2.05, 0.82, 0.10),
        (3.05, 0.54, -0.08),
        (3.82, 0.25, 0.10),
        (4.25, 0.12, 0.14),
    )), dark)
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
        triangles += len(obj.data.polygons)
    if triangles > 320:
        raise RuntimeError(f"Conifer set exceeds triangle budget: {triangles}")
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
