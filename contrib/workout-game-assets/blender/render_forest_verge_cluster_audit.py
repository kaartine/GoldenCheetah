#!/usr/bin/env python3
"""Render matched sparse-prop and forest-verge cluster catalogs."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import traceback

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_forest_verge_clusters import VARIANT_NAMES
from generate_tabletop import canonical_to_blender, make_material


CATALOG_WIDTH = 960
CATALOG_HEIGHT = 540
CELL_COLUMNS = 3
CELL_ROWS = 2
CELL_WIDTH = CATALOG_WIDTH // CELL_COLUMNS
CELL_HEIGHT = CATALOG_HEIGHT // CELL_ROWS
VERTICAL_FOV_DEGREES = 47.0
CAMERA_DISTANCE_METERS = 4.45
CAMERA_TARGET_METERS = (1.55, 0.30, 0.0)
BEFORE_VARIANTS = (
    "GEO_GraniteLow_LOD0",
    "GEO_StumpRooted_LOD0",
    "GEO_DeadwoodFallen_LOD0",
)
VIEW_CAMERAS = (
    ("front", (1.55, 1.42, -4.45)),
    ("left-three-quarter", (-1.60, 1.42, -3.15)),
    ("rear-three-quarter", (-1.60, 1.42, 3.15)),
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def point_at(obj, canonical_target):
    target = Vector(canonical_to_blender(canonical_target))
    obj.rotation_euler = (target - obj.location).to_track_quat(
        "-Z", "Y"
    ).to_euler()


def add_stage(scene):
    ground_material = make_material(
        "AUDIT_ForestGround", (0.105, 0.16, 0.105, 1.0)
    )
    scale_material = make_material(
        "AUDIT_TrailWidthScale", (0.60, 0.52, 0.32, 1.0)
    )
    bpy.ops.mesh.primitive_plane_add(
        size=4.2, location=canonical_to_blender((1.55, -0.006, 0.0))
    )
    ground = bpy.context.object
    ground.name = "AUDIT_GroundDatum"
    ground.data.materials.append(ground_material)

    bpy.ops.mesh.primitive_cube_add(
        location=canonical_to_blender((1.55, 0.008, -0.74))
    )
    scale_bar = bpy.context.object
    scale_bar.name = "AUDIT_TrailWidth_1_36m"
    scale_bar.dimensions = (1.36, 0.025, 0.016)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    scale_bar.data.materials.append(scale_material)

    camera_data = bpy.data.cameras.new("AUDIT_Camera_47deg")
    camera_data.sensor_fit = "VERTICAL"
    camera_data.sensor_height = 32.0
    camera_data.lens = camera_data.sensor_height / (
        2.0 * math.tan(math.radians(VERTICAL_FOV_DEGREES) * 0.5)
    )
    camera = bpy.data.objects.new("AUDIT_Camera_47deg", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    key_data = bpy.data.lights.new("AUDIT_Key", type="AREA")
    key_data.energy = 560.0
    key_data.shape = "DISK"
    key_data.size = 3.2
    key = bpy.data.objects.new("AUDIT_Key", key_data)
    bpy.context.collection.objects.link(key)
    key.location = canonical_to_blender((-1.8, 4.8, -3.2))
    point_at(key, CAMERA_TARGET_METERS)

    fill_data = bpy.data.lights.new("AUDIT_Fill", type="AREA")
    fill_data.energy = 190.0
    fill_data.size = 2.6
    fill = bpy.data.objects.new("AUDIT_Fill", fill_data)
    bpy.context.collection.objects.link(fill)
    fill.location = canonical_to_blender((4.2, 2.4, -0.6))
    point_at(fill, CAMERA_TARGET_METERS)

    scene.world = bpy.data.worlds.new("AUDIT_World")
    scene.world.color = (0.055, 0.07, 0.075)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 12
    scene.cycles.use_denoising = False
    scene.render.resolution_x = CELL_WIDTH
    scene.render.resolution_y = CELL_HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    return camera


def copy_cell(target, source, column, row):
    x_offset = column * CELL_WIDTH
    y_offset = (CELL_ROWS - 1 - row) * CELL_HEIGHT
    source_pixels = list(source.pixels[:])
    for source_y in range(CELL_HEIGHT):
        source_start = source_y * CELL_WIDTH * 4
        target_start = (
            (y_offset + source_y) * CATALOG_WIDTH + x_offset
        ) * 4
        target[target_start:target_start + CELL_WIDTH * 4] = source_pixels[
            source_start:source_start + CELL_WIDTH * 4
        ]


def render_catalog(
    scene, camera, before_meshes, after_meshes, output_path,
    camera_position, scratch,
):
    camera.location = canonical_to_blender(camera_position)
    point_at(camera, CAMERA_TARGET_METERS)
    target_pixels = [0.0] * (CATALOG_WIDTH * CATALOG_HEIGHT * 4)

    for row, inventory in enumerate((BEFORE_VARIANTS, VARIANT_NAMES)):
        for column, name in enumerate(inventory):
            for candidate in before_meshes.values():
                candidate.hide_render = True
            for candidate in after_meshes.values():
                candidate.hide_render = True
            selected = (
                before_meshes[name] if row == 0 else after_meshes[name]
            )
            selected.hide_render = False
            bpy.context.view_layer.update()
            cell_path = scratch / f"cell-{row}-{column}.png"
            scene.render.filepath = str(cell_path)
            bpy.ops.render.render(write_still=True)
            cell = bpy.data.images.load(str(cell_path), check_existing=False)
            copy_cell(target_pixels, cell, column, row)
            bpy.data.images.remove(cell)

    catalog = bpy.data.images.new(
        f"EN09_{output_path.stem}",
        width=CATALOG_WIDTH,
        height=CATALOG_HEIGHT,
        alpha=True,
    )
    catalog.pixels.foreach_set(target_pixels)
    catalog.file_format = "PNG"
    catalog.filepath_raw = str(output_path)
    catalog.save()
    bpy.data.images.remove(catalog)


def import_named_meshes(asset_path, names, x_offset=0.0):
    existing_objects = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=str(asset_path))
    imported_meshes = [
        obj for obj in bpy.context.scene.objects
        if obj not in existing_objects and obj.type == "MESH"
    ]
    meshes = {
        obj.name: obj for obj in imported_meshes if obj.name in names
    }
    if set(meshes) != set(names):
        raise RuntimeError(f"GLB inventory changed: {asset_path.name}")
    for mesh in imported_meshes:
        mesh.hide_render = True
    for mesh in meshes.values():
        mesh.location = canonical_to_blender((x_offset, 0.0, 0.0))
    return meshes


def render(source_asset, cluster_asset, output_directory):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    before_meshes = import_named_meshes(
        source_asset, BEFORE_VARIANTS, x_offset=1.55
    )
    after_meshes = import_named_meshes(cluster_asset, VARIANT_NAMES)
    scene = bpy.context.scene
    camera = add_stage(scene)
    output_directory.mkdir(parents=True, exist_ok=True)

    render_paths = []
    with tempfile.TemporaryDirectory(prefix="gc-en09-audit-") as temporary:
        scratch = Path(temporary)
        for view_name, camera_position in VIEW_CAMERAS:
            output_path = output_directory / f"EN-09-{view_name}.png"
            render_catalog(
                scene, camera, before_meshes, after_meshes, output_path,
                camera_position, scratch,
            )
            render_paths.append(output_path)

    metadata = {
        "format": "goldencheetah-workout-game-asset-audit-1",
        "assetId": "EN-09-forest-verge-clusters",
        "sourcePropGlb": source_asset.name,
        "sourcePropGlbSha256": sha256(source_asset),
        "clusterGlb": cluster_asset.name,
        "clusterGlbSha256": sha256(cluster_asset),
        "catalog": {
            "widthPixels": CATALOG_WIDTH,
            "heightPixels": CATALOG_HEIGHT,
            "cellColumns": CELL_COLUMNS,
            "cellRows": CELL_ROWS,
            "rows": ["before-isolated-prop", "after-verge-cluster"],
            "beforeCellOrder": list(BEFORE_VARIANTS),
            "afterCellOrder": list(VARIANT_NAMES),
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "cameraDistanceMeters": CAMERA_DISTANCE_METERS,
            "cameraTargetMeters": list(CAMERA_TARGET_METERS),
            "cameraPositionsMeters": [
                list(position) for _, position in VIEW_CAMERAS
            ],
            "trailWidthScaleBarMeters": 1.36,
            "renderEngine": "CYCLES_CPU_12_SAMPLES",
            "viewTransform": "Standard",
            "look": "Medium High Contrast",
        },
        "renders": [
            {
                "view": view_name,
                "path": path.name,
                "sha256": sha256(path),
            }
            for (view_name, _), path in zip(VIEW_CAMERAS, render_paths)
        ],
    }
    metadata_path = output_directory / "EN-09-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print("Rendered", ", ".join(str(path) for path in render_paths))


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(
        description="Render before/after forest-verge cluster audits"
    )
    parser.add_argument("--source-asset", required=True)
    parser.add_argument("--cluster-asset", required=True)
    parser.add_argument("--output-dir", required=True)
    parsed = parser.parse_args(arguments)
    source_asset = Path(os.path.expanduser(parsed.source_asset)).resolve()
    cluster_asset = Path(os.path.expanduser(parsed.cluster_asset)).resolve()
    output = Path(os.path.expanduser(parsed.output_dir)).resolve()
    for asset in (source_asset, cluster_asset):
        if asset.suffix.lower() != ".glb" or not asset.is_file():
            raise RuntimeError("audit inputs must be existing GLB files")
    render(source_asset, cluster_asset, output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
