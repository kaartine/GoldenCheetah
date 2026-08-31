#!/usr/bin/env python3
"""Validate Workout Game frame pacing and forward progress from a UI trace."""

from __future__ import annotations

import argparse
import bisect
import csv
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
TRAINER_TARGET_MARKER = "workout-game-trainer-target "
FIELD = re.compile(r"([a-z][a-z0-9_]*)=([^\s]+)")
RECORDING_COLUMNS = (
    "secs", "cad", "hr", "km", "watts", "slope", "target", "virtualgear",
)
TRACE_ALIGNMENT_WINDOW_MS = 750.0
ERG_RECORDING_LATENCY_MS = 1250.0
TARGET_MATCH_TOLERANCE = 0.5

TraceValue = float | str
TraceSample = dict[str, TraceValue]


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return ordered[max(0, index)]


def parse_fields(text: str) -> TraceSample:
    fields: TraceSample = {}
    for name, value in FIELD.findall(text):
        try:
            fields[name] = float(value)
        except ValueError:
            fields[name] = value
    return fields


def parse_trace(path: Path) -> list[TraceSample]:
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
        fields = parse_fields(line[offset + len(marker) :])
        if fields:
            samples.append(fields)
    return samples


def parse_trainer_targets(
    path: Path, within_trace: bool = False
) -> list[TraceSample]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    first_trace = 0
    last_trace = len(lines) - 1
    if within_trace:
        trace_lines = [
            index for index, line in enumerate(lines)
            if any(marker in line for marker in TRACE_MARKERS)
        ]
        if not trace_lines:
            return []
        first_trace = trace_lines[0]
        last_trace = trace_lines[-1]
    targets = []
    for index, line in enumerate(lines):
        if index < first_trace or index > last_trace:
            continue
        offset = line.find(TRAINER_TARGET_MARKER)
        if offset < 0:
            continue
        fields = parse_fields(line[offset + len(TRAINER_TARGET_MARKER) :])
        if fields:
            targets.append(fields)
    return targets


def parse_recording(path: Path) -> list[dict[str, float]]:
    samples: list[dict[str, float]] = []
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream, skipinitialspace=True)
        available = {name.strip() for name in (reader.fieldnames or []) if name}
        missing = sorted(set(RECORDING_COLUMNS) - available)
        if missing:
            raise ValueError(
                "recording is missing required columns: " + ", ".join(missing)
            )
        previous_secs = -math.inf
        for line_number, row in enumerate(reader, start=2):
            normalized = {
                (name.strip() if name else name): value
                for name, value in row.items()
            }
            sample: dict[str, float] = {}
            for name in RECORDING_COLUMNS:
                raw = normalized.get(name)
                try:
                    value = float(raw) if raw is not None else math.nan
                except ValueError as error:
                    raise ValueError(
                        f"recording line {line_number} has invalid {name}"
                    ) from error
                if not math.isfinite(value):
                    raise ValueError(
                        f"recording line {line_number} has non-finite {name}"
                    )
                sample[name] = value
            if sample["secs"] <= previous_secs:
                raise ValueError("recording time must be strictly increasing")
            previous_secs = sample["secs"]
            samples.append(sample)
    return samples


def numeric(sample: TraceSample, name: str) -> float | None:
    value = sample.get(name)
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def nearest_by(
    samples: list[dict[str, float]] | list[TraceSample],
    values: list[float],
    position: float,
):
    if not samples:
        return None
    index = bisect.bisect_left(values, position)
    candidates = [candidate for candidate in (index - 1, index)
                  if 0 <= candidate < len(samples)]
    if not candidates:
        return None
    return samples[min(candidates, key=lambda candidate: abs(
        values[candidate] - position
    ))]


