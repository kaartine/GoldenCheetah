#!/usr/bin/env bash

if [ -z "${REPOSITORY_ROOT:-}" ]; then
    echo "REPOSITORY_ROOT must be set before loading build-input paths." >&2
    return 1 2>/dev/null || exit 1
fi

GC_APPVEYOR_INPUT_ROOT=${GC_APPVEYOR_INPUT_ROOT:-${TMPDIR:-/tmp}/goldencheetah-appveyor-inputs-${APPVEYOR_BUILD_ID:-local}}
GC_D2XX_ROOT="$GC_APPVEYOR_INPUT_ROOT/D2XX"
GC_SRMIO_ROOT="$GC_APPVEYOR_INPUT_ROOT/srmio"
GC_PYTHON_SOURCE_ROOT="$GC_APPVEYOR_INPUT_ROOT/python-source"
export GC_APPVEYOR_INPUT_ROOT GC_D2XX_ROOT GC_SRMIO_ROOT
export GC_PYTHON_SOURCE_ROOT

validate_appveyor_build_input_root()
{
    case "$GC_APPVEYOR_INPUT_ROOT" in
    /*) ;;
    *)
        echo "AppVeyor build-input root must be absolute." >&2
        return 1
        ;;
    esac
    case "/$GC_APPVEYOR_INPUT_ROOT/" in
    *'/../'*|*'/./'*)
        echo "AppVeyor build-input root is not normalized." >&2
        return 1
        ;;
    esac
    local repository
    repository=$(cd -- "$REPOSITORY_ROOT" && pwd -P) || return
    case "$GC_APPVEYOR_INPUT_ROOT" in
    /|"$repository"|"$repository"/*)
        echo "AppVeyor build inputs must remain outside the source worktree." >&2
        return 1
        ;;
    esac
    if [ -L "$GC_APPVEYOR_INPUT_ROOT" ] ||
       { [ -e "$GC_APPVEYOR_INPUT_ROOT" ] &&
         [ ! -d "$GC_APPVEYOR_INPUT_ROOT" ]; }; then
        echo "Unsafe AppVeyor build-input root." >&2
        return 1
    fi
}

prepare_appveyor_build_inputs()
{
    validate_appveyor_build_input_root || return
    if [ -d "$GC_APPVEYOR_INPUT_ROOT" ]; then
        [ "$(stat -c '%u' "$GC_APPVEYOR_INPUT_ROOT")" = "$(id -u)" ] || {
            echo "AppVeyor build-input root has an unexpected owner." >&2
            return 1
        }
        rm -rf -- "$GC_APPVEYOR_INPUT_ROOT"
    fi
    (umask 077; mkdir -p -- \
        "$GC_D2XX_ROOT" "$GC_SRMIO_ROOT" "$GC_PYTHON_SOURCE_ROOT")
}
