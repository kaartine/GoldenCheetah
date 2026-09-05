#!/usr/bin/env python3
"""Render fixed-camera multi-angle catalogs from the exported prop GLB."""

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
from generate_forest_floor_props import VARIANT_NAMES
from generate_tabletop import canonical_to_blender, make_material


CATALOG_WIDTH = 960
CATALOG_HEIGHT = 540
CELL_COLUMNS = 4
CELL_ROWS = 2
CELL_WIDTH = CATALOG_WIDTH // CELL_COLUMNS
CELL_HEIGHT = CATALOG_HEIGHT // CELL_ROWS
VERTICAL_FOV_DEGREES = 47.0
CAMERA_DISTANCE_METERS = 3.0
CAMERA_CANONICAL_POSITION = (0.0, 1.25, -CAMERA_DISTANCE_METERS)
CAMERA_CANONICAL_TARGET = (0.0, 0.28, 0.0)
VIEW_ROTATIONS = (
    ("front", 0.0),
    ("left-three-quarter", 45.0),
    ("rear-three-quarter", 135.0),
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def point_camera(camera, canonical_target):
    target = Vector(canonical_to_blender(canonical_target))
    camera.rotation_euler = (target - camera.location).to_track_quat(
        "-Z", "Y"
    ).to_euler()


def add_audit_stage(scene):
    ground_material = make_material(
        "AUDIT_ForestGround", (0.105, 0.16, 0.105, 1.0)
    )
    scale_material = make_material(
        "AUDIT_TrailWidthScale", (0.60, 0.52, 0.32, 1.0)
    )
    bpy.ops.mesh.primitive_plane_add(size=3.0, location=(0.0, 0.0, -0.006))
    ground = bpy.context.object
    ground.name = "AUDIT_GroundDatum"
    ground.data.materials.append(ground_material)

    # A 1.36-metre bar at the ground datum shows the accepted trail width.
    bpy.ops.mesh.primitive_cube_add(location=(0.0, -0.72, 0.008))
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
    camera.location = canonical_to_blender(CAMERA_CANONICAL_POSITION)
    point_camera(camera, CAMERA_CANONICAL_TARGET)
    scene.camera = camera

    sun_data = bpy.data.lights.new("AUDIT_Key", type="AREA")
    sun_data.energy = 520.0
    sun_data.shape = "DISK"
    sun_data.size = 3.0
    sun = bpy.data.objects.new("AUDIT_Key", sun_data)
    bpy.context.collection.objects.link(sun)
    sun.location = canonical_to_blender((-2.8, 4.6, -3.5))
    point_camera(sun, (0.0, 0.15, 0.0))

    fill_data = bpy.data.lights.new("AUDIT_Fill", type="AREA")
    fill_data.energy = 180.0
    fill_data.size = 2.5
    fill = bpy.data.objects.new("AUDIT_Fill", fill_data)
    bpy.context.collection.objects.link(fill)
    fill.location = canonical_to_blender((2.5, 2.1, -0.8))
    point_camera(fill, (0.0, 0.18, 0.0))

    scene.world = bpy.data.worlds.new("AUDIT_World")
    scene.world.color = (0.055, 0.07, 0.075)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 16
    scene.cycles.use_denoising = False
    scene.render.resolution_x = CELL_WIDTH
    scene.render.resolution_y = CELL_HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.render.filepath = ""
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0


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


def render_catalog(scene, meshes, output_path, rotation_degrees, scratch):
    target_pixels = [0.0] * (CATALOG_WIDTH * CATALOG_HEIGHT * 4)
    rotation = math.radians(rotation_degrees)
    for index, name in enumerate(VARIANT_NAMES):
        for candidate_name, candidate in meshes.items():
            candidate.hide_render = candidate_name != name
            candidate.rotation_mode = "XYZ"
            candidate.rotation_euler = (0.0, 0.0, rotation)
        bpy.context.view_layer.update()
        cell_path = scratch / f"cell-{index}.png"
        scene.render.filepath = str(cell_path)
        bpy.ops.render.render(write_still=True)
        cell = bpy.data.images.load(str(cell_path), check_existing=False)
        copy_cell(target_pixels, cell, index % CELL_COLUMNS,
                  index // CELL_COLUMNS)
        bpy.data.images.remove(cell)

    catalog = bpy.data.images.new(
        f"EN08_{output_path.stem}", width=CATALOG_WIDTH,
        height=CATALOG_HEIGHT, alpha=True
    )
    catalog.pixels.foreach_set(target_pixels)
    catalog.file_format = "PNG"
    catalog.filepath_raw = str(output_path)
    catalog.save()
    bpy.data.images.remove(catalog)


def render(asset_path, output_directory):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(asset_path))
    meshes = {
        obj.name: obj for obj in bpy.context.scene.objects
        if obj.type == "MESH" and obj.name in VARIANT_NAMES
    }
    if tuple(sorted(meshes)) != tuple(sorted(VARIANT_NAMES)):
        raise RuntimeError("Exported GLB variant inventory changed")
    for mesh in meshes.values():
        mesh.hide_render = True

    scene = bpy.context.scene
    add_audit_stage(scene)
    output_directory.mkdir(parents=True, exist_ok=True)
    render_paths = []
    with tempfile.TemporaryDirectory(prefix="gc-en08-audit-") as temporary:
        scratch = Path(temporary)
        for view_name, rotation in VIEW_ROTATIONS:
            output_path = output_directory / f"EN-08-{view_name}.png"
            render_catalog(scene, meshes, output_path, rotation, scratch)
            render_paths.append(output_path)

    metadata = {
        "format": "goldencheetah-workout-game-asset-audit-1",
        "assetId": "EN-08-forest-floor-props",
        "sourceGlb": asset_path.name,
        "sourceGlbSha256": sha256(asset_path),
        "catalog": {
            "widthPixels": CATALOG_WIDTH,
            "heightPixels": CATALOG_HEIGHT,
            "cellColumns": CELL_COLUMNS,
            "cellRows": CELL_ROWS,
            "cellOrder": list(VARIANT_NAMES),
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "cameraDistanceMeters": CAMERA_DISTANCE_METERS,
            "cameraPositionMeters": list(CAMERA_CANONICAL_POSITION),
            "cameraTargetMeters": list(CAMERA_CANONICAL_TARGET),
            "trailWidthScaleBarMeters": 1.36,
            "renderEngine": "CYCLES_CPU_16_SAMPLES",
            "viewTransform": "Standard",
            "look": "Medium High Contrast",
        },
        "renders": [
            {
                "view": view_name,
                "assetRotationDegrees": rotation,
                "path": path.name,
                "sha256": sha256(path),
            }
            for (view_name, rotation), path in zip(VIEW_ROTATIONS, render_paths)
        ],
    }
    metadata_path = output_directory / "EN-08-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print("Rendered", ", ".join(str(path) for path in render_paths))


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Render forest-floor prop audits")
    parser.add_argument("--asset", required=True)
    parser.add_argument("--output-dir", required=True)
    parsed = parser.parse_args(arguments)
    asset = Path(os.path.expanduser(parsed.asset)).resolve()
    output = Path(os.path.expanduser(parsed.output_dir)).resolve()
    if asset.suffix.lower() != ".glb" or not asset.is_file():
        raise RuntimeError("--asset must name an existing GLB")
    render(asset, output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
