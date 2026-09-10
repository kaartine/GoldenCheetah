#!/usr/bin/env python3
"""Build the Workout Game review catalog from committed asset manifests."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path


@dataclass(frozen=True)
class GalleryAsset:
    asset_id: str
    display_name: str
    role: str
    path: Path
    triangles: int


def _repository_path(repository: Path, value: str) -> Path:
    repository = repository.resolve()
    path = (repository / value).resolve()
    try:
        path.relative_to(repository)
    except ValueError as error:
        raise ValueError(f"asset path leaves repository: {value}") from error
    return path


def load_gallery_assets(repository: Path) -> list[GalleryAsset]:
    """Return every manifest-backed GLB in stable display order."""
    repository = repository.resolve()
    manifests = repository / "contrib/workout-game-assets/manifests"
    assets: list[GalleryAsset] = []

    for manifest_path in sorted(manifests.glob("*.json")):
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        glb_files = [
            entry["path"]
            for entry in document.get("files", [])
            if Path(entry.get("path", "")).suffix.lower() == ".glb"
        ]
        if not glb_files:
            continue
        if len(glb_files) != 1:
            raise ValueError(
                f"{manifest_path.name} must contain exactly one GLB"
            )

        path = _repository_path(repository, glb_files[0])
        if not path.is_file():
            raise FileNotFoundError(path)
        technical = document.get("technical", {})
        assets.append(GalleryAsset(
            asset_id=document["assetId"],
            display_name=document["displayName"],
            role=document["role"],
            path=path,
            triangles=int(technical.get("trianglesLod0", 0)),
        ))

    identifiers = [asset.asset_id for asset in assets]
    if len(identifiers) != len(set(identifiers)):
        raise ValueError("gallery asset identifiers must be unique")
    return sorted(assets, key=lambda asset: (asset.role, asset.display_name))
