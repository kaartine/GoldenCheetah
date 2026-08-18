#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 APPIMAGE [ARTIFACT_DIR]" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
IMAGE=$(cd -- "$(dirname -- "$1")" && pwd -P)/$(basename -- "$1")
ARTIFACT_DIR=${2:-$PWD/ui-test-artifacts}

[ -f "$IMAGE" ] && [ -x "$IMAGE" ] || {
    echo "AppImage is missing or not executable: $IMAGE" >&2
    exit 2
}

for command in Xvfb dbus-run-session gdbus python3; do
    command -v "$command" >/dev/null || {
        echo "Missing UI test dependency: $command" >&2
        exit 2
    }
done
python3 -c 'import pyatspi; import Xlib' 2>/dev/null || {
    echo "Missing Python UI dependencies: pyatspi and/or Xlib" >&2
    exit 2
}

if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$IMAGE" "$ARTIFACT_DIR"
fi

mkdir -p -- "$ARTIFACT_DIR"
ARTIFACT_DIR=$(cd -- "$ARTIFACT_DIR" && pwd -P)
TEST_ROOT=$(mktemp -d)
DISPLAY_NUMBER=
APP_PID=
XVFB_PID=
VIDEO_PID=

cleanup()
{
    set +e
    [ -z "$VIDEO_PID" ] || kill -INT "$VIDEO_PID" 2>/dev/null
    [ -z "$APP_PID" ] || kill "$APP_PID" 2>/dev/null
    [ -z "$XVFB_PID" ] || kill "$XVFB_PID" 2>/dev/null
    [ -z "$VIDEO_PID" ] || wait "$VIDEO_PID" 2>/dev/null
    [ -z "$APP_PID" ] || wait "$APP_PID" 2>/dev/null
    [ -z "$XVFB_PID" ] || wait "$XVFB_PID" 2>/dev/null
    rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

for candidate in $(seq 91 119); do
    if [ ! -e "/tmp/.X11-unix/X$candidate" ]; then
        DISPLAY_NUMBER=$candidate
        break
    fi
done
[ -n "$DISPLAY_NUMBER" ] || {
    echo "No free X display for UI tests" >&2
    exit 2
}

python3 "$SCRIPT_DIR/pre_release_ui.py" prepare "$TEST_ROOT"

Xvfb ":$DISPLAY_NUMBER" -screen 0 1440x900x24 -nolisten tcp \
    >"$ARTIFACT_DIR/xvfb.log" 2>&1 &
XVFB_PID=$!
export DISPLAY=:$DISPLAY_NUMBER

for unused in $(seq 1 50); do
    [ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ] && break
    sleep 0.1
done
[ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ] || {
    echo "Xvfb did not become ready" >&2
    exit 1
}

gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
    --method org.a11y.Bus.GetAddress >"$ARTIFACT_DIR/at-spi-address.log"

export HOME=$TEST_ROOT/home
export XDG_CONFIG_HOME=$HOME/.config
export XDG_CACHE_HOME=$HOME/.cache
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export QT_ACCESSIBILITY=1
export QT_OPENGL=software
export LIBGL_ALWAYS_SOFTWARE=1
export QTWEBENGINE_DISABLE_SANDBOX=1
export APPIMAGE_EXTRACT_AND_RUN=1
export GC_WORKOUT_GAME_FORCE_PAINTER=${GC_WORKOUT_GAME_FORCE_PAINTER:-1}

if [ "${GC_UI_RECORD_VIDEO:-0}" = 1 ] && command -v ffmpeg >/dev/null; then
    ffmpeg -nostdin -loglevel warning -y -f x11grab -framerate 12 \
        -video_size 1440x900 -i "$DISPLAY" -c:v libx264 -preset ultrafast \
        -pix_fmt yuv420p "$ARTIFACT_DIR/session.mp4" \
        >"$ARTIFACT_DIR/ffmpeg.log" 2>&1 &
    VIDEO_PID=$!
fi

"$IMAGE" "$TEST_ROOT/library" UiTestAthlete --debug \
    >"$ARTIFACT_DIR/application.log" 2>&1 &
APP_PID=$!

set +e
python3 "$SCRIPT_DIR/pre_release_ui.py" exercise \
    "$TEST_ROOT" "$ARTIFACT_DIR" "$APP_PID"
STATUS=$?
set -e

if [ "$STATUS" -eq 0 ]; then
    for unused in $(seq 1 50); do
        ! kill -0 "$APP_PID" 2>/dev/null && break
        sleep 0.1
    done
    if kill -0 "$APP_PID" 2>/dev/null; then
        echo "GoldenCheetah did not exit after the UI shutdown request" >&2
        STATUS=1
    fi
fi

exit "$STATUS"
