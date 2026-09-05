#!/usr/bin/env python3
"""Generate three compact, trail-edge-safe Finnish forest verge clusters."""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path
import sys
import traceback

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_forest_floor_props import (
    GRANITE_BASE_COLOR,
    bilberry_understory,
    fallen_deadwood,
    fern_understory,
    heather_understory,
    irregular_rock,
    rooted_stump,
)
from generate_tabletop import canonical_to_blender, create_empty, make_material


ASSET_ID = "EN-09-forest-verge-clusters"
ROOT_NAME = "ROOT_ForestVergeClusters"
MATERIAL_NAMES = (
    "MAT_ForestGranite",
    "MAT_ForestBark",
    "MAT_ForestEndGrain",
    "MAT_ForestUnderstory",
)
VARIANT_NAMES = (
    "GEO_VergeGraniteBilberry_LOD0",
    "GEO_VergeStumpFern_LOD0",
    "GEO_VergeDeadwoodHeather_LOD0",
)
PIVOT_NAMES = tuple(
    name.replace("GEO_", "PIVOT_").replace("_LOD0", "_TRAIL_EDGE")
    for name in VARIANT_NAMES
)
EXPECTED_TRIANGLES = {
    "GEO_VergeGraniteBilberry_LOD0": 104,
    "GEO_VergeStumpFern_LOD0": 144,
    "GEO_VergeDeadwoodHeather_LOD0": 134,
}
EXPECTED_COMPONENTS = {
    "GEO_VergeGraniteBilberry_LOD0": 3,
    "GEO_VergeStumpFern_LOD0": 4,
    "GEO_VergeDeadwoodHeather_LOD0": 3,
}
MAXIMUM_TRIANGLES = 400
MAXIMUM_VARIANT_TRIANGLES = 150
MAXIMUM_WIDTH_METERS = 3.20
MAXIMUM_DEPTH_METERS = 1.50
MAXIMUM_HEIGHT_METERS = 0.70
MINIMUM_TRAIL_CLEARANCE_METERS = 0.14
MINIMUM_COMPONENT_GAP_METERS = 0.025
EPSILON = 1.0e-7


def transformed_bounds(geometry, translation, yaw_degrees, scale):
    vertices = geometry[0]
    yaw = math.radians(yaw_degrees)
    cosine = math.cos(yaw)
    sine = math.sin(yaw)
    points = []
    for x_value, y_value, z_value in vertices:
        points.append((
            translation[0] + scale * (
                cosine * x_value - sine * z_value
            ),
            translation[1] + scale * y_value,
            translation[2] + scale * (
                sine * x_value + cosine * z_value
            ),
        ))
    return (
        tuple(min(point[axis] for point in points) for axis in range(3)),
        tuple(max(point[axis] for point in points) for axis in range(3)),
    )


def horizontal_gap(first, second):
    first_minimum, first_maximum = first
    second_minimum, second_maximum = second
    x_gap = max(
        second_minimum[0] - first_maximum[0],
        first_minimum[0] - second_maximum[0],
    )
    z_gap = max(
        second_minimum[2] - first_maximum[2],
        first_minimum[2] - second_maximum[2],
    )
    return max(x_gap, z_gap)


def append_component(
    target, geometry, material_mapping, translation, yaw_degrees=0.0,
    scale=1.0,
):
    vertices, faces, material_indices = geometry
    target_vertices, target_faces, target_material_indices = target
    yaw = math.radians(yaw_degrees)
    cosine = math.cos(yaw)
    sine = math.sin(yaw)
    base = len(target_vertices)
    for x_value, y_value, z_value in vertices:
        target_vertices.append((
            translation[0] + scale * (
                cosine * x_value - sine * z_value
            ),
            translation[1] + scale * y_value,
            translation[2] + scale * (
                sine * x_value + cosine * z_value
            ),
        ))
    target_faces.extend(
        tuple(base + vertex for vertex in face) for face in faces
    )
    target_material_indices.extend(
        material_mapping[index] for index in material_indices
    )


