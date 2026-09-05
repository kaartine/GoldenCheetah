#!/usr/bin/env python3
"""Generate a compact project-authored Finnish forest-floor prop kit."""

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


ASSET_ID = "EN-08-forest-floor-props"
ROOT_NAME = "ROOT_ForestFloorProps"
MATERIAL_NAMES = (
    "MAT_ForestGranite",
    "MAT_ForestBark",
    "MAT_ForestEndGrain",
    "MAT_ForestUnderstory",
)
VARIANT_NAMES = (
    "GEO_GraniteLow_LOD0",
    "GEO_GraniteUpright_LOD0",
    "GEO_GraniteSlab_LOD0",
    "GEO_StumpRooted_LOD0",
    "GEO_DeadwoodFallen_LOD0",
    "GEO_UnderstoryFern_LOD0",
    "GEO_UnderstoryBilberry_LOD0",
    "GEO_UnderstoryHeather_LOD0",
)
PIVOT_NAMES = tuple(
    name.replace("GEO_", "PIVOT_").replace("_LOD0", "_BASE")
    for name in VARIANT_NAMES
)
REQUIRED_NAMES = {ROOT_NAME, *VARIANT_NAMES, *PIVOT_NAMES}
EXPECTED_TRIANGLES = {
    "GEO_GraniteLow_LOD0": 32,
    "GEO_GraniteUpright_LOD0": 32,
    "GEO_GraniteSlab_LOD0": 32,
    "GEO_StumpRooted_LOD0": 50,
    "GEO_DeadwoodFallen_LOD0": 72,
    "GEO_UnderstoryFern_LOD0": 20,
    "GEO_UnderstoryBilberry_LOD0": 40,
    "GEO_UnderstoryHeather_LOD0": 42,
}
EXPECTED_MATERIAL_PASSES = {
    name: 2 if name in {"GEO_StumpRooted_LOD0", "GEO_DeadwoodFallen_LOD0"}
    else 1
    for name in VARIANT_NAMES
}
MAXIMUM_TRIANGLES = 480
MAXIMUM_VARIANT_TRIANGLES = 96
MAXIMUM_WIDTH_METERS = 2.20
MAXIMUM_DEPTH_METERS = 0.80
MAXIMUM_HEIGHT_METERS = 0.70
DEADWOOD_MAXIMUM_HEIGHT_METERS = 0.34
EPSILON = 1.0e-7


def add_face(faces, material_indices, vertices, material_index=0):
    faces.append(tuple(vertices))
    material_indices.append(material_index)


def irregular_rock(width, depth, height, phase, lean_x=0.0):
    vertices = []
    faces = []
    material_indices = []
    sides = 8
    lower = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        noise = 1.0 + 0.09 * math.sin(phase + side * 2.17)
        vertices.append((
            0.50 * width * noise * math.cos(angle),
            0.0,
            0.50 * depth * noise * math.sin(angle),
        ))
    shoulder = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        noise = 0.73 + 0.10 * math.cos(phase * 0.7 + side * 1.71)
        y_value = height * (0.70 + 0.08 * math.sin(phase + side * 1.37))
        vertices.append((
            lean_x + 0.50 * width * noise * math.cos(angle),
            y_value,
            0.50 * depth * noise * math.sin(angle),
        ))
    bottom_center = len(vertices)
    vertices.append((0.0, 0.0, 0.0))
    top = len(vertices)
    vertices.append((lean_x * 1.35, height, -0.04 * depth))
    for side in range(sides):
        following = (side + 1) % sides
        add_face(faces, material_indices,
                 (lower + side, shoulder + side, shoulder + following))
        add_face(faces, material_indices,
                 (lower + side, shoulder + following, lower + following))
        add_face(faces, material_indices,
                 (shoulder + side, top, shoulder + following))
        add_face(faces, material_indices,
                 (bottom_center, lower + following, lower + side))
    return vertices, faces, material_indices


