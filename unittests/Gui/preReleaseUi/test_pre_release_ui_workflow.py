#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
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
RUNNER_PATH = Path(__file__).with_name("run-pre-release-ui.sh")


class PreReleaseUiWorkflowTests(unittest.TestCase):
    def test_course_sidecar_acceptance_requires_exact_prescription_and_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            sidecar = Path(directory) / "ui-test-mtb.gcmtb.json"
            document = {
                "title": "ui-test MTB",
                "source": {
                    "intervals": [
                        {
                            "startMs": 0,
                            "durationMs": 6000,
                            "startWatts": 100.0,
                            "endWatts": 100.0,
                        },
                        {
                            "startMs": 6000,
                            "durationMs": 4800,
                            "startWatts": 220.0,
                            "endWatts": 220.0,
                        },
                    ]
                },
                "conversion": {
                    "preset": "ride-first",
                    "parameters": {
                        "gradeScale": 1.18,
                        "technicality": 0.95,
                    },
                },
                "course": {
                    "sections": [
                        {
                            "sourceStartMs": 0,
                            "nominalDurationMs": 6000,
                            "targetStartWatts": 100.0,
                            "targetEndWatts": 100.0,
                            "gradePercent": 1.0,
                            "terrain": "smooth-trail",
                        },
                        {
                            "sourceStartMs": 6000,
                            "nominalDurationMs": 4800,
                            "targetStartWatts": 220.0,
                            "targetEndWatts": 220.0,
                            "gradePercent": 2.0,
                            "terrain": "rock-garden",
                        },
                    ]
                },
                "roadPlan": {
                    "pieces": [
                        {"turnRadians": 0.1},
                        {"turnRadians": -0.2},
                    ]
                },
            }
            sidecar.write_text(json.dumps(document), encoding="utf-8")

            accepted = UI.validate_mtb_course_sidecar(
                sidecar, "ride-first", "ui-test MTB"
            )

            self.assertEqual(accepted["title"], "ui-test MTB")
            self.assertEqual(accepted["interval_count"], 2)
            self.assertEqual(accepted["duration_ms"], 10800)

            document["course"]["sections"][1]["targetStartWatts"] = 219.0
            sidecar.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(UI.UiFailure, "prescription"):
                UI.validate_mtb_course_sidecar(
                    sidecar, "ride-first", "ui-test MTB"
                )

    def test_right_click_named_item_uses_context_mouse_button(self):
        item = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[item])
        driver.role = mock.Mock(return_value="table cell")
        driver.context_click = mock.Mock()

        with mock.patch.object(UI.time, "sleep"):
            driver.right_click_named_item("ui-test")

        driver.context_click.assert_called_once_with(item)

    def test_exact_selection_clicks_only_the_named_workout_row(self):
        row = object()
        driver = object.__new__(UI.UiDriver)
        driver.click_named_item = mock.Mock(return_value=row)

        selected = driver.select_named_item_exact("ui-test-mtb")

        self.assertIs(selected, row)
        driver.click_named_item.assert_called_once_with("ui-test-mtb")

    def test_runner_requires_generated_distance_course_at_game_start(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn("GC_UI_VALIDATE_MTB_COURSE", runner)
        self.assertIn("Workout Game session course: distance-course", runner)
        self.assertIn("mtb-course-runtime-evidence.txt", runner)
        self.assertIn("--cold-start-continuity-only", runner)
        self.assertIn("GC_UI_USE_HARDWARE_GL", runner)

    def test_popup_item_activation_uses_position_independent_keyboard_steps(self):
        driver = object.__new__(UI.UiDriver)
        driver.send_named_key = mock.Mock()

        driver.activate_popup_item(5)

        self.assertEqual(
            driver.send_named_key.call_args_list,
            [mock.call("Home")] + [mock.call("Down")] * 5
            + [mock.call("Return")],
        )
        self.assertEqual(
            UI.MtbCourseUiWorkflow.CONTEXT_ACTION_STEPS,
            {"Create MTB Course": 5, "Edit MTB Course": 5},
        )

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
                "printf '%s|%s|%s|%s|%s|%s|%s\\n' "
                '"$GC_WORKOUT_GAME_FORCE_PAINTER" '
                '"$GC_WORKOUT_GAME_3D" '
                '"$GC_WORKOUT_GAME_TRACE" '
                '"$GC_WORKOUT_GAME_DIAGNOSTICS" '
                '"$GC_UI_REQUIRE_QUICK3D_EVIDENCE" '
                '"$GC_UI_VALIDATE_MTB_COURSE" "$2" >>"$CALLS"\n',
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
                    f"1|0|0|0|0|0|{artifacts / 'painter'}",
                    f"0|0|1|1|0|0|{artifacts / 'scenegraph'}",
                    f"0|1|1|1|1|1|{artifacts / 'quick3d'}",
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

    def test_mtb_course_lifecycle_persists_all_three_modes_in_order(self):
        workflow = object.__new__(UI.MtbCourseUiWorkflow)
        calls = mock.Mock()
        workflow.create = calls.create
        workflow.edit = calls.edit
        prescription = [{"durationMs": 6000, "startWatts": 100.0}]
        calm = {
            "title": "ui-test MTB",
            "preset": "workout-first",
            "source_intervals": prescription,
            "route_fingerprint": "calm",
            "grade_scale": 0.82,
            "technicality": 0.15,
            "technical_section_count": 3,
            "total_absolute_turn_radians": 1.0,
        }
        varied = {
            "title": "ui-test MTB",
            "preset": "balanced",
            "source_intervals": prescription,
            "route_fingerprint": "varied",
            "grade_scale": 1.0,
            "technicality": 0.55,
            "technical_section_count": 6,
            "total_absolute_turn_radians": 2.0,
        }
        technical = {
            "title": "ui-test MTB",
            "preset": "ride-first",
            "source_intervals": prescription,
            "route_fingerprint": "technical",
            "grade_scale": 1.18,
            "technicality": 0.95,
            "technical_section_count": 9,
            "total_absolute_turn_radians": 3.0,
        }
        workflow.create.return_value = calm
        workflow.edit.side_effect = [varied, technical]

        with tempfile.TemporaryDirectory() as directory:
            workflow.artifacts = Path(directory)
            result = workflow.run()

        self.assertEqual(result, technical)
        self.assertEqual(
            calls.mock_calls,
            [
                mock.call.create("workout-first"),
                mock.call.edit("balanced"),
                mock.call.edit("ride-first"),
            ],
        )

    def test_mtb_course_lifecycle_rides_each_persisted_mode(self):
        workflow = object.__new__(UI.MtbCourseUiWorkflow)
        prescription = [{"durationMs": 6000, "startWatts": 100.0}]
        results = [
            {
                "preset": preset,
                "workout_name": "ui-test-mtb",
                "source_intervals": prescription,
                "route_fingerprint": f"route-{index}",
                "grade_scale": 0.7 + index * 0.3,
                "technicality": 0.1 + index * 0.4,
                "technical_section_count": 3 + index * 3,
                "total_absolute_turn_radians": 1.0 + index,
            }
            for index, preset in enumerate(
                ("workout-first", "balanced", "ride-first")
            )
        ]
        workflow.create = mock.Mock(return_value=results[0])
        workflow.edit = mock.Mock(side_effect=results[1:])
        workflow.ride_course = mock.Mock()

        with tempfile.TemporaryDirectory() as directory:
            workflow.artifacts = Path(directory)
            workflow.run()

        self.assertEqual(
            workflow.ride_course.call_args_list,
            [
                mock.call("workout-first", results[0]),
                mock.call("balanced", results[1]),
                mock.call("ride-first", results[2]),
            ],
        )

    def test_mtb_course_lifecycle_rejects_identical_routes(self):
        workflow = object.__new__(UI.MtbCourseUiWorkflow)
        result = {
            "source_intervals": [{"durationMs": 6000}],
            "route_fingerprint": "same-route",
            "grade_scale": 1.0,
            "technicality": 0.5,
            "technical_section_count": 5,
            "total_absolute_turn_radians": 2.0,
        }
        workflow.create = mock.Mock(return_value=result)
        workflow.edit = mock.Mock(return_value=result)

        with tempfile.TemporaryDirectory() as directory:
            workflow.artifacts = Path(directory)
            with self.assertRaisesRegex(UI.UiFailure, "identical routes"):
                workflow.run()

    def test_mtb_course_lifecycle_rejects_equal_technical_exposure(self):
        workflow = object.__new__(UI.MtbCourseUiWorkflow)
        prescription = [{"durationMs": 6000}]
        results = [
            {
                "source_intervals": prescription,
                "route_fingerprint": f"route-{index}",
                "grade_scale": 0.7 + index * 0.3,
                "technicality": 0.1 + index * 0.4,
                "technical_section_count": 5,
                "total_absolute_turn_radians": 1.0 + index,
            }
            for index in range(3)
        ]
        workflow.create = mock.Mock(return_value=results[0])
        workflow.edit = mock.Mock(side_effect=results[1:])

        with tempfile.TemporaryDirectory() as directory:
            workflow.artifacts = Path(directory)
            with self.assertRaisesRegex(UI.UiFailure, "technical terrain ordering"):
                workflow.run()

    def test_mtb_course_preset_selection_requires_checked_state(self):
        control = object()
        driver = mock.Mock()
        driver.find.return_value = control
        driver.checked.return_value = True
        workflow = object.__new__(UI.MtbCourseUiWorkflow)
        workflow.driver = driver

        workflow._select_preset("ride-first")

        driver.find.assert_called_once_with(
            "Ride first", showing=True, timeout=8.0
        )
        driver.click.assert_called_once_with(control)
        driver.checked.assert_called_with(control)

    def test_prepared_workout_selection_retries_while_library_refreshes(self):
        driver = mock.Mock()
        driver.select_named_item_exact.side_effect = [
            UI.UiFailure("not ready"),
            UI.UiFailure("not ready"),
            UI.UiFailure("not ready"),
            None,
        ]
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver

        with mock.patch.object(UI.time, "sleep"):
            workflow.select_prepared_workout(timeout=1.0)

        self.assertEqual(driver.select_named_item_exact.call_count, 4)
        self.assertEqual(
            driver.select_named_item_exact.call_args.args[0],
            "Pre-release UI test",
        )

    def test_explicit_generated_workout_never_falls_back_to_source(self):
        driver = mock.Mock()
        driver.select_named_item_exact.side_effect = UI.UiFailure("not ready")
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver
        workflow.workout_names = ("ui-test-mtb",)

        with mock.patch.object(UI.time, "sleep"), self.assertRaisesRegex(
            UI.UiFailure, "ui-test-mtb"
        ):
            workflow.select_prepared_workout(timeout=0.01)

        self.assertTrue(driver.select_named_item_exact.called)
        self.assertEqual(
            {call.args[0] for call in driver.select_named_item_exact.call_args_list},
            {"ui-test-mtb"},
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

    def test_preset_smoke_ride_records_motion_and_discards_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            cancel = object()
            driver = mock.Mock()
            driver.find.return_value = cancel
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.capture_screenshots = False
            workflow.open_game = mock.Mock()
            workflow.start = mock.Mock(return_value=recording)
            workflow.activate_stop_training = mock.Mock()

            with mock.patch.object(UI.time, "sleep"):
                workflow.run_smoke_and_discard("balanced")

            workflow.open_game.assert_called_once_with()
            workflow.start.assert_called_once_with("04-mtb-course-balanced-first")
            driver.wait_file_growth.assert_called_once_with(
                recording, recording.stat().st_size
            )
            workflow.activate_stop_training.assert_called_once_with()
            driver.find.assert_called_once_with(
                "Cancel", "push button", showing=True, timeout=8.0
            )
            driver.activate.assert_called_once_with(cancel)
            driver.wait_file_removed.assert_called_once_with(recording)

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

    def test_all_nodes_does_not_descend_into_quick3d_canvas(self):
        canvas = mock.MagicMock()
        canvas.name = "Workout game 3D canvas"
        driver = object.__new__(UI.UiDriver)

        self.assertEqual(list(driver.all_nodes(canvas)), [canvas])

        canvas.__iter__.assert_not_called()

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
