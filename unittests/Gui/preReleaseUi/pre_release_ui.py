#!/usr/bin/env python3
"""Isolated AT-SPI pre-release exercise for GoldenCheetah."""

from __future__ import annotations

import hashlib
import csv
import io
import json
import math
import os
from pathlib import Path
import signal
import shutil
import sys
import time
import traceback
import xml.etree.ElementTree as ET


ATHLETE = "UiTestAthlete"
WORKOUT_GAME_CANVAS_NAMES = (
    "Workout game canvas",
    "Workout game 3D canvas",
)
RENDERER_CANVAS_NAME_FILE = "renderer-canvas-name.txt"
GENERATOR_MODES = {
    "follow-target",
    "on-target",
    "over-target",
    "under-target",
    "cadence-low",
    "cadence-high",
}
UI_TEST_NAMES = (
    "startup_and_main_navigation",
    "view_navigation",
    "prepared_workout_library_import",
    "library_scan_preserves_unsearched_workouts",
    "library_scan_rejects_unavailable_path",
    "train_control_accessibility",
    "data_generator_and_virtual_gears",
    "create_edit_mtb_course_lifecycle",
    "workout_game_training_lifecycle",
    "training_failure_independence",
    "workout_generator_lifecycle",
    "new_workout_save_as",
    "dirty_workout_transition_guard",
    "workout_deletion_removes_mtb_sidecar",
    "graceful_shutdown_request",
)
UI_TEST_DEPENDENCIES = {
    "training_failure_independence": ("prepared_workout_library_import",),
    "library_scan_preserves_unsearched_workouts": (
        "prepared_workout_library_import",
    ),
    "library_scan_rejects_unavailable_path": (
        "prepared_workout_library_import",
    ),
    "create_edit_mtb_course_lifecycle": (
        "prepared_workout_library_import",
    ),
    "workout_game_training_lifecycle": (
        "prepared_workout_library_import",
    ),
    "workout_deletion_removes_mtb_sidecar": (
        "prepared_workout_library_import",
    ),
    "dirty_workout_transition_guard": (
        "prepared_workout_library_import",
    ),
}


def canvas_requires_pixel_motion(accessible_name: str) -> bool:
    return accessible_name != "Workout game 3D canvas"


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def generator_mode_from_environment() -> str:
    mode = os.environ.get("GC_UI_GENERATOR_MODE", "follow-target")
    if mode not in GENERATOR_MODES:
        supported = ", ".join(sorted(GENERATOR_MODES))
        raise ValueError(
            f"Unsupported GC_UI_GENERATOR_MODE {mode!r}; expected one of {supported}"
        )
    return mode


def training_failure_case_from_environment() -> str | None:
    case = os.environ.get("GC_UI_TRAINING_FAILURE_CASE", "")
    if not case:
        return None
    if case not in {"absent", "healthy", "renderer", "runner"}:
        raise ValueError("Unsupported GC_UI_TRAINING_FAILURE_CASE")
    if os.environ.get("GC_WORKOUT_GAME_FEATURE_LAB", "0") != "0":
        raise ValueError("Failure-independence tests must disable Feature Lab")
    return case


def game_run_seconds_from_environment() -> float:
    value = os.environ.get("GC_UI_GAME_RUN_SECONDS", "11.8")
    try:
        seconds = float(value)
    except ValueError as error:
        raise ValueError(
            f"Invalid GC_UI_GAME_RUN_SECONDS {value!r}"
        ) from error
    if not 1.0 <= seconds <= 120.0:
        raise ValueError(
            "GC_UI_GAME_RUN_SECONDS must be between 1 and 120 seconds"
        )
    if (
        os.environ.get("GC_UI_REQUIRE_QUICK3D_EVIDENCE") == "1"
        and seconds < 10.5
    ):
        raise ValueError(
            "Quick3D evidence requires at least 10.5 seconds so visual "
            "capture follows the cold-start window"
        )
    return seconds


def trainer_acceptance_shift_delays(seconds: float) -> tuple[float, float, float]:
    if seconds < 8.0:
        raise ValueError(
            "trainer acceptance game duration must be at least 8 seconds"
        )
    first = seconds / 4.0
    second = seconds / 4.0
    return first, second, seconds - first - second


def ui_screenshots_enabled_from_environment() -> bool:
    if os.environ.get("GC_UI_REQUIRE_QUICK3D_EVIDENCE") == "1":
        return False
    return not (
        validate_trainer_acceptance_from_environment()
        and bool(os.environ.get("GC_UI_EXISTING_DISPLAY"))
    )


def skip_save_as_from_environment() -> bool:
    value = os.environ.get("GC_UI_SKIP_SAVE_AS", "0")
    if value not in ("0", "1"):
        raise ValueError("GC_UI_SKIP_SAVE_AS must be 0 or 1")
    return value == "1"


def validate_trainer_acceptance_from_environment() -> bool:
    value = os.environ.get("GC_UI_VALIDATE_TRAINER_ACCEPTANCE", "0")
    if value not in ("0", "1"):
        raise ValueError("GC_UI_VALIDATE_TRAINER_ACCEPTANCE must be 0 or 1")
    return value == "1"


def validate_mtb_course_from_environment() -> bool:
    value = os.environ.get("GC_UI_VALIDATE_MTB_COURSE", "0")
    if value not in ("0", "1"):
        raise ValueError("GC_UI_VALIDATE_MTB_COURSE must be 0 or 1")
    return value == "1"


def selected_ui_tests_from_environment() -> tuple[str, ...]:
    value = os.environ.get("GC_UI_TESTS", "").strip()
    if not value:
        selected = set(UI_TEST_NAMES)
        selected.discard("training_failure_independence")
        if not validate_mtb_course_from_environment():
            selected.discard("create_edit_mtb_course_lifecycle")
    else:
        requested = [name.strip() for name in value.split(",")]
        if any(not name for name in requested):
            raise ValueError("GC_UI_TESTS contains an empty test name")
        if len(requested) != len(set(requested)):
            raise ValueError("GC_UI_TESTS contains duplicate test names")
        unknown = sorted(set(requested) - set(UI_TEST_NAMES))
        if unknown:
            raise ValueError(
                "GC_UI_TESTS contains unknown tests: " + ", ".join(unknown)
            )
        selected = set(requested)

    failure_case = training_failure_case_from_environment()
    if failure_case:
        selected = {"startup_and_main_navigation", "prepared_workout_library_import",
                    "training_failure_independence", "graceful_shutdown_request"}
    elif "training_failure_independence" in selected:
        raise ValueError("training_failure_independence requires GC_UI_TRAINING_FAILURE_CASE")

    pending = list(selected)
    while pending:
        test_name = pending.pop()
        for dependency in UI_TEST_DEPENDENCIES.get(test_name, ()):
            if dependency not in selected:
                selected.add(dependency)
                pending.append(dependency)

    if validate_mtb_course_from_environment() and (
        "create_edit_mtb_course_lifecycle" not in selected
    ):
        raise ValueError(
            "GC_UI_VALIDATE_MTB_COURSE requires "
            "create_edit_mtb_course_lifecycle in GC_UI_TESTS"
        )
    if (
        validate_trainer_acceptance_from_environment()
        or os.environ.get("GC_UI_REQUIRE_QUICK3D_EVIDENCE") == "1"
    ) and "workout_game_training_lifecycle" not in selected:
        raise ValueError(
            "trainer or Quick3D evidence requires "
            "workout_game_training_lifecycle in GC_UI_TESTS"
        )
    return tuple(name for name in UI_TEST_NAMES if name in selected)


def preserve_game_recording(source: Path, artifacts: Path) -> Path:
    destination = artifacts / "game-training-recording.csv"
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return destination


def record_renderer_canvas_name(root: Path, accessible_name: str) -> Path:
    destination = root / RENDERER_CANVAS_NAME_FILE
    write_text(destination, accessible_name + "\n")
    return destination


def x11_bgrx_to_rgb(data: bytes) -> bytes:
    if len(data) % 4:
        raise ValueError("X11 image data must contain whole BGRX pixels")
    pixels = len(data) // 4
    rgb = bytearray(pixels * 3)
    rgb[0::3] = data[2::4]
    rgb[1::3] = data[1::4]
    rgb[2::3] = data[0::4]
    return bytes(rgb)


def prepare(root: Path) -> None:
    home = root / "home"
    library = root / "library"
    athlete = library / ATHLETE
    for path in (
        home / ".config",
        home / ".cache",
        athlete / "activities",
        athlete / "tempActivities",
        athlete / "imports",
        athlete / "records",
        athlete / "downloads",
        athlete / "bak",
        athlete / "config",
        athlete / "cache",
        athlete / "calendar",
        athlete / "workouts",
        athlete / "logs",
        athlete / "temp",
        athlete / "quarantine",
        athlete / "planned",
        athlete / "snippets",
        athlete / "media",
    ):
        path.mkdir(parents=True, exist_ok=True)

    write_text(
        home / ".config/goldencheetah.org/GoldenCheetah.ini",
        "[migration]\nlegacy_qsettings_v1\\system_state=complete\n",
    )
    write_text(
        library / "configglobal-general.ini",
        "[General]\n"
        f"workoutDir={athlete / 'workouts'}\n\n"
        "[migration]\n"
        "legacy_qsettings_v1\\global_state=complete\n",
    )
    generator_mode = generator_mode_from_environment()
    write_text(
        library / "configglobal-trainmode.ini",
        f"""[General]
devices=1
devicename1=Data Generator
devicetype1=64
devicespec1=
deviceprof1={generator_mode}
devicewheel1=2100
devicestride1=0
devicepostProcess1=0
devicevirtualPower1=

[train]
autoconnect=false
autohide=false
startdelay=0
tooltips=true
""",
    )
    write_text(
        athlete / "config/athlete-general.ini",
        """[General]
id={00000000-0000-4000-8000-000000000042}
safeexit=false
versionused=5012

[migration]
legacy_qsettings_v1\\athlete_state=complete

[opendata]
allowed=N
runcount=1

[upgradesuccess]
folder=true
""",
    )
    for name in (
        "athlete-layout.ini",
        "athlete-preferences.ini",
        "athlete-private.ini",
    ):
        write_text(athlete / "config" / name, "")
    failure_case = training_failure_case_from_environment()
    if failure_case:
        fixture = os.environ.get("GC_UI_TRAINING_FAILURE_FIXTURE")
        if fixture:
            for suffix in ("crs", "gcmtb.json"):
                shutil.copyfile(Path(fixture) / f"failure-course.{suffix}",
                                athlete / "workouts" / f"ui-test-mtb.{suffix}")
        # A hidden game chart is still instantiated. Version 4 also prevents
        # perspective migration from silently adding chart 59 to the baseline.
        layouts = ET.Element("layouts", version="4")
        charts = [("Workout Editor", "36")]
        if failure_case != "absent":
            charts.append(("Workout Game", "59"))
        for title, chart_id in charts:
            layout = ET.SubElement(layouts, "layout", name=title, style="0",
                                   type="1", expression="", trainswitch="0")
            chart = ET.SubElement(layout, "chart", id=chart_id, name="", title=title)
            for name, kind, value in (("title", "QString", title),
                                      ("subtitle", "QString", ""),
                                      ("widthFactor", "double", "1"),
                                      ("heightFactor", "double", "1"),
                                      ("style", "int", "0"),
                                      ("resizable", "bool", "0")):
                ET.SubElement(chart, "property", name=name, type=kind, value=value)
        write_text(athlete / "config/train-perspectives.xml",
                   ET.tostring(layouts, encoding="unicode") + "\n")
    write_text(
        athlete / "workouts/ui-test.erg",
        """[COURSE HEADER]
VERSION = 2
UNITS = ENGLISH
FTP = 190
DESCRIPTION = Pre-release UI test
FILE NAME = ui-test.erg
MINUTES WATTS
[END COURSE HEADER]
[COURSE DATA]
0 100
0.10 100
0.10 220
0.18 220
0.18 100
1.00 100
1.00 135
1.20 135
1.20 100
1.40 100
1.40 160
1.60 160
1.60 105
1.80 105
1.80 170
2.00 170
2.00 100
2.20 100
2.20 150
2.40 150
2.40 110
2.60 110
2.60 165
3.00 165
3.00 100
30.00 100
[END COURSE DATA]
""",
    )
    write_text(
        athlete / "workouts/ui-delete.crs",
        """[COURSE HEADER]
VERSION=1
UNITS=METRIC
DESCRIPTION=Deletion test course
FILE NAME=ui-delete.crs
DISTANCE GRADE WIND
[END COURSE HEADER]
[COURSE DATA]
0.25 0.0 0
0.25 2.0 0
[END COURSE DATA]
""",
    )


class UiFailure(RuntimeError):
    pass


def training_recording_rows(text: str) -> list[dict[str, float]]:
    """Read complete production CSV rows; malformed evidence must not pass."""
    if not text or not text.endswith("\n"):
        raise UiFailure("Recording is empty or contains an incomplete row")
    reader = csv.DictReader(io.StringIO(text), skipinitialspace=True)
    required = {"secs", "cad", "hr", "km", "watts", "target", "virtualgear"}
    if not required.issubset(reader.fieldnames or ()):
        raise UiFailure("Recording is missing required fields")
    rows = []
    for raw in reader:
        try:
            row = {key: float(raw[key]) for key in required}
        except (TypeError, ValueError) as error:
            raise UiFailure("Recording contains a nonnumeric value") from error
        if not all(math.isfinite(value) for value in row.values()):
            raise UiFailure("Recording contains non-finite values")
        if (row["secs"] < 0 or not row["secs"].is_integer()
                or not row["virtualgear"].is_integer()
                or not 1 <= row["virtualgear"] <= 12
                or any(row[key] < 0 for key in ("cad", "hr", "km", "watts", "target"))):
            raise UiFailure("Recording contains invalid training values")
        if rows and (row["secs"] <= rows[-1]["secs"]
                     or row["km"] < rows[-1]["km"]):
            raise UiFailure("Recording time or distance is not monotonic")
        rows.append(row)
    return rows