def rooted_stump():
    vertices = []
    faces = []
    material_indices = []
    sides = 8
    lower = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        radius = 0.235 * (1.0 + 0.05 * math.sin(side * 1.9))
        vertices.append((radius * math.cos(angle), 0.0,
                         radius * math.sin(angle)))
    upper = len(vertices)
    for side in range(sides):
        angle = 2.0 * math.pi * side / sides
        radius = 0.185 * (1.0 + 0.06 * math.cos(side * 1.6))
        top_y = 0.49 + 0.045 * math.sin(side * 2.35)
        vertices.append((0.018 + radius * math.cos(angle), top_y,
                         -0.012 + radius * math.sin(angle)))
    top_center = len(vertices)
    vertices.append((0.018, 0.505, -0.012))
    bottom_center = len(vertices)
    vertices.append((0.0, 0.0, 0.0))
    for side in range(sides):
        following = (side + 1) % sides
        add_face(faces, material_indices,
                 (lower + side, upper + side, upper + following), 0)
        add_face(faces, material_indices,
                 (lower + side, upper + following, lower + following), 0)
        add_face(faces, material_indices,
                 (upper + side, top_center, upper + following), 1)
        if side < sides - 2:
            add_face(faces, material_indices,
                     (bottom_center, lower + following, lower + side), 0)

    for root_index, angle in enumerate((0.12, 1.42, 2.65, 3.91, 5.15)):
        tangent_x = -math.sin(angle)
        tangent_z = math.cos(angle)
        direction_x = math.cos(angle)
        direction_z = math.sin(angle)
        half_width = 0.075 - root_index * 0.004
        inner_radius = 0.18
        outer_radius = 0.38 + 0.025 * math.sin(root_index * 2.1)
        base = len(vertices)
        vertices.extend((
            (direction_x * inner_radius + tangent_x * half_width, 0.0,
             direction_z * inner_radius + tangent_z * half_width),
            (direction_x * inner_radius - tangent_x * half_width, 0.0,
             direction_z * inner_radius - tangent_z * half_width),
            (direction_x * outer_radius, 0.0, direction_z * outer_radius),
            (direction_x * 0.21, 0.13, direction_z * 0.21),
        ))
        for indices in ((0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)):
            add_face(faces, material_indices,
                     tuple(base + index for index in indices), 0)
    return vertices, faces, material_indices


def fallen_deadwood():
    vertices = []
    faces = []
    material_indices = []
    sides = 8
    rings = (
        (-0.98, 0.130, -0.090, 0.130),
        (-0.34, 0.160, 0.035, 0.160),
        (0.48, 0.130, 0.065, 0.120),
        (0.97, 0.070, -0.040, 0.065),
    )
    starts = []
    for x_value, center_y, center_z, radius in rings:
        starts.append(len(vertices))
        for side in range(sides):
            angle = 2.0 * math.pi * side / sides
            vertices.append((
                x_value,
                center_y + radius * math.sin(angle),
                center_z + radius * math.cos(angle),
            ))
    for first, second in zip(starts, starts[1:]):
        for side in range(sides):
            following = (side + 1) % sides
            add_face(faces, material_indices,
                     (first + side, second + side, second + following), 0)
            add_face(faces, material_indices,
                     (first + side, second + following, first + following), 0)
    for ring_index, center in ((0, (-0.98, 0.130, -0.090)),
                               (3, (0.97, 0.070, -0.040))):
        center_index = len(vertices)
        vertices.append(center)
        start = starts[ring_index]
        for side in range(sides):
            following = (side + 1) % sides
            order = (center_index, start + side, start + following)
            if ring_index == 0:
                order = (center_index, start + following, start + side)
            add_face(faces, material_indices, order, 1)

    for base_x, direction in ((-0.30, -1.0), (0.48, 1.0)):
        base = len(vertices)
        vertices.extend((
            (base_x - 0.06, 0.20, -0.08),
            (base_x + 0.06, 0.19, -0.06),
            (base_x, 0.28, -0.02),
            (base_x + direction * 0.26, 0.055, -0.22),
        ))
        for indices in ((0, 1, 2), (0, 3, 1), (1, 3, 2), (2, 3, 0)):
            add_face(faces, material_indices,
                     tuple(base + index for index in indices), 0)
    return vertices, faces, material_indices


