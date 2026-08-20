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


class AnalyzeWorkoutGameTest(unittest.TestCase):
    def test_accepts_smooth_forward_trace(self):
        samples = [
            {
                "frame_ms": 16,
                "fps": 59.4 + (index % 3),
                "p95_frame_ms": 18,
                "render_road_m": index * 0.5,
                "backwards": 0,
                "skipped_ticks": 0,
            }
            for index in range(10)
        ]
        summary = ANALYZER.analyze(samples)
        self.assertEqual(
            ANALYZER.validate(summary, 8, 25.0, 45.0, 1.0, 4), []
        )

    def test_rejects_regression_and_pacing_failure(self):
        samples = [
            {
                "frame_ms": 80,
                "fps": 12,
                "p95_frame_ms": 90,
                "render_road_m": distance,
                "backwards": 1,
                "skipped_ticks": 2,
            }
            for distance in (0.0, 1.0, 0.5, 0.6)
        ]
        failures = ANALYZER.validate(
            ANALYZER.analyze(samples), 4, 25.0, 45.0, 0.5, 4
        )
        self.assertGreaterEqual(len(failures), 4)

    def test_parses_trace_fields_from_qt_log_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[debug] workout-game-trace frame=9 frame_ms=17 "
                "fps=58.7 render_road_m=12.5 p95_frame_ms=19 "
                "backwards=0 skipped_ticks=0\n",
                encoding="utf-8",
            )
            samples = ANALYZER.parse_trace(path)
            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]["frame"], 9)
            self.assertEqual(samples[0]["fps"], 58.7)


if __name__ == "__main__":
    unittest.main()
