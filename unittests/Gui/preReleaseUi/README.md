# Pre-release UI tests

This suite exercises a packaged Linux AppImage through Qt's AT-SPI
accessibility interface. It creates a temporary athlete library and never
opens the user's normal GoldenCheetah data.

Run the release matrix with:

```bash
unittests/Gui/preReleaseUi/run-pre-release-ui-matrix.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui
```

The output directory contains `junit.xml`, `goldencheetah.log`, screenshots and,
when `GC_UI_RECORD_VIDEO=1` and `ffmpeg` are available, `session.mp4`.

The Workout Game lifecycle is verified through its filesystem effects as well
as its visible controls. One isolated Data Generator session starts in Game
mode, shifts the virtual gear up and down, stops and continues the same raw
recording, then stops and saves it. The workflow finishes by leaving and
returning to Activities. It selects the saved activity row when the current
Activities implementation exposes one through AT-SPI; otherwise it verifies
the automatically selected activity. In both cases the Activities view's
accessible description must identify the exact new activity file in the
single-activity isolated library. The saved file and reopen result are recorded
in `reopened-activity.txt`. All application and XDG persistence paths run only
inside the temporary athlete library and test home.
When the MTB conversion gate is enabled, the workflow also starts and advances
each of the three generated presets. Earlier smoke rides are discarded, and
trace/recording reconciliation is scoped to the final complete training
session while preserving its Stop/Continue segments.

Set `GC_UI_GENERATOR_MODE` to `on-target`, `over-target`, `under-target`,
`cadence-low`, `cadence-high` or `follow-target` to select the isolated Data
Generator scenario. Unknown values stop the run before GoldenCheetah starts.
Keep the default `GC_UI_GAME_RUN_SECONDS=11.8` when the full UI workflow uses
`GC_WORKOUT_GAME_FEATURE_LAB=1`. A value close to the 122.5-second course
duration lets the workout auto-complete before the later stop, discard and save
tests and therefore invalidates those tests. Validate a full-course trace in a
separate game session. For an interactive 122.5-second ride through all eleven
terrain types, including six progressive unscored berms, use
`run-feature-lab.sh [APPIMAGE]`. When copied beside
`GoldenCheetah-latest.AppImage`, the AppImage argument can be omitted. The
launcher enables Quick 3D, Feature Lab and diagnostics while preserving the
normal athlete/device selection, so it can be used with Data Generator or a
real trainer.
Set `GC_UI_VALIDATE_TRAINER_ACCEPTANCE=1` to preserve the isolated Workout Game
recording as `game-training-recording.csv` and reconcile its telemetry, trainer
targets and feature outcomes with the trace. This mode requires
`GC_WORKOUT_GAME_TRACE=1`, selects the 3D renderer by default and never reads
the normal athlete library. Trace validation replaces the two synchronous X11
game screenshots in this mode so readback cannot distort workout timing.

For an unpackaged development binary, set `GC_UI_APPDIR` to an extracted
AppImage root whose `lib`, `plugins` and `qml` directories provide the matching
runtime. Those paths are applied only to GoldenCheetah, so the host AT-SPI
Python process keeps using its normal system libraries.

The matrix runs three times against separate temporary athletes. The first run
forces the QPainter fallback, the second exercises the packaged Scene Graph
path with trace validation, and the third selects the production Qt Quick 3D
renderer and requires renderer and cold-start trace evidence. The Quick 3D leg
defers synchronous pixel readback until after the measured ten-second
cold-start window, then requires nonblank canvas images. Motion remains
trace-authoritative because synchronous X11 readback can block long enough for
a short test course to finish; comparing those final images would report a
false freeze. This avoids distorting the frame intervals while still detecting
a blank renderer. The
harness uses the visible Data Generator and perspective controls; it does not
require Feature Lab or another hidden course-selection switch. To run only the
Scene Graph leg:

```bash
GC_WORKOUT_GAME_FORCE_PAINTER=0 \
GC_WORKOUT_GAME_TRACE=1 \
GC_WORKOUT_GAME_DIAGNOSTICS=1 \
unittests/Gui/preReleaseUi/run-pre-release-ui.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui-scenegraph
```

To run only the production Quick 3D leg:

