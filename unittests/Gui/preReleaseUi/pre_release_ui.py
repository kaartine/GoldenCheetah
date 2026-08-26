#!/usr/bin/env python3
"""Isolated AT-SPI pre-release exercise for GoldenCheetah."""

from __future__ import annotations

import os
from pathlib import Path
import signal
import sys
import time
import traceback
import xml.etree.ElementTree as ET


ATHLETE = "UiTestAthlete"
WORKOUT_GAME_CANVAS_NAMES = (
    "Workout game canvas",
    "Workout game 3D canvas",
)
GENERATOR_MODES = {
    "follow-target",
    "on-target",
    "over-target",
    "under-target",
    "cadence-low",
    "cadence-high",
}


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
    return seconds


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
2.00 100
[END COURSE DATA]
""",
    )


class UiFailure(RuntimeError):
    pass


class UiDriver:
    def __init__(self, root: Path, artifacts: Path):
        import pyatspi
        from Xlib import X, display
        from Xlib.ext import xtest

        self.pyatspi = pyatspi
        self.X = X
        self.display = display.Display()
        self.xtest = xtest
        self.root_path = root
        self.artifacts = artifacts
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

    def wait_for_application(self, timeout=30.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            desktop = self.pyatspi.Registry.getDesktop(0)
            for app in desktop:
                try:
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
            missing = [name for name in names if not self.find_all(name, role)]
            if missing:
                time.sleep(0.15)
        if missing:
            raise UiFailure(f"Missing accessible controls: {', '.join(missing)}")

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

    def combo_with_items(self, expected, timeout=15.0):
        expected = set(expected)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for combo in self.find_all(role="combo box"):
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
        combo = self.combo_with_items(expected)
        self.click(combo)
        item = self.find_combo_item(combo, name, timeout)
        self.click(item)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.name(combo) == name or self.selected(item):
                return combo
            time.sleep(0.1)
        raise UiFailure(f"Combo box did not select {name!r}")

    def current_value(self, node) -> float:
        try:
            return float(node.queryValue().currentValue)
        except Exception as error:
            raise UiFailure(f"No numeric value for {self.name(node)!r}") from error

    def send_key(self, text: str):
        frame = self.find(ATHLETE, "frame")
        frame.queryComponent().grabFocus()
        windows = [self.display.screen().root]
        target = None
        while windows:
            window = windows.pop()
            try:
                if window.get_wm_name() == ATHLETE:
                    target = window
                    break
                windows.extend(window.query_tree().children)
            except Exception:
                continue
        if target is None:
            raise UiFailure("GoldenCheetah X11 window was not found")
        target.set_input_focus(self.X.RevertToParent, self.X.CurrentTime)
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
        rgb = bytearray(width * height * 3)
        for source in range(0, len(data), 4):
            target = source // 4 * 3
            rgb[target : target + 3] = bytes(
                (data[source + 2], data[source + 1], data[source])
            )
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
        if first[:2] != second[:2]:
            raise UiFailure("Screenshot dimensions changed during the game test")
        width, height = first[:2]
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
                offset = (y * width + x) * 3
                if first_rgb[offset : offset + 3] != second_rgb[offset : offset + 3]:
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


def exercise(root: Path, artifacts: Path, app_pid: int) -> int:
    artifacts.mkdir(parents=True, exist_ok=True)
    suite = None
    try:
        driver = UiDriver(root, artifacts)
        suite = Suite(driver, artifacts)

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
            driver.screenshot("01-startup")

        def views():
            for view in ("Plan", "Trends", "Activities", "Train"):
                driver.activate_named(view, "menu item")
                driver.find(view, "label", showing=True, timeout=30.0)
                time.sleep(0.5)
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
                driver.screenshot("03-generator-connected")
            finally:
                stop_without_saving()

        def game():
            enter_train()
            selected = False
            for workout_name in ("Pre-release UI test", "ui-test.erg", "ui-test"):
                try:
                    driver.click_named_item(workout_name)
                    selected = True
                    break
                except UiFailure:
                    pass
            if not selected:
                raise UiFailure("Prepared ui-test.erg workout was not selectable")

            driver.select_named("Data Generator")
            gear = driver.find("Virtual gear", "spin button", showing=True)
            if not driver.enabled(gear):
                driver.activate(
                    driver.find(
                        "Connect training devices", "push button", showing=True
                    )
                )
                deadline = time.monotonic() + 8.0
                while not driver.enabled(gear) and time.monotonic() < deadline:
                    time.sleep(0.1)
                if not driver.enabled(gear):
                    raise UiFailure("Data Generator did not connect for Workout Game")

            driver.select_combo_item(
                ["Workout Game", "Workout Editor"], "Workout Game"
            )
            driver.find_named_any(WORKOUT_GAME_CANVAS_NAMES, showing=True)
            driver.activate(
                driver.find(
                    "Start or pause training", "push button", showing=True
                )
            )
            try:
                canvas = driver.find_named_any(
                    WORKOUT_GAME_CANVAS_NAMES, showing=True
                )
                time.sleep(1.2)
                first = driver.screenshot("04-workout-game-first", canvas)
                time.sleep(game_run_seconds_from_environment())
                second = driver.screenshot("04-workout-game-running", canvas)
                changed = driver.changed_pixels(first, second)
                if changed < 1200:
                    raise UiFailure(
                        "Workout Game appears static: "
                        f"only {changed} sampled pixels changed"
                    )
            finally:
                stop_without_saving()
                driver.select_combo_item(
                    ["Erg Workout", "Workout Game"], "Erg Workout"
                )

        def stop_continue():
            enter_train()
            driver.select_named("Manual Erg Mode")
            start = driver.find(
                "Start or pause training", "push button", showing=True
            )
            driver.activate(start)
            time.sleep(1.5)
            driver.activate(driver.find("Stop training", "push button", showing=True))
            continue_button = driver.find(
                "Continue Training", "push button", showing=True, timeout=8.0
            )
            driver.activate(continue_button)
            time.sleep(0.5)
            driver.screenshot("05-continued-training")
            driver.activate(driver.find("Stop training", "push button", showing=True))
            driver.activate(
                driver.find("Cancel", "push button", showing=True, timeout=8.0)
            )
            time.sleep(0.5)

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
                os.kill(app_pid, signal.SIGTERM)
            deadline = time.monotonic() + 8.0
            while time.monotonic() < deadline:
                try:
                    os.kill(app_pid, 0)
                except ProcessLookupError:
                    return
                time.sleep(0.1)
            raise UiFailure("GoldenCheetah did not exit after Quit")

        suite.run("startup_and_main_navigation", startup)
        suite.run("view_navigation", views)
        suite.run("prepared_workout_library_import", import_prepared_workout)
        suite.run("train_control_accessibility", train_controls)
        suite.run("data_generator_and_virtual_gears", generator_and_gears)
        suite.run("workout_game_perspective", game)
        suite.run("stop_and_continue_training", stop_continue)
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
        print("Usage: pre_release_ui.py prepare ROOT | exercise ROOT ARTIFACTS PID")
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
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
