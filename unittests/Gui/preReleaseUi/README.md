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
