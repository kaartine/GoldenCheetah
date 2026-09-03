#!/usr/bin/env python3

import importlib.util
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("pre_release_ui.py")
SPEC = importlib.util.spec_from_file_location("pre_release_ui", MODULE_PATH)
UI = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(UI)

MATRIX_PATH = Path(__file__).with_name("run-pre-release-ui-matrix.sh")


class PreReleaseUiWorkflowTests(unittest.TestCase):
    def test_matrix_runs_painter_scenegraph_and_production_quick3d(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            matrix = root / MATRIX_PATH.name
            runner = root / "run-pre-release-ui.sh"
            image = root / "GoldenCheetah.AppImage"
            artifacts = root / "artifacts"
            calls = root / "calls.txt"
            shutil.copy2(MATRIX_PATH, matrix)
            runner.write_text(
                "#!/bin/sh\n"
                "printf '%s|%s|%s|%s|%s|%s\\n' "
                '"$GC_WORKOUT_GAME_FORCE_PAINTER" '
                '"$GC_WORKOUT_GAME_3D" '
                '"$GC_WORKOUT_GAME_TRACE" '
                '"$GC_WORKOUT_GAME_DIAGNOSTICS" '
                '"$GC_UI_REQUIRE_QUICK3D_EVIDENCE" "$2" >>"$CALLS"\n',
                encoding="ascii",
            )
            for executable in (matrix, runner):
                executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
            image.write_text("fake image\n", encoding="ascii")
            environment = dict(os.environ, CALLS=str(calls))

            completed = subprocess.run(
                [str(matrix), str(image), str(artifacts)],
                env=environment,
                check=False,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(
                calls.read_text(encoding="ascii").splitlines(),
                [
                    f"1|0|0|0|0|{artifacts / 'painter'}",
                    f"0|0|1|1|0|{artifacts / 'scenegraph'}",
                    f"0|1|1|1|1|{artifacts / 'quick3d'}",
                ],
            )

    def test_workout_game_lifecycle_has_one_ordered_training_session(self):
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        calls = mock.Mock()
        workflow.open_game = calls.open_game
        workflow.start = calls.start
        workflow.shift_up = calls.shift_up
        workflow.shift_down = calls.shift_down
        workflow.stop_and_continue = calls.stop_and_continue
        workflow.stop_save_and_reopen = calls.stop_save_and_reopen
        recording = Path("isolated-recording.csv")
        saved = Path("isolated-activity.json")
        workflow.start.return_value = recording
        workflow.stop_save_and_reopen.return_value = saved

        result = workflow.run()

        self.assertEqual(result, saved)
        self.assertEqual(
            calls.mock_calls,
            [
                mock.call.open_game(),
                mock.call.start(),
                mock.call.shift_up(),
                mock.call.shift_down(),
                mock.call.stop_and_continue(recording),
                mock.call.stop_save_and_reopen(recording),
            ],
        )

    def test_prepare_uses_only_the_requested_isolated_library(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            UI.prepare(root)

            athlete = root / "library" / UI.ATHLETE
            train_settings = (
                root / "library" / "configglobal-trainmode.ini"
            ).read_text(encoding="utf-8")
            self.assertTrue((athlete / "activities").is_dir())
            self.assertTrue((athlete / "records").is_dir())
            self.assertTrue((athlete / "workouts" / "ui-test.erg").is_file())
            self.assertIn("devicename1=Data Generator\n", train_settings)
            self.assertNotIn(str(Path.home() / ".goldencheetah"), train_settings)

    def test_stop_continue_resumes_the_same_raw_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            stop = object()
            continue_button = object()
            driver = mock.Mock()
            driver.find.side_effect = [stop, continue_button]
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.capture_screenshots = False
            workflow.run_delays = (0.0, 0.0, 0.0)

            with mock.patch.object(UI.time, "sleep"):
                workflow.stop_and_continue(recording)

            self.assertEqual(
                driver.find.call_args_list,
                [
                    mock.call(
                        "Stop training", "push button", showing=True
                    ),
                    mock.call(
                        "Continue Training",
                        "push button",
                        showing=True,
                        timeout=8.0,
                    ),
                ],
            )
            self.assertEqual(
                driver.activate.call_args_list,
                [mock.call(stop), mock.call(continue_button)],
            )
            driver.wait_file_growth.assert_called_once_with(
                recording, recording.stat().st_size
            )

    def test_stop_save_reopens_the_new_isolated_activity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            recording = root / "records" / "recording.csv"
            activity = root / "activities" / "saved.json"
            artifacts = root / "artifacts"
            recording.parent.mkdir()
            activity.parent.mkdir()
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            activity.write_text("{}\n", encoding="ascii")
            stop = object()
            save = object()
            finish = object()
            driver = mock.Mock()
            driver.find.side_effect = [stop, save, finish]
            driver.wait_new_file.return_value = activity
            driver.reopen_saved_activity.return_value = "saved activity row"
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.activities = activity.parent
            workflow.existing_activities = set()
            workflow.artifacts = artifacts
            workflow.capture_screenshots = False

            with mock.patch.object(UI.time, "sleep"), mock.patch.dict(
                os.environ, {"GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "0"}
            ):
                result = workflow.stop_save_and_reopen(recording)

            self.assertEqual(result, activity)
            driver.wait_new_file.assert_called_once_with(
                activity.parent, set(), "*.json", timeout=15.0
            )
            driver.reopen_saved_activity.assert_called_once_with(activity)
            self.assertEqual(
                (artifacts / "reopened-activity.txt").read_text(
                    encoding="utf-8"
                ),
                "saved.json\nsaved activity row\n",
            )

    def test_reopen_selects_an_activity_row_after_leaving_game_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            activity = Path(directory) / "saved.json"
            activity.write_text("{}\n", encoding="ascii")
            table = object()
            row = object()
            driver = object.__new__(UI.UiDriver)
            driver.activate_named = mock.Mock()
            driver.find = mock.Mock(return_value=object())
            driver.find_all = mock.Mock(
                side_effect=lambda role=None, showing=None: (
                    [table] if role == "table" else []
                )
            )
            driver.all_nodes = mock.Mock(return_value=[table, row])
            driver.role = mock.Mock(
                side_effect=lambda node: "table cell" if node is row else "table"
            )
            driver.showing = mock.Mock(return_value=True)
            driver.enabled = mock.Mock(return_value=True)
            driver.selectable = mock.Mock(
                side_effect=lambda node: node is row
            )
            driver.activate = mock.Mock()
            driver.selected = mock.Mock(return_value=True)
            driver.name = mock.Mock(return_value="saved activity row")

            selected = driver.reopen_saved_activity(activity, timeout=0.1)

            self.assertEqual(selected, "saved activity row")
            self.assertEqual(
                driver.activate_named.call_args_list,
                [
                    mock.call("Train", "menu item"),
                    mock.call("Activities", "menu item"),
                ],
            )
            driver.activate.assert_called_once_with(row)


if __name__ == "__main__":
    unittest.main()