def trace_for_recording(
    samples: list[TraceSample],
    times: list[float],
    row: dict[str, float],
) -> TraceSample | None:
    position = row["secs"] * 1000.0
    first = bisect.bisect_left(times, position - TRACE_ALIGNMENT_WINDOW_MS)
    last = bisect.bisect_right(times, position + TRACE_ALIGNMENT_WINDOW_MS)
    candidates = samples[first:last]
    if not candidates:
        return None

    matching_target = [
        sample for sample in candidates
        if (target := numeric(sample, "target_watts")) is not None
        and abs(target - row["target"]) <= TARGET_MATCH_TOLERANCE
    ]
    usable = matching_target or candidates
    return min(
        usable,
        key=lambda sample: abs((numeric(sample, "source_ms") or 0.0) - position),
    )


def trainer_target_delta(
    target: TraceSample,
    recording: list[dict[str, float]],
    recording_times: list[float],
    recording_distances: list[float],
) -> float | None:
    mode = target.get("mode")
    position = numeric(target, "workout_pos")
    value = numeric(target, "value")
    if mode not in ("erg", "slope") or position is None or value is None:
        return None

    positions = recording_times if mode == "erg" else recording_distances
    expected_name = "target" if mode == "erg" else "slope"
    if mode == "erg":
        first = bisect.bisect_left(
            positions, position - ERG_RECORDING_LATENCY_MS
        )
        last = bisect.bisect_right(
            positions, position + ERG_RECORDING_LATENCY_MS
        )
        candidates = recording[first:last]
    else:
        nearest = nearest_by(recording, positions, position)
        candidates = [nearest] if nearest is not None else []
    if not candidates:
        return None
    return min(abs(value - row[expected_name]) for row in candidates)