def build_cluster(component_specs):
    geometry = ([], [], [])
    bounds = []
    for factory, factory_arguments, material_mapping, translation, yaw, scale \
            in component_specs:
        component = factory(*factory_arguments)
        component_bounds = transformed_bounds(
            component, translation, yaw, scale
        )
        if abs(component_bounds[0][1]) > EPSILON:
            raise RuntimeError("Forest-verge component lost its ground contact")
        for previous in bounds:
            if horizontal_gap(previous, component_bounds) \
                    < MINIMUM_COMPONENT_GAP_METERS - EPSILON:
                raise RuntimeError("Forest-verge component footprints overlap")
        bounds.append(component_bounds)
        append_component(
            geometry, component, material_mapping, translation, yaw, scale
        )
    return geometry


def granite_bilberry_cluster():
    return build_cluster((
        (irregular_rock, (0.78, 0.58, 0.30, 0.4, 0.025), (0, 3),
         (0.66, 0.0, -0.34), -7.0, 1.0),
        (irregular_rock, (0.56, 0.52, 0.67, 1.8, -0.065), (0, 3),
         (0.57, 0.0, 0.43), 11.0, 1.0),
        (bilberry_understory, (), (3,),
         (1.54, 0.0, 0.34), -9.0, 0.82),
    ))


def stump_fern_cluster():
    return build_cluster((
        (rooted_stump, (), (1, 2, 3), (0.78, 0.0, 0.0), 8.0, 1.0),
        (fern_understory, (), (3,), (1.52, 0.0, 0.49), -12.0, 0.72),
        (irregular_rock, (0.78, 0.58, 0.30, 0.4, 0.025), (0, 3),
         (1.58, 0.0, -0.48), 15.0, 0.62),
        (heather_understory, (), (3,),
         (2.26, 0.0, 0.02), 7.0, 0.66),
    ))


def deadwood_heather_cluster():
    return build_cluster((
        (fallen_deadwood, (), (1, 2),
         (1.47, 0.0, -0.22), 2.0, 1.0),
        (fern_understory, (), (3,),
         (0.40, 0.0, 0.54), -15.0, 0.70),
        (heather_understory, (), (3,),
         (2.83, 0.0, 0.48), 9.0, 0.66),
    ))


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
                0.5 + 0.12 * first,
                0.5 + 0.12 * second,
            )


def validate_uv0(mesh):
    uv_layer = mesh.uv_layers.active
    if uv_layer is None:
        raise RuntimeError(f"{mesh.name} has no UV0 layer")
    for polygon in mesh.polygons:
        triangle_uvs = [
            uv_layer.data[index].uv for index in polygon.loop_indices
        ]
        if any(
            not math.isfinite(component) or component < -EPSILON
            or component > 1.0 + EPSILON
            for uv in triangle_uvs for component in uv
        ):
            raise RuntimeError(f"{mesh.name} has UV0 outside the atlas")
        first, second, third = triangle_uvs
        signed_area = (
            (second.x - first.x) * (third.y - first.y)
            - (second.y - first.y) * (third.x - first.x)
        )
        if abs(signed_area) <= EPSILON:
            raise RuntimeError(f"{mesh.name} has degenerate UV0 triangles")


