#!/usr/bin/env python3

import contextlib
import importlib.util
import io
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

    def test_prepared_workout_selection_retries_while_library_refreshes(self):
        driver = mock.Mock()
        driver.click_named_item.side_effect = [
            UI.UiFailure("not ready"),
            UI.UiFailure("not ready"),
            UI.UiFailure("not ready"),
            None,
        ]
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver

        with mock.patch.object(UI.time, "sleep"):
            workflow.select_prepared_workout(timeout=1.0)

        self.assertEqual(driver.click_named_item.call_count, 4)
        self.assertEqual(
            driver.click_named_item.call_args,
            mock.call("Pre-release UI test"),
        )

    def test_focus_main_window_does_not_require_an_atspi_component(self):
        window = mock.Mock()
        window.get_wm_name.return_value = UI.ATHLETE
        display = mock.Mock()
        display.screen.return_value.root = window
        driver = object.__new__(UI.UiDriver)
        driver.display = display
        driver.X = mock.Mock(RevertToParent=1, CurrentTime=2)
        driver.find = mock.Mock(
            side_effect=AssertionError("AT-SPI frame must not be required")
        )

        driver.focus_main_window()

        window.set_input_focus.assert_called_once_with(1, 2)
        display.sync.assert_called_once_with()
        driver.find.assert_not_called()

    def test_start_uses_canvas_name_captured_before_quick3d_initialization(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            records = root / "records"
            records.mkdir()
            recording = records / "recording.csv"
            recording.write_text("secs,watts\n", encoding="ascii")
            start_button = object()
            stale_canvas = object()
            gear = object()
            driver = mock.Mock()
            driver.find.return_value = start_button
            driver.wait_new_file.return_value = recording
            driver.current_value.return_value = 7.0
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.root = root
            workflow.records = records
            workflow.existing_records = set()
            workflow.canvas = stale_canvas
            workflow.canvas_accessible_name = "Workout game 3D canvas"
            workflow.gear = gear
            workflow.capture_screenshots = False

            result = workflow.start()

            self.assertEqual(result, recording)
            driver.find_named_any.assert_not_called()
            driver.name.assert_not_called()
            self.assertIs(workflow.canvas, stale_canvas)
            self.assertEqual(
                (root / UI.RENDERER_CANVAS_NAME_FILE).read_text(
                    encoding="utf-8"
                ),
                "Workout game 3D canvas\n",
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
            workout = athlete / "workouts" / "ui-test.erg"
            self.assertTrue(workout.is_file())
            self.assertIn("30.00 100\n", workout.read_text(encoding="utf-8"))
            self.assertIn("devicename1=Data Generator\n", train_settings)
            self.assertNotIn(str(Path.home() / ".goldencheetah"), train_settings)

    def test_stop_continue_resumes_the_same_raw_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            stop = object()
            continue_button = object()
            driver = mock.Mock()
            driver.find.side_effect = [continue_button]
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.capture_screenshots = False
            workflow.run_delays = (0.0, 0.0, 0.0)
            workflow.stop_training_button = stop

            with mock.patch.object(UI.time, "sleep"):
                workflow.stop_and_continue(recording)

            self.assertEqual(
                driver.find.call_args_list,
                [
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
            driver.find.side_effect = [save, finish]
            driver.wait_new_file.return_value = activity
            driver.reopen_saved_activity.return_value = "saved activity row"
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.activities = activity.parent
            workflow.existing_activities = set()
            workflow.artifacts = artifacts
            workflow.capture_screenshots = False
            workflow.stop_training_button = stop

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

    def test_require_names_indexes_the_accessibility_tree_once(self):
        first = object()
        second = object()
        driver = object.__new__(UI.UiDriver)
        driver.all_nodes = mock.Mock(return_value=iter((first, second)))
        driver.name = mock.Mock(
            side_effect=lambda node: "Start" if node is first else "Stop"
        )
        driver.role = mock.Mock(return_value="push button")

        driver.require_names(("Start", "Stop"), role="push button")

        driver.all_nodes.assert_called_once_with()

    def test_stop_training_recovers_from_a_stale_cached_button(self):
        stale = object()
        replacement = object()
        driver = mock.Mock()
        driver.activate.side_effect = [UI.UiFailure("stale"), None]
        driver.find.return_value = replacement
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver
        workflow.stop_training_button = stale

        workflow.activate_stop_training()

        driver.find.assert_called_once_with(
            "Stop training", "push button", showing=True
        )
        self.assertEqual(driver.activate.call_args_list,
                         [mock.call(stale), mock.call(replacement)])
        self.assertIs(workflow.stop_training_button, replacement)

    def test_suite_captures_a_best_effort_failure_screenshot(self):
        with tempfile.TemporaryDirectory() as directory:
            driver = mock.Mock()
            suite = UI.Suite(driver, Path(directory))

            def fail():
                raise UI.UiFailure("expected failure")

            with contextlib.redirect_stderr(io.StringIO()):
                suite.run("Workout Game lifecycle", fail)

            driver.screenshot.assert_called_once_with(
                "failure-workout-game-lifecycle"
            )
            self.assertEqual(len(suite.results), 1)
            self.assertIn("expected failure", suite.results[0][2])

    def test_reopen_selects_an_activity_row_after_leaving_game_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            activity = Path(directory) / "saved.json"
            activity.write_text("{}\n", encoding="ascii")
            table = object()
            row = object()
            view = object()
            driver = object.__new__(UI.UiDriver)
            driver.activate_named = mock.Mock()
            driver.activate_view = mock.Mock()
            driver.find = mock.Mock(return_value=object())
            driver.find_all = mock.Mock(
                side_effect=lambda name=None, role=None, showing=None: (
                    [view]
                    if name == "Activities view" and driver.activate.called
                    else [table] if role == "table" else []
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
            driver.description = mock.Mock(
                return_value="Selected activity saved.json"
            )

            selected = driver.reopen_saved_activity(activity, timeout=0.1)

            self.assertEqual(selected, "Selected activity saved.json")
            self.assertEqual(
                driver.activate_named.call_args_list,
                [mock.call("Train", "menu item", timeout=0.1)],
            )
            driver.activate_view.assert_called_once_with(
                "Activities",
                timeout=0.1,
                ready_names=("Activities view",),
            )
            driver.activate.assert_called_once_with(row)

    def test_reopen_accepts_the_exact_automatically_selected_activity(self):
        with tempfile.TemporaryDirectory() as directory:
            activity = Path(directory) / "saved.json"
            activity.write_text("{}\n", encoding="ascii")
            driver = object.__new__(UI.UiDriver)
            driver.activate_named = mock.Mock()
            driver.activate_view = mock.Mock()
            view = object()
            driver.find_all = mock.Mock(
                side_effect=lambda name=None, role=None, showing=None: (
                    [view] if name == "Activities view" else []
                )
            )
            driver.description = mock.Mock(
                return_value="Selected activity saved.json"
            )

            selected = driver.reopen_saved_activity(activity, timeout=0.1)

            self.assertEqual(selected, "Selected activity saved.json")
            driver.activate_view.assert_called_once_with(
                "Activities",
                timeout=0.1,
                ready_names=("Activities view",),
            )

    def test_reopen_rejects_an_ambiguous_activity_library(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            activity = root / "saved.json"
            activity.write_text("{}\n", encoding="ascii")
            (root / "other.json").write_text("{}\n", encoding="ascii")
            driver = object.__new__(UI.UiDriver)

            with self.assertRaisesRegex(UI.UiFailure, "exactly one isolated"):
                driver.reopen_saved_activity(activity, timeout=0.1)

    def test_activate_view_retries_until_the_destination_is_visible(self):
        control = object()
        destination = object()
        driver = object.__new__(UI.UiDriver)
        activations = 0

        def activate(node):
            nonlocal activations
            self.assertIs(node, control)
            activations += 1

        def find_all(name=None, role=None, showing=None):
            if role == "menu item":
                return [control]
            return [destination] if activations >= 2 else []

        driver.find_all = mock.Mock(side_effect=find_all)
        driver.role = mock.Mock(
            side_effect=lambda node: "menu item" if node is control else "label"
        )
        driver.enabled = mock.Mock(return_value=True)
        driver.showing = mock.Mock(return_value=True)
        driver.activate = mock.Mock(side_effect=activate)
        driver.selected = mock.Mock(return_value=False)
        driver.checked = mock.Mock(return_value=False)

        with mock.patch.object(UI.time, "sleep"):
            driver.activate_view("Activities", timeout=1.0)

        self.assertEqual(driver.activate.call_args_list, [mock.call(control)] * 2)

    def test_activate_view_does_not_accept_its_navigation_control_as_content(self):
        control = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[control])
        driver.role = mock.Mock(return_value="menu item")
        driver.enabled = mock.Mock(return_value=True)
        driver.showing = mock.Mock(return_value=True)
        driver.activate = mock.Mock()
        driver.selected = mock.Mock(return_value=False)
        driver.checked = mock.Mock(return_value=False)

        with mock.patch.object(UI.time, "sleep"), self.assertRaises(
            UI.UiFailure
        ):
            driver.activate_view("Activities", timeout=0.01)

    def test_activate_view_accepts_a_checked_navigation_control(self):
        control = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[control])
        driver.enabled = mock.Mock(return_value=True)
        driver.showing = mock.Mock(return_value=True)
        driver.activate = mock.Mock()
        driver.selected = mock.Mock(return_value=False)
        driver.checked = mock.Mock(return_value=True)

        driver.activate_view(
            "Activities", timeout=1.0, ready_names=("Activities", "Overview")
        )

        driver.activate.assert_called_once_with(control)

    def test_quick3d_capture_defers_motion_validation_to_renderer_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            driver = mock.Mock()
            driver.find.side_effect = [object(), object()]
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.capture_screenshots = False
            workflow.run_delays = (0.0, 0.0, 0.0)
            workflow.canvas = object()

            with mock.patch.object(UI.time, "sleep"), mock.patch.dict(
                os.environ, {"GC_UI_REQUIRE_QUICK3D_EVIDENCE": "1"}
            ):
                workflow.stop_and_continue(recording)

            self.assertEqual(
                driver.screenshot.call_args_list,
                [
                    mock.call(
                        "04-workout-game-quick3d-post-cold-start-first"
                    ),
                    mock.call(
                        "04-workout-game-quick3d-post-cold-start-second"
                    ),
                ],
            )
            driver.changed_pixels.assert_not_called()


if __name__ == "__main__":
    unittest.main()