def reconcile_acceptance(
    trace: list[TraceSample],
    trainer_targets: list[TraceSample],
    recording: list[dict[str, float]],
) -> dict[str, float | int]:
    timed_trace = sorted(
        (sample for sample in trace if numeric(sample, "source_ms") is not None),
        key=lambda sample: numeric(sample, "source_ms") or 0.0,
    )
    trace_times = [numeric(sample, "source_ms") or 0.0 for sample in timed_trace]
    if trace_times:
        recording_in_trace_window = [
            row for row in recording
            if trace_times[0] - 750.0
            <= row["secs"] * 1000.0
            <= trace_times[-1] + 750.0
        ]
    else:
        recording_in_trace_window = recording
    power_deltas = []
    cadence_deltas = []
    heart_rate_deltas = []
    gear_mismatches = 0
    matched_recordings = 0
    for row in recording_in_trace_window:
        sample = trace_for_recording(timed_trace, trace_times, row)
        if sample is None:
            continue
        source_ms = numeric(sample, "source_ms")
        if (source_ms is None
                or abs(source_ms - row["secs"] * 1000.0)
                > TRACE_ALIGNMENT_WINDOW_MS):
            continue
        values = {
            "watts": numeric(sample, "watts"),
            "cadence": numeric(sample, "cadence"),
            "hr": numeric(sample, "hr"),
            "gear": numeric(sample, "gear"),
        }
        if any(value is None for value in values.values()):
            continue
        matched_recordings += 1
        power_deltas.append(abs(values["watts"] - row["watts"]))
        cadence_deltas.append(abs(values["cadence"] - row["cad"]))
        heart_rate_deltas.append(abs(values["hr"] - row["hr"]))
        gear_mismatches += int(round(values["gear"]) != round(row["virtualgear"]))

    recording_times = [row["secs"] * 1000.0 for row in recording]
    recording_distances = [row["km"] * 1000.0 for row in recording]
    trainer_target_deltas = []
    trainer_targets_with_devices = 0
    for target in trainer_targets:
        mode = target.get("mode")
        position = numeric(target, "workout_pos")
        value = numeric(target, "value")
        devices = numeric(target, "devices")
        if mode not in ("erg", "slope") or position is None or value is None:
            continue
        trainer_targets_with_devices += int(devices is not None and devices > 0)
        delta = trainer_target_delta(
            target, recording, recording_times, recording_distances
        )
        if delta is not None:
            trainer_target_deltas.append(delta)

    gear_changes = 0
    gear_change_speed_steps = []
    for previous, current in zip(trace, trace[1:]):
        previous_gear = numeric(previous, "gear")
        current_gear = numeric(current, "gear")
        previous_speed = numeric(previous, "speed_kph")
        current_speed = numeric(current, "speed_kph")
        if (previous_gear is None or current_gear is None
                or round(previous_gear) == round(current_gear)):
            continue
        gear_changes += 1
        if previous_speed is not None and current_speed is not None:
            gear_change_speed_steps.append(abs(current_speed - previous_speed))

    decisions: dict[int, TraceSample] = {}
    for sample in trace:
        outcome = sample.get("feature_outcome")
        action_id = numeric(sample, "action_id")
        if outcome in ("completed", "bypassed") and action_id is not None:
            decisions[int(action_id)] = sample
    inconsistent_decisions = 0
    for sample in decisions.values():
        outcome = sample.get("feature_outcome")
        route = sample.get("route")
        terrain = str(sample.get(
            "feature_terrain", sample.get("feature_geometry", "")
        ))
        readiness = numeric(sample, "readiness")
        if readiness is None:
            inconsistent_decisions += 1
        elif outcome == "completed" and (route != "main" or readiness < 0.999):
            inconsistent_decisions += 1
        elif outcome == "bypassed" and readiness >= 0.999:
            inconsistent_decisions += 1
        elif (outcome == "bypassed" and route != "bypass"
              and terrain not in ("rollers", "climb")):
            inconsistent_decisions += 1

    return {
        "recording_samples": len(recording),
        "recording_samples_in_trace_window": len(recording_in_trace_window),
        "matched_recording_samples": matched_recordings,
        "recording_match_ratio": (
            matched_recordings / len(recording_in_trace_window)
            if recording_in_trace_window else 0.0
        ),
        "maximum_power_delta_watts": max(power_deltas, default=0.0),
        "p95_power_delta_watts": percentile(power_deltas, 0.95),
        "maximum_cadence_delta_rpm": max(cadence_deltas, default=0.0),
        "maximum_heart_rate_delta_bpm": max(heart_rate_deltas, default=0.0),
        "gear_mismatches": gear_mismatches,
        "trainer_target_dispatches": len(trainer_targets),
        "trainer_targets_with_devices": trainer_targets_with_devices,
        "maximum_trainer_target_delta": max(trainer_target_deltas, default=0.0),
        "p95_trainer_target_delta": percentile(
            trainer_target_deltas, 0.95
        ),
        "gear_changes": gear_changes,
        "maximum_gear_change_speed_step_kph": max(
            gear_change_speed_steps, default=0.0
        ),
        "feature_decisions": len(decisions),
        "inconsistent_feature_decisions": inconsistent_decisions,
    }


def validate_acceptance(
    summary: dict[str, float | int],
    minimum_recording_matches: int,
    minimum_recording_match_ratio: float,
    maximum_power_delta_watts: float,
    maximum_cadence_delta_rpm: float,
    maximum_heart_rate_delta_bpm: float,
    maximum_gear_mismatches: int,
    maximum_trainer_target_delta: float,
    minimum_trainer_target_dispatches: int,
    minimum_feature_decisions: int,
    minimum_gear_changes: int = 0,
    maximum_gear_change_speed_step_kph: float = math.inf,
) -> list[str]:
    failures = []
    if summary["matched_recording_samples"] < minimum_recording_matches:
        failures.append("too few recording samples matched the game trace")
    if summary["recording_match_ratio"] < minimum_recording_match_ratio:
        failures.append("recording-to-trace match ratio is too low")
    if summary["p95_power_delta_watts"] > maximum_power_delta_watts:
        failures.append(
            "recorded power persistently disagrees with game telemetry"
        )
    if summary["maximum_cadence_delta_rpm"] > maximum_cadence_delta_rpm:
        failures.append("recorded cadence disagrees with game telemetry")
    if summary["maximum_heart_rate_delta_bpm"] > maximum_heart_rate_delta_bpm:
        failures.append("recorded heart rate disagrees with game telemetry")
    if summary["gear_mismatches"] > maximum_gear_mismatches:
        failures.append("recorded virtual gear disagrees with game telemetry")
    if summary["trainer_target_dispatches"] < minimum_trainer_target_dispatches:
        failures.append("too few trainer target dispatches were traced")
    if summary["trainer_targets_with_devices"] < minimum_trainer_target_dispatches:
        failures.append("trainer targets were dispatched without active devices")
    if summary["p95_trainer_target_delta"] > maximum_trainer_target_delta:
        failures.append(
            "trainer target persistently disagrees with the recording"
        )
    if summary["feature_decisions"] < minimum_feature_decisions:
        failures.append("too few feature decisions were observed")
    if summary["inconsistent_feature_decisions"]:
        failures.append("feature decision disagrees with readiness or route")
    if summary["gear_changes"] < minimum_gear_changes:
        failures.append("too few virtual gear changes were observed")
    if (summary["maximum_gear_change_speed_step_kph"]
            > maximum_gear_change_speed_step_kph):
        failures.append("virtual gear change caused an immediate speed step")
    return failures


