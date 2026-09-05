#!/usr/bin/env python3
"""Render fixed-camera rider and bicycle audit views from the exported GLB."""

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
    ("front", (0.0, 1.18, 4.5), (0.0, 0.91, 0.18)),
    ("rear", (0.0, 1.22, -4.25), (0.0, 0.91, 0.08)),
    ("side", (4.35, 1.18, 0.18), (0.0, 0.88, 0.18)),
    ("chase", (2.65, 1.86, -4.15), (0.0, 0.94, 0.10)),
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def point_at(obj, canonical_target) -> None:
    target = Vector(canonical_to_blender(canonical_target))
    obj.rotation_euler = (target - obj.location).to_track_quat("-Z", "Y").to_euler()


def place_canonical(obj, position, rotation_x=0.0) -> None:
    obj.location = canonical_to_blender(position)
    obj.rotation_euler[0] = math.radians(rotation_x)


def place_segment(source, name, start, end, thickness) -> None:
    segment = source.copy()
    segment.data = source.data.copy()
    segment.name = name
    segment.parent = None
    segment.hide_render = False
    bpy.context.collection.objects.link(segment)
    start_blender = Vector(canonical_to_blender(start))
    end_blender = Vector(canonical_to_blender(end))
    delta = end_blender - start_blender
    segment.location = start_blender
    segment.rotation_euler = delta.to_track_quat("Z", "Y").to_euler()
    segment.scale = (thickness, thickness, delta.length)


def assemble_neutral_pose(objects) -> None:
    body_origin = (0.0, 1.23, -0.03)
    body_pitch = 13.0
    for name in ("GEO_Torso_LOD0", "GEO_JerseyAccent_LOD0"):
        place_canonical(objects[name], (0.0, 1.08, -0.12), body_pitch)

    shorts = objects["GEO_Torso_LOD0"].copy()
    shorts.data = objects["GEO_Torso_LOD0"].data.copy()
    shorts.name = "AUDIT_Shorts"
    shorts.parent = None
    bpy.context.collection.objects.link(shorts)
    place_canonical(shorts, (0.0, 1.12, -0.22), body_pitch)
    shorts.scale = (0.72, 0.60, 0.70)
    shorts.data.materials.clear()
    shorts.data.materials.append(objects["GEO_HairBeard_LOD0"].data.materials[0])

    head_origin = (0.0, 1.69, -0.005)
    for name in (
        "GEO_Head_LOD0",
        "GEO_HairBeard_LOD0",
        "GEO_Eyewear_LOD0",
    ):
        place_canonical(objects[name], head_origin, body_pitch)
    for name in ("GEO_Helmet_LOD0", "GEO_HelmetAccent_LOD0"):
        place_canonical(objects[name], (0.0, 1.735, -0.005), body_pitch)

    left_pedal = (-0.13, 0.5375, 0.0)
    right_pedal = (0.13, 0.2175, 0.0)
    left_hip = (-0.12, 1.08, -0.12)
    right_hip = (0.12, 1.08, -0.12)
    left_knee = (-0.13, 0.93, 0.04)
    right_knee = (0.13, 0.75, -0.01)
    left_shoulder = (-0.19, 1.47, -0.07)
    right_shoulder = (0.19, 1.47, -0.07)
    left_elbow = (-0.25, 1.30, 0.13)
    right_elbow = (0.25, 1.30, 0.13)
    left_hand = (-0.30, 1.085, 0.465)
    right_hand = (0.30, 1.085, 0.465)
    limb = objects["GEO_Limb_LOD0"]
    limb.hide_render = True
    for index, (start, end, thickness) in enumerate((
        (left_hip, left_knee, 0.72),
        (left_knee, left_pedal, 0.62),
        (right_hip, right_knee, 0.72),
        (right_knee, right_pedal, 0.62),
        (left_shoulder, left_elbow, 0.58),
        (left_elbow, left_hand, 0.52),
        (right_shoulder, right_elbow, 0.58),
        (right_elbow, right_hand, 0.52),
    )):
        place_segment(limb, f"AUDIT_Limb_{index}", start, end, thickness)

    objects["GEO_Shadow_LOD0"].hide_render = True


