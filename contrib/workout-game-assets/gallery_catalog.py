#!/usr/bin/env python3
"""Build the Workout Game review catalog from committed asset manifests."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
from types import ModuleType


@dataclass(frozen=True)
class GalleryAsset:
    asset_id: str
    display_name: str
    role: str
    path: Path
    triangles: int
    review_only: bool = False
    lod_level: str = ""
    expected_sha256: str = ""
    expected_bytes: int = 0
    candidate_directory: Path | None = None
    selection_aliases: tuple[str, ...] = ()


def gallery_asset_index(assets: list[GalleryAsset], identifier: str) -> int:
    """Resolve one user-facing gallery selector without silent fallback."""
    if not assets:
        raise ValueError("gallery has no assets")
    if not identifier:
        return 0
    normalized = identifier.casefold()
    matches = []
    for index, asset in enumerate(assets):
        candidates = {
            asset.asset_id.casefold(),
            asset.display_name.casefold(),
            asset.path.stem.casefold(),
            *(alias.casefold() for alias in asset.selection_aliases),
        }
        if normalized in candidates:
            matches.append(index)
    if len(matches) != 1:
        reason = "ambiguous" if matches else "unknown"
        raise ValueError(f"{reason} gallery asset selector: {identifier}")
    return matches[0]


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


def _triposr_validator(repository: Path) -> ModuleType:
    module_path = (
        repository.resolve()
        / "contrib/workout-game-assets/triposr/triposr_candidate.py"
    )
    if not module_path.is_file():
        raise FileNotFoundError(f"TripoSR candidate validator not found: {module_path}")
    specification = importlib.util.spec_from_file_location(
        "workout_game_triposr_candidate", module_path
    )
    if specification is None or specification.loader is None:
        raise RuntimeError(f"cannot load TripoSR candidate validator: {module_path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    return module


def _external_candidate_directory(repository: Path, candidate_directory: Path) -> Path:
    repository = repository.resolve(strict=True)
    expanded = candidate_directory.expanduser()
    absolute = expanded if expanded.is_absolute() else Path.cwd() / expanded
    for component in (absolute, *absolute.parents):
        if component.exists() and component.is_symlink():
            raise ValueError("candidate directory may not traverse a symlink")
        if component == component.parent:
            break
    candidate = absolute.resolve(strict=True)
    if not candidate.is_dir():
        raise ValueError(f"candidate directory is not a directory: {candidate}")
    try:
        candidate.relative_to(repository)
    except ValueError:
        return candidate
    raise ValueError("candidate directory must remain outside the repository")


def load_candidate_gallery_assets(
    repository: Path, candidate_directory: Path
) -> list[GalleryAsset]:
    """Return a verified external TripoSR LOD pair for review only."""
    repository = repository.resolve(strict=True)
    candidate = _external_candidate_directory(repository, candidate_directory)
    validator = _triposr_validator(repository)
    document = validator.verify_candidate_directory(candidate)

    candidate_id = document["candidateId"]
    expected_files = (
        ("review-model-lod0", "candidate-lod0.glb"),
        ("review-model-lod1", "candidate-lod1.glb"),
    )
    files = document.get("files", [])
    if len(files) < 2 or tuple(
        (entry.get("role"), entry.get("name")) for entry in files[:2]
    ) != expected_files:
        raise ValueError("verified candidate has an unexpected LOD file inventory")
    assets: list[GalleryAsset] = []
    for lod_level, file_entry in zip(("LOD0", "LOD1"), files):
        relative_path = Path(file_entry["name"])
        if relative_path.is_absolute() or relative_path.parts != (relative_path.name,):
            raise ValueError(
                f"candidate {lod_level} path leaves candidate directory"
            )
        path = (candidate / relative_path).resolve(strict=True)
        try:
            path.relative_to(candidate)
        except ValueError as error:
            raise ValueError(
                f"candidate {lod_level} path leaves candidate directory"
            ) from error
        technical = document["technical"][lod_level.casefold()]
        assets.append(GalleryAsset(
            asset_id=f"triposr-review-{candidate_id}-{lod_level.casefold()}",
            display_name=f"REVIEW ONLY - {candidate_id} - TripoSR {lod_level}",
            role=f"review-only TripoSR candidate {lod_level}",
            path=path,
            triangles=int(technical["triangles"]),
            review_only=True,
            lod_level=lod_level,
            expected_sha256=file_entry["sha256"],
            expected_bytes=int(file_entry["bytes"]),
            candidate_directory=candidate,
            selection_aliases=(candidate_id,) if lod_level == "LOD0" else (),
        ))
    return assets


def validate_gallery_asset_file(asset: GalleryAsset) -> None:
    """Reject a review candidate changed after catalog verification."""
    if not asset.review_only:
        return
    if (
        asset.candidate_directory is None
        or not asset.expected_sha256
        or asset.expected_bytes <= 0
    ):
        raise ValueError("review-only gallery asset lacks verification metadata")

    candidate = asset.candidate_directory.resolve(strict=True)
    path = asset.path.resolve(strict=True)
    try:
        path.relative_to(candidate)
    except ValueError as error:
        raise ValueError("review-only asset path leaves candidate directory") from error
    if path != asset.path:
        raise ValueError("review-only asset path may not traverse a symlink")

    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        initial = os.fstat(descriptor)
        if not stat.S_ISREG(initial.st_mode) or initial.st_size != asset.expected_bytes:
            raise ValueError(
                f"review-only asset changed after verification: {asset.lod_level}"
            )
        digest_builder = hashlib.sha256()
        bytes_read = 0
        while bytes_read <= asset.expected_bytes:
            block = os.read(descriptor, min(1024 * 1024, asset.expected_bytes + 1))
            if not block:
                break
            bytes_read += len(block)
            if bytes_read > asset.expected_bytes:
                raise ValueError(
                    f"review-only asset changed after verification: {asset.lod_level}"
                )
            digest_builder.update(block)
        final = os.fstat(descriptor)
        if (
            bytes_read != asset.expected_bytes
            or initial.st_dev != final.st_dev
            or initial.st_ino != final.st_ino
            or initial.st_size != final.st_size
            or initial.st_mtime_ns != final.st_mtime_ns
        ):
            raise ValueError(
                f"review-only asset changed after verification: {asset.lod_level}"
            )
        digest = digest_builder.hexdigest()
    finally:
        os.close(descriptor)
    if digest != asset.expected_sha256:
        raise ValueError(
            f"review-only asset changed after verification: {asset.lod_level}"
        )
