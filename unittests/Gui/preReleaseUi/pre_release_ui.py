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


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


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
        "[migration]\nlegacy_qsettings_v1\\global_state=complete\n",
    )
    write_text(
        library / "configglobal-trainmode.ini",
        """[General]
devices=1
devicename1=Data Generator
devicetype1=64
devicespec1=
deviceprof1=
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
DESCRIPTION = Pre-release UI test
FILE NAME = ui-test.erg
MINUTES WATTS
[END COURSE HEADER]
[COURSE DATA]
0 100
0.25 100
0.5 220
0.75 220
1 100
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

    def require_names(self, names, role=None):
        missing = [name for name in names if not self.find_all(name, role)]
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

    def select_named(self, name):
        nodes = self.find_all(name=name)
        for node in reversed(nodes):
            if self.role(node) in ("list item", "table cell"):
                try:
                    self.activate(node)
                    time.sleep(0.5)
                    return
                except UiFailure:
                    continue
        raise UiFailure(f"Cannot select {name!r}")

    def combo_with_items(self, expected):
        expected = set(expected)
        for combo in self.find_all(role="combo box"):
            descendants = {
                self.name(node)
                for node in self.all_nodes(combo)
                if self.role(node) == "list item"
            }
            if expected.issubset(descendants):
                return combo
        raise UiFailure(f"Perspective selector lacks: {sorted(expected)!r}")

    def current_value(self, node) -> float:
        try:
            return float(node.queryValue().currentValue)
        except Exception as error:
            raise UiFailure(f"No numeric value for {self.name(node)!r}") from error

    def send_key(self, text: str):
        frame = self.find(ATHLETE, "frame")
        frame.queryComponent().grabFocus()
        keycode = self.display.keysym_to_keycode(ord(text.lower()))
        self.xtest.fake_input(self.display, self.X.KeyPress, keycode)
        self.xtest.fake_input(self.display, self.X.KeyRelease, keycode)
        self.display.sync()

    def screenshot(self, name: str):
        screen = self.display.screen()
        width = screen.width_in_pixels
        height = screen.height_in_pixels
        image = screen.root.get_image(
            0, 0, width, height, self.X.ZPixmap, 0xFFFFFFFF
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

        def startup():
            driver.require_names(
                ["Athlete", "Activity", "Share", "Tools", "View", "Help"],
                "menu item",
            )
            driver.screenshot("01-startup")

        def views():
            for view in ("Plan", "Trends", "Activities", "Train"):
                driver.activate(driver.find(view, "menu item"))
                time.sleep(0.4)
            driver.find("Train", "label", showing=True)
            driver.screenshot("02-train")

        def train_controls():
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
                    "Training mode",
                    "Virtual gear",
                    "Decrease intensity",
                    "Increase intensity",
                    "Workout intensity",
                ]
            )
            driver.find("Data Generator", "table cell")

        def generator_and_gears():
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
            initial = driver.current_value(gear)
            driver.send_key("w")
            driver.wait_value(gear, initial + 1)
            driver.send_key("s")
            driver.wait_value(gear, initial)
            driver.screenshot("03-generator-connected")

        def game():
            driver.select_named("Workout Game")
            combo = driver.combo_with_items(["Workout Game", "Workout Editor"])
            deadline = time.monotonic() + 5.0
            while driver.name(combo) != "Workout Game" and time.monotonic() < deadline:
                time.sleep(0.1)
            if driver.name(combo) != "Workout Game":
                raise UiFailure("Workout Game perspective did not become active")
            driver.find("Workout game canvas", showing=True)
            driver.screenshot("04-workout-game")

        def stop_continue():
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
            driver.select_named("Workout Editor")
            driver.activate(driver.find("New", "push button", showing=True))
            driver.activate(driver.find("Save As", "push button", showing=True))
            driver.find(role="file chooser", showing=True, timeout=8.0)
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
            driver.activate(driver.find("Save", "push button", showing=True))
            driver.wait_file(destination)
            driver.screenshot("06-workout-saved")

        def shutdown():
            try:
                driver.activate(driver.find("Quit", "menu item"))
            except Exception:
                os.kill(app_pid, signal.SIGTERM)

        suite.run("startup_and_main_navigation", startup)
        suite.run("view_navigation", views)
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