def training_effort_watts(document: dict, watts: float, gear: int) -> float:
    conversion = document["conversion"]
    if conversion["preset"] != "workout-first":
        raise UiFailure("Failure fixture requires the workout-first prescription")
    reference = conversion["parameters"].get("referenceGear", 6)
    sprockets = (51, 45, 39, 33, 28, 24, 21, 18, 16, 14, 12, 10)
    if (not isinstance(gear, int) or not 1 <= gear <= 12
            or not isinstance(reference, int) or not 1 <= reference <= 12
            or not math.isfinite(watts) or watts < 0):
        raise UiFailure("Invalid prescription effort or gear")
    value = min(1500.0, watts) * sprockets[reference - 1] / sprockets[gear - 1]
    return min(1500.0, value)


def training_prescribed_target(document: dict, position_ms: float, gear: int) -> int:
    """Read the persisted terrain-effort prescription, including variation."""
    if not math.isfinite(position_ms) or position_ms < 0:
        raise UiFailure("Invalid prescription position")
    for interval in training_effort_sections(document):
        start = interval["startMs"]
        duration = interval["durationMs"]
        if start <= position_ms < start + duration:
            fraction = (position_ms - start) / duration
            watts = (interval["startWatts"]
                     + fraction * (interval["endWatts"] - interval["startWatts"]))
            return math.floor(training_effort_watts(document, watts, gear) + 0.5)
    raise UiFailure("Dispatch position is outside the prepared prescription")


def training_effort_sections(document: dict) -> list[dict]:
    sections = document.get("course", {}).get("sections")
    if sections is None:
        return document["source"]["intervals"]
    result = []
    for section in sections:
        start = section.get("referenceEffortStartWatts", -1)
        end = section.get("referenceEffortEndWatts", -1)
        if start < 0 or end < 0:
            start, end = section["targetStartWatts"], section["targetEndWatts"]
        result.append({"startMs": section["sourceStartMs"],
                       "durationMs": section["nominalDurationMs"],
                       "startWatts": start, "endWatts": end})
    return result


def training_prescribed_targets(document: dict, position_ms: float, gear: int) -> set[int]:
    # Production reports llround(section progress * nominal duration). Evaluate
    # only that +/-0.5 ms bin, separately on each side of section discontinuities.
    # Do not accept the intervening watts of a discontinuous power step.
    values = set()
    for interval in training_effort_sections(document):
        low = max(interval["startMs"], position_ms - 0.5)
        high = min(interval["startMs"] + interval["durationMs"], position_ms + 0.5)
        if low >= high:
            continue
        endpoints = [training_prescribed_target(document, sample, gear)
                     for sample in (low, math.nextafter(high, low))]
        values.update(range(min(endpoints), max(endpoints) + 1))
    if not values:
        raise UiFailure("Dispatch position has no valid prescription quantization bin")
    return values


def training_recorded_targets(document: dict, distance_km: float, gear: int) -> set[int]:
    """CSV stores current course effort, not a log of dispatched commands.

    This no-seek fixture advances displayDistance and rawWorkoutDistance by the
    same increments. diskUpdate writes km with QTextStream's default six
    significant digits. guiUpdate truncates its continuous target into long
    load; dispatch can instead leave a rounded target at the same position.
    Evaluate just those conversions within the serialized distance bin.
    """
    if not math.isfinite(distance_km) or distance_km < 0:
        raise UiFailure("Invalid recorded prescription distance")
    exponent = math.floor(math.log10(distance_km)) if distance_km else 0
    half_bin_km = 0.5 * 10 ** (exponent - 5) if distance_km else 0.0
    # Below an exact decade (e.g. 0.01), six significant digits give ten
    # times finer spacing than above it. A symmetric bin admits other targets.
    lower_half_bin_km = half_bin_km / 10 if distance_km == 10 ** exponent else half_bin_km
    low_m = max(0.0, (distance_km - lower_half_bin_km) * 1000.0)
    high_m = (distance_km + half_bin_km) * 1000.0
    sections = document.get("course", {}).get("sections", [])
    values = set()
    for section, effort in zip(sections, training_effort_sections(document)):
        start, length = section["startDistanceMeters"], section["lengthMeters"]
        if not math.isfinite(start) or not math.isfinite(length) or length <= 0:
            raise UiFailure("Invalid persisted course distance section")
        low, high = max(start, low_m), min(start + length, high_m)
        if low > high or low >= start + length:
            continue
        samples = (low, math.nextafter(high, low) if high > low else low)
        endpoints = []
        for distance in samples:
            fraction = (distance - start) / length
            watts = effort["startWatts"] + fraction * (effort["endWatts"] - effort["startWatts"])
            value = training_effort_watts(document, watts, gear)
            endpoints.extend((math.trunc(value), math.floor(value + 0.5)))
        # Keep discontinuous sections separate: never fill the watts between
        # the two sides of a step, even when distance precision straddles it.
        values.update(range(min(endpoints), max(endpoints) + 1))
    if not values:
        raise UiFailure("Recorded distance is outside the prepared prescription")
    return values


def training_dispatches(text: str, expected_pid: int | None = None) -> list[dict]:
    """Pair real controller receipts with the subsequent coordinator trace."""
    pending = None
    result = []
    for line in text.splitlines():
        receiver = "gc-test-device event=load "
        target = "workout-game-trainer-target "
        if receiver in line:
            fields = dict(word.split("=", 1) for word in line.split(receiver, 1)[1].split()
                          if "=" in word)
            try:
                if expected_pid is not None and int(fields["pid"]) != expected_pid:
                    raise ValueError("receipt belongs to another application process")
                pending = {key: float(fields[key])
                           for key in ("mono_ms", "value", "accepted", "gear")}
                if (not all(math.isfinite(value) for value in pending.values())
                        or not pending["gear"].is_integer()
                        or not 1 <= pending["gear"] <= 12
                        or pending["value"] != pending["accepted"]):
                    raise ValueError("invalid receipt")
                pending["gear"] = int(pending["gear"])
            except (KeyError, ValueError) as error:
                raise UiFailure("Invalid Data Generator receipt") from error
        elif target in line:
            fields = dict(word.split("=", 1) for word in line.split(target, 1)[1].split()
                          if "=" in word)
            try:
                if (pending is None or fields["mode"] != "erg"
                        or fields["devices"] != "1"
                        or float(fields["value"]) != pending["value"]):
                    raise ValueError("unmatched dispatch")
                position = float(fields["workout_pos"])
                if not math.isfinite(position) or position < 0:
                    raise ValueError("invalid position")
            except (KeyError, ValueError) as error:
                raise UiFailure("Dispatch lacks a matching real controller receipt") from error
            result.append(dict(pending, workout_pos=position))
            pending = None
    return result


def validate_training_phase(before: str, after: str, commands: list[dict], *,
                            active: bool, gear: int, document: dict) -> dict:
    previous = training_recording_rows(before)
    current = training_recording_rows(after)
    if not after.startswith(before):
        raise UiFailure("Recording was replaced or rewritten during training")
    added = current[len(previous):]
    if not active:
        if added or commands:
            raise UiFailure("Recording or target dispatch continued while paused/stopped")
    else:
        if len(added) < 2 or len(commands) < 2:
            raise UiFailure("Active recording or target dispatch stalled")
        if any(command["gear"] != gear for command in commands):
            raise UiFailure("Trainer receipts have the wrong virtual gear")
        positions = [command["workout_pos"] for command in commands]
        if (positions[-1] <= positions[0]
                or any(right < left for left, right in zip(positions, positions[1:]))
                or added[-1]["km"] <= added[0]["km"]
                or any(row[key] <= 0 for row in added for key in ("cad", "hr", "watts"))):
            raise UiFailure("Active workout progression or generated telemetry stalled")
        for row in added:
            if row["virtualgear"] != gear:
                raise UiFailure("Recording does not match received targets/gear")
            expected = training_recorded_targets(document, row["km"], gear)
            if row["target"] not in expected:
                raise UiFailure(f"Recorded {row['target']} W at {row['km']} km; "
                                f"prescribed {sorted(expected)} W for gear {gear}")
        adjacent = ([previous[-1]] if previous else []) + added
        if any(right["secs"] - left["secs"] > 3
               for left, right in zip(adjacent, adjacent[1:])):
            raise UiFailure("Recording cadence stalled or included paused time")
    return {"new_rows": len(added), "commands": len(commands), "gear": gear,
            "active": active}


def validate_generated_workout(path: Path) -> dict:
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        start = lines.index("[COURSE DATA]") + 1
        end = lines.index("[END COURSE DATA]", start)
    except (OSError, UnicodeError, ValueError) as error:
        raise UiFailure(f"Invalid generated workout: {path}") from error

    points = []
    for line_number, line in enumerate(lines[start:end], start + 1):
        fields = line.split()
        if len(fields) != 2:
            raise UiFailure(
                f"Invalid generated workout point at line {line_number}"
            )
        try:
            minute, percent = map(float, fields)
        except ValueError as error:
            raise UiFailure(
                f"Invalid generated workout point at line {line_number}"
            ) from error
        if (not math.isfinite(minute) or not math.isfinite(percent)
                or minute < 0.0 or not 20.0 <= percent <= 250.0):
            raise UiFailure(
                f"Generated workout value is out of range at line {line_number}"
            )
        if points and minute < points[-1][0]:
            raise UiFailure("Generated workout time is not monotonic")
        points.append((minute, percent))

    if len(points) < 2 or points[0][0] != 0.0 or points[-1][0] <= 0.0:
        raise UiFailure("Generated workout course data is incomplete")
    return {
        "duration_minutes": points[-1][0],
        "minimum_percent": min(point[1] for point in points),
        "maximum_percent": max(point[1] for point in points),
        "point_count": len(points),
    }


