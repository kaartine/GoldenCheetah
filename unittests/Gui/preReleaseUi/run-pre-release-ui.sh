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

REQUIRED_COMMANDS=(dbus-run-session gdbus python3 setsid)
if [ -z "${GC_UI_EXISTING_DISPLAY:-}" ]; then
    REQUIRED_COMMANDS+=(Xvfb)
fi
for command in "${REQUIRED_COMMANDS[@]}"; do
    command -v "$command" >/dev/null || {
        echo "Missing UI test dependency: $command" >&2
        exit 2
    }
done
python3 -c 'import pyatspi; import Xlib' 2>/dev/null || {
    echo "Missing Python UI dependencies: pyatspi and/or Xlib" >&2
    exit 2
}

if [ -z "${GC_UI_DBUS_SESSION:-}" ]; then
    exec env -u DBUS_SESSION_BUS_ADDRESS GC_UI_DBUS_SESSION=1 \
        dbus-run-session -- "$0" "$IMAGE" "$ARTIFACT_DIR"
fi

mkdir -p -- "$ARTIFACT_DIR"
ARTIFACT_DIR=$(cd -- "$ARTIFACT_DIR" && pwd -P)
TEST_ROOT=$(mktemp -d)
DISPLAY_NUMBER=
APP_PID=
APP_PGID=
XVFB_PID=
VIDEO_PID=

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
    [ -z "$VIDEO_PID" ] || kill -INT "$VIDEO_PID" 2>/dev/null
    stop_app_group
    [ -z "$XVFB_PID" ] || kill "$XVFB_PID" 2>/dev/null
    [ -z "$VIDEO_PID" ] || wait "$VIDEO_PID" 2>/dev/null
    [ -z "$XVFB_PID" ] || wait "$XVFB_PID" 2>/dev/null
    rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT HUP INT TERM

python3 "$SCRIPT_DIR/pre_release_ui.py" prepare "$TEST_ROOT"

if [ -n "${GC_UI_EXISTING_DISPLAY:-}" ]; then
    export DISPLAY=$GC_UI_EXISTING_DISPLAY
    python3 -c 'from Xlib import display; connection = display.Display(); connection.sync(); connection.close()' \
        >/dev/null 2>&1 || {
        echo "Existing X display is not accessible: $DISPLAY" >&2
        exit 1
    }
else
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

    Xvfb ":$DISPLAY_NUMBER" -screen 0 1440x900x24 -nolisten tcp \
        >"$ARTIFACT_DIR/xvfb.log" 2>&1 &
    XVFB_PID=$!
    export DISPLAY=:$DISPLAY_NUMBER

    for unused in $(seq 1 50); do
        if [ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ] && \
            python3 -c 'from Xlib import display; connection = display.Display(); connection.sync(); connection.close()' \
                >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    [ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ] && \
        python3 -c 'from Xlib import display; connection = display.Display(); connection.close()' \
            >/dev/null 2>&1 || {
        echo "Xvfb did not become ready" >&2
        exit 1
    }
fi

AT_SPI_REPLY=$(gdbus call --session --dest org.a11y.Bus \
    --object-path /org/a11y/bus --method org.a11y.Bus.GetAddress)
printf '%s\n' "$AT_SPI_REPLY" >"$ARTIFACT_DIR/at-spi-address.log"
AT_SPI_BUS_ADDRESS=$(printf '%s\n' "$AT_SPI_REPLY" | \
    sed -n "s/^('\\(.*\\)',)$/\\1/p")
[ -n "$AT_SPI_BUS_ADDRESS" ] || {
    echo "AT-SPI bus returned an invalid address" >&2
    exit 1
}
export AT_SPI_BUS_ADDRESS
AT_SPI_READY=0
for unused in $(seq 1 50); do
    if python3 -c 'import pyatspi; desktop = pyatspi.Registry.getDesktop(0); desktop.childCount' \
        >/dev/null 2>&1; then
        AT_SPI_READY=1
        break
    fi
    sleep 0.1
done
[ "$AT_SPI_READY" -eq 1 ] || {
    echo "AT-SPI accessibility registry did not become ready" >&2
    exit 1
}

export HOME=$TEST_ROOT/home
export XDG_CONFIG_HOME=$HOME/.config
export XDG_CACHE_HOME=$HOME/.cache
export XDG_DATA_HOME=$HOME/.local/share
export XDG_STATE_HOME=$HOME/.local/state
export XDG_RUNTIME_DIR=$HOME/.runtime
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
    "$XDG_STATE_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
export QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
export QT_ACCESSIBILITY=1
if [ "${GC_UI_USE_HARDWARE_GL:-0}" = 1 ]; then
    export QT_OPENGL=${QT_OPENGL:-desktop}
    export QSG_INFO=1
    unset LIBGL_ALWAYS_SOFTWARE
else
    export QT_OPENGL=${QT_OPENGL:-software}
    export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
fi
if [ -n "${GC_UI_EXPECTED_GPU_PATTERN:-}" ] && \
    [ "${GC_UI_USE_HARDWARE_GL:-0}" != 1 ]; then
    echo "GC_UI_EXPECTED_GPU_PATTERN requires GC_UI_USE_HARDWARE_GL=1" >&2
    exit 2
fi
export QTWEBENGINE_DISABLE_SANDBOX=1
export APPIMAGE_EXTRACT_AND_RUN=1
export GC_WORKOUT_GAME_FORCE_PAINTER=${GC_WORKOUT_GAME_FORCE_PAINTER:-1}
if [ -n "${GC_UI_REQUIRE_QUICK3D_EVIDENCE+x}" ]; then
    REQUIRE_QUICK3D_EVIDENCE=$GC_UI_REQUIRE_QUICK3D_EVIDENCE
elif [ "${GC_UI_VALIDATE_TRAINER_ACCEPTANCE:-0}" = 1 ] || \
    [ "${GC_WORKOUT_GAME_3D:-0}" = 1 ]; then
    REQUIRE_QUICK3D_EVIDENCE=1
else
    REQUIRE_QUICK3D_EVIDENCE=0
fi
case "$REQUIRE_QUICK3D_EVIDENCE" in
    0|1) ;;
    *)
        echo "GC_UI_REQUIRE_QUICK3D_EVIDENCE must be 0 or 1" >&2
        exit 2
        ;;
esac
if [ "${GC_UI_VALIDATE_TRAINER_ACCEPTANCE:-0}" = 1 ] && \
    [ "$REQUIRE_QUICK3D_EVIDENCE" != 1 ]; then
    echo "Trainer acceptance requires Quick 3D trace evidence" >&2
    exit 2
fi
if [ "$REQUIRE_QUICK3D_EVIDENCE" = 1 ]; then
    export GC_WORKOUT_GAME_3D=1
    export GC_WORKOUT_GAME_TRACE=1
fi

APP_ENV=()
if [ -n "${GC_UI_APPDIR:-}" ]; then
    APPDIR=$(cd -- "$GC_UI_APPDIR" && pwd -P)
    [ -d "$APPDIR/lib" ] && [ -d "$APPDIR/plugins" ] || {
        echo "GC_UI_APPDIR is not an extracted AppImage runtime: $APPDIR" >&2
        exit 2
    }
    APP_ENV=(
        env
        "APPDIR=$APPDIR"
        "LD_LIBRARY_PATH=$APPDIR/lib:$APPDIR/usr/lib"
        "QT_PLUGIN_PATH=$APPDIR/plugins"
        "QML2_IMPORT_PATH=$APPDIR/qml"
    )
fi

if [ "${GC_UI_RECORD_VIDEO:-0}" = 1 ] && command -v ffmpeg >/dev/null; then
    ffmpeg -nostdin -loglevel warning -y -f x11grab -framerate 12 \
        -video_size 1440x900 -i "$DISPLAY" -c:v libx264 -preset ultrafast \
        -pix_fmt yuv420p "$ARTIFACT_DIR/session.mp4" \
        >"$ARTIFACT_DIR/ffmpeg.log" 2>&1 &
    VIDEO_PID=$!
fi

setsid "${APP_ENV[@]}" "$IMAGE" "$TEST_ROOT/library" UiTestAthlete --debug \
    >"$ARTIFACT_DIR/application.log" 2>&1 &
APP_PID=$!
APP_PGID=$APP_PID

set +e
python3 "$SCRIPT_DIR/pre_release_ui.py" exercise \
    "$TEST_ROOT" "$ARTIFACT_DIR" "$APP_PGID"
STATUS=$?
set -e

if [ -f "$TEST_ROOT/library/goldencheetah.log" ]; then
    cp -f -- "$TEST_ROOT/library/goldencheetah.log" \
        "$ARTIFACT_DIR/goldencheetah.log"
fi

