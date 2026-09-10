#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository="$(cd -- "$script_dir/../.." && pwd)"
gallery_script="$script_dir/blender/workout_game_asset_gallery.py"
image="goldencheetah-workout-game-blender:ubuntu24.04"

if [[ -z "${DISPLAY:-}" ]]; then
    runtime_directory="/run/user/$(id -u)"
    detected_authority=""
    if [[ -d "$runtime_directory" ]]; then
        detected_authority="$(find "$runtime_directory" -maxdepth 1 \
            -type f -name '.mutter-Xwaylandauth.*' -print -quit 2>/dev/null)"
    fi
    if [[ -S /tmp/.X11-unix/X0 && -n "$detected_authority" ]]; then
        export DISPLAY=:0
        export XAUTHORITY="$detected_authority"
    fi
fi

if command -v blender >/dev/null 2>&1; then
    exec blender --python "$gallery_script" -- \
        --root "$repository" "$@"
fi

if [[ -z "${DISPLAY:-}" ]]; then
    printf 'DISPLAY is not set; the Blender gallery needs a graphical session.\n' >&2
    exit 1
fi
if ! docker image inspect "$image" >/dev/null 2>&1; then
    printf 'Missing Docker image %s. Build it with:\n' "$image" >&2
    printf '  docker build -t %s %s\n' "$image" "$script_dir/blender" >&2
    exit 1
fi

docker_args=(
    run --rm
    --network=none
    --memory=2g --memory-swap=2500m --cpus=2
    --user "$(id -u):$(id -g)"
    --env "DISPLAY=$DISPLAY"
    --env HOME=/tmp
    --volume /tmp/.X11-unix:/tmp/.X11-unix:rw
    --volume "$repository:/work:ro"
    --workdir /work
)

xauthority="${XAUTHORITY:-$HOME/.Xauthority}"
if [[ -r "$xauthority" ]]; then
    docker_args+=(
        --env XAUTHORITY=/tmp/.Xauthority
        --volume "$xauthority:/tmp/.Xauthority:ro"
    )
fi
if [[ -e /dev/dri/renderD128 ]]; then
    docker_args+=(
        --device /dev/dri:/dev/dri
        --group-add "$(stat -c '%g' /dev/dri/renderD128)"
    )
fi

exec docker "${docker_args[@]}" \
    --entrypoint /bin/bash "$image" \
    /work/contrib/workout-game-assets/blender/run_gallery_container.sh "$@"
