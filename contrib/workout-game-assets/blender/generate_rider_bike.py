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
HEAD_HIGH = (0.0, 1.00, 0.39)
HEAD_TUBE_RISE_M = 0.22
HEAD_LOW = (
    0.0,
    HEAD_HIGH[1] - HEAD_TUBE_RISE_M,
    HEAD_HIGH[2]
    + HEAD_TUBE_RISE_M / math.tan(math.radians(HEAD_ANGLE_DEGREES)),
)
FORK_SPLIT = tuple(
    FRONT_AXLE[index] + (HEAD_LOW[index] - FRONT_AXLE[index]) * 0.56
    for index in range(3)
)
LOWER_LINK_PIVOT = (0.0, 0.52, -0.015)
SEATSTAY_PIVOT = (0.0, 0.66, -0.205)
ROCKER_PIVOT = (0.0, 0.75, -0.075)
SHOCK_UPPER = (0.0, 0.88, -0.18)
MOTOR_REFERENCE_RADIUS_M = 0.105
MOTOR_HALF_WIDTH_M = 0.110
DOWN_TUBE_HALF_WIDTH_M = 0.078
DOWN_TUBE_PROFILE = (
    (0.395, -0.055),
    (0.390, 0.105),
    (0.690, 0.535),
    (0.775, 0.535),
    (0.825, 0.405),
    (0.560, -0.035),
)
MOTOR_PROFILE = (
    (0.355, -0.055),
    (0.375, 0.075),
    (0.425, 0.112),
    (0.505, 0.095),
    (0.555, 0.025),
    (0.525, -0.075),
    (0.455, -0.110),
    (0.385, -0.095),
)
SEAT_MAST_PROFILE = (
    (0.5701, 0.0291),
    (0.9683, -0.0558),
    (0.9517, -0.1442),
    (0.5499, -0.0791),
)
TOP_BRIDGE_PROFILE = (
    (0.9102, -0.0959),
    (0.9452, 0.3945),
    (1.0548, 0.3855),
    (1.0098, -0.1041),
)
LOWER_SWINGARM_PROFILE = (
    (0.3258, -0.4403),
    (0.4680, 0.0029),
    (0.5720, -0.0329),
    (0.4108, -0.4697),
)
UPPER_SWINGARM_PROFILE = (
    (0.3377, -0.4193),
    (0.6262, -0.1655),
    (0.6938, -0.2445),
    (0.3989, -0.4907),
)
MAX_GLB_BYTES = 64 * 1024
PIVOT_LOCATIONS = {
    "PIVOT_REAR_AXLE": REAR_AXLE,
    "PIVOT_FRONT_AXLE": FRONT_AXLE,
    "PIVOT_CRANK": CRANK,
    "PIVOT_STEER": STEER,
    "PIVOT_PELVIS": PELVIS,
    "PIVOT_CAMERA_TARGET": (0.0, 1.34, 0.08),
    "PIVOT_SHADOW": (0.0, 0.0, -0.04),
}

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


