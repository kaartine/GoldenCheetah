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

mkdir -p -- "$ARTIFACT_DIR"
ARTIFACT_DIR=$(cd -- "$ARTIFACT_DIR" && pwd -P)
TEST_ROOT=$ARTIFACT_DIR/isolated-profile
RECORDING_COPY=$ARTIFACT_DIR/training-recording.csv
APP_PID=

cleanup()
{
    set +e
    [ -z "$APP_PID" ] || kill "$APP_PID" 2>/dev/null
    [ -z "$APP_PID" ] || wait "$APP_PID" 2>/dev/null
}
trap cleanup EXIT HUP INT TERM

python3 "$SCRIPT_DIR/pre_release_ui.py" prepare "$TEST_ROOT"

export HOME=$TEST_ROOT/home
export XDG_CONFIG_HOME=$HOME/.config
export XDG_CACHE_HOME=$HOME/.cache
export APPIMAGE_EXTRACT_AND_RUN=${APPIMAGE_EXTRACT_AND_RUN:-1}
export GC_WORKOUT_GAME_TRACE=1
export GC_WORKOUT_GAME_DIAGNOSTICS=${GC_WORKOUT_GAME_DIAGNOSTICS:-1}
export GC_WORKOUT_GAME_3D=${GC_WORKOUT_GAME_3D:-1}

cat <<'EOF'
An isolated GoldenCheetah profile will open. Add or select the real trainer,
select the prepared workout and Workout Game, then ride through at least one
feature. Shift up and back down while moving. Stop with Save or Cancel and
close GoldenCheetah when finished.
No file under the normal athlete library is opened or modified.
EOF

"$IMAGE" "$TEST_ROOT/library" UiTestAthlete --debug \
    >"$ARTIFACT_DIR/application.log" 2>&1 &
APP_PID=$!

RECORDS=$TEST_ROOT/library/UiTestAthlete/records
while kill -0 "$APP_PID" 2>/dev/null; do
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
[ "$APP_STATUS" -eq 0 ] || {
    echo "GoldenCheetah exited with status $APP_STATUS" >&2
    exit "$APP_STATUS"
}
[ -s "$RECORDING_COPY" ] || {
    echo "No training recording was captured" >&2
    exit 1
}

TRACE_LOG=$ARTIFACT_DIR/application.log
if [ -f "$TEST_ROOT/library/goldencheetah.log" ] && \
    grep -q 'workout-game-3d-trace ' "$TEST_ROOT/library/goldencheetah.log"; then
    cp -f -- "$TEST_ROOT/library/goldencheetah.log" \
        "$ARTIFACT_DIR/goldencheetah.log"
    TRACE_LOG=$ARTIFACT_DIR/goldencheetah.log
fi

python3 "$SCRIPT_DIR/analyze_workout_game.py" \
    "$TRACE_LOG" \
    --recording "$RECORDING_COPY" \
    --json "$ARTIFACT_DIR/workout-game-trainer-summary.json"

echo "Trainer acceptance passed: $ARTIFACT_DIR/workout-game-trainer-summary.json"