def analyze(samples: list[TraceSample]) -> dict[str, float | int]:
    def values(name: str, positive: bool = False) -> list[float]:
        result = [value for sample in samples
                  if (value := numeric(sample, name)) is not None]
        return [value for value in result if value > 0.0] if positive else result

    frame_ms = values("frame_ms", positive=True)
    fps = values("fps", positive=True)
    reported_p95 = values("p95_frame_ms", positive=True)
    reported_max = values("max_frame_ms", positive=True)
    distances = values("render_road_m")
    target_watts = values("target_watts", positive=True)
    lateral_offsets = values("lateral_m")
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
            values("backwards"), default=0
        )),
        "trace_regressions": trace_regressions,
        "skipped_simulation_ticks": int(max(
            values("skipped_ticks"), default=0
        )),
        "unexpected_airborne_frames": int(max(
            values("unexpected_airborne_frames"), default=0,
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
    parser.add_argument("--recording", type=Path)
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
    parser.add_argument("--minimum-recording-matches", type=int, default=5)
    parser.add_argument(
        "--minimum-recording-match-ratio", type=float, default=0.75
    )
    parser.add_argument("--maximum-power-delta-watts", type=float, default=20.0)
    parser.add_argument("--maximum-cadence-delta-rpm", type=float, default=10.0)
    parser.add_argument(
        "--maximum-heart-rate-delta-bpm", type=float, default=10.0
    )
    parser.add_argument("--maximum-gear-mismatches", type=int, default=1)
    parser.add_argument("--maximum-trainer-target-delta", type=float, default=5.0)
    parser.add_argument(
        "--minimum-trainer-target-dispatches", type=int, default=1
    )
    parser.add_argument("--minimum-feature-decisions", type=int, default=1)
    parser.add_argument("--minimum-gear-changes", type=int, default=2)
    parser.add_argument(
        "--maximum-gear-change-speed-step-kph", type=float, default=2.0
    )
    args = parser.parse_args()

    trace = parse_trace(args.log)
    summary = analyze(trace)
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
    if args.recording:
        acceptance = reconcile_acceptance(
            trace,
            parse_trainer_targets(args.log, within_trace=True),
            parse_recording(args.recording),
        )
        summary.update(acceptance)
        failures.extend(validate_acceptance(
            acceptance,
            args.minimum_recording_matches,
            args.minimum_recording_match_ratio,
            args.maximum_power_delta_watts,
            args.maximum_cadence_delta_rpm,
            args.maximum_heart_rate_delta_bpm,
            args.maximum_gear_mismatches,
            args.maximum_trainer_target_delta,
            args.minimum_trainer_target_dispatches,
            args.minimum_feature_decisions,
            args.minimum_gear_changes,
            args.maximum_gear_change_speed_step_kph,
        ))
    summary["passed"] = not failures
    summary["failures"] = failures
    rendered = json.dumps(summary, indent=2, sort_keys=True)
    print(rendered)
    if args.json:
        args.json.write_text(rendered + "\n", encoding="utf-8")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
