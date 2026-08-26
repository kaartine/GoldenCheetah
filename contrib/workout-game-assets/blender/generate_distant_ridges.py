#!/usr/bin/env python3
"""Generate the bounded 360-degree Workout Game distant terrain ring."""

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


ROOT_NAME = "ROOT_DistantRidges"
MESH_NAME = "GEO_DistantRidges_LOD0"
REQUIRED_NAMES = {ROOT_NAME, MESH_NAME, "PIVOT_CENTER"}
RADII_METERS = (42.0, 72.0, 110.0, 165.0, 240.0)
BASE_HEIGHTS_METERS = (-1.8, 1.4, 4.8, 9.0, 14.0)
AMPLITUDES_METERS = (0.0, 0.8, 2.1, 4.0, 7.0)
SEGMENTS = 32
EPSILON = 1.0e-7


def ridge_height(ring: int, angle: float) -> float:
    amplitude = AMPLITUDES_METERS[ring]
    phase = ring * 0.73
    variation = (
        0.55 * math.sin(2.0 * angle + phase)
        + 0.30 * math.sin(5.0 * angle - phase * 0.6)
        + 0.15 * math.cos(9.0 * angle + phase * 1.7)
    )
    return BASE_HEIGHTS_METERS[ring] + amplitude * variation


def ridge_mesh():
    vertices = []
    faces = []
    uvs = []
    for ring, radius in enumerate(RADII_METERS):
        for segment in range(SEGMENTS + 1):
            ratio = segment / SEGMENTS
            angle = ratio * 2.0 * math.pi
            vertices.append((
                radius * math.cos(angle),
                ridge_height(ring, angle),
                radius * math.sin(angle),
            ))
            uvs.append((ratio * 8.0, ring * 1.5))
    stride = SEGMENTS + 1
    for ring in range(len(RADII_METERS) - 1):
        for segment in range(SEGMENTS):
            inner = ring * stride + segment
            outer = (ring + 1) * stride + segment
            faces.extend((
                (inner, outer, outer + 1),
                (inner, outer + 1, inner + 1),
            ))
    return vertices, faces, uvs


def create_mesh(root, material):
    vertices, faces, uvs = ridge_mesh()
    mesh = bpy.data.meshes.new(name=MESH_NAME)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in vertices], [], faces)
    mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError("Blender repaired distant-ridge geometry")
    uv_layer = mesh.uv_layers.new(name="UVMap")
    for polygon in mesh.polygons:
        polygon.use_smooth = False
        for loop_index in polygon.loop_indices:
            vertex_index = mesh.loops[loop_index].vertex_index
            uv_layer.data[loop_index].uv = uvs[vertex_index]
    result = bpy.data.objects.new(name=MESH_NAME, object_data=mesh)
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
    root.empty_display_size = 2.0
    root["asset_id"] = "EN-03-distant-ridges"
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["physics_authority"] = "external"
    material = make_material(
        "MAT_DistantRidges", (0.20, 0.39, 0.22, 1.0))
    create_mesh(root, material)
    create_empty(root, "PIVOT_CENTER", (0.0, 0.0, 0.0), 1.0,
                 {"visual_only": True, "physics_authority": "external"})
    return root


def self_check(root) -> int:
    if bpy.app.version[0] != 4:
        raise RuntimeError(
            f"Blender 4.x is required, found {bpy.app.version_string}")
    objects = {obj.name: obj for obj in bpy.context.scene.objects}
    if set(objects) != REQUIRED_NAMES:
        raise RuntimeError("Distant-ridge scene node inventory changed")
    obj = objects[MESH_NAME]
    if obj.location.length > EPSILON or any(
        abs(value) > EPSILON for value in obj.rotation_euler
    ) or any(abs(value - 1.0) > EPSILON for value in obj.scale):
        raise RuntimeError("Distant-ridge transform is not applied")
    if obj.get("physics_authority") != "external":
        raise RuntimeError("Distant ridges claim physics authority")
    if len(obj.data.uv_layers) != 1:
        raise RuntimeError("Distant ridges must have one UV map")
    if any(polygon.loop_total != 3 or polygon.area <= EPSILON
           for polygon in obj.data.polygons):
        raise RuntimeError("Distant ridges contain invalid triangles")
    triangles = len(obj.data.polygons)
    if triangles != 256:
        raise RuntimeError(f"Unexpected triangle count: {triangles}")
    maximum_radius = max(
        math.hypot(vertex.co.x, vertex.co.y) for vertex in obj.data.vertices)
    if not math.isclose(maximum_radius, 240.0, abs_tol=1.0e-4):
        raise RuntimeError(f"Unexpected outer radius: {maximum_radius}")
    return triangles


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


def main() -> None:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Generate distant-ridge GLB")
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
