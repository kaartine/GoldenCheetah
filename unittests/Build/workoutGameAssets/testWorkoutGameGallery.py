#!/usr/bin/env python3

from pathlib import Path
import hashlib
import py_compile
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPOSITORY = Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "contrib/workout-game-assets"
sys.path.insert(0, str(TOOLS))

from gallery_catalog import (  # noqa: E402
    load_candidate_gallery_assets,
    load_gallery_assets,
    validate_gallery_asset_file,
)


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class WorkoutGameGalleryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.external_root = Path(self.temporary.name)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _candidate_directory(self) -> tuple[Path, dict]:
        directory = self.external_root / "candidate"
        directory.mkdir()
        lod0 = directory / "candidate-lod0.glb"
        lod1 = directory / "candidate-lod1.glb"
        shutil.copyfile(TOOLS / "generated/WG_RiderBike.glb", lod0)
        shutil.copyfile(TOOLS / "generated/WG_ConiferSet.glb", lod1)
        descriptor = {
            "candidateId": "gallery-review-001",
            "technical": {
                "lod0": {"triangles": 7_494},
                "lod1": {"triangles": 536},
            },
            "files": [
                {
                    "role": "review-model-lod0",
                    "name": lod0.name,
                    "sha256": _sha256(lod0),
                    "bytes": lod0.stat().st_size,
                },
                {
                    "role": "review-model-lod1",
                    "name": lod1.name,
                    "sha256": _sha256(lod1),
                    "bytes": lod1.stat().st_size,
                },
            ],
        }
        return directory, descriptor

    def _load_candidate(self, directory: Path, descriptor: dict):
        validator = mock.Mock()
        validator.verify_candidate_directory.return_value = descriptor
        with mock.patch("gallery_catalog._triposr_validator", return_value=validator):
            assets = load_candidate_gallery_assets(REPOSITORY, directory)
        validator.verify_candidate_directory.assert_called_once_with(directory.resolve())
        return assets

    def test_catalog_contains_every_manifest_backed_glb(self) -> None:
        assets = load_gallery_assets(REPOSITORY)
        generated = set((TOOLS / "generated").glob("*.glb"))

        self.assertEqual({asset.path for asset in assets}, generated)
        self.assertEqual(len(assets), 10)
        self.assertEqual(len({asset.asset_id for asset in assets}), len(assets))
        self.assertTrue(all(asset.display_name for asset in assets))
        self.assertTrue(all(asset.triangles > 0 for asset in assets))
        self.assertFalse(any(asset.review_only for asset in assets))

    def test_gallery_scripts_compile(self) -> None:
        for path in (
            TOOLS / "gallery_catalog.py",
            TOOLS / "blender/workout_game_asset_gallery.py",
            TOOLS / "triposr/blender/render_candidate_audit.py",
        ):
            py_compile.compile(str(path), doraise=True)

    def test_external_candidate_adds_two_review_only_lods(self) -> None:
        directory, descriptor = self._candidate_directory()
        assets = self._load_candidate(directory, descriptor)

        self.assertEqual([asset.lod_level for asset in assets], ["LOD0", "LOD1"])
        self.assertEqual(len({asset.asset_id for asset in assets}), 2)
        self.assertTrue(all(asset.review_only for asset in assets))
        self.assertTrue(all("REVIEW ONLY" in asset.display_name for asset in assets))
        self.assertTrue(all(asset.path.parent == directory for asset in assets))
        for asset in assets:
            validate_gallery_asset_file(asset)

    def test_external_candidate_verifier_rejection_blocks_loading(self) -> None:
        directory, _descriptor = self._candidate_directory()
        validator = mock.Mock()
        validator.verify_candidate_directory.side_effect = ValueError(
            "candidate descriptor or GLB SHA-256 mismatch"
        )
        with mock.patch("gallery_catalog._triposr_validator", return_value=validator):
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                load_candidate_gallery_assets(REPOSITORY, directory)
        validator.verify_candidate_directory.assert_called_once_with(directory.resolve())

    def test_external_candidate_detects_change_before_blender_import(self) -> None:
        directory, descriptor = self._candidate_directory()
        asset = self._load_candidate(directory, descriptor)[0]
        with asset.path.open("ab") as output:
            output.write(b"changed-after-verification")
        with self.assertRaisesRegex(ValueError, "changed after verification"):
            validate_gallery_asset_file(asset)

    def test_external_candidate_must_remain_outside_repository(self) -> None:
        with self.assertRaisesRegex(ValueError, "outside the repository"):
            load_candidate_gallery_assets(REPOSITORY, TOOLS / "triposr")

        link = self.external_root / "repository-link"
        link.symlink_to(TOOLS / "triposr", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            load_candidate_gallery_assets(REPOSITORY, link)

    def test_external_candidate_rejects_symlink_and_file_escape(self) -> None:
        directory, descriptor = self._candidate_directory()
        link = self.external_root / "candidate-link"
        link.symlink_to(directory, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            load_candidate_gallery_assets(REPOSITORY, link)

        descriptor["files"][0]["name"] = "../escaped.glb"
        with self.assertRaisesRegex(ValueError, "unexpected LOD file inventory"):
            self._load_candidate(directory, descriptor)

    def test_launcher_has_valid_shell_syntax(self) -> None:
        for path in (
            TOOLS / "open_gallery.sh",
            TOOLS / "blender/run_gallery_container.sh",
        ):
            subprocess.run(["bash", "-n", str(path)], check=True)


if __name__ == "__main__":
    unittest.main()
