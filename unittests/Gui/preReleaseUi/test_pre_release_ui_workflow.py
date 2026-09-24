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
ENVIRONMENT_HELPER_PATH = Path(__file__).with_name("ui-test-environment.sh")
LIBRARY_SOURCE_PATH = MODULE_PATH.parents[3] / "src" / "Train" / "Library.cpp"
WORKOUT_WIZARD_SOURCE_PATH = (
    MODULE_PATH.parents[3] / "src" / "Train" / "WorkoutWizard.cpp"
)


class PreReleaseUiWorkflowTests(unittest.TestCase):
    @staticmethod
    def training_course(start=100, end=100):
        return {
            "conversion": {"preset": "workout-first", "parameters": {"referenceGear": 6}},
            "course": {"sections": [{
                "startDistanceMeters": 0, "lengthMeters": 100,
                "sourceStartMs": 0, "nominalDurationMs": 10000,
                "referenceEffortStartWatts": start, "referenceEffortEndWatts": end,
            }]},
        }

    def test_csv_oracle_uses_own_distance_not_receiver_sample(self):
        document = self.training_course(320, 340)
        # 321.665 W at gear 6 becomes 367.617142... W at gear 7.
        targets = UI.training_recorded_targets(document, 0.008325, 7)
        self.assertEqual(targets, {367, 368})
        before = "secs,cad,hr,km,watts,target,virtualgear\n1,90,120,0.001,366,366,7\n"
        after = before + "2,90,120,0.008325,368,367,7\n3,90,120,0.02,370,370,7\n"
        commands = [{"mono_ms": t, "value": value, "gear": 7, "workout_pos": t}
                    for t, value in ((1100, 368), (2100, 369))]
        UI.validate_training_phase(before, after, commands, active=True, gear=7,
                                   document=document)
        for broken in (after.replace(",367,7", ",369,7"),
                       after.replace(",370,7", ",100,7"),
                       after.replace(",370,7", ",370,6"),
                       after.replace("0.02,370", "0.002,370")):
            with self.subTest(broken=broken), self.assertRaises(UI.UiFailure):
                UI.validate_training_phase(before, broken, commands, active=True,
                                           gear=7, document=document)

    def test_csv_oracle_preserves_quantization_and_step_discontinuity(self):
        document = self.training_course(100, 100)
        document["course"]["sections"][0]["lengthMeters"] = 1.234567
        second = dict(document["course"]["sections"][0],
                      startDistanceMeters=1.234567, lengthMeters=10,
                      referenceEffortStartWatts=300, referenceEffortEndWatts=300)
        document["course"]["sections"].append(second)
        # QTextStream's default six significant digits places this distance
        # bin across the step. Intermediate watts must never be admitted.
        self.assertEqual(UI.training_recorded_targets(document, 0.00123457, 6), {100, 300})
        self.assertEqual(UI.training_recorded_targets(document, 0.00123456, 6), {100})
        self.assertEqual(UI.training_recorded_targets(document, 0.00123458, 6), {300})
        for km, gear in ((-1, 6), (float("nan"), 6), (1, 6), (0.001, 13)):
            with self.subTest(km=km, gear=gear), self.assertRaises(UI.UiFailure):
                UI.training_recorded_targets(document, km, gear)

    def test_csv_precision_bin_is_asymmetric_at_power_of_ten(self):
        document = self.training_course()
        document["course"]["sections"][0]["lengthMeters"] = 9.99997
        document["course"]["sections"].append(dict(
            document["course"]["sections"][0], startDistanceMeters=9.99997,
            lengthMeters=10, referenceEffortStartWatts=300, referenceEffortEndWatts=300))
        self.assertEqual(UI.training_recorded_targets(document, 0.01, 6), {300})
        self.assertEqual(UI.training_recorded_targets(document, 0.00999996, 6), {100})

    def test_csv_oracle_zero_reference_gear_and_clamps(self):
        self.assertEqual(UI.training_recorded_targets(self.training_course(), 0, 6), {100})
        document = self.training_course(2000, 2000)
        self.assertEqual(UI.training_recorded_targets(document, 0.001, 6), {1500})
        self.assertEqual(UI.training_recorded_targets(document, 0.001, 7), {1500})
        document["conversion"]["parameters"]["referenceGear"] = 7
        self.assertEqual(UI.training_recorded_targets(document, 0.001, 7), {1500})
        # Input clamps to 1500 before gear-down, not after multiplying 2000.
        self.assertEqual(UI.training_recorded_targets(document, 0.001, 6), {1312, 1313})

    def test_failure_case_is_explicit_and_rejects_feature_lab(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertIsNone(UI.training_failure_case_from_environment())
            for case in ("absent", "healthy", "renderer", "runner"):
                os.environ["GC_UI_TRAINING_FAILURE_CASE"] = case
                self.assertEqual(UI.training_failure_case_from_environment(), case)
            os.environ["GC_UI_TRAINING_FAILURE_CASE"] = "typo"
            with self.assertRaises(ValueError):
                UI.training_failure_case_from_environment()
            os.environ["GC_UI_TRAINING_FAILURE_CASE"] = "runner"
            os.environ["GC_WORKOUT_GAME_FEATURE_LAB"] = "1"
            with self.assertRaisesRegex(ValueError, "Feature Lab"):
                UI.training_failure_case_from_environment()

    def test_failure_fixture_absent_means_no_game_chart_not_a_hidden_one(self):
        for case in ("absent", "healthy", "renderer", "runner"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as directory:
                with mock.patch.dict(os.environ, {
                    "GC_UI_TRAINING_FAILURE_CASE": case,
                }, clear=True):
                    UI.prepare(Path(directory))
                path = Path(directory) / "library" / UI.ATHLETE / "config/train-perspectives.xml"
                document = UI.ET.parse(path).getroot()
                self.assertEqual(document.attrib["version"], "4")
                game_charts = document.findall(".//chart[@id='59']")
                self.assertEqual(len(game_charts), 0 if case == "absent" else 1)
                for chart in document.findall(".//chart"):
                    for dimension in ("widthFactor", "heightFactor"):
                        self.assertEqual(chart.find(f"property[@name='{dimension}']").attrib["value"], "1")

    def test_recording_parser_fails_closed_and_accepts_actual_csv_spacing(self):
        header = "secs, cad, hr, km, watts, target, virtualgear,\n"
        valid = header + "1,90,120,0.001,101,100,6,\n2,91,121,0.002,102,100,6,\n"
        rows = UI.training_recording_rows(valid)
        self.assertEqual([row["secs"] for row in rows], [1, 2])
        for broken in (
            valid.replace("2,91", "1,91"),
            valid.replace("0.002", "0.000"),
            valid.replace("102", "nan"),
            valid.replace("100,6", "100,6.5"),
            valid.replace("virtualgear", "missinggear"),
            valid.rstrip("\n"),
        ):
            with self.subTest(broken=broken), self.assertRaises(UI.UiFailure):
                UI.training_recording_rows(broken)

    def test_failure_target_oracle_uses_prescription_position_and_reference_gear(self):
        document = {
            "conversion": {"preset": "workout-first", "parameters": {"referenceGear": 6}},
            "source": {"intervals": [
                {"startMs": 0, "durationMs": 6000, "startWatts": 100, "endWatts": 100},
                {"startMs": 6000, "durationMs": 6000, "startWatts": 220, "endWatts": 100},
            ]},
        }
        self.assertEqual(UI.training_prescribed_target(document, 1000, 6), 100)
        self.assertEqual(UI.training_prescribed_target(document, 1000, 7), 114)
        self.assertEqual(UI.training_prescribed_target(document, 6000, 6), 220)
        self.assertEqual(UI.training_prescribed_target(document, 9000, 6), 160)
        with self.assertRaises(UI.UiFailure):
            UI.training_prescribed_target(document, 12000, 6)
        document["conversion"]["preset"] = "balanced"
        with self.assertRaises(UI.UiFailure):
            UI.training_prescribed_target(document, 1000, 6)

    def test_failure_target_oracle_preserves_generated_terrain_effort(self):
        document = {
            "conversion": {"preset": "workout-first", "parameters": {"referenceGear": 6}},
            "source": {"intervals": [
                {"startMs": 0, "durationMs": 6000, "startWatts": 100, "endWatts": 100},
            ]},
            "course": {"sections": [
                {"sourceStartMs": 0, "nominalDurationMs": 6000,
                 "targetStartWatts": 100, "targetEndWatts": 100,
                 "referenceEffortStartWatts": 150, "referenceEffortEndWatts": 130},
            ]},
        }
        self.assertEqual(UI.training_prescribed_target(document, 3000, 6), 140)
        self.assertEqual(UI.training_prescribed_target(document, 3000, 7), 160)
        # The dispatch log rounds nominal position to one millisecond. Only
        # values genuinely possible in that quantization bin may be accepted.
        self.assertEqual(UI.training_prescribed_targets(document, 3000, 6), {140})
        self.assertNotIn(100, UI.training_prescribed_targets(document, 3000, 6))

    def test_failure_workflow_reads_the_redirected_debug_log_without_duplicate_events(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "library").mkdir()
            artifacts = root / "artifacts"
            artifacts.mkdir()
            app_log = artifacts / "application.log"
            runtime_log = root / "library/goldencheetah.log"
            app_log.write_text(f'GoldenCheetah: redirecting log messages (stderr) to file  "{runtime_log}"\n', encoding="utf-8")
            runtime_log.write_text("gc-test-game event=frame mono_ms=100\n", encoding="utf-8")
            workflow = object.__new__(UI.TrainingFailureUiWorkflow)
            workflow.root = root
            workflow.artifacts = artifacts
            self.assertEqual(workflow.log_text(), runtime_log.read_text(encoding="utf-8"))
            app_log.write_text('redirecting log messages to file "/another/session/goldencheetah.log"\n', encoding="utf-8")
            with self.assertRaises(UI.UiFailure):
                workflow.log_text()
            # Release logging may use stderr instead; choose one authority,
            # never concatenate two copies of the same receipt stream.
            app_log.write_text("gc-test-game event=frame mono_ms=200\n", encoding="utf-8")
            self.assertEqual(workflow.log_text(), app_log.read_text(encoding="utf-8"))

    def test_dispatch_evidence_requires_a_matching_real_receiver(self):
        receiver = "gc-test-device event=load mono_ms=1500 value=100 accepted=100 gear=6\n"
        target = "workout-game-trainer-target mode=erg value=100 wind=0 workout_pos=200 devices=1\n"
        dispatches = UI.training_dispatches(receiver + target)
        self.assertEqual(len(dispatches), 1)
        self.assertEqual(dispatches[0]["gear"], 6)
        self.assertEqual(dispatches[0]["workout_pos"], 200)
        for broken in (
            target,
            receiver + target.replace("value=100", "value=110"),
            receiver.replace("accepted=100", "accepted=90") + target,
            receiver + target.replace("devices=1", "devices=0"),
            receiver + target.replace("mode=erg", "mode=slope"),
            receiver + target + target,
            receiver.replace("mono_ms=1500", "mono_ms=nan") + target,
        ):
            with self.subTest(broken=broken), self.assertRaises(UI.UiFailure):
                UI.training_dispatches(broken)

    def test_dispatch_evidence_rejects_a_different_application_process(self):
        receiver = "gc-test-device event=load mono_ms=1500 value=100 accepted=100 gear=6 pid=42\n"
        target = "workout-game-trainer-target mode=erg value=100 wind=0 workout_pos=200 devices=1\n"
        self.assertEqual(len(UI.training_dispatches(receiver + target, expected_pid=42)), 1)
        with self.assertRaises(UI.UiFailure):
            UI.training_dispatches(receiver + target, expected_pid=43)

    def test_phase_evidence_rejects_stalled_recording_or_commands(self):
        header = "secs,cad,hr,km,watts,target,virtualgear\n"
        before = header + "1,90,120,0.001,100,100,6\n"
        after = before + "2,90,120,0.002,100,100,6\n3,90,120,0.003,100,100,6\n"
        commands = [{"mono_ms": t, "value": 100, "gear": 6, "workout_pos": t}
                    for t in (1100, 2100)]
        UI.validate_training_phase(before, after, commands, active=True, gear=6,
                                   document=self.training_course())
        UI.validate_training_phase(before, before, [], active=False, gear=6,
                                   document=self.training_course())
        for recording, receipts, active, gear in (
            (before, commands, True, 6),
            (after, [], True, 6),
            (after, commands, True, 7),
            (after, commands, False, 6),
            (before, commands, False, 6),
            (after.replace("100,6", "130,6"), commands, True, 6),
        ):
            with self.subTest(active=active, gear=gear), self.assertRaises(UI.UiFailure):
                UI.validate_training_phase(before, recording, receipts, active=active, gear=gear,
                                           document=self.training_course())

    def test_active_failure_phase_requires_real_progress_and_live_telemetry(self):
        before = "secs,cad,hr,km,watts,target,virtualgear\n1,90,120,0.001,100,100,6\n"
        after = before + "2,90,120,0.002,100,100,6\n3,90,120,0.003,100,100,6\n"
        commands = [{"mono_ms": t, "value": 100, "gear": 6, "workout_pos": t}
                    for t in (1100, 2100)]
        for frozen, receipts in (
            (after, [dict(command, workout_pos=0) for command in commands]),
            (after.replace("0.002", "0.001").replace("0.003", "0.001"), commands),
            (after.replace("90,120,0.002,100", "0,0,0.002,0")
                  .replace("90,120,0.003,100", "0,0,0.003,0"), commands),
        ):
            with self.subTest(frozen=frozen), self.assertRaises(UI.UiFailure):
                UI.validate_training_phase(before, frozen, receipts, active=True, gear=6,
                                           document=self.training_course())

    def test_recorded_targets_cannot_reverse_prescribed_progression(self):
        before = "secs,cad,hr,km,watts,target,virtualgear\n1,90,120,0.001,100,100,6\n"
        commands = [{"mono_ms": t, "value": value, "gear": 6, "workout_pos": t}
                    for t, value in ((1100, 100), (2100, 200))]
        reversed_rows = before + "2,90,120,0.002,200,200,6\n3,90,120,0.003,100,100,6\n"
        with self.assertRaises(UI.UiFailure):
            UI.validate_training_phase(before, reversed_rows, commands, active=True, gear=6,
                                       document=self.training_course())

    def test_failure_workflow_exercises_pause_gears_and_stop_in_order(self):
        workflow = object.__new__(UI.TrainingFailureUiWorkflow)
        workflow.case = "runner"
        workflow.document = {"conversion": {"parameters": {"referenceGear": 6}}}
        workflow.gear = object()
        workflow.driver = mock.Mock()
        workflow.log_text = mock.Mock(return_value="")
        calls = mock.Mock()
        for name in ("open_game", "start", "wait_initial_evidence", "wait_fault_evidence",
                     "hold_phase", "toggle_pause", "stop_and_continue", "stop_save_and_reopen",
                     "finish_evidence"):
            setattr(workflow, name, getattr(calls, name))
        with tempfile.TemporaryDirectory() as directory:
            workflow.artifacts = Path(directory)
            (workflow.artifacts / "application.log").write_text("", encoding="utf-8")
            recording = Path(directory) / "recording.csv"
            calls.start.return_value = recording
            workflow.run_failure_case()
        self.assertEqual(calls.open_game.call_args, mock.call(workout_ride_expected=False, game_visible=True))
        self.assertEqual(calls.hold_phase.call_args_list, [
            mock.call("initial", recording, active=True, gear=6),
            mock.call("gear-up", recording, active=True, gear=7),
            mock.call("gear-back", recording, active=True, gear=6),
            mock.call("paused", recording, active=False, gear=6),
            mock.call("paused-gear", recording, active=False, gear=7),
            mock.call("resumed", recording, active=True, gear=7),
            mock.call("continued", recording, active=True, gear=7),
            mock.call("stopped", recording, active=False, gear=7),
        ])
        self.assertEqual(calls.toggle_pause.call_count, 2)
        calls.wait_initial_evidence.assert_called_once_with(recording)
        calls.wait_fault_evidence.assert_called_once_with()
        calls.finish_evidence.assert_called_once_with(recording)
        calls.stop_and_continue.assert_called_once_with(recording)
        calls.stop_save_and_reopen.assert_called_once_with(recording)
        self.assertLess(calls.mock_calls.index(mock.call.wait_initial_evidence(recording)),
                        calls.mock_calls.index(mock.call.wait_fault_evidence()))

    def test_failure_mode_selects_only_its_isolated_workflow(self):
        with mock.patch.dict(os.environ, {"GC_UI_TRAINING_FAILURE_CASE": "runner"}, clear=True):
            self.assertEqual(UI.selected_ui_tests_from_environment(), (
                "startup_and_main_navigation", "prepared_workout_library_import",
                "training_failure_independence", "graceful_shutdown_request",
            ))

    def test_failure_stop_confirmation_is_observed_before_continue(self):
        workflow = object.__new__(UI.TrainingFailureUiWorkflow)
        calls = mock.Mock()
        workflow.driver = calls.driver
        workflow.gear = object()
        workflow.driver.current_value.return_value = 7
        workflow.activate_stop_training = calls.stop
        workflow.activate_stop_dialog_button = calls.dialog
        workflow.hold_phase = calls.hold
        recording = Path("recording.csv")
        workflow.stop_and_continue(recording)
        calls.hold.assert_called_once_with("stop-confirmation", recording, active=False, gear=7)
        self.assertLess(calls.mock_calls.index(mock.call.stop()),
                        calls.mock_calls.index(mock.call.hold("stop-confirmation", recording, active=False, gear=7)))
        self.assertLess(calls.mock_calls.index(mock.call.hold("stop-confirmation", recording, active=False, gear=7)),
                        calls.mock_calls.index(mock.call.dialog("Continue Training")))

    def test_failure_runner_does_not_require_visual_continuity_or_disable_sandbox(self):
        source = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn('case "${GC_UI_TRAINING_FAILURE_CASE:-}" in', source)
        self.assertIn('unset QTWEBENGINE_DISABLE_SANDBOX', source)
        self.assertIn('export GC_WORKOUT_GAME_FORCE_PAINTER=0', source)
        self.assertIn('[ -z "${GC_UI_TRAINING_FAILURE_CASE:-}" ]', source)

    def test_failure_fixture_can_reuse_the_exact_generated_course(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = Path(directory) / "fixture"
            fixture.mkdir()
            (fixture / "failure-course.crs").write_bytes(b"generated course\n")
            (fixture / "failure-course.gcmtb.json").write_bytes(b'{"preserved":true}\n')
            root = Path(directory) / "new-run"
            with mock.patch.dict(os.environ, {
                "GC_UI_TRAINING_FAILURE_CASE": "runner",
                "GC_UI_TRAINING_FAILURE_FIXTURE": str(fixture),
            }, clear=True):
                UI.prepare(root)
            workouts = root / "library" / UI.ATHLETE / "workouts"
            self.assertEqual((workouts / "ui-test-mtb.crs").read_bytes(), b"generated course\n")
            self.assertEqual((workouts / "ui-test-mtb.gcmtb.json").read_bytes(), b'{"preserved":true}\n')


    def test_generated_workout_validator_accepts_monotonic_mrc(self):
        with tempfile.TemporaryDirectory() as directory:
            workout = Path(directory) / "generated.mrc"
            workout.write_text(
                "[COURSE DATA]\n"
                "0.000 55\n"
                "1.000 55\n"
                "1.000 135\n"
                "57.000 55\n"
                "[END COURSE DATA]\n",
                encoding="utf-8",
            )

            result = UI.validate_generated_workout(workout)

            self.assertEqual(result["duration_minutes"], 57.0)
            self.assertEqual(result["minimum_percent"], 55.0)
            self.assertEqual(result["maximum_percent"], 135.0)
            self.assertEqual(result["point_count"], 4)
            self.assertEqual(result["percent_values"], [55.0, 135.0])

    def test_generated_workout_validator_rejects_time_reversal(self):
        with tempfile.TemporaryDirectory() as directory:
            workout = Path(directory) / "generated.mrc"
            workout.write_text(
                "[COURSE DATA]\n0.000 55\n1.000 135\n0.500 55\n"
                "[END COURSE DATA]\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(UI.UiFailure, "not monotonic"):
                UI.validate_generated_workout(workout)

    def test_library_search_dialog_exposes_its_title_to_accessibility(self):
        source = LIBRARY_SOURCE_PATH.read_text(encoding="utf-8")

        self.assertIn(
            'const QString dialogTitle = tr("Search for Workouts, Syncs and Media");',
            source,
        )
        self.assertIn("setWindowTitle(dialogTitle);", source)
        self.assertIn("setAccessibleName(dialogTitle);", source)

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

            document["course"]["sections"][0:1] = [
                {
                    "sourceStartMs": 0,
                    "nominalDurationMs": 3000,
                    "targetStartWatts": 100.0,
                    "targetEndWatts": 100.0,
                    "gradePercent": 0.5,
                    "terrain": "roots",
                },
                {
                    "sourceStartMs": 3000,
                    "nominalDurationMs": 3000,
                    "targetStartWatts": 100.0,
                    "targetEndWatts": 100.0,
                    "gradePercent": 1.0,
                    "terrain": "smooth-trail",
                },
            ]
            sidecar.write_text(json.dumps(document), encoding="utf-8")
            subdivided = UI.validate_mtb_course_sidecar(
                sidecar, "ride-first", "ui-test MTB"
            )
            self.assertEqual(subdivided["interval_count"], 2)
            self.assertEqual(subdivided["duration_ms"], 10800)

            document["course"]["sections"][2]["targetStartWatts"] = 219.0
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

    def test_interactive_controls_open_popup_without_changing_selection(self):
        focus = mock.Mock()
        driver = object.__new__(UI.UiDriver)
        driver.find = mock.Mock(return_value=focus)
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
        driver.find_combo_item = mock.Mock()
        driver.send_named_key = mock.Mock()
        driver.name = mock.Mock(return_value="20/20 descending sets")

        with mock.patch.object(UI.time, "sleep"):
            driver.require_interactive_controls(
                [("20/20 descending sets", "combo box")], timeout=1.0
            )

        self.assertEqual(driver.name.call_count, 1)
        self.assertEqual(
            driver.send_named_key.call_args_list,
            [mock.call("Escape")],
        )
        driver.find_combo_item.assert_called_once_with(
            focus, "20/20 descending sets", 1.0
        )

    def test_interactive_checkbox_uses_accessible_action(self):
        focus = mock.Mock()
        action = mock.Mock()
        action.nActions = 1
        action.doAction.return_value = True
        checkbox = mock.Mock()
        checkbox.queryAction.return_value = action
        driver = object.__new__(UI.UiDriver)
        driver.find = mock.Mock(side_effect=[focus, checkbox, checkbox])
        driver.name = mock.Mock(return_value="Training focus")
        driver.click = mock.Mock()
        driver.find_combo_item = mock.Mock()
        driver.send_named_key = mock.Mock()
        driver.checked = mock.Mock(side_effect=[False, True, False])

        with mock.patch.object(UI.time, "sleep"):
            driver.require_interactive_controls(
                [
                    ("Training focus", "combo box"),
                    ("Recovery after the last repetition", "check box"),
                ],
                timeout=1.0,
            )

        self.assertEqual(
            driver.send_named_key.call_args_list,
            [mock.call("Escape")],
        )
        driver.click.assert_called_once_with(focus)
        self.assertEqual(action.doAction.call_args_list,
                         [mock.call(0), mock.call(0)])

    def test_combo_item_uses_keyboard_selection(self):
        combo = mock.Mock()
        item = mock.Mock()
        item.getIndexInParent.return_value = 2
        driver = object.__new__(UI.UiDriver)
        driver.combo_with_items = mock.Mock(return_value=combo)
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
        driver.find_combo_item = mock.Mock(return_value=item)
        driver.activate_popup_item = mock.Mock()
        driver.name = mock.Mock(
            side_effect=["Erg Workout", "Workout Game"]
        )

        selected = driver.select_combo_item(
            ("Workout Game", "Workout Editor"),
            "Workout Game",
        )

        self.assertIs(selected, combo)
        driver.click.assert_called_once_with(combo)
        driver.find_combo_item.assert_called_once_with(
            combo, "Workout Game", 10.0
        )
        driver.activate_popup_item.assert_called_once_with(2)

    def test_workout_generator_declares_complete_tab_order(self):
        source = WORKOUT_WIZARD_SOURCE_PATH.read_text(encoding="utf-8")
        tab_order = source[source.index("const QList<QWidget *> tabOrder") :]
        controls = (
            "focusBox", "ftpBox", "workPowerSlider", "workPowerBox",
            "recoveryPowerSlider", "recoveryPowerBox", "workSecondsBox",
            "recoverySecondsBox", "repetitionsBox", "setsBox",
            "repetitionDeltaBox", "setRecoveryBox", "finalSetRecoveryBox",
            "warmupMinutesBox", "warmupStartPowerSlider",
            "warmupStartPowerBox", "warmupEndPowerSlider",
            "warmupEndPowerBox", "primerSecondsBox", "primerPowerSlider",
            "primerPowerBox", "preWorkRecoverySecondsBox",
            "cooldownMinutesBox", "recoverAfterLastBox",
        )

        positions = [tab_order.index(control) for control in controls]
        self.assertEqual(positions, sorted(positions))
        self.assertIn("QWidget::setTabOrder", tab_order)

    def test_runner_requires_generated_distance_course_at_game_start(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")
        helper = ENVIRONMENT_HELPER_PATH.read_text(encoding="utf-8")

        self.assertIn("GC_UI_VALIDATE_MTB_COURSE", runner)
        self.assertIn("Workout Game session course: distance-course", runner)
        self.assertIn("mtb-course-runtime-evidence.txt", runner)
        self.assertIn("--cold-start-continuity-only", runner)
        self.assertIn("GC_UI_USE_HARDWARE_GL", runner)
        self.assertIn(
            "Hardware GL validation requires GC_UI_EXISTING_DISPLAY", runner
        )
        self.assertIn(
            "Hardware GL validation requires GC_UI_EXPECTED_GPU_PATTERN", runner
        )
        self.assertIn("require_unlocked_desktop_session", runner)
        self.assertIn("org.gnome.ScreenSaver.GetActive", helper)
        self.assertIn("Existing desktop session is locked", helper)

    def test_desktop_unlock_check_fails_closed(self):
        cases = (
            ("printf '(false,)\\n'", 0, ""),
            ("printf '(true,)\\n'", 1, "is locked"),
            ("printf '(unknown,)\\n'", 2, "invalid state"),
            ("return 1", 2, "Cannot verify"),
        )
        for fake_gdbus, expected_status, expected_error in cases:
            with self.subTest(fake_gdbus=fake_gdbus):
                completed = subprocess.run(
                    [
                        "bash", "-c",
                        'source "$1"; '
                        f'gdbus() {{ {fake_gdbus}; }}; '
                        "require_unlocked_desktop_session",
                        "bash", str(ENVIRONMENT_HELPER_PATH),
                    ],
                    check=False,
                    text=True,
                    capture_output=True,
                )

                self.assertEqual(completed.returncode, expected_status)
                self.assertIn(expected_error, completed.stderr)

    def test_renderer_environment_selects_a_consistent_default(self):
        cases = (
            ({}, "1", 0, ""),
            ({"GC_WORKOUT_GAME_3D": "0"}, "1", 0, ""),
            ({"GC_WORKOUT_GAME_3D": "1"}, "0", 0, ""),
            ({
                "GC_WORKOUT_GAME_3D": "0",
                "GC_WORKOUT_GAME_FORCE_PAINTER": "0",
            }, "0", 0, ""),
            ({
                "GC_WORKOUT_GAME_3D": "1",
                "GC_WORKOUT_GAME_FORCE_PAINTER": "1",
            }, "", 2, "cannot force the Painter renderer"),
            ({"GC_WORKOUT_GAME_3D": "quick"}, "", 2,
             "GC_WORKOUT_GAME_3D must be 0 or 1"),
            ({"GC_WORKOUT_GAME_FORCE_PAINTER": "quick"}, "", 2,
             "GC_WORKOUT_GAME_FORCE_PAINTER must be 0 or 1"),
        )
        for variables, expected, status, error in cases:
            with self.subTest(variables=variables):
                environment = dict(os.environ)
                environment.pop("GC_WORKOUT_GAME_3D", None)
                environment.pop("GC_WORKOUT_GAME_FORCE_PAINTER", None)
                environment.update(variables)
                completed = subprocess.run(
                    [
                        "bash", "-c",
                        'source "$1"; '
                        "configure_ui_test_workout_game_renderer; "
                        'status=$?; [ "$status" -eq 0 ] && '
                        'printf "%s" "$GC_WORKOUT_GAME_FORCE_PAINTER"; '
                        'exit "$status"',
                        "bash", str(ENVIRONMENT_HELPER_PATH),
                    ],
                    env=environment,
                    check=False,
                    text=True,
                    capture_output=True,
                )

                self.assertEqual(completed.returncode, status)
                self.assertEqual(completed.stdout, expected)
                self.assertIn(error, completed.stderr)

    def test_xvfb_recursion_drops_the_callers_runtime_before_at_spi(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn("-u DBUS_SESSION_BUS_ADDRESS -u XDG_RUNTIME_DIR", runner)
        self.assertLess(
            runner.index('configure_ui_test_xdg_environment "$TEST_ROOT/home"'),
            runner.index("AT_SPI_REPLY=$(gdbus call"),
        )

    def test_xvfb_environment_uses_an_isolated_runtime_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory) / "home"
            environment = dict(os.environ)
            environment.pop("DBUS_SESSION_BUS_ADDRESS", None)
            environment.pop("XDG_RUNTIME_DIR", None)
            completed = subprocess.run(
                [
                    "bash", "-c",
                    'source "$1"; '
                    'capture_ui_test_session_environment ""; '
                    'configure_ui_test_xdg_environment "$2"; '
                    'printf "%s|%s" "$XDG_RUNTIME_DIR" '
                    '"$(stat -Lc %a -- "$XDG_RUNTIME_DIR")"',
                    "bash", str(ENVIRONMENT_HELPER_PATH), str(home),
                ],
                env=environment,
                check=False,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, f"{home / '.runtime'}|700")
            for relative in (
                ".config", ".cache", ".local/share", ".local/state"
            ):
                self.assertTrue((home / relative).is_dir())

    def test_existing_display_preserves_a_valid_session_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "desktop-runtime"
            home = root / "home"
            runtime.mkdir(mode=0o700)
            marker = runtime / "session-socket-placeholder"
            marker.write_text("keep\n", encoding="ascii")
            environment = dict(
                os.environ,
                DBUS_SESSION_BUS_ADDRESS="unix:path=/test/session-bus",
                XDG_RUNTIME_DIR=str(runtime),
            )
            completed = subprocess.run(
                [
                    "bash", "-c",
                    'source "$1"; '
                    'capture_ui_test_session_environment ":1"; '
                    'configure_ui_test_xdg_environment "$2"; '
                    'printf "%s|%s" "$XDG_RUNTIME_DIR" '
                    '"$(stat -Lc %a -- "$XDG_RUNTIME_DIR")"',
                    "bash", str(ENVIRONMENT_HELPER_PATH), str(home),
                ],
                env=environment,
                check=False,
                text=True,
                capture_output=True,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, f"{runtime}|700")
            self.assertEqual(marker.read_text(encoding="ascii"), "keep\n")

    def test_existing_display_rejects_invalid_session_runtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            wrong_mode = root / "wrong-mode"
            wrong_mode.mkdir(mode=0o755)
            cases = (
                ("", "unix:path=/test/session-bus", "absolute"),
                ("relative", "unix:path=/test/session-bus", "absolute"),
                (str(root / "missing"), "unix:path=/test/session-bus",
                 "not a directory"),
                (str(wrong_mode), "unix:path=/test/session-bus", "mode 0700"),
                (str(root), "", "desktop D-Bus session"),
            )
            for runtime, bus, expected_error in cases:
                with self.subTest(runtime=runtime, bus=bus):
                    environment = dict(os.environ, XDG_RUNTIME_DIR=runtime)
                    if bus:
                        environment["DBUS_SESSION_BUS_ADDRESS"] = bus
                    else:
                        environment.pop("DBUS_SESSION_BUS_ADDRESS", None)
                    completed = subprocess.run(
                        [
                            "bash", "-c",
                            'source "$1"; '
                            'capture_ui_test_session_environment ":1"',
                            "bash", str(ENVIRONMENT_HELPER_PATH),
                        ],
                        env=environment,
                        check=False,
                        text=True,
                        capture_output=True,
                    )

                    self.assertEqual(completed.returncode, 2)
                    self.assertIn(expected_error, completed.stderr)

        helper = ENVIRONMENT_HELPER_PATH.read_text(encoding="utf-8")
        self.assertIn('[ -O "$runtime_dir" ]', helper)

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
                    f"0||1|1|1|1|{artifacts / 'quick3d'}",
                ],
            )

    def test_runner_updates_dbus_activation_environment_after_display(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        configured_xdg = runner.index(
            'configure_ui_test_xdg_environment "$TEST_ROOT/home"'
        )
        existing_display_branch = runner.index(
            'if [ -n "${GC_UI_EXISTING_DISPLAY:-}" ]; then', configured_xdg
        )
        xvfb_branch = runner.index("\nelse\n", existing_display_branch)
        export_display = runner.index("export DISPLAY=:$DISPLAY_NUMBER")
        readiness_gate = runner.index('echo "Xvfb did not become ready"')
        update_dbus = runner.index(
            "dbus-update-activation-environment DISPLAY"
        )
        xvfb_branch_end = runner.index("\nfi\n", update_dbus)
        start_at_spi = runner.index("AT_SPI_REPLY=$(gdbus call")

        self.assertEqual(
            runner.count("dbus-update-activation-environment DISPLAY"), 1
        )
        self.assertLess(xvfb_branch, export_display)
        self.assertLess(export_display, update_dbus)
        self.assertLess(readiness_gate, update_dbus)
        self.assertLess(update_dbus, xvfb_branch_end)
        self.assertLess(update_dbus, start_at_spi)
        self.assertIn(
            "REQUIRED_COMMANDS+=(Xvfb dbus-update-activation-environment)",
            runner,
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
            "Ride first", showing=True, timeout=30.0
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
            pre_start_stop = object()
            live_stop = object()
            driver = mock.Mock()
            driver.find.return_value = start_button
            driver.find_enabled.return_value = live_stop
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
            workflow.stop_training_button = pre_start_stop
            workflow.stop_training_scope = "train toolbar"
            workflow.capture_screenshots = False

            result = workflow.start()

            self.assertEqual(result, recording)
            driver.find_named_any.assert_not_called()
            driver.name.assert_not_called()
            self.assertIs(workflow.canvas, stale_canvas)
            self.assertIs(workflow.stop_training_button, live_stop)
            driver.find_enabled.assert_called_once_with(
                "Stop training", "push button", showing=True, timeout=10.0,
                scope="train toolbar",
            )
            self.assertEqual(
                (root / UI.RENDERER_CANVAS_NAME_FILE).read_text(
                    encoding="utf-8"
                ),
                "Workout game 3D canvas\n",
            )

    def test_preset_smoke_preserves_standard_erg_and_discards_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            cancel = object()
            driver = mock.Mock()
            driver.find.return_value = cancel
            driver.current_value.return_value = 6.0
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.gear = object()
            workflow.capture_screenshots = False
            workflow.open_game = mock.Mock()
            workflow.start = mock.Mock(return_value=recording)
            workflow.activate_stop_training = mock.Mock()

            with mock.patch.object(UI.time, "sleep"):
                workflow.run_smoke_and_discard("balanced")

            workflow.open_game.assert_called_once_with(
                workout_ride_expected=False
            )
            driver.send_key.assert_not_called()
            workflow.start.assert_called_once_with("04-mtb-course-balanced-first")
            driver.wait_file_growth.assert_called_once_with(
                recording, recording.stat().st_size
            )
            workflow.activate_stop_training.assert_called_once_with()
            driver.find.assert_called_once_with(
                "Cancel", "push button", showing=True, timeout=30.0
            )
            driver.activate.assert_called_once_with(cancel)
            driver.wait_file_removed.assert_called_once_with(recording)

    def test_ride_first_smoke_requires_workout_ride_to_be_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            recording = Path(directory) / "recording.csv"
            recording.write_text("secs,watts\n0,190\n", encoding="ascii")
            driver = mock.Mock()
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.gear = object()
            workflow.capture_screenshots = False
            workflow.open_game = mock.Mock()
            workflow.start = mock.Mock(return_value=recording)
            workflow.activate_stop_training = mock.Mock()

            with mock.patch.object(UI.time, "sleep"):
                workflow.run_smoke_and_discard("ride-first")

            workflow.open_game.assert_called_once_with(
                workout_ride_expected=False
            )
            driver.send_key.assert_not_called()

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
            resumed_stop = object()
            continue_button = object()
            driver = mock.Mock()
            driver.find.side_effect = [continue_button]
            driver.find_enabled.return_value = resumed_stop
            workflow = object.__new__(UI.WorkoutGameUiWorkflow)
            workflow.driver = driver
            workflow.capture_screenshots = False
            workflow.run_delays = (0.0, 0.0, 0.0)
            workflow.stop_training_button = stop
            workflow.stop_training_scope = "train toolbar"

            with mock.patch.object(UI.time, "sleep"):
                workflow.stop_and_continue(recording)

            self.assertEqual(
                driver.find.call_args_list,
                [
                    mock.call(
                        "Continue Training",
                        "push button",
                        showing=True,
                        timeout=30.0,
                    ),
                ],
            )
            driver.activate.assert_not_called()
            self.assertEqual(
                driver.click.call_args_list,
                [mock.call(stop), mock.call(continue_button)],
            )
            driver.find_enabled.assert_called_once_with(
                "Stop training", "push button",
                showing=True, timeout=10.0, scope="train toolbar",
            )
            self.assertIs(workflow.stop_training_button, resumed_stop)
            driver.wait_file_growth.assert_called_once_with(
                recording, recording.stat().st_size
            )

    def test_reopened_stop_dialog_accepts_stale_atspi_showing_state(self):
        button = object()
        driver = mock.Mock()
        driver.find.side_effect = [UI.UiFailure("stale state"), button]
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver

        workflow.activate_stop_dialog_button("Save")

        self.assertEqual(
            driver.find.call_args_list,
            [
                mock.call(
                    "Save", "push button", showing=True, timeout=30.0
                ),
                mock.call("Save", "push button", timeout=1.0),
            ],
        )
        self.assertEqual(
            driver.send_named_key.call_args_list,
            [mock.call("Tab"), mock.call("Tab"), mock.call("Return")],
        )
        driver.activate.assert_not_called()

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
            ready = object()
            save = object()
            finish = object()
            driver = mock.Mock()
            driver.find.side_effect = [
                ready,
                save,
                UI.UiFailure("stop control removed"),
                finish,
            ]
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
                driver.find.call_args_list[0],
                mock.call(
                    "Continue Training",
                    "push button",
                    showing=True,
                    timeout=30.0,
                ),
            )
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

    def test_all_nodes_skips_a_stale_accessible_without_descending(self):
        stale = mock.MagicMock()
        type(stale).name = mock.PropertyMock(
            side_effect=RuntimeError("stale accessible")
        )
        driver = object.__new__(UI.UiDriver)

        self.assertEqual(list(driver.all_nodes(stale)), [])

        stale.__iter__.assert_not_called()

    def test_pruned_canvas_path_never_queries_a_stale_name(self):
        canvas = mock.MagicMock()
        canvas.path = "/org/a11y/atspi/accessible/42"
        driver = object.__new__(UI.UiDriver)
        driver.pruned_accessible_paths = {}
        driver.prune_descendants(canvas, "Workout game 3D canvas")
        type(canvas).name = mock.PropertyMock(
            side_effect=AssertionError("stale name must not be queried")
        )

        self.assertEqual(list(driver.all_nodes(canvas)), [canvas])

        canvas.__iter__.assert_not_called()

    def test_find_enabled_can_limit_search_to_a_toolbar(self):
        toolbar = object()
        stop = object()
        unrelated = object()
        driver = object.__new__(UI.UiDriver)
        driver.nodes_with_names = mock.Mock(
            return_value=iter(
                (
                    (toolbar, "Train toolbar"),
                    (unrelated, "Start or pause training"),
                    (stop, "Stop training"),
                )
            )
        )
        driver.role = mock.Mock(return_value="push button")
        driver.showing = mock.Mock(return_value=True)
        driver.enabled = mock.Mock(side_effect=lambda node: node is stop)
        driver.find_all = mock.Mock(
            side_effect=AssertionError("global tree must not be scanned")
        )

        result = driver.find_enabled(
            "Stop training", "push button", scope=toolbar
        )

        self.assertIs(result, stop)
        driver.nodes_with_names.assert_called_once_with(toolbar)
        driver.find_all.assert_not_called()

    def test_stop_training_resolves_missing_button_and_invalidates_it(self):
        replacement = object()
        driver = mock.Mock()
        driver.find_enabled.return_value = replacement
        workflow = object.__new__(UI.WorkoutGameUiWorkflow)
        workflow.driver = driver
        workflow.stop_training_button = None

        workflow.activate_stop_training()

        driver.find_enabled.assert_called_once_with(
            "Stop training", "push button", showing=True, timeout=10.0
        )
        driver.click.assert_called_once_with(replacement)
        driver.activate.assert_not_called()
        self.assertIsNone(workflow.stop_training_button)

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

    def test_activate_refreshes_a_stale_accessible_control_once(self):
        stale = object()
        replacement = object()
        driver = object.__new__(UI.UiDriver)
        driver._accessible_metadata = mock.Mock(
            return_value=("Start or pause training", "push button", True)
        )
        driver._activate_once = mock.Mock(
            side_effect=[UI.UiFailure("stale"), None]
        )
        driver.refresh_accessible = mock.Mock(return_value=replacement)

        driver.activate(stale)

        self.assertEqual(
            driver._activate_once.call_args_list,
            [mock.call(stale), mock.call(replacement)],
        )
        driver.refresh_accessible.assert_called_once_with(
            stale, "Start or pause training", "push button", True
        )

    def test_activate_retries_transient_stale_metadata_once(self):
        control = mock.Mock()
        type(control).name = mock.PropertyMock(
            side_effect=[RuntimeError("stale"), "Start or pause training"]
        )
        control.getRoleName.return_value = "push button"
        control.getState.return_value.contains.return_value = True
        driver = object.__new__(UI.UiDriver)
        driver.pyatspi = mock.Mock(STATE_SHOWING=1)
        driver._activate_once = mock.Mock()

        with mock.patch.object(UI.time, "sleep") as sleep:
            driver.activate(control)

        sleep.assert_called_once_with(0.05)
        driver._activate_once.assert_called_once_with(control)

    def test_click_refreshes_stale_accessible_bounds_once(self):
        stale = object()
        replacement = object()
        driver = object.__new__(UI.UiDriver)
        driver._accessible_metadata = mock.Mock(
            return_value=("Save", "push button", True)
        )
        driver._mouse_click_once = mock.Mock(
            side_effect=[UI.UiFailure("stale"), None]
        )
        driver.refresh_accessible = mock.Mock(return_value=replacement)

        driver.click(stale)

        self.assertEqual(
            driver._mouse_click_once.call_args_list,
            [mock.call(stale, 1), mock.call(replacement, 1)],
        )
        driver.refresh_accessible.assert_called_once_with(
            stale, "Save", "push button", True
        )

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
