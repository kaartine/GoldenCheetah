#!/usr/bin/env python3
"""Render fixed-camera audit views from the exported gap-jump GLB."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import traceback

import bpy
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_tabletop import canonical_to_blender, make_material


WIDTH = 960
HEIGHT = 540
VERTICAL_FOV_DEGREES = 47.0
VIEWS = (
    ("chase", (0.0, 6.5, -11.0), (0.0, 0.10, 16.5)),
    ("overhead", (0.0, 33.0, 20.0), (0.0, 0.0, 20.0)),
    ("side-short", (-12.0, 3.3, 13.0), (-2.3, 0.15, 13.0)),
    ("side-medium", (-12.0, 3.7, 13.8), (0.0, 0.20, 13.8)),
    ("side-long", (13.5, 4.2, 14.4), (2.3, 0.24, 14.4)),
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def point_at(obj, canonical_target) -> None:
    target = Vector(canonical_to_blender(canonical_target))
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()


def add_stage(scene):
    camera_data = bpy.data.cameras.new("AUDIT_Camera_47deg")
    camera_data.sensor_fit = "VERTICAL"
    camera_data.sensor_height = 32.0
    camera_data.lens = camera_data.sensor_height / (
        2.0 * math.tan(math.radians(VERTICAL_FOV_DEGREES) * 0.5)
    )
    camera_data.clip_start = 0.1
    camera_data.clip_end = 180.0
    camera = bpy.data.objects.new("AUDIT_Camera_47deg", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    key_data = bpy.data.lights.new("AUDIT_Key", type="AREA")
    key_data.energy = 760.0
    key_data.shape = "DISK"
    key_data.size = 7.0
    key = bpy.data.objects.new("AUDIT_Key", key_data)
    bpy.context.collection.objects.link(key)
    key.location = canonical_to_blender((-8.0, 14.0, -4.0))
    point_at(key, (0.0, 0.0, 16.0))

    fill_data = bpy.data.lights.new("AUDIT_Fill", type="AREA")
    fill_data.energy = 380.0
    fill_data.size = 8.0
    fill = bpy.data.objects.new("AUDIT_Fill", fill_data)
    bpy.context.collection.objects.link(fill)
    fill.location = canonical_to_blender((9.0, 7.0, 13.0))
    point_at(fill, (0.0, 0.0, 16.0))

    rim_data = bpy.data.lights.new("AUDIT_Rim", type="AREA")
    rim_data.energy = 520.0
    rim_data.size = 5.0
    rim = bpy.data.objects.new("AUDIT_Rim", rim_data)
    bpy.context.collection.objects.link(rim)
    rim.location = canonical_to_blender((0.0, 8.0, 35.0))
    point_at(rim, (0.0, 0.0, 18.0))

    scale_material = make_material("AUDIT_TrailWidthScale", (0.91, 0.77, 0.31, 1.0))
    bpy.ops.mesh.primitive_cube_add(location=canonical_to_blender((0.0, 0.08, 1.0)))
    scale = bpy.context.object
    scale.name = "AUDIT_TrailWidth_1_36m"
    scale.dimensions = (1.36, 0.03, 0.03)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    scale.data.materials.append(scale_material)

    scene.world = bpy.data.worlds.new("AUDIT_World")
    scene.world.color = (0.055, 0.075, 0.08)
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 24
    scene.cycles.use_denoising = False
    scene.render.resolution_x = WIDTH
    scene.render.resolution_y = HEIGHT
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGBA"
    scene.render.film_transparent = False
    scene.view_settings.view_transform = "Standard"
    scene.view_settings.look = "Medium High Contrast"
    scene.view_settings.exposure = 0.0
    scene.view_settings.gamma = 1.0
    return camera


def render(asset_path: Path, output_directory: Path) -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(asset_path))
    expected = {
        "GEO_GapJumpGround_LOD0",
        "GEO_GapJumpTread_LOD0",
        "GEO_GapJumpAccents_LOD0",
    }
    observed = {obj.name for obj in bpy.context.scene.objects if obj.type == "MESH"}
    if observed != expected:
        raise RuntimeError(f"Exported GLB mesh inventory changed: {sorted(observed)}")

    scene = bpy.context.scene
    camera = add_stage(scene)
    output_directory.mkdir(parents=True, exist_ok=True)
    renders = []
    for view_name, position, target in VIEWS:
        camera.location = canonical_to_blender(position)
        point_at(camera, target)
        scene.render.filepath = str(output_directory / f"FT-12-{view_name}.png")
        bpy.context.view_layer.update()
        bpy.ops.render.render(write_still=True)
        output_path = Path(scene.render.filepath)
        renders.append({
            "view": view_name,
            "cameraPositionMeters": list(position),
            "cameraTargetMeters": list(target),
            "path": output_path.name,
            "sha256": sha256(output_path),
        })

    metadata = {
        "format": "goldencheetah-workout-game-asset-audit-1",
        "assetId": "FT-12-gap-jump-three-line",
        "sourceGlb": asset_path.name,
        "sourceGlbSha256": sha256(asset_path),
        "catalog": {
            "widthPixels": WIDTH,
            "heightPixels": HEIGHT,
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "trailWidthScaleBarMeters": 1.36,
            "renderEngine": "CYCLES_CPU_24_SAMPLES",
            "viewTransform": "Standard",
            "look": "Medium High Contrast",
        },
        "renders": renders,
    }
    metadata_path = output_directory / "FT-12-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print("Rendered", ", ".join(item["path"] for item in renders))


def main() -> None:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Render gap-jump asset audits")
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
