#!/usr/bin/env python3
"""Validate Workout Game frame pacing and forward progress from a UI trace."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import re
import statistics
import sys


TRACE_MARKERS = (
    "workout-game-trace ",
    "workout-game-3d-trace ",
)
FIELD = re.compile(r"([a-z][a-z0-9_]*)=([^\s]+)")


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return ordered[max(0, index)]


def parse_trace(path: Path) -> list[dict[str, float]]:
    samples = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = next(
            (
                (line.find(marker), marker)
                for marker in TRACE_MARKERS
                if line.find(marker) >= 0
            ),
            None,
        )
        if match is None:
            continue
        offset, marker = match
        fields: dict[str, float] = {}
        for name, value in FIELD.findall(line[offset + len(marker) :]):
            try:
                fields[name] = float(value)
            except ValueError:
                pass
        if fields:
            samples.append(fields)
    return samples


def analyze(samples: list[dict[str, float]]) -> dict[str, float | int]:
    frame_ms = [sample["frame_ms"] for sample in samples if sample.get("frame_ms", 0) > 0]
    fps = [sample["fps"] for sample in samples if sample.get("fps", 0) > 0]
    reported_p95 = [sample["p95_frame_ms"] for sample in samples if sample.get("p95_frame_ms", 0) > 0]
    reported_max = [sample["max_frame_ms"] for sample in samples if sample.get("max_frame_ms", 0) > 0]
    distances = [sample["render_road_m"] for sample in samples if "render_road_m" in sample]
    target_watts = [sample["target_watts"] for sample in samples if sample.get("target_watts", 0) > 0]
    lateral_offsets = [
        sample["lateral_m"] for sample in samples if "lateral_m" in sample
    ]
    trace_regressions = sum(
        1 for previous, current in zip(distances, distances[1:])
        if current < previous - 1e-6
    )
    return {
        "samples": len(samples),
        "median_fps": statistics.median(fps) if fps else 0.0,
        "minimum_fps": min(fps, default=0.0),
        "median_frame_ms": statistics.median(frame_ms) if frame_ms else 0.0,
        "observed_p95_frame_ms": percentile(frame_ms, 0.95),
        "observed_p99_frame_ms": percentile(frame_ms, 0.99),
        "observed_max_frame_ms": max(frame_ms, default=0.0),
        "reported_p95_frame_ms": (
            statistics.median(reported_p95[-8:]) if reported_p95 else 0.0
        ),
        "reported_max_frame_ms": max(reported_max, default=0.0),
        "backward_frames": int(max(
            (sample.get("backwards", 0) for sample in samples), default=0
        )),
        "trace_regressions": trace_regressions,
        "skipped_simulation_ticks": int(max(
            (sample.get("skipped_ticks", 0) for sample in samples), default=0
        )),
        "unexpected_airborne_frames": int(max(
            (sample.get("unexpected_airborne_frames", 0) for sample in samples),
            default=0,
        )),
        "distance_advanced_m": max(0.0, distances[-1] - distances[0])
            if len(distances) >= 2 else 0.0,
        "maximum_target_watts": max(target_watts, default=0.0),
        "maximum_lateral_step_m": max(
            (abs(current - previous)
             for previous, current in zip(lateral_offsets, lateral_offsets[1:])),
            default=0.0,
        ),
    }


def validate(
    summary: dict[str, float | int],
    minimum_samples: int,
    minimum_fps: float,
    maximum_p95_ms: float,
    maximum_stall_ms: float,
    minimum_distance_m: float,
    maximum_skipped_ticks: int,
    minimum_target_watts: float,
    maximum_unexpected_airborne_frames: int,
    maximum_lateral_step_m: float,
) -> list[str]:
    failures = []
    if summary["samples"] < minimum_samples:
        failures.append(f"only {summary['samples']} trace samples")
    if summary["median_fps"] < minimum_fps:
        failures.append(
            f"median FPS {summary['median_fps']:.1f} is below {minimum_fps:.1f}"
        )
    if summary["reported_p95_frame_ms"] > maximum_p95_ms:
        failures.append(
            "reported p95 frame interval "
            f"{summary['reported_p95_frame_ms']:.1f} ms exceeds {maximum_p95_ms:.1f} ms"
        )
    if summary["reported_max_frame_ms"] > maximum_stall_ms:
        failures.append(
            "reported maximum frame interval "
            f"{summary['reported_max_frame_ms']:.1f} ms exceeds {maximum_stall_ms:.1f} ms"
        )
    if summary["backward_frames"]:
        failures.append(f"renderer counted {summary['backward_frames']} backward frames")
    if summary["trace_regressions"]:
        failures.append(f"trace contains {summary['trace_regressions']} distance regressions")
    if summary["distance_advanced_m"] < minimum_distance_m:
        failures.append(
            f"course advanced only {summary['distance_advanced_m']:.2f} m"
        )
    if summary["skipped_simulation_ticks"] > maximum_skipped_ticks:
        failures.append(
            f"simulation skipped {summary['skipped_simulation_ticks']} ticks"
        )
    if summary["maximum_target_watts"] < minimum_target_watts:
        failures.append(
            f"maximum target power {summary['maximum_target_watts']:.1f} W "
            f"is below {minimum_target_watts:.1f} W"
        )
    if summary["unexpected_airborne_frames"] > maximum_unexpected_airborne_frames:
        failures.append(
            "renderer counted "
            f"{summary['unexpected_airborne_frames']} unexpected airborne frames"
        )
    if summary["maximum_lateral_step_m"] > maximum_lateral_step_m:
        failures.append(
            "lateral route offset changed "
            f"{summary['maximum_lateral_step_m']:.2f} m between trace samples"
        )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--minimum-samples", type=int, default=8)
    parser.add_argument("--minimum-fps", type=float, default=25.0)
    parser.add_argument("--maximum-p95-ms", type=float, default=45.0)
    parser.add_argument("--maximum-stall-ms", type=float, default=150.0)
    parser.add_argument("--minimum-distance-m", type=float, default=1.0)
    parser.add_argument("--maximum-skipped-ticks", type=int, default=4)
    parser.add_argument("--minimum-target-watts", type=float, default=190.0)
    parser.add_argument(
        "--maximum-unexpected-airborne-frames", type=int, default=0
    )
    parser.add_argument("--maximum-lateral-step-m", type=float, default=1.0)
    args = parser.parse_args()

    summary = analyze(parse_trace(args.log))
    failures = validate(
        summary,
        args.minimum_samples,
        args.minimum_fps,
        args.maximum_p95_ms,
        args.maximum_stall_ms,
        args.minimum_distance_m,
        args.maximum_skipped_ticks,
        args.minimum_target_watts,
        args.maximum_unexpected_airborne_frames,
        args.maximum_lateral_step_m,
    )
    summary["passed"] = not failures
    summary["failures"] = failures
    rendered = json.dumps(summary, indent=2, sort_keys=True)
    print(rendered)
    if args.json:
        args.json.write_text(rendered + "\n", encoding="utf-8")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
