#!/usr/bin/env python3

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("analyze_workout_game.py")
SPEC = importlib.util.spec_from_file_location("analyze_workout_game", MODULE_PATH)
ANALYZER = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(ANALYZER)

UI_MODULE_PATH = Path(__file__).with_name("pre_release_ui.py")
UI_SPEC = importlib.util.spec_from_file_location("pre_release_ui", UI_MODULE_PATH)
UI = importlib.util.module_from_spec(UI_SPEC)
assert UI_SPEC.loader is not None
UI_SPEC.loader.exec_module(UI)


class AnalyzeWorkoutGameTest(unittest.TestCase):
    def test_native_quick_3d_canvas_uses_trace_for_motion_gate(self):
        self.assertFalse(
            UI.canvas_requires_pixel_motion("Workout game 3D canvas")
        )
        self.assertTrue(
            UI.canvas_requires_pixel_motion("Workout game canvas")
        )

    def test_find_named_any_accepts_the_quick_3d_canvas(self):
        quick_3d_canvas = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(
            side_effect=lambda name, role, showing: (
                [quick_3d_canvas]
                if name == "Workout game 3D canvas" else []
            )
        )

        found = driver.find_named_any(
            UI.WORKOUT_GAME_CANVAS_NAMES, showing=True, timeout=0.01
        )

        self.assertIs(found, quick_3d_canvas)

    def test_find_named_any_reports_all_missing_names(self):
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[])

        with self.assertRaisesRegex(
            UI.UiFailure,
            "Workout game canvas.*Workout game 3D canvas",
        ):
            driver.find_named_any(
                UI.WORKOUT_GAME_CANVAS_NAMES,
                showing=True,
                timeout=0.01,
            )

    def test_combo_selection_accepts_selected_item_when_name_is_stale(self):
        combo = object()
        item = object()
        driver = object.__new__(UI.UiDriver)
        driver.combo_with_items = mock.Mock(return_value=combo)
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
        driver.find_combo_item = mock.Mock(return_value=item)
        driver.name = mock.Mock(
            side_effect=lambda node: (
                "Workout Editor" if node is item else "Workout Game"
            )
        )
        driver.selected = mock.Mock(return_value=True)

        selected = driver.select_combo_item(
            ["Workout Game", "Workout Editor"],
            "Workout Editor",
            timeout=0.01,
        )

        self.assertIs(selected, combo)
        self.assertEqual(
            driver.click.call_args_list,
            [mock.call(combo), mock.call(item)],
        )
        driver.focus_main_window.assert_called_once_with()

    def test_combo_selection_uses_its_own_item_instead_of_global_duplicate(self):
        combo = object()
        own_item = object()
        driver = object.__new__(UI.UiDriver)
        driver.combo_with_items = mock.Mock(return_value=combo)
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
        driver.find = mock.Mock(
            side_effect=AssertionError("global item lookup is ambiguous")
        )
        driver.all_nodes = mock.Mock(return_value=[combo, own_item])
        driver.role = mock.Mock(
            side_effect=lambda node: "list item" if node is own_item else "combo box"
        )
        driver.name = mock.Mock(
            side_effect=lambda node: (
                "Workout Editor" if node is own_item else "Workout Game"
            )
        )
        driver.showing = mock.Mock(return_value=True)
        driver.selected = mock.Mock(
            side_effect=lambda node: node is own_item
        )

        selected = driver.select_combo_item(
            ["Workout Game", "Workout Editor"],
            "Workout Editor",
            timeout=0.01,
        )

        self.assertIs(selected, combo)
        self.assertEqual(
            driver.click.call_args_list,
            [mock.call(combo), mock.call(own_item)],
        )
        driver.focus_main_window.assert_called_once_with()
        driver.find.assert_not_called()

    def test_combo_with_items_ignores_hidden_stale_selectors(self):
        hidden_combo = object()
        visible_combo = object()
        game = object()
        editor = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[hidden_combo, visible_combo])
        driver.all_nodes = mock.Mock(return_value=[game, editor])
        driver.role = mock.Mock(return_value="list item")
        driver.name = mock.Mock(
            side_effect=lambda node: (
                "Workout Game" if node is game else "Workout Editor"
            )
        )
        driver.showing = mock.Mock(
            side_effect=lambda node: node is visible_combo
        )
        driver.enabled = mock.Mock(return_value=True)

        selected = driver.combo_with_items(
            ["Workout Game", "Workout Editor"],
            timeout=0.01,
            require_interactable=True,
        )

        self.assertIs(selected, visible_combo)

    def test_prepare_anchors_a_usable_workout_library(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            UI.prepare(root)
            workout_directory = (
                root / "library" / UI.ATHLETE / "workouts"
            )
            settings = (
                root / "library" / "configglobal-general.ini"
            ).read_text(encoding="utf-8")
            workout = (workout_directory / "ui-test.erg").read_text(
                encoding="utf-8"
            )

            self.assertIn(f"workoutDir={workout_directory}\n", settings)
            self.assertIn("FTP = 190\n", workout)
            self.assertIn("0.10 100\n0.10 220\n", workout)
            self.assertIn("1.00 100\n", workout)
            self.assertIn("2.00 100\n", workout)

    def test_prepare_selects_a_validated_generator_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.dict(
                os.environ, {"GC_UI_GENERATOR_MODE": "under-target"}
            ):
                UI.prepare(root)

            settings = (
                root / "library" / "configglobal-trainmode.ini"
            ).read_text(encoding="utf-8")
            self.assertIn("deviceprof1=under-target\n", settings)

    def test_prepare_rejects_an_unknown_generator_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(
                os.environ, {"GC_UI_GENERATOR_MODE": "unreliable"}
            ):
                with self.assertRaisesRegex(
                    ValueError, "Unsupported GC_UI_GENERATOR_MODE"
                ):
                    UI.prepare(Path(directory))

    def test_recording_file_waits_verify_lifecycle(self):
        driver = object.__new__(UI.UiDriver)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous = root / "previous.csv"
            previous.write_text("old\n", encoding="utf-8")
            existing = set(root.glob("*.csv"))

            recording = root / "recording.csv"
            recording.write_text("header\n", encoding="utf-8")
            self.assertEqual(
                driver.wait_new_file(root, existing, "*.csv", timeout=0.01),
                recording,
            )

            initial_size = recording.stat().st_size
            with recording.open("a", encoding="utf-8") as stream:
                stream.write("sample\n")
            driver.wait_file_growth(recording, initial_size, timeout=0.01)

            recording.unlink()
            driver.wait_file_removed(recording, timeout=0.01)

    def test_game_duration_is_bounded_and_configurable(self):
        with mock.patch.dict(
            os.environ, {"GC_UI_GAME_RUN_SECONDS": "72.5"}
        ):
            self.assertEqual(UI.game_run_seconds_from_environment(), 72.5)
        for value in ("invalid", "0.5", "121"):
            with self.subTest(value=value), mock.patch.dict(
                os.environ, {"GC_UI_GAME_RUN_SECONDS": value}
            ):
                with self.assertRaises(ValueError):
                    UI.game_run_seconds_from_environment()

    def test_save_as_gate_can_be_split_from_renderer_gate(self):
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "1"}):
            self.assertTrue(UI.skip_save_as_from_environment())
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "0"}):
            self.assertFalse(UI.skip_save_as_from_environment())
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "yes"}):
            with self.assertRaisesRegex(ValueError, "must be 0 or 1"):
                UI.skip_save_as_from_environment()

    def test_frame_delta_ignores_header_and_counts_game_pixels(self):
        width = 4
        height = 4
        first = bytes(width * height * 3)
        second = bytearray(first)
        second[0:3] = b"\xff\xff\xff"
        second[((2 * width + 1) * 3):((2 * width + 2) * 3)] = b"\xff\xff\xff"
        self.assertEqual(
            UI.UiDriver.changed_pixels(
                (width, height, first),
                (width, height, bytes(second)),
                top_ratio=0.5,
                bottom_ratio=1.0,
                side_ratio=0.5,
                sample_step=1,
            ),
            1,
        )

    def test_frame_delta_compares_common_area_after_dimension_change(self):
        self.assertEqual(
            UI.UiDriver.changed_pixels(
                (4, 1, b"\0" * 12),
                (5, 1, b"\xff\xff\xff" + b"\0" * 12),
                top_ratio=0.0,
                bottom_ratio=1.0,
                side_ratio=0.25,
                sample_step=1,
            ),
            1,
        )

    def test_frame_delta_ignores_center_rider_animation(self):
        width = 10
        height = 10
        first = bytes(width * height * 3)
        second = bytearray(first)
        for y in range(2, 9):
            for x in range(3, 7):
                offset = (y * width + x) * 3
                second[offset:offset + 3] = b"\xff\xff\xff"
        self.assertEqual(
            UI.UiDriver.changed_pixels(
                (width, height, first),
                (width, height, bytes(second)),
                sample_step=1,
            ),
            0,
        )

    def test_accepts_smooth_forward_trace(self):
        samples = [
            {
                "frame_ms": 16,
                "fps": 59.4 + (index % 3),
                "p95_frame_ms": 18,
                "max_frame_ms": 24,
                "render_road_m": index * 0.5,
                "backwards": 0,
                "skipped_ticks": 0,
                "target_watts": 220,
                "unexpected_airborne_frames": 0,
            }
            for index in range(10)
        ]
        summary = ANALYZER.analyze(samples)
        self.assertEqual(
            ANALYZER.validate(
                summary, 8, 25.0, 45.0, 150.0, 1.0, 4, 200.0, 0, 1.0
            ), []
        )

    def test_reported_p95_uses_stable_tail_but_keeps_worst_stall(self):
        samples = [
            {
                "frame_ms": 16,
                "fps": 60,
                "p95_frame_ms": p95,
                "max_frame_ms": stall,
                "render_road_m": index,
            }
            for index, (p95, stall) in enumerate(
                [(70, 90), (65, 100)] + [(18, 24)] * 8
            )
        ]

        summary = ANALYZER.analyze(samples)

        self.assertEqual(summary["reported_p95_frame_ms"], 18)
        self.assertEqual(summary["reported_max_frame_ms"], 100)

    def test_rejects_regression_and_pacing_failure(self):
        samples = [
            {
                "frame_ms": 80,
                "fps": 12,
                "p95_frame_ms": 90,
                "max_frame_ms": 220,
                "render_road_m": distance,
                "backwards": 1,
                "skipped_ticks": 2,
                "target_watts": 100,
                "unexpected_airborne_frames": 3,
            }
            for distance in (0.0, 1.0, 0.5, 0.6)
        ]
        failures = ANALYZER.validate(
            ANALYZER.analyze(samples),
            4, 25.0, 45.0, 150.0, 0.5, 4, 200.0, 0, 1.0,
        )
        self.assertGreaterEqual(len(failures), 4)

    def test_parses_trace_fields_from_qt_log_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[debug] workout-game-trace frame=9 frame_ms=17 "
                "fps=58.7 render_road_m=12.5 p95_frame_ms=19 "
                "max_frame_ms=31 backwards=0 skipped_ticks=0\n",
                encoding="utf-8",
            )
            samples = ANALYZER.parse_trace(path)
            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]["frame"], 9)
            self.assertEqual(samples[0]["fps"], 58.7)
            self.assertEqual(samples[0]["p95_frame_ms"], 19)
            self.assertEqual(samples[0]["max_frame_ms"], 31)

    def test_parses_quick_3d_trace_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[info] workout-game-3d-trace frame=12 frame_ms=16 "
                "fps=61.2 render_road_m=18.5 target_watts=220 "
                "lateral_m=0.2 unexpected_airborne_frames=0 "
                "feature_phase=recovery feature_outcome=completed "
                "route=main feature_geometry=jump\n",
                encoding="utf-8",
            )

            samples = ANALYZER.parse_trace(path)

            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]["frame"], 12)
            self.assertEqual(samples[0]["target_watts"], 220)
            self.assertEqual(samples[0]["lateral_m"], 0.2)
            self.assertEqual(samples[0]["feature_phase"], "recovery")
            self.assertEqual(samples[0]["feature_outcome"], "completed")
            self.assertEqual(samples[0]["route"], "main")

    def test_malformed_numeric_trace_field_does_not_break_analysis(self):
        samples = [{
            "frame_ms": "unavailable",
            "fps": 60.0,
            "render_road_m": 12.0,
            "feature_phase": "approach",
        }]

        summary = ANALYZER.analyze(samples)

        self.assertEqual(summary["samples"], 1)
        self.assertEqual(summary["median_fps"], 60.0)
        self.assertEqual(summary["median_frame_ms"], 0.0)

    def test_parses_anonymous_trainer_target_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[info] workout-game-trainer-target mode=slope value=4.25 "
                "wind=0.31 workout_pos=125.5 devices=1\n",
                encoding="utf-8",
            )

            targets = ANALYZER.parse_trainer_targets(path)

            self.assertEqual(targets, [{
                "mode": "slope",
                "value": 4.25,
                "wind": 0.31,
                "workout_pos": 125.5,
                "devices": 1.0,
            }])

    def test_reconciles_trace_trainer_target_and_recording(self):
        trace = [
            {
                "source_ms": float(index * 1000),
                "watts": 210.0 + index,
                "cadence": 84.0 + index,
                "hr": 140.0 + index,
                "gear": 7.0,
                "action_id": 19.0,
                "feature_outcome": "completed",
                "feature_phase": "recovery",
                "route": "main",
                "readiness": 1.0,
                "feature_terrain": "bunny-hop",
            }
            for index in range(3)
        ]
        recording = [
            {
                "secs": float(index),
                "cad": 84.0 + index,
                "hr": 140.0 + index,
                "km": index * 0.01,
                "watts": 210.0 + index,
                "slope": 4.0 + index,
                "target": 220.0,
                "virtualgear": 7.0,
            }
            for index in range(3)
        ]
        targets = [
            {
                "mode": "slope",
                "value": 4.0 + index,
                "workout_pos": index * 10.0,
                "devices": 1.0,
            }
            for index in range(3)
        ]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)

        self.assertEqual(summary["matched_recording_samples"], 3)
        self.assertEqual(summary["trainer_target_dispatches"], 3)
        self.assertEqual(summary["feature_decisions"], 1)
        self.assertEqual(
            ANALYZER.validate_acceptance(
                summary,
                minimum_recording_matches=3,
                minimum_recording_match_ratio=1.0,
                maximum_power_delta_watts=1.0,
                maximum_cadence_delta_rpm=1.0,
                maximum_heart_rate_delta_bpm=1.0,
                maximum_gear_mismatches=0,
                maximum_trainer_target_delta=0.01,
                minimum_trainer_target_dispatches=3,
                minimum_feature_decisions=1,
            ),
            [],
        )

    def test_rejects_recording_and_feature_outcome_disagreement(self):
        trace = [{
            "source_ms": 1000.0,
            "watts": 250.0,
            "cadence": 90.0,
            "hr": 150.0,
            "gear": 8.0,
            "action_id": 20.0,
            "feature_outcome": "completed",
            "feature_phase": "recovery",
            "route": "bypass",
            "readiness": 0.4,
            "feature_terrain": "tabletop",
        }]
        recording = [{
            "secs": 1.0,
            "cad": 70.0,
            "hr": 120.0,
            "km": 0.02,
            "watts": 180.0,
            "slope": 2.0,
            "target": 200.0,
            "virtualgear": 3.0,
        }]
        targets = [{
            "mode": "slope",
            "value": 8.0,
            "workout_pos": 20.0,
            "devices": 1.0,
        }]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)
        failures = ANALYZER.validate_acceptance(
            summary,
            minimum_recording_matches=1,
            minimum_recording_match_ratio=1.0,
            maximum_power_delta_watts=5.0,
            maximum_cadence_delta_rpm=5.0,
            maximum_heart_rate_delta_bpm=5.0,
            maximum_gear_mismatches=0,
            maximum_trainer_target_delta=0.1,
            minimum_trainer_target_dispatches=1,
            minimum_feature_decisions=1,
        )

        self.assertTrue(any("power" in failure for failure in failures))
        self.assertTrue(any("trainer target" in failure for failure in failures))
        self.assertTrue(any("feature decision" in failure for failure in failures))

    def test_erg_target_is_aligned_by_workout_time(self):
        recording = [{
            "secs": 2.0,
            "cad": 80.0,
            "hr": 140.0,
            "km": 0.01,
            "watts": 220.0,
            "slope": 0.0,
            "target": 225.0,
            "virtualgear": 5.0,
        }]
        targets = [{
            "mode": "erg",
            "value": 225.0,
            "workout_pos": 2000.0,
            "devices": 1.0,
        }]

        summary = ANALYZER.reconcile_acceptance([], targets, recording)

        self.assertEqual(summary["maximum_trainer_target_delta"], 0.0)

    def test_missed_climb_keeps_main_route_without_inconsistency(self):
        trace = [{
            "action_id": 21.0,
            "feature_outcome": "bypassed",
            "feature_terrain": "climb",
            "route": "main",
            "readiness": 0.6,
        }]

        summary = ANALYZER.reconcile_acceptance(trace, [], [])

        self.assertEqual(summary["feature_decisions"], 1)
        self.assertEqual(summary["inconsistent_feature_decisions"], 0)

    def test_recording_parser_accepts_spaced_header_and_trailing_field(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.csv"
            path.write_text(
                "secs, cad, hr, km, watts, slope, target, virtualgear\n"
                "1,80,140,0.01,200,3,210,5,\n",
                encoding="utf-8",
            )

            samples = ANALYZER.parse_recording(path)

            self.assertEqual(samples[0]["secs"], 1.0)
            self.assertEqual(samples[0]["virtualgear"], 5.0)

    def test_recording_parser_rejects_non_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.csv"
            path.write_text(
                "secs,cad,hr,km,kph,watts,slope,target,virtualgear\n"
                "2,80,140,0.01,20,200,3,210,5\n"
                "1,81,141,0.02,21,201,4,211,5\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "strictly increasing"):
                ANALYZER.parse_recording(path)

    def test_accepts_fractional_target_near_test_ftp(self):
        summary = ANALYZER.analyze(
            [{
                "frame_ms": 10,
                "fps": 100,
                "p95_frame_ms": 12,
                "max_frame_ms": 14,
                "render_road_m": index,
                "backwards": 0,
                "skipped_ticks": 0,
                "target_watts": 199.5,
                "unexpected_airborne_frames": 0,
            } for index in range(8)]
        )
        self.assertEqual(
            ANALYZER.validate(
                summary, 8, 25.0, 45.0, 150.0, 1.0, 4, 190.0, 0, 1.0
            ),
            [],
        )

    def test_rejects_lateral_teleport(self):
        samples = [
            {
                "frame_ms": 10,
                "fps": 100,
                "render_road_m": index,
                "target_watts": 220,
                "lateral_m": lateral,
            }
            for index, lateral in enumerate((0.0, 0.1, 0.25, 2.2))
        ]
        failures = ANALYZER.validate(
            ANALYZER.analyze(samples),
            4, 25.0, 45.0, 150.0, 1.0, 4, 190.0, 0, 1.0,
        )
        self.assertTrue(any("lateral route offset" in item for item in failures))


if __name__ == "__main__":
    unittest.main()