def append_tube(vertices, faces, start, end, radius, sides=6, end_radius=None):
    direction = vector_normalize(vector_subtract(end, start))
    reference = (1.0, 0.0, 0.0) if abs(direction[0]) < 0.8 else (0.0, 1.0, 0.0)
    first_axis = vector_normalize(vector_cross(direction, reference))
    second_axis = vector_cross(direction, first_axis)
    base = len(vertices)
    for point, point_radius in (
        (start, radius),
        (end, radius if end_radius is None else end_radius),
    ):
        for side in range(sides):
            angle = 2.0 * math.pi * side / sides
            offset = vector_add(
                vector_scale(first_axis, point_radius * math.cos(angle)),
                vector_scale(second_axis, point_radius * math.sin(angle)),
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


def append_side_prism(vertices, faces, profile_yz, half_width):
    """Extrude a convex side profile across X with triangulated caps."""
    if len(profile_yz) < 3:
        raise RuntimeError("A side profile requires at least three points")
    turns = []
    for index in range(len(profile_yz)):
        first = profile_yz[index]
        second = profile_yz[(index + 1) % len(profile_yz)]
        third = profile_yz[(index + 2) % len(profile_yz)]
        turns.append(
            (second[1] - first[1]) * (third[0] - second[0])
            - (second[0] - first[0]) * (third[1] - second[1])
        )
    if not (all(turn > EPSILON for turn in turns)
            or all(turn < -EPSILON for turn in turns)):
        raise RuntimeError("A side profile must be strictly convex")
    base = len(vertices)
    for x_value in (-half_width, half_width):
        vertices.extend((x_value, y_value, z_value)
                        for y_value, z_value in profile_yz)
    count = len(profile_yz)
    for index in range(1, count - 1):
        faces.append((base, base + index + 1, base + index))
        faces.append((base + count, base + count + index,
                      base + count + index + 1))
    for index in range(count):
        following = (index + 1) % count
        faces.extend((
            (base + index, base + following, base + count + following),
            (base + index, base + count + following, base + count + index),
        ))


def append_axle_cylinder(vertices, faces, center, half_width, radius, sides=8):
    start = (center[0] - half_width, center[1], center[2])
    end = (center[0] + half_width, center[1], center[2])
    append_tube(vertices, faces, start, end, radius, sides=sides)


def append_disc_ring(vertices, faces, center, x_value, inner_radius,
                     outer_radius, segments=8):
    """Add one visible low-poly brake-rotor face in the wheel plane."""
    base = len(vertices)
    for radius in (inner_radius, outer_radius):
        for index in range(segments):
            angle = 2.0 * math.pi * index / segments
            vertices.append((
                x_value,
                center[1] + radius * math.cos(angle),
                center[2] + radius * math.sin(angle),
            ))
    for index in range(segments):
        following = (index + 1) % segments
        inner = base + index
        outer = base + segments + index
        next_inner = base + following
        next_outer = base + segments + following
        if x_value < center[0]:
            faces.extend(((inner, next_outer, outer),
                          (inner, next_inner, next_outer)))
        else:
            faces.extend(((inner, outer, next_outer),
                          (inner, next_outer, next_inner)))


def append_spokes(vertices, faces, center, x_value, count=6):
    spoke_half_width = 0.006
    hub_radius = 0.045
    rim_radius = WHEEL_RADIUS_M - 0.050
    for index in range(count):
        angle = 2.0 * math.pi * index / count
        radial_y = math.cos(angle)
        radial_z = math.sin(angle)
        tangent_y = -radial_z * spoke_half_width
        tangent_z = radial_y * spoke_half_width
        inner_y = center[1] + radial_y * hub_radius
        inner_z = center[2] + radial_z * hub_radius
        outer_y = center[1] + radial_y * rim_radius
        outer_z = center[2] + radial_z * rim_radius
        base = len(vertices)
        vertices.extend((
            (x_value, inner_y + tangent_y, inner_z + tangent_z),
            (x_value, outer_y + tangent_y, outer_z + tangent_z),
            (x_value, outer_y - tangent_y, outer_z - tangent_z),
            (x_value, inner_y - tangent_y, inner_z - tangent_z),
        ))
        if x_value < center[0]:
            faces.extend(((base, base + 2, base + 1),
                          (base, base + 3, base + 2)))
        else:
            faces.extend(((base, base + 1, base + 2),
                          (base, base + 2, base + 3)))


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


def mesh_has_canonical_point(mesh_object, expected, tolerance=1e-6):
    for vertex in mesh_object.data.vertices:
        canonical = (vertex.co.x, vertex.co.z, -vertex.co.y)
        if vector_length(vector_subtract(canonical, expected)) <= tolerance:
            return True
    return False


def wheel_mesh(center):
    vertices = []
    faces = []
    major_segments = 12
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
    append_axle_cylinder(vertices, faces, center, 0.075, 0.040, sides=4)
    append_disc_ring(vertices, faces, center, 0.047, 0.052, 0.105)
    append_spokes(vertices, faces, center, 0.047)
    return vertices, faces


def tube_mesh(rods):
    vertices = []
    faces = []
    for start, end, radius in rods:
        append_tube(vertices, faces, start, end, radius)
    return vertices, faces


def main_frame_mesh():
    vertices = []
    faces = []
    append_side_prism(
        vertices, faces, DOWN_TUBE_PROFILE, DOWN_TUBE_HALF_WIDTH_M
    )
    append_side_prism(vertices, faces, MOTOR_PROFILE, MOTOR_HALF_WIDTH_M)
    append_side_prism(vertices, faces, SEAT_MAST_PROFILE, 0.064)
    append_side_prism(vertices, faces, TOP_BRIDGE_PROFILE, 0.058)
    for start, end, radius in (
        (HEAD_LOW, HEAD_HIGH, 0.046),
        ((0.0, 0.62, -0.09), ROCKER_PIVOT, 0.030),
    ):
        append_tube(vertices, faces, start, end, radius, sides=4)
    append_tube(
        vertices, faces,
        (-0.34, STEER[1], STEER[2]),
        (0.34, STEER[1], STEER[2]),
        0.019, sides=4,
    )
    append_tube(
        vertices, faces,
        (-0.145, 1.01, SEAT[2]),
        (0.145, 1.01, SEAT[2]),
        0.027, sides=4,
    )
    return vertices, faces


def swingarm_mesh():
    vertices = []
    faces = []
    append_side_prism(vertices, faces, LOWER_SWINGARM_PROFILE, 0.052)
    append_side_prism(vertices, faces, UPPER_SWINGARM_PROFILE, 0.050)
    for start, end, radius in (
        (LOWER_LINK_PIVOT, ROCKER_PIVOT, 0.030),
        (SEATSTAY_PIVOT, ROCKER_PIVOT, 0.034),
    ):
        append_tube(vertices, faces, start, end, radius, sides=4)
    return vertices, faces


def fork_mesh():
    vertices = []
    faces = []
    for x_value in (-0.052, 0.052):
        axle = (x_value, FRONT_AXLE[1], FRONT_AXLE[2])
        split = (x_value, FORK_SPLIT[1], FORK_SPLIT[2])
        crown = (x_value, HEAD_LOW[1], HEAD_LOW[2])
        append_tube(vertices, faces, axle, split, 0.038, sides=4,
                    end_radius=0.034)
        append_tube(vertices, faces, split, crown, 0.025, sides=4)
    append_tube(
        vertices, faces,
        (-0.080, HEAD_LOW[1], HEAD_LOW[2]),
        (0.080, HEAD_LOW[1], HEAD_LOW[2]),
        0.037, sides=4,
    )
    append_tube(vertices, faces, HEAD_LOW, HEAD_HIGH, 0.034, sides=4)
    return vertices, faces


def rear_shock_mesh():
    vertices = []
    faces = []
    append_side_prism(
        vertices,
        faces,
        tuple((point[1], point[2]) for point in (
            SEATSTAY_PIVOT,
            ROCKER_PIVOT,
            (0.0, 0.705, -0.025),
        )),
        0.045,
    )
    midpoint = vector_scale(vector_add(ROCKER_PIVOT, SHOCK_UPPER), 0.5)
    append_tube(vertices, faces, ROCKER_PIVOT, midpoint, 0.024, sides=4)
    append_tube(vertices, faces, midpoint, SHOCK_UPPER, 0.038, sides=6)
    return vertices, faces


def crank_mesh():
    vertices = []
    faces = []
    append_tube(vertices, faces, (-0.18, CRANK[1], CRANK[2]),
                (0.18, CRANK[1], CRANK[2]), 0.015, sides=4)
    append_tube(vertices, faces, (0.0, CRANK[1] - 0.16, CRANK[2]),
                (0.0, CRANK[1] + 0.16, CRANK[2]), 0.012, sides=4)
    return vertices, faces


def torso_mesh():
    vertices = []
    rings = (
        (0.00, 0.14, -0.08, 0.10),
        (0.29, 0.21, -0.06, 0.18),
        (0.41, 0.235, -0.015, 0.205),
        (0.48, 0.12, 0.015, 0.185),
    )
    for y_value, half_x, back_z, front_z in rings:
        vertices.extend((
            (-half_x, y_value, back_z),
            (half_x, y_value, back_z),
            (half_x, y_value, front_z),
            (-half_x, y_value, front_z),
        ))
    faces = [(0, 2, 1), (0, 3, 2)]
    for ring in range(len(rings) - 1):
        lower = ring * 4
        upper = lower + 4
        for side in range(4):
            following = (side + 1) % 4
            faces.extend((
                (lower + side, lower + following, upper + following),
                (lower + side, upper + following, upper + side),
            ))
    top = (len(rings) - 1) * 4
    faces.extend(((top, top + 1, top + 2), (top, top + 2, top + 3)))
    return vertices, faces


def visor_mesh(vertices, faces):
    append_side_prism(
        vertices,
        faces,
        (
            (0.005, 0.070),
            (0.005, 0.205),
            (0.035, 0.225),
            (0.060, 0.090),
        ),
        0.092,
    )


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
    visor_mesh(vertices, faces)
    return vertices, faces


def limb_mesh():
    vertices = []
    faces = []
    append_tube(
        vertices, faces,
        (0.0, 0.0, 0.0), (0.0, 1.0, 0.0),
        0.078, sides=6, end_radius=0.058,
    )
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
    root["surface_provenance"] = "GPL project-authored; no copied CAD or branding"
    root["wheelbase_m"] = WHEELBASE_M
    root["chainstay_m"] = CHAINSTAY_M
    root["head_angle_degrees"] = HEAD_ANGLE_DEGREES
    root["suspension_travel_m"] = SUSPENSION_TRAVEL_M
    root["motor_radius_m"] = MOTOR_REFERENCE_RADIUS_M

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
    for name, location in PIVOT_LOCATIONS.items():
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
    if not math.isclose(2.0 * WHEEL_RADIUS_M, 0.7366, abs_tol=1e-9):
        raise RuntimeError("29er outside wheel diameter contract changed")
    if not math.isclose(
        FRONT_AXLE[2] - REAR_AXLE[2], WHEELBASE_M, abs_tol=1e-9
    ):
        raise RuntimeError("Wheelbase contract changed")
    if not math.isclose(REAR_AXLE[1], WHEEL_RADIUS_M, abs_tol=1e-9):
        raise RuntimeError("29er wheel does not touch the ground")
    if not math.isclose(CRANK[2] - REAR_AXLE[2], CHAINSTAY_M, abs_tol=1e-9):
        raise RuntimeError("Chainstay contract changed")
    head_angle = math.degrees(math.atan2(
        HEAD_HIGH[1] - HEAD_LOW[1],
        abs(HEAD_HIGH[2] - HEAD_LOW[2]),
    ))
    if not math.isclose(head_angle, HEAD_ANGLE_DEGREES, abs_tol=1e-9):
        raise RuntimeError("Head-angle contract changed")
    if not math.isclose(SUSPENSION_TRAVEL_M, 0.190, abs_tol=1e-9):
        raise RuntimeError("Suspension-travel contract changed")
    fork_length = vector_length(vector_subtract(HEAD_LOW, FRONT_AXLE))
    visible_stanchion = vector_length(vector_subtract(HEAD_LOW, FORK_SPLIT))
    if fork_length < 0.53 or visible_stanchion < SUSPENSION_TRAVEL_M:
        raise RuntimeError("Long-travel fork silhouette collapsed")
    down_tube_y = [point[0] for point in DOWN_TUBE_PROFILE]
    down_tube_z = [point[1] for point in DOWN_TUBE_PROFILE]
    if (max(down_tube_y) - min(down_tube_y) < 0.42
            or max(down_tube_z) < HEAD_LOW[2]
            or min(down_tube_z) > CRANK[2]
            or DOWN_TUBE_HALF_WIDTH_M < 0.075):
        raise RuntimeError("Battery/down-tube silhouette collapsed")
    motor_y = [point[0] for point in MOTOR_PROFILE]
    motor_z = [point[1] for point in MOTOR_PROFILE]
    if (max(motor_y) - min(motor_y) < 0.19
            or max(motor_z) - min(motor_z) < 0.20
            or MOTOR_HALF_WIDTH_M < 0.10):
        raise RuntimeError("Motor silhouette collapsed")
    if (min(point[1] for point in LOWER_SWINGARM_PROFILE)
            > REAR_AXLE[2] - 0.01
            or max(point[1] for point in LOWER_SWINGARM_PROFILE)
            < LOWER_LINK_PIVOT[2]
            or min(point[1] for point in UPPER_SWINGARM_PROFILE)
            > REAR_AXLE[2] - 0.01
            or max(point[0] for point in UPPER_SWINGARM_PROFILE)
            < SEATSTAY_PIVOT[1]):
        raise RuntimeError("Long swingarm silhouette collapsed")
    linkage_points = (
        REAR_AXLE, LOWER_LINK_PIVOT, ROCKER_PIVOT, SEATSTAY_PIVOT
    )
    if (min(vector_length(vector_subtract(linkage_points[index + 1], point))
            for index, point in enumerate(linkage_points[:-1])) < 0.10
            or vector_length(vector_subtract(SEATSTAY_PIVOT, REAR_AXLE))
            < 0.30):
        raise RuntimeError("Four-bar linkage silhouette collapsed")
    expected_root_properties = {
        "wheelbase_m": WHEELBASE_M,
        "chainstay_m": CHAINSTAY_M,
        "head_angle_degrees": HEAD_ANGLE_DEGREES,
        "suspension_travel_m": SUSPENSION_TRAVEL_M,
        "motor_radius_m": MOTOR_REFERENCE_RADIUS_M,
    }
    for property_name, expected in expected_root_properties.items():
        if not math.isclose(float(root[property_name]), expected, abs_tol=1e-9):
            raise RuntimeError(f"Root metadata mismatch for {property_name}")
    for name, expected in PIVOT_LOCATIONS.items():
        actual = tuple(objects[name].location)
        if any(not math.isclose(actual[index], value, abs_tol=1e-6)
               for index, value in enumerate(canonical_to_blender(expected))):
            raise RuntimeError(f"Pivot location mismatch for {name}")
        if objects[name].get("physics_authority") != "external":
            raise RuntimeError(f"{name} claims physics authority")
    critical_mesh_points = {
        "GEO_MainFrame_LOD0": (
            (DOWN_TUBE_HALF_WIDTH_M,
             DOWN_TUBE_PROFILE[2][0], DOWN_TUBE_PROFILE[2][1]),
            (MOTOR_HALF_WIDTH_M, MOTOR_PROFILE[2][0], MOTOR_PROFILE[2][1]),
            (0.064, SEAT_MAST_PROFILE[1][0], SEAT_MAST_PROFILE[1][1]),
            (0.058, TOP_BRIDGE_PROFILE[1][0], TOP_BRIDGE_PROFILE[1][1]),
            HEAD_HIGH,
        ),
        "GEO_Swingarm_LOD0": (
            (0.052, LOWER_SWINGARM_PROFILE[0][0],
             LOWER_SWINGARM_PROFILE[0][1]),
            (0.050, UPPER_SWINGARM_PROFILE[0][0],
             UPPER_SWINGARM_PROFILE[0][1]),
            LOWER_LINK_PIVOT,
            SEATSTAY_PIVOT,
            ROCKER_PIVOT,
        ),
        "GEO_Fork_LOD0": (
            (0.052, FRONT_AXLE[1], FRONT_AXLE[2]),
            (0.052, FORK_SPLIT[1], FORK_SPLIT[2]),
            (0.052, HEAD_LOW[1], HEAD_LOW[2]),
        ),
        "GEO_RearShock_LOD0": (ROCKER_PIVOT, SHOCK_UPPER),
        "GEO_RearWheel_LOD0": (
            (0.0, 0.0, REAR_AXLE[2]),
            (0.075, REAR_AXLE[1], REAR_AXLE[2]),
        ),
        "GEO_FrontWheel_LOD0": (
            (0.0, 0.0, FRONT_AXLE[2]),
            (0.075, FRONT_AXLE[1], FRONT_AXLE[2]),
        ),
        "GEO_Torso_LOD0": ((0.12, 0.48, 0.185),),
    }
    for mesh_name, points in critical_mesh_points.items():
        for point in points:
            if not mesh_has_canonical_point(objects[mesh_name], point):
                raise RuntimeError(
                    f"Silhouette point {point} missing from {mesh_name}"
                )
    if len(bpy.data.materials) > 8:
        raise RuntimeError(f"Rider exceeds material budget: {len(bpy.data.materials)}")
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
    if output_path.stat().st_size > MAX_GLB_BYTES:
        raise RuntimeError(
            f"Rider exceeds GLB budget: {output_path.stat().st_size} bytes"
        )


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