def validate_mtb_course_sidecar(
        path: Path, expected_preset: str, expected_title: str) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        title = document["title"]
        source = document["source"]["intervals"]
        conversion = document["conversion"]
        preset = conversion["preset"]
        parameters = conversion["parameters"]
        course = document["course"]
        sections = course["sections"]
        road_plan = document["roadPlan"]
        road_pieces = road_plan["pieces"]
    except (OSError, UnicodeError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise UiFailure(f"Invalid MTB course metadata: {path}") from error
    if (
        not isinstance(title, str)
        or title != expected_title
        or preset != expected_preset
        or not isinstance(parameters, dict)
        or not isinstance(course, dict)
        or not isinstance(road_plan, dict)
        or not isinstance(road_pieces, list)
        or not road_pieces
        or not isinstance(source, list)
        or not isinstance(sections, list)
        or not source
        or len(sections) < len(source)
    ):
        raise UiFailure(
            "MTB course metadata or preset does not match: "
            f"expected={expected_preset!r}, actual={preset!r}, "
            f"source={len(source) if isinstance(source, list) else 'invalid'}, "
            f"sections={len(sections) if isinstance(sections, list) else 'invalid'}"
        )

    source_fields = ("startMs", "durationMs", "startWatts", "endWatts")
    duration_ms = 0
    expected_start_ms = 0
    section_index = 0
    for index, interval in enumerate(source):
        try:
            source_values = tuple(interval[field] for field in source_fields)
        except (KeyError, TypeError) as error:
            raise UiFailure(
                f"MTB course prescription fields are missing at interval {index}"
            ) from error
        times = source_values[:2]
        watts = source_values[2:]
        if (any(isinstance(value, bool) or not isinstance(value, int)
                       for value in times)
                or source_values[0] != expected_start_ms
                or source_values[1] <= 0
                or any(isinstance(value, bool)
                       or not isinstance(value, (int, float))
                       or not math.isfinite(value)
                       or value < 0.0 for value in watts)):
            raise UiFailure(
                f"MTB course prescription changed at interval {index}"
            )

        source_start_ms, source_duration_ms = times
        source_end_ms = source_start_ms + source_duration_ms
        expected_section_start_ms = source_start_ms
        interval_section_count = 0
        while section_index < len(sections):
            section = sections[section_index]
            try:
                section_start_ms = section["sourceStartMs"]
                section_duration_ms = section["nominalDurationMs"]
                section_start_watts = section["targetStartWatts"]
                section_end_watts = section["targetEndWatts"]
            except (KeyError, TypeError) as error:
                raise UiFailure(
                    "MTB course prescription fields are missing at section "
                    f"{section_index}"
                ) from error
            if (isinstance(section_start_ms, bool)
                    or not isinstance(section_start_ms, int)
                    or isinstance(section_duration_ms, bool)
                    or not isinstance(section_duration_ms, int)
                    or section_duration_ms <= 0):
                raise UiFailure(
                    f"MTB course prescription changed at section {section_index}"
                )
            if section_start_ms >= source_end_ms:
                break
            section_end_ms = section_start_ms + section_duration_ms
            numeric_watts = (section_start_watts, section_end_watts)
            if (section_start_ms != expected_section_start_ms
                    or section_end_ms > source_end_ms
                    or any(isinstance(value, bool)
                           or not isinstance(value, (int, float))
                           or not math.isfinite(value)
                           or value < 0.0 for value in numeric_watts)):
                raise UiFailure(
                    f"MTB course prescription changed at section {section_index}"
                )

            start_fraction = (
                (section_start_ms - source_start_ms) / source_duration_ms
            )
            end_fraction = (
                (section_end_ms - source_start_ms) / source_duration_ms
            )
            expected_start_watts = (
                watts[0] + (watts[1] - watts[0]) * start_fraction
            )
            expected_end_watts = (
                watts[0] + (watts[1] - watts[0]) * end_fraction
            )
            if (not math.isclose(section_start_watts, expected_start_watts,
                                 rel_tol=1e-9, abs_tol=1e-6)
                    or not math.isclose(section_end_watts, expected_end_watts,
                                        rel_tol=1e-9, abs_tol=1e-6)):
                raise UiFailure(
                    f"MTB course prescription changed at section {section_index}"
                )
            expected_section_start_ms = section_end_ms
            interval_section_count += 1
            section_index += 1

        if interval_section_count == 0 or expected_section_start_ms != source_end_ms:
            raise UiFailure(
                f"MTB course prescription changed at interval {index}"
            )
        duration_ms += source_values[1]
        expected_start_ms += source_values[1]

    if section_index != len(sections):
        raise UiFailure(
            f"MTB course prescription has {len(sections) - section_index} "
            "unmatched sections"
        )

    route_payload = {"course": course, "roadPlan": road_plan}
    try:
        route_bytes = json.dumps(
            route_payload,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        ).encode("utf-8")
        grades = [float(section["gradePercent"]) for section in sections]
        terrains = [section["terrain"] for section in sections]
        turns = [float(piece["turnRadians"]) for piece in road_pieces]
        grade_scale = float(parameters["gradeScale"])
        technicality = float(parameters["technicality"])
    except (KeyError, TypeError, ValueError) as error:
        raise UiFailure(f"Invalid MTB route metadata: {path}") from error
    numeric_route_values = grades + turns + [grade_scale, technicality]
    if (any(not math.isfinite(value) for value in numeric_route_values)
            or any(not isinstance(terrain, str) or not terrain
                   for terrain in terrains)):
        raise UiFailure(f"Invalid MTB route values: {path}")

    return {
        "title": title,
        "preset": preset,
        "interval_count": len(source),
        "duration_ms": duration_ms,
        "source_intervals": source,
        "route_fingerprint": hashlib.sha256(route_bytes).hexdigest(),
        "grade_scale": grade_scale,
        "technicality": technicality,
        "technical_section_count": sum(
            terrain != "smooth-trail" for terrain in terrains
        ),
        "maximum_absolute_grade_percent": max(map(abs, grades)),
        "total_absolute_turn_radians": sum(map(abs, turns)),
    }


class UiDriver:
    def __init__(self, root: Path, artifacts: Path, app_pgid: int):
        import pyatspi
        from Xlib import X, XK, display
        from Xlib.ext import xtest

        self.pyatspi = pyatspi
        self.X = X
        self.XK = XK
        self.display = display.Display()
        self.xtest = xtest
        self.root_path = root
        self.artifacts = artifacts
        self.app_pgid = app_pgid
        self.app = self.wait_for_application()

    def all_nodes(self, node=None):
        node = self.app if node is None else node
        yield node
        if self.name(node) == "Workout game 3D canvas":
            return
        try:
            children = list(node)
        except Exception:
            return
        for child in children:
            yield from self.all_nodes(child)

    @staticmethod
    def role(node) -> str:
        try:
            return node.getRoleName()
        except Exception:
            return ""

    @staticmethod
    def name(node) -> str:
        try:
            return node.name or ""
        except Exception:
            return ""

    @staticmethod
    def description(node) -> str:
        try:
            return node.description or ""
        except Exception:
            return ""

    def showing(self, node) -> bool:
        try:
            return node.getState().contains(self.pyatspi.STATE_SHOWING)
        except Exception:
            return False

    def enabled(self, node) -> bool:
        try:
            return node.getState().contains(self.pyatspi.STATE_ENABLED)
        except Exception:
            return False

    def selected(self, node) -> bool:
        try:
            return node.getState().contains(self.pyatspi.STATE_SELECTED)
        except Exception:
            return False

    def checked(self, node) -> bool:
        try:
            return node.getState().contains(self.pyatspi.STATE_CHECKED)
        except Exception:
            return False

    def selectable(self, node) -> bool:
        try:
            return node.getState().contains(self.pyatspi.STATE_SELECTABLE)
        except Exception:
            return False

    def wait_for_application(self, timeout=30.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            desktop = self.pyatspi.Registry.getDesktop(0)
            for app in desktop:
                try:
                    if not process_belongs_to_group(
                        int(app.get_process_id()), self.app_pgid
                    ):
                        continue
                    if any(
                        child.getRoleName() == "frame"
                        and child.name == ATHLETE
                        for child in app
                    ):
                        return app
                except Exception:
                    pass
            time.sleep(0.2)
        raise UiFailure("GoldenCheetah main window did not appear")

    def find_all(self, name=None, role=None, showing=None):
        matches = []
        for node in self.all_nodes():
            if name is not None and self.name(node) != name:
                continue
            if role is not None and self.role(node) != role:
                continue
            if showing is not None and self.showing(node) != showing:
                continue
            matches.append(node)
        return matches

    def find(self, name=None, role=None, showing=None, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            matches = self.find_all(name, role, showing)
            if matches:
                return matches[-1] if showing else matches[0]
            time.sleep(0.15)
        raise UiFailure(
            f"Accessible object not found: name={name!r}, role={role!r}, "
            f"showing={showing!r}"
        )

    def find_named_any(self, names, role=None, showing=None, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for name in names:
                matches = self.find_all(name, role, showing)
                if matches:
                    return matches[-1] if showing else matches[0]
            time.sleep(0.15)
        raise UiFailure(
            "Accessible object not found with any name: " + ", ".join(names)
        )

    def find_enabled(self, name, role=None, showing=True, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for node in reversed(self.find_all(name, role, showing)):
                if self.enabled(node):
                    return node
            time.sleep(0.1)
        raise UiFailure(
            f"Enabled accessible object not found: name={name!r}, "
            f"role={role!r}"
        )

    def require_interactive_controls(self, controls, timeout=5.0, scope=None):
        def find_control(name, role):
            if scope is None:
                return self.find(
                    name, role, showing=True, timeout=timeout
                )
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                matches = [
                    node for node in self.all_nodes(scope)
                    if self.name(node) == name
                    and self.role(node) == role
                    and self.showing(node)
                ]
                if matches:
                    return matches[-1]
                time.sleep(0.05)
            raise UiFailure(
                f"Accessible dialog control not found: {role} {name!r}"
            )

        first_name, first_role = controls[0]
        first = find_control(first_name, first_role)
        try:
            self.click(first)
            self.send_named_key("Home")
            self.send_named_key("Return")
        except Exception as error:
            raise UiFailure(
                f"Cannot focus {first_role} {first_name!r}"
            ) from error

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.name(first) != first_name:
                break
            time.sleep(0.1)
        else:
            raise UiFailure("Training focus is not keyboard-operable")

        for name, role in controls[1:]:
            node = find_control(name, role)
            if role in ("spin button", "slider"):
                before = self.current_value(node)
                value = node.queryValue()
                delta = -1 if before >= float(value.maximumValue) else 1
                try:
                    self.set_value(node, before + delta, timeout)
                except UiFailure as error:
                    raise UiFailure(
                        f"Cannot operate {role} {name!r}"
                    ) from error
                self.set_value(node, before)
                continue

            if role == "check box":
                before = self.checked(node)
                self.click(node)
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if self.checked(node) != before:
                        break
                    time.sleep(0.05)
                else:
                    raise UiFailure(f"Cannot operate {role} {name!r}")
                self.click(node)
                continue

            raise UiFailure(f"Unsupported keyboard-control role: {role}")

    def require_names(self, names, role=None, timeout=10.0):
        deadline = time.monotonic() + timeout
        missing = list(names)
        while missing and time.monotonic() < deadline:
            found = {
                self.name(node)
                for node in self.all_nodes()
                if (role is None or self.role(node) == role)
            }
            missing = [name for name in names if name not in found]
            if missing:
                time.sleep(0.15)
        if missing:
            raise UiFailure(f"Missing accessible controls: {', '.join(missing)}")

    def require_visible_names(self, names, timeout=10.0):
        deadline = time.monotonic() + timeout
        missing = list(names)
        while missing and time.monotonic() < deadline:
            missing = [
                name for name in names
                if not self.find_all(name=name, showing=True)
            ]
            if missing:
                time.sleep(0.15)
        if missing:
            raise UiFailure(
                f"Missing visible accessible content: {', '.join(missing)}"
            )

    def refresh_accessible(self, node, name, role, showing, timeout=1.0):
        if not name and not role:
            return node
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            matches = self.find_all(
                name=name or None,
                role=role or None,
                showing=True if showing else None,
            )
            if matches:
                return matches[-1] if showing else matches[0]
            time.sleep(0.1)
        return node

    def _activate_once(self, node):
        if not self.enabled(node):
            raise UiFailure("accessible control is disabled or stale")
        try:
            action = node.queryAction()
            for index in range(action.nActions):
                if action.doAction(index):
                    return
        except Exception:
            pass
        try:
            node.queryComponent().grabFocus()
            parent = node.parent
            parent.querySelection().selectChild(node.getIndexInParent())
            return
        except Exception as error:
            raise UiFailure("accessible action is stale") from error

    def activate(self, node):
        name, role, showing = self._accessible_metadata(node)
        last_error = None
        for attempt in range(2):
            try:
                self._activate_once(node)
                return
            except UiFailure as error:
                last_error = error
                if attempt == 0:
                    node = self.refresh_accessible(
                        node, name, role, showing
                    )
        raise UiFailure(f"Cannot activate {role} {name!r}") from last_error

    def _mouse_click(self, node, button):
        name, role, showing = self._accessible_metadata(node)
        last_error = None
        for attempt in range(2):
            try:
                self._mouse_click_once(node, button)
                return
            except Exception as error:
                last_error = error
                if attempt == 0:
                    node = self.refresh_accessible(
                        node, name, role, showing
                    )
        raise UiFailure(f"Cannot click {role} {name!r}") from last_error

    def _accessible_metadata(self, node):
        last_error = None
        for attempt in range(2):
            try:
                return (
                    node.name or "",
                    node.getRoleName(),
                    node.getState().contains(self.pyatspi.STATE_SHOWING),
                )
            except Exception as error:
                last_error = error
                if attempt == 0:
                    time.sleep(0.05)
        raise UiFailure("Cannot inspect accessible control") from last_error

    def _mouse_click_once(self, node, button):
        try:
            bounds = node.queryComponent().getExtents(
                self.pyatspi.DESKTOP_COORDS
            )
            if bounds.width <= 0 or bounds.height <= 0:
                raise ValueError("empty accessible bounds")
            x = bounds.x + bounds.width // 2
            y = bounds.y + bounds.height // 2
            self.xtest.fake_input(self.display, self.X.MotionNotify, x=x, y=y)
            self.xtest.fake_input(self.display, self.X.ButtonPress, button)
            self.xtest.fake_input(self.display, self.X.ButtonRelease, button)
            self.display.sync()
        except Exception as error:
            raise UiFailure("accessible bounds are stale") from error

    def click(self, node):
        self._mouse_click(node, 1)

    def context_click(self, node):
        self._mouse_click(node, 3)

    def activate_named(self, name, role=None, showing=None, timeout=30.0):
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            try:
                node = self.find(
                    name=name,
                    role=role,
                    showing=showing,
                    timeout=min(1.0, max(0.1, deadline - time.monotonic())),
                )
                self.activate(node)
                return
            except Exception as error:
                last_error = error
                time.sleep(0.2)
        raise UiFailure(f"Cannot activate {role or 'control'} {name!r}") from last_error

    def activate_view(self, name, timeout=30.0, ready_names=None):
        deadline = time.monotonic() + timeout
        last_error = None
        ready_names = tuple(ready_names or (name,))
        while time.monotonic() < deadline:
            try:
                controls = [
                    node for node in self.find_all(
                        name=name, role="menu item"
                    )
                    if self.enabled(node)
                ]
                if not controls:
                    raise UiFailure(f"Enabled view control is unavailable: {name!r}")
                controls.sort(key=self.showing, reverse=True)
                for control in controls:
                    self.activate(control)
                    if self.selected(control) or self.checked(control):
                        return
                    for ready_name in ready_names:
                        destinations = [
                            node for node in self.find_all(
                                name=ready_name, showing=True
                            )
                            if self.role(node) != "menu item"
                        ]
                        if destinations:
                            return
                raise UiFailure(
                    f"View destination is not visible for {name!r}"
                )
            except Exception as error:
                last_error = error
            time.sleep(0.2)
        raise UiFailure(f"Cannot open view {name!r}") from last_error

    def select_named(self, name, timeout=10.0):
        deadline = time.monotonic() + timeout
        nodes = [
            node for node in self.find_all(name=name, showing=True)
            if self.role(node) in ("list item", "table cell")
        ]
        last_error = None
        for node in reversed(nodes):
            try:
                self.select_accessible_item(
                    node,
                    timeout=min(1.0, max(0.1, deadline - time.monotonic())),
                )
                return
            except UiFailure as error:
                last_error = error
                continue

        for node in reversed(nodes):
            try:
                parent = node.parent
                index = node.getIndexInParent()
                selection = parent.querySelection()
                self.click(node)
                confirmation_deadline = min(
                    deadline, time.monotonic() + 1.0
                )
                while time.monotonic() < confirmation_deadline:
                    if selection.isChildSelected(index):
                        return
                    time.sleep(0.1)
                last_error = UiFailure(
                    f"Mouse click did not select {name!r}"
                )
            except Exception as error:
                last_error = error
        if nodes:
            raise UiFailure(f"Selectable item did not select {name!r}") \
                from last_error
        for combo in self.find_all(role="combo box"):
            if not any(
                self.role(node) == "list item" and self.name(node) == name
                for node in self.all_nodes(combo)
            ):
                continue
            self.activate(combo)
            item = self.find(
                name=name,
                role="list item",
                showing=True,
                timeout=timeout,
            )
            self.activate(item)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                if self.name(combo) == name:
                    return
                time.sleep(0.1)
            raise UiFailure(f"Combo box did not select {name!r}")
        raise UiFailure(f"Cannot select {name!r}")

    def select_accessible_item(self, node, timeout=1.0):
        name, role, showing = self._accessible_metadata(node)
        last_error = None
        for attempt in range(2):
            try:
                if not self.enabled(node):
                    raise UiFailure("accessible item is disabled or stale")
                parent = node.parent
                index = node.getIndexInParent()
                selection = parent.querySelection()
                if index < 0 or not selection.selectChild(index):
                    raise UiFailure("accessible container rejected selection")
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if selection.isChildSelected(index):
                        return
                    time.sleep(0.1)
                raise UiFailure("accessible container did not retain selection")
            except Exception as error:
                last_error = error
                if attempt == 0:
                    node = self.refresh_accessible(
                        node, name, role, showing
                    )
        raise UiFailure(f"Cannot select {role} {name!r}") from last_error

    def click_named_item(self, name):
        for node in reversed(self.find_all(name=name, showing=True)):
            if self.role(node) in ("list item", "table cell"):
                self.click(node)
                time.sleep(0.5)
                return node
        raise UiFailure(f"Cannot click selectable item {name!r}")

    def select_named_item_exact(self, name):
        return self.click_named_item(name)

    def right_click_named_item(self, name):
        for node in reversed(self.find_all(name=name, showing=True)):
            if self.role(node) in ("list item", "table cell"):
                self.context_click(node)
                time.sleep(0.5)
                return
        raise UiFailure(f"Cannot context-click selectable item {name!r}")

    def combo_with_items(
            self, expected, timeout=15.0, require_interactable=False):
        expected = set(expected)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            named_candidates = []
            for combo in self.find_all(role="combo box"):
                if not self.showing(combo):
                    continue
                if require_interactable and not self.enabled(combo):
                    continue
                if self.name(combo) in expected:
                    named_candidates.append(combo)
                descendants = {
                    self.name(node)
                    for node in self.all_nodes(combo)
                    if self.role(node) == "list item"
                }
                if expected.issubset(descendants):
                    return combo
            if len(named_candidates) == 1:
                return named_candidates[0]
            time.sleep(0.15)
        raise UiFailure(f"Perspective selector lacks: {sorted(expected)!r}")

    def find_combo_item(self, combo, name, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            matches = [
                node
                for node in self.all_nodes(combo)
                if self.role(node) == "list item"
                and self.name(node) == name
                and self.showing(node)
            ]
            if matches:
                return matches[-1]
            time.sleep(0.1)
        raise UiFailure(f"Combo box item did not appear: {name!r}")

    def select_combo_item(self, expected, name, timeout=10.0):
        combo = self.combo_with_items(expected, require_interactable=True)
        self.focus_main_window()
        self.click(combo)
        item = self.find_combo_item(combo, name, timeout)
        self.click(item)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.name(combo) == name or self.selected(item):
                return combo
            time.sleep(0.1)
        self.screenshot("combo-selection-failed")
        raise UiFailure(f"Combo box did not select {name!r}")

    def combo_selects(self, combo, name):
        return self.name(combo) == name or any(
            self.role(node) == "list item"
            and self.name(node) == name
            and self.selected(node)
            for node in self.all_nodes(combo)
        )

    def focus_main_window(self):
        windows = [self.display.screen().root]
        while windows:
            window = windows.pop()
            try:
                if window.get_wm_name() == ATHLETE:
                    window.set_input_focus(
                        self.X.RevertToParent, self.X.CurrentTime
                    )
                    self.display.sync()
                    return
                windows.extend(window.query_tree().children)
            except Exception:
                continue
        raise UiFailure("GoldenCheetah X11 window was not found")

    def current_value(self, node) -> float:
        try:
            return float(node.queryValue().currentValue)
        except Exception as error:
            raise UiFailure(f"No numeric value for {self.name(node)!r}") from error

    def set_value(self, node, expected, timeout=5.0):
        try:
            value = node.queryValue()
            value.currentValue = expected
        except Exception as error:
            raise UiFailure(
                f"Cannot set numeric value for {self.name(node)!r}"
            ) from error
        self.wait_value(node, expected, timeout)

    def send_key(self, text: str):
        self.focus_main_window()
        keycode = self.display.keysym_to_keycode(ord(text.lower()))
        self.xtest.fake_input(self.display, self.X.KeyPress, keycode)
        self.xtest.fake_input(self.display, self.X.KeyRelease, keycode)
        self.display.sync()

    def send_named_key(self, name: str):
        keysym = self.XK.string_to_keysym(name)
        keycode = self.display.keysym_to_keycode(keysym)
        if not keysym or not keycode:
            raise UiFailure(f"X11 key is unavailable: {name}")
        self.xtest.fake_input(self.display, self.X.KeyPress, keycode)
        self.xtest.fake_input(self.display, self.X.KeyRelease, keycode)
        self.display.sync()

    def activate_popup_item(self, zero_based_index: int):
        if zero_based_index < 0:
            raise UiFailure("Popup menu index must not be negative")
        self.send_named_key("Home")
        for unused in range(zero_based_index):
            self.send_named_key("Down")
        self.send_named_key("Return")

    def screenshot(self, name: str, node=None):
        screen = self.display.screen()
        x = 0
        y = 0
        width = screen.width_in_pixels
        height = screen.height_in_pixels
        if node is not None:
            bounds = node.queryComponent().getExtents(
                self.pyatspi.DESKTOP_COORDS
            )
            x = max(0, bounds.x)
            y = max(0, bounds.y)
            width = min(bounds.width, screen.width_in_pixels - x)
            height = min(bounds.height, screen.height_in_pixels - y)
            if width < 64 or height < 64:
                raise UiFailure("Workout game canvas has invalid bounds")
        image = screen.root.get_image(
            x, y, width, height, self.X.ZPixmap, 0xFFFFFFFF
        )
        data = image.data
        rgb = x11_bgrx_to_rgb(data)
        output = self.artifacts / f"{name}.ppm"
        with output.open("wb") as handle:
            handle.write(f"P6\n{width} {height}\n255\n".encode("ascii"))
            handle.write(rgb)
        if len(set(rgb[:: max(3, len(rgb) // 5000)])) < 4:
            raise UiFailure(f"Screenshot appears blank: {output}")
        return width, height, bytes(rgb)

    @staticmethod
    def changed_pixels(
        first,
        second,
        top_ratio=0.20,
        bottom_ratio=0.92,
        side_ratio=0.30,
        sample_step=2,
    ) -> int:
        first_width, first_height = first[:2]
        second_width, second_height = second[:2]
        width = min(first_width, second_width)
        height = min(first_height, second_height)
        first_rgb = first[2]
        second_rgb = second[2]
        changed = 0
        top = int(height * top_ratio)
        bottom = min(height, int(height * bottom_ratio))
        side = int(width * side_ratio)
        for y in range(top, bottom, sample_step):
            for x in range(0, width, sample_step):
                if side <= x < width - side:
                    continue
                first_offset = (y * first_width + x) * 3
                second_offset = (y * second_width + x) * 3
                if (
                    first_rgb[first_offset : first_offset + 3]
                    != second_rgb[second_offset : second_offset + 3]
                ):
                    changed += 1
        return changed

    def wait_value(self, node, expected, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.current_value(node) == expected:
                return
            time.sleep(0.1)
        raise UiFailure(
            f"Expected {self.name(node)!r} value {expected}, got "
            f"{self.current_value(node)}"
        )

    def wait_file(self, path: Path, timeout=8.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if path.is_file() and path.stat().st_size > 40:
                return
            time.sleep(0.1)
        raise UiFailure(f"Workout was not saved: {path}")

    def wait_new_file(
        self, directory: Path, existing: set[Path], pattern: str, timeout=8.0
    ) -> Path:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            candidates = {
                path for path in directory.glob(pattern)
                if path.is_file() and path.stat().st_size > 0
            } - existing
            if len(candidates) == 1:
                return candidates.pop()
            if len(candidates) > 1:
                raise UiFailure(
                    f"Expected one new {pattern} file in {directory}, "
                    f"found {len(candidates)}"
                )
            time.sleep(0.1)
        raise UiFailure(f"No new {pattern} file appeared in {directory}")

    def wait_file_growth(self, path: Path, initial_size: int, timeout=8.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if path.is_file() and path.stat().st_size > initial_size:
                return
            time.sleep(0.1)
        raise UiFailure(f"Recording did not resume: {path}")

    def wait_file_removed(self, path: Path, timeout=8.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not path.exists():
                return
            time.sleep(0.1)
        raise UiFailure(f"Discarded recording still exists: {path}")

    def reopen_saved_activity(self, activity: Path, timeout=20.0) -> str:
        if not activity.is_file() or activity.stat().st_size == 0:
            raise UiFailure(f"Saved activity is unavailable: {activity}")
        activities = {
            path.resolve()
            for path in activity.parent.glob("*.json")
            if path.is_file() and path.stat().st_size > 0
        }
        if activities != {activity.resolve()}:
            raise UiFailure(
                "Saved-activity UI verification requires exactly one isolated "
                "activity"
            )

        self.activate_named("Train", "menu item", timeout=timeout)
        self.activate_view(
            "Activities", timeout=timeout, ready_names=("Activities view",)
        )
        expected_description = f"Selected activity {activity.name}"

        def matching_activity_description() -> str:
            activities_views = self.find_all(
                name="Activities view", showing=True
            )
            if any(
                self.description(node) == expected_description
                for node in activities_views
            ):
                return expected_description
            return ""

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            selected_activity = matching_activity_description()
            if selected_activity:
                return selected_activity
            candidates = []
            for role in ("table", "tree table"):
                for table in self.find_all(role=role, showing=True):
                    candidates.extend(
                        node
                        for node in self.all_nodes(table)
                        if self.role(node) in ("table cell", "list item")
                        and self.showing(node)
                        and self.enabled(node)
                        and self.selectable(node)
                    )
            for node in candidates:
                try:
                    self.activate(node)
                except UiFailure:
                    continue
                selected_deadline = min(deadline, time.monotonic() + 2.0)
                while time.monotonic() < selected_deadline:
                    if self.selected(node):
                        selected_activity = matching_activity_description()
                        if selected_activity:
                            return selected_activity
                        break
                    time.sleep(0.1)
            time.sleep(0.2)
        raise UiFailure(
            "Saved activity exists in the isolated library, but the Activities "
            "view exposes no selectable activity row through AT-SPI: "
            f"{activity.name}"
        )


class MtbCourseUiWorkflow:
    PRESET_CONTROL_NAMES = {
        "workout-first": "Workout first",
        "balanced": "Balanced",
        "ride-first": "Ride first",
    }
    CONTEXT_ACTION_STEPS = {
        "Create MTB Course": 5,
        "Edit MTB Course": 5,
    }

    def __init__(
        self,
        driver: UiDriver,
        root: Path,
        artifacts: Path,
        capture_screenshots: bool,
        enter_train,
        ride_course=None,
    ):
        self.driver = driver
        self.artifacts = artifacts
        self.capture_screenshots = capture_screenshots
        self.enter_train = enter_train
        self.ride_course = ride_course
        self.workouts = root / "library" / ATHLETE / "workouts"
        self.course_path = self.workouts / "ui-test-mtb.crs"
        self.sidecar_path = self.workouts / "ui-test-mtb.gcmtb.json"
        self.title = "ui-test MTB"
        self.workout_name = "ui-test-mtb"

    def run(self) -> dict:
        results = []
        for operation, preset in (
            (self.create, "workout-first"),
            (self.edit, "balanced"),
            (self.edit, "ride-first"),
        ):
            result = operation(preset)
            results.append(result)
            ride_course = getattr(self, "ride_course", None)
            if ride_course is not None:
                ride_course(preset, result)
        prescriptions = [result["source_intervals"] for result in results]
        if prescriptions[0] != prescriptions[1] or prescriptions[1] != prescriptions[2]:
            raise UiFailure("Create MTB Course changed the source prescription")
        fingerprints = [result["route_fingerprint"] for result in results]
        if len(set(fingerprints)) != len(fingerprints):
            raise UiFailure("Create MTB Course presets produced identical routes")
        for metric in ("grade_scale", "technicality", "total_absolute_turn_radians"):
            values = [result[metric] for result in results]
            if not values[0] < values[1] < values[2]:
                raise UiFailure(
                    f"Create MTB Course preset ordering is invalid for {metric}: "
                    f"{values!r}"
                )
        technical_counts = [
            result["technical_section_count"] for result in results
        ]
        if not technical_counts[0] < technical_counts[1] < technical_counts[2]:
            raise UiFailure(
                "Create MTB Course technical terrain ordering is invalid: "
                f"{technical_counts!r}"
            )
        write_text(
            self.artifacts / "mtb-course-ui-summary.json",
            json.dumps(
                [
                    {
                        key: value
                        for key, value in result.items()
                        if key != "source_intervals"
                    }
                    for result in results
                ],
                indent=2,
                sort_keys=True,
            )
            + "\n",
        )
        return results[-1]

    def _context_action(self, workout_name: str, action_name: str) -> None:
        self.enter_train()
        self.driver.right_click_named_item(workout_name)
        try:
            action_steps = self.CONTEXT_ACTION_STEPS[action_name]
        except KeyError as error:
            raise UiFailure(f"Unsupported workout context action: {action_name}") from error
        self.driver.activate_popup_item(action_steps)
        try:
            self.driver.find(action_name, "dialog", showing=True, timeout=30.0)
        except UiFailure:
            cancel = self.driver.find_all(
                name="Cancel", role="push button", showing=True
            )
            if cancel:
                self.driver.activate(cancel[-1])
            raise

    def _select_preset(self, preset: str) -> None:
        try:
            control_name = self.PRESET_CONTROL_NAMES[preset]
        except KeyError as error:
            raise UiFailure(f"Unsupported MTB course preset: {preset}") from error
        control = self.driver.find(control_name, showing=True, timeout=30.0)
        self.driver.click(control)
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            if self.driver.checked(control):
                return
            time.sleep(0.1)
        raise UiFailure(f"MTB course preset did not become selected: {preset}")

    def _preserve_and_validate(self, preset: str) -> dict:
        shutil.copy2(
            self.sidecar_path,
            self.artifacts / f"mtb-course-{preset}.gcmtb.json",
        )
        result = validate_mtb_course_sidecar(
            self.sidecar_path, preset, self.title
        )
        result["workout_name"] = self.workout_name
        return result

    def create(self, preset: str) -> dict:
        self._context_action("ui-test", "Create MTB Course")
        self._select_preset(preset)
        if self.capture_screenshots:
            self.driver.screenshot(f"03-mtb-course-create-{preset}")
        self.driver.activate(
            self.driver.find(
                "Create Course", "push button", showing=True, timeout=30.0
            )
        )
        self.driver.wait_file(self.course_path, timeout=20.0)
        self.driver.wait_file(self.sidecar_path, timeout=20.0)
        self.driver.find(
            self.workout_name, "table cell", showing=True, timeout=30.0
        )
        return self._preserve_and_validate(preset)

    def edit(self, preset: str) -> dict:
        self._context_action(self.workout_name, "Edit MTB Course")
        before = self.sidecar_path.read_bytes()
        self._select_preset(preset)
        if self.capture_screenshots:
            self.driver.screenshot(f"03-mtb-course-edit-{preset}")
        self.driver.activate(
            self.driver.find(
                "Save Course", "push button", showing=True, timeout=30.0
            )
        )
        deadline = time.monotonic() + 20.0
        while time.monotonic() < deadline:
            try:
                if self.sidecar_path.read_bytes() != before:
                    break
            except OSError:
                pass
            time.sleep(0.1)
        else:
            raise UiFailure(f"MTB course metadata did not change to {preset}")
        self.driver.find(
            self.workout_name, "table cell", showing=True, timeout=30.0
        )
        return self._preserve_and_validate(preset)


class WorkoutGameUiWorkflow:
    def __init__(
        self,
        driver: UiDriver,
        root: Path,
        artifacts: Path,
        capture_screenshots: bool,
        enter_train,
        workout_names=(),
    ):
        self.driver = driver
        self.root = root
        self.artifacts = artifacts
        self.capture_screenshots = capture_screenshots
        self.enter_train = enter_train
        self.records = root / "library" / ATHLETE / "records"
        self.activities = root / "library" / ATHLETE / "activities"
        self.existing_records: set[Path] = set()
        self.existing_activities: set[Path] = set()
        self.gear = None
        self.canvas = None
        self.first_frame = None
        self.initial_gear = 0.0
        self.stop_training_button = None
        self.workout_names = tuple(workout_names)
        run_seconds = game_run_seconds_from_environment()
        self.run_delays = (
            run_seconds / 4.0,
            run_seconds / 4.0,
            run_seconds / 2.0,
        )

    def run(self) -> Path:
        self.open_game()
        recording = self.start()
        self.shift_up()
        self.shift_down()
        self.stop_and_continue(recording)
        return self.stop_save_and_reopen(recording)

    def select_prepared_workout(self, timeout=20.0) -> None:
        deadline = time.monotonic() + timeout
        requested = getattr(self, "workout_names", ())
        workout_names = requested or (
            "Pre-release UI test", "ui-test.erg", "ui-test"
        )
        while time.monotonic() < deadline:
            for workout_name in workout_names:
                try:
                    self.driver.select_named_item_exact(workout_name)
                    return
                except UiFailure:
                    pass
            time.sleep(0.2)
        raise UiFailure(
            "Requested workout was not selectable: "
            + ", ".join(repr(name) for name in workout_names)
        )

    def open_game(self, workout_ride_expected=None, game_visible=True) -> None:
        self.enter_train()
        self.select_prepared_workout()

        self.driver.select_named("Data Generator")
        self.gear = self.driver.find(
            "Virtual gear", "spin button", showing=True
        )
        self.stop_training_button = self.driver.find(
            "Stop training", "push button", showing=True
        )
        if not self.driver.enabled(self.gear):
            self.driver.activate(
                self.driver.find(
                    "Connect training devices", "push button", showing=True
                )
            )
            try:
                self.gear = self.driver.find_enabled(
                    "Virtual gear", "spin button", showing=True, timeout=15.0
                )
                self.stop_training_button = self.driver.find(
                    "Stop training", "push button", showing=True
                )
            except UiFailure as error:
                raise UiFailure(
                    "Data Generator did not connect for Workout Game"
                ) from error

        if workout_ride_expected is not None:
            ride_mode = self.driver.combo_with_items(
                ["Standard ERG", "Workout Ride"]
            )
            if self.driver.enabled(ride_mode) != workout_ride_expected:
                availability = "available" if workout_ride_expected else "unavailable"
                raise UiFailure(
                    f"Workout Ride should be {availability} for this MTB preset"
                )
            if workout_ride_expected:
                deadline = time.monotonic() + 5.0
                while (not self.driver.combo_selects(ride_mode, "Workout Ride")
                       and time.monotonic() < deadline):
                    time.sleep(0.1)
                if not self.driver.combo_selects(ride_mode, "Workout Ride"):
                    raise UiFailure(
                        "Power-controlled MTB course did not automatically "
                        "select Workout Ride before opening Workout Game"
                    )

        if game_visible:
            self.driver.select_combo_item(
                ["Workout Game", "Workout Editor"], "Workout Game"
            )
        else:
            self.driver.select_combo_item(["Workout Editor"], "Workout Editor")
        if workout_ride_expected:
            deadline = time.monotonic() + 5.0
            while (not self.driver.combo_selects(ride_mode, "Workout Ride")
                   and time.monotonic() < deadline):
                time.sleep(0.1)
            if not self.driver.combo_selects(ride_mode, "Workout Ride"):
                raise UiFailure(
                    "Workout Game did not automatically select Workout Ride"
                )
        if game_visible:
            self.canvas = self.driver.find_named_any(
                WORKOUT_GAME_CANVAS_NAMES, showing=True
            )
            self.canvas_accessible_name = self.driver.name(self.canvas)
        else:
            self.canvas_accessible_name = "no game chart"
        self.existing_records = set(self.records.glob("*.csv"))
        self.existing_activities = set(self.activities.glob("*.json"))

    def start(self, screenshot_name="04-workout-game-first") -> Path:
        self.driver.activate(
            self.driver.find(
                "Start or pause training", "push button", showing=True
            )
        )
        recording = self.driver.wait_new_file(
            self.records, self.existing_records, "*.csv"
        )
        record_renderer_canvas_name(self.root, self.canvas_accessible_name)
        self.initial_gear = self.driver.current_value(self.gear)
        if self.capture_screenshots:
            time.sleep(1.2)
            self.first_frame = self.driver.screenshot(
                screenshot_name, self.canvas
            )
        return recording

    def run_smoke_and_discard(self, preset: str) -> None:
        self.open_game(workout_ride_expected=False)
        recording = self.start(f"04-mtb-course-{preset}-first")
        initial_size = recording.stat().st_size
        time.sleep(1.2)
        self.driver.wait_file_growth(recording, initial_size)
        if self.capture_screenshots:
            second = self.driver.screenshot(
                f"04-mtb-course-{preset}-running", self.canvas
            )
            if canvas_requires_pixel_motion(self.driver.name(self.canvas)):
                changed = self.driver.changed_pixels(self.first_frame, second)
                if changed < 1200:
                    raise UiFailure(
                        f"{preset} MTB course appears static: "
                        f"only {changed} sampled pixels changed"
                    )
        self.activate_stop_training()
        self.driver.activate(
            self.driver.find(
                "Cancel", "push button", showing=True, timeout=30.0
            )
        )
        self.driver.wait_file_removed(recording)

    def shift_up(self) -> None:
        time.sleep(self.run_delays[0])
        self.driver.send_key("w")
        self.driver.wait_value(self.gear, self.initial_gear + 1)

    def shift_down(self) -> None:
        time.sleep(self.run_delays[1])
        self.driver.send_key("s")
        self.driver.wait_value(self.gear, self.initial_gear)

    def activate_stop_training(self) -> None:
        cached = getattr(self, "stop_training_button", None)
        if cached is not None:
            try:
                self.driver.activate(cached)
                return
            except UiFailure:
                self.stop_training_button = None
        self.stop_training_button = self.driver.find(
            "Stop training", "push button", showing=True
        )
        self.driver.activate(self.stop_training_button)

    def stop_and_continue(self, recording: Path) -> None:
        time.sleep(self.run_delays[2])
        if self.capture_screenshots:
            second = self.driver.screenshot(
                "04-workout-game-running", self.canvas
            )
            if canvas_requires_pixel_motion(self.driver.name(self.canvas)):
                changed = self.driver.changed_pixels(self.first_frame, second)
                if changed < 1200:
                    raise UiFailure(
                        "Workout Game appears static: "
                        f"only {changed} sampled pixels changed"
                    )
        elif os.environ.get("GC_UI_REQUIRE_QUICK3D_EVIDENCE") == "1":
            # Synchronous X11 readback can perturb the measured cold-start
            # window, so capture nonblank evidence only after that window.
            # Motion remains trace-authoritative: readback itself can block
            # long enough for a short Feature Lab course to finish.
            self.driver.screenshot(
                "04-workout-game-quick3d-post-cold-start-first"
            )
            time.sleep(0.4)
            self.driver.screenshot(
                "04-workout-game-quick3d-post-cold-start-second"
            )

        self.activate_stop_training()
        paused_size = recording.stat().st_size
        self.activate_stop_dialog_button("Continue Training")
        self.stop_training_button = self.driver.find_enabled(
            "Stop training", "push button", showing=True, timeout=10.0
        )
        self.driver.wait_file_growth(recording, paused_size)
        if self.capture_screenshots:
            self.driver.screenshot("05-workout-game-continued")

    def activate_stop_dialog_button(self, name):
        try:
            button = self.driver.find(
                name, "push button", showing=True, timeout=30.0
            )
        except UiFailure:
            # Qt can retain a stale AT-SPI SHOWING state when this dialog is
            # hidden and shown again. The subsequent workflow assertions still
            # verify that selecting the exact named control had an effect.
            self.driver.find(name, "push button", timeout=1.0)
            steps = {
                "Continue Training": (),
                "Save": ("Tab", "Tab"),
                "Finish": (),
            }
            try:
                keys = steps[name]
            except KeyError as error:
                raise UiFailure(
                    f"No keyboard fallback for stop dialog button {name!r}"
                ) from error
            for key in keys:
                self.driver.send_named_key(key)
            self.driver.send_named_key("Return")
            return
        self.driver.click(button)

    def stop_save_and_reopen(self, recording: Path) -> Path:
        time.sleep(1.0)
        self.activate_stop_training()
        # Save becomes visible while the import pass is still finishing.  The
        # continuation control is exposed only after TrainSidebar has armed
        # the dialog's save/discard signals, so use it as the readiness gate.
        self.driver.find(
            "Continue Training", "push button", showing=True, timeout=30.0
        )
        self.activate_stop_dialog_button("Save")
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            try:
                stop = self.driver.find(
                    "Stop training", "push button", showing=True, timeout=1.0
                )
            except UiFailure:
                break
            if not self.driver.enabled(stop):
                break
            time.sleep(0.1)
        else:
            raise UiFailure("Training remained active after saving")
        activity = self.driver.wait_new_file(
            self.activities,
            self.existing_activities,
            "*.json",
            timeout=15.0,
        )
        self.activate_stop_dialog_button("Finish")
        selected_name = self.driver.reopen_saved_activity(activity)
        write_text(
            self.artifacts / "reopened-activity.txt",
            f"{activity.name}\n{selected_name}\n",
        )
        if self.capture_screenshots:
            self.driver.screenshot("06-workout-game-saved-and-reopened")
        if validate_trainer_acceptance_from_environment():
            preserve_game_recording(recording, self.artifacts)
        return activity


class TrainingFailureUiWorkflow(WorkoutGameUiWorkflow):
    """Real app exercise. Mocks of this class prove orchestration only."""

    def __init__(self, driver, root, artifacts, enter_train, case, document):
        super().__init__(driver, root, artifacts, False, enter_train, ("ui-test-mtb",))
        self.case = case
        self.document = document
        self.phases = []
        self.run_delays = (0.0, 0.0, 0.0)
        self.training_log_offset = 0

    def log_text(self):
        stderr = (self.artifacts / "application.log").read_text(encoding="utf-8", errors="replace")
        runtime = self.root / "library/goldencheetah.log"
        # Debug builds redirect stderr; release --debug normally stays on it.
        # Choose one stream so mirrored receipts can never be counted twice.
        if "gc-test-" not in stderr and runtime.is_file():
            if runtime.is_symlink() or f'"{runtime}"' not in stderr:
                raise UiFailure("Redirected log does not belong to this isolated athlete root")
            return runtime.read_text(encoding="utf-8", errors="replace")
        return stderr

    def run_failure_case(self):
        print(f"Training failure case {self.case}: selecting generated course", flush=True)
        self.open_game(workout_ride_expected=False, game_visible=self.case != "absent")
        reference = self.document["conversion"]["parameters"].get("referenceGear", 6)
        self.driver.set_value(self.gear, reference)
        self.training_log_offset = len(self.log_text())
        print(f"Training failure case {self.case}: starting recording", flush=True)
        recording = self.start()
        self.wait_initial_evidence(recording)
        self.wait_fault_evidence()
        self.hold_phase("initial", recording, active=True, gear=reference)
        self.driver.set_value(self.gear, reference + 1)
        self.hold_phase("gear-up", recording, active=True, gear=reference + 1)
        self.driver.set_value(self.gear, reference)
        self.hold_phase("gear-back", recording, active=True, gear=reference)
        self.toggle_pause()
        self.hold_phase("paused", recording, active=False, gear=reference)
        self.driver.set_value(self.gear, reference + 1)
        self.hold_phase("paused-gear", recording, active=False, gear=reference + 1)
        self.toggle_pause()
        self.hold_phase("resumed", recording, active=True, gear=reference + 1)
        self.stop_and_continue(recording)
        self.hold_phase("continued", recording, active=True, gear=reference + 1)
        self.stop_save_and_reopen(recording)
        self.hold_phase("stopped", recording, active=False, gear=reference + 1)
        self.finish_evidence(recording)

    def wait_initial_evidence(self, recording):
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            text = recording.read_text(encoding="utf-8")
            try:
                rows = training_recording_rows(text)
            except UiFailure:
                rows = []
            if len(rows) >= 2:
                log = self.log_text()
                if self.case == "runner" and "event=runner-unavailable " in log:
                    raise UiFailure("Runner fault preceded the initial recording evidence")
                commands = training_dispatches(log[self.training_log_offset:],
                                               expected_pid=self.driver.app_pgid)
                if len(commands) < 2:
                    raise UiFailure("Initial recording lacks real controller receipts")
                write_text(self.artifacts / "before-fault-recording.csv", text)
                return
            time.sleep(0.1)
        raise UiFailure("Initial real recording did not start")

    def wait_fault_evidence(self):
        marker = {"runner": "event=runner-unavailable ",
                  "renderer": "event=renderer-fallback ",
                  "healthy": "event=frame "}.get(self.case)
        if marker is None:
            if "gc-test-game event=constructed" in self.log_text():
                raise UiFailure("Absent-game baseline instantiated a hidden game chart")
            return
        deadline = time.monotonic() + 15.0
        while time.monotonic() < deadline:
            if "gc-test-game " + marker in self.log_text():
                return
            time.sleep(0.1)
        raise UiFailure("Missing compiled-in game fault/frame evidence: " + marker)

    def toggle_pause(self):
        self.driver.activate(self.driver.find(
            "Start or pause training", "push button", showing=True))

    def stop_and_continue(self, recording):
        self.activate_stop_training()
        self.driver.find("Continue Training", "push button", showing=True, timeout=30.0)
        self.hold_phase("stop-confirmation", recording, active=False,
                        gear=int(self.driver.current_value(self.gear)))
        self.activate_stop_dialog_button("Continue Training")
        self.stop_training_button = self.driver.find_enabled(
            "Stop training", "push button", showing=True, timeout=10.0)

    def hold_phase(self, name, recording, *, active, gear):
        print(f"Training failure case {self.case}: observing {name}", flush=True)
        # Let one already queued telemetry/disk callback settle, then observe
        # more than two recording intervals. Never compare exact A/B row counts.
        time.sleep(1.2)
        before = recording.read_text(encoding="utf-8")
        started_ms = time.monotonic() * 1000.0
        time.sleep(3.2)
        finished_ms = time.monotonic() * 1000.0
        after = recording.read_text(encoding="utf-8")
        log = self.log_text()[self.training_log_offset:]
        all_commands = training_dispatches(log, expected_pid=self.driver.app_pgid)
        commands = [command for command in all_commands
                    if started_ms <= command["mono_ms"] <= finished_ms]
        # An unpaired controller receipt is also a real command. It must not
        # disappear merely because the coordinator trace was absent.
        receipts = []
        for line in log.splitlines():
            if "gc-test-device event=load " not in line:
                continue
            fields = dict(word.split("=", 1) for word in line.split() if "=" in word)
            timestamp = float(fields["mono_ms"])
            if started_ms <= timestamp <= finished_ms:
                receipts.append(fields)
        evidence = {"name": name, "start_ms": started_ms, "end_ms": finished_ms,
                    "before": before, "after": after, "commands": commands}
        self.phases.append(evidence)
        write_text(self.artifacts / "training-failure-phases.json",
                   json.dumps(self.phases, indent=2) + "\n")
        write_text(self.artifacts / "game-training-recording.csv", after)
        if len(receipts) != len(commands):
            raise UiFailure("Unpaired real controller commands during " + name)
        validate_training_phase(before, after, commands, active=active, gear=gear,
                                document=self.document)
        if active and self.case in ("healthy", "renderer"):
            frames = []
            for line in log.splitlines():
                if "gc-test-game event=frame " in line:
                    fields = dict(word.split("=", 1) for word in line.split() if "=" in word)
                    if started_ms <= float(fields["mono_ms"]) <= finished_ms:
                        frames.append(line)
            if len(frames) < 2:
                raise UiFailure("Control/fallback game stopped producing fresh frames during " + name)
        for command in commands:
            expected = training_prescribed_targets(self.document, command["workout_pos"], gear)
            if command["value"] not in expected:
                raise UiFailure(f"{name}: received {command['value']} W, prescribed {sorted(expected)} W")

    def finish_evidence(self, recording):
        log = self.log_text()
        self.wait_fault_evidence()
        if self.case == "runner":
            following = log.split("gc-test-game event=runner-unavailable ", 1)[1]
            if "gc-test-game event=frame " in following:
                raise UiFailure("Unavailable runner was restarted by a lifecycle event")
        if self.case != "absent" and "Workout Game session course: distance-course" not in log:
            raise UiFailure("Game did not accept the generated distance course")
        if "gc-test-device event=mode value=1" not in log:
            raise UiFailure("Data Generator did not receive ERG mode")
        reference = self.document["conversion"]["parameters"].get("referenceGear", 6)
        prescription_values = {
            training_prescribed_target(self.document, command["workout_pos"], reference)
            for phase in self.phases for command in phase["commands"]
        }
        if len(prescription_values) < 2:
            raise UiFailure("Post-fault exercise never crossed a prescribed power transition")
        rows = training_recording_rows(recording.read_text(encoding="utf-8"))
        if any(right["secs"] - left["secs"] > 3
               for left, right in zip(rows, rows[1:])):
            raise UiFailure("Recording clock includes pause time or has a cadence gap")
        preserve_game_recording(recording, self.artifacts)
        write_text(self.artifacts / "training-failure-summary.json", json.dumps({
            "case": self.case, "phases": [phase["name"] for phase in self.phases],
            "result": "lifecycle-checked", "scope": "recoverable game failure; simulated trainer",
        }, indent=2) + "\n")


class Suite:
    def __init__(self, driver: UiDriver, artifacts: Path):
        self.driver = driver
        self.artifacts = artifacts
        self.results = []

    def run(self, name, test):
        started = time.monotonic()
        error = None
        try:
            test()
            print(f"PASS {name}", flush=True)
        except Exception:
            error = traceback.format_exc()
            try:
                safe_name = "".join(
                    character if character.isalnum() else "-"
                    for character in name.lower()
                ).strip("-")
                self.driver.screenshot(f"failure-{safe_name or 'ui-test'}")
            except Exception:
                pass
            print(f"FAIL {name}\n{error}", file=sys.stderr, flush=True)
        self.results.append((name, time.monotonic() - started, error))

    def write_junit(self):
        failures = sum(error is not None for _, _, error in self.results)
        root = ET.Element(
            "testsuite",
            name="GoldenCheetahPreReleaseUi",
            tests=str(len(self.results)),
            failures=str(failures),
            time=f"{sum(duration for _, duration, _ in self.results):.3f}",
        )
        for name, duration, error in self.results:
            case = ET.SubElement(
                root, "testcase", name=name, time=f"{duration:.3f}"
            )
            if error:
                ET.SubElement(case, "failure", message=error.splitlines()[-1]).text = error
        ET.ElementTree(root).write(
            self.artifacts / "junit.xml", encoding="utf-8", xml_declaration=True
        )
        return failures


def process_group_exists(pgid: int) -> bool:
    try:
        os.killpg(pgid, 0)
        return True
    except ProcessLookupError:
        return False


def process_belongs_to_group(pid: int, pgid: int) -> bool:
    try:
        return os.getpgid(pid) == pgid
    except (OSError, ValueError):
        return False


def observe_renderer_canvas(output: Path, app_pgid: int) -> int:
    import pyatspi

    def nodes(node):
        yield node
        try:
            children = list(node)
        except Exception:
            return
        for child in children:
            yield from nodes(child)

    while process_group_exists(app_pgid):
        desktop = pyatspi.Registry.getDesktop(0)
        applications = []
        for app in desktop:
            try:
                if process_belongs_to_group(
                    int(app.get_process_id()), app_pgid
                ):
                    applications.append(app)
            except Exception:
                continue
        for node in (
            descendant
            for app in applications
            for descendant in nodes(app)
        ):
            try:
                name = node.name or ""
                showing = node.getState().contains(pyatspi.STATE_SHOWING)
            except Exception:
                continue
            if name in WORKOUT_GAME_CANVAS_NAMES and showing:
                write_text(output, name + "\n")
                return 0
        time.sleep(0.2)
    return 1


def exercise(root: Path, artifacts: Path, app_pgid: int) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    suite = None
    try:
        driver = UiDriver(root, artifacts, app_pgid)
        suite = Suite(driver, artifacts)
        selected_tests = set(selected_ui_tests_from_environment())
        capture_screenshots = ui_screenshots_enabled_from_environment()
        generated_course = {}

        def enter_train():
            driver.activate_named("Train", "menu item")
            deadline = time.monotonic() + 30.0
            while time.monotonic() < deadline:
                controls = driver.find_all(
                    name="Connect training devices",
                    role="push button",
                    showing=True,
                )
                if controls:
                    return
                close_buttons = driver.find_all(
                    name="Close", role="push button", showing=True
                )
                if close_buttons:
                    driver.activate(close_buttons[-1])
                time.sleep(0.2)
            raise UiFailure("Train controls did not become ready")

        def stop_without_saving():
            stop_buttons = driver.find_all(
                name="Stop training", role="push button", showing=True
            )
            if not stop_buttons or not driver.enabled(stop_buttons[-1]):
                return
            driver.activate(stop_buttons[-1])
            cancel = driver.find(
                "Cancel", "push button", showing=True, timeout=30.0
            )
            driver.activate(cancel)

        def startup():
            driver.require_names(
                ["Athlete", "Activity", "Share", "Tools", "View", "Help"],
                "menu item",
            )
            if capture_screenshots:
                driver.screenshot("01-startup")

        def views():
            for view in ("Plan", "Trends", "Activities", "Train"):
                driver.activate_view(view)
                time.sleep(0.5)
            if capture_screenshots:
                driver.screenshot("02-train")

        def train_controls():
            enter_train()
            driver.combo_with_items(
                [
                    "Erg Workout",
                    "Slope Workout",
                    "Map Workout",
                    "Video Workout",
                    "Workout Editor",
                    "Workout Game",
                ]
            )
            driver.require_names(
                [
                    "Connect training devices",
                    "Rewind workout",
                    "Stop training",
                    "Start or pause training",
                    "Advance workout",
                    "Previous lap",
                    "New lap",
                    "Next lap",
                    "Calibrate trainer",
                    "Virtual gear",
                    "Decrease intensity",
                    "Increase intensity",
                    "Workout intensity",
                    "Filter workouts",
                    "Workout order",
                ]
            )
            driver.combo_with_items(["Standard ERG", "Workout Ride"])
            driver.find("Data Generator", "table cell")

        def import_prepared_workout():
            driver.activate_named("Train", "menu item")
            driver.activate(
                driver.find(
                    "Scan hard drives", "push button", showing=True, timeout=20.0
                )
            )
            driver.find(
                "Search for Workouts, Syncs and Media",
                "dialog",
                showing=True,
                timeout=30.0,
            )
            driver.activate(
                driver.find("Search", "push button", showing=True, timeout=10.0)
            )
            driver.activate(
                driver.find("Save", "push button", showing=True, timeout=60.0)
            )
            enter_train()
            driver.find(
                "ui-test", "table cell", showing=True, timeout=30.0
            )

        def scan_preserves_unsearched_workouts():
            driver.click(
                driver.find(
                    "Tools", "menu item", showing=True, timeout=20.0
                )
            )
            driver.click(
                driver.find(
                    "Scan disk for workouts, videos, videoSyncs...",
                    "menu item",
                    showing=True,
                    timeout=10.0,
                )
            )
            driver.find(
                "Search for Workouts, Syncs and Media",
                "dialog",
                showing=True,
                timeout=30.0,
            )
            workouts = driver.find(
                "Workout files (.erg, .mrc, .zwo etc)",
                "check box",
                showing=True,
            )
            if driver.checked(workouts):
                driver.activate(workouts)
            if driver.checked(workouts):
                raise UiFailure("Workout search checkbox did not turn off")
            driver.activate(
                driver.find("Search", "push button", showing=True, timeout=10.0)
            )
            driver.activate(
                driver.find("Save", "push button", showing=True, timeout=60.0)
            )
            enter_train()
            driver.find(
                "ui-test", "table cell", showing=True, timeout=30.0
            )

        def scan_rejects_unavailable_path():
            workouts = root / "library" / ATHLETE / "workouts"
            unavailable = workouts.with_name("workouts-unavailable")
            workouts.rename(unavailable)
            try:
                driver.click(
                    driver.find(
                        "Tools", "menu item", showing=True, timeout=20.0
                    )
                )
                driver.click(
                    driver.find(
                        "Scan disk for workouts, videos, videoSyncs...",
                        "menu item",
                        showing=True,
                        timeout=10.0,
                    )
                )
                driver.find(
                    "Search for Workouts, Syncs and Media",
                    "dialog",
                    showing=True,
                    timeout=30.0,
                )
                driver.activate(
                    driver.find(
                        "Search", "push button", showing=True, timeout=10.0
                    )
                )
                driver.find(
                    "The search cannot start because a configured search "
                    "path is unavailable or unreadable.",
                    showing=True,
                    timeout=10.0,
                )
                driver.activate(
                    driver.find("OK", "push button", showing=True, timeout=10.0)
                )
                driver.activate(
                    driver.find(
                        "Cancel", "push button", showing=True, timeout=10.0
                    )
                )
            finally:
                if unavailable.exists() and not workouts.exists():
                    unavailable.rename(workouts)

            enter_train()
            driver.find(
                "ui-test", "table cell", showing=True, timeout=30.0
            )

        def generator_and_gears():
            enter_train()
            driver.select_named("Data Generator")
            connect = driver.find(
                "Connect training devices", "push button", showing=True
            )
            driver.activate(connect)
            try:
                gear = driver.find_enabled(
                    "Virtual gear", "spin button", showing=True, timeout=15.0
                )
            except UiFailure as error:
                raise UiFailure("Data Generator did not connect") from error
            driver.select_named("Manual Erg Mode")
            driver.activate(
                driver.find(
                    "Start or pause training", "push button", showing=True
                )
            )
            time.sleep(1.0)
            try:
                initial = driver.current_value(gear)
                driver.send_key("w")
                driver.wait_value(gear, initial + 1)
                driver.send_key("s")
                driver.wait_value(gear, initial)
                if capture_screenshots:
                    driver.screenshot("03-generator-connected")
            finally:
                stop_without_saving()

        def mtb_course_lifecycle():
            def ride_generated_course(preset: str, result: dict) -> None:
                workflow = WorkoutGameUiWorkflow(
                    driver,
                    root,
                    artifacts,
                    capture_screenshots,
                    enter_train,
                    (result["workout_name"],),
                )
                workflow.run_smoke_and_discard(preset)

            workflow = MtbCourseUiWorkflow(
                driver,
                root,
                artifacts,
                capture_screenshots,
                enter_train,
                ride_generated_course,
            )
            generated_course.update(workflow.run())

        def game_training_lifecycle():
            workout_names = ()
            if generated_course.get("workout_name"):
                workout_names = (generated_course["workout_name"],)
            workflow = WorkoutGameUiWorkflow(
                driver,
                root,
                artifacts,
                capture_screenshots,
                enter_train,
                workout_names,
            )
            completed = False
            try:
                workflow.run()
                completed = True
            finally:
                if not completed:
                    stop_without_saving()

        def training_failure_independence():
            case = training_failure_case_from_environment()
            generator = MtbCourseUiWorkflow(driver, root, artifacts, False, enter_train)
            if os.environ.get("GC_UI_TRAINING_FAILURE_FIXTURE"):
                validate_mtb_course_sidecar(generator.sidecar_path, "workout-first", generator.title)
            else:
                print("Training failure fixture: creating the production MTB course", flush=True)
                generator.create("workout-first")
            course_before = generator.course_path.read_bytes()
            sidecar_before = generator.sidecar_path.read_bytes()
            shutil.copy2(generator.course_path, artifacts / "failure-course.crs")
            shutil.copy2(generator.sidecar_path, artifacts / "failure-course.gcmtb.json")
            workflow = TrainingFailureUiWorkflow(
                driver, root, artifacts, enter_train, case, json.loads(sidecar_before))
            completed = False
            try:
                workflow.run_failure_case()
                if (generator.course_path.read_bytes() != course_before
                        or generator.sidecar_path.read_bytes() != sidecar_before):
                    raise UiFailure("Game failure fixture changed the persisted course")
                summary_path = artifacts / "training-failure-summary.json"
                summary = json.loads(summary_path.read_text(encoding="utf-8"))
                summary["result"] = "passed"
                summary["persisted_course_unchanged"] = True
                write_text(summary_path, json.dumps(summary, indent=2) + "\n")
                completed = True
            finally:
                if not completed:
                    stop_without_saving()

        def choose_save_path(destination: Path) -> None:
            try:
                driver.find(role="file chooser", showing=True, timeout=2.0)
            except UiFailure:
                driver.find(role="dialog", showing=True, timeout=30.0)
            editable = None
            for node in driver.find_all(role="text", showing=True):
                try:
                    node.queryEditableText()
                    editable = node
                except Exception:
                    continue
            if editable is None:
                raise UiFailure("Save dialog file name input was not found")
            editable.queryEditableText().setTextContents(str(destination))
            driver.click(driver.find("Save", "push button", showing=True))

        def ensure_workout_code_visible() -> None:
            if not driver.find_all(
                    name="Workout code", role="text", showing=True):
                driver.activate(
                    driver.find(
                        "Properties", "push button", showing=True, timeout=10.0
                    )
                )
                driver.find(
                    "Workout code", "text", showing=True, timeout=10.0
                )

        def start_new_erg_workout() -> None:
            ensure_workout_code_visible()
            driver.click(driver.find("New", "push button", showing=True))
            driver.send_named_key("Down")
            driver.send_named_key("Return")
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if not editor.queryText().getText(0, -1).strip():
                    return
                time.sleep(0.05)
            raise UiFailure("New ERG workout did not clear the editor")

        def save_workout():
            enter_train()
            driver.select_combo_item(
                ["Workout Game", "Workout Editor"], "Workout Editor"
            )
            conflict = (
                root / "library" / ATHLETE / "workouts" / "ui-save.erg"
            )
            write_text(conflict, "library workout must remain unchanged\n")
            original = conflict.read_bytes()
            external = root / "external" / conflict.name
            external.parent.mkdir(parents=True, exist_ok=True)

            start_new_erg_workout()
            driver.click(driver.find("Save As", "push button", showing=True))
            choose_save_path(external)
            driver.wait_file(external)
            warning_text = (
                "The workout was saved, but it could not be added to the "
                "workout library. The editor will continue using the saved file."
            )
            driver.find(
                warning_text,
                showing=True,
                timeout=20.0,
            )
            driver.click(
                driver.find("OK", "push button", showing=True, timeout=10.0)
            )
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if not driver.find_all(name=warning_text, showing=True):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Save As import warning did not close")
            if conflict.read_bytes() != original:
                raise UiFailure(
                    "Failed Save As import overwrote the library workout"
                )

            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            editor.queryEditableText().setTextContents("1m@150")
            driver.find_enabled(
                "Save", "push button", showing=True, timeout=10.0
            )
            destination = conflict.with_name("ui-save-ok.erg")
            driver.click(driver.find("Save As", "push button", showing=True))
            choose_save_path(destination)
            driver.wait_file(destination)
            if driver.find_all(name=warning_text, showing=True):
                raise UiFailure("Valid Save As unexpectedly failed to import")
            if capture_screenshots:
                driver.screenshot("06-workout-saved")

        def workout_generator():
            driver.activate(driver.find("Tools", "menu item", showing=True))
            driver.activate(
                driver.find(
                    "Create a new workout...",
                    "menu item",
                    showing=True,
                    timeout=10.0,
                )
            )
            wizard = driver.find(
                "Workout Wizard", "dialog", showing=True, timeout=10.0
            )
            driver.find(
                "Generate for a training goal",
                "radio button",
                showing=True,
            )
            driver.activate(
                driver.find_named_any(
                    ("Next >", "Next"),
                    "push button",
                    showing=True,
                )
            )

            driver.require_visible_names(
                [
                    "Training focus",
                    "FTP",
                    "Work intensity",
                    "Recovery intensity",
                    "Work interval",
                    "Recovery interval",
                    "Repetitions in first set",
                    "Sets",
                    "Recovery between sets",
                    "Recovery before final set",
                    "Warm-up start",
                    "Warm-up end",
                ],
                timeout=15.0,
            )
            driver.require_interactive_controls(
                [
                    ("20/20 descending sets", "combo box"),
                    ("FTP", "spin button"),
                    ("Work intensity slider", "slider"),
                    ("Work intensity", "spin button"),
                    ("Recovery intensity slider", "slider"),
                    ("Recovery intensity", "spin button"),
                    ("Work interval", "spin button"),
                    ("Recovery interval", "spin button"),
                    ("Repetitions in first set", "spin button"),
                    ("Sets", "spin button"),
                    ("Repetition change per set", "spin button"),
                    ("Recovery between sets", "spin button"),
                    ("Recovery before final set", "spin button"),
                    ("Warm-up", "spin button"),
                    ("Warm-up start slider", "slider"),
                    ("Warm-up start", "spin button"),
                    ("Warm-up end slider", "slider"),
                    ("Warm-up end", "spin button"),
                    ("Cool-down", "spin button"),
                    ("Recovery after the last repetition", "check box"),
                ],
                scope=wizard,
            )
            driver.select_combo_item(
                (
                    "Endurance", "Tempo", "Sweet spot", "Threshold",
                    "VO2max", "Anaerobic", "Sprint",
                    "20/20 descending sets",
                ),
                "20/20 descending sets",
            )
            driver.find("0:53:20", showing=True, timeout=10.0)
            driver.find(
                "14 / 12 / 10 / 8 efforts; set recovery "
                "4:00 / 4:00 / 3:00",
                showing=True,
                timeout=10.0,
            )
            driver.find("247 W", "label", showing=True, timeout=10.0)
            driver.find("105 W", "label", showing=True, timeout=10.0)
            ftp = driver.find("FTP", "spin button", showing=True)
            work_power = driver.find(
                "Work intensity", "spin button", showing=True
            )
            work_seconds = driver.find(
                "Work interval", "spin button", showing=True
            )
            final_set_recovery = driver.find(
                "Recovery before final set",
                "spin button",
                showing=True,
            )
            warmup_start = driver.find(
                "Warm-up start", "spin button", showing=True
            )
            warmup_end = driver.find(
                "Warm-up end", "spin button", showing=True
            )
            driver.set_value(ftp, 200)
            driver.set_value(work_power, 135)
            driver.set_value(work_seconds, 25)
            driver.set_value(final_set_recovery, 150)
            driver.set_value(warmup_start, 50)
            driver.set_value(warmup_end, 75)
            driver.find("270 W", "label", showing=True, timeout=10.0)
            driver.find("110 W", "label", showing=True, timeout=10.0)
            driver.find("100 W", "label", showing=True, timeout=10.0)
            driver.find("150 W", "label", showing=True, timeout=10.0)
            driver.find("0:56:30", showing=True, timeout=10.0)
            driver.find(
                "14 / 12 / 10 / 8 efforts; set recovery "
                "4:00 / 4:00 / 2:30",
                showing=True,
                timeout=10.0,
            )
            if capture_screenshots:
                driver.screenshot("05-workout-generator-configured")

            driver.activate(
                driver.find_named_any(
                    ("< Back", "Back"),
                    "push button",
                    showing=True,
                )
            )
            driver.find(
                "Generate for a training goal",
                "radio button",
                showing=True,
            )
            driver.activate(
                driver.find_named_any(
                    ("Next >", "Next"),
                    "push button",
                    showing=True,
                )
            )
            work_seconds = driver.find(
                "Work interval", "spin button", showing=True
            )
            if driver.current_value(work_seconds) != 25:
                raise UiFailure("Workout generator lost edits after Back/Next")
            final_set_recovery = driver.find(
                "Recovery before final set",
                "spin button",
                showing=True,
            )
            if driver.current_value(final_set_recovery) != 150:
                raise UiFailure(
                    "Workout generator lost final-set recovery after Back/Next"
                )

            repetitions = driver.find(
                "Repetitions in first set", "spin button", showing=True
            )
            repetition_delta = driver.find(
                "Repetition change per set", "spin button", showing=True
            )
            driver.set_value(repetitions, 1)
            driver.set_value(repetition_delta, -1)
            driver.find(
                "The repetition progression produces an invalid set.",
                showing=True,
                timeout=10.0,
            )
            finish = driver.find("Finish", "push button", showing=True)
            if driver.enabled(finish):
                raise UiFailure(
                    "Workout generator enabled Finish for invalid settings"
                )
            driver.set_value(repetitions, 14)
            driver.set_value(repetition_delta, -2)
            driver.find_enabled(
                "Finish", "push button", showing=True, timeout=10.0
            )

            destination = (
                root / "library" / ATHLETE / "workouts"
                / "ui-generated-20-20.mrc"
            )
            driver.activate(driver.find("Finish", "push button", showing=True))
            try:
                driver.find(role="file chooser", showing=True, timeout=2.0)
            except UiFailure:
                driver.find(role="dialog", showing=True, timeout=30.0)
            editable = None
            for node in driver.find_all(role="text", showing=True):
                try:
                    node.queryEditableText()
                    editable = node
                except Exception:
                    continue
            if editable is None:
                raise UiFailure("Generated workout filename input was not found")
            editable.queryEditableText().setTextContents(str(destination))
            driver.click(driver.find("Save", "push button", showing=True))
            driver.wait_file(destination)
            result = validate_generated_workout(destination)
            if (not math.isclose(result["duration_minutes"], 56.5,
                                 abs_tol=0.001)
                    or result["minimum_percent"] != 55.0
                    or result["maximum_percent"] != 135.0
                    or result["point_count"] != 190):
                raise UiFailure(
                    f"Generated workout did not preserve controls: {result!r}"
                )
            write_text(
                artifacts / "generated-workout-evidence.json",
                json.dumps(result, indent=2, sort_keys=True) + "\n",
            )
            if capture_screenshots:
                driver.screenshot("06-workout-generator-saved")

        def dirty_workout_transition_guard():
            def wait_unsaved_dialog():
                message = driver.find(
                    "You have unsaved changes to a workout.",
                    showing=True,
                    timeout=10.0,
                )
                ancestor = message
                message_bounds = message.queryComponent().getExtents(
                    driver.pyatspi.DESKTOP_COORDS
                )
                message_center = (
                    message_bounds.x + message_bounds.width // 2,
                    message_bounds.y + message_bounds.height // 2,
                )
                for unused in range(8):
                    candidates = [
                        node
                        for node in driver.all_nodes(ancestor)
                        if driver.role(node) == "push button"
                        and driver.showing(node)
                        and driver.name(node) in {"Save", "Discard", "Cancel"}
                    ]
                    names = {driver.name(node) for node in candidates}
                    if names == {"Save", "Discard", "Cancel"}:
                        def distance(node):
                            bounds = node.queryComponent().getExtents(
                                driver.pyatspi.DESKTOP_COORDS
                            )
                            center = (
                                bounds.x + bounds.width // 2,
                                bounds.y + bounds.height // 2,
                            )
                            return ((center[0] - message_center[0]) ** 2
                                    + (center[1] - message_center[1]) ** 2)

                        buttons = {
                            name: min(
                                (node for node in candidates
                                 if driver.name(node) == name),
                                key=distance,
                            )
                            for name in names
                        }
                        with (artifacts / "unsaved-dialog-buttons.log").open(
                                "a", encoding="utf-8") as evidence:
                            for node in candidates:
                                bounds = node.queryComponent().getExtents(
                                    driver.pyatspi.DESKTOP_COORDS
                                )
                                evidence.write(
                                    f"candidate {driver.name(node)} "
                                    f"{bounds.x},{bounds.y},"
                                    f"{bounds.width},{bounds.height} "
                                    f"distance={distance(node)}\n"
                                )
                            for name, node in sorted(buttons.items()):
                                bounds = node.queryComponent().getExtents(
                                    driver.pyatspi.DESKTOP_COORDS
                                )
                                evidence.write(
                                    f"selected {name} {bounds.x},{bounds.y},"
                                    f"{bounds.width},{bounds.height}\n"
                                )
                        return buttons
                    try:
                        ancestor = ancestor.parent
                    except Exception:
                        break
                raise UiFailure("Unsaved workout dialog buttons were not found")

            enter_train()
            driver.select_combo_item(
                ["Workout Game", "Workout Editor"], "Workout Editor"
            )
            source = root / "library" / ATHLETE / "workouts" / "ui-test.erg"
            original = source.read_bytes()
            driver.click_named_item("ui-test")
            selected = driver.find(
                "ui-test", "table cell", showing=True, timeout=10.0
            )
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if driver.selected(selected):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Edited workout selection did not settle")
            ensure_workout_code_visible()
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            text = editor.queryText().getText(0, -1)
            dirty_marker = "\n1m@111"
            editor.queryEditableText().setTextContents(text + dirty_marker)
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if dirty_marker in editor.queryText().getText(0, -1):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Workout editor did not retain the dirty edit")
            driver.find_enabled(
                "Save", "push button", showing=True, timeout=10.0
            )

            driver.click_named_item("ui-delete")
            buttons = wait_unsaved_dialog()
            driver.click(buttons["Cancel"])
            selected = driver.find(
                "ui-test", "table cell", showing=True, timeout=10.0
            )
            if not driver.selected(selected):
                raise UiFailure(
                    "Cancel did not restore the edited workout selection"
                )
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            if dirty_marker not in editor.queryText().getText(0, -1):
                raise UiFailure(
                    "Cancel discarded the edited workout contents"
                )
            if source.read_bytes() != original:
                raise UiFailure("Cancel changed the source workout file")

            driver.click_named_item("ui-delete")
            buttons = wait_unsaved_dialog()
            driver.click(buttons["Discard"])
            target = driver.find(
                "ui-delete", "table cell", showing=True, timeout=10.0
            )
            if not driver.selected(target):
                raise UiFailure("Discard did not select the target workout")
            if source.read_bytes() != original:
                raise UiFailure("Discard changed the source workout file")

            start_new_erg_workout()
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            editor.queryEditableText().setTextContents("1m@123")
            driver.find_enabled(
                "Save", "push button", showing=True, timeout=10.0
            )
            external = root / "external-transition" / "ui-test.erg"
            external.parent.mkdir(parents=True, exist_ok=True)

            driver.click_named_item("ui-test")
            buttons = wait_unsaved_dialog()
            driver.click(buttons["Save"])
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if not driver.find_all(
                        name="You have unsaved changes to a workout.",
                        showing=True):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Save did not close the unsaved workout dialog")
            choose_save_path(external)
            driver.wait_file(external)
            warning_text = (
                "The workout was saved, but it could not be added to the "
                "workout library. The editor will continue using the saved file."
            )
            driver.find(
                warning_text,
                showing=True,
                timeout=20.0,
            )
            driver.click(
                driver.find("OK", "push button", showing=True, timeout=10.0)
            )
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if not driver.find_all(name=warning_text, showing=True):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Saved transition warning did not close")
            selected = driver.find(
                "ui-test", "table cell", showing=True, timeout=10.0
            )
            if not driver.selected(selected):
                raise UiFailure(
                    "Saved transition did not select the requested workout"
                )
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            if "1m@123" in editor.queryText().getText(0, -1):
                raise UiFailure(
                    "Saved transition left the external draft in the editor"
                )
            if source.read_bytes() != original:
                raise UiFailure(
                    "Failed library import overwrote the selected workout"
                )

            start_new_erg_workout()
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            editor.queryEditableText().setTextContents("1m@124")
            driver.find_enabled(
                "Save", "push button", showing=True, timeout=10.0
            )
            imported = source.with_name("ui-transition-save-ok.erg")

            driver.click_named_item("ui-delete")
            buttons = wait_unsaved_dialog()
            driver.click(buttons["Save"])
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                if not driver.find_all(
                        name="You have unsaved changes to a workout.",
                        showing=True):
                    break
                time.sleep(0.05)
            else:
                raise UiFailure("Save did not close the unsaved workout dialog")
            choose_save_path(imported)
            driver.wait_file(imported)
            target = driver.find(
                "ui-delete", "table cell", showing=True, timeout=20.0
            )
            if not driver.selected(target):
                raise UiFailure(
                    "Successful Save As changed the requested tree selection"
                )
            editor = driver.find(
                "Workout code", "text", showing=True, timeout=10.0
            )
            if "1m@124" in editor.queryText().getText(0, -1):
                raise UiFailure(
                    "Successful Save As left the imported draft loaded"
                )

        def delete_workout_and_sidecar():
            enter_train()
            workout = root / "library" / ATHLETE / "workouts" / "ui-delete.crs"
            sidecar = workout.with_name("ui-delete.gcmtb.json")
            write_text(sidecar, '{"schemaVersion": 1}\n')

            driver.right_click_named_item("ui-delete")
            driver.activate_popup_item(7)
            driver.activate(
                driver.find(
                    "Delete", "push button", showing=True, timeout=20.0
                )
            )
            driver.wait_file_removed(workout, timeout=20.0)
            driver.wait_file_removed(sidecar, timeout=20.0)

            deadline = time.monotonic() + 20.0
            while time.monotonic() < deadline:
                if not driver.find_all(
                    name="ui-delete", role="table cell", showing=True
                ):
                    return
                time.sleep(0.2)
            raise UiFailure("Deleted workout remains visible in the library")

        def shutdown():
            try:
                driver.click(
                    driver.find(
                        "Athlete", "menu item", showing=True, timeout=5.0
                    )
                )
                driver.click(
                    driver.find(
                        "Quit", "menu item", showing=True, timeout=5.0
                    )
                )
            except Exception:
                try:
                    os.killpg(app_pgid, signal.SIGTERM)
                except ProcessLookupError:
                    return
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                if not process_group_exists(app_pgid):
                    return
                time.sleep(0.1)
            raise UiFailure("GoldenCheetah did not exit after Quit")

        if "startup_and_main_navigation" in selected_tests:
            suite.run("startup_and_main_navigation", startup)
        if "view_navigation" in selected_tests:
            suite.run("view_navigation", views)
        if "prepared_workout_library_import" in selected_tests:
            suite.run("prepared_workout_library_import", import_prepared_workout)
        if "library_scan_preserves_unsearched_workouts" in selected_tests:
            suite.run(
                "library_scan_preserves_unsearched_workouts",
                scan_preserves_unsearched_workouts,
            )
        if "library_scan_rejects_unavailable_path" in selected_tests:
            suite.run(
                "library_scan_rejects_unavailable_path",
                scan_rejects_unavailable_path,
            )
        if "train_control_accessibility" in selected_tests:
            suite.run("train_control_accessibility", train_controls)
        if "data_generator_and_virtual_gears" in selected_tests:
            suite.run("data_generator_and_virtual_gears", generator_and_gears)
        if "create_edit_mtb_course_lifecycle" in selected_tests:
            suite.run("create_edit_mtb_course_lifecycle", mtb_course_lifecycle)
        if "workout_game_training_lifecycle" in selected_tests:
            suite.run("workout_game_training_lifecycle", game_training_lifecycle)
        if "training_failure_independence" in selected_tests:
            suite.run("training_failure_independence", training_failure_independence)
        if "workout_generator_lifecycle" in selected_tests:
            suite.run("workout_generator_lifecycle", workout_generator)
        if (
            "new_workout_save_as" in selected_tests
            and not skip_save_as_from_environment()
        ):
            suite.run("new_workout_save_as", save_workout)
        if "dirty_workout_transition_guard" in selected_tests:
            suite.run(
                "dirty_workout_transition_guard",
                dirty_workout_transition_guard,
            )
        if "workout_deletion_removes_mtb_sidecar" in selected_tests:
            suite.run(
                "workout_deletion_removes_mtb_sidecar",
                delete_workout_and_sidecar,
            )
        if "graceful_shutdown_request" in selected_tests:
            suite.run("graceful_shutdown_request", shutdown)
        return 1 if suite.write_junit() else 0
    except Exception:
        error = traceback.format_exc()
        print(error, file=sys.stderr)
        root_xml = ET.Element(
            "testsuite",
            name="GoldenCheetahPreReleaseUi",
            tests="1",
            failures="1",
        )
        case = ET.SubElement(root_xml, "testcase", name="suite_initialization")
        ET.SubElement(case, "failure", message=error.splitlines()[-1]).text = error
        ET.ElementTree(root_xml).write(
            artifacts / "junit.xml", encoding="utf-8", xml_declaration=True
        )
        return 1


def main() -> int:
    if len(sys.argv) < 3:
        print(
            "Usage: pre_release_ui.py prepare ROOT | "
            "exercise ROOT ARTIFACTS PID | observe-canvas OUTPUT PID"
        )
        return 2
    command = sys.argv[1]
    if command == "prepare" and len(sys.argv) == 3:
        prepare(Path(sys.argv[2]).resolve())
        return 0
    if command == "exercise" and len(sys.argv) == 5:
        return exercise(
            Path(sys.argv[2]).resolve(),
            Path(sys.argv[3]).resolve(),
            int(sys.argv[4]),
        )
    if command == "observe-canvas" and len(sys.argv) == 4:
        return observe_renderer_canvas(
            Path(sys.argv[2]).resolve(), int(sys.argv[3])
        )
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
