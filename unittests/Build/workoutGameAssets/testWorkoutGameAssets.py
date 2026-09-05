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
                "FT-01-tabletop-greybox",
                "FT-02-log-over-greybox",
                "FT-03-bunny-hop-greybox",
                "FT-04-drop-greybox",
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
                "GEO_ScotsPineTrunk_LOD0",
                "GEO_ScotsPineCrown_LOD0",
            },
        )
        self.assertLessEqual(manifest["technical"]["trianglesLod0"], 420)
        bounds = manifest["technical"]["boundsMeters"]
        self.assertGreaterEqual(bounds["maximum"][1], 5.4)
        self.assertGreaterEqual(bounds["minimum"][1], 0.0)
        nodes = {node["name"]: node for node in document["nodes"]}
        for name in ("GEO_ScotsPineTrunk_LOD0", "GEO_ScotsPineCrown_LOD0"):
            self.assertIn("Pinus sylvestris-inspired original silhouette",
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
        runtime_qml = (
            REPOSITORY / "src/Train/qml/WorkoutGameConifer.qml"
        ).read_text(encoding="utf-8")
        for primitive in ("#Cube", "#Cylinder", "#Cone", "#Sphere"):
            self.assertNotIn(primitive, runtime_qml)
        self.assertIn("variant === 3", runtime_qml)
        self.assertIn("geo_ScotsPineTrunk_LOD0_mesh.mesh", runtime_qml)
        self.assertIn("geo_ScotsPineCrown_LOD0_mesh.mesh", runtime_qml)
        qrc = (
            REPOSITORY / "src/Resources/workout-game-assets.qrc"
        ).read_text(encoding="utf-8")
        self.assertIn("geo_ScotsPineTrunk_LOD0_mesh.mesh", qrc)
        self.assertIn("geo_ScotsPineCrown_LOD0_mesh.mesh", qrc)

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
