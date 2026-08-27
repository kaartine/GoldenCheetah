#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
DEFAULT_IMAGE=$SCRIPT_DIR/GoldenCheetah-latest.AppImage

if [ "$#" -gt 0 ] && [[ "$1" == *.AppImage ]]; then
    IMAGE=$1
    shift
else
    IMAGE=$DEFAULT_IMAGE
fi

[ -f "$IMAGE" ] && [ -x "$IMAGE" ] || {
    echo "AppImage is missing or not executable: $IMAGE" >&2
    echo "Usage: $0 [APPIMAGE] [GOLDENCHEETAH_ARGUMENTS...]" >&2
    exit 2
}
IMAGE=$(cd -- "$(dirname -- "$IMAGE")" && pwd -P)/$(basename -- "$IMAGE")

export GC_WORKOUT_GAME_3D=1
export GC_WORKOUT_GAME_FEATURE_LAB=1
export GC_WORKOUT_GAME_DIAGNOSTICS=${GC_WORKOUT_GAME_DIAGNOSTICS:-1}

exec "$IMAGE" "$@"
