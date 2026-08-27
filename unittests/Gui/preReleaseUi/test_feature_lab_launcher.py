#!/usr/bin/env python3

import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("run-feature-lab.sh")


class FeatureLabLauncherTests(unittest.TestCase):
    def test_launcher_selects_complete_3d_lab_and_forwards_arguments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "environment.txt"
            image = root / "GoldenCheetah.AppImage"
            image.write_text(
                "#!/usr/bin/env bash\n"
                "printf '%s\\n' \"$GC_WORKOUT_GAME_3D\" "
                "\"$GC_WORKOUT_GAME_FEATURE_LAB\" "
                "\"$GC_WORKOUT_GAME_DIAGNOSTICS\" \"$*\" >\"$OUTPUT\"\n",
                encoding="ascii",
            )
            image.chmod(image.stat().st_mode | stat.S_IXUSR)
            environment = os.environ.copy()
            environment["OUTPUT"] = str(output)

            completed = subprocess.run(
                [str(SCRIPT), str(image), "library", "Athlete"],
                env=environment,
                check=False,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                output.read_text(encoding="ascii").splitlines(),
                ["1", "1", "1", "library Athlete"],
            )

    def test_launcher_rejects_missing_image(self):
        completed = subprocess.run(
            [str(SCRIPT), "/does/not/exist.AppImage"],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("AppImage is missing or not executable", completed.stderr)
        self.assertIn("/does/not/exist.AppImage", completed.stderr)


if __name__ == "__main__":
    unittest.main()
