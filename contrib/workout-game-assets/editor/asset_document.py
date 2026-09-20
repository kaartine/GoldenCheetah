#!/usr/bin/env python3
"""Validated, atomic edits for canonical Workout Game asset manifests."""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import re
import secrets
import stat
import tempfile
from typing import Any

if os.name == "nt":
    import msvcrt
else:
    import fcntl

from validate_assets import (
    AssetValidationError,
    load_json_bytes,
    load_json_file,
    open_directory_anchored,
    parse_glb_bytes,
    read_regular_file,
    resolve_repository_file,
    validate_against_schema,
    validate_glb_document,
    validate_manifest,
)


_ASSET_ID = re.compile(r"^[A-Z]{2}-[0-9]{2}(?:-[a-z0-9-]+)?$")
_SRGB = re.compile(r"^#[0-9a-f]{6}$")
_INTERACTIONS = frozenset((
    "visual-only",
    "surface",
    "obstacle",
    "rideable-feature",
))


class AssetDocumentError(ValueError):
    """The requested edit would violate the asset-document contract."""


class AssetDocumentConflict(AssetDocumentError):
    """The manifest changed after it was opened."""


@dataclass(frozen=True)
class AssetPhysics:
    authority: str = "external"
    interaction: str = "visual-only"
    coulomb_friction: float = 1.0
    restitution: float = 0.0
    collision_node: str = ""


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _finite_range(name: str, value: Any, minimum: float, maximum: float) -> float:
    if isinstance(value, bool):
        raise AssetDocumentError(f"{name} must be a finite number")
    try:
        result = float(value)
    except (TypeError, ValueError) as error:
        raise AssetDocumentError(f"{name} must be a finite number") from error
    if not math.isfinite(result) or result < minimum or result > maximum:
        raise AssetDocumentError(
            f"{name} must be between {minimum:g} and {maximum:g}"
        )
    return 0.0 if result == 0.0 else result


def _linear_channel_to_srgb(value: float) -> float:
    value = min(1.0, max(0.0, value))
    return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055


def _linear_color_to_hex(value: list[Any]) -> str:
    if len(value) < 3:
        raise AssetDocumentError("GLB base color is incomplete")
    channels = [
        min(255, max(0, round(_linear_channel_to_srgb(float(channel)) * 255.0)))
        for channel in value[:3]
    ]
    return "#" + "".join(f"{channel:02x}" for channel in channels)


def _lock_directory() -> Path:
    runtime = os.environ.get("XDG_RUNTIME_DIR")
    if runtime:
        directory = Path(runtime) / "goldencheetah-asset-locks"
    else:
        user = getattr(os, "getuid", lambda: 0)()
        directory = Path(tempfile.gettempdir()) / f"goldencheetah-asset-locks-{user}"
    if directory.is_symlink():
        raise AssetDocumentError(f"asset lock directory may not be a symlink: {directory}")
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    if directory.is_symlink() or not directory.is_dir():
        raise AssetDocumentError(f"invalid asset lock directory: {directory}")
    try:
        directory.chmod(0o700)
    except OSError:
        if os.name != "nt":
            raise
    return directory


@contextmanager
def _manifest_lock(manifest_path: Path):
    identity = str(manifest_path.resolve(strict=False)).encode("utf-8")
    lock_path = _lock_directory() / f"{hashlib.sha256(identity).hexdigest()}.lock"
    flags = (
        os.O_CREAT
        | os.O_RDWR
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_NOFOLLOW", 0)
    )
    descriptor = os.open(lock_path, flags, 0o600)
    try:
        if os.name == "nt":
            if os.fstat(descriptor).st_size == 0:
                os.write(descriptor, b"\0")
                os.fsync(descriptor)
            os.lseek(descriptor, 0, os.SEEK_SET)
            msvcrt.locking(descriptor, msvcrt.LK_LOCK, 1)
        else:
            fcntl.flock(descriptor, fcntl.LOCK_EX)
        yield
    finally:
        try:
            if os.name == "nt":
                os.lseek(descriptor, 0, os.SEEK_SET)
                msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(descriptor, fcntl.LOCK_UN)
        finally:
            os.close(descriptor)


def _regular_bytes(path: Path, description: str) -> bytes:
    try:
        return read_regular_file(path, 1024 * 1024)
    except AssetValidationError as error:
        if "changed while reading" in str(error):
            raise AssetDocumentConflict(f"{description} changed while reading")
        raise AssetDocumentError(f"invalid {description}: {error}") from error


