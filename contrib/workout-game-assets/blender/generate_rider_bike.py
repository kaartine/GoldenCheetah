#!/usr/bin/env python3
"""Generate the project-authored low-poly Workout Game rider and 29er MTB."""

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


ROOT_NAME = "ROOT_RiderBike"
WHEEL_RADIUS_M = 0.3683
WHEELBASE_M = 1.313
CHAINSTAY_M = 0.455
HEAD_ANGLE_DEGREES = 63.5
SUSPENSION_TRAVEL_M = 0.190
REAR_AXLE = (0.0, WHEEL_RADIUS_M, -CHAINSTAY_M)
FRONT_AXLE = (0.0, WHEEL_RADIUS_M, WHEELBASE_M - CHAINSTAY_M)
CRANK = (0.0, 0.45, 0.0)
STEER = (0.0, 1.04, 0.39)
PELVIS = (0.0, 1.08, -0.10)
SEAT = (0.0, 0.96, -0.10)
HEAD_LOW = (0.0, 0.70, 0.53)
HEAD_HIGH = (0.0, 1.00, 0.39)
REAR_PIVOT = (0.0, 0.60, -0.07)
SHOCK_UPPER = (0.0, 0.88, -0.17)

MESH_NAMES = {
    "GEO_MainFrame_LOD0",
    "GEO_Swingarm_LOD0",
    "GEO_Fork_LOD0",
    "GEO_RearShock_LOD0",
    "GEO_RearWheel_LOD0",
    "GEO_FrontWheel_LOD0",
    "GEO_Crank_LOD0",
    "GEO_Torso_LOD0",
    "GEO_Head_LOD0",
    "GEO_Helmet_LOD0",
    "GEO_Limb_LOD0",
    "GEO_Shadow_LOD0",
}
PIVOT_NAMES = {
    "PIVOT_REAR_AXLE",
    "PIVOT_FRONT_AXLE",
    "PIVOT_CRANK",
    "PIVOT_STEER",
    "PIVOT_PELVIS",
    "PIVOT_CAMERA_TARGET",
    "PIVOT_SHADOW",
}
REQUIRED_NAMES = {ROOT_NAME, *MESH_NAMES, *PIVOT_NAMES}

MATERIALS = (
    ("MAT_Bike_Yellow", (0.88, 0.57, 0.04, 1.0)),
    ("MAT_Tire_Dark", (0.035, 0.045, 0.045, 1.0)),
    ("MAT_Rider_Red", (0.72, 0.055, 0.045, 1.0)),
    ("MAT_Skin", (0.86, 0.54, 0.34, 1.0)),
    ("MAT_Helmet_Yellow", (0.98, 0.69, 0.03, 1.0)),
    ("MAT_Shadow", (0.05, 0.065, 0.06, 1.0)),
)

EPSILON = 1.0e-7


def vector_add(left, right):
    return tuple(left[index] + right[index] for index in range(3))


def vector_subtract(left, right):
    return tuple(left[index] - right[index] for index in range(3))


def vector_scale(value, amount):
    return tuple(component * amount for component in value)


def vector_length(value):
    return math.sqrt(sum(component * component for component in value))


def vector_normalize(value):
    length = vector_length(value)
    if length <= EPSILON:
        raise RuntimeError("Cannot normalize a zero-length vector")
    return vector_scale(value, 1.0 / length)


def vector_cross(left, right):
    return (
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    )


def append_tube(vertices, faces, start, end, radius, sides=6):
    direction = vector_normalize(vector_subtract(end, start))
    reference = (1.0, 0.0, 0.0) if abs(direction[0]) < 0.8 else (0.0, 1.0, 0.0)
    first_axis = vector_normalize(vector_cross(direction, reference))
    second_axis = vector_cross(direction, first_axis)
    base = len(vertices)
    for point in (start, end):
        for side in range(sides):
            angle = 2.0 * math.pi * side / sides
            offset = vector_add(
                vector_scale(first_axis, radius * math.cos(angle)),
                vector_scale(second_axis, radius * math.sin(angle)),
            )
            vertices.append(vector_add(point, offset))
    for side in range(sides):
        following = (side + 1) % sides
        faces.extend(
            (
                (base + side, base + sides + side, base + sides + following),
                (base + side, base + sides + following, base + following),
            )
        )
    start_center = len(vertices)
    vertices.append(start)
    end_center = len(vertices)
    vertices.append(end)
    for side in range(sides):
        following = (side + 1) % sides
        faces.append((start_center, base + following, base + side))
        faces.append((end_center, base + sides + side, base + sides + following))


