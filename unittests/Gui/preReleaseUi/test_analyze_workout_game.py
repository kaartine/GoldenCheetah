#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import tempfile
import unittest


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
            }
            for index in range(10)
        ]
        summary = ANALYZER.analyze(samples)
        self.assertEqual(
            ANALYZER.validate(
                summary, 8, 25.0, 45.0, 150.0, 1.0, 4, 200.0
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
            }
            for distance in (0.0, 1.0, 0.5, 0.6)
        ]
        failures = ANALYZER.validate(
            ANALYZER.analyze(samples), 4, 25.0, 45.0, 150.0, 0.5, 4, 200.0
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
            self.assertEqual(samples[0]["max_frame_ms"], 31)


if __name__ == "__main__":
    unittest.main()