```bash
GC_WORKOUT_GAME_FORCE_PAINTER=0 \
GC_WORKOUT_GAME_3D=1 \
GC_WORKOUT_GAME_TRACE=1 \
GC_WORKOUT_GAME_DIAGNOSTICS=1 \
GC_UI_REQUIRE_QUICK3D_EVIDENCE=1 \
unittests/Gui/preReleaseUi/run-pre-release-ui.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui-quick3d
```

On a test session where direct rendering is available, add
`GC_UI_USE_HARDWARE_GL=1`. Every mode still uses the temporary athlete library
created by the runner.

To exercise the target desktop GPU instead of Xvfb, set
`GC_UI_EXISTING_DISPLAY` to an accessible display such as `:1`, set the matching
`XAUTHORITY`, enable `GC_UI_USE_HARDWARE_GL=1`, and set
`GC_UI_EXPECTED_GPU_PATTERN` to a case-insensitive extended regular expression
matching the intended adapter. The runner requires that adapter in the same
application log and saves the matching line as `gpu-evidence.txt`. The
application remains on the isolated athlete library, but its test window is
visible on that desktop.
The desktop must remain unlocked for the complete run; a screen lock hides the
window and removes its visible controls from AT-SPI. The runner starts the
AppImage in an owned process group and terminates that complete group on every
success, failure and signal path, including launchers that leave `AppRun`
children behind.
Trainer-acceptance runs on an existing display use trace validation instead of
root-window screenshots because compositors may deny root pixel readback.

## Workout Game diagnostics

The packaged application can expose world and frame state without a debugger:

```bash
GC_WORKOUT_GAME_DIAGNOSTICS=1 \
GC_WORKOUT_GAME_TRACE=1 \
GC_WORKOUT_GAME_CAPTURE_DIR=/tmp/gc-game-frames \
GC_WORKOUT_GAME_CAPTURE_MS=100 \
GC_WORKOUT_GAME_CAPTURE_FRAMES=120 \
/path/to/GoldenCheetah.AppImage
```

`GC_WORKOUT_GAME_DIAGNOSTICS` adds road distance, source and rendered workout
time, section progress, frame interval, late/stationary/backward-frame counters,
rolling realized FPS, p95 frame interval, skipped simulation ticks, and maximum
regression to the debug HUD. `GC_WORKOUT_GAME_TRACE` writes the same state to the
application log about four times per second. The pre-release runner validates a
trace automatically and writes `workout-game-summary.json` when tracing is on.

Direct capture writes numbered PNG files while a visible game session is
running. Capturing reads pixels back from the GPU and can disturb frame pacing,
so use trace-only runs for performance measurements. A capture can be encoded
afterward with:

```bash
ffmpeg -framerate 10 -i /tmp/gc-game-frames/frame-%06d.png \
  -c:v libx264 -pix_fmt yuv420p workout-game.mp4
```

Qt scene graph backend and timing details can be added to the log with:

```bash
QSG_INFO=1 QSG_RENDER_TIMING=1 \
QT_LOGGING_RULES='qt.scenegraph.general=true;qt.scenegraph.time.renderloop=true' \
/path/to/GoldenCheetah.AppImage
```

Required Linux packages are `Xvfb`, `dbus-run-session`, `gdbus`, Python 3,
PyGObject AT-SPI bindings (`python3-pyatspi`) and Python Xlib
(`python3-xlib`). The AppImage is run with extraction mode so FUSE is not a
test prerequisite.

## Isolated real-trainer acceptance

The final trainer gate is interactive, but it still uses a generated athlete
library and does not open normal athlete data. Run it from the target desktop:

```bash
unittests/Gui/preReleaseUi/run-real-trainer-acceptance.sh \
  /path/to/GoldenCheetah.AppImage /tmp/gc-real-trainer-acceptance
```

Add or select the real trainer in the opened profile, select the prepared
workout and Workout Game, then ride through at least one feature. The runner
keeps an evidence copy of the raw recording even if the session is discarded.
After GoldenCheetah closes, it verifies that rendered power, cadence, heart
rate and virtual gear agree with the recording, that the dispatched ERG/slope
target agrees with the corresponding recording field, and that feature
outcome, readiness and route agree. Shift up and back down during the ride; the
report also requires both transitions and rejects an immediate speed step. The
report is written to
`workout-game-trainer-summary.json`; logs contain no device names or addresses.
The interactive runner owns the complete AppImage process group, continues
copying the raw recording if the extraction launcher exits before `AppRun`, and
cleans up the group on interruption or analyzer failure.