def _regular_bytes_at(directory: int, name: str, description: str) -> bytes:
    if not name or name != Path(name).name:
        raise AssetDocumentError(f"invalid {description} name: {name}")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(name, flags, dir_fd=directory)
    except OSError as error:
        raise AssetDocumentError(f"cannot open {description}: {name}") from error
    try:
        initial = os.fstat(descriptor)
        if not stat.S_ISREG(initial.st_mode) or initial.st_nlink != 1:
            raise AssetDocumentError(f"invalid {description}: {name}")
        if initial.st_size > 1024 * 1024:
            raise AssetDocumentError(f"{description} exceeds 1 MiB: {name}")
        data = bytearray()
        while len(data) <= initial.st_size:
            block = os.read(descriptor, min(65536, initial.st_size + 1 - len(data)))
            if not block:
                break
            data.extend(block)
        final = os.fstat(descriptor)
        if (
            len(data) != initial.st_size
            or final.st_dev != initial.st_dev
            or final.st_ino != initial.st_ino
            or final.st_size != initial.st_size
            or final.st_mtime_ns != initial.st_mtime_ns
        ):
            raise AssetDocumentConflict(f"{description} changed while reading")
        return bytes(data)
    finally:
        os.close(descriptor)


def _default_interaction(role: str) -> str:
    if role in {"feature", "trail-tile"}:
        return "rideable-feature"
    if role == "terrain":
        return "surface"
    return "visual-only"