def fern_understory():
    vertices = []
    faces = []
    material_indices = []
    for frond in range(10):
        angle = 2.0 * math.pi * frond / 10.0 + 0.16 * (frond % 2)
        length = 0.30 + 0.095 * ((frond * 3) % 5) / 4.0
        width = 0.075 + 0.012 * (frond % 3)
        direction = (math.cos(angle), math.sin(angle))
        tangent = (-direction[1], direction[0])
        base = len(vertices)
        vertices.extend((
            (0.0, 0.0, 0.0),
            (direction[0] * length * 0.52 + tangent[0] * width,
             0.13 + 0.025 * (frond % 2),
             direction[1] * length * 0.52 + tangent[1] * width),
            (direction[0] * length, 0.055 + 0.015 * (frond % 3),
             direction[1] * length),
            (direction[0] * length * 0.52 - tangent[0] * width,
             0.13 + 0.025 * (frond % 2),
             direction[1] * length * 0.52 - tangent[1] * width),
        ))
        add_face(faces, material_indices, (base, base + 1, base + 2))
        add_face(faces, material_indices, (base, base + 2, base + 3))
    return vertices, faces, material_indices


def append_octahedron(vertices, faces, material_indices, center, radii):
    center_x, center_y, center_z = center
    radius_x, radius_y, radius_z = radii
    base = len(vertices)
    vertices.extend((
        (center_x + radius_x, center_y, center_z),
        (center_x - radius_x, center_y, center_z),
        (center_x, center_y + radius_y, center_z),
        (center_x, center_y - radius_y, center_z),
        (center_x, center_y, center_z + radius_z),
        (center_x, center_y, center_z - radius_z),
    ))
    for indices in (
        (2, 0, 4), (2, 4, 1), (2, 1, 5), (2, 5, 0),
        (3, 4, 0), (3, 1, 4), (3, 5, 1), (3, 0, 5),
    ):
        add_face(faces, material_indices,
                 tuple(base + index for index in indices))


def bilberry_understory():
    vertices = []
    faces = []
    material_indices = []
    clumps = (
        (-0.18, 0.13, -0.10, 0.17, 0.13, 0.14),
        (0.13, 0.16, -0.13, 0.19, 0.16, 0.15),
        (-0.08, 0.20, 0.13, 0.20, 0.17, 0.16),
        (0.22, 0.12, 0.12, 0.15, 0.12, 0.13),
        (0.01, 0.10, 0.00, 0.18, 0.10, 0.16),
    )
    for x_value, y_value, z_value, rx, ry, rz in clumps:
        append_octahedron(vertices, faces, material_indices,
                          (x_value, y_value, z_value), (rx, ry, rz))
    return vertices, faces, material_indices


def heather_understory():
    vertices = []
    faces = []
    material_indices = []
    stems = (
        (-0.23, -0.09, 0.26), (-0.14, 0.13, 0.36),
        (-0.04, -0.02, 0.31), (0.07, 0.16, 0.38),
        (0.16, -0.12, 0.33), (0.25, 0.08, 0.27),
        (0.00, 0.05, 0.29),
    )
    for stem, (x_value, z_value, height) in enumerate(stems):
        radius = 0.075 + 0.008 * (stem % 3)
        base = len(vertices)
        vertices.extend((
            (x_value - radius, 0.0, z_value - radius * 0.7),
            (x_value + radius, 0.0, z_value - radius * 0.7),
            (x_value + radius, 0.0, z_value + radius * 0.7),
            (x_value - radius, 0.0, z_value + radius * 0.7),
            (x_value + 0.025 * math.sin(stem * 1.7), height, z_value),
        ))
        for indices in ((0, 1, 4), (1, 2, 4), (2, 3, 4), (3, 0, 4),
                        (0, 2, 1), (0, 3, 2)):
            add_face(faces, material_indices,
                     tuple(base + index for index in indices))
    return vertices, faces, material_indices


def assign_uv0(mesh):
    uv_layer = mesh.uv_layers.new(name="UVMap")
    for polygon in mesh.polygons:
        normal = tuple(abs(component) for component in polygon.normal)
        for loop_index in polygon.loop_indices:
            coordinate = mesh.vertices[mesh.loops[loop_index].vertex_index].co
            if normal[2] >= max(normal[0], normal[1]):
                first, second = coordinate.x, coordinate.y
            elif normal[0] >= normal[1]:
                first, second = coordinate.y, coordinate.z
            else:
                first, second = coordinate.x, coordinate.z
            uv_layer.data[loop_index].uv = (
                0.5 + 0.40 * first,
                0.5 + 0.40 * second,
            )


