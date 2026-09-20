#!/usr/bin/env python3

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "contrib/workout-game-assets"
sys.path.insert(0, str(TOOLS))

from editor.asset_document import (  # noqa: E402
    AssetDocument,
    AssetDocumentConflict,
    AssetDocumentError,
    _manifest_lock,
)


class WorkoutGameAssetDocumentTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.repository = Path(self.temporary.name) / "repository"
        manifests = self.repository / "contrib/workout-game-assets/manifests"
        generated = self.repository / "contrib/workout-game-assets/generated"
        schema_directory = self.repository / "doc/design"
        manifests.mkdir(parents=True)
        generated.mkdir(parents=True)
        schema_directory.mkdir(parents=True)

        self.manifest = manifests / "FT-01-tabletop-greybox.json"
        source_manifest = TOOLS / "manifests/FT-01-tabletop-greybox.json"
        manifest_document = json.loads(source_manifest.read_text(encoding="utf-8"))
        shutil.copyfile(source_manifest, self.manifest)
        for entry in manifest_document["files"]:
            source = REPOSITORY / entry["path"]
            destination = self.repository / entry["path"]
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
        shutil.copyfile(REPOSITORY / "COPYING", self.repository / "COPYING")
        shutil.copyfile(
            REPOSITORY / "doc/design/workout_game_asset_manifest.schema.json",
            schema_directory / "workout_game_asset_manifest.schema.json",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_defaults_are_explicit_and_do_not_modify_manifest(self) -> None:
        before = self.manifest.read_bytes()
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )

        self.assertEqual(
            document.material_names,
            (
                "MAT_TabletopTrail_Grey",
                "MAT_TabletopTerrain_Grey",
                "MAT_TabletopBypass_Grey",
            ),
        )
        self.assertEqual(document.material_overrides, {})
        self.assertNotEqual(
            document.material_defaults["MAT_TabletopTrail_Grey"]["baseColorSrgb"],
            "#ffffff",
        )
        self.assertEqual(document.physics.authority, "external")
        self.assertEqual(document.physics.interaction, "rideable-feature")
        self.assertEqual(document.physics.coulomb_friction, 1.0)
        self.assertEqual(document.physics.restitution, 0.0)
        self.assertEqual(document.physics.collision_node, "")
        self.assertEqual(self.manifest.read_bytes(), before)

    def test_color_and_physics_round_trip_is_deterministic(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        document.set_material(
            "MAT_TabletopTrail_Grey",
            base_color_srgb="#3a7bc2",
            roughness=0.85,
            metallic=0.05,
        )
        document.set_physics(
            interaction="rideable-feature",
            coulomb_friction=1.25,
            restitution=0.1,
            collision_node="ROOT_Tabletop",
        )
        self.assertTrue(document.save())
        first = self.manifest.read_bytes()

        reopened = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        self.assertEqual(
            reopened.material_overrides,
            {
                "MAT_TabletopTrail_Grey": {
                    "baseColorSrgb": "#3a7bc2",
                    "roughness": 0.85,
                    "metallic": 0.05,
                }
            },
        )
        self.assertEqual(reopened.physics.authority, "external")
        self.assertEqual(reopened.physics.interaction, "rideable-feature")
        self.assertAlmostEqual(reopened.physics.coulomb_friction, 1.25)
        self.assertAlmostEqual(reopened.physics.restitution, 0.1)
        self.assertEqual(reopened.physics.collision_node, "ROOT_Tabletop")
        self.assertEqual(reopened.review_status, "candidate")

        self.assertFalse(reopened.save())
        self.assertEqual(self.manifest.read_bytes(), first)
        self.assertTrue(first.endswith(b"\n"))

    def test_no_op_save_preserves_review(self) -> None:
        before = self.manifest.read_bytes()
        original_status = json.loads(before)["review"]["status"]
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )

        self.assertFalse(document.save())
        self.assertEqual(self.manifest.read_bytes(), before)
        self.assertEqual(document.review_status, original_status)

    def test_partial_material_edit_preserves_glb_defaults(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        expected = document.material_defaults["MAT_TabletopTrail_Grey"]

        document.set_material("MAT_TabletopTrail_Grey", roughness=0.42)
        self.assertTrue(document.save())
        reopened = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        override = reopened.material_overrides["MAT_TabletopTrail_Grey"]
        self.assertEqual(override["baseColorSrgb"], expected["baseColorSrgb"])
        self.assertEqual(override["metallic"], expected["metallic"])
        self.assertAlmostEqual(override["roughness"], 0.42)

    def test_unknown_material_and_invalid_values_are_rejected(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )

        with self.assertRaisesRegex(AssetDocumentError, "unknown material"):
            document.set_material("MAT_NotInGlb", base_color_srgb="#ffffff")
        with self.assertRaisesRegex(AssetDocumentError, "sRGB"):
            document.set_material("MAT_TabletopTrail_Grey", base_color_srgb="red")
        with self.assertRaisesRegex(AssetDocumentError, "roughness"):
            document.set_material("MAT_TabletopTrail_Grey", roughness=1.01)
        with self.assertRaisesRegex(AssetDocumentError, "friction"):
            document.set_physics(coulomb_friction=2.01)
        with self.assertRaisesRegex(AssetDocumentError, "restitution"):
            document.set_physics(restitution=float("nan"))
        with self.assertRaisesRegex(AssetDocumentError, "interaction"):
            document.set_physics(interaction="magic")
        with self.assertRaisesRegex(AssetDocumentError, "collision node"):
            document.set_physics(collision_node="MISSING_NODE")

    def test_external_change_is_not_overwritten(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        document.set_material("MAT_TabletopTrail_Grey", base_color_srgb="#010203")
        changed = json.loads(self.manifest.read_text(encoding="utf-8"))
        changed["displayName"] = "Concurrent edit"
        self.manifest.write_text(
            json.dumps(changed, indent=2) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(AssetDocumentConflict, "changed on disk"):
            document.save()
        self.assertIn("Concurrent edit", self.manifest.read_text(encoding="utf-8"))

    def test_glb_change_after_open_is_not_accepted_by_save(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        document.set_material("MAT_TabletopTrail_Grey", roughness=0.42)
        glb = self.repository / (
            "contrib/workout-game-assets/generated/WG_Tabletop_Greybox.glb"
        )
        glb.write_bytes(glb.read_bytes() + b"changed")

        with self.assertRaisesRegex(AssetDocumentConflict, "GLB changed on disk"):
            document.save()

    def test_repository_and_manifest_ancestor_symlinks_are_rejected(self) -> None:
        alias_parent = Path(self.temporary.name) / "alias-parent"
        alias_parent.symlink_to(self.repository.parent, target_is_directory=True)
        with self.assertRaisesRegex(AssetDocumentError, "symlink"):
            AssetDocument.open(
                alias_parent / self.repository.name,
                "FT-01-tabletop-greybox",
            )

        assets = self.repository / "contrib/workout-game-assets"
        real_assets = assets.with_name("workout-game-assets-real")
        assets.rename(real_assets)
        assets.symlink_to(real_assets.name, target_is_directory=True)
        with self.assertRaisesRegex(AssetDocumentError, "manifest directory"):
            AssetDocument.open(
                self.repository,
                "FT-01-tabletop-greybox",
            )

    def test_failed_atomic_replace_keeps_original_and_removes_temporary_file(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        document.set_material("MAT_TabletopTrail_Grey", base_color_srgb="#010203")
        before = self.manifest.read_bytes()

        with mock.patch(
            "editor.asset_document.os.replace",
            side_effect=OSError("injected replace failure"),
        ):
            with self.assertRaisesRegex(OSError, "injected replace failure"):
                document.save()

        self.assertEqual(self.manifest.read_bytes(), before)
        leftovers = list(self.manifest.parent.glob(".*.tmp"))
        self.assertEqual(leftovers, [])

    def test_manifest_and_glb_symlinks_are_rejected(self) -> None:
        real_manifest = self.manifest.with_suffix(".real.json")
        self.manifest.rename(real_manifest)
        self.manifest.symlink_to(real_manifest.name)
        with self.assertRaisesRegex(AssetDocumentError, "symlink"):
            AssetDocument.open(self.repository, "FT-01-tabletop-greybox")

        self.manifest.unlink()
        real_manifest.rename(self.manifest)
        glb = self.repository / "contrib/workout-game-assets/generated/WG_Tabletop_Greybox.glb"
        real_glb = glb.with_suffix(".real.glb")
        glb.rename(real_glb)
        glb.symlink_to(real_glb.name)
        with self.assertRaisesRegex(AssetDocumentError, "symlink"):
            AssetDocument.open(self.repository, "FT-01-tabletop-greybox")

    def test_asset_id_cannot_be_used_as_a_path(self) -> None:
        with self.assertRaisesRegex(AssetDocumentError, "asset id"):
            AssetDocument.open(self.repository, "../../outside")

    def test_inconsistent_physics_document_is_rejected_on_open(self) -> None:
        manifest = json.loads(self.manifest.read_text(encoding="utf-8"))
        manifest["physics"] = {
            "authority": "external",
            "interaction": "visual-only",
            "surface": {
                "coulombFriction": 1.0,
                "restitution": 0.0,
            },
            "collisionProxy": {"kind": "none", "node": "ROOT_Tabletop"},
        }
        self.manifest.write_text(
            json.dumps(manifest, indent=2) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(AssetDocumentError, "visual-only"):
            AssetDocument.open(self.repository, "FT-01-tabletop-greybox")

    def test_visual_only_interaction_clears_collision_proxy(self) -> None:
        document = AssetDocument.open(
            self.repository,
            "FT-01-tabletop-greybox",
        )
        document.set_physics(collision_node="ROOT_Tabletop")
        document.set_physics(interaction="visual-only")

        self.assertEqual(document.physics.collision_node, "")
        self.assertTrue(document.save())
        saved = json.loads(self.manifest.read_text(encoding="utf-8"))
        self.assertNotIn("surface", saved["physics"])
        self.assertEqual(saved["physics"]["collisionProxy"], {"kind": "none"})

    @unittest.skipIf(os.name == "nt", "POSIX advisory-lock behavior")
    def test_save_waits_for_cross_process_manifest_lock(self) -> None:
        marker = self.repository / "child-finished"
        code = """
from pathlib import Path
import sys
sys.path.insert(0, sys.argv[1])
from editor.asset_document import AssetDocument
document = AssetDocument.open(Path(sys.argv[2]), 'FT-01-tabletop-greybox')
document.set_material('MAT_TabletopTrail_Grey', roughness=0.41)
document.save()
Path(sys.argv[3]).write_text('done', encoding='utf-8')
"""
        with _manifest_lock(self.manifest):
            child = subprocess.Popen([
                sys.executable,
                "-c",
                code,
                str(TOOLS),
                str(self.repository),
                str(marker),
            ])
            time.sleep(0.2)
            self.assertIsNone(child.poll())
            self.assertFalse(marker.exists())
        stdout, stderr = child.communicate(timeout=5)
        self.assertEqual(child.returncode, 0, (stdout, stderr))
        self.assertTrue(marker.is_file())


if __name__ == "__main__":
    unittest.main()
