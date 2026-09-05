#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
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
RUNNER_PATH = Path(__file__).with_name("run-pre-release-ui.sh")
REAL_TRAINER_RUNNER_PATH = Path(__file__).with_name(
    "run-real-trainer-acceptance.sh"
)
MATRIX_RUNNER_PATH = Path(__file__).with_name("run-pre-release-ui-matrix.sh")


class AnalyzeWorkoutGameTest(unittest.TestCase):
    def gap_line_samples(self, line):
        safe = line == "safe"
        locked_line = "none" if safe else line
        route = "bypass" if safe else "main"
        outcome = "bypassed" if safe else "completed"
        states = (
            (9.0, "measure", 0, 0, "none", "active", "main", 1, 1),
            (3.0, "committed", 1, 0, locked_line, outcome, route, 1, 1),
            (2.0, "action", 1, 0, locked_line, outcome, route, 1, 0),
            (1.0, "action", 1, 0 if safe else 1,
             locked_line, outcome, route, 1 if safe else 0, 1 if safe else 0),
            (0.0, "action", 1, 0, locked_line, outcome, route, 1, 1),
            (-1.0, "recovery", 1, 0, locked_line, outcome, route, 1, 1),
            (-2.0, "recovery", 1, 0, locked_line, outcome, route, 1, 1),
        )
        return [
            {
                "feature_terrain": "gap-jump",
                "distance_to_lip_m": distance,
                "launch_window": int(3.0 < distance <= 10.0),
                "line_locked": line_locked,
                "launch_speed_ready": 1,
                "launch_power_ready": int(not safe),
                "power_hold_ms": 0 if safe else 500,
                "action_id": 41.0,
                "locked_line": selected_line,
                "feature_phase": phase,
                "feature_outcome": sample_outcome,
                "route": sample_route,
                "airborne": airborne,
                "rear_contact": rear_contact,
                "front_contact": front_contact,
            }
            for (
                distance, phase, line_locked, airborne, selected_line,
                sample_outcome, sample_route, rear_contact, front_contact,
            ) in states
        ]

    def renderer_evidence(self, log_text, canvas_name="Workout game 3D canvas"):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        log = root / "application.log"
        image = root / "GoldenCheetah.AppImage"
        log.write_text(log_text, encoding="utf-8")
        image.write_bytes(b"test AppImage")
        return ANALYZER.renderer_evidence(
            log, "Qt Quick 3D", canvas_name, image
        )

    def test_valid_quick3d_renderer_evidence_passes(self):
        evidence = self.renderer_evidence(
            "[info] Workout Game renderer selection: SceneGraph reason=default\n"
            "[info] Workout Game renderer selection: Qt Quick 3D\n"
            "[debug] workout-game-3d-trace frame=1 fps=60\n"
        )

        self.assertTrue(evidence["passed"])
        self.assertEqual(evidence["selected_renderer"], "Qt Quick 3D")
        self.assertEqual(evidence["quick3d_trace_samples"], 1)
        self.assertEqual(evidence["failures"], [])

    def test_known_quick3d_mislabel_rejects_scenegraph_only_evidence(self):
        evidence = self.renderer_evidence(
            "[info] artifact=gc-ui-cc84c34-final-long quick3d-acceptance\n"
            "[info] Workout Game renderer selection: SceneGraph "
            "reason=default platform=xcb OpenGL=4\n"
            "[debug] workout-game-trace frame=1 fps=60\n"
        )

        self.assertFalse(evidence["passed"])
        self.assertEqual(evidence["selected_renderer"], "SceneGraph")
        self.assertEqual(evidence["quick3d_trace_samples"], 0)
        self.assertEqual(evidence["legacy_trace_samples"], 1)

    def test_quick3d_renderer_evidence_rejects_later_fallback(self):
        evidence = self.renderer_evidence(
            "Workout Game renderer selection: Qt Quick 3D\n"
            "workout-game-3d-trace frame=1 fps=60\n"
            "Workout Game renderer fallback: Qt Quick 3D -> SceneGraph\n"
        )

        self.assertFalse(evidence["passed"])
        self.assertTrue(evidence["fallback_detected"])
        self.assertTrue(any("fell back" in item for item in evidence["failures"]))

    def test_quick3d_trace_without_exact_selection_fails(self):
        evidence = self.renderer_evidence(
            "Workout Game renderer selection: Qt Quick 3D reason=requested\n"
            "workout-game-3d-trace frame=1 fps=60\n"
        )

        self.assertFalse(evidence["passed"])
        self.assertNotEqual(evidence["selected_renderer"], "Qt Quick 3D")

    def test_exact_quick3d_selection_without_trace_fails(self):
        evidence = self.renderer_evidence(
            "Workout Game renderer selection: Qt Quick 3D (unknown:0)\n"
        )

        self.assertFalse(evidence["passed"])
        self.assertEqual(evidence["selected_renderer"], "Qt Quick 3D")
        self.assertTrue(
            any("no samples" in item for item in evidence["failures"])
        )

    def test_renderer_evidence_does_not_borrow_from_an_earlier_session(self):
        evidence = self.renderer_evidence(
            "Workout Game renderer selection: Qt Quick 3D\n"
            "workout-game-3d-trace frame=1 fps=60\n"
            "Workout Game renderer selection: SceneGraph reason=default\n"
            "workout-game-trace frame=1 fps=60\n"
        )

        self.assertFalse(evidence["passed"])
        self.assertEqual(evidence["selected_renderer"], "SceneGraph")
        self.assertEqual(evidence["quick3d_trace_samples"], 0)
        self.assertEqual(evidence["legacy_trace_samples"], 1)

    def test_quick3d_analysis_reads_only_the_latest_renderer_session(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "application.log"
            log.write_text(
                "Workout Game renderer selection: SceneGraph reason=default\n"
                "workout-game-trace frame=900 fps=10\n"
                "Workout Game renderer selection: Qt Quick 3D\n"
                "workout-game-3d-trace frame=1 fps=60\n",
                encoding="utf-8",
            )

            samples = ANALYZER.parse_trace(log, quick3d_session=True)

            self.assertEqual(samples, [{"frame": 1.0, "fps": 60.0}])

    def test_renderer_evidence_rejects_wrong_accessible_canvas(self):
        evidence = self.renderer_evidence(
            "Workout Game renderer selection: Qt Quick 3D\n"
            "workout-game-3d-trace frame=1 fps=60\n",
            canvas_name="Workout game canvas",
        )

        self.assertFalse(evidence["passed"])
        self.assertTrue(
            any("accessible canvas" in item for item in evidence["failures"])
        )

    def test_missing_and_malformed_renderer_evidence_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            missing = root / "missing.json"
            malformed = root / "malformed.json"
            malformed.write_text("{not json", encoding="utf-8")

            self.assertTrue(ANALYZER.load_renderer_evidence(missing)[1])
            self.assertTrue(ANALYZER.load_renderer_evidence(malformed)[1])
            self.assertTrue(ANALYZER.validate_renderer_evidence({}))

    def test_renderer_evidence_json_round_trips_only_when_passed(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "renderer-evidence.json"
            evidence = self.renderer_evidence(
                "Workout Game renderer selection: Qt Quick 3D\n"
                "workout-game-3d-trace frame=1 fps=60\n"
            )

            ANALYZER.write_renderer_evidence(output, evidence)
            loaded, failures = ANALYZER.load_renderer_evidence(output)

            self.assertEqual(loaded, evidence)
            self.assertEqual(failures, [])

    def test_failed_renderer_guard_stops_before_performance_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = root / "application.log"
            image = root / "GoldenCheetah.AppImage"
            canvas = root / "canvas.txt"
            output = root / "renderer-evidence.json"
            log.write_text(
                "Workout Game renderer selection: SceneGraph reason=default\n"
                "workout-game-trace frame=1 fps=60\n",
                encoding="utf-8",
            )
            image.write_bytes(b"image")
            canvas.write_text("Workout game 3D canvas\n", encoding="utf-8")
            arguments = [
                "analyze_workout_game.py",
                str(log),
                "--require-quick3d-evidence",
                "--renderer-evidence-json",
                str(output),
                "--accessible-canvas-name-file",
                str(canvas),
                "--appimage",
                str(image),
            ]

            with contextlib.redirect_stdout(io.StringIO()):
                with mock.patch.object(
                    sys, "argv", arguments
                ), mock.patch.object(
                    ANALYZER,
                    "parse_trace",
                    side_effect=AssertionError(
                        "performance analysis must not run"
                    ),
                ):
                    status = ANALYZER.main()

            self.assertEqual(status, 1)
            self.assertFalse(json.loads(output.read_text())["passed"])

    def test_ui_runner_owns_and_cleans_the_appimage_process_group(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn('setsid "${APP_ENV[@]}" "$IMAGE"', runner)
        self.assertIn('kill -TERM -- "-$APP_PGID"', runner)
        self.assertIn('kill -KILL -- "-$APP_PGID"', runner)

    def test_ui_runner_enables_guard_only_for_explicit_quick3d_acceptance(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")
        matrix = MATRIX_RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn("GC_UI_REQUIRE_QUICK3D_EVIDENCE", runner)
        self.assertIn("--require-quick3d-evidence", runner)
        self.assertEqual(
            matrix.count("GC_UI_REQUIRE_QUICK3D_EVIDENCE=0"), 2
        )
        self.assertEqual(matrix.count("GC_WORKOUT_GAME_3D=0"), 2)

    def test_target_gpu_runs_require_application_reported_identity(self):
        for runner_path in (RUNNER_PATH, REAL_TRAINER_RUNNER_PATH):
            runner = runner_path.read_text(encoding="utf-8")
            self.assertIn("GC_UI_EXPECTED_GPU_PATTERN", runner)
            self.assertIn("QSG_INFO=1", runner)
            self.assertIn('"$ARTIFACT_DIR/gpu-evidence.txt"', runner)
            self.assertIn('"$ARTIFACT_DIR/application.log"', runner)

    def test_real_trainer_acceptance_requires_renderer_evidence(self):
        runner = REAL_TRAINER_RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn("--require-quick3d-evidence", runner)
        self.assertIn("renderer-evidence.json", runner)
        self.assertIn("observe-canvas", runner)

    def test_real_trainer_runner_tracks_the_complete_appimage_process_group(self):
        runner = REAL_TRAINER_RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn('setsid "$IMAGE"', runner)
        self.assertIn('while kill -0 -- "-$APP_PGID"', runner)
        self.assertIn('kill -TERM -- "-$APP_PGID"', runner)
        self.assertIn('kill -KILL -- "-$APP_PGID"', runner)

    def test_real_trainer_runner_waits_for_orphaned_app_child(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "fake.AppImage"
            artifacts = root / "artifacts"
            image.write_text(textwrap.dedent("""\
                #!/usr/bin/env bash
                library=$1
                records=$library/UiTestAthlete/records
                recording=$records/fake.csv
                printf '%s\n' 'Workout Game renderer selection: Qt Quick 3D'
                printf '%s\n' 'Workout game 3D canvas' \
                    >"$GC_UI_RENDERER_CANVAS_EVIDENCE_FILE"
                (
                    printf '%s\\n' \
                        'secs,cad,hr,km,watts,slope,target,virtualgear' \
                        >"$recording"
                    for second in $(seq 1 24); do
                        gear=6
                        if [ "$second" -ge 9 ] && [ "$second" -le 16 ]; then
                            gear=7
                        fi
                        road=$((second * 2))
                        printf '%s,80,140,0.%03d,200,0,200,%s\\n' \
                            "$second" "$road" "$gear" >>"$recording"
                        printf '%s\\n' \
                            "workout-game-3d-trace source_ms=$((second * 1000)) render_road_m=$road frame_ms=16 fps=60 p95_frame_ms=16 max_frame_ms=16 backwards=0 skipped_ticks=0 unexpected_airborne_frames=0 lateral_m=0 watts=200 target_watts=200 cadence=80 hr=140 gear=$gear speed_kph=$((20 + second)) action_id=1 feature_outcome=completed feature_terrain=roots route=main readiness=1"
                        printf '%s\\n' \
                            "workout-game-trainer-target mode=erg value=200 workout_pos=$((second * 1000)) devices=1"
                        sleep 0.03
                    done
                ) &
                exit 0
                """), encoding="utf-8")
            image.chmod(0o755)
            environment = dict(os.environ)
            environment["DISPLAY"] = ":test"

            completed = subprocess.run(
                [str(REAL_TRAINER_RUNNER_PATH), str(image), str(artifacts)],
                env=environment,
                text=True,
                capture_output=True,
                timeout=15.0,
                check=False,
            )

            self.assertEqual(
                completed.returncode,
                0,
                msg=completed.stdout + completed.stderr,
            )
            self.assertTrue((artifacts / "training-recording.csv").is_file())
            summary = json.loads(
                (artifacts / "workout-game-trainer-summary.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertTrue(summary["passed"])
            self.assertGreaterEqual(summary["matched_recording_samples"], 5)
            renderer = json.loads(
                (artifacts / "renderer-evidence.json").read_text(
                    encoding="utf-8"
                )
            )
            self.assertTrue(renderer["passed"])
            self.assertEqual(renderer["selected_renderer"], "Qt Quick 3D")

    def test_process_group_exists_checks_the_complete_app_group(self):
        with mock.patch.object(UI.os, "killpg") as killpg:
            self.assertTrue(UI.process_group_exists(1234))

        killpg.assert_called_once_with(1234, 0)

    def test_process_group_exists_handles_a_finished_app_group(self):
        with mock.patch.object(
            UI.os, "killpg", side_effect=ProcessLookupError
        ):
            self.assertFalse(UI.process_group_exists(1234))

    def test_accessible_application_must_belong_to_the_app_process_group(self):
        with mock.patch.object(UI.os, "getpgid", return_value=1234):
            self.assertTrue(UI.process_belongs_to_group(5678, 1234))
        with mock.patch.object(UI.os, "getpgid", return_value=9999):
            self.assertFalse(UI.process_belongs_to_group(5678, 1234))
        with mock.patch.object(
            UI.os, "getpgid", side_effect=ProcessLookupError
        ):
            self.assertFalse(UI.process_belongs_to_group(5678, 1234))

    def test_ui_runner_isolates_every_xdg_persistence_location(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")
        trainer_runner = REAL_TRAINER_RUNNER_PATH.read_text(encoding="utf-8")

        for variable in (
            "XDG_CONFIG_HOME",
            "XDG_CACHE_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "XDG_RUNTIME_DIR",
        ):
            self.assertIn(f"export {variable}=", runner)
            self.assertIn(f"export {variable}=", trainer_runner)

    def test_native_quick_3d_canvas_uses_trace_for_motion_gate(self):
        self.assertFalse(
            UI.canvas_requires_pixel_motion("Workout game 3D canvas")
        )
        self.assertTrue(
            UI.canvas_requires_pixel_motion("Workout game canvas")
        )

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
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
        driver.find_combo_item = mock.Mock(return_value=item)
        driver.name = mock.Mock(
            side_effect=lambda node: (
                "Workout Editor" if node is item else "Workout Game"
            )
        )
        driver.selected = mock.Mock(return_value=True)

        selected = driver.select_combo_item(
            ["Workout Game", "Workout Editor"],
            "Workout Editor",
            timeout=0.01,
        )

        self.assertIs(selected, combo)
        self.assertEqual(
            driver.click.call_args_list,
            [mock.call(combo), mock.call(item)],
        )
        driver.focus_main_window.assert_called_once_with()

    def test_combo_selection_uses_its_own_item_instead_of_global_duplicate(self):
        combo = object()
        own_item = object()
        driver = object.__new__(UI.UiDriver)
        driver.combo_with_items = mock.Mock(return_value=combo)
        driver.focus_main_window = mock.Mock()
        driver.click = mock.Mock()
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
        self.assertEqual(
            driver.click.call_args_list,
            [mock.call(combo), mock.call(own_item)],
        )
        driver.focus_main_window.assert_called_once_with()
        driver.find.assert_not_called()

    def test_combo_with_items_ignores_hidden_stale_selectors(self):
        hidden_combo = object()
        visible_combo = object()
        game = object()
        editor = object()
        driver = object.__new__(UI.UiDriver)
        driver.find_all = mock.Mock(return_value=[hidden_combo, visible_combo])
        driver.all_nodes = mock.Mock(return_value=[game, editor])
        driver.role = mock.Mock(return_value="list item")
        driver.name = mock.Mock(
            side_effect=lambda node: (
                "Workout Game" if node is game else "Workout Editor"
            )
        )
        driver.showing = mock.Mock(
            side_effect=lambda node: node is visible_combo
        )
        driver.enabled = mock.Mock(return_value=True)

        selected = driver.combo_with_items(
            ["Workout Game", "Workout Editor"],
            timeout=0.01,
            require_interactable=True,
        )

        self.assertIs(selected, visible_combo)

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
            self.assertIn("30.00 100\n", workout)

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

    def test_recording_file_waits_verify_lifecycle(self):
        driver = object.__new__(UI.UiDriver)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous = root / "previous.csv"
            previous.write_text("old\n", encoding="utf-8")
            existing = set(root.glob("*.csv"))

            recording = root / "recording.csv"
            recording.write_text("header\n", encoding="utf-8")
            self.assertEqual(
                driver.wait_new_file(root, existing, "*.csv", timeout=0.01),
                recording,
            )

            initial_size = recording.stat().st_size
            with recording.open("a", encoding="utf-8") as stream:
                stream.write("sample\n")
            driver.wait_file_growth(recording, initial_size, timeout=0.01)

            recording.unlink()
            driver.wait_file_removed(recording, timeout=0.01)

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

    def test_trainer_acceptance_duration_leaves_time_for_two_shifts(self):
        self.assertEqual(
            UI.trainer_acceptance_shift_delays(20.0), (5.0, 5.0, 10.0)
        )
        with self.assertRaisesRegex(ValueError, "at least 8 seconds"):
            UI.trainer_acceptance_shift_delays(7.9)

    def test_existing_display_acceptance_uses_trace_instead_of_root_capture(self):
        with mock.patch.dict(os.environ, {
            "GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "1",
            "GC_UI_EXISTING_DISPLAY": ":1",
        }):
            self.assertFalse(UI.ui_screenshots_enabled_from_environment())
        with mock.patch.dict(os.environ, {
            "GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "1",
            "GC_UI_EXISTING_DISPLAY": "",
        }):
            self.assertTrue(UI.ui_screenshots_enabled_from_environment())

    def test_quick3d_continuity_uses_trace_instead_of_gpu_readback(self):
        with mock.patch.dict(
            os.environ, {"GC_UI_REQUIRE_QUICK3D_EVIDENCE": "1"}, clear=True
        ):
            self.assertFalse(UI.ui_screenshots_enabled_from_environment())
        with mock.patch.dict(
            os.environ, {"GC_UI_REQUIRE_QUICK3D_EVIDENCE": "0"}, clear=True
        ):
            self.assertTrue(UI.ui_screenshots_enabled_from_environment())

    def test_quick3d_visual_capture_cannot_enter_the_cold_start_window(self):
        with mock.patch.dict(
            os.environ,
            {
                "GC_UI_REQUIRE_QUICK3D_EVIDENCE": "1",
                "GC_UI_GAME_RUN_SECONDS": "9.9",
            },
            clear=True,
        ):
            with self.assertRaisesRegex(ValueError, "at least 10.5 seconds"):
                UI.game_run_seconds_from_environment()

    def test_save_as_gate_can_be_split_from_renderer_gate(self):
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "1"}):
            self.assertTrue(UI.skip_save_as_from_environment())
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "0"}):
            self.assertFalse(UI.skip_save_as_from_environment())
        with mock.patch.dict(os.environ, {"GC_UI_SKIP_SAVE_AS": "yes"}):
            with self.assertRaisesRegex(ValueError, "must be 0 or 1"):
                UI.skip_save_as_from_environment()

    def test_trainer_acceptance_gate_is_explicit(self):
        with mock.patch.dict(
            os.environ, {"GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "1"}
        ):
            self.assertTrue(UI.validate_trainer_acceptance_from_environment())
        with mock.patch.dict(
            os.environ, {"GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "0"}
        ):
            self.assertFalse(UI.validate_trainer_acceptance_from_environment())
        with mock.patch.dict(
            os.environ, {"GC_UI_VALIDATE_TRAINER_ACCEPTANCE": "yes"}
        ):
            with self.assertRaisesRegex(ValueError, "must be 0 or 1"):
                UI.validate_trainer_acceptance_from_environment()

    def test_renderer_canvas_name_is_recorded_for_the_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            destination = UI.record_renderer_canvas_name(
                root, "Workout game 3D canvas"
            )

            self.assertEqual(destination, root / "renderer-canvas-name.txt")
            self.assertEqual(
                destination.read_text(encoding="utf-8"),
                "Workout game 3D canvas\n",
            )

    def test_game_recording_is_preserved_in_artifact_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "isolated-records" / "training.csv"
            artifacts = root / "artifacts"
            source.parent.mkdir()
            source.write_text("secs,watts\n0,190\n", encoding="utf-8")

            destination = UI.preserve_game_recording(source, artifacts)

            self.assertEqual(
                destination, artifacts / "game-training-recording.csv"
            )
            self.assertEqual(
                destination.read_text(encoding="utf-8"),
                "secs,watts\n0,190\n",
            )
            self.assertTrue(source.exists())

    def test_x11_bgrx_conversion_uses_all_pixels(self):
        self.assertEqual(
            UI.x11_bgrx_to_rgb(
                bytes((0x33, 0x22, 0x11, 0x00, 0xCC, 0xBB, 0xAA, 0xFF))
            ),
            bytes((0x11, 0x22, 0x33, 0xAA, 0xBB, 0xCC)),
        )
        with self.assertRaisesRegex(ValueError, "whole BGRX pixels"):
            UI.x11_bgrx_to_rgb(b"abc")

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

    def test_frame_delta_compares_common_area_after_dimension_change(self):
        self.assertEqual(
            UI.UiDriver.changed_pixels(
                (4, 1, b"\0" * 12),
                (5, 1, b"\xff\xff\xff" + b"\0" * 12),
                top_ratio=0.0,
                bottom_ratio=1.0,
                side_ratio=0.25,
                sample_step=1,
            ),
            1,
        )

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

    def test_reported_p95_uses_stable_tail_but_keeps_worst_stall(self):
        samples = [
            {
                "frame_ms": 16,
                "fps": 60,
                "p95_frame_ms": p95,
                "max_frame_ms": stall,
                "render_road_m": index,
            }
            for index, (p95, stall) in enumerate(
                [(70, 90), (65, 100)] + [(18, 24)] * 8
            )
        ]

        summary = ANALYZER.analyze(samples)

        self.assertEqual(summary["reported_p95_frame_ms"], 18)
        self.assertEqual(summary["reported_max_frame_ms"], 100)

    def test_accepts_complete_cold_start_frame_continuity(self):
        samples = [{
            "cold_complete": 1,
            "cold_samples": 601,
            "cold_dropped_frames": 0,
            "cold_swap_fps": 60.1,
            "cold_visual_fps": 59.8,
            "cold_start_first_swap_ms": 16.4,
            "cold_p99_frame_ms": 18.2,
            "cold_max_frame_ms": 24.8,
            "cold_consecutive_late": 0,
            "cold_visual_stall_ms": 18.0,
            "geometry_queue": 1,
            "backwards": 0,
            "skipped_ticks": 0,
        }]

        summary = ANALYZER.analyze_cold_start(samples)

        self.assertEqual(ANALYZER.validate_cold_start(summary), [])
        self.assertEqual(summary["cold_samples"], 601)

    def test_rejects_missing_or_stalled_cold_start_evidence(self):
        missing = ANALYZER.validate_cold_start(
            ANALYZER.analyze_cold_start([])
        )
        self.assertTrue(any("complete" in failure for failure in missing))

        samples = [{
            "cold_complete": 1,
            "cold_samples": 350,
            "cold_dropped_frames": 2,
            "cold_swap_fps": 35.0,
            "cold_visual_fps": 8.0,
            "cold_start_first_swap_ms": 75.0,
            "cold_p99_frame_ms": 28.0,
            "cold_max_frame_ms": 70.0,
            "cold_consecutive_late": 2,
            "cold_visual_stall_ms": 250.0,
            "geometry_queue": 2,
            "backwards": 1,
            "skipped_ticks": 1,
        }]
        failures = ANALYZER.validate_cold_start(
            ANALYZER.analyze_cold_start(samples)
        )
        for expected in (
            "dropped", "p99", "maximum", "consecutive", "first swap",
            "visual", "queue", "backward", "skipped",
        ):
            self.assertTrue(
                any(expected in failure for failure in failures), expected
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

    def test_parses_quick_3d_trace_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[info] workout-game-3d-trace frame=12 frame_ms=16 "
                "fps=61.2 render_road_m=18.5 target_watts=220 "
                "lateral_m=0.2 unexpected_airborne_frames=0 "
                "feature_phase=recovery feature_outcome=completed "
                "route=main camera_presentation=idle-side "
                "camera_side_blend=0.75 feature_geometry=jump\n",
                encoding="utf-8",
            )

            samples = ANALYZER.parse_trace(path)

            self.assertEqual(len(samples), 1)
            self.assertEqual(samples[0]["frame"], 12)
            self.assertEqual(samples[0]["target_watts"], 220)
            self.assertEqual(samples[0]["lateral_m"], 0.2)
            self.assertEqual(samples[0]["feature_phase"], "recovery")
            self.assertEqual(samples[0]["feature_outcome"], "completed")
            self.assertEqual(samples[0]["route"], "main")
            self.assertEqual(samples[0]["camera_presentation"], "idle-side")
            self.assertEqual(samples[0]["camera_side_blend"], 0.75)

    def test_accepts_gap_launch_window_between_ten_and_three_meters(self):
        samples = [
            {
                "feature_terrain": "gap-jump",
                "distance_to_lip_m": distance,
                "launch_window": launch,
                "line_locked": locked,
                "launch_speed_ready": speed_ready,
                "launch_power_ready": power_ready,
                "power_hold_ms": hold,
                "action_id": 41.0,
                "locked_line": line,
            }
            for distance, launch, locked, speed_ready, power_ready, hold, line in (
                (11.0, 0, 0, 0, 0, 0, "none"),
                (9.0, 1, 0, 0, 0, 250, "none"),
                (6.0, 1, 0, 1, 1, 500, "none"),
                (3.0, 0, 1, 1, 1, 500, "long"),
            )
        ]

        summary = ANALYZER.analyze_gap_jump(samples)

        self.assertEqual(ANALYZER.validate_gap_jump(summary), [])
        self.assertEqual(summary["gap_launch_distance_min_m"], 6.0)
        self.assertEqual(summary["gap_locked_distance_max_m"], 3.0)

    def test_rejects_gap_launch_outside_window_and_early_or_mutable_lock(self):
        samples = [
            {
                "feature_terrain": "gap-jump",
                "distance_to_lip_m": distance,
                "launch_window": 1,
                "line_locked": locked,
                "launch_speed_ready": 0,
                "launch_power_ready": 0,
                "power_hold_ms": 100,
                "action_id": action_id,
                "locked_line": line,
            }
            for distance, locked, action_id, line in (
                (12.0, 0, 41.0, "none"),
                (5.0, 1, 42.0, "medium"),
                (2.0, 1, 42.0, "long"),
            )
        ]

        failures = ANALYZER.validate_gap_jump(
            ANALYZER.analyze_gap_jump(samples)
        )

        self.assertTrue(any("outside 10-3 m" in item for item in failures))
        self.assertTrue(any("before the 3 m" in item for item in failures))
        self.assertTrue(any("identity changed" in item for item in failures))
        self.assertTrue(any("after lock" in item for item in failures))

    def test_accepts_each_expected_gap_line(self):
        for line in ("short", "medium", "long", "safe"):
            with self.subTest(line=line):
                samples = self.gap_line_samples(line)

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, line
                )

                self.assertEqual(failures, [])

    def test_rejects_wrong_expected_gap_line(self):
        unused, failures = ANALYZER.validate_expected_gap_jump(
            self.gap_line_samples("medium"), "short"
        )

        self.assertTrue(any("expected short" in item for item in failures))
        self.assertFalse(any("changed after lock" in item for item in failures))

    def test_rejects_gap_line_change_after_lock(self):
        samples = self.gap_line_samples("short")
        samples[-1]["locked_line"] = "medium"

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "short"
        )

        self.assertTrue(any("changed after lock" in item for item in failures))

    def test_rejects_multiple_actions_matching_expected_gap_line(self):
        samples = self.gap_line_samples("medium")
        samples.extend(
            dict(sample, action_id=42.0)
            for sample in self.gap_line_samples("medium")
        )

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "medium"
        )

        self.assertTrue(any("exactly one positive" in item
                            for item in failures), failures)

    def test_rejects_multiple_actions_when_only_one_matches_expected_line(self):
        for first, later in (("short", "long"), ("long", "short")):
            with self.subTest(first=first, later=later):
                samples = self.gap_line_samples(first)
                samples.extend(
                    dict(sample, action_id=42.0)
                    for sample in self.gap_line_samples(later)
                )

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any("exactly one positive" in item
                                    for item in failures), failures)

    def test_accepts_actual_c137_positive_action_id_shape(self):
        for line in ("long", "safe"):
            with self.subTest(line=line):
                samples = self.gap_line_samples(line)
                for sample in samples:
                    sample["action_id"] = 38654705677.0

                summary, failures = ANALYZER.validate_expected_gap_jump(
                    samples, line
                )

                self.assertEqual(failures, [])
                self.assertEqual(
                    summary["gap_selected_action_id"], 38654705677
                )

    def test_rejects_contradictory_gap_outcomes_for_selected_action(self):
        samples = self.gap_line_samples("long")
        samples[-1]["feature_outcome"] = "bypassed"
        samples[-1]["route"] = "bypass"

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "long"
        )

        self.assertTrue(any("contradictory" in item for item in failures))

    def test_rejects_gap_line_lock_drop_after_selection(self):
        samples = self.gap_line_samples("short")
        samples[-1]["line_locked"] = 0

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "short"
        )

        self.assertTrue(any("line_locked" in item for item in failures))

    def test_rejects_missing_required_gap_fields_after_lock(self):
        for field in (
            "action_id", "distance_to_lip_m", "launch_window",
            "line_locked", "locked_line", "feature_phase", "feature_outcome",
            "route", "airborne", "rear_contact", "front_contact",
            "launch_speed_ready", "launch_power_ready", "power_hold_ms",
        ):
            with self.subTest(field=field):
                samples = self.gap_line_samples("long")
                index = -1 if field == "action_id" else -2
                del samples[index][field]

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any(
                    "missing" in item and field in item for item in failures
                ), failures)

    def test_rejects_malformed_gap_numeric_and_boolean_fields(self):
        cases = (
            ("distance_to_lip_m", float("nan")),
            ("distance_to_lip_m", float("inf")),
            ("distance_to_lip_m", "unknown"),
            ("launch_window", 2),
            ("line_locked", -1),
            ("airborne", 0.5),
            ("rear_contact", "yes"),
            ("front_contact", float("nan")),
            ("launch_speed_ready", 2),
            ("launch_power_ready", -1),
            ("power_hold_ms", float("nan")),
            ("power_hold_ms", -1),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                samples = self.gap_line_samples("long")
                samples[-2][field] = value

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any(
                    "invalid" in item and field in item for item in failures
                ), failures)

    def test_rejects_malformed_or_inappropriate_gap_enums(self):
        cases = (
            ("locked_line", "extra"),
            ("route", "detour"),
            ("feature_outcome", "success"),
            ("feature_phase", "flying"),
            ("feature_phase", "measure"),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                samples = self.gap_line_samples("long")
                samples[-2][field] = value

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any(
                    "invalid" in item and field in item for item in failures
                ), failures)

    def test_rejects_non_positive_or_malformed_gap_action_id(self):
        for value in (0, -1, 1.5, float("nan"), "unknown"):
            with self.subTest(value=value):
                samples = self.gap_line_samples("long")
                for sample in samples:
                    sample["action_id"] = value

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any("exactly one positive" in item
                                    for item in failures), failures)

    def test_expected_gap_line_rejects_early_or_missing_launch_window(self):
        for condition in ("early", "missing"):
            with self.subTest(condition=condition):
                samples = self.gap_line_samples("long")
                for sample in samples:
                    sample["launch_window"] = 0
                if condition == "early":
                    samples[0]["distance_to_lip_m"] = 11.0
                    samples[0]["launch_window"] = 1

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                expected = "outside 10-3 m" if condition == "early" \
                    else "was not observed"
                self.assertTrue(any(expected in item for item in failures))

    def test_expected_gap_line_requires_speed_and_jump_power_readiness(self):
        cases = (
            ("long", "launch_speed_ready", "speed window"),
            ("long", "launch_power_ready", "power gate"),
            ("safe", "launch_speed_ready", "speed window"),
        )
        for line, field, message in cases:
            with self.subTest(line=line, field=field):
                samples = self.gap_line_samples(line)
                for sample in samples:
                    sample[field] = 0
                    if field == "launch_power_ready":
                        sample["power_hold_ms"] = 0

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, line
                )

                self.assertTrue(any(message in item for item in failures))

    def test_safe_gap_line_omits_only_power_readiness(self):
        samples = self.gap_line_samples("safe")
        for sample in samples:
            sample["launch_power_ready"] = 0
            sample["power_hold_ms"] = 0

        unused, failures = ANALYZER.validate_expected_gap_jump(samples, "safe")

        self.assertEqual(failures, [])

    def test_rejects_safe_gap_line_with_ready_power_gate(self):
        cases = (
            ("launch_power_ready", 1, "became ready"),
            ("power_hold_ms", 500, "hold reached"),
        )
        for field, value, message in cases:
            with self.subTest(field=field):
                samples = self.gap_line_samples("safe")
                for sample in samples:
                    sample["launch_power_ready"] = 0
                    sample["power_hold_ms"] = 0
                samples[-2][field] = value

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "safe"
                )

                self.assertTrue(any(message in item for item in failures),
                                failures)

    def test_rejects_safe_gap_line_truncated_before_recovery_merge(self):
        samples = self.gap_line_samples("safe")[:5]

        unused, failures = ANALYZER.validate_expected_gap_jump(samples, "safe")

        self.assertTrue(any("recovery" in item and "after action" in item
                            for item in failures), failures)

    def test_rejects_gap_jump_without_airborne_or_landing_evidence(self):
        for missing in ("airborne", "landing"):
            with self.subTest(missing=missing):
                samples = self.gap_line_samples("long")
                if missing == "airborne":
                    for sample in samples:
                        sample["airborne"] = 0
                else:
                    samples = samples[:4]

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(
                    any(missing in item for item in failures), failures
                )

    def test_rejects_safe_gap_line_with_airborne_frame(self):
        samples = self.gap_line_samples("safe")
        samples[3]["airborne"] = 1

        unused, failures = ANALYZER.validate_expected_gap_jump(samples, "safe")

        self.assertTrue(any("safe" in item and "airborne" in item
                            for item in failures))

    def test_rejects_gap_jump_that_becomes_airborne_after_landing(self):
        samples = self.gap_line_samples("medium")
        samples[-1]["airborne"] = 1
        samples[-1]["rear_contact"] = 0
        samples[-1]["front_contact"] = 0

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "medium"
        )

        self.assertTrue(any("re-airborne" in item for item in failures))

    def test_rejects_incomplete_ordered_gap_flight_evidence(self):
        for missing in ("takeoff", "airborne phase", "merge"):
            with self.subTest(missing=missing):
                samples = self.gap_line_samples("long")
                if missing == "takeoff":
                    for sample in samples[:3]:
                        sample["feature_phase"] = "measure"
                        sample["rear_contact"] = 0
                        sample["front_contact"] = 0
                elif missing == "airborne phase":
                    samples[3]["feature_phase"] = "recovery"
                else:
                    for sample in samples[5:]:
                        sample["feature_phase"] = "action"

                unused, failures = ANALYZER.validate_expected_gap_jump(
                    samples, "long"
                )

                self.assertTrue(any(missing in item for item in failures),
                                failures)

    def test_rejects_non_contiguous_gap_airborne_phase(self):
        samples = self.gap_line_samples("medium")
        samples.insert(4, dict(
            samples[3], airborne=0, rear_contact=0, front_contact=0
        ))

        unused, failures = ANALYZER.validate_expected_gap_jump(
            samples, "medium"
        )

        self.assertTrue(any("contiguous" in item for item in failures))

    def test_rejects_malformed_expected_gap_line_option(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "application.log"
            log.write_text("", encoding="utf-8")
            arguments = [
                "analyze_workout_game.py",
                str(log),
                "--expected-gap-line",
                "extra-long",
            ]

            error_output = io.StringIO()
            with contextlib.redirect_stderr(error_output):
                with mock.patch.object(sys, "argv", arguments):
                    with self.assertRaises(SystemExit) as error:
                        ANALYZER.main()

            self.assertEqual(error.exception.code, 2)
            self.assertIn("invalid choice", error_output.getvalue())

    def test_expected_gap_line_cli_always_runs_base_gap_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "application.log"
            log.write_text("", encoding="utf-8")
            arguments = [
                "analyze_workout_game.py", str(log),
                "--expected-gap-line", "long",
            ]

            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                with mock.patch.object(sys, "argv", arguments):
                    status = ANALYZER.main()

            result = json.loads(output.getvalue())
            self.assertEqual(status, 1)
            self.assertTrue(any("too few gap jump" in item
                                for item in result["failures"]))

    def test_no_gap_options_preserves_non_gap_analysis_path(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "application.log"
            log.write_text("", encoding="utf-8")

            with contextlib.redirect_stdout(io.StringIO()):
                with mock.patch.object(
                    sys, "argv", ["analyze_workout_game.py", str(log)]
                ), mock.patch.object(
                    ANALYZER,
                    "validate_expected_gap_jump",
                    side_effect=AssertionError("gap validation must not run"),
                ):
                    status = ANALYZER.main()

            self.assertEqual(status, 1)

    def test_ui_runner_requires_gap_acceptance_for_feature_lab(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn('GC_WORKOUT_GAME_FEATURE_LAB:-0', runner)
        self.assertIn('GC_WORKOUT_GAME_FEATURE_LAB_GAP_SCENARIO:-', runner)
        self.assertIn('--expected-gap-line "$GAP_SCENARIO"', runner)
        self.assertIn("--require-gap-launch-window", runner)

    def test_ui_runner_requires_cold_start_continuity_for_quick3d(self):
        runner = RUNNER_PATH.read_text(encoding="utf-8")

        self.assertIn("--require-cold-start-continuity", runner)

    def test_malformed_numeric_trace_field_does_not_break_analysis(self):
        samples = [{
            "frame_ms": "unavailable",
            "fps": 60.0,
            "render_road_m": 12.0,
            "feature_phase": "approach",
        }]

        summary = ANALYZER.analyze(samples)

        self.assertEqual(summary["samples"], 1)
        self.assertEqual(summary["median_fps"], 60.0)
        self.assertEqual(summary["median_frame_ms"], 0.0)

    def test_parses_anonymous_trainer_target_dispatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "[info] workout-game-trainer-target mode=slope value=4.25 "
                "wind=0.31 workout_pos=125.5 devices=1\n",
                encoding="utf-8",
            )

            targets = ANALYZER.parse_trainer_targets(path)

            self.assertEqual(targets, [{
                "mode": "slope",
                "value": 4.25,
                "wind": 0.31,
                "workout_pos": 125.5,
                "devices": 1.0,
            }])

    def test_trainer_targets_can_be_scoped_to_game_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "app.log"
            path.write_text(
                "workout-game-trainer-target mode=erg value=100 "
                "workout_pos=0 devices=1\n"
                "workout-game-3d-trace source_ms=0 watts=100\n"
                "workout-game-trainer-target mode=erg value=210 "
                "workout_pos=1000 devices=1\n"
                "workout-game-3d-trace source_ms=1000 watts=205\n"
                "workout-game-trainer-target mode=erg value=0 "
                "workout_pos=0 devices=1\n",
                encoding="utf-8",
            )

            targets = ANALYZER.parse_trainer_targets(
                path, within_trace=True
            )

            self.assertEqual(len(targets), 1)
            self.assertEqual(targets[0]["value"], 210.0)

    def test_reconciles_trace_trainer_target_and_recording(self):
        trace = [
            {
                "source_ms": float(index * 1000),
                "watts": 210.0 + index,
                "cadence": 84.0 + index,
                "hr": 140.0 + index,
                "gear": 7.0,
                "action_id": 19.0,
                "feature_outcome": "completed",
                "feature_phase": "recovery",
                "route": "main",
                "readiness": 1.0,
                "feature_terrain": "bunny-hop",
            }
            for index in range(3)
        ]
        recording = [
            {
                "secs": float(index),
                "cad": 84.0 + index,
                "hr": 140.0 + index,
                "km": index * 0.01,
                "watts": 210.0 + index,
                "slope": 4.0 + index,
                "target": 220.0,
                "virtualgear": 7.0,
            }
            for index in range(3)
        ]
        targets = [
            {
                "mode": "slope",
                "value": 4.0 + index,
                "workout_pos": index * 10.0,
                "devices": 1.0,
            }
            for index in range(3)
        ]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)

        self.assertEqual(summary["matched_recording_samples"], 3)
        self.assertEqual(summary["recording_samples_in_trace_window"], 3)
        self.assertEqual(summary["trainer_target_dispatches"], 3)
        self.assertEqual(summary["feature_decisions"], 1)
        self.assertEqual(
            ANALYZER.validate_acceptance(
                summary,
                minimum_recording_matches=3,
                minimum_recording_match_ratio=1.0,
                maximum_power_delta_watts=1.0,
                maximum_cadence_delta_rpm=1.0,
                maximum_heart_rate_delta_bpm=1.0,
                maximum_gear_mismatches=0,
                maximum_trainer_target_delta=0.01,
                minimum_trainer_target_dispatches=3,
                minimum_feature_decisions=1,
            ),
            [],
        )

    def test_acceptance_measures_progressive_speed_across_gear_changes(self):
        trace = [
            {"source_ms": 0.0, "gear": 5.0, "speed_kph": 20.0},
            {"source_ms": 250.0, "gear": 6.0, "speed_kph": 20.8},
            {"source_ms": 500.0, "gear": 6.0, "speed_kph": 21.5},
            {"source_ms": 750.0, "gear": 5.0, "speed_kph": 20.9},
        ]

        summary = ANALYZER.reconcile_acceptance(trace, [], [])

        self.assertEqual(summary["gear_changes"], 2)
        self.assertAlmostEqual(
            summary["maximum_gear_change_speed_step_kph"], 0.8
        )
        self.assertEqual(
            ANALYZER.validate_acceptance(
                summary,
                minimum_recording_matches=0,
                minimum_recording_match_ratio=0.0,
                maximum_power_delta_watts=1.0,
                maximum_cadence_delta_rpm=1.0,
                maximum_heart_rate_delta_bpm=1.0,
                maximum_gear_mismatches=0,
                maximum_trainer_target_delta=1.0,
                minimum_trainer_target_dispatches=0,
                minimum_feature_decisions=0,
                minimum_gear_changes=2,
                maximum_gear_change_speed_step_kph=2.0,
            ),
            [],
        )

    def test_acceptance_rejects_speed_teleport_or_missing_shift(self):
        teleport = ANALYZER.reconcile_acceptance([
            {"source_ms": 0.0, "gear": 5.0, "speed_kph": 12.0},
            {"source_ms": 250.0, "gear": 10.0, "speed_kph": 30.0},
        ], [], [])

        failures = ANALYZER.validate_acceptance(
            teleport,
            minimum_recording_matches=0,
            minimum_recording_match_ratio=0.0,
            maximum_power_delta_watts=1.0,
            maximum_cadence_delta_rpm=1.0,
            maximum_heart_rate_delta_bpm=1.0,
            maximum_gear_mismatches=0,
            maximum_trainer_target_delta=1.0,
            minimum_trainer_target_dispatches=0,
            minimum_feature_decisions=0,
            minimum_gear_changes=2,
            maximum_gear_change_speed_step_kph=2.0,
        )

        self.assertTrue(any("gear changes" in failure for failure in failures))
        self.assertTrue(any("speed step" in failure for failure in failures))

    def test_rejects_recording_and_feature_outcome_disagreement(self):
        trace = [{
            "source_ms": 1000.0,
            "watts": 250.0,
            "cadence": 90.0,
            "hr": 150.0,
            "gear": 8.0,
            "action_id": 20.0,
            "feature_outcome": "completed",
            "feature_phase": "recovery",
            "route": "bypass",
            "readiness": 0.4,
            "feature_terrain": "tabletop",
        }]
        recording = [{
            "secs": 1.0,
            "cad": 70.0,
            "hr": 120.0,
            "km": 0.02,
            "watts": 180.0,
            "slope": 2.0,
            "target": 200.0,
            "virtualgear": 3.0,
        }]
        targets = [{
            "mode": "slope",
            "value": 8.0,
            "workout_pos": 20.0,
            "devices": 1.0,
        }]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)
        failures = ANALYZER.validate_acceptance(
            summary,
            minimum_recording_matches=1,
            minimum_recording_match_ratio=1.0,
            maximum_power_delta_watts=5.0,
            maximum_cadence_delta_rpm=5.0,
            maximum_heart_rate_delta_bpm=5.0,
            maximum_gear_mismatches=0,
            maximum_trainer_target_delta=0.1,
            minimum_trainer_target_dispatches=1,
            minimum_feature_decisions=1,
        )

        self.assertTrue(any("power" in failure for failure in failures))
        self.assertTrue(any("trainer target" in failure for failure in failures))
        self.assertTrue(any("feature decision" in failure for failure in failures))

    def test_acceptance_tolerates_one_asynchronous_transition_sample(self):
        trace = [
            {
                "source_ms": float(index * 1000),
                "watts": 200.0 if index != 10 else 350.0,
                "cadence": 80.0,
                "hr": 140.0,
                "gear": 5.0,
                "action_id": 1.0,
                "feature_outcome": "completed",
                "route": "main",
                "readiness": 1.0,
                "feature_terrain": "roots",
            }
            for index in range(20)
        ]
        recording = [
            {
                "secs": float(index),
                "cad": 80.0,
                "hr": 140.0,
                "km": index * 0.01,
                "watts": 200.0,
                "slope": 0.0,
                "target": 200.0,
                "virtualgear": 5.0,
            }
            for index in range(20)
        ]
        targets = [
            {
                "mode": "erg",
                "value": 350.0 if index == 10 else 200.0,
                "workout_pos": float(index * 1000),
                "devices": 1.0,
            }
            for index in range(20)
        ]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)
        failures = ANALYZER.validate_acceptance(
            summary,
            minimum_recording_matches=5,
            minimum_recording_match_ratio=0.75,
            maximum_power_delta_watts=20.0,
            maximum_cadence_delta_rpm=10.0,
            maximum_heart_rate_delta_bpm=10.0,
            maximum_gear_mismatches=1,
            maximum_trainer_target_delta=5.0,
            minimum_trainer_target_dispatches=1,
            minimum_feature_decisions=1,
        )

        self.assertEqual(summary["maximum_power_delta_watts"], 150.0)
        self.assertEqual(summary["p95_power_delta_watts"], 0.0)
        self.assertEqual(summary["maximum_trainer_target_delta"], 150.0)
        self.assertEqual(summary["p95_trainer_target_delta"], 0.0)
        self.assertEqual(failures, [])

    def test_acceptance_aligns_recording_samples_across_erg_transitions(self):
        trace = [
            {
                "source_ms": source_ms,
                "watts": watts,
                "cadence": 85.0,
                "hr": 140.0,
                "gear": 6.0,
                "target_watts": target,
            }
            for source_ms, watts, target in (
                (6850.0, 170.0, 158.0),
                (7050.0, 380.0, 347.0),
                (8050.0, 380.0, 347.0),
                (10850.0, 375.0, 347.0),
                (11050.0, 165.0, 158.0),
                (12050.0, 165.0, 158.0),
            )
        ]
        recording = [
            {
                "secs": seconds,
                "cad": 85.0,
                "hr": 140.0,
                "km": seconds * 0.005,
                "watts": watts,
                "slope": 0.0,
                "target": target,
                "virtualgear": 6.0,
            }
            for seconds, watts, target in (
                (7.0, 170.0, 158.0),
                (8.0, 380.0, 347.0),
                (11.0, 375.0, 347.0),
                (12.0, 165.0, 158.0),
            )
        ]
        targets = [
            {
                "mode": "erg",
                "value": value,
                "workout_pos": position,
                "devices": 1.0,
            }
            for position, value in (
                (6872.0, 347.0),
                (7871.0, 347.0),
                (10870.0, 158.0),
                (11868.0, 158.0),
            )
        ]

        summary = ANALYZER.reconcile_acceptance(trace, targets, recording)

        self.assertEqual(summary["matched_recording_samples"], 4)
        self.assertEqual(summary["maximum_power_delta_watts"], 0.0)
        self.assertEqual(summary["maximum_trainer_target_delta"], 0.0)

    def test_erg_target_is_aligned_by_workout_time(self):
        recording = [{
            "secs": 2.0,
            "cad": 80.0,
            "hr": 140.0,
            "km": 0.01,
            "watts": 220.0,
            "slope": 0.0,
            "target": 225.0,
            "virtualgear": 5.0,
        }]
        targets = [{
            "mode": "erg",
            "value": 225.0,
            "workout_pos": 2000.0,
            "devices": 1.0,
        }]

        summary = ANALYZER.reconcile_acceptance([], targets, recording)

        self.assertEqual(summary["maximum_trainer_target_delta"], 0.0)

    def test_recording_match_ratio_ignores_rows_after_trace_window(self):
        trace = [{
            "source_ms": 1000.0,
            "watts": 200.0,
            "cadence": 80.0,
            "hr": 140.0,
            "gear": 5.0,
        }]
        recording = [
            {
                "secs": seconds,
                "cad": 80.0,
                "hr": 140.0,
                "km": seconds * 0.01,
                "watts": 200.0,
                "slope": 0.0,
                "target": 200.0,
                "virtualgear": 5.0,
            }
            for seconds in (1.0, 2.0, 3.0)
        ]

        summary = ANALYZER.reconcile_acceptance(trace, [], recording)

        self.assertEqual(summary["recording_samples"], 3)
        self.assertEqual(summary["recording_samples_in_trace_window"], 1)
        self.assertEqual(summary["recording_match_ratio"], 1.0)

    def test_missed_climb_keeps_main_route_without_inconsistency(self):
        trace = [{
            "action_id": 21.0,
            "feature_outcome": "bypassed",
            "feature_terrain": "climb",
            "route": "main",
            "readiness": 0.6,
        }]

        summary = ANALYZER.reconcile_acceptance(trace, [], [])

        self.assertEqual(summary["feature_decisions"], 1)
        self.assertEqual(summary["inconsistent_feature_decisions"], 0)

    def test_recording_parser_accepts_spaced_header_and_trailing_field(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.csv"
            path.write_text(
                "secs, cad, hr, km, watts, slope, target, virtualgear\n"
                "1,80,140,0.01,200,3,210,5,\n",
                encoding="utf-8",
            )

            samples = ANALYZER.parse_recording(path)

            self.assertEqual(samples[0]["secs"], 1.0)
            self.assertEqual(samples[0]["virtualgear"], 5.0)

    def test_recording_parser_rejects_non_monotonic_time(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "recording.csv"
            path.write_text(
                "secs,cad,hr,km,kph,watts,slope,target,virtualgear\n"
                "2,80,140,0.01,20,200,3,210,5\n"
                "1,81,141,0.02,21,201,4,211,5\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "strictly increasing"):
                ANALYZER.parse_recording(path)

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
