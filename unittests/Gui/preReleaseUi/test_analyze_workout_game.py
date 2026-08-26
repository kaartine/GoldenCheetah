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
        driver.click = mock.Mock()
        driver.activate = mock.Mock()
        driver.find_combo_item = mock.Mock(return_value=item)
        driver.name = mock.Mock(return_value="Workout Game")
        driver.selected = mock.Mock(return_value=True)

        selected = driver.select_combo_item(
            ["Workout Game", "Workout Editor"],
            "Workout Editor",
            timeout=0.01,
        )

        self.assertIs(selected, combo)
        driver.click.assert_called_once_with(combo)
        driver.activate.assert_called_once_with(item)

    def test_combo_selection_uses_its_own_item_instead_of_global_duplicate(self):
        combo = object()
        own_item = object()
        driver = object.__new__(UI.UiDriver)
        driver.combo_with_items = mock.Mock(return_value=combo)
        driver.click = mock.Mock()
        driver.activate = mock.Mock()
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
        driver.click.assert_called_once_with(combo)
        driver.activate.assert_called_once_with(own_item)
        driver.find.assert_not_called()

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

    def test_frame_delta_rejects_dimension_change(self):
        with self.assertRaises(UI.UiFailure):
            UI.UiDriver.changed_pixels((1, 1, b"\0\0\0"), (2, 1, b"\0" * 6))

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
