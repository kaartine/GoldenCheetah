#!/usr/bin/env python3

from pathlib import Path
import py_compile
import subprocess
import sys
import unittest


REPOSITORY = Path(__file__).resolve().parents[3]
TOOLS = REPOSITORY / "contrib/workout-game-assets"
sys.path.insert(0, str(TOOLS))

from gallery_catalog import load_gallery_assets  # noqa: E402


class WorkoutGameGalleryTest(unittest.TestCase):
    def test_catalog_contains_every_manifest_backed_glb(self) -> None:
        assets = load_gallery_assets(REPOSITORY)
        generated = set((TOOLS / "generated").glob("*.glb"))

        self.assertEqual({asset.path for asset in assets}, generated)
        self.assertEqual(len(assets), 10)
        self.assertEqual(len({asset.asset_id for asset in assets}), len(assets))
        self.assertTrue(all(asset.display_name for asset in assets))
        self.assertTrue(all(asset.triangles > 0 for asset in assets))

    def test_gallery_scripts_compile(self) -> None:
        for path in (
            TOOLS / "gallery_catalog.py",
            TOOLS / "blender/workout_game_asset_gallery.py",
        ):
            py_compile.compile(str(path), doraise=True)

    def test_launcher_has_valid_shell_syntax(self) -> None:
        for path in (
            TOOLS / "open_gallery.sh",
            TOOLS / "blender/run_gallery_container.sh",
        ):
            subprocess.run(["bash", "-n", str(path)], check=True)


if __name__ == "__main__":
    unittest.main()
