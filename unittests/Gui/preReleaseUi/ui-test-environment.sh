#!/usr/bin/env bash

PRESERVED_XDG_RUNTIME_DIR=

capture_ui_test_session_environment()
{
    local runtime_dir

    if [ "$#" -ne 1 ]; then
        echo "capture_ui_test_session_environment requires EXISTING_DISPLAY" >&2
        return 2
    fi

    PRESERVED_XDG_RUNTIME_DIR=
    [ -n "$1" ] || return 0

    [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || {
        echo "Existing display requires the matching desktop D-Bus session" >&2
        return 2
    }

    runtime_dir=${XDG_RUNTIME_DIR:-}
    case "$runtime_dir" in
        /*) ;;
        *)
            echo "Existing display requires an absolute XDG_RUNTIME_DIR" >&2
            return 2
            ;;
    esac
    [ -d "$runtime_dir" ] || {
        echo "Existing display XDG_RUNTIME_DIR is not a directory: $runtime_dir" >&2
        return 2
    }
    [ -O "$runtime_dir" ] || {
        echo "Existing display XDG_RUNTIME_DIR is not owned by this user" >&2
        return 2
    }
    [ "$(stat -Lc %a -- "$runtime_dir")" = 700 ] || {
        echo "Existing display XDG_RUNTIME_DIR must have mode 0700" >&2
        return 2
    }
    PRESERVED_XDG_RUNTIME_DIR=$runtime_dir
}

require_unlocked_desktop_session()
{
    local lock_state

    lock_state=$(gdbus call --session --dest org.gnome.ScreenSaver \
        --object-path /org/gnome/ScreenSaver \
        --method org.gnome.ScreenSaver.GetActive 2>/dev/null) || {
        echo "Cannot verify desktop unlock state through org.gnome.ScreenSaver" >&2
        return 2
    }
    case "$lock_state" in
        "(false,)") return 0 ;;
        "(true,)")
            echo "Existing desktop session is locked; unlock it before hardware UI validation" >&2
            return 1
            ;;
        *)
            echo "Desktop lock service returned an invalid state: $lock_state" >&2
            return 2
            ;;
    esac
}

configure_ui_test_xdg_environment()
{
    if [ "$#" -ne 1 ]; then
        echo "configure_ui_test_xdg_environment requires TEST_HOME" >&2
        return 2
    fi

    export HOME=$1
    export XDG_CONFIG_HOME=$HOME/.config
    export XDG_CACHE_HOME=$HOME/.cache
    export XDG_DATA_HOME=$HOME/.local/share
    export XDG_STATE_HOME=$HOME/.local/state
    mkdir -p -- "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$XDG_DATA_HOME" \
        "$XDG_STATE_HOME"

    if [ -n "$PRESERVED_XDG_RUNTIME_DIR" ]; then
        export XDG_RUNTIME_DIR=$PRESERVED_XDG_RUNTIME_DIR
    else
        export XDG_RUNTIME_DIR=$HOME/.runtime
        mkdir -p -- "$XDG_RUNTIME_DIR"
        chmod 700 -- "$XDG_RUNTIME_DIR"
    fi
}
