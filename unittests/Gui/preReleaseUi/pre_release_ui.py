#!/usr/bin/env python3
"""Isolated AT-SPI pre-release exercise for GoldenCheetah."""

from __future__ import annotations

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
30.00 100
[END COURSE DATA]
""",
    )


class UiFailure(RuntimeError):
    pass


class UiDriver:
    def __init__(self, root: Path, artifacts: Path, app_pgid: int):
        import pyatspi
        from Xlib import X, display
        from Xlib.ext import xtest

        self.pyatspi = pyatspi
        self.X = X
        self.display = display.Display()
        self.xtest = xtest
        self.root_path = root
        self.artifacts = artifacts
        self.app_pgid = app_pgid
        self.app = self.wait_for_application()

    def all_nodes(self, node=None):
        node = self.app if node is None else node
        yield node
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

    def activate(self, node):
        if not self.enabled(node):
            raise UiFailure(
                f"Control is disabled: {self.role(node)} {self.name(node)!r}"
            )
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
            raise UiFailure(
                f"Cannot activate {self.role(node)} {self.name(node)!r}"
            ) from error

    def click(self, node):
        try:
            bounds = node.queryComponent().getExtents(
                self.pyatspi.DESKTOP_COORDS
            )
            if bounds.width <= 0 or bounds.height <= 0:
                raise ValueError("empty accessible bounds")
            x = bounds.x + bounds.width // 2
            y = bounds.y + bounds.height // 2
            self.xtest.fake_input(self.display, self.X.MotionNotify, x=x, y=y)
            self.xtest.fake_input(self.display, self.X.ButtonPress, 1)
            self.xtest.fake_input(self.display, self.X.ButtonRelease, 1)
            self.display.sync()
        except Exception as error:
            raise UiFailure(
                f"Cannot click {self.role(node)} {self.name(node)!r}"
            ) from error

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
        nodes = self.find_all(name=name, showing=True)
        for node in reversed(nodes):
            if self.role(node) in ("list item", "table cell"):
                try:
                    self.activate(node)
                    time.sleep(0.5)
                    return
                except UiFailure:
                    continue
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

    def click_named_item(self, name):
        for node in reversed(self.find_all(name=name, showing=True)):
            if self.role(node) in ("list item", "table cell"):
                self.click(node)
                time.sleep(0.5)
                return
        raise UiFailure(f"Cannot click selectable item {name!r}")

    def combo_with_items(
            self, expected, timeout=15.0, require_interactable=False):
        expected = set(expected)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for combo in self.find_all(role="combo box"):
                if require_interactable and (
                        not self.showing(combo) or not self.enabled(combo)):
                    continue
                descendants = {
                    self.name(node)
                    for node in self.all_nodes(combo)
                    if self.role(node) == "list item"
                }
                if expected.issubset(descendants):
                    return combo
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

    def focus_main_window(self):
        frame = self.find(ATHLETE, "frame")
        frame.queryComponent().grabFocus()
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

    def send_key(self, text: str):
        self.focus_main_window()
        keycode = self.display.keysym_to_keycode(ord(text.lower()))
        self.xtest.fake_input(self.display, self.X.KeyPress, keycode)
        self.xtest.fake_input(self.display, self.X.KeyRelease, keycode)
        self.display.sync()

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


class WorkoutGameUiWorkflow:
    def __init__(
        self,
        driver: UiDriver,
        root: Path,
        artifacts: Path,
        capture_screenshots: bool,
        enter_train,
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

    def open_game(self) -> None:
        self.enter_train()
        selected = False
        for workout_name in ("Pre-release UI test", "ui-test.erg", "ui-test"):
            try:
                self.driver.click_named_item(workout_name)
                selected = True
                break
            except UiFailure:
                pass
        if not selected:
            raise UiFailure("Prepared ui-test.erg workout was not selectable")

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
            deadline = time.monotonic() + 8.0
            while not self.driver.enabled(self.gear) and time.monotonic() < deadline:
                time.sleep(0.1)
            if not self.driver.enabled(self.gear):
                raise UiFailure("Data Generator did not connect for Workout Game")

        self.driver.select_combo_item(
            ["Workout Game", "Workout Editor"], "Workout Game"
        )
        self.canvas = self.driver.find_named_any(
            WORKOUT_GAME_CANVAS_NAMES, showing=True
        )
        self.existing_records = set(self.records.glob("*.csv"))
        self.existing_activities = set(self.activities.glob("*.json"))

    def start(self) -> Path:
        self.driver.activate(
            self.driver.find(
                "Start or pause training", "push button", showing=True
            )
        )
        recording = self.driver.wait_new_file(
            self.records, self.existing_records, "*.csv"
        )
        record_renderer_canvas_name(self.root, self.driver.name(self.canvas))
        self.initial_gear = self.driver.current_value(self.gear)
        if self.capture_screenshots:
            time.sleep(1.2)
            self.first_frame = self.driver.screenshot(
                "04-workout-game-first", self.canvas
            )
        return recording

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
            # window, so capture visual evidence only after that window ends.
            first = self.driver.screenshot(
                "04-workout-game-quick3d-post-cold-start-first", self.canvas
            )
            time.sleep(0.4)
            second = self.driver.screenshot(
                "04-workout-game-quick3d-post-cold-start-second", self.canvas
            )
            changed = self.driver.changed_pixels(first, second)
            if changed < 1200:
                raise UiFailure(
                    "Quick3D scene appears frozen after cold start: "
                    f"only {changed} sampled pixels changed"
                )

        self.activate_stop_training()
        continue_button = self.driver.find(
            "Continue Training", "push button", showing=True, timeout=8.0
        )
        paused_size = recording.stat().st_size
        self.driver.activate(continue_button)
        self.driver.wait_file_growth(recording, paused_size)
        if self.capture_screenshots:
            self.driver.screenshot("05-workout-game-continued")

    def stop_save_and_reopen(self, recording: Path) -> Path:
        time.sleep(1.0)
        self.activate_stop_training()
        self.driver.activate(
            self.driver.find("Save", "push button", showing=True, timeout=8.0)
        )
        activity = self.driver.wait_new_file(
            self.activities,
            self.existing_activities,
            "*.json",
            timeout=15.0,
        )
        self.driver.activate(
            self.driver.find("Finish", "push button", showing=True, timeout=8.0)
        )
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
        capture_screenshots = ui_screenshots_enabled_from_environment()

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
                "Cancel", "push button", showing=True, timeout=8.0
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
                timeout=10.0,
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

        def generator_and_gears():
            enter_train()
            driver.select_named("Data Generator")
            connect = driver.find(
                "Connect training devices", "push button", showing=True
            )
            driver.activate(connect)
            gear = driver.find("Virtual gear", "spin button", showing=True)
            deadline = time.monotonic() + 8.0
            while not driver.enabled(gear) and time.monotonic() < deadline:
                time.sleep(0.1)
            if not driver.enabled(gear):
                raise UiFailure("Data Generator did not connect")
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

        def game_training_lifecycle():
            workflow = WorkoutGameUiWorkflow(
                driver,
                root,
                artifacts,
                capture_screenshots,
                enter_train,
            )
            completed = False
            try:
                workflow.run()
                completed = True
            finally:
                if not completed:
                    stop_without_saving()

        def save_workout():
            enter_train()
            driver.select_combo_item(
                ["Workout Game", "Workout Editor"], "Workout Editor"
            )
            driver.activate(driver.find("New", "push button", showing=True))
            driver.click(driver.find("Save As", "push button", showing=True))
            try:
                driver.find(role="file chooser", showing=True, timeout=2.0)
            except UiFailure:
                driver.find(role="dialog", showing=True, timeout=8.0)
            destination = root / "library" / ATHLETE / "workouts" / "ui-save.erg"
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
            driver.wait_file(destination)
            if capture_screenshots:
                driver.screenshot("06-workout-saved")

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

        suite.run("startup_and_main_navigation", startup)
        suite.run("view_navigation", views)
        suite.run("prepared_workout_library_import", import_prepared_workout)
        suite.run("train_control_accessibility", train_controls)
        suite.run("data_generator_and_virtual_gears", generator_and_gears)
        suite.run("workout_game_training_lifecycle", game_training_lifecycle)
        if not skip_save_as_from_environment():
            suite.run("new_workout_save_as", save_workout)
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