def create_mesh(root, name, vertices, faces, material):
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata([canonical_to_blender(point) for point in vertices], [], faces)
    mesh.materials.append(material)
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError(f"Blender repaired generated geometry in {name}")
    for polygon in mesh.polygons:
        polygon.use_smooth = False
    spans = [
        max(point[axis] for point in vertices)
        - min(point[axis] for point in vertices)
        for axis in range(3)
    ]
    axes = sorted(range(3), key=lambda axis: spans[axis], reverse=True)[:2]
    minima = [min(point[axis] for point in vertices) for axis in axes]
    uv_layer = mesh.uv_layers.new(name="UVMap")
    for loop in mesh.loops:
        point = vertices[loop.vertex_index]
        uv_layer.data[loop.index].uv = (
            (point[axes[0]] - minima[0]) / max(spans[axes[0]], EPSILON),
            (point[axes[1]] - minima[1]) / max(spans[axes[1]], EPSILON),
        )
    result = bpy.data.objects.new(name=name, object_data=mesh)
    bpy.context.collection.objects.link(result)
    result.parent = root
    result["physics_authority"] = "external"
    return result


def wheel_mesh(center):
    vertices = []
    faces = []
    major_segments = 16
    minor_segments = 4
    tire_radius = 0.038
    for major in range(major_segments):
        major_angle = 2.0 * math.pi * major / major_segments
        radial_y = math.cos(major_angle)
        radial_z = math.sin(major_angle)
        for minor in range(minor_segments):
            minor_angle = 2.0 * math.pi * minor / minor_segments
            radial = WHEEL_RADIUS_M - tire_radius + tire_radius * math.cos(minor_angle)
            vertices.append((
                center[0] + tire_radius * math.sin(minor_angle),
                center[1] + radial * radial_y,
                center[2] + radial * radial_z,
            ))
    for major in range(major_segments):
        next_major = (major + 1) % major_segments
        for minor in range(minor_segments):
            next_minor = (minor + 1) % minor_segments
            a = major * minor_segments + minor
            b = next_major * minor_segments + minor
            c = next_major * minor_segments + next_minor
            d = major * minor_segments + next_minor
            faces.extend(((a, b, c), (a, c, d)))
    return vertices, faces


def tube_mesh(rods):
    vertices = []
    faces = []
    for start, end, radius in rods:
        append_tube(vertices, faces, start, end, radius)
    return vertices, faces


def main_frame_mesh():
    return tube_mesh((
        (CRANK, SEAT, 0.033),
        (CRANK, HEAD_LOW, 0.060),
        (SEAT, HEAD_HIGH, 0.032),
        (HEAD_LOW, HEAD_HIGH, 0.038),
        (REAR_PIVOT, SEAT, 0.028),
        ((-0.34, STEER[1], STEER[2]),
         (0.34, STEER[1], STEER[2]), 0.018),
        ((-0.14, 1.01, SEAT[2]), (0.14, 1.01, SEAT[2]), 0.026),
    ))


def swingarm_mesh():
    return tube_mesh((
        (REAR_AXLE, REAR_PIVOT, 0.030),
        (REAR_AXLE, CRANK, 0.026),
        (CRANK, REAR_PIVOT, 0.025),
    ))


def fork_mesh():
    return tube_mesh((
        ((-0.045, HEAD_LOW[1], HEAD_LOW[2]),
         (-0.055, FRONT_AXLE[1], FRONT_AXLE[2]), 0.025),
        ((0.045, HEAD_LOW[1], HEAD_LOW[2]),
         (0.055, FRONT_AXLE[1], FRONT_AXLE[2]), 0.025),
        (HEAD_LOW, HEAD_HIGH, 0.031),
    ))


def rear_shock_mesh():
    midpoint = vector_scale(vector_add(REAR_PIVOT, SHOCK_UPPER), 0.5)
    return tube_mesh((
        (REAR_PIVOT, midpoint, 0.027),
        (midpoint, SHOCK_UPPER, 0.038),
    ))