class AssetDocument:
    """One manifest plus its canonical GLB and editable metadata."""

    def __init__(
        self,
        repository: Path,
        manifest_path: Path,
        schema: dict[str, Any],
        document: dict[str, Any],
        fingerprint: str,
        glb_path: Path,
        maximum_glb_bytes: int,
        glb_fingerprint: str,
        material_names: tuple[str, ...],
        material_defaults: dict[str, dict[str, Any]],
        node_names: tuple[str, ...],
    ) -> None:
        self.repository = repository
        self.manifest_path = manifest_path
        self._schema = schema
        self._document = document
        self._fingerprint = fingerprint
        self._glb_path = glb_path
        self._maximum_glb_bytes = maximum_glb_bytes
        self._glb_fingerprint = glb_fingerprint
        self.material_names = material_names
        self._material_defaults = material_defaults
        self.node_names = node_names
        self._material_overrides = self._read_material_overrides(document)
        self._physics = self._read_physics(document)
        self._dirty = False
        self._validate_semantics()

    @classmethod
    def open(cls, repository: Path, asset_id: str) -> "AssetDocument":
        repository = Path(repository).expanduser().absolute()
        try:
            descriptor = open_directory_anchored(repository)
        except AssetValidationError as error:
            raise AssetDocumentError(f"invalid repository: {error}") from error
        else:
            os.close(descriptor)
        repository = repository.resolve(strict=True)
        if not _ASSET_ID.fullmatch(asset_id):
            raise AssetDocumentError(f"invalid asset id: {asset_id}")

        schema_path = repository / "doc/design/workout_game_asset_manifest.schema.json"
        try:
            schema = load_json_file(schema_path)
        except AssetValidationError as error:
            raise AssetDocumentError(str(error)) from error

        matches: list[tuple[Path, dict[str, Any], bytes]] = []
        manifests = repository / "contrib/workout-game-assets/manifests"
        try:
            descriptor = open_directory_anchored(manifests)
        except AssetValidationError as error:
            raise AssetDocumentError("asset manifest directory is unavailable") from error
        else:
            os.close(descriptor)
        for path in sorted(manifests.glob("*.json")):
            raw = _regular_bytes(path, "asset manifest")
            try:
                candidate = load_json_bytes(raw, str(path))
            except AssetValidationError as error:
                raise AssetDocumentError(str(error)) from error
            if candidate.get("assetId") == asset_id:
                matches.append((path, candidate, raw))
        if not matches:
            raise AssetDocumentError(f"asset id not found: {asset_id}")
        if len(matches) != 1:
            raise AssetDocumentError(f"duplicate asset id: {asset_id}")

        manifest_path, document, raw = matches[0]
        try:
            validate_against_schema(document, schema)
            validated = validate_manifest(repository, manifest_path, schema)
        except AssetValidationError as error:
            raise AssetDocumentError(str(error)) from error
        latest = _regular_bytes(manifest_path, "asset manifest")
        if _digest(latest) != _digest(raw):
            raise AssetDocumentConflict("asset manifest changed during validation")
        document = validated
        glb_entries = [
            entry["path"]
            for entry in document.get("files", [])
            if Path(entry.get("path", "")).suffix.lower() == ".glb"
        ]
        if len(glb_entries) != 1:
            raise AssetDocumentError("asset document must reference exactly one GLB")
        try:
            glb_path = resolve_repository_file(repository, glb_entries[0])
            maximum_glb_bytes = min(
                64 * 1024 * 1024,
                int(document["technical"].get("budgets", {}).get(
                    "maxGlbBytes", 64 * 1024 * 1024
                )),
            )
            glb_data = read_regular_file(glb_path, maximum_glb_bytes)
            glb = parse_glb_bytes(glb_data, str(glb_path))
            validate_glb_document(glb, len(glb_data), document)
        except (AssetValidationError, OSError) as error:
            raise AssetDocumentError(str(error)) from error
        material_names = tuple(item.get("name", "") for item in glb.get("materials", []))
        if any(not name for name in material_names):
            raise AssetDocumentError("every GLB material must have a name")
        if len(material_names) != len(set(material_names)):
            raise AssetDocumentError("GLB material names must be unique")
        material_defaults: dict[str, dict[str, Any]] = {}
        for material in glb.get("materials", []):
            pbr = material.get("pbrMetallicRoughness", {})
            material_defaults[material["name"]] = {
                "baseColorSrgb": _linear_color_to_hex(
                    pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
                ),
                "roughness": _finite_range(
                    "GLB roughness", pbr.get("roughnessFactor", 1.0), 0.0, 1.0
                ),
                "metallic": _finite_range(
                    "GLB metallic", pbr.get("metallicFactor", 1.0), 0.0, 1.0
                ),
            }
        node_names = tuple(item.get("name", "") for item in glb.get("nodes", []))

        return cls(
            repository,
            manifest_path,
            schema,
            document,
            _digest(raw),
            glb_path,
            maximum_glb_bytes,
            _digest(glb_data),
            material_names,
            material_defaults,
            node_names,
        )

    @property
    def asset_id(self) -> str:
        return str(self._document["assetId"])

    @property
    def display_name(self) -> str:
        return str(self._document["displayName"])

    @property
    def role(self) -> str:
        return str(self._document["role"])

    @property
    def source_summary(self) -> str:
        source = self._document["source"]
        return f"{source['provider']} / {source['author']}"

    @property
    def license_id(self) -> str:
        return str(self._document["license"]["spdxId"])

    @property
    def material_overrides(self) -> dict[str, dict[str, Any]]:
        return {name: dict(value) for name, value in self._material_overrides.items()}

    @property
    def material_defaults(self) -> dict[str, dict[str, Any]]:
        return {name: dict(value) for name, value in self._material_defaults.items()}

    @property
    def physics(self) -> AssetPhysics:
        return self._physics

    @property
    def review_status(self) -> str:
        return str(self._document["review"]["status"])

    @property
    def dirty(self) -> bool:
        return self._dirty

    def set_material(
        self,
        material_name: str,
        *,
        base_color_srgb: str | None = None,
        roughness: float | None = None,
        metallic: float | None = None,
    ) -> None:
        if material_name not in self.material_names:
            raise AssetDocumentError(f"unknown material: {material_name}")
        current = self._material_overrides.get(
            material_name, self._material_defaults[material_name]
        )
        color = current["baseColorSrgb"] if base_color_srgb is None else str(base_color_srgb).lower()
        if not _SRGB.fullmatch(color):
            raise AssetDocumentError("material color must be #rrggbb sRGB")
        updated = {
            "baseColorSrgb": color,
            "roughness": _finite_range(
                "roughness", current["roughness"] if roughness is None else roughness, 0.0, 1.0
            ),
            "metallic": _finite_range(
                "metallic", current["metallic"] if metallic is None else metallic, 0.0, 1.0
            ),
        }
        before = self._material_overrides.get(material_name)
        if updated == self._material_defaults[material_name]:
            self._material_overrides.pop(material_name, None)
            after = None
        else:
            self._material_overrides[material_name] = updated
            after = updated
        if before != after:
            self._dirty = True

    def set_physics(
        self,
        *,
        interaction: str | None = None,
        coulomb_friction: float | None = None,
        restitution: float | None = None,
        collision_node: str | None = None,
    ) -> None:
        selected_interaction = self._physics.interaction if interaction is None else interaction
        if selected_interaction not in _INTERACTIONS:
            raise AssetDocumentError(f"invalid interaction: {selected_interaction}")
        selected_node = self._physics.collision_node if collision_node is None else collision_node
        if selected_interaction == "visual-only":
            selected_node = ""
        if selected_node and selected_node not in self.node_names:
            raise AssetDocumentError(f"unknown collision node: {selected_node}")
        updated = AssetPhysics(
            authority="external",
            interaction=selected_interaction,
            coulomb_friction=_finite_range(
                "Coulomb friction",
                self._physics.coulomb_friction
                if coulomb_friction is None else coulomb_friction,
                0.0,
                2.0,
            ),
            restitution=_finite_range(
                "restitution",
                self._physics.restitution if restitution is None else restitution,
                0.0,
                0.25,
            ),
            collision_node=selected_node,
        )
        if updated != self._physics:
            self._physics = updated
            self._dirty = True

    def save(self) -> bool:
        if not self._dirty:
            return False
        self._validate_semantics()
        document = self._serialized_document()
        try:
            validate_against_schema(document, self._schema)
        except AssetValidationError as error:
            raise AssetDocumentError(str(error)) from error
        encoded = (json.dumps(
            document,
            indent=2,
            ensure_ascii=True,
            allow_nan=False,
        ) + "\n").encode("utf-8")

        parent = self.manifest_path.parent
        with _manifest_lock(self.manifest_path):
            directory = None
            temporary_name = ""
            temporary = None
            try:
                if os.name != "nt" and os.open in os.supports_dir_fd:
                    try:
                        directory = open_directory_anchored(parent)
                    except AssetValidationError as error:
                        raise AssetDocumentError(str(error)) from error
                    current = _regular_bytes_at(
                        directory, self.manifest_path.name, "asset manifest"
                    )
                else:
                    current = _regular_bytes(self.manifest_path, "asset manifest")
                if _digest(current) != self._fingerprint:
                    raise AssetDocumentConflict(
                        f"asset manifest changed on disk: {self.manifest_path}"
                    )

                try:
                    glb_data = read_regular_file(
                        self._glb_path, self._maximum_glb_bytes
                    )
                except AssetValidationError as error:
                    raise AssetDocumentError(str(error)) from error
                if _digest(glb_data) != self._glb_fingerprint:
                    raise AssetDocumentConflict(
                        f"asset GLB changed on disk: {self._glb_path}"
                    )
                try:
                    glb = parse_glb_bytes(glb_data, str(self._glb_path))
                    validate_glb_document(glb, len(glb_data), document)
                except AssetValidationError as error:
                    raise AssetDocumentError(str(error)) from error

                if directory is not None:
                    mode = os.stat(
                        self.manifest_path.name,
                        dir_fd=directory,
                        follow_symlinks=False,
                    ).st_mode & 0o777
                    flags = (
                        os.O_CREAT
                        | os.O_EXCL
                        | os.O_WRONLY
                        | getattr(os, "O_CLOEXEC", 0)
                        | getattr(os, "O_NOFOLLOW", 0)
                    )
                    for _attempt in range(32):
                        temporary_name = (
                            f".{self.manifest_path.name}.{secrets.token_hex(8)}.tmp"
                        )
                        try:
                            descriptor = os.open(
                                temporary_name, flags, mode, dir_fd=directory
                            )
                            break
                        except FileExistsError:
                            continue
                    else:
                        raise AssetDocumentError("cannot allocate temporary manifest")
                    with os.fdopen(descriptor, "wb") as output:
                        output.write(encoded)
                        output.flush()
                        os.fsync(output.fileno())
                    latest = _regular_bytes_at(
                        directory, self.manifest_path.name, "asset manifest"
                    )
                else:
                    mode = self.manifest_path.stat(
                        follow_symlinks=False
                    ).st_mode & 0o777
                    descriptor, native_name = tempfile.mkstemp(
                        prefix=f".{self.manifest_path.name}.",
                        suffix=".tmp",
                        dir=parent,
                    )
                    temporary = Path(native_name)
                    with os.fdopen(descriptor, "wb") as output:
                        if hasattr(os, "fchmod"):
                            os.fchmod(output.fileno(), mode)
                        else:
                            os.chmod(temporary, mode)
                        output.write(encoded)
                        output.flush()
                        os.fsync(output.fileno())
                    latest = _regular_bytes(self.manifest_path, "asset manifest")
                if _digest(latest) != self._fingerprint:
                    raise AssetDocumentConflict(
                        f"asset manifest changed on disk: {self.manifest_path}"
                    )
                if directory is None:
                    os.replace(temporary, self.manifest_path)
                else:
                    os.replace(
                        temporary_name,
                        self.manifest_path.name,
                        src_dir_fd=directory,
                        dst_dir_fd=directory,
                    )
                    temporary_name = ""
                    os.fsync(directory)
                self._document = document
                self._fingerprint = _digest(encoded)
                self._dirty = False
            finally:
                if directory is not None and temporary_name:
                    try:
                        os.unlink(temporary_name, dir_fd=directory)
                    except FileNotFoundError:
                        pass
                if directory is not None:
                    os.close(directory)
                if temporary is not None:
                    try:
                        temporary.unlink()
                    except FileNotFoundError:
                        pass
        return True

    def _serialized_document(self) -> dict[str, Any]:
        materials = [
            {
                "materialName": name,
                "baseColorSrgb": value["baseColorSrgb"],
                "roughness": value["roughness"],
                "metallic": value["metallic"],
            }
            for name, value in sorted(self._material_overrides.items())
        ]
        physics: dict[str, Any] = {
            "authority": "external",
            "interaction": self._physics.interaction,
        }
        if self._physics.interaction != "visual-only":
            physics["surface"] = {
                "coulombFriction": self._physics.coulomb_friction,
                "restitution": self._physics.restitution,
            }
        physics["collisionProxy"] = (
            {"kind": "node", "node": self._physics.collision_node}
            if self._physics.collision_node else {"kind": "none"}
        )

        if self._physics.interaction != "visual-only":
            route_profiles = self._document.get("physics", {}).get(
                "routeProfiles"
            )
            if route_profiles:
                physics["routeProfiles"] = route_profiles

        result: dict[str, Any] = {}
        for key, value in self._document.items():
            if key in {"materialOverrides", "physics"}:
                continue
            if key == "review":
                result["materialOverrides"] = materials
                result["physics"] = physics
                review = dict(value)
                review["status"] = "candidate"
                review.pop("reviewer", None)
                review.pop("reviewedAt", None)
                result[key] = review
            else:
                result[key] = value
        return result

    def _validate_semantics(self) -> None:
        unknown = sorted(set(self._material_overrides) - set(self.material_names))
        if unknown:
            raise AssetDocumentError(f"unknown material: {unknown[0]}")
        if self._physics.authority != "external":
            raise AssetDocumentError("asset physics authority must remain external")
        if self._physics.collision_node and self._physics.collision_node not in self.node_names:
            raise AssetDocumentError(
                f"unknown collision node: {self._physics.collision_node}"
            )
        if self._physics.interaction == "visual-only" and self._physics.collision_node:
            raise AssetDocumentError(
                "visual-only physics must not define a collision proxy"
            )

    @staticmethod
    def _read_material_overrides(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
        result: dict[str, dict[str, Any]] = {}
        for entry in document.get("materialOverrides", []):
            name = entry.get("materialName", "")
            if name in result:
                raise AssetDocumentError(f"duplicate material override: {name}")
            color = str(entry.get("baseColorSrgb", "")).lower()
            if not _SRGB.fullmatch(color):
                raise AssetDocumentError("material color must be #rrggbb sRGB")
            result[name] = {
                "baseColorSrgb": color,
                "roughness": _finite_range("roughness", entry.get("roughness"), 0.0, 1.0),
                "metallic": _finite_range("metallic", entry.get("metallic"), 0.0, 1.0),
            }
        return result

    @staticmethod
    def _read_physics(document: dict[str, Any]) -> AssetPhysics:
        physics = document.get("physics")
        if physics is None:
            return AssetPhysics(interaction=_default_interaction(document.get("role", "")))
        authority = physics.get("authority", "")
        interaction = physics.get("interaction", "")
        if authority != "external":
            raise AssetDocumentError("asset physics authority must remain external")
        if interaction not in _INTERACTIONS:
            raise AssetDocumentError(f"invalid interaction: {interaction}")
        has_surface = "surface" in physics
        if interaction == "visual-only" and has_surface:
            raise AssetDocumentError(
                "visual-only physics must not define surface properties"
            )
        if interaction != "visual-only" and not has_surface:
            raise AssetDocumentError(
                "interactive physics requires surface properties"
            )
        surface = physics.get("surface", {})
        proxy = physics.get("collisionProxy", {"kind": "none"})
        if interaction == "visual-only" and proxy.get("kind") != "none":
            raise AssetDocumentError(
                "visual-only physics must not define a collision proxy"
            )
        if proxy.get("kind") == "none" and "node" in proxy:
            raise AssetDocumentError("disabled collision proxy names a node")
        if proxy.get("kind") == "node" and not proxy.get("node"):
            raise AssetDocumentError("collision proxy node is missing")
        collision_node = proxy.get("node", "") if proxy.get("kind") == "node" else ""
        return AssetPhysics(
            authority=authority,
            interaction=interaction,
            coulomb_friction=_finite_range(
                "Coulomb friction", surface.get("coulombFriction", 1.0), 0.0, 2.0
            ),
            restitution=_finite_range(
                "restitution", surface.get("restitution", 0.0), 0.0, 0.25
            ),
            collision_node=collision_node,
        )
