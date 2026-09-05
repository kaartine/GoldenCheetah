#!/usr/bin/env python3
"""Validate Workout Game frame pacing and forward progress from a UI trace."""

from __future__ import annotations

import argparse
import bisect
import csv
import hashlib
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
QUICK3D_RENDERER = "Qt Quick 3D"
QUICK3D_SELECTION = "Workout Game renderer selection: Qt Quick 3D"
RENDERER_SELECTION = "Workout Game renderer selection:"
QUICK3D_FALLBACK = "Workout Game renderer fallback: Qt Quick 3D -> SceneGraph"
QUICK3D_TRACE_MARKER = "workout-game-3d-trace "
LEGACY_TRACE_MARKER = "workout-game-trace "
QUICK3D_CANVAS = "Workout game 3D canvas"
SOURCE_SUFFIX = re.compile(r"\([^()\r\n]+:\d+\)")
SHA256 = re.compile(r"[0-9a-f]{64}")
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


def has_exact_qt_message(line: str, message: str) -> bool:
    offset = line.find(message)
    if offset < 0:
        return False
    suffix = line[offset + len(message):].strip()
    return not suffix or SOURCE_SUFFIX.fullmatch(suffix) is not None


def selected_renderer(line: str) -> str | None:
    offset = line.find(RENDERER_SELECTION)
    if offset < 0:
        return None
    value = line[offset + len(RENDERER_SELECTION):].strip()
    if not value:
        return None
    if value.startswith(QUICK3D_RENDERER) and has_exact_qt_message(
        line, QUICK3D_SELECTION
    ):
        return QUICK3D_RENDERER
    return value.split()[0]


def latest_renderer_session(lines: list[str]) -> tuple[list[str], str]:
    selections = [
        (index, renderer)
        for index, line in enumerate(lines)
        if (renderer := selected_renderer(line)) is not None
    ]
    session_start, renderer = selections[-1] if selections else (len(lines), "")
    return lines[session_start:], renderer


def appimage_sha256(path: Path | None) -> str:
    if path is None or not path.is_file():
        return ""
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError:
        return ""
    return digest.hexdigest()


def renderer_evidence(
    log: Path,
    requested_renderer: str,
    accessible_canvas_name: str,
    image: Path | None,
) -> dict[str, object]:
    try:
        lines = log.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines()
    except OSError:
        lines = []
    session_lines, renderer = latest_renderer_session(lines)
    quick3d_traces = sum(
        QUICK3D_TRACE_MARKER in line for line in session_lines
    )
    legacy_traces = sum(LEGACY_TRACE_MARKER in line for line in session_lines)
    fallback = any(
        has_exact_qt_message(line, QUICK3D_FALLBACK)
        for line in session_lines
    )
    digest = appimage_sha256(image)
    evidence: dict[str, object] = {
        "requested_renderer": requested_renderer,
        "selected_renderer": renderer,
        "trace_marker": (
            QUICK3D_TRACE_MARKER.strip() if quick3d_traces else ""
        ),
        "quick3d_trace_samples": quick3d_traces,
        "legacy_trace_samples": legacy_traces,
        "accessible_canvas_name": accessible_canvas_name,
        "fallback_detected": fallback,
        "appimage_sha256": digest,
    }
    failures = validate_renderer_evidence(evidence)
    evidence["passed"] = not failures
    evidence["failures"] = failures
    return evidence


