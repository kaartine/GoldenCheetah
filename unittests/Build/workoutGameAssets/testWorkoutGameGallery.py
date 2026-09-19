#!/usr/bin/env python3

from pathlib import Path
import hashlib
import os
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
    gallery_asset_index,
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
        self.assertEqual(assets[0].selection_aliases, ("gallery-review-001",))
        self.assertEqual(assets[1].selection_aliases, ())
        self.assertEqual(gallery_asset_index(assets, "gallery-review-001"), 0)
        self.assertEqual(
            gallery_asset_index(assets, "triposr-review-gallery-review-001-lod1"),
            1,
        )
        for asset in assets:
            validate_gallery_asset_file(asset)

    def test_gallery_selector_rejects_unknown_identifier(self) -> None:
        assets = load_gallery_assets(REPOSITORY)
        with self.assertRaisesRegex(ValueError, "unknown gallery asset selector"):
            gallery_asset_index(assets, "does-not-exist")

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

    def _capture_docker_launcher(
        self,
        *arguments: str,
        default_opengl: str = "4.6",
        prime_opengl: str = "4.6",
    ) -> list[str]:
        binary = self.external_root / "bin"
        binary.mkdir(exist_ok=True)
        capture = self.external_root / "docker-arguments"
        docker = binary / "docker"
        docker.write_text(
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = image ]; then exit 0; fi\n"
            "printf '%s\\n' \"$@\" > \"$WG_DOCKER_CAPTURE\"\n",
            encoding="utf-8",
        )
        docker.chmod(0o755)
        glxinfo = binary / "glxinfo"
        glxinfo.write_text(
            "#!/bin/sh\n"
            f'default_opengl="{default_opengl}"\n'
            f'prime_opengl="{prime_opengl}"\n'
            'if [ "${DRI_PRIME:-}" = 1 ]; then version="$prime_opengl"; '
            'else version="$default_opengl"; fi\n'
            'printf "Max core profile version: %s (Core Profile)\\n" "$version"\n',
            encoding="utf-8",
        )
        glxinfo.chmod(0o755)
        environment = dict(os.environ)
        environment.update({
            "DISPLAY": ":99",
            "PATH": f"{binary}:/usr/bin:/bin",
            "WG_DOCKER_CAPTURE": str(capture),
            "XAUTHORITY": str(self.external_root / "missing-authority"),
        })
        subprocess.run(
            [str(TOOLS / "open_gallery.sh"), *arguments],
            check=True,
            env=environment,
        )
        return capture.read_text(encoding="utf-8").splitlines()

    def test_docker_launcher_selects_supported_discrete_gpu(self) -> None:
        arguments = self._capture_docker_launcher(
            default_opengl="4.2", prime_opengl="4.3"
        )

        self.assertIn("DRI_PRIME=1", arguments)
        self.assertNotIn("LIBGL_ALWAYS_SOFTWARE=1", arguments)
        self.assertNotIn("GALLIUM_DRIVER=llvmpipe", arguments)

    def test_docker_launcher_falls_back_for_unsupported_hardware(self) -> None:
        arguments = self._capture_docker_launcher(
            default_opengl="4.2", prime_opengl="4.2"
        )

        self.assertIn("LIBGL_ALWAYS_SOFTWARE=1", arguments)
        self.assertIn("GALLIUM_DRIVER=llvmpipe", arguments)
        self.assertNotIn("/dev/dri:/dev/dri", arguments)

    def test_docker_launcher_is_read_only_by_default(self) -> None:
        arguments = self._capture_docker_launcher("--asset", "FT-01-tabletop-greybox")
        repository_mount = f"{REPOSITORY}:/work:ro"
        manifest_mount = (
            f"{TOOLS / 'manifests'}:"
            "/work/contrib/workout-game-assets/manifests:rw"
        )

        self.assertIn(repository_mount, arguments)
        self.assertNotIn(manifest_mount, arguments)
        self.assertNotIn("--edit", arguments)

    def test_docker_edit_mounts_only_manifest_directory_read_write(self) -> None:
        arguments = self._capture_docker_launcher(
            "--edit", "--asset", "FT-01-tabletop-greybox"
        )
        repository_mount = f"{REPOSITORY}:/work:ro"
        manifest_mount = (
            f"{TOOLS / 'manifests'}:"
            "/work/contrib/workout-game-assets/manifests:rw"
        )

        self.assertIn(repository_mount, arguments)
        self.assertIn(manifest_mount, arguments)
        self.assertIn("--edit", arguments)

    def test_docker_evidence_mount_is_explicit_and_outside_repository(self) -> None:
        evidence = self.external_root / "evidence"
        evidence.mkdir()
        arguments = self._capture_docker_launcher(
            "--ui-smoke-test", "--evidence-directory", str(evidence)
        )

        self.assertIn(f"{evidence}:/evidence:rw", arguments)
        self.assertIn("--evidence-directory", arguments)
        self.assertIn("/evidence", arguments)

        result = subprocess.run(
            [
                str(TOOLS / "open_gallery.sh"),
                "--evidence-directory",
                str(TOOLS),
            ],
            check=False,
            env={
                **os.environ,
                "DISPLAY": ":99",
                "PATH": f"{self.external_root / 'bin'}:/usr/bin:/bin",
                "WG_DOCKER_CAPTURE": str(self.external_root / "unused"),
                "XAUTHORITY": str(self.external_root / "missing-authority"),
            },
            text=True,
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2, result)
        self.assertIn("must remain outside the repository", result.stderr)

    def test_docker_edit_rejects_symlinked_manifest_directory(self) -> None:
        repository = self.external_root / "repository"
        tools = repository / "contrib/workout-game-assets"
        tools.mkdir(parents=True)
        launcher = tools / "open_gallery.sh"
        shutil.copyfile(TOOLS / "open_gallery.sh", launcher)
        launcher.chmod(0o755)
        external_manifests = self.external_root / "external-manifests"
        external_manifests.mkdir()
        (tools / "manifests").symlink_to(
            external_manifests, target_is_directory=True
        )

        binary = self.external_root / "symlink-bin"
        binary.mkdir()
        docker = binary / "docker"
        docker.write_text(
            "#!/bin/sh\n"
            "if [ \"${1:-}\" = image ]; then exit 0; fi\n"
            "exit 99\n",
            encoding="utf-8",
        )
        docker.chmod(0o755)
        environment = dict(os.environ)
        environment.update({
            "DISPLAY": ":99",
            "PATH": f"{binary}:/usr/bin:/bin",
            "XAUTHORITY": str(self.external_root / "missing-authority"),
        })

        result = subprocess.run(
            [str(launcher), "--edit"],
            check=False,
            env=environment,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 2, result)
        self.assertIn("must not contain symlinks", result.stderr)


if __name__ == "__main__":
    unittest.main()