def create_mesh(root, name, geometry, materials, material_slots, properties):
    vertices, faces, material_indices = geometry
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata([canonical_to_blender(point) for point in vertices], [], faces)
    for material_name in material_slots:
        mesh.materials.append(materials[material_name])
    mesh.update(calc_edges=True)
    if mesh.validate(verbose=True, clean_customdata=False):
        raise RuntimeError(f"Blender repaired generated geometry in {name}")
    assign_uv0(mesh)
    for polygon, material_index in zip(mesh.polygons, material_indices):
        polygon.material_index = material_index
        polygon.use_smooth = False
    result = bpy.data.objects.new(name=name, object_data=mesh)
    bpy.context.collection.objects.link(result)
    result.parent = root
    result["physics_authority"] = "external"
    result["instance_ready"] = True
    result["ground_contact_y_m"] = 0.0
    result["atlas_uv0"] = True
    for key, value in properties.items():
        result[key] = value
    return result


def build_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = 1.0
    root = bpy.data.objects.new(name=ROOT_NAME, object_data=None)
    bpy.context.collection.objects.link(root)
    root.empty_display_type = "PLAIN_AXES"
    root.empty_display_size = 0.25
    root["asset_id"] = ASSET_ID
    root["unit_meters"] = 1.0
    root["up_axis"] = "+Y"
    root["forward_axis"] = "+Z"
    root["physics_authority"] = "external"
    root["project_generated"] = True
    root["generated_output_license"] = "CC0-1.0"
    root["atlas_ready"] = True

    materials = {
        MATERIAL_NAMES[0]: make_material(MATERIAL_NAMES[0],
                                         (0.34, 0.36, 0.37, 1.0)),
        MATERIAL_NAMES[1]: make_material(MATERIAL_NAMES[1],
                                         (0.25, 0.14, 0.065, 1.0)),
        MATERIAL_NAMES[2]: make_material(MATERIAL_NAMES[2],
                                         (0.42, 0.29, 0.15, 1.0)),
        MATERIAL_NAMES[3]: make_material(MATERIAL_NAMES[3],
                                         (0.12, 0.29, 0.13, 1.0)),
    }
    geometries = {
        VARIANT_NAMES[0]: (irregular_rock(0.78, 0.58, 0.30, 0.4, 0.025),
                           (MATERIAL_NAMES[0],),
                           {"silhouette": "low-rounded-granite"}),
        VARIANT_NAMES[1]: (irregular_rock(0.56, 0.52, 0.67, 1.8, -0.065),
                           (MATERIAL_NAMES[0],),
                           {"silhouette": "upright-granite"}),
        VARIANT_NAMES[2]: (irregular_rock(1.04, 0.68, 0.41, 3.2, 0.085),
                           (MATERIAL_NAMES[0],),
                           {"silhouette": "slab-granite"}),
        VARIANT_NAMES[3]: (rooted_stump(),
                           (MATERIAL_NAMES[1], MATERIAL_NAMES[2]),
                           {"silhouette": "rooted-broken-stump"}),
        VARIANT_NAMES[4]: (fallen_deadwood(),
                           (MATERIAL_NAMES[1], MATERIAL_NAMES[2]),
                           {"silhouette": "crooked-tapered-deadwood",
                            "placement_role": "scenery-only",
                            "collision_role": "none",
                            "feature_role": "none"}),
        VARIANT_NAMES[5]: (fern_understory(),
                           (MATERIAL_NAMES[3],),
                           {"silhouette": "fern-rosette"}),
        VARIANT_NAMES[6]: (bilberry_understory(),
                           (MATERIAL_NAMES[3],),
                           {"silhouette": "bilberry-mounded"}),
        VARIANT_NAMES[7]: (heather_understory(),
                           (MATERIAL_NAMES[3],),
                           {"silhouette": "heather-upright"}),
    }
    for name, (geometry, slots, properties) in geometries.items():
        create_mesh(root, name, geometry, materials, slots, properties)
        create_empty(root, name.replace("GEO_", "PIVOT_").replace(
            "_LOD0", "_BASE"), (0.0, 0.0, 0.0), 0.07,
            {"visual_only": True, "physics_authority": "external",
             "ground_contact_y_m": 0.0})
    return root


def canonical_bounds(obj):
    points = [(vertex.co.x, vertex.co.z, -vertex.co.y)
              for vertex in obj.data.vertices]
    return (
        tuple(min(point[axis] for point in points) for axis in range(3)),
        tuple(max(point[axis] for point in points) for axis in range(3)),
    )