def validate_renderer_evidence(evidence: object) -> list[str]:
    if not isinstance(evidence, dict):
        return ["renderer evidence is not a JSON object"]
    required = {
        "requested_renderer": str,
        "selected_renderer": str,
        "trace_marker": str,
        "quick3d_trace_samples": int,
        "legacy_trace_samples": int,
        "accessible_canvas_name": str,
        "fallback_detected": bool,
        "appimage_sha256": str,
    }
    failures = []
    for name, expected_type in required.items():
        value = evidence.get(name)
        if type(value) is not expected_type:
            failures.append(f"renderer evidence has invalid {name}")
    if failures:
        return failures
    if evidence["requested_renderer"] != QUICK3D_RENDERER:
        failures.append("Qt Quick 3D was not explicitly requested")
    if evidence["selected_renderer"] != QUICK3D_RENDERER:
        failures.append("exact Qt Quick 3D renderer selection is missing")
    if evidence["trace_marker"] != QUICK3D_TRACE_MARKER.strip():
        failures.append("Qt Quick 3D trace marker is missing")
    if evidence["quick3d_trace_samples"] < 1:
        failures.append("Qt Quick 3D trace contains no samples")
    if evidence["accessible_canvas_name"] != QUICK3D_CANVAS:
        failures.append("Qt Quick 3D accessible canvas was not observed")
    if evidence["fallback_detected"]:
        failures.append("Qt Quick 3D fell back to SceneGraph")
    if not SHA256.fullmatch(evidence["appimage_sha256"]):
        failures.append("AppImage SHA-256 is missing or malformed")
    return failures


def load_renderer_evidence(
    path: Path,
) -> tuple[dict[str, object] | None, list[str]]:
    try:
        evidence = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return None, [f"renderer evidence cannot be read: {error}"]
    failures = validate_renderer_evidence(evidence)
    if isinstance(evidence, dict):
        if evidence.get("passed") is not True:
            failures.append("renderer evidence is not marked passed")
        stored_failures = evidence.get("failures")
        if stored_failures != []:
            failures.append("renderer evidence contains failures")
        return evidence, failures
    return None, failures


def write_renderer_evidence(path: Path, evidence: dict[str, object]) -> None:
    rendered = json.dumps(evidence, indent=2, sort_keys=True) + "\n"
    temporary = path.with_name(path.name + ".tmp")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary.write_text(rendered, encoding="utf-8")
    temporary.replace(path)


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


