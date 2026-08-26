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

The stop workflow is verified through its filesystem effects as well as its
buttons. Continuing must resume growth of the same raw recording, Cancel must
remove that recording, and Save must publish a new activity JSON before the
import dialog can finish. All paths run only inside the temporary athlete
library.

Set `GC_UI_GENERATOR_MODE` to `on-target`, `over-target`, `under-target`,
`cadence-low`, `cadence-high` or `follow-target` to select the isolated Data
Generator scenario. Unknown values stop the run before GoldenCheetah starts.
Set `GC_UI_GAME_RUN_SECONDS=70` with `GC_WORKOUT_GAME_FEATURE_LAB=1` to
capture all eleven feature sections without crossing the 72-second automatic
workout-completion boundary during the surrounding UI workflow.
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

The matrix runs twice against separate temporary athletes. The first run forces
the QPainter fallback. The second run exercises the packaged Scene Graph path
with trace validation. To run only the Scene Graph leg:

```bash
GC_WORKOUT_GAME_FORCE_PAINTER=0 \
GC_WORKOUT_GAME_TRACE=1 \
GC_WORKOUT_GAME_DIAGNOSTICS=1 \
unittests/Gui/preReleaseUi/run-pre-release-ui.sh \
  /path/to/GoldenCheetah.AppImage artifacts/ui-scenegraph
```

On a test session where direct rendering is available, add
`GC_UI_USE_HARDWARE_GL=1`. Every mode still uses the temporary athlete library
created by the runner.

To exercise the target desktop GPU instead of Xvfb, set
`GC_UI_EXISTING_DISPLAY` to an accessible display such as `:1`, set the matching
`XAUTHORITY`, and enable `GC_UI_USE_HARDWARE_GL=1`. The application remains on
the isolated athlete library, but its test window is visible on that desktop.
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
