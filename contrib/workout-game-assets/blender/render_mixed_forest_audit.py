#!/usr/bin/env python3
"""Render matched before/after catalogs for the runtime mixed-forest set."""

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
from generate_tabletop import canonical_to_blender, make_material


CATALOG_WIDTH = 1280
CATALOG_HEIGHT = 720
CELL_WIDTH = CATALOG_WIDTH // 4
VERTICAL_FOV_DEGREES = 43.0
CAMERA_TARGET_METERS = (0.0, 2.65, 0.0)
BEFORE_VARIANTS = (
    ("narrow-spruce", "GEO_ConiferTrunk_LOD0", "GEO_ConiferNarrow_LOD0"),
    ("layered-spruce", "GEO_ConiferTrunk_LOD0", "GEO_ConiferLayered_LOD0"),
    ("broken-top-spruce", "GEO_ConiferTrunk_LOD0", "GEO_ConiferBrokenTop_LOD0"),
    ("scots-pine", "GEO_ScotsPineTrunk_LOD0", "GEO_ScotsPineCrown_LOD0"),
)
AFTER_VARIANTS = (
    BEFORE_VARIANTS[0],
    BEFORE_VARIANTS[1],
    ("silver-birch", "GEO_BirchTrunk_LOD0", "GEO_BirchCrown_LOD0"),
    BEFORE_VARIANTS[3],
)
VIEW_CAMERAS = (
    ("front", (0.0, 2.9, -11.5)),
    ("left-three-quarter", (-8.13, 2.9, -8.13)),
    ("rear-three-quarter", (-8.13, 2.9, 8.13)),
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def point_at(obj, canonical_target):
    target = Vector(canonical_to_blender(canonical_target))
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()


def add_stage(scene):
    ground_material = make_material(
        "AUDIT_ForestGround", (0.20, 0.34, 0.18, 1.0)
    )
    bpy.ops.mesh.primitive_plane_add(size=14.0, location=(0.0, 0.0, -0.01))
    ground = bpy.context.object
    ground.name = "AUDIT_GroundDatum"
    ground.data.materials.append(ground_material)

    camera_data = bpy.data.cameras.new("AUDIT_Camera_43deg")
    camera_data.sensor_fit = "VERTICAL"
    camera_data.sensor_height = 32.0
    camera_data.lens = camera_data.sensor_height / (
        2.0 * math.tan(math.radians(VERTICAL_FOV_DEGREES) * 0.5)
    )
    camera = bpy.data.objects.new("AUDIT_Camera_43deg", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    key_data = bpy.data.lights.new("AUDIT_Key", type="AREA")
    key_data.energy = 700.0
    key_data.shape = "DISK"
    key_data.size = 4.0
    key = bpy.data.objects.new("AUDIT_Key", key_data)
    bpy.context.collection.objects.link(key)
    key.location = canonical_to_blender((-4.0, 8.0, -5.0))
    point_at(key, CAMERA_TARGET_METERS)

    scene.world = bpy.data.worlds.new("AUDIT_World")
    scene.world.color = (0.12, 0.20, 0.24)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 8
    scene.cycles.use_denoising = False
    scene.render.resolution_x = CELL_WIDTH
    scene.render.resolution_y = CATALOG_HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    return camera


def copy_cell(target, source, column):
    pixels = list(source.pixels[:])
    for source_y in range(CATALOG_HEIGHT):
        source_start = source_y * CELL_WIDTH * 4
        target_start = (source_y * CATALOG_WIDTH + column * CELL_WIDTH) * 4
        target[target_start:target_start + CELL_WIDTH * 4] = pixels[
            source_start:source_start + CELL_WIDTH * 4
        ]


def render_set(asset_path, variants, output_directory, prefix):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(asset_path))
    meshes = {obj.name: obj for obj in bpy.context.scene.objects if obj.type == "MESH"}
    required = {name for _, trunk, crown in variants for name in (trunk, crown)}
    if not required.issubset(meshes):
        raise RuntimeError(f"GLB inventory changed: {sorted(required - set(meshes))}")
    for mesh in meshes.values():
        mesh.hide_render = True

    scene = bpy.context.scene
    camera = add_stage(scene)
    paths = []
    with tempfile.TemporaryDirectory(prefix="gc-mixed-forest-audit-") as temporary:
        scratch = Path(temporary)
        for view_name, camera_position in VIEW_CAMERAS:
            camera.location = canonical_to_blender(camera_position)
            point_at(camera, CAMERA_TARGET_METERS)
            target_pixels = [0.0] * (CATALOG_WIDTH * CATALOG_HEIGHT * 4)
            for column, (_, trunk_name, crown_name) in enumerate(variants):
                for mesh in meshes.values():
                    mesh.hide_render = True
                meshes[trunk_name].hide_render = False
                meshes[crown_name].hide_render = False
                bpy.context.view_layer.update()
                cell_path = scratch / f"{prefix}-{view_name}-{column}.png"
                scene.render.filepath = str(cell_path)
                bpy.ops.render.render(write_still=True)
                cell = bpy.data.images.load(str(cell_path), check_existing=False)
                copy_cell(target_pixels, cell, column)
                bpy.data.images.remove(cell)

            output_path = output_directory / f"{prefix}-{view_name}.png"
            catalog = bpy.data.images.new(
                f"MixedForest_{prefix}_{view_name}",
                width=CATALOG_WIDTH,
                height=CATALOG_HEIGHT,
                alpha=True,
            )
            catalog.pixels.foreach_set(target_pixels)
            catalog.file_format = "PNG"
            catalog.filepath_raw = str(output_path)
            catalog.save()
            bpy.data.images.remove(catalog)
            paths.append(output_path)
    return paths


def render(before_asset, after_asset, output_directory):
    output_directory.mkdir(parents=True, exist_ok=True)
    before_paths = render_set(
        before_asset, BEFORE_VARIANTS, output_directory, "before"
    )
    after_paths = render_set(
        after_asset, AFTER_VARIANTS, output_directory, "after"
    )
    metadata = {
        "format": "goldencheetah-workout-game-asset-audit-1",
        "assetId": "EN-01-conifer-set",
        "beforeGlb": before_asset.name,
        "beforeGlbSha256": sha256(before_asset),
        "afterGlb": after_asset.name,
        "afterGlbSha256": sha256(after_asset),
        "catalog": {
            "widthPixels": CATALOG_WIDTH,
            "heightPixels": CATALOG_HEIGHT,
            "cellColumns": 4,
            "cellRows": 1,
            "beforeCellOrder": [variant[0] for variant in BEFORE_VARIANTS],
            "afterCellOrder": [variant[0] for variant in AFTER_VARIANTS],
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "cameraTargetMeters": list(CAMERA_TARGET_METERS),
            "cameraPositionsMeters": [
                list(position) for _, position in VIEW_CAMERAS
            ],
            "renderEngine": "CYCLES_CPU_8_SAMPLES",
            "viewTransform": "Standard",
            "look": "Medium High Contrast",
        },
        "renders": [
            {
                "state": state,
                "view": view_name,
                "path": path.name,
                "sha256": sha256(path),
            }
            for state, paths in (("before", before_paths), ("after", after_paths))
            for (view_name, _), path in zip(VIEW_CAMERAS, paths)
        ],
    }
    metadata_path = output_directory / "EN-01-mixed-forest-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print("Rendered", ", ".join(str(path) for path in before_paths + after_paths))


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Render mixed-forest comparison")
    parser.add_argument("--before-asset", required=True)
    parser.add_argument("--after-asset", required=True)
    parser.add_argument("--output-dir", required=True)
    parsed = parser.parse_args(arguments)
    before_asset = Path(os.path.expanduser(parsed.before_asset)).resolve()
    after_asset = Path(os.path.expanduser(parsed.after_asset)).resolve()
    output = Path(os.path.expanduser(parsed.output_dir)).resolve()
    for asset in (before_asset, after_asset):
        if asset.suffix.lower() != ".glb" or not asset.is_file():
            raise RuntimeError("audit inputs must be existing GLB files")
    render(before_asset, after_asset, output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