def parse_trace(
    path: Path, quick3d_session: bool = False
) -> list[TraceSample]:
    samples = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    markers = TRACE_MARKERS
    if quick3d_session:
        lines, unused = latest_renderer_session(lines)
        markers = (QUICK3D_TRACE_MARKER,)
    for line in lines:
        match = next(
            (
                (line.find(marker), marker)
                for marker in markers
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
    path: Path,
    within_trace: bool = False,
    quick3d_session: bool = False,
) -> list[TraceSample]:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    trace_markers = TRACE_MARKERS
    if quick3d_session:
        lines, unused = latest_renderer_session(lines)
        trace_markers = (QUICK3D_TRACE_MARKER,)
    first_trace = 0
    last_trace = len(lines) - 1
    if within_trace:
        trace_lines = [
            index for index, line in enumerate(lines)
            if any(marker in line for marker in trace_markers)
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


def analyze_cold_start(
    samples: list[TraceSample], deadline_errors: int = 0
) -> dict[str, float | int]:
    def maximum(name: str) -> float:
        return max(
            (value for sample in samples
             if (value := numeric(sample, name)) is not None),
            default=0.0,
        )

    latest = max(
        samples,
        key=lambda sample: numeric(sample, "cold_samples") or -1.0,
        default={},
    )

    def latest_value(name: str) -> float:
        return numeric(latest, name) or 0.0

    return {
        "cold_complete": int(maximum("cold_complete")),
        "cold_samples": int(maximum("cold_samples")),
        "cold_dropped_frames": int(maximum("cold_dropped_frames")),
        "cold_swap_fps": latest_value("cold_swap_fps"),
        "cold_visual_fps": latest_value("cold_visual_fps"),
        "cold_start_first_swap_ms": latest_value(
            "cold_start_first_swap_ms"
        ),
        "cold_p99_frame_ms": latest_value("cold_p99_frame_ms"),
        "cold_max_frame_ms": maximum("cold_max_frame_ms"),
        "cold_consecutive_late": int(maximum("cold_consecutive_late")),
        "cold_visual_stall_ms": maximum("cold_visual_stall_ms"),
        "cold_max_geometry_queue": int(maximum("geometry_queue")),
        "cold_backward_frames": int(maximum("backwards")),
        "cold_skipped_ticks": int(maximum("skipped_ticks")),
        "cold_deadline_errors": int(max(
            deadline_errors, maximum("deadline_errors")
        )),
    }


def validate_cold_start(summary: dict[str, float | int]) -> list[str]:
    failures = []
    if not summary["cold_complete"]:
        failures.append("cold-start evidence is not complete for the first 10 seconds")
    if summary["cold_samples"] <= 0:
        failures.append("cold-start evidence contains no frame swaps")
    if summary["cold_dropped_frames"]:
        failures.append("cold-start frame capture dropped swap timestamps")
    if summary["cold_p99_frame_ms"] > 25.0:
        failures.append("cold-start p99 frame interval exceeds 25 ms")
    if summary["cold_max_frame_ms"] > 50.0:
        failures.append("cold-start maximum frame interval exceeds 50 ms")
    if summary["cold_consecutive_late"] > 1:
        failures.append("cold-start contains consecutive late frames")
    if summary["cold_start_first_swap_ms"] > 50.0:
        failures.append("cold-start first swap took more than 50 ms")
    if summary["cold_visual_stall_ms"] > 50.0:
        failures.append("cold-start visual revision stalled for more than 50 ms")
    if summary["cold_swap_fps"] <= 0.0:
        failures.append("cold-start swap FPS was not reported")
    if summary["cold_visual_fps"] <= 0.0:
        failures.append("cold-start unique visual FPS was not reported")
    if summary["cold_max_geometry_queue"] > 1:
        failures.append("cold-start renderer queue exceeded one item")
    if summary["cold_backward_frames"]:
        failures.append("cold-start renderer counted backward frames")
    if summary["cold_skipped_ticks"]:
        failures.append("cold-start simulation skipped ticks")
    if summary["cold_deadline_errors"]:
        failures.append("cold-start trainer or recording deadline errors were logged")
    return failures


def count_deadline_errors(path: Path) -> int:
    pattern = re.compile(
        r"(?:trainer|recording).*deadline.*(?:miss|error|exceed)|"
        r"deadline.*(?:trainer|recording).*(?:miss|error|exceed)",
        re.IGNORECASE,
    )
    return sum(1 for line in path.read_text(
        encoding="utf-8", errors="replace"
    ).splitlines() if pattern.search(line))


def analyze_gap_jump(samples: list[TraceSample]) -> dict[str, float | int]:
    gap_samples = [
        sample for sample in samples
        if sample.get("feature_terrain") == "gap-jump"
    ]
    launch_samples = [
        sample for sample in gap_samples
        if numeric(sample, "launch_window") == 1.0
    ]
    locked_samples = [
        sample for sample in gap_samples
        if numeric(sample, "line_locked") == 1.0
    ]
    launch_distances = [
        distance for sample in launch_samples
        if (distance := numeric(sample, "distance_to_lip_m")) is not None
    ]
    locked_distances = [
        distance for sample in locked_samples
        if (distance := numeric(sample, "distance_to_lip_m")) is not None
    ]
    action_ids = [
        int(action_id) for sample in gap_samples
        if (action_id := numeric(sample, "action_id")) is not None
        and action_id > 0
    ]
    post_lock_line_changes = 0
    locked_line = None
    for sample in locked_samples:
        current = sample.get("locked_line")
        if current is None:
            continue
        if locked_line is not None and current != locked_line:
            post_lock_line_changes += 1
        locked_line = current
    return {
        "gap_jump_samples": len(gap_samples),
        "gap_launch_window_samples": len(launch_samples),
        "gap_locked_samples": len(locked_samples),
        "gap_launch_distance_min_m": min(launch_distances, default=0.0),
        "gap_launch_distance_max_m": max(launch_distances, default=0.0),
        "gap_locked_distance_max_m": max(locked_distances, default=0.0),
        "gap_max_power_hold_ms": max(
            (numeric(sample, "power_hold_ms") or 0.0
             for sample in gap_samples),
            default=0.0,
        ),
        "gap_speed_ready_samples": sum(
            numeric(sample, "launch_speed_ready") == 1.0
            for sample in gap_samples
        ),
        "gap_power_ready_samples": sum(
            numeric(sample, "launch_power_ready") == 1.0
            for sample in gap_samples
        ),
        "gap_action_id_changes": sum(
            current != previous
            for previous, current in zip(action_ids, action_ids[1:])
        ),
        "gap_post_lock_line_changes": post_lock_line_changes,
        "gap_launch_samples_outside_window": sum(
            distance <= 3.0 or distance > 10.0
            for distance in launch_distances
        ),
    }


def validate_gap_jump(
    summary: dict[str, float | int],
    require_launch_readiness: bool = True,
) -> list[str]:
    failures = []
    if summary["gap_jump_samples"] < 3:
        failures.append("too few gap jump trace samples")
    if summary["gap_launch_window_samples"] < 1:
        failures.append("gap jump launch window was not observed")
    if summary["gap_locked_samples"] < 1:
        failures.append("gap jump line lock was not observed")
    if summary["gap_launch_samples_outside_window"]:
        failures.append("gap jump launch window was active outside 10-3 m")
    if summary["gap_locked_distance_max_m"] > 3.25:
        failures.append("gap jump line locked before the 3 m decision point")
    if require_launch_readiness:
        if summary["gap_max_power_hold_ms"] < 500.0:
            failures.append("gap jump did not sustain target power for 500 ms")
        if summary["gap_speed_ready_samples"] < 1:
            failures.append("gap jump never completed a 500 ms speed window")
        if summary["gap_power_ready_samples"] < 1:
            failures.append("gap jump power gate never became ready")
    if summary["gap_action_id_changes"]:
        failures.append("gap jump action identity changed during approach")
    if summary["gap_post_lock_line_changes"]:
        failures.append("gap jump line changed after lock")
    return failures


def validate_gap_jump_line(
    samples: list[TraceSample], expected_line: str
) -> list[str]:
    gap_samples = [
        sample for sample in samples
        if sample.get("feature_terrain") == "gap-jump"
    ]
    expected_locked_line = "none" if expected_line == "safe" else expected_line
    locked_indexes = [
        index for index, sample in enumerate(gap_samples)
        if numeric(sample, "line_locked") == 1.0
    ]
    failures = []
    if not locked_indexes:
        return ["gap jump line lock was not observed"]

    first_lock = locked_indexes[0]
    observed_line = str(gap_samples[first_lock].get("locked_line", ""))
    if observed_line != expected_locked_line:
        failures.append(
            f"gap jump locked {observed_line or 'unknown'}, "
            f"expected {expected_line}"
        )
    if any(
        str(sample.get("locked_line", "")) != observed_line
        for sample in gap_samples[first_lock + 1:]
    ):
        failures.append("gap jump line changed after lock")

    completed = any(
        sample.get("route") == "main"
        and sample.get("feature_outcome") == "completed"
        for sample in gap_samples[first_lock:]
    )
    bypassed = any(
        sample.get("route") == "bypass"
        and sample.get("feature_outcome") == "bypassed"
        for sample in gap_samples[first_lock:]
    )
    airborne_indexes = [
        index for index, sample in enumerate(gap_samples[first_lock:], first_lock)
        if numeric(sample, "airborne") == 1.0
    ]

    if expected_line == "safe":
        if not bypassed:
            failures.append("safe gap line did not produce a bypass outcome")
        if airborne_indexes:
            failures.append("safe gap line contains airborne frames")
        return failures

    if not completed:
        failures.append(
            f"{expected_line} gap line did not complete on the main route"
        )
    if not airborne_indexes:
        failures.append(f"{expected_line} gap line has no airborne evidence")
        return failures

    first_airborne = airborne_indexes[0]
    takeoff_indexes = [
        index for index, sample in enumerate(
            gap_samples[first_lock:first_airborne], first_lock
        )
        if sample.get("feature_phase") in ("committed", "action")
        and numeric(sample, "airborne") != 1.0
        and (
            numeric(sample, "rear_contact") == 1.0
            or numeric(sample, "front_contact") == 1.0
        )
    ]
    if not takeoff_indexes:
        failures.append(f"{expected_line} gap line has no takeoff evidence")

    landing_indexes = [
        index for index, sample in enumerate(
            gap_samples[first_airborne + 1:], first_airborne + 1
        )
        if numeric(sample, "airborne") == 0.0
        and (
            numeric(sample, "rear_contact") == 1.0
            or numeric(sample, "front_contact") == 1.0
        )
        and sample.get("feature_phase") in ("action", "recovery")
    ]
    if not landing_indexes:
        failures.append(f"{expected_line} gap line has no landing evidence")
        return failures

    first_landing = landing_indexes[0]
    merged = any(
        sample.get("feature_phase") == "recovery"
        and sample.get("route") == "main"
        and sample.get("feature_outcome") == "completed"
        for sample in gap_samples[first_landing + 1:]
    )
    if not merged:
        failures.append(f"{expected_line} gap line has no merge evidence")
    return failures


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
    parser.add_argument("--require-quick3d-evidence", action="store_true")
    parser.add_argument("--renderer-evidence-json", type=Path)
    parser.add_argument("--accessible-canvas-name-file", type=Path)
    parser.add_argument("--appimage", type=Path)
    parser.add_argument("--renderer-evidence-only", action="store_true")
    parser.add_argument("--require-gap-launch-window", action="store_true")
    parser.add_argument(
        "--expected-gap-line",
        choices=("short", "medium", "long", "safe"),
    )
    parser.add_argument("--require-cold-start-continuity", action="store_true")
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

    if args.renderer_evidence_only and not args.require_quick3d_evidence:
        parser.error(
            "--renderer-evidence-only requires --require-quick3d-evidence"
        )
    if args.require_quick3d_evidence:
        if args.renderer_evidence_json is None:
            parser.error(
                "--require-quick3d-evidence requires --renderer-evidence-json"
            )
        canvas_name = ""
        try:
            if args.accessible_canvas_name_file is not None:
                canvas_name = args.accessible_canvas_name_file.read_text(
                    encoding="utf-8"
                ).strip()
        except OSError:
            pass
        evidence = renderer_evidence(
            args.log,
            QUICK3D_RENDERER,
            canvas_name,
            args.appimage,
        )
        try:
            write_renderer_evidence(args.renderer_evidence_json, evidence)
            unused, evidence_failures = load_renderer_evidence(
                args.renderer_evidence_json
            )
        except OSError as error:
            print(f"Cannot write renderer evidence: {error}", file=sys.stderr)
            return 1
        if evidence_failures:
            print(json.dumps(evidence, indent=2, sort_keys=True))
            return 1
        if args.renderer_evidence_only:
            print(json.dumps(evidence, indent=2, sort_keys=True))
            return 0

    trace = parse_trace(
        args.log, quick3d_session=args.require_quick3d_evidence
    )
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
    if args.require_cold_start_continuity:
        cold_start = analyze_cold_start(
            trace, count_deadline_errors(args.log)
        )
        summary.update(cold_start)
        failures.extend(validate_cold_start(cold_start))
    if args.require_gap_launch_window or args.expected_gap_line:
        gap_summary = analyze_gap_jump(trace)
        summary.update(gap_summary)
        if args.require_gap_launch_window:
            failures.extend(validate_gap_jump(
                gap_summary,
                require_launch_readiness=args.expected_gap_line != "safe",
            ))
        if args.expected_gap_line:
            summary["expected_gap_line"] = args.expected_gap_line
            failures.extend(validate_gap_jump_line(
                trace, args.expected_gap_line
            ))
    if args.recording:
        acceptance = reconcile_acceptance(
            trace,
            parse_trainer_targets(
                args.log,
                within_trace=True,
                quick3d_session=args.require_quick3d_evidence,
            ),
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
