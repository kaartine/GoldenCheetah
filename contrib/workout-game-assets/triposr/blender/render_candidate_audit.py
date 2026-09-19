#!/usr/bin/env python3
"""Render fixed review views from a verified external TripoSR candidate."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
import traceback

import bpy


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
TRIPOSR_DIRECTORY = SCRIPT_DIRECTORY.parent
sys.path.insert(0, str(SCRIPT_DIRECTORY.parent.parent / "blender"))
sys.path.insert(0, str(TRIPOSR_DIRECTORY))

from render_rider_bike_audit import (  # noqa: E402
    VIEWS,
    add_stage,
    canonical_to_blender,
    normalize_png,
    point_at,
)
from triposr_candidate import (  # noqa: E402
    CandidateError,
    external_destination,
    read_external_snapshot,
    verify_candidate_directory,
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(
        description="Render a verified TripoSR review candidate"
    )
    parser.add_argument("--candidate-directory", required=True)
    parser.add_argument("--output-dir", required=True)
    return parser.parse_args(arguments)


def render(candidate_directory: Path, output_directory: Path) -> None:
    descriptor = verify_candidate_directory(candidate_directory)
    lod0_entry = next(
        entry
        for entry in descriptor["files"]
        if entry["role"] == "review-model-lod0"
    )
    snapshot = read_external_snapshot(
        candidate_directory / lod0_entry["name"],
        "candidate LOD0",
        lod0_entry["bytes"],
    )
    if snapshot.sha256 != lod0_entry["sha256"]:
        raise CandidateError("candidate LOD0 changed after verification")

    output_directory.mkdir(parents=True, exist_ok=True)
    import_path = output_directory / ".verified-candidate-lod0.glb"
    if import_path.is_symlink():
        raise CandidateError("audit import path may not be a symlink")
    import_path.write_bytes(snapshot.data)
    try:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.import_scene.gltf(filepath=str(import_path), import_pack_images=False)
    finally:
        import_path.unlink(missing_ok=True)

    scene = bpy.context.scene
    camera = add_stage(scene)
    candidate_id = descriptor["candidateId"]
    renders = []
    for view, position, target in VIEWS:
        camera.location = canonical_to_blender(position)
        point_at(camera, target)
        path = output_directory / f"{candidate_id}-{view}.png"
        scene.render.filepath = str(path)
        bpy.ops.render.render(write_still=True)
        normalize_png(path)
        renders.append({"view": view, "file": path.name, "sha256": sha256(path)})

    metadata = {
        "candidateId": candidate_id,
        "candidateLod0Sha256": lod0_entry["sha256"],
        "triangleBudget": descriptor["validation"]["budgets"]["maxTrianglesLod0"],
        "triangles": descriptor["technical"]["lod0"]["triangles"],
        "renders": renders,
    }
    metadata_path = output_directory / f"{candidate_id}-audit.json"
    metadata_path.write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="ascii"
    )
    print("Rendered", ", ".join(entry["file"] for entry in renders))


def main() -> None:
    arguments = parse_arguments()
    candidate = Path(arguments.candidate_directory).expanduser().resolve(strict=True)
    output = external_destination(Path(arguments.output_dir))
    render(candidate, output)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