def add_stage(scene):
    camera_data = bpy.data.cameras.new("AUDIT_Camera_47deg")
    camera_data.sensor_fit = "VERTICAL"
    camera_data.sensor_height = 32.0
    camera_data.lens = camera_data.sensor_height / (
        2.0 * math.tan(math.radians(VERTICAL_FOV_DEGREES) * 0.5)
    )
    camera_data.clip_start = 0.1
    camera_data.clip_end = 80.0
    camera = bpy.data.objects.new("AUDIT_Camera_47deg", camera_data)
    bpy.context.collection.objects.link(camera)
    scene.camera = camera

    for name, energy, size, position, target in (
        ("AUDIT_Key", 820.0, 5.0, (-4.0, 6.5, 1.0), (0.0, 0.8, 0.1)),
        ("AUDIT_Fill", 430.0, 4.0, (4.5, 3.5, -1.5), (0.0, 0.9, 0.1)),
        ("AUDIT_Rim", 620.0, 3.0, (0.5, 4.5, 4.0), (0.0, 1.0, 0.1)),
    ):
        light_data = bpy.data.lights.new(name, type="AREA")
        light_data.energy = energy
        light_data.shape = "DISK"
        light_data.size = size
        light = bpy.data.objects.new(name, light_data)
        bpy.context.collection.objects.link(light)
        light.location = canonical_to_blender(position)
        point_at(light, target)

    ground_material = make_material(
        "AUDIT_Ground", (0.075, 0.095, 0.085, 1.0)
    )
    bpy.ops.mesh.primitive_plane_add(size=16.0, location=(0.0, 0.0, 0.0))
    ground = bpy.context.object
    ground.name = "AUDIT_Ground"
    ground.data.materials.append(ground_material)

    scale_material = make_material(
        "AUDIT_WheelbaseScale", (0.91, 0.77, 0.31, 1.0)
    )
    bpy.ops.mesh.primitive_cube_add(
        location=canonical_to_blender((0.0, 0.018, -0.455))
    )
    scale = bpy.context.object
    scale.name = "AUDIT_Wheelbase_1_313m"
    scale.dimensions = (0.025, 0.025, 1.313)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    scale.data.materials.append(scale_material)

    scene.world = bpy.data.worlds.new("AUDIT_World")
    scene.world.color = (0.035, 0.050, 0.055)
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
    objects = {obj.name: obj for obj in bpy.context.scene.objects}
    required = {
        "GEO_MainFrame_LOD0",
        "GEO_Swingarm_LOD0",
        "GEO_Fork_LOD0",
        "GEO_RearShock_LOD0",
        "GEO_RearWheel_LOD0",
        "GEO_FrontWheel_LOD0",
        "GEO_Crank_LOD0",
        "GEO_BikeComponents_LOD0",
        "GEO_Torso_LOD0",
        "GEO_Head_LOD0",
        "GEO_Helmet_LOD0",
        "GEO_HelmetAccent_LOD0",
        "GEO_Limb_LOD0",
    }
    if not required.issubset(objects):
        raise RuntimeError(
            f"Exported GLB node inventory changed: {sorted(required - objects.keys())}"
        )
    assemble_neutral_pose(objects)

    scene = bpy.context.scene
    camera = add_stage(scene)
    output_directory.mkdir(parents=True, exist_ok=True)
    renders = []
    for view, position, target in VIEWS:
        camera.location = canonical_to_blender(position)
        point_at(camera, target)
        path = output_directory / f"RB-01-{view}.png"
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        renders.append({
            "view": view,
            "path": path.name,
            "sha256": sha256(path),
            "cameraPositionMeters": list(position),
            "cameraTargetMeters": list(target),
        })

    metadata = {
        "assetId": "RB-01-rider-bike",
        "assetSha256": sha256(asset_path),
        "catalog": {
            "widthPixels": WIDTH,
            "heightPixels": HEIGHT,
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "pose": "neutral-seated-crank-left-high",
            "wheelbaseScaleMeters": 1.313,
        },
        "renders": renders,
    }
    metadata_path = output_directory / "RB-01-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print("Rendered", ", ".join(item["path"] for item in renders))


def main() -> None:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Render rider-bike asset audits")
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
