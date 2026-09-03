#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 APPIMAGE NEW_ARTIFACT_DIR" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IMAGE=$(cd -- "$(dirname -- "$1")" && pwd -P)/$(basename -- "$1")
ARTIFACT_DIR=$2

[ -f "$IMAGE" ] && [ -x "$IMAGE" ] || {
    echo "AppImage is missing or not executable: $IMAGE" >&2
    exit 2
}
[ ! -e "$ARTIFACT_DIR" ] || {
    echo "Artifact directory must not already exist: $ARTIFACT_DIR" >&2
    exit 2
}
[ -n "${DISPLAY:-}" ] || {
    echo "DISPLAY is not set; run this from the target desktop session" >&2
    exit 2
}
command -v setsid >/dev/null || {
    echo "Missing real-trainer test dependency: setsid" >&2
    exit 2
}

mkdir -p -- "$ARTIFACT_DIR"
ARTIFACT_DIR=$(cd -- "$ARTIFACT_DIR" && pwd -P)
TEST_ROOT=$ARTIFACT_DIR/isolated-profile
RECORDING_COPY=$ARTIFACT_DIR/training-recording.csv
APP_PID=
APP_PGID=
CANVAS_OBSERVER_PID=
CANVAS_NAME_FILE=$ARTIFACT_DIR/renderer-canvas-name.txt

stop_app_group()
{
    [ -n "$APP_PGID" ] || return
    if kill -0 -- "-$APP_PGID" 2>/dev/null; then
        kill -TERM -- "-$APP_PGID" 2>/dev/null
        for unused in $(seq 1 30); do
            kill -0 -- "-$APP_PGID" 2>/dev/null || break
            sleep 0.1
        done
        kill -0 -- "-$APP_PGID" 2>/dev/null && \
            kill -KILL -- "-$APP_PGID" 2>/dev/null
    fi
    [ -z "$APP_PID" ] || wait "$APP_PID" 2>/dev/null
}

cleanup()
{
    set +e
    stop_app_group
    [ -z "$CANVAS_OBSERVER_PID" ] || kill "$CANVAS_OBSERVER_PID" 2>/dev/null
    [ -z "$CANVAS_OBSERVER_PID" ] || wait "$CANVAS_OBSERVER_PID" 2>/dev/null
}
trap cleanup EXIT HUP INT TERM

python3 "$SCRIPT_DIR/pre_release_ui.py" prepare "$TEST_ROOT"

export HOME=$TEST_ROOT/home
export XDG_CONFIG_HOME=$HOME/.config
export XDG_CACHE_HOME=$HOME/.cache
export XDG_DATA_HOME=$HOME/.local/share
export XDG_STATE_HOME=$HOME/.local/state
export XDG_RUNTIME_DIR=$HOME/.runtime
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
    "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export APPIMAGE_EXTRACT_AND_RUN=${APPIMAGE_EXTRACT_AND_RUN:-1}
export GC_WORKOUT_GAME_TRACE=1
export GC_WORKOUT_GAME_DIAGNOSTICS=${GC_WORKOUT_GAME_DIAGNOSTICS:-1}
export GC_WORKOUT_GAME_3D=1
if [ "${GC_UI_USE_HARDWARE_GL:-0}" = 1 ]; then
    export QT_OPENGL=${QT_OPENGL:-desktop}
    export QSG_INFO=1
    unset LIBGL_ALWAYS_SOFTWARE
fi
if [ -n "${GC_UI_EXPECTED_GPU_PATTERN:-}" ] && \
    [ "${GC_UI_USE_HARDWARE_GL:-0}" != 1 ]; then
    echo "GC_UI_EXPECTED_GPU_PATTERN requires GC_UI_USE_HARDWARE_GL=1" >&2
    exit 2
fi
export QT_ACCESSIBILITY=1
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export GC_UI_RENDERER_CANVAS_EVIDENCE_FILE=$CANVAS_NAME_FILE

cat <<'EOF'
An isolated GoldenCheetah profile will open. Add or select the real trainer,
select the prepared workout and Workout Game, then ride through at least one
feature. Shift up and back down while moving. Stop with Save or Cancel and
close GoldenCheetah when finished.
No file under the normal athlete library is opened or modified.
EOF

setsid "$IMAGE" "$TEST_ROOT/library" UiTestAthlete --debug \
    >"$ARTIFACT_DIR/application.log" 2>&1 &
APP_PID=$!
APP_PGID=$APP_PID
APP_GROUP_READY=0
for unused in $(seq 1 50); do
    if kill -0 -- "-$APP_PGID" 2>/dev/null; then
        APP_GROUP_READY=1
        break
    fi
    sleep 0.02
done
[ "$APP_GROUP_READY" -eq 1 ] || {
    echo "GoldenCheetah process group did not start" >&2
    exit 1
}
python3 "$SCRIPT_DIR/pre_release_ui.py" observe-canvas \
    "$CANVAS_NAME_FILE" "$APP_PGID" \
    >"$ARTIFACT_DIR/canvas-observer.log" 2>&1 &
CANVAS_OBSERVER_PID=$!

RECORDS=$TEST_ROOT/library/UiTestAthlete/records
while kill -0 -- "-$APP_PGID" 2>/dev/null; do
    newest=$(find "$RECORDS" -maxdepth 1 -type f -name '*.csv' \
        -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2-)
    if [ -n "$newest" ]; then
        cp -f -- "$newest" "$RECORDING_COPY"
    fi
    sleep 0.2
done

set +e
wait "$APP_PID"
APP_STATUS=$?
set -e
APP_PID=
APP_PGID=
wait "$CANVAS_OBSERVER_PID" 2>/dev/null || true
CANVAS_OBSERVER_PID=
[ "$APP_STATUS" -eq 0 ] || {
    echo "GoldenCheetah exited with status $APP_STATUS" >&2
    exit "$APP_STATUS"
}
[ -s "$RECORDING_COPY" ] || {
    echo "No training recording was captured" >&2
    exit 1
}

if [ -n "${GC_UI_EXPECTED_GPU_PATTERN:-}" ]; then
    grep -Ei \
        'qt\.(scenegraph|rhi)|OpenGL|GL_RENDERER|Driver Info|Graphics API' \
        "$ARTIFACT_DIR/application.log" | \
        grep -Eim1 -- "$GC_UI_EXPECTED_GPU_PATTERN" \
        >"$ARTIFACT_DIR/gpu-evidence.txt" || {
        echo "Expected GPU was not reported by the application" >&2
        exit 1
    }
fi

if [ -f "$TEST_ROOT/library/goldencheetah.log" ]; then
    cp -f -- "$TEST_ROOT/library/goldencheetah.log" \
        "$ARTIFACT_DIR/goldencheetah.log"
fi

python3 "$SCRIPT_DIR/analyze_workout_game.py" \
    "$ARTIFACT_DIR/application.log" \
    --recording "$RECORDING_COPY" \
    --json "$ARTIFACT_DIR/workout-game-trainer-summary.json" \
    --require-quick3d-evidence \
    --renderer-evidence-json "$ARTIFACT_DIR/renderer-evidence.json" \
    --accessible-canvas-name-file "$CANVAS_NAME_FILE" \
    --appimage "$IMAGE"

echo "Trainer acceptance passed: $ARTIFACT_DIR/workout-game-trainer-summary.json"