if [ "$STATUS" -eq 0 ] && [ "${GC_UI_VALIDATE_MTB_COURSE:-0}" = 1 ]; then
    MTB_RUNTIME_EVIDENCE=$ARTIFACT_DIR/mtb-course-runtime-evidence.txt
    if grep -E 'Workout Game session course: distance-course' \
            "$ARTIFACT_DIR/application.log" >"$MTB_RUNTIME_EVIDENCE"; then
        :
    else
        echo "Generated MTB distance course was not active at game start" >&2
        STATUS=1
    fi
fi

if [ "$STATUS" -eq 0 ] && [ "${GC_WORKOUT_GAME_TRACE:-0}" = 1 ]; then
    TRACE_LOG=$ARTIFACT_DIR/application.log
    if [ "$REQUIRE_QUICK3D_EVIDENCE" = 0 ] && \
        [ -f "$ARTIFACT_DIR/goldencheetah.log" ] && \
        grep -Eq 'workout-game(-3d)?-trace ' \
            "$ARTIFACT_DIR/goldencheetah.log"; then
        TRACE_LOG=$ARTIFACT_DIR/goldencheetah.log
    fi
    ANALYZER_ARGS=(
        "$TRACE_LOG"
        --json "$ARTIFACT_DIR/workout-game-summary.json"
    )
    if [ "$REQUIRE_QUICK3D_EVIDENCE" = 1 ]; then
        ANALYZER_ARGS+=(
            --require-quick3d-evidence
            --require-cold-start-continuity
            --renderer-evidence-json "$ARTIFACT_DIR/renderer-evidence.json"
            --accessible-canvas-name-file \
                "$TEST_ROOT/renderer-canvas-name.txt"
            --appimage "$IMAGE"
        )
        if [ "${GC_UI_USE_HARDWARE_GL:-0}" != 1 ]; then
            ANALYZER_ARGS+=(--cold-start-continuity-only)
        fi
    fi
    if [ "${GC_WORKOUT_GAME_FEATURE_LAB:-0}" = 1 ]; then
        GAP_SCENARIO=${GC_WORKOUT_GAME_FEATURE_LAB_GAP_SCENARIO:-}
        if [ -n "$GAP_SCENARIO" ]; then
            ANALYZER_ARGS+=(--expected-gap-line "$GAP_SCENARIO")
        else
            ANALYZER_ARGS+=(--require-gap-launch-window)
        fi
    fi
    if [ "${GC_UI_VALIDATE_TRAINER_ACCEPTANCE:-0}" = 1 ]; then
        RECORDING=$ARTIFACT_DIR/game-training-recording.csv
        if [ ! -s "$RECORDING" ]; then
            echo "Workout Game training recording was not preserved" >&2
            STATUS=1
        else
            ANALYZER_ARGS+=(--recording "$RECORDING")
        fi
    fi
    if [ "$STATUS" -eq 0 ]; then
        python3 "$SCRIPT_DIR/analyze_workout_game.py" \
            "${ANALYZER_ARGS[@]}" || STATUS=$?
    fi
fi

if [ "$STATUS" -ne 0 ] && [ "$REQUIRE_QUICK3D_EVIDENCE" = 1 ] && \
    [ ! -f "$ARTIFACT_DIR/renderer-evidence.json" ]; then
    python3 "$SCRIPT_DIR/analyze_workout_game.py" \
        "$ARTIFACT_DIR/application.log" \
        --require-quick3d-evidence \
        --renderer-evidence-json "$ARTIFACT_DIR/renderer-evidence.json" \
        --accessible-canvas-name-file "$TEST_ROOT/renderer-canvas-name.txt" \
        --appimage "$IMAGE" \
        --renderer-evidence-only >/dev/null 2>&1 || true
fi

if [ "$STATUS" -eq 0 ] && [ -n "${GC_UI_EXPECTED_GPU_PATTERN:-}" ]; then
    if ! grep -Ei \
        'qt\.(scenegraph|rhi)|OpenGL|GL_RENDERER|Driver Info|Graphics API' \
        "$ARTIFACT_DIR/application.log" | \
        grep -Eim1 -- "$GC_UI_EXPECTED_GPU_PATTERN" \
        >"$ARTIFACT_DIR/gpu-evidence.txt"; then
        echo "Expected GPU was not reported by the application" >&2
        STATUS=1
    fi
fi

if [ "$STATUS" -eq 0 ]; then
    for unused in $(seq 1 50); do
        ! kill -0 -- "-$APP_PGID" 2>/dev/null && break
        sleep 0.1
    done
    if kill -0 -- "-$APP_PGID" 2>/dev/null; then
        echo "GoldenCheetah did not exit after the UI shutdown request" >&2
        STATUS=1
    fi
fi

exit "$STATUS"
