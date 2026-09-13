#!/usr/bin/env python3
"""Create a hostile-enough synthetic TripoSR-style GLB for cleanup smoke tests."""

from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import struct
import sys
import traceback

import bpy


GLB_HEADER = struct.Struct("<4sII")
CHUNK_HEADER = struct.Struct("<II")
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Create synthetic cleanup input")
    parser.add_argument("--output", required=True)
    return parser.parse_args(arguments)


def image_material() -> bpy.types.Material:
    image = bpy.data.images.new("PRIVATE_REFERENCE_PIXELS", width=2, height=2)
    image.pixels = [
        0.8, 0.2, 0.1, 1.0,
        0.2, 0.8, 0.1, 1.0,
        0.1, 0.2, 0.8, 1.0,
        0.7, 0.7, 0.7, 1.0,
    ]
    image.pack()
    material = bpy.data.materials.new("MAT_ImageBased")
    material.use_nodes = True
    tree = material.node_tree
    texture = tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    principled = tree.nodes.get("Principled BSDF")
    tree.links.new(texture.outputs["Color"], principled.inputs["Base Color"])
    return material


def add_torus(name: str, location: tuple[float, float, float]) -> bpy.types.Object:
    bpy.ops.mesh.primitive_torus_add(
        align="WORLD",
        major_segments=256,
        minor_segments=24,
        location=location,
        major_radius=0.36,
        minor_radius=0.045,
    )
    obj = bpy.context.active_object
    obj.name = name
    obj.rotation_euler = (math.radians(90.0), 0.0, 0.0)
    obj.scale = (1.05, 1.0, 0.95)
    return obj


def add_frame(material: bpy.types.Material) -> bpy.types.Object:
    bpy.ops.mesh.primitive_cube_add(
        location=(0.0, 0.64, 0.0), scale=(0.48, 0.035, 0.045)
    )
    frame = bpy.context.active_object
    frame.name = "RawFrameWithMorphAndSkin"
    frame.rotation_euler[1] = math.radians(-18.0)
    frame.data.materials.append(material)
    frame.shape_key_add(name="Basis")
    morph = frame.shape_key_add(name="UntrustedMorph")
    morph.data[0].co.z += 0.08

    armature_data = bpy.data.armatures.new("UntrustedArmatureData")
    armature = bpy.data.objects.new("UntrustedArmature", armature_data)
    bpy.context.collection.objects.link(armature)
    bpy.context.view_layer.objects.active = armature
    armature.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bone = armature_data.edit_bones.new("UntrustedBone")
    bone.head = (0.0, 0.0, 0.0)
    bone.tail = (0.0, 1.0, 0.0)
    bpy.ops.object.mode_set(mode="OBJECT")
    group = frame.vertex_groups.new(name=bone.name)
    group.add(range(len(frame.data.vertices)), 1.0, "REPLACE")
    modifier = frame.modifiers.new("UntrustedSkin", "ARMATURE")
    modifier.object = armature
    return frame


def add_noise_triangle() -> None:
    mesh = bpy.data.meshes.new("TinyFragmentMesh")
    mesh.from_pydata(
        ((4.0, 4.0, 4.0), (4.00001, 4.0, 4.0), (4.0, 4.00001, 4.0)),
        [],
        ((0, 1, 2),),
    )
    obj = bpy.data.objects.new("TinyFragment", mesh)
    bpy.context.collection.objects.link(obj)


def add_untrusted_external_image_uri(path: Path) -> None:
    data = path.read_bytes()
    _magic, _version, _length = GLB_HEADER.unpack_from(data)
    offset = GLB_HEADER.size
    document = None
    binary = b""
    while offset < len(data):
        length, chunk_type = CHUNK_HEADER.unpack_from(data, offset)
        offset += CHUNK_HEADER.size
        payload = data[offset:offset + length]
        offset += length
        if chunk_type == JSON_CHUNK:
            document = json.loads(payload.rstrip(b" \t\r\n\0"))
        elif chunk_type == BIN_CHUNK:
            binary = payload
    if not isinstance(document, dict):
        raise RuntimeError("synthetic export has no GLB document")
    document.setdefault("images", []).append(
        {
            "name": "UntrustedExternalImage",
            "uri": "file:///definitely-not-read/private-reference.png",
        }
    )
    json_bytes = json.dumps(
        document, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("ascii")
    json_bytes += b" " * ((-len(json_bytes)) % 4)
    binary += b"\0" * ((-len(binary)) % 4)
    body = CHUNK_HEADER.pack(len(json_bytes), JSON_CHUNK) + json_bytes
    body += CHUNK_HEADER.pack(len(binary), BIN_CHUNK) + binary
    path.write_bytes(GLB_HEADER.pack(b"glTF", 2, GLB_HEADER.size + len(body)) + body)


def build_scene(output: Path) -> None:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = image_material()
    rear = add_torus("RawRearWheel", (-0.58, 0.38, 0.0))
    front = add_torus("RawFrontWheel", (0.58, 0.38, 0.0))
    rear.data.materials.append(material)
    front.data.materials.append(material)
    frame = add_frame(material)
    add_noise_triangle()

    camera_data = bpy.data.cameras.new("UntrustedCameraData")
    camera = bpy.data.objects.new("UntrustedCamera", camera_data)
    bpy.context.collection.objects.link(camera)
    light_data = bpy.data.lights.new("UntrustedLightData", type="POINT")
    light = bpy.data.objects.new("UntrustedLight", light_data)
    bpy.context.collection.objects.link(light)

    frame.keyframe_insert(data_path="location", frame=1)
    frame.location.x = 0.1
    frame.keyframe_insert(data_path="location", frame=20)

    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.export_scene.gltf(
        filepath=str(output),
        check_existing=False,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_cameras=True,
        export_lights=True,
        export_animations=True,
        export_skins=True,
        export_morph=True,
    )
    if not output.is_file() or output.stat().st_size <= 0:
        raise RuntimeError("synthetic GLB export failed")
    add_untrusted_external_image_uri(output)
    print("Generated", output)


def main() -> None:
    arguments = parse_arguments()
    output = Path(os.path.expanduser(arguments.output)).resolve(strict=False)
    if output.suffix.lower() != ".glb":
        raise RuntimeError("--output must end in .glb")
    build_scene(output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
