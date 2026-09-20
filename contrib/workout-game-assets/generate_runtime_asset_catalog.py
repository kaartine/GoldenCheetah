#!/usr/bin/env python3
"""Generate the deterministic, approved-only Workout Game runtime catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET
from typing import Any

from validate_assets import (
    AssetValidationError,
    load_json_file,
    read_regular_file,
    validate_repository,
)


CATALOG_SCHEMA_VERSION = 1
GENERATOR_VERSION = 2
MAXIMUM_CATALOG_BYTES = 1024 * 1024
MAXIMUM_ASSETS = 256
QRC_PATH = Path("src/Resources/workout-game-assets.qrc")
OUTPUT_PATH = Path("src/Resources/json/workout-game-asset-catalog.json")
MANIFEST_DIRECTORY = Path("contrib/workout-game-assets/manifests")
PACKAGED_PURPOSES = frozenset(("runtime", "texture"))
CORE_RESOURCE_PATHS = frozenset((
    "src/Resources/audio/workout-game-feature.wav",
    "src/Resources/audio/workout-game-landing.wav",
    "src/Resources/json/workout-game-asset-catalog.json",
    "src/Train/qml/WorkoutGameDevelopmentAsset.qml",
    "src/Train/qml/WorkoutGameLandingDust.qml",
    "src/Train/qml/WorkoutGameSuccessFeedback.qml",
))


def _canonical_bytes(document: dict[str, Any]) -> bytes:
    return (
        json.dumps(
            document,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")


def catalog_digest(payload: dict[str, Any]) -> str:
    return hashlib.sha256(_canonical_bytes(payload)).hexdigest()


def _repository_path(root: Path, path: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError as error:
        raise AssetValidationError(
            f"qrc resource escapes repository: {path}"
        ) from error


def qrc_aliases(root: Path) -> dict[str, str]:
    root = root.resolve(strict=True)
    qrc_path = root / QRC_PATH
    data = read_regular_file(qrc_path, 1024 * 1024)
    if b"<!DOCTYPE" in data.upper() or b"<!ENTITY" in data.upper():
        raise AssetValidationError("runtime asset qrc may not contain a DTD")
    try:
        document = ET.fromstring(data)
    except ET.ParseError as error:
        raise AssetValidationError(f"invalid runtime asset qrc: {error}") from error

    result: dict[str, str] = {}
    seen_aliases: set[str] = set()
    for resource in document.findall("qresource"):
        prefix = resource.get("prefix", "/").strip("/")
        for entry in resource.findall("file"):
            relative = (entry.text or "").strip()
            alias = (entry.get("alias") or relative).strip("/")
            if not relative or not alias or ".." in Path(alias).parts:
                raise AssetValidationError("unsafe runtime asset qrc entry")
            try:
                source = (qrc_path.parent / relative).resolve(strict=True)
            except OSError as error:
                raise AssetValidationError(
                    f"qrc resource is unavailable: {relative}"
                ) from error
            repository_path = _repository_path(root, source)
            qrc_url = "qrc:/" + "/".join(part for part in (prefix, alias) if part)
            if repository_path in result or qrc_url in seen_aliases:
                raise AssetValidationError(
                    f"duplicate runtime asset qrc entry: {repository_path}"
                )
            result[repository_path] = qrc_url
            seen_aliases.add(qrc_url)
    return result


def _scaled_integer(name: str, value: Any, scale: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise AssetValidationError(f"{name} must be numeric")
    scaled = float(value) * scale
    rounded = round(scaled)
    if (
        not math.isfinite(scaled)
        or abs(scaled - rounded) > 1.0e-9
        or rounded < 0
        or rounded > maximum
    ):
        raise AssetValidationError(f"{name} is not exactly representable")
    return int(rounded)


def _catalog_physics(manifest: dict[str, Any]) -> dict[str, Any]:
    source = manifest["physics"]
    result: dict[str, Any] = {
        "authority": "external",
        "interaction": source["interaction"],
    }
    surface = source.get("surface")
    if surface is not None:
        result["surface"] = {
            "coulombFrictionMilli": _scaled_integer(
                "Coulomb friction",
                surface["coulombFriction"],
                1000,
                2000,
            ),
            "restitutionMilli": _scaled_integer(
                "restitution", surface["restitution"], 1000, 250
            ),
        }
    route_profiles = source.get("routeProfiles", [])
    if route_profiles:
        result["routeProfiles"] = sorted(
            ({"profileId": profile["profileId"],
              "routeKey": profile["routeKey"]}
             for profile in route_profiles),
            key=lambda item: (item["routeKey"], item["profileId"]),
        )
    return result


def _catalog_profiles(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    surface = _catalog_physics(manifest).get("surface")
    profiles = []
    for source in manifest["physics"].get("routeProfiles", []):
        profile = {
            "chains": [
                {"points": [dict(point) for point in chain["points"]]}
                for chain in source["chains"]
            ],
            "kind": source["kind"],
            "profileId": source["profileId"],
            "profileVersion": source["profileVersion"],
            "surface": dict(surface),
        }
        if "difficultyScale" in source:
            profile["difficultyScale"] = dict(source["difficultyScale"])
        profiles.append(profile)
    return profiles


def _admitted_asset(
    root: Path, manifest: dict[str, Any], aliases: dict[str, str]
) -> dict[str, Any] | None:
    review = manifest["review"]
    if review["status"] != "approved":
        return None
    license_data = manifest["license"]
    scopes = set(license_data.get("distributionScopes", []))
    if (
        license_data["decision"] not in {"allow", "conditional"}
        or not license_data["modificationAllowed"]
        or not license_data["redistributionAllowed"]
        or not {"source", "appimage"}.issubset(scopes)
    ):
        raise AssetValidationError(
            f"asset is not approved for production: {manifest['assetId']}"
        )
    if not review.get("reviewer") or not review.get("reviewedAt"):
        raise AssetValidationError(
            f"approved asset lacks review evidence: {manifest['assetId']}"
        )
    accepted_clearance = {"clear", "not-applicable"}
    for field in (
        "trademarkStatus",
        "personReleaseStatus",
        "propertyReleaseStatus",
    ):
        if review.get(field) not in accepted_clearance:
            raise AssetValidationError(
                f"approved asset lacks {field} clearance: {manifest['assetId']}"
            )

    resources = []
    for entry in manifest["files"]:
        if entry["purpose"] not in PACKAGED_PURPOSES:
            continue
        repository_path = entry["path"]
        if repository_path not in aliases:
            raise AssetValidationError(
                f"approved asset resource is absent from qrc: {repository_path}"
            )
        data = read_regular_file(root / repository_path, 64 * 1024 * 1024)
        resources.append({
            "bytes": len(data),
            "purpose": entry["purpose"],
            "repositoryPath": repository_path,
            "sha256": hashlib.sha256(data).hexdigest(),
            "url": aliases[repository_path],
        })
    if not resources:
        raise AssetValidationError(
            f"approved asset has no packaged resources: {manifest['assetId']}"
        )

    return {
        "assetId": manifest["assetId"],
        "physics": _catalog_physics(manifest),
        "resources": sorted(
            resources,
            key=lambda item: (item["repositoryPath"], item["purpose"]),
        ),
        "role": manifest["role"],
    }


def _verify_qrc_inventory(
    manifests: list[dict[str, Any]], aliases: dict[str, str]
) -> None:
    declared = {
        entry["path"]
        for manifest in manifests
        for entry in manifest["files"]
        if entry["purpose"] in PACKAGED_PURPOSES
    }
    missing = sorted(declared - aliases.keys())
    if missing:
        raise AssetValidationError(
            f"manifest resource is absent from qrc: {missing[0]}"
        )
    unmanaged = sorted(aliases.keys() - declared - CORE_RESOURCE_PATHS)
    if unmanaged:
        raise AssetValidationError(
            f"unmanaged runtime asset qrc resource: {unmanaged[0]}"
        )


def generate_catalog_bytes(root: Path) -> bytes:
    root = root.resolve(strict=True)
    validate_repository(root)
    aliases = qrc_aliases(root)
    manifests = [
        load_json_file(path)
        for path in sorted((root / MANIFEST_DIRECTORY).glob("*.json"))
    ]
    _verify_qrc_inventory(manifests, aliases)
    assets = [
        admitted
        for manifest in manifests
        if (admitted := _admitted_asset(root, manifest, aliases)) is not None
    ]
    assets.sort(key=lambda item: item["assetId"])
    if not assets or len(assets) > MAXIMUM_ASSETS:
        raise AssetValidationError("runtime asset catalog size is invalid")

    profiles_by_id: dict[str, dict[str, Any]] = {}
    total_profile_points = 0
    for manifest in manifests:
        if manifest["review"]["status"] != "approved":
            continue
        for profile in _catalog_profiles(manifest):
            profile_id = profile["profileId"]
            previous = profiles_by_id.get(profile_id)
            if previous is not None and previous != profile:
                raise AssetValidationError(
                    f"conflicting runtime profile ID: {profile_id}"
                )
            if previous is None:
                profiles_by_id[profile_id] = profile
                total_profile_points += sum(
                    len(chain["points"]) for chain in profile["chains"]
                )
    if len(profiles_by_id) > 512 or total_profile_points > 32768:
        raise AssetValidationError("runtime profile catalog size is invalid")
    profiles = [profiles_by_id[key] for key in sorted(profiles_by_id)]

    payload = {
        "assets": assets,
        "generatorVersion": GENERATOR_VERSION,
        "profiles": profiles,
        "schemaVersion": CATALOG_SCHEMA_VERSION,
    }
    document = dict(payload)
    document["catalogSha256"] = catalog_digest(payload)
    encoded = _canonical_bytes(document)
    if len(encoded) > MAXIMUM_CATALOG_BYTES:
        raise AssetValidationError("runtime asset catalog is too large")
    return encoded


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="wb", dir=path.parent, prefix=path.name + ".", delete=False
    ) as temporary:
        temporary.write(data)
        temporary.flush()
        os.fsync(temporary.fileno())
        temporary_path = Path(temporary.name)
    try:
        os.replace(temporary_path, path)
    finally:
        temporary_path.unlink(missing_ok=True)


def main() -> None:
    repository = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(
        description="Generate approved Workout Game runtime asset catalog"
    )
    parser.add_argument("--root", type=Path, default=repository)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()
    root = arguments.root.resolve(strict=True)
    output = arguments.output or root / OUTPUT_PATH
    generated = generate_catalog_bytes(root)
    if arguments.check:
        if read_regular_file(output, MAXIMUM_CATALOG_BYTES) != generated:
            raise SystemExit("Workout Game runtime asset catalog is stale")
        return
    _write_atomic(output, generated)


if __name__ == "__main__":
    main()
