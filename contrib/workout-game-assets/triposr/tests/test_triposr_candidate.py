from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from typing import Any, Callable

try:
    import jsonschema
except ImportError:  # Optional in minimal build environments.
    jsonschema = None


MODULE_PATH = Path(__file__).resolve().parents[1] / "triposr_candidate.py"
SPEC = importlib.util.spec_from_file_location("triposr_candidate", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
candidate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(candidate)


def pad4(data: bytes, byte: bytes = b"\x00") -> bytes:
    return data + byte * ((-len(data)) % 4)


def make_glb(
    path: Path,
    *,
    positions: list[tuple[float, float, float]] | None = None,
    indices: list[int] | None = None,
    materials: int = 0,
    texture_bytes: int = 0,
    unreferenced_payload: bytes = b"",
    external_buffer: bool = False,
    mutate: Callable[[dict[str, Any]], None] | None = None,
) -> None:
    positions = positions or [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    indices = indices or [0, 1, 2]
    position_data = b"".join(struct.pack("<fff", *position) for position in positions)
    index_data = b"".join(struct.pack("<H", index) for index in indices)
    binary = pad4(position_data) + pad4(index_data)
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": len(position_data)},
        {
            "buffer": 0,
            "byteOffset": len(pad4(position_data)),
            "byteLength": len(index_data),
        },
    ]
    images = []
    textures = []
    if texture_bytes:
        texture_offset = len(binary)
        texture = b"x" * texture_bytes
        binary += pad4(texture)
        views.append({
            "buffer": 0,
            "byteOffset": texture_offset,
            "byteLength": texture_bytes,
        })
        images.append({"bufferView": 2, "mimeType": "image/png"})
        textures.append({"source": 0})
    declared_binary_length = views[-1]["byteOffset"] + views[-1]["byteLength"]
    if unreferenced_payload:
        binary += pad4(unreferenced_payload)
        declared_binary_length = len(binary)
    buffer = {"byteLength": declared_binary_length}
    if external_buffer:
        buffer["uri"] = "model.bin"
    primitive: dict[str, Any] = {
        "attributes": {"POSITION": 0},
        "indices": 1,
    }
    if materials:
        primitive["material"] = 0
    document = {
        "asset": {"version": "2.0", "generator": "test"},
        "buffers": [buffer],
        "bufferViews": views,
        "accessors": [
            {
                "bufferView": 0,
                "componentType": 5126,
                "count": len(positions),
                "type": "VEC3",
            },
            {
                "bufferView": 1,
                "componentType": 5123,
                "count": len(indices),
                "type": "SCALAR",
            },
        ],
        "materials": [{} for _ in range(materials)],
        "meshes": [{
            "primitives": [primitive]
        }],
        "nodes": [{"mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    if images:
        document["images"] = images
        document["textures"] = textures
    if mutate is not None:
        mutate(document)
    json_chunk = pad4(json.dumps(document, separators=(",", ":")).encode(), b" ")
    chunks = struct.pack("<II", len(json_chunk), candidate.JSON_CHUNK) + json_chunk
    chunks += struct.pack("<II", len(binary), candidate.BIN_CHUNK) + binary
    path.write_bytes(struct.pack("<4sII", b"glTF", 2, 12 + len(chunks)) + chunks)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class TripoSRCandidateTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.reference = self.root / "reference.png"
        self.reference.write_bytes(b"\x89PNG\r\n\x1a\nprivate-test-image")
        self.model_weights = self.root / "model.ckpt"
        self.model_weights.write_bytes(b"private-test-model-weights")
        self.lod0 = self.root / "mesh-lod0.glb"
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
        )
        self.lod1 = self.root / "mesh-lod1.glb"
        make_glb(self.lod1)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def args(self, destination: str = "candidate") -> argparse.Namespace:
        return argparse.Namespace(
            candidate_id="bike-side-001",
            target_asset_id="RB-01",
            target_role="rider-bike",
            reference=self.reference,
            reference_sha256=sha256(self.reference),
            reference_rights="unverified-review-only",
            source_uri="https://example.invalid/reference.png",
            lod0_glb=self.lod0,
            lod0_glb_sha256=sha256(self.lod0),
            lod1_glb=self.lod1,
            lod1_glb_sha256=sha256(self.lod1),
            candidate_dir=self.root / destination,
            triposr_revision="a" * 40,
            model_id="stabilityai/TripoSR",
            model_weights=self.model_weights,
            model_weights_sha256=sha256(self.model_weights),
            model_license="MIT",
            seed=0,
            mc_resolution=192,
            chunk_size=4096,
            foreground_removal=True,
            unit_meters=1.0,
            up_axis="+Y",
            forward_axis="+Z",
            max_glb_bytes=1024 * 1024,
            max_triangles_lod0=100,
            max_triangles_lod1=50,
            max_materials=4,
            max_textures=0,
            max_texture_bytes=0,
            collision_proxy_glb=None,
            collision_proxy_glb_sha256=None,
            collision_proxy_generator=None,
        )

    def test_import_emits_deterministic_review_only_descriptor(self) -> None:
        first = self.args("first")
        second = self.args("second")
        candidate.import_candidate(first)
        candidate.import_candidate(second)
        first_bytes = (first.candidate_dir / "candidate.json").read_bytes()
        self.assertEqual(first_bytes, (second.candidate_dir / "candidate.json").read_bytes())
        descriptor = candidate.verify_candidate_directory(first.candidate_dir)
        self.assertEqual(descriptor["status"], "review-required")
        self.assertEqual(descriptor["technical"]["lod0"]["triangles"], 2)
        self.assertEqual(descriptor["technical"]["lod1"]["triangles"], 1)
        self.assertEqual(descriptor["technical"]["lod1TriangleRatio"], 0.5)
        self.assertEqual(
            descriptor["technical"]["lod1"]["bounds"]["maximum"],
            [1.0, 1.0, 0.0],
        )
        self.assertEqual(descriptor["installation"], {
            "automatic": False,
            "targetAssetId": "RB-01",
            "targetRole": "rider-bike",
            "decision": "prohibited-pending-human-review",
        })
        self.assertNotIn(str(self.reference), first_bytes.decode())

    def test_rejects_reference_hash_mismatch(self) -> None:
        args = self.args()
        args.reference_sha256 = "0" * 64
        with self.assertRaisesRegex(candidate.CandidateError, "reference input SHA-256 mismatch"):
            candidate.import_candidate(args)

    def test_rejects_output_hash_mismatch(self) -> None:
        args = self.args()
        args.lod0_glb_sha256 = "0" * 64
        with self.assertRaisesRegex(candidate.CandidateError, "LOD0 GLB SHA-256 mismatch"):
            candidate.import_candidate(args)

        args = self.args()
        args.lod1_glb_sha256 = "0" * 64
        with self.assertRaisesRegex(candidate.CandidateError, "LOD1 GLB SHA-256 mismatch"):
            candidate.import_candidate(args)

    def test_rejects_model_weights_hash_mismatch(self) -> None:
        args = self.args()
        args.model_weights_sha256 = "0" * 64
        with self.assertRaisesRegex(candidate.CandidateError, "model weights SHA-256 mismatch"):
            candidate.import_candidate(args)

    def test_verify_detects_candidate_tampering(self) -> None:
        args = self.args()
        candidate.import_candidate(args)
        with (args.candidate_dir / "candidate-lod0.glb").open("ab") as output:
            output.write(b"tampered")
        with self.assertRaisesRegex(candidate.CandidateError, "SHA-256 mismatch"):
            candidate.verify_candidate_directory(args.candidate_dir)

    def test_rejects_nonfinite_geometry(self) -> None:
        make_glb(self.lod0, positions=[(0.0, 0.0, 0.0), (float("nan"), 0.0, 0.0), (0.0, 1.0, 0.0)])
        args = self.args()
        with self.assertRaisesRegex(candidate.CandidateError, "non-finite accessor"):
            candidate.import_candidate(args)

    def test_rejects_invalid_index(self) -> None:
        make_glb(self.lod0, indices=[0, 1, 7])
        args = self.args()
        with self.assertRaisesRegex(candidate.CandidateError, "index exceeds"):
            candidate.import_candidate(args)

    def test_rejects_triangle_budget(self) -> None:
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
        )
        args = self.args()
        args.max_triangles_lod0 = 1
        with self.assertRaisesRegex(candidate.CandidateError, "triangle budget"):
            candidate.import_candidate(args)

    def test_rejects_material_budget_and_all_texture_payloads(self) -> None:
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
            materials=3,
        )
        args = self.args()
        args.max_materials = 2
        with self.assertRaisesRegex(candidate.CandidateError, "material budget"):
            candidate.import_candidate(args)
        make_glb(self.lod0, texture_bytes=16)
        args = self.args()
        args.lod0_glb_sha256 = sha256(self.lod0)
        with self.assertRaisesRegex(candidate.CandidateError, "unsupported top-level"):
            candidate.import_candidate(args)

    def test_rejects_external_glb_resources(self) -> None:
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
            external_buffer=True,
        )
        args = self.args()
        with self.assertRaisesRegex(candidate.CandidateError, "unsupported or missing fields"):
            candidate.import_candidate(args)

    def test_rejects_unsupported_gltf_structures_and_payload_fields(self) -> None:
        mutations = {
            "extension": lambda document: document.update({
                "extensionsUsed": ["EVIL_payload"],
                "extensionsRequired": ["EVIL_payload"],
                "extensions": {"EVIL_payload": {"uri": "file:///private/key"}},
            }),
            "camera": lambda document: document.update({"cameras": [{}]}),
            "animation": lambda document: document.update({"animations": [{}]}),
            "skin": lambda document: document.update({"skins": [{}]}),
            "arbitrary extras": lambda document: document["meshes"][0].update({
                "extras": {"private": "secret"}
            }),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                make_glb(self.lod0, mutate=mutation)
                args = self.args()
                with self.assertRaises(candidate.CandidateError):
                    candidate.import_candidate(args)

    def test_rejects_hidden_binary_payload_bytes(self) -> None:
        make_glb(self.lod0, unreferenced_payload=b"private-model-weights")
        with self.assertRaisesRegex(candidate.CandidateError, "payload bytes"):
            candidate.import_candidate(self.args())

    def test_accepts_only_known_triposr_processed_marker(self) -> None:
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
            mutate=lambda document: document["meshes"][0].update(
                {"extras": {"processed": True}}
            ),
        )
        candidate.import_candidate(self.args())

    def test_rejects_invalid_optional_attribute_reference(self) -> None:
        make_glb(
            self.lod0,
            mutate=lambda document: document["meshes"][0]["primitives"][0][
                "attributes"
            ].update({"NORMAL": 999_999}),
        )
        with self.assertRaisesRegex(candidate.CandidateError, "NORMAL accessor index"):
            candidate.import_candidate(self.args())

    def test_rejects_nonidentity_and_malformed_transforms(self) -> None:
        transforms = (
            {"translation": [1.0, 0.0, 0.0]},
            {"matrix": ["not-a-number"]},
        )
        for transform in transforms:
            with self.subTest(transform=transform):
                make_glb(
                    self.lod0,
                    mutate=lambda document, transform=transform: document["nodes"][0].update(
                        transform
                    ),
                )
                with self.assertRaises(candidate.CandidateError):
                    candidate.import_candidate(self.args())

    def test_rejects_accessor_decode_amplification(self) -> None:
        make_glb(
            self.lod0,
            mutate=lambda document: document["accessors"][0].update({
                "count": candidate.HARD_MAX_COMPONENT_VALUES + 1
            }),
        )
        with self.assertRaisesRegex(candidate.CandidateError, "component limit"):
            candidate.import_candidate(self.args())

    def test_rejects_mesh_instancing(self) -> None:
        def instance_mesh(document: dict[str, Any]) -> None:
            document["nodes"] = [{"mesh": 0}, {"mesh": 0}]
            document["scenes"] = [{"nodes": [0, 1]}]

        make_glb(self.lod0, mutate=instance_mesh)
        with self.assertRaisesRegex(candidate.CandidateError, "exactly one node"):
            candidate.import_candidate(self.args())

    def test_malformed_scene_references_raise_candidate_error(self) -> None:
        mutations = (
            lambda document: document["nodes"][0].update({"children": [[]]}),
            lambda document: document["scenes"][0].update({"nodes": [[]]}),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                make_glb(self.lod0, mutate=mutation)
                with self.assertRaises(candidate.CandidateError):
                    candidate.import_candidate(self.args())

    def test_rejects_source_uri_secrets(self) -> None:
        unsafe = (
            "https://user:password@example.invalid/reference.png",
            "https://example.invalid/reference.png?token=private",
            "https://example.invalid/reference.png#private",
        )
        for uri in unsafe:
            with self.subTest(uri=uri):
                args = self.args()
                args.source_uri = uri
                with self.assertRaisesRegex(candidate.CandidateError, "source-uri"):
                    candidate.import_candidate(args)

    def test_verify_rejects_extra_private_files(self) -> None:
        args = self.args()
        candidate.import_candidate(args)
        (args.candidate_dir / "reference.png").write_bytes(b"private")
        with self.assertRaisesRegex(candidate.CandidateError, "unexpected files"):
            candidate.verify_candidate_directory(args.candidate_dir)

    def test_verify_rejects_symlinked_candidate_payload(self) -> None:
        args = self.args()
        candidate.import_candidate(args)
        payload = args.candidate_dir / "candidate-lod0.glb"
        replacement = self.root / "replacement.glb"
        payload.rename(replacement)
        payload.symlink_to(replacement)
        with self.assertRaisesRegex(candidate.CandidateError, "cannot open"):
            candidate.verify_candidate_directory(args.candidate_dir)

    def test_snapshot_is_stable_after_source_path_changes(self) -> None:
        snapshot = candidate.read_external_snapshot(
            self.lod0, "LOD0", candidate.HARD_MAX_GLB_BYTES
        )
        original_hash = snapshot.sha256
        self.lod0.write_bytes(b"replacement")
        self.assertEqual(hashlib.sha256(snapshot.data).hexdigest(), original_hash)
        info = candidate.inspect_glb(snapshot.data, {
            "maxGlbBytes": candidate.HARD_MAX_GLB_BYTES,
            "maxTriangles": 100,
            "maxMaterials": 4,
            "maxTextures": 0,
            "maxTextureBytes": 0,
        })
        self.assertEqual(info["triangles"], 2)

    def test_rejects_budgets_above_hard_limits(self) -> None:
        changes = {
            "max_glb_bytes": candidate.HARD_MAX_GLB_BYTES + 1,
            "max_triangles_lod0": candidate.HARD_MAX_TRIANGLES_LOD0 + 1,
            "max_triangles_lod1": candidate.HARD_MAX_TRIANGLES_LOD1 + 1,
            "max_materials": candidate.HARD_MAX_MATERIALS + 1,
            "max_textures": 1,
            "max_texture_bytes": 1,
        }
        for field, value in changes.items():
            with self.subTest(field=field):
                args = self.args()
                setattr(args, field, value)
                with self.assertRaisesRegex(candidate.CandidateError, "hard safety limit"):
                    candidate.import_candidate(args)

    def test_rejects_invalid_material_reference(self) -> None:
        make_glb(
            self.lod0,
            materials=0,
            mutate=lambda document: document["meshes"][0]["primitives"][0].update(
                {"material": 0}
            ),
        )
        args = self.args()
        with self.assertRaisesRegex(candidate.CandidateError, "material index"):
            candidate.import_candidate(args)

    def test_rejects_nonorthogonal_axes(self) -> None:
        args = self.args()
        args.forward_axis = "-Y"
        with self.assertRaisesRegex(candidate.CandidateError, "orthogonal"):
            candidate.import_candidate(args)

    def test_requires_lod1_to_be_lighter_than_lod0(self) -> None:
        make_glb(
            self.lod1,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
        )
        args = self.args()
        with self.assertRaisesRegex(candidate.CandidateError, "LOD1 must have fewer"):
            candidate.import_candidate(args)

    def test_applies_configurable_lod1_triangle_budget(self) -> None:
        make_glb(
            self.lod0,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
                (0.0, 0.0, 2.0), (1.0, 0.0, 2.0), (0.0, 1.0, 2.0),
            ],
            indices=[0, 1, 2, 3, 4, 5, 6, 7, 8],
        )
        make_glb(
            self.lod1,
            positions=[
                (0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0),
                (0.0, 0.0, 1.0), (1.0, 0.0, 1.0), (0.0, 1.0, 1.0),
            ],
            indices=[0, 1, 2, 3, 4, 5],
        )
        args = self.args()
        args.max_triangles_lod1 = 1
        with self.assertRaisesRegex(candidate.CandidateError, "triangle budget"):
            candidate.import_candidate(args)

    def test_optional_collision_proxy_is_hashed_validated_and_recorded(self) -> None:
        collision = self.root / "collision.glb"
        make_glb(collision)
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        candidate.import_candidate(args)
        descriptor = candidate.verify_candidate_directory(args.candidate_dir)
        self.assertEqual(descriptor["files"][2]["role"], "collision-proxy")
        self.assertEqual(descriptor["files"][2]["sha256"], sha256(collision))
        self.assertEqual(
            descriptor["collisionProxyProvenance"]["generator"], "Blender 4.0.2"
        )
        self.assertEqual(
            descriptor["collisionProxyProvenance"]["derivedFromSha256"],
            descriptor["files"][0]["sha256"],
        )
        self.assertEqual(descriptor["technical"]["collisionProxy"]["triangles"], 1)
        self.assertEqual(descriptor["technical"]["collisionProxy"]["materials"], 0)
        self.assertEqual(
            set(path.name for path in args.candidate_dir.iterdir()),
            {
                "candidate.json", "candidate-lod0.glb", "candidate-lod1.glb",
                "candidate-collision.glb",
            },
        )

    def test_collision_proxy_arguments_are_all_or_nothing(self) -> None:
        collision = self.root / "collision.glb"
        make_glb(collision)
        args = self.args()
        args.collision_proxy_glb = collision
        with self.assertRaisesRegex(candidate.CandidateError, "all be provided"):
            candidate.import_candidate(args)
        args = self.args()
        args.collision_proxy_glb_sha256 = sha256(collision)
        with self.assertRaisesRegex(candidate.CandidateError, "all be provided"):
            candidate.import_candidate(args)
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        with self.assertRaisesRegex(candidate.CandidateError, "all be provided"):
            candidate.import_candidate(args)

    def test_collision_proxy_rejects_materials_and_non_position_attributes(self) -> None:
        collision = self.root / "collision.glb"
        make_glb(collision, materials=1)
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        with self.assertRaisesRegex(candidate.CandidateError, "must not contain materials"):
            candidate.import_candidate(args)

        make_glb(
            collision,
            mutate=lambda document: document["meshes"][0]["primitives"][0][
                "attributes"
            ].update({"COLOR_0": 0}),
        )
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        with self.assertRaisesRegex(candidate.CandidateError, "attributes are malformed"):
            candidate.import_candidate(args)

        make_glb(collision, texture_bytes=16)
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        with self.assertRaisesRegex(candidate.CandidateError, "unsupported top-level"):
            candidate.import_candidate(args)

    def test_collision_proxy_rejects_more_than_one_hundred_triangles(self) -> None:
        collision = self.root / "collision.glb"
        positions = [(float(index), 0.0, 0.0) for index in range(303)]
        make_glb(collision, positions=positions, indices=list(range(303)))
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        with self.assertRaisesRegex(candidate.CandidateError, "triangle budget"):
            candidate.import_candidate(args)

    def test_verify_detects_collision_proxy_tampering(self) -> None:
        collision = self.root / "collision.glb"
        make_glb(collision)
        args = self.args()
        args.collision_proxy_glb = collision
        args.collision_proxy_glb_sha256 = sha256(collision)
        args.collision_proxy_generator = "Blender 4.0.2"
        candidate.import_candidate(args)
        with (args.candidate_dir / "candidate-collision.glb").open("ab") as output:
            output.write(b"tampered")
        with self.assertRaisesRegex(candidate.CandidateError, "SHA-256 mismatch"):
            candidate.verify_candidate_directory(args.candidate_dir)

    def test_default_lod0_budget_is_eighteen_thousand_triangles(self) -> None:
        parsed = candidate.parser().parse_args([
            "import",
            "--candidate-id", "bike-side-001",
            "--target-asset-id", "RB-01",
            "--target-role", "rider-bike",
            "--reference", str(self.reference),
            "--reference-sha256", sha256(self.reference),
            "--reference-rights", "owned",
            "--source-uri", "local-private",
            "--lod0-glb", str(self.lod0),
            "--lod0-glb-sha256", sha256(self.lod0),
            "--lod1-glb", str(self.lod1),
            "--lod1-glb-sha256", sha256(self.lod1),
            "--candidate-dir", str(self.root / "parsed"),
            "--triposr-revision", "a" * 40,
            "--model-id", "stabilityai/TripoSR",
            "--model-weights", str(self.model_weights),
            "--model-weights-sha256", sha256(self.model_weights),
            "--model-license", "MIT",
            "--seed", "0",
            "--mc-resolution", "192",
            "--chunk-size", "4096",
            "--unit-meters", "1.0",
            "--up-axis", "+Y",
            "--forward-axis", "+Z",
        ])
        self.assertEqual(parsed.max_triangles_lod0, 18_000)
        self.assertEqual(parsed.max_triangles_lod1, 9_000)
        self.assertEqual(parsed.max_glb_bytes, 64 * 1024 * 1024)
        self.assertEqual(parsed.max_textures, 0)
        self.assertEqual(parsed.max_texture_bytes, 0)

    def test_rejects_paths_inside_repository(self) -> None:
        with self.assertRaisesRegex(candidate.CandidateError, "outside the repository"):
            candidate.external_destination(candidate.SCRIPT_DIRECTORY / "work")
        with self.assertRaisesRegex(candidate.CandidateError, "outside the repository"):
            candidate.read_external_snapshot(
                MODULE_PATH, "reference input", 10 * 1024 * 1024
            )

    def test_rejects_symlinked_input_and_destination_paths(self) -> None:
        linked_input = self.root / "linked-lod0.glb"
        linked_input.symlink_to(self.lod0)
        args = self.args()
        args.lod0_glb = linked_input
        args.lod0_glb_sha256 = sha256(self.lod0)
        with self.assertRaisesRegex(candidate.CandidateError, "traverse a symlink"):
            candidate.import_candidate(args)

        real_parent = self.root / "real-parent"
        real_parent.mkdir()
        linked_parent = self.root / "linked-parent"
        linked_parent.symlink_to(real_parent, target_is_directory=True)
        with self.assertRaisesRegex(candidate.CandidateError, "traverse a symlink"):
            candidate.external_destination(linked_parent / "candidate")

    def test_schema_is_valid_json_and_encodes_install_boundary(self) -> None:
        schema = json.loads(
            (MODULE_PATH.parent / "candidate-descriptor.schema.json").read_text()
        )
        installation = schema["properties"]["installation"]["properties"]
        self.assertEqual(installation["automatic"]["const"], False)
        self.assertEqual(
            installation["targetAssetId"]["pattern"],
            "^[A-Z]{2}-[0-9]{2}(?:-[a-z0-9-]+)?$",
        )
        self.assertIn("prop", installation["targetRole"]["enum"])
        self.assertEqual(
            installation["decision"]["const"],
            "prohibited-pending-human-review",
        )

    def test_generic_target_is_recorded_and_old_v1_descriptor_remains_verifiable(self) -> None:
        args = self.args()
        args.target_asset_id = "EN-10-mossy-log"
        args.target_role = "prop"
        candidate.import_candidate(args)
        descriptor_path = args.candidate_dir / "candidate.json"
        descriptor = json.loads(descriptor_path.read_text())
        self.assertEqual(descriptor["schemaVersion"], 2)
        self.assertEqual(descriptor["installation"]["targetAssetId"], "EN-10-mossy-log")
        self.assertEqual(descriptor["installation"]["targetRole"], "prop")

        descriptor["schemaVersion"] = 1
        descriptor["validation"]["validator"] = "triposr_candidate.py/1"
        descriptor["installation"] = {
            "automatic": False,
            "target": "RB-01",
            "decision": "prohibited-pending-human-review",
        }
        candidate.validate_descriptor(descriptor)

    def test_invalid_generic_target_is_rejected(self) -> None:
        args = self.args()
        args.target_asset_id = "../../escape"
        with self.assertRaisesRegex(candidate.CandidateError, "installation target"):
            candidate.import_candidate(args)

    def test_wrong_typed_descriptor_version_is_rejected_cleanly(self) -> None:
        args = self.args()
        candidate.import_candidate(args)
        descriptor = json.loads((args.candidate_dir / "candidate.json").read_text())
        descriptor["schemaVersion"] = []
        with self.assertRaisesRegex(candidate.CandidateError, "version or kind"):
            candidate.validate_descriptor(descriptor)

    @unittest.skipUnless(jsonschema is not None, "jsonschema is not installed")
    def test_schema_matches_safe_runtime_contract(self) -> None:
        schema = json.loads(
            (MODULE_PATH.parent / "candidate-descriptor.schema.json").read_text()
        )
        jsonschema.Draft202012Validator.check_schema(schema)
        args = self.args()
        candidate.import_candidate(args)
        descriptor = json.loads((args.candidate_dir / "candidate.json").read_text())
        jsonschema.validate(descriptor, schema)

        collision = self.root / "schema-collision.glb"
        make_glb(collision)
        collision_args = self.args("schema-collision-candidate")
        collision_args.collision_proxy_glb = collision
        collision_args.collision_proxy_glb_sha256 = sha256(collision)
        collision_args.collision_proxy_generator = "Blender 4.0.2"
        candidate.import_candidate(collision_args)
        collision_descriptor = json.loads(
            (collision_args.candidate_dir / "candidate.json").read_text()
        )
        jsonschema.validate(collision_descriptor, schema)

        unsafe_uri = json.loads(json.dumps(descriptor))
        unsafe_uri["sourceReference"]["sourceUri"] = "file:///private/reference.png"
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(unsafe_uri, schema)

        unsafe_checks = json.loads(json.dumps(descriptor))
        unsafe_checks["validation"]["checks"] = [f"made-up-{index}" for index in range(8)]
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(unsafe_checks, schema)

        unsafe_budget = json.loads(json.dumps(descriptor))
        unsafe_budget["validation"]["budgets"]["maxGlbBytes"] = 64 * 1024 * 1024 + 1
        with self.assertRaises(jsonschema.ValidationError):
            jsonschema.validate(unsafe_budget, schema)


if __name__ == "__main__":
    unittest.main()