def create_mesh(root, name, geometry, materials):
    vertices, faces, material_indices = geometry
    mesh = bpy.data.meshes.new(name=name)
    mesh.from_pydata(
        [canonical_to_blender(point) for point in vertices], [], faces
    )
    for material_name in MATERIAL_NAMES:
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
    result["collision_role"] = "none"
    result["placement_role"] = "scenery-only"
    result["instance_ready"] = True
    result["atlas_uv0"] = True
    result["ground_contact_y_m"] = 0.0
    result["trail_edge_clearance_m"] = MINIMUM_TRAIL_CLEARANCE_METERS
    result["component_count"] = EXPECTED_COMPONENTS[name]
    result["mirror_safe"] = True
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
    root["placement_anchor"] = "trail-edge"

    materials = {
        MATERIAL_NAMES[0]: make_material(
            MATERIAL_NAMES[0], GRANITE_BASE_COLOR
        ),
        MATERIAL_NAMES[1]: make_material(
            MATERIAL_NAMES[1], (0.36, 0.19, 0.075, 1.0)
        ),
        MATERIAL_NAMES[2]: make_material(
            MATERIAL_NAMES[2], (0.67, 0.40, 0.14, 1.0)
        ),
        MATERIAL_NAMES[3]: make_material(
            MATERIAL_NAMES[3], (0.23, 0.48, 0.13, 1.0)
        ),
    }
    geometries = {
        VARIANT_NAMES[0]: granite_bilberry_cluster(),
        VARIANT_NAMES[1]: stump_fern_cluster(),
        VARIANT_NAMES[2]: deadwood_heather_cluster(),
    }
    for name, geometry in geometries.items():
        create_mesh(root, name, geometry, materials)
        create_empty(
            root,
            name.replace("GEO_", "PIVOT_").replace(
                "_LOD0", "_TRAIL_EDGE"
            ),
            (0.0, 0.0, 0.0),
            0.08,
            {
                "visual_only": True,
                "physics_authority": "external",
                "ground_contact_y_m": 0.0,
                "placement_anchor": "trail-edge",
            },
        )
    return root


def canonical_bounds(obj):
    points = [
        (vertex.co.x, vertex.co.z, -vertex.co.y)
        for vertex in obj.data.vertices
    ]
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
    if set(objects) != {ROOT_NAME, *VARIANT_NAMES, *PIVOT_NAMES}:
        raise RuntimeError("Forest-verge scene node inventory changed")

    total_triangles = 0
    for name in VARIANT_NAMES:
        obj = objects[name]
        if obj.location.length > EPSILON or any(
            abs(value) > EPSILON for value in obj.rotation_euler
        ) or any(abs(value - 1.0) > EPSILON for value in obj.scale):
            raise RuntimeError(f"{name} transform is not applied")
        if obj.get("physics_authority") != "external" \
                or obj.get("collision_role") != "none":
            raise RuntimeError(f"{name} claims gameplay authority")
        if not obj.data.uv_layers or obj.get("instance_ready") is not True:
            raise RuntimeError(f"{name} is not atlas/instance ready")
        validate_uv0(obj.data)
        minimum, maximum = canonical_bounds(obj)
        if abs(minimum[1]) > EPSILON or maximum[1] \
                > MAXIMUM_HEIGHT_METERS + EPSILON:
            raise RuntimeError(f"{name} is floating, buried or too tall")
        if minimum[0] < MINIMUM_TRAIL_CLEARANCE_METERS - EPSILON:
            raise RuntimeError(f"{name} violates the trail-edge clearance")
        if maximum[0] - minimum[0] > MAXIMUM_WIDTH_METERS + EPSILON \
                or maximum[2] - minimum[2] \
                > MAXIMUM_DEPTH_METERS + EPSILON:
            raise RuntimeError(f"{name} exceeds its placement footprint")
        count = len(obj.data.polygons)
        if count != EXPECTED_TRIANGLES[name] \
                or count > MAXIMUM_VARIANT_TRIANGLES:
            raise RuntimeError(f"{name} triangle contract changed: {count}")
        if any(polygon.loop_total != 3 or polygon.area <= EPSILON
               for polygon in obj.data.polygons):
            raise RuntimeError(f"{name} has invalid triangles")
        used_materials = {polygon.material_index for polygon in obj.data.polygons}
        if not used_materials.issubset(set(range(len(MATERIAL_NAMES)))):
            raise RuntimeError(f"{name} has an invalid material slot")
        total_triangles += count

    if total_triangles > MAXIMUM_TRIANGLES:
        raise RuntimeError(
            f"Forest-verge set exceeds triangle budget: {total_triangles}"
        )
    for name in PIVOT_NAMES:
        pivot = objects[name]
        if pivot.parent != root or pivot.location.length > EPSILON:
            raise RuntimeError(f"{name} is not the canonical trail-edge pivot")
        if pivot.get("ground_contact_y_m") != 0.0:
            raise RuntimeError(f"{name} ground contact changed")
    return total_triangles


def export_glb(output_path):
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


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(
        description="Generate Finnish forest-verge cluster GLB"
    )
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
