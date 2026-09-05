#!/usr/bin/env python3

import copy
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]
ASSET_TOOLS = REPOSITORY / "contrib" / "workout-game-assets"
sys.path.insert(0, str(ASSET_TOOLS))

import validate_assets as assets  # noqa: E402


SCHEMA_PATH = REPOSITORY / "doc/design/workout_game_asset_manifest.schema.json"
MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/FT-01-tabletop-greybox.json"
)
GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_Tabletop_Greybox.glb"
)
LOG_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/FT-02-log-over-greybox.json"
)
LOG_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_LogOver_Greybox.glb"
)
BUNNY_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/FT-03-bunny-hop-greybox.json"
)
BUNNY_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_BunnyHop_Greybox.glb"
)
DROP_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/FT-04-drop-greybox.json"
)
DROP_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_Drop_Greybox.glb"
)
GAP_JUMP_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/FT-12-gap-jump-three-line.json"
)
GAP_JUMP_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_GapJumpThreeLine.glb"
)
GAP_JUMP_AUDIT_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/audits/FT-12/FT-12-audit.json"
)
RIDER_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/RB-01-rider-bike.json"
)
RIDER_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_RiderBike.glb"
)
CONIFER_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/EN-01-conifer-set.json"
)
CONIFER_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_ConiferSet.glb"
)
FOREST_FLOOR_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/EN-08-forest-floor-props.json"
)
FOREST_FLOOR_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_ForestFloorProps.glb"
)
FOREST_FLOOR_AUDIT_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/audits/EN-08/EN-08-audit.json"
)
FOREST_VERGE_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/EN-09-forest-verge-clusters.json"
)
FOREST_VERGE_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_ForestVergeClusters.glb"
)
FOREST_VERGE_AUDIT_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/audits/EN-09/EN-09-audit.json"
)
DISTANT_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/EN-03-distant-ridges.json"
)
DISTANT_GLB_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generated/WG_DistantRidges.glb"
)
SURFACE_MANIFEST_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/manifests/TR-08-surface-atlas.json"
)
SURFACE_GENERATOR_PATH = (
    REPOSITORY
    / "contrib/workout-game-assets/generate_surface_atlas.py"
)
SURFACE_ATLAS_PATH = (
    REPOSITORY
    / "src/Resources/images/workout-game-surface-atlas.png"
)
SURFACE_TILE_NAMES = (
    "atlas", "forest", "dirt", "stone", "wood", "rider"
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def glb_accessor_values(path: Path, document: dict, accessor_index: int) -> list:
    """Decode the uncompressed scalar/vector accessors used by authored GLBs."""
    data = path.read_bytes()
    offset = 12
    binary = None
    while offset < len(data):
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk = data[offset:offset + chunk_length]
        offset += chunk_length
        if chunk_type == assets.GLB_BINARY_CHUNK:
            binary = chunk
    if binary is None:
        raise AssertionError("GLB has no binary payload")

    accessor = document["accessors"][accessor_index]
    view = document["bufferViews"][accessor["bufferView"]]
    component_formats = {
        5120: "b", 5121: "B", 5122: "h", 5123: "H",
        5125: "I", 5126: "f",
    }
    component_counts = {
        "SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4,
    }
    component_count = component_counts[accessor["type"]]
    value_format = "<" + (
        component_formats[accessor["componentType"]] * component_count
    )
    value_size = struct.calcsize(value_format)
    stride = view.get("byteStride", value_size)
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    return [
        struct.unpack_from(value_format, binary, start + index * stride)
        for index in range(accessor["count"])
    ]


class AssetFixture:
    def __init__(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="gc-workout-assets-")
        self.root = Path(self.temporary.name)
        self.manifest = assets.load_json_file(MANIFEST_PATH)
        paths = [
            "doc/design/workout_game_asset_manifest.schema.json",
            "COPYING",
            *[entry["path"] for entry in self.manifest["files"]],
        ]
        for relative in paths:
            source = REPOSITORY / relative
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        self.manifest_path = (
            self.root
            / "contrib/workout-game-assets/manifests/FT-01-tabletop-greybox.json"
        )
        self.manifest_path.parent.mkdir(parents=True, exist_ok=True)
        self.write_manifest()

    def write_manifest(self) -> None:
        self.manifest_path.write_text(
            json.dumps(self.manifest, indent=2) + "\n", encoding="utf-8"
        )

    def close(self) -> None:
        self.temporary.cleanup()


class TestWorkoutGameAssets(unittest.TestCase):
    def test_repository_assets_pass_all_gates(self) -> None:
        self.assertEqual(
            assets.validate_repository(REPOSITORY),
            [
                "EN-01-conifer-set",
                "EN-03-distant-ridges",
                "EN-08-forest-floor-props",
                "EN-09-forest-verge-clusters",
                "FT-01-tabletop-greybox",
                "FT-02-log-over-greybox",
                "FT-03-bunny-hop-greybox",
                "FT-04-drop-greybox",
                "FT-12-gap-jump-three-line",
                "RB-01-rider-bike",
                "TR-08-surface-atlas",
            ],
        )

    def test_schema_rejects_unknown_property(self) -> None:
        manifest = assets.load_json_file(MANIFEST_PATH)
        schema = assets.load_json_file(SCHEMA_PATH)
        manifest["unexpected"] = True
        with self.assertRaisesRegex(assets.AssetValidationError, "unknown property"):
            assets.validate_against_schema(manifest, schema)

    def test_schema_requires_complete_coordinate_vectors(self) -> None:
        manifest = assets.load_json_file(MANIFEST_PATH)
        schema = assets.load_json_file(SCHEMA_PATH)
        manifest["technical"]["boundsMeters"]["minimum"] = [0.0, 0.0]
        with self.assertRaisesRegex(assets.AssetValidationError, "too few items"):
            assets.validate_against_schema(manifest, schema)

    def test_hash_mismatch_is_rejected(self) -> None:
        fixture = AssetFixture()
        try:
            qml = fixture.root / "src/Train/qml/assets/Wg_Tabletop_Greybox.qml"
            qml.write_text(qml.read_text(encoding="utf-8") + "// changed\n")
            with self.assertRaisesRegex(assets.AssetValidationError, "hash mismatch"):
                assets.validate_repository(fixture.root)
        finally:
            fixture.close()

    def test_parent_traversal_path_is_rejected(self) -> None:
        fixture = AssetFixture()
        try:
            fixture.manifest["files"][0]["path"] = "../COPYING"
            fixture.write_manifest()
            with self.assertRaisesRegex(assets.AssetValidationError, "unsafe repository"):
                assets.validate_repository(fixture.root)
        finally:
            fixture.close()

    def test_unapproved_conditional_asset_is_rejected(self) -> None:
        fixture = AssetFixture()
        try:
            fixture.manifest["license"]["decision"] = "conditional"
            fixture.manifest["license"]["conditions"] = ["Provide attribution"]
            fixture.write_manifest()
            with self.assertRaisesRegex(assets.AssetValidationError, "not fully approved"):
                assets.validate_repository(fixture.root)
        finally:
            fixture.close()

    def test_runtime_qml_cannot_load_external_content(self) -> None:
        fixture = AssetFixture()
        try:
            qml = fixture.root / "src/Train/qml/assets/Wg_Tabletop_Greybox.qml"
            qml.write_text(
                qml.read_text(encoding="utf-8")
                + '\nRuntimeLoader { source: "https://example.invalid/model.glb" }\n',
                encoding="utf-8",
            )
            qml_hash = sha256(qml)
            for entry in fixture.manifest["files"]:
                if entry["path"].endswith("Wg_Tabletop_Greybox.qml"):
                    entry["sha256"] = qml_hash
            fixture.write_manifest()
            with self.assertRaisesRegex(
                assets.AssetValidationError, "dynamic or external"
            ):
                assets.validate_repository(fixture.root)
        finally:
            fixture.close()

    def test_glb_rejects_external_uri(self) -> None:
        document, size = assets.read_glb(GLB_PATH)
        manifest = assets.load_json_file(MANIFEST_PATH)
        document = copy.deepcopy(document)
        document["buffers"][0]["uri"] = "https://example.invalid/payload.bin"
        with self.assertRaisesRegex(assets.AssetValidationError, "must not contain"):
            assets.validate_glb_document(document, size, manifest)

    def test_glb_rejects_camera(self) -> None:
        document, size = assets.read_glb(GLB_PATH)
        manifest = assets.load_json_file(MANIFEST_PATH)
        document = copy.deepcopy(document)
        document["cameras"] = [{"type": "perspective"}]
        with self.assertRaisesRegex(assets.AssetValidationError, "camera or light"):
            assets.validate_glb_document(document, size, manifest)

    def test_glb_rejects_socket_position_change(self) -> None:
        document, size = assets.read_glb(GLB_PATH)
        manifest = assets.load_json_file(MANIFEST_PATH)
        document = copy.deepcopy(document)
        socket = next(node for node in document["nodes"] if node["name"] == "SOCKET_OUT")
        socket["translation"] = [0.2, 0.0, 6.34]
        with self.assertRaisesRegex(assets.AssetValidationError, "position mismatch"):
            assets.validate_glb_document(document, size, manifest)

    def test_glb_enforces_triangle_budget(self) -> None:
        document, size = assets.read_glb(GLB_PATH)
        manifest = assets.load_json_file(MANIFEST_PATH)
        manifest["technical"]["budgets"]["maxTrianglesLod0"] = (
            manifest["technical"]["trianglesLod0"] - 1
        )
        with self.assertRaisesRegex(assets.AssetValidationError, "triangle budget"):
            assets.validate_glb_document(document, size, manifest)

    def test_log_asset_does_not_duplicate_runtime_ground(self) -> None:
        document, size = assets.read_glb(LOG_GLB_PATH)
        manifest = assets.load_json_file(LOG_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_LogOverBark_Grey",
                "MAT_LogOverEndGrain_Grey",
            },
        )
        bounds = manifest["technical"]["boundsMeters"]
        self.assertLessEqual(bounds["maximum"][0] - bounds["minimum"][0], 2.24)

    def test_bunny_hop_is_a_compact_supported_hurdle(self) -> None:
        document, size = assets.read_glb(BUNNY_GLB_PATH)
        manifest = assets.load_json_file(BUNNY_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_BunnyHopBar_Grey",
                "MAT_BunnyHopSupport_Grey",
            },
        )
        bounds = manifest["technical"]["boundsMeters"]
        self.assertLessEqual(bounds["maximum"][1], 0.20)
        self.assertGreater(bounds["maximum"][0], 0.68 + 0.25)
        self.assertEqual(
            manifest["technical"]["sockets"][1]["positionMeters"],
            [0.0, 0.0, 3.58],
        )
        self.assertLessEqual(manifest["technical"]["materials"], 2)

    def test_drop_asset_is_only_a_narrow_face_below_the_lip(self) -> None:
        document, size = assets.read_glb(DROP_GLB_PATH)
        manifest = assets.load_json_file(DROP_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {"MAT_DropFace_Grey", "MAT_DropEdge_Grey"},
        )
        self.assertEqual(
            [node["name"] for node in document["nodes"] if "mesh" in node],
            ["GEO_DropFace_LOD0"],
        )
        bounds = manifest["technical"]["boundsMeters"]
        self.assertLessEqual(bounds["maximum"][1], 0.03)
        self.assertLessEqual(bounds["minimum"][1], -0.70)
        self.assertLessEqual(
            bounds["maximum"][2] - bounds["minimum"][2], 0.25
        )
        self.assertEqual(
            manifest["technical"]["sockets"][1]["positionMeters"],
            [0.0, 0.0, 24.0],
        )

    def test_gap_jump_has_three_open_speed_progressive_lines(self) -> None:
        document, size = assets.read_glb(GAP_JUMP_GLB_PATH)
        manifest = assets.load_json_file(GAP_JUMP_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        nodes = {node["name"]: node for node in document["nodes"]}
        root = nodes["ROOT_GapJumpThreeLine"]["extras"]
        self.assertEqual(
            [root[f"{line}_gap_length_m"] for line in ("short", "medium", "long")],
            [1.8, 3.2, 4.7],
        )
        self.assertEqual(
            [root[f"{line}_lateral_m"] for line in ("short", "medium", "long")],
            [-2.3, 0.0, 2.3],
        )
        self.assertEqual(root["physics_authority"], "external")
        self.assertEqual(root["tile_length_m"], 40.7)
        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_GapJumpPackedDirt",
                "MAT_GapJumpCutEarth",
                "MAT_GapJumpForestFloor",
            },
        )
        self.assertEqual(
            {node["name"] for node in document["nodes"] if "mesh" in node},
            {
                "GEO_GapJumpGround_LOD0",
                "GEO_GapJumpTread_LOD0",
                "GEO_GapJumpAccents_LOD0",
            },
        )
        for line in ("SHORT", "MEDIUM", "LONG"):
            lip = nodes[f"MARKER_{line}_LIP"]["translation"]
            land = nodes[f"MARKER_{line}_LAND"]["translation"]
            self.assertAlmostEqual(land[2] - lip[2], root[f"{line.lower()}_gap_length_m"], places=5)
            self.assertAlmostEqual(lip[0], land[0], places=5)

        tread = next(mesh for mesh in document["meshes"]
                     if mesh["name"] == "GEO_GapJumpTread_LOD0")
        for primitive in tread["primitives"]:
            positions = glb_accessor_values(
                GAP_JUMP_GLB_PATH, document, primitive["attributes"]["POSITION"]
            )
            indices = [value[0] for value in glb_accessor_values(
                GAP_JUMP_GLB_PATH, document, primitive["indices"]
            )]
            for offset in range(0, len(indices), 3):
                triangle = [positions[index] for index in indices[offset:offset + 3]]
                minimum_z = min(point[2] for point in triangle)
                maximum_z = max(point[2] for point in triangle)
                for gap_length in (1.8, 3.2, 4.7):
                    self.assertFalse(
                        minimum_z < 12.0 - 1.0e-5
                        and maximum_z > 12.0 + gap_length + 1.0e-5,
                        "a tread triangle bridges an authored open gap",
                    )
        self.assertEqual(
            manifest["technical"]["sockets"][1]["positionMeters"],
            [0.0, 0.0, 40.7],
        )
        ground = next(mesh for mesh in document["meshes"]
                      if mesh["name"] == "GEO_GapJumpGround_LOD0")
        ground_positions = []
        for primitive in ground["primitives"]:
            ground_positions.extend(glb_accessor_values(
                GAP_JUMP_GLB_PATH, document,
                primitive["attributes"]["POSITION"],
            ))
        for socket_z in (0.0, 40.7):
            socket_row = [point for point in ground_positions
                          if abs(point[2] - socket_z) < 1.0e-4]
            self.assertTrue(socket_row)
            self.assertLessEqual(
                max(abs(point[0]) for point in socket_row), 0.68 + 1.0e-5
            )
        self.assertLessEqual(manifest["technical"]["trianglesLod0"], 3600)
        self.assertLessEqual(manifest["technical"]["materials"], 3)

    def test_gap_jump_audit_is_content_anchored(self) -> None:
        audit = assets.load_json_file(GAP_JUMP_AUDIT_PATH)
        self.assertEqual(
            audit["format"], "goldencheetah-workout-game-asset-audit-1"
        )
        self.assertEqual(audit["assetId"], "FT-12-gap-jump-three-line")
        self.assertEqual(audit["sourceGlbSha256"], sha256(GAP_JUMP_GLB_PATH))
        self.assertEqual(audit["catalog"]["verticalFovDegrees"], 47.0)
        self.assertEqual(
            [render["view"] for render in audit["renders"]],
            ["chase", "overhead", "side-short", "side-medium", "side-long"],
        )
        render_hashes = []
        for render in audit["renders"]:
            path = GAP_JUMP_AUDIT_PATH.parent / render["path"]
            self.assertTrue(path.is_file())
            data = path.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", data[16:24]), (960, 540))
            self.assertEqual(render["sha256"], sha256(path))
            self.assertEqual(len(render["cameraPositionMeters"]), 3)
            self.assertEqual(len(render["cameraTargetMeters"]), 3)
            render_hashes.append(render["sha256"])
        self.assertEqual(len(set(render_hashes)), 5)

    def test_rider_bike_has_29er_dimensions_named_pivots_and_no_primitives(self) -> None:
        document, size = assets.read_glb(RIDER_GLB_PATH)
        manifest = assets.load_json_file(RIDER_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        nodes = {node["name"]: node for node in document["nodes"]}
        root_extras = nodes["ROOT_RiderBike"]["extras"]
        required_pivots = {
            "PIVOT_REAR_AXLE",
            "PIVOT_FRONT_AXLE",
            "PIVOT_CRANK",
            "PIVOT_STEER",
            "PIVOT_PELVIS",
            "PIVOT_CAMERA_TARGET",
            "PIVOT_SHADOW",
        }
        self.assertTrue(required_pivots.issubset(nodes))
        self.assertIn("GEO_HairBeard_LOD0", nodes)
        self.assertIn("GEO_Eyewear_LOD0", nodes)
        self.assertIn("GEO_HelmetAccent_LOD0", nodes)
        self.assertAlmostEqual(
            nodes["PIVOT_FRONT_AXLE"]["translation"][2]
            - nodes["PIVOT_REAR_AXLE"]["translation"][2],
            1.313,
            places=5,
        )
        self.assertAlmostEqual(
            nodes["PIVOT_CRANK"]["translation"][2]
            - nodes["PIVOT_REAR_AXLE"]["translation"][2],
            0.455,
            places=5,
        )
        self.assertAlmostEqual(
            nodes["PIVOT_REAR_AXLE"]["translation"][1], 0.3775, places=4
        )
        self.assertAlmostEqual(
            nodes["PIVOT_CRANK"]["translation"][1]
            - nodes["PIVOT_REAR_AXLE"]["translation"][1],
            0.0,
            places=5,
        )
        self.assertEqual(root_extras["reference_model"], "Pole Voima K2")
        self.assertEqual(
            root_extras["rider_reference"],
            "fictional project-authored rider; no specific likeness",
        )
        self.assertEqual(root_extras["front_tire"], "Maxxis Assegai DD 29x2.5")
        self.assertEqual(root_extras["rear_tire"], "Maxxis Minion DHR II DD 29x2.5")
        self.assertEqual(
            root_extras["helmet_reference"],
            "black-white open-face enduro helmet with visor",
        )
        self.assertAlmostEqual(root_extras["tire_width_m"], 0.0635, places=4)
        self.assertEqual(
            nodes["GEO_FrontWheel_LOD0"]["extras"]["tread_role"],
            "front-grip",
        )
        self.assertEqual(
            nodes["GEO_RearWheel_LOD0"]["extras"]["tread_role"],
            "rear-braking",
        )
        crank_extras = nodes["GEO_Crank_LOD0"]["extras"]
        self.assertEqual(crank_extras["left_pedal_contact_m"], [-0.13, 0.5375, 0.0])
        self.assertEqual(crank_extras["right_pedal_contact_m"], [0.13, 0.2175, 0.0])
        self.assertAlmostEqual(crank_extras["crank_length_m"], 0.16, places=5)
        self.assertGreaterEqual(crank_extras["pedal_platform_length_m"], 0.10)
        self.assertLessEqual(manifest["technical"]["trianglesLod0"], 3600)
        self.assertEqual(manifest["review"]["status"], "approved")
        self.assertEqual(manifest["review"]["trademarkStatus"], "clear")
        self.assertEqual(manifest["review"]["personReleaseStatus"], "not-applicable")
        self.assertEqual(manifest["review"]["propertyReleaseStatus"], "not-applicable")
        self.assertIn("No endorsement", manifest["review"]["notes"])
        self.assertNotIn("Leo Kokkonen", json.dumps(manifest))
        for mesh in document["meshes"]:
            for primitive in mesh["primitives"]:
                self.assertIn("TEXCOORD_0", primitive["attributes"])

        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameRiderBike.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        self.assertIn("workout-game-surface-rider.png", runtime_qml)
        self.assertGreaterEqual(
            runtime_qml.count("baseColorMap: riderPixelTexture"), 4
        )
        self.assertIn('baseColor: "#2f68b2"', runtime_qml)
        self.assertIn('baseColor: "#d7dad8"', runtime_qml)

    def test_conifer_set_has_varied_bounded_project_authored_silhouettes(self) -> None:
        document, size = assets.read_glb(CONIFER_GLB_PATH)
        manifest = assets.load_json_file(CONIFER_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        mesh_nodes = {
            node["name"] for node in document["nodes"] if "mesh" in node
        }
        self.assertEqual(
            mesh_nodes,
            {
                "GEO_ConiferTrunk_LOD0",
                "GEO_ConiferNarrow_LOD0",
                "GEO_ConiferLayered_LOD0",
                "GEO_ConiferBrokenTop_LOD0",
                "GEO_BirchTrunk_LOD0",
                "GEO_BirchCrown_LOD0",
                "GEO_ScotsPineTrunk_LOD0",
                "GEO_ScotsPineCrown_LOD0",
            },
        )
        self.assertLessEqual(manifest["technical"]["trianglesLod0"], 560)
        bounds = manifest["technical"]["boundsMeters"]
        self.assertGreaterEqual(bounds["maximum"][1], 5.4)
        self.assertGreaterEqual(bounds["minimum"][1], 0.0)
        nodes = {node["name"]: node for node in document["nodes"]}
        for name in ("GEO_ScotsPineTrunk_LOD0", "GEO_ScotsPineCrown_LOD0"):
            self.assertIn("Pinus sylvestris-inspired original silhouette",
                          nodes[name]["extras"]["species"])
        for name in ("GEO_BirchTrunk_LOD0", "GEO_BirchCrown_LOD0"):
            self.assertIn("Betula pendula-inspired original silhouette",
                          nodes[name]["extras"]["species"])
        triangle_counts = {}
        for name in mesh_nodes:
            mesh = document["meshes"][nodes[name]["mesh"]]
            triangle_counts[name] = sum(
                document["accessors"][primitive["indices"]]["count"] // 3
                for primitive in mesh["primitives"]
            )
        self.assertLessEqual(
            triangle_counts["GEO_ConiferTrunk_LOD0"]
            + max(triangle_counts["GEO_ConiferNarrow_LOD0"],
                  triangle_counts["GEO_ConiferLayered_LOD0"],
                  triangle_counts["GEO_ConiferBrokenTop_LOD0"]),
            136,
        )
        self.assertLessEqual(
            triangle_counts["GEO_ScotsPineTrunk_LOD0"]
            + triangle_counts["GEO_ScotsPineCrown_LOD0"],
            136,
        )
        self.assertLessEqual(
            triangle_counts["GEO_BirchTrunk_LOD0"]
            + triangle_counts["GEO_BirchCrown_LOD0"],
            136,
        )
        for name in mesh_nodes:
            mesh = document["meshes"][nodes[name]["mesh"]]
            minimum_y = min(
                document["accessors"][primitive["attributes"]["POSITION"]]["min"][1]
                for primitive in mesh["primitives"]
            )
            self.assertAlmostEqual(minimum_y, 0.0, places=6, msg=name)
        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameConifer.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        self.assertIn("variant === 3", runtime_qml)
        self.assertIn("variant === 2", runtime_qml)
        self.assertIn("geo_BirchTrunk_LOD0_mesh.mesh", runtime_qml)
        self.assertIn("geo_BirchCrown_LOD0_mesh.mesh", runtime_qml)
        self.assertNotIn("geo_ConiferBrokenTop_LOD0_mesh.mesh", runtime_qml)
        self.assertIn("geo_ScotsPineTrunk_LOD0_mesh.mesh", runtime_qml)
        self.assertIn("geo_ScotsPineCrown_LOD0_mesh.mesh", runtime_qml)
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        self.assertIn("geo_BirchTrunk_LOD0_mesh.mesh", qrc)
        self.assertIn("geo_BirchCrown_LOD0_mesh.mesh", qrc)
        self.assertNotIn("geo_ConiferBrokenTop_LOD0_mesh.mesh", qrc)
        self.assertIn("geo_ScotsPineTrunk_LOD0_mesh.mesh", qrc)
        self.assertIn("geo_ScotsPineCrown_LOD0_mesh.mesh", qrc)

    def test_mixed_forest_generation_has_fixed_runtime_inventory(self) -> None:
        document, _ = assets.read_glb(CONIFER_GLB_PATH)
        manifest = assets.load_json_file(CONIFER_MANIFEST_PATH)
        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_BirchBark",
                "MAT_BirchLeaf",
                "MAT_ConiferBark",
                "MAT_ConiferDark",
                "MAT_ConiferLight",
                "MAT_ScotsPineBark",
            },
        )
        self.assertEqual(manifest["source"]["kind"], "project-authored")
        self.assertEqual(manifest["technical"]["trianglesLod0"], 536)
        generator = (
            REPOSITORY
            / "contrib/workout-game-assets/blender/generate_conifer_set.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("import random", generator)
        self.assertNotIn("import time", generator)
        self.assertIn("RUNTIME_VARIANT_PAIRS", generator)
        self.assertIn("does not touch its terrain anchor", generator)

    def test_forest_floor_props_are_grounded_atlas_ready_and_runtime_approved(self) -> None:
        document, size = assets.read_glb(FOREST_FLOOR_GLB_PATH)
        manifest = assets.load_json_file(FOREST_FLOOR_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        expected_triangles = {
            "GEO_GraniteLow_LOD0": 32,
            "GEO_GraniteUpright_LOD0": 32,
            "GEO_GraniteSlab_LOD0": 32,
            "GEO_StumpRooted_LOD0": 50,
            "GEO_DeadwoodFallen_LOD0": 72,
            "GEO_UnderstoryFern_LOD0": 20,
            "GEO_UnderstoryBilberry_LOD0": 40,
            "GEO_UnderstoryHeather_LOD0": 42,
        }
        nodes = {node["name"]: node for node in document["nodes"]}
        mesh_nodes = {
            name: node for name, node in nodes.items() if "mesh" in node
        }
        self.assertEqual(set(mesh_nodes), set(expected_triangles))
        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_ForestGranite",
                "MAT_ForestBark",
                "MAT_ForestEndGrain",
                "MAT_ForestUnderstory",
            },
        )

        observed_total = 0
        for name, expected in expected_triangles.items():
            node = nodes[name]
            mesh = document["meshes"][node["mesh"]]
            count = sum(
                document["accessors"][primitive["indices"]]["count"] // 3
                for primitive in mesh["primitives"]
            )
            observed_total += count
            self.assertEqual(count, expected)
            self.assertLessEqual(count, 96)
            self.assertEqual(node["extras"]["instance_ready"], True)
            self.assertEqual(node["extras"]["ground_contact_y_m"], 0.0)
            minimum_y = float("inf")
            maximum_y = float("-inf")
            for primitive in mesh["primitives"]:
                self.assertIn("TEXCOORD_0", primitive["attributes"])
                position = document["accessors"][
                    primitive["attributes"]["POSITION"]
                ]
                minimum_y = min(minimum_y, position["min"][1])
                maximum_y = max(maximum_y, position["max"][1])
            self.assertEqual(minimum_y, 0.0)
            self.assertLessEqual(maximum_y, 0.70 + 1.0e-6)
            expected_passes = 2 if name in {
                "GEO_StumpRooted_LOD0", "GEO_DeadwoodFallen_LOD0"
            } else 1
            self.assertEqual(len(mesh["primitives"]), expected_passes)
            pivot = name.replace("GEO_", "PIVOT_").replace("_LOD0", "_BASE")
            self.assertIn(pivot, nodes)
            self.assertEqual(nodes[pivot].get("translation", [0, 0, 0]), [0, 0, 0])

        self.assertEqual(observed_total, 320)
        deadwood = nodes["GEO_DeadwoodFallen_LOD0"]["extras"]
        self.assertEqual(deadwood["placement_role"], "scenery-only")
        self.assertEqual(deadwood["collision_role"], "none")
        self.assertEqual(deadwood["feature_role"], "none")
        self.assertLessEqual(
            manifest["technical"]["boundsMeters"]["maximum"][1], 0.70 + 1.0e-6
        )
        self.assertEqual(manifest["review"]["status"], "approved")
        self.assertEqual(manifest["license"]["spdxId"], "CC0-1.0")
        self.assertIn("appimage", manifest["license"]["distributionScopes"])
        runtime_paths = {
            entry["path"] for entry in manifest["files"]
            if entry["purpose"] == "runtime"
        }
        expected_meshes = {
            "src/Train/qml/assets/meshes/"
            f"geo_{name[4:]}_mesh.mesh"
            for name in expected_triangles
        }
        self.assertEqual(
            runtime_paths,
            {"src/Train/qml/WorkoutGameForestFloorProp.qml"}
            | expected_meshes,
        )
        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameForestFloorProp.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        for path in expected_meshes:
            self.assertIn(Path(path).name, runtime_qml)
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        self.assertIn("WorkoutGameForestFloorProp.qml", qrc)
        for path in expected_meshes:
            self.assertIn(Path(path).name, qrc)

    def test_forest_floor_deadwood_is_an_irregular_branched_log(self) -> None:
        document, _ = assets.read_glb(FOREST_FLOOR_GLB_PATH)
        nodes = {node["name"]: node for node in document["nodes"]}
        node = nodes["GEO_DeadwoodFallen_LOD0"]
        extras = node["extras"]
        self.assertEqual(extras["cross_section_sides"], 7)
        self.assertEqual(extras["branch_stub_count"], 2)
        self.assertEqual(extras["placement_role"], "scenery-only")
        self.assertEqual(extras["collision_role"], "none")
        self.assertEqual(extras["feature_role"], "none")

        mesh = document["meshes"][node["mesh"]]
        primitive_triangles = [
            document["accessors"][primitive["indices"]]["count"] // 3
            for primitive in mesh["primitives"]
        ]
        # A seven-sided four-ring trunk contributes 42 bark triangles. Two
        # tapered triangular branch stubs add 14 bark triangles, while their
        # broken tips join the 14 trunk-end triangles in the end-grain pass.
        self.assertEqual(primitive_triangles, [56, 16])

        positions = glb_accessor_values(
            FOREST_FLOOR_GLB_PATH,
            document,
            mesh["primitives"][0]["attributes"]["POSITION"],
        )
        unique_positions = {tuple(round(value, 5) for value in p) for p in positions}
        self.assertEqual(len(unique_positions), 40)
        self.assertGreaterEqual(len({p[0] for p in unique_positions}), 12)
        self.assertGreater(max(p[2] for p in unique_positions)
                           - min(p[2] for p in unique_positions), 0.55)

        end_positions = glb_accessor_values(
            FOREST_FLOOR_GLB_PATH,
            document,
            mesh["primitives"][1]["attributes"]["POSITION"],
        )
        unique_ends = {tuple(round(value, 5) for value in p) for p in end_positions}
        self.assertEqual(len(unique_ends), 22)
        left = sorted(unique_ends)[:8]
        right = sorted(unique_ends)[-8:]
        self.assertGreater(max(p[0] for p in left) - min(p[0] for p in left), 0.04)
        self.assertGreater(max(p[0] for p in right) - min(p[0] for p in right), 0.04)

    def test_forest_floor_granite_is_cool_mid_grey(self) -> None:
        document, _ = assets.read_glb(FOREST_FLOOR_GLB_PATH)
        material = next(
            item for item in document["materials"]
            if item["name"] == "MAT_ForestGranite"
        )
        red, green, blue, alpha = material[
            "pbrMetallicRoughness"
        ]["baseColorFactor"]
        luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue
        self.assertGreaterEqual(luminance, 0.10)
        self.assertLessEqual(luminance, 0.18)
        self.assertGreaterEqual(blue - red, 0.02)
        self.assertLessEqual(max(red, green, blue), 0.18)
        self.assertEqual(alpha, 1.0)

    def test_forest_floor_audits_use_fixed_camera_scale_and_distinct_angles(self) -> None:
        audit = assets.load_json_file(FOREST_FLOOR_AUDIT_PATH)
        catalog = audit["catalog"]
        self.assertEqual((catalog["widthPixels"], catalog["heightPixels"]),
                         (960, 540))
        self.assertEqual(catalog["verticalFovDegrees"], 47.0)
        self.assertEqual(catalog["cameraDistanceMeters"], 3.0)
        self.assertEqual(catalog["trailWidthScaleBarMeters"], 1.36)
        self.assertEqual((catalog["cellColumns"], catalog["cellRows"]), (4, 2))
        self.assertEqual(len(catalog["cellOrder"]), 8)
        self.assertEqual(audit["sourceGlbSha256"], sha256(FOREST_FLOOR_GLB_PATH))

        render_hashes = []
        for render in audit["renders"]:
            path = FOREST_FLOOR_AUDIT_PATH.parent / render["path"]
            data = path.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", data[16:24]), (960, 540))
            self.assertEqual(render["sha256"], sha256(path))
            render_hashes.append(render["sha256"])
        self.assertEqual(
            [render["assetRotationDegrees"] for render in audit["renders"]],
            [0.0, 45.0, 135.0],
        )
        self.assertEqual(len(set(render_hashes)), 3)

    def test_forest_verge_clusters_are_grounded_separated_and_runtime_approved(self) -> None:
        document, size = assets.read_glb(FOREST_VERGE_GLB_PATH)
        manifest = assets.load_json_file(FOREST_VERGE_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        expected_triangles = {
            "GEO_VergeGraniteBilberry_LOD0": 104,
            "GEO_VergeStumpFern_LOD0": 144,
            "GEO_VergeDeadwoodHeather_LOD0": 134,
        }
        nodes = {node["name"]: node for node in document["nodes"]}
        mesh_nodes = {
            name: node for name, node in nodes.items() if "mesh" in node
        }
        self.assertEqual(set(mesh_nodes), set(expected_triangles))
        self.assertEqual(
            {material["name"] for material in document["materials"]},
            {
                "MAT_ForestGranite",
                "MAT_ForestBark",
                "MAT_ForestEndGrain",
                "MAT_ForestUnderstory",
            },
        )

        observed_total = 0
        for name, expected in expected_triangles.items():
            node = nodes[name]
            extras = node["extras"]
            mesh = document["meshes"][node["mesh"]]
            triangles = sum(
                document["accessors"][primitive["indices"]]["count"] // 3
                for primitive in mesh["primitives"]
            )
            observed_total += triangles
            self.assertEqual(triangles, expected)
            self.assertLessEqual(triangles, 150)
            self.assertEqual(extras["placement_role"], "scenery-only")
            self.assertEqual(extras["physics_authority"], "external")
            self.assertEqual(extras["collision_role"], "none")
            self.assertEqual(extras["instance_ready"], True)
            self.assertEqual(extras["ground_contact_y_m"], 0.0)
            self.assertEqual(extras["trail_edge_clearance_m"], 0.14)
            self.assertGreaterEqual(extras["component_count"], 3)

            minimum_y = float("inf")
            maximum_y = float("-inf")
            minimum_x = float("inf")
            for primitive in mesh["primitives"]:
                self.assertIn("TEXCOORD_0", primitive["attributes"])
                positions = document["accessors"][
                    primitive["attributes"]["POSITION"]
                ]
                minimum_x = min(minimum_x, positions["min"][0])
                minimum_y = min(minimum_y, positions["min"][1])
                maximum_y = max(maximum_y, positions["max"][1])
                for uv in glb_accessor_values(
                    FOREST_VERGE_GLB_PATH,
                    document,
                    primitive["attributes"]["TEXCOORD_0"],
                ):
                    self.assertTrue(all(-1.0e-6 <= value <= 1.0 + 1.0e-6
                                        for value in uv))
            self.assertEqual(minimum_y, 0.0)
            self.assertLessEqual(maximum_y, 0.70 + 1.0e-6)
            self.assertGreaterEqual(minimum_x, 0.14 - 1.0e-6)
            pivot = name.replace("GEO_", "PIVOT_").replace(
                "_LOD0", "_TRAIL_EDGE"
            )
            self.assertEqual(nodes[pivot].get("translation", [0, 0, 0]),
                             [0, 0, 0])

        self.assertEqual(observed_total, 382)
        root = nodes["ROOT_ForestVergeClusters"]["extras"]
        self.assertEqual(root["project_generated"], True)
        self.assertEqual(root["generated_output_license"], "CC0-1.0")
        self.assertEqual(manifest["review"]["status"], "approved")
        self.assertEqual(manifest["license"]["spdxId"], "CC0-1.0")
        self.assertIn("appimage", manifest["license"]["distributionScopes"])
        runtime_paths = {
            entry["path"] for entry in manifest["files"]
            if entry["purpose"] == "runtime"
        }
        expected_meshes = {
            "src/Train/qml/assets/meshes/"
            f"geo_{name[4:]}_mesh.mesh"
            for name in expected_triangles
        }
        self.assertEqual(
            runtime_paths,
            {"src/Train/qml/WorkoutGameForestVergeCluster.qml"}
            | expected_meshes,
        )
        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameForestVergeCluster.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        for path in expected_meshes:
            self.assertIn(Path(path).name, runtime_qml)
        production_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGame3D.qml"
        ).read_text(encoding="utf-8")
        self.assertIn("WorkoutGameForestFloorProp", production_qml)
        self.assertIn("WorkoutGameForestVergeCluster", production_qml)
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        self.assertIn("WorkoutGameForestVergeCluster.qml", qrc)
        for path in expected_meshes:
            self.assertIn(Path(path).name, qrc)

    def test_forest_verge_audits_are_matched_before_after_catalogs(self) -> None:
        audit = assets.load_json_file(FOREST_VERGE_AUDIT_PATH)
        catalog = audit["catalog"]
        self.assertEqual((catalog["widthPixels"], catalog["heightPixels"]),
                         (960, 540))
        self.assertEqual((catalog["cellColumns"], catalog["cellRows"]), (3, 2))
        self.assertEqual(
            catalog["rows"],
            ["before-isolated-prop", "after-verge-cluster"],
        )
        self.assertEqual(catalog["verticalFovDegrees"], 47.0)
        self.assertEqual(catalog["cameraDistanceMeters"], 4.45)
        self.assertEqual(catalog["trailWidthScaleBarMeters"], 1.36)
        self.assertEqual(
            audit["clusterGlbSha256"], sha256(FOREST_VERGE_GLB_PATH)
        )
        self.assertEqual(
            audit["sourcePropGlbSha256"], sha256(FOREST_FLOOR_GLB_PATH)
        )

        render_hashes = []
        for render in audit["renders"]:
            path = FOREST_VERGE_AUDIT_PATH.parent / render["path"]
            data = path.read_bytes()
            self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack(">II", data[16:24]), (960, 540))
            self.assertEqual(render["sha256"], sha256(path))
            render_hashes.append(render["sha256"])
        self.assertEqual(len(render_hashes), 3)
        self.assertEqual(len(set(render_hashes)), 3)

    def test_forest_verge_generator_is_explicit_and_dependency_is_manifested(self) -> None:
        generator = (
            REPOSITORY
            / "contrib/workout-game-assets/blender/generate_forest_verge_clusters.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("import random", generator)
        manifest = assets.load_json_file(FOREST_VERGE_MANIFEST_PATH)
        paths = {entry["path"] for entry in manifest["files"]}
        self.assertIn(
            "contrib/workout-game-assets/blender/generate_forest_floor_props.py",
            paths,
        )

    def test_forest_dressing_runtime_stays_within_resident_asset_budget(self) -> None:
        manifests = (
            assets.load_json_file(FOREST_FLOOR_MANIFEST_PATH),
            assets.load_json_file(FOREST_VERGE_MANIFEST_PATH),
        )
        runtime_paths = [
            entry["path"]
            for manifest in manifests
            for entry in manifest["files"]
            if entry["purpose"] == "runtime"
        ]
        self.assertEqual(len(runtime_paths), 13)
        self.assertLessEqual(
            sum((REPOSITORY / path).stat().st_size for path in runtime_paths),
            80 * 1024,
        )
        # The production resident window selects four floor props and three
        # verge clusters. Keep its worst-case authored mesh cost explicit.
        self.assertLessEqual(4 * 96 + 3 * 150, 850)

    def test_distant_ridges_are_bounded_socket_free_scenery(self) -> None:
        document, size = assets.read_glb(DISTANT_GLB_PATH)
        manifest = assets.load_json_file(DISTANT_MANIFEST_PATH)
        assets.validate_glb_document(document, size, manifest)

        nodes = {node["name"]: node for node in document["nodes"]}
        self.assertIn("ROOT_DistantRidges", nodes)
        self.assertIn("PIVOT_CENTER", nodes)
        self.assertEqual(
            [node["name"] for node in document["nodes"] if "mesh" in node],
            ["GEO_DistantRidges_LOD0"],
        )
        self.assertLessEqual(manifest["technical"]["trianglesLod0"], 300)
        bounds = manifest["technical"]["boundsMeters"]
        self.assertGreaterEqual(bounds["maximum"][0], 230.0)
        self.assertLessEqual(bounds["minimum"][0], -230.0)

        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameDistantTerrain.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        self.assertIn("WorkoutGameDistantTerrain.qml", qrc)
        self.assertIn("geo_DistantRidges_LOD0_mesh.mesh", qrc)

    def test_gap_jump_runtime_policy_packages_one_authored_surface(self) -> None:
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        runtime = (
            REPOSITORY / "src/Train/qml/WorkoutGame3D.qml"
        ).read_text(encoding="utf-8")
        for name in (
            "Wg_GapJumpThreeLine.qml",
            "geo_GapJumpAccents_LOD0_mesh.mesh",
            "geo_GapJumpGround_LOD0_mesh.mesh",
            "geo_GapJumpTread_LOD0_mesh.mesh",
        ):
            prefix = "qml/assets/" if name.endswith(".qml") \
                else "qml/assets/meshes/"
            self.assertEqual(qrc.count(f'alias="{prefix}{name}"'), 1,
                             msg=name)
        self.assertEqual(runtime.count("Wg_GapJumpThreeLine"), 1)
        self.assertNotIn("gapJumpGeometryModel", runtime)

    def test_surface_atlas_is_deterministic_bounded_and_packaged(self) -> None:
        manifest = assets.load_json_file(SURFACE_MANIFEST_PATH)
        atlas = SURFACE_ATLAS_PATH.read_bytes()
        self.assertEqual(atlas[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack(">II", atlas[16:24]), (96, 64))
        self.assertEqual(atlas[24:29], bytes((8, 6, 0, 0, 0)))
        runtime_texture_bytes = sum(
            (
                REPOSITORY
                / f"src/Resources/images/workout-game-surface-{name}.png"
            ).stat().st_size
            for name in SURFACE_TILE_NAMES
        )
        self.assertEqual(
            manifest["technical"]["textureBytes"], runtime_texture_bytes
        )
        self.assertLessEqual(runtime_texture_bytes, 16 * 1024)

        with tempfile.TemporaryDirectory(
            prefix="gc-surface-atlas-"
        ) as temporary:
            subprocess.run(
                [
                    sys.executable,
                    str(SURFACE_GENERATOR_PATH),
                    "--output-dir",
                    temporary,
                ],
                check=True,
                stdout=subprocess.DEVNULL,
            )
            generated = Path(temporary)
            for name in SURFACE_TILE_NAMES:
                expected = (
                    REPOSITORY
                    / f"src/Resources/images/workout-game-surface-{name}.png"
                )
                self.assertEqual(
                    (generated / expected.name).read_bytes(), expected.read_bytes()
                )

        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        qml = (
            REPOSITORY / "src/Train/qml/WorkoutGame3D.qml"
        ).read_text(encoding="utf-8")
        for name in SURFACE_TILE_NAMES:
            filename = f"workout-game-surface-{name}.png"
            self.assertIn(filename, qrc)
            if name not in ("atlas", "rider"):
                self.assertIn(filename, qml)
        self.assertEqual(qml.count("minFilter: Texture.Linear"), 4)
        self.assertEqual(qml.count("magFilter: Texture.Nearest"), 4)
        self.assertEqual(qml.count("generateMipmaps: true"), 4)

    def test_malformed_glb_structure_fails_cleanly(self) -> None:
        document, size = assets.read_glb(GLB_PATH)
        manifest = assets.load_json_file(MANIFEST_PATH)
        document["nodes"] = "not-a-node-list"
        with self.assertRaisesRegex(assets.AssetValidationError, "GLB nodes"):
            assets.validate_glb_document(document, size, manifest)

    def test_truncated_glb_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="gc-bad-glb-") as temporary:
            path = Path(temporary) / "bad.glb"
            path.write_bytes(GLB_PATH.read_bytes()[:11])
            with self.assertRaisesRegex(assets.AssetValidationError, "truncated GLB"):
                assets.read_glb(path)


if __name__ == "__main__":
    unittest.main()
