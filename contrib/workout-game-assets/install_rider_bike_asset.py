#!/usr/bin/env python3

"""Install one fully built rider-bike candidate into the repository."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
REPOSITORY = SCRIPT_DIRECTORY.parents[1]
sys.path.insert(0, str(SCRIPT_DIRECTORY))

import validate_assets as assets  # noqa: E402


ASSET_ID = "RB-01-rider-bike"
GLB_NAME = "WG_RiderBike.glb"
BLEND_RELATIVE = (
    "contrib/workout-game-assets/blender/sources/WG_RiderBike.blend"
)
GENERATOR_RELATIVE = (
    "contrib/workout-game-assets/blender/generate_rider_bike.py"
)
INSTALLER_RELATIVE = "contrib/workout-game-assets/install_rider_bike_asset.py"
PIPELINE_RELATIVE = "contrib/workout-game-assets/rebuild_rider_bike.sh"
AUDIT_RELATIVE = "contrib/workout-game-assets/audits/RB-01"
GENERATED_RELATIVE = f"contrib/workout-game-assets/generated/{GLB_NAME}"
RUNTIME_MESH_RELATIVE = "src/Train/qml/assets/meshes"

EXPECTED_MESH_NODES = {
    "GEO_BikeComponents_LOD0",
    "GEO_Crank_LOD0",
    "GEO_Eyewear_LOD0",
    "GEO_Fork_LOD0",
    "GEO_FrontWheel_LOD0",
    "GEO_HairBeard_LOD0",
    "GEO_Head_LOD0",
    "GEO_Helmet_LOD0",
    "GEO_HelmetAccent_LOD0",
    "GEO_JerseyAccent_LOD0",
    "GEO_Limb_LOD0",
    "GEO_MainFrame_LOD0",
    "GEO_RearShock_LOD0",
    "GEO_RearWheel_LOD0",
    "GEO_Shadow_LOD0",
    "GEO_Swingarm_LOD0",
    "GEO_Torso_LOD0",
}
AUDIT_NAMES = (
    "RB-01-audit.json",
    "RB-01-front.png",
    "RB-01-rear.png",
    "RB-01-side.png",
    "RB-01-chase.png",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def mesh_file_name(node_name: str) -> str:
    return f"geo_{node_name.removeprefix('GEO_')}_mesh.mesh"


def technical_metadata(document: dict[str, Any], glb_size: int) -> dict[str, Any]:
    accessors = document.get("accessors", [])
    triangle_count = 0
    bounds_min = [float("inf")] * 3
    bounds_max = [-float("inf")] * 3
    for mesh in document.get("meshes", []):
        for primitive in mesh.get("primitives", []):
            index = primitive.get(
                "indices", primitive.get("attributes", {}).get("POSITION")
            )
            triangle_count += int(accessors[index]["count"]) // 3
            position = accessors[primitive["attributes"]["POSITION"]]
            bounds_min = [
                min(bounds_min[axis], float(position["min"][axis]))
                for axis in range(3)
            ]
            bounds_max = [
                max(bounds_max[axis], float(position["max"][axis]))
                for axis in range(3)
            ]

    buffer_views = document.get("bufferViews", [])
    texture_bytes = sum(
        int(buffer_views[image["bufferView"]]["byteLength"])
        for image in document.get("images", [])
    )
    return {
        "glbBytes": glb_size,
        "trianglesLod0": triangle_count,
        "materials": len(document.get("materials", [])),
        "textureBytes": texture_bytes,
        "nodes": sorted(node["name"] for node in document.get("nodes", [])),
        "animations": [
            animation.get("name")
            for animation in document.get("animations", [])
        ],
        "boundsMeters": {
            "minimum": bounds_min,
            "maximum": bounds_max,
        },
    }


def verify_candidate(candidate: Path) -> tuple[dict[str, Any], int]:
    glb = candidate / GLB_NAME
    audit_directory = candidate / "audit"
    balsam_meshes = candidate / "balsam" / "meshes"
    blend = REPOSITORY / BLEND_RELATIVE
    blend_magic = blend.read_bytes()[:7] if blend.is_file() else b""
    if not (
        blend_magic == b"BLENDER"
        or blend_magic.startswith(b"\x28\xb5\x2f\xfd")
    ):
        raise RuntimeError(f"editable Blender source is missing or invalid: {blend}")
    if not glb.is_file():
        raise RuntimeError(f"candidate GLB is missing: {glb}")

    document, glb_size = assets.read_glb(glb)
    mesh_nodes = {
        node["name"]
        for node in document.get("nodes", [])
        if node.get("mesh") is not None
    }
    if mesh_nodes != EXPECTED_MESH_NODES:
        raise RuntimeError(
            "candidate mesh inventory changed: "
            f"expected {sorted(EXPECTED_MESH_NODES)}, got {sorted(mesh_nodes)}"
        )

    expected_mesh_files = {
        mesh_file_name(node_name) for node_name in EXPECTED_MESH_NODES
    }
    actual_mesh_files = {
        path.name for path in balsam_meshes.glob("*.mesh") if path.is_file()
    }
    if actual_mesh_files != expected_mesh_files:
        raise RuntimeError(
            "Balsam mesh inventory changed: "
            f"expected {sorted(expected_mesh_files)}, got {sorted(actual_mesh_files)}"
        )
    if any((balsam_meshes / name).stat().st_size <= 0 for name in expected_mesh_files):
        raise RuntimeError("Balsam produced an empty runtime mesh")

    for name in AUDIT_NAMES:
        if not (audit_directory / name).is_file():
            raise RuntimeError(f"candidate audit output is missing: {name}")
    audit = assets.load_json_file(audit_directory / AUDIT_NAMES[0])
    if audit.get("assetId") != ASSET_ID or audit.get("assetSha256") != sha256(glb):
        raise RuntimeError("candidate audit does not describe the candidate GLB")
    for render in audit.get("renders", []):
        render_path = audit_directory / render["path"]
        if sha256(render_path) != render.get("sha256"):
            raise RuntimeError(f"candidate audit hash mismatch: {render_path.name}")
    if {render.get("path") for render in audit.get("renders", [])} != set(
        AUDIT_NAMES[1:]
    ):
        raise RuntimeError("candidate audit view inventory changed")
    return document, glb_size


def update_file_entry(
    manifest: dict[str, Any], relative: str, purpose: str
) -> None:
    entries = {entry["path"]: entry for entry in manifest["files"]}
    if relative not in entries:
        entry = {"path": relative, "purpose": purpose, "sha256": ""}
        if relative == BLEND_RELATIVE:
            generator_index = next(
                index
                for index, candidate in enumerate(manifest["files"])
                if candidate["path"] == GENERATOR_RELATIVE
            )
            manifest["files"].insert(generator_index + 1, entry)
        else:
            manifest["files"].append(entry)
        entries[relative] = entry
    entries[relative]["purpose"] = purpose
    entries[relative]["sha256"] = sha256(REPOSITORY / relative)


def install(candidate: Path) -> None:
    document, glb_size = verify_candidate(candidate)
    manifest_path = (
        REPOSITORY
        / "contrib/workout-game-assets/manifests/RB-01-rider-bike.json"
    )
    manifest = assets.load_json_file(manifest_path)

    destinations: list[tuple[Path, Path]] = [
        (candidate / GLB_NAME, REPOSITORY / GENERATED_RELATIVE),
    ]
    destinations.extend(
        (candidate / "audit" / name, REPOSITORY / AUDIT_RELATIVE / name)
        for name in AUDIT_NAMES
    )
    destinations.extend(
        (
            candidate / "balsam" / "meshes" / mesh_file_name(node_name),
            REPOSITORY / RUNTIME_MESH_RELATIVE / mesh_file_name(node_name),
        )
        for node_name in sorted(EXPECTED_MESH_NODES)
    )

    original_files = {
        destination: destination.read_bytes() if destination.exists() else None
        for _, destination in destinations
    }
    original_manifest = manifest_path.read_bytes()
    try:
        for source, destination in destinations:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)

        blend_hash = sha256(REPOSITORY / BLEND_RELATIVE)
        manifest["source"].update({
            "url": (
                "https://github.com/kaartine/GoldenCheetah/blob/master/"
                + BLEND_RELATIVE
            ),
            "retrievedAt": "2026-09-10",
            "originalFileName": "WG_RiderBike.blend",
            "originalSha256": blend_hash,
            "generator": "Blender",
            "generatorVersion": "4.0.2",
            "promptOrScript": (
                f"{BLEND_RELATIVE}; {GENERATOR_RELATIVE}"
            ),
        })

        update_file_entry(manifest, GENERATOR_RELATIVE, "source")
        update_file_entry(manifest, BLEND_RELATIVE, "source")
        update_file_entry(manifest, INSTALLER_RELATIVE, "source")
        update_file_entry(manifest, PIPELINE_RELATIVE, "source")
        for entry in manifest["files"]:
            path = REPOSITORY / entry["path"]
            if path.is_file():
                entry["sha256"] = sha256(path)

        measured = technical_metadata(document, glb_size)
        manifest["technical"].update(measured)
        manifest["technical"]["budgets"].update({
            "maxGlbBytes": 614400,
            "maxTrianglesLod0": 9000,
        })
        manifest["processing"]["steps"] = [
            "Open the committed editable Blender source in Blender 4.0.2.",
            "Run generate_rider_bike.py in source mode with dimension, pivot, topology, rights-metadata and budget checks.",
            "Export glTF 2.0 GLB with opaque materials, extras and no cameras, lights, textures or animations.",
            "Export the source twice and require byte-identical GLB output.",
            "Validate the GLB structure, metadata, transforms, bounds and budgets with the repository asset policy.",
            "Convert the candidate twice with Qt Balsam 6.8.3 and compare every generated output byte.",
            "Require the exact 17-mesh runtime inventory used by the existing animation pivots.",
            "Package the converted meshes and shared rider surface texture in workout-game-assets.qrc.",
            "Render front, rear, side and chase audit views twice with fixed pose, cameras, field of view and lighting.",
            "Require repeated audit output to be byte-identical before installation.",
        ]
        manifest["review"]["reviewedAt"] = "2026-09-10"
        manifest["review"]["notes"] = (
            "Approved original stylized rider and generic enduro 29er runtime "
            "asset. The editable Blender source and generated runtime meshes "
            "preserve the established axle, crank, steering, pelvis, shadow "
            "and camera pivots plus the snapshot-driven wheel, pedal, "
            "suspension and rider-pose animation contract. The model uses an "
            "open twin-link rear triangle, tapered frame members, detailed "
            "cockpit, one-by drivetrain, platform pedals, suspension, "
            "open-face helmet and large unbranded black treaded tires. It "
            "contains no named bicycle design, person likeness, branded "
            "tread, logo, source mesh or proprietary surface. No endorsement "
            "is claimed or implied. Deterministic Blender export, Balsam "
            "conversion, packaged-resource loading and fixed-view visual "
            "audits are release requirements."
        )

        manifest_path.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )
        assets.validate_repository(REPOSITORY)
    except Exception:
        for destination, content in original_files.items():
            if content is None:
                destination.unlink(missing_ok=True)
            else:
                destination.write_bytes(content)
        manifest_path.write_bytes(original_manifest)
        raise

    print(
        f"Installed {ASSET_ID}: {measured['trianglesLod0']} triangles, "
        f"{glb_size} GLB bytes, {len(EXPECTED_MESH_NODES)} runtime meshes"
    )


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Install a validated rider-bike build into GoldenCheetah"
    )
    parser.add_argument(
        "--candidate-directory",
        type=Path,
        required=True,
        help="directory containing WG_RiderBike.glb, audit/ and balsam/",
    )
    arguments = parser.parse_args()
    install(arguments.candidate_directory.resolve())


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