def crank_mesh():
    vertices = []
    faces = []
    append_tube(vertices, faces, (-0.18, CRANK[1], CRANK[2]),
                (0.18, CRANK[1], CRANK[2]), 0.015)
    append_tube(vertices, faces, (0.0, CRANK[1] - 0.16, CRANK[2]),
                (0.0, CRANK[1] + 0.16, CRANK[2]), 0.012)
    return vertices, faces


def torso_mesh():
    lower_y, upper_y = 0.0, 0.48
    lower_half_x, upper_half_x = 0.16, 0.23
    lower_half_z, upper_half_z = 0.10, 0.13
    vertices = []
    for y_value, half_x, half_z in (
        (lower_y, lower_half_x, lower_half_z),
        (upper_y, upper_half_x, upper_half_z),
    ):
        vertices.extend((
            (-half_x, y_value, -half_z),
            (half_x, y_value, -half_z),
            (half_x, y_value, half_z),
            (-half_x, y_value, half_z),
        ))
    faces = [
        (0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
        (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
        (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7),
    ]
    return vertices, faces


def low_poly_sphere(center, radius, lower_fraction=-1.0):
    vertices = [(center[0], center[1] + radius, center[2])]
    ring_y = center[1] + radius * 0.15
    ring_radius = radius * 0.98
    for index in range(8):
        angle = 2.0 * math.pi * index / 8
        vertices.append((
            center[0] + ring_radius * math.cos(angle),
            ring_y,
            center[2] + ring_radius * math.sin(angle),
        ))
    vertices.append((center[0], center[1] + radius * lower_fraction, center[2]))
    bottom = len(vertices) - 1
    faces = []
    for index in range(8):
        following = 1 + (index + 1) % 8
        current = 1 + index
        faces.append((0, current, following))
        faces.append((bottom, following, current))
    return vertices, faces


def helmet_mesh():
    vertices = []
    for y_value, radius in ((-0.02, 0.145), (0.105, 0.12)):
        for index in range(8):
            angle = 2.0 * math.pi * index / 8
            vertices.append((
                radius * math.cos(angle),
                y_value,
                radius * math.sin(angle),
            ))
    vertices.append((0.0, 0.145, 0.0))
    top = len(vertices) - 1
    faces = []
    for index in range(8):
        following = (index + 1) % 8
        faces.extend((
            (index, 8 + index, 8 + following),
            (index, 8 + following, following),
            (top, 8 + following, 8 + index),
        ))
    return vertices, faces


def limb_mesh():
    vertices = []
    faces = []
    append_tube(vertices, faces, (0.0, 0.0, 0.0), (0.0, 1.0, 0.0), 0.075)
    return vertices, faces


def shadow_mesh():
    vertices = [(0.0, 0.012, -0.04)]
    faces = []
    for index in range(16):
        angle = 2.0 * math.pi * index / 16
        vertices.append((0.42 * math.cos(angle), 0.012,
                         -0.04 + 0.78 * math.sin(angle)))
    for index in range(16):
        faces.append((0, 1 + index, 1 + (index + 1) % 16))
    return vertices, faces


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0

    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.30
    root["asset_id"] = "RB-01-rider-bike"
    root["asset_name"] = "Workout Game Rider Bike"
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["physics_authority"] = "external"
    root["reference_geometry"] = "Pole Voima K2 public geometry table"
    root["wheelbase_m"] = WHEELBASE_M
    root["chainstay_m"] = CHAINSTAY_M
    root["head_angle_degrees"] = HEAD_ANGLE_DEGREES
    root["suspension_travel_m"] = SUSPENSION_TRAVEL_M

    materials = {name: make_material(name, color) for name, color in MATERIALS}
    for name, generator in (
        ("GEO_MainFrame_LOD0", main_frame_mesh),
        ("GEO_Swingarm_LOD0", swingarm_mesh),
        ("GEO_Fork_LOD0", fork_mesh),
        ("GEO_RearShock_LOD0", rear_shock_mesh),
    ):
        vertices, faces = generator()
        create_mesh(root, name, vertices, faces,
                    materials["MAT_Bike_Yellow"])
    for name, center in (
        ("GEO_RearWheel_LOD0", REAR_AXLE),
        ("GEO_FrontWheel_LOD0", FRONT_AXLE),
    ):
        vertices, faces = wheel_mesh(center)
        create_mesh(root, name, vertices, faces, materials["MAT_Tire_Dark"])
    vertices, faces = crank_mesh()
    create_mesh(root, "GEO_Crank_LOD0", vertices, faces,
                materials["MAT_Bike_Yellow"])
    vertices, faces = torso_mesh()
    create_mesh(root, "GEO_Torso_LOD0", vertices, faces,
                materials["MAT_Rider_Red"])
    vertices, faces = low_poly_sphere((0.0, 0.0, 0.0), 0.14)
    create_mesh(root, "GEO_Head_LOD0", vertices, faces, materials["MAT_Skin"])
    vertices, faces = helmet_mesh()
    create_mesh(root, "GEO_Helmet_LOD0", vertices, faces,
                materials["MAT_Helmet_Yellow"])
    vertices, faces = limb_mesh()
    create_mesh(root, "GEO_Limb_LOD0", vertices, faces,
                materials["MAT_Rider_Red"])
    vertices, faces = shadow_mesh()
    create_mesh(root, "GEO_Shadow_LOD0", vertices, faces,
                materials["MAT_Shadow"])

    pivot_properties = {"visual_only": True, "physics_authority": "external"}
    for name, location in (
        ("PIVOT_REAR_AXLE", REAR_AXLE),
        ("PIVOT_FRONT_AXLE", FRONT_AXLE),
        ("PIVOT_CRANK", CRANK),
        ("PIVOT_STEER", STEER),
        ("PIVOT_PELVIS", PELVIS),
        ("PIVOT_CAMERA_TARGET", (0.0, 1.34, 0.08)),
        ("PIVOT_SHADOW", (0.0, 0.0, -0.04)),
    ):
        create_empty(root, name, location, 0.10, pivot_properties)
    return root


def self_check(root) -> tuple[int, int]:
    if bpy.app.version[0] != 4:
        raise RuntimeError(f"Blender 4.x is required, found {bpy.app.version_string}")
    objects = {obj.name: obj for obj in bpy.context.scene.objects}
    if set(objects) != REQUIRED_NAMES:
        raise RuntimeError(
            f"Scene node mismatch; missing={sorted(REQUIRED_NAMES - set(objects))}, "
            f"unexpected={sorted(set(objects) - REQUIRED_NAMES)}"
        )
    if not math.isclose(FRONT_AXLE[2] - REAR_AXLE[2], WHEELBASE_M, abs_tol=1e-9):
        raise RuntimeError("Wheelbase contract changed")
    if not math.isclose(REAR_AXLE[1], WHEEL_RADIUS_M, abs_tol=1e-9):
        raise RuntimeError("29er wheel does not touch the ground")
    if not math.isclose(CRANK[2] - REAR_AXLE[2], CHAINSTAY_M, abs_tol=1e-9):
        raise RuntimeError("Chainstay contract changed")
    triangle_count = 0
    vertex_count = 0
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
        if len(obj.data.uv_layers) != 1:
            raise RuntimeError(f"{name} must have exactly one UV map")
        if any(not all(math.isfinite(value) and -EPSILON <= value <= 1.0 + EPSILON
                       for value in uv.uv)
               for uv in obj.data.uv_layers[0].data):
            raise RuntimeError(f"{name} has invalid UV coordinates")
        triangle_count += len(obj.data.polygons)
        vertex_count += len(obj.data.vertices)
    if triangle_count > 1400:
        raise RuntimeError(f"Rider exceeds triangle budget: {triangle_count}")
    if root.location.length > EPSILON:
        raise RuntimeError("Root transform is not identity")
    return vertex_count, triangle_count


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
    parser = argparse.ArgumentParser(description="Generate the rider-bike GLB")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def main() -> None:
    output_path = Path(os.path.expanduser(parse_arguments().output)).resolve()
    if output_path.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root = build_scene()
    vertices, triangles = self_check(root)
    export_glb(output_path)
    print("Generated", output_path,
          f"({vertices} vertices, {triangles} triangles)")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