def self_check(root):
    if bpy.app.version_string != "4.0.2":
        raise RuntimeError(
            f"Blender 4.0.2 is required, found {bpy.app.version_string}"
        )
    objects = {obj.name: obj for obj in bpy.context.scene.objects}
    if set(objects) != REQUIRED_NAMES:
        raise RuntimeError("Forest-floor prop scene node inventory changed")
    total_triangles = 0
    triangle_counts = {}
    for name in VARIANT_NAMES:
        obj = objects[name]
        if obj.location.length > EPSILON or any(
            abs(value) > EPSILON for value in obj.rotation_euler
        ) or any(abs(value - 1.0) > EPSILON for value in obj.scale):
            raise RuntimeError(f"{name} transform is not applied")
        if obj.get("physics_authority") != "external":
            raise RuntimeError(f"{name} claims physics authority")
        if obj.get("instance_ready") is not True or not obj.data.uv_layers:
            raise RuntimeError(f"{name} is not atlas/instancing ready")
        if len(obj.data.materials) != EXPECTED_MATERIAL_PASSES[name]:
            raise RuntimeError(f"{name} material-pass contract changed")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in obj.data.polygons):
            raise RuntimeError(f"{name} has invalid triangles")
        uv_data = obj.data.uv_layers.active.data
        for polygon in obj.data.polygons:
            uv_points = [uv_data[index].uv for index in polygon.loop_indices]
            uv_area_twice = abs(
                (uv_points[1].x - uv_points[0].x)
                * (uv_points[2].y - uv_points[0].y)
                - (uv_points[2].x - uv_points[0].x)
                * (uv_points[1].y - uv_points[0].y)
            )
            if uv_area_twice <= EPSILON or any(
                not math.isfinite(component) or component < -EPSILON
                or component > 1.0 + EPSILON
                for uv in uv_points for component in uv
            ):
                raise RuntimeError(f"{name} has invalid atlas UV triangles")
        minimum, maximum = canonical_bounds(obj)
        if minimum[1] < -EPSILON or abs(minimum[1]) > EPSILON:
            raise RuntimeError(f"{name} ground contact is not at Y=0")
        if maximum[0] - minimum[0] > MAXIMUM_WIDTH_METERS + EPSILON:
            raise RuntimeError(f"{name} exceeds width contract")
        if maximum[2] - minimum[2] > MAXIMUM_DEPTH_METERS + EPSILON:
            raise RuntimeError(f"{name} exceeds depth contract")
        if maximum[1] > MAXIMUM_HEIGHT_METERS + EPSILON:
            raise RuntimeError(f"{name} exceeds height contract")
        count = len(obj.data.polygons)
        if count != EXPECTED_TRIANGLES[name]:
            raise RuntimeError(f"{name} triangle contract changed: {count}")
        if count > MAXIMUM_VARIANT_TRIANGLES:
            raise RuntimeError(f"{name} exceeds per-variant triangle budget")
        triangle_counts[name] = count
        total_triangles += count
    for name in PIVOT_NAMES:
        pivot = objects[name]
        if pivot.parent != root or pivot.location.length > EPSILON:
            raise RuntimeError(f"{name} is not at its root ground contact")
        if pivot.get("ground_contact_y_m") != 0.0:
            raise RuntimeError(f"{name} ground-contact metadata changed")
    deadwood_bounds = canonical_bounds(objects["GEO_DeadwoodFallen_LOD0"])
    if deadwood_bounds[1][1] > DEADWOOD_MAXIMUM_HEIGHT_METERS + EPSILON:
        raise RuntimeError("Decorative deadwood exceeds non-feature height")
    if len({material.name for material in bpy.data.materials}) != 4:
        raise RuntimeError("Forest-floor shared material inventory changed")
    if total_triangles > MAXIMUM_TRIANGLES:
        raise RuntimeError(
            f"Forest-floor prop set exceeds triangle budget: {total_triangles}"
        )
    return total_triangles, triangle_counts


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
    parser = argparse.ArgumentParser(description="Generate forest-floor prop GLB")
    parser.add_argument("--output", required=True)
    output = Path(os.path.expanduser(parser.parse_args(arguments).output)).resolve()
    if output.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    root = build_scene()
    triangles, counts = self_check(root)
    export_glb(output)
    print("Generated", output, f"({triangles} triangles: {counts})")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
