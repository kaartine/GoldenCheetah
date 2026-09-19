#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repository="$(cd -- "$script_dir/../.." && pwd -P)"
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
    exec blender --no-window-focus --python "$gallery_script" -- \
        --root "$repository" "$@"
fi

forwarded_args=()
candidate_directory=""
evidence_directory=""
edit_mode=false
while (($#)); do
    case "$1" in
        --candidate-directory)
            if (($# < 2)); then
                printf '%s requires a directory argument.\n' "$1" >&2
                exit 2
            fi
            candidate_directory="$2"
            shift 2
            ;;
        --evidence-directory)
            if (($# < 2)); then
                printf '%s requires a directory argument.\n' "$1" >&2
                exit 2
            fi
            evidence_directory="$2"
            shift 2
            ;;
        --evidence-directory=*)
            evidence_directory="${1#*=}"
            shift
            ;;
        --candidate-directory=*)
            candidate_directory="${1#*=}"
            shift
            ;;
        --edit)
            edit_mode=true
            forwarded_args+=(--edit)
            shift
            ;;
        *)
            forwarded_args+=("$1")
            shift
            ;;
    esac
done

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

if [[ "$edit_mode" == true ]]; then
    manifest_directory="$repository/contrib/workout-game-assets/manifests"
    if [[ ! -d "$manifest_directory" ]]; then
        printf 'Manifest directory is unavailable: %s\n' "$manifest_directory" >&2
        exit 2
    fi
    canonical_manifest_directory="$(realpath -e -- "$manifest_directory")"
    if [[ "$canonical_manifest_directory" != "$manifest_directory" ]]; then
        printf 'Manifest directory must not contain symlinks: %s\n' \
            "$manifest_directory" >&2
        exit 2
    fi
    docker_args+=(
        --volume "$manifest_directory:/work/contrib/workout-game-assets/manifests:rw"
    )
fi

if [[ -n "$candidate_directory" ]]; then
    candidate_directory="$(realpath -e -- "$candidate_directory")"
    if [[ ! -d "$candidate_directory" ]]; then
        printf 'Candidate path is not a directory: %s\n' "$candidate_directory" >&2
        exit 2
    fi
    case "$candidate_directory/" in
        "$repository/"|"$repository/"*)
            printf 'Candidate directory must remain outside the repository.\n' >&2
            exit 2
            ;;
    esac
    docker_args+=(--volume "$candidate_directory:/candidate:ro")
    forwarded_args+=(--candidate-directory /candidate)
fi

if [[ -n "$evidence_directory" ]]; then
    evidence_directory="$(realpath -e -- "$evidence_directory")"
    if [[ ! -d "$evidence_directory" ]]; then
        printf 'Evidence path is not a directory: %s\n' "$evidence_directory" >&2
        exit 2
    fi
    case "$evidence_directory/" in
        "$repository/"|"$repository/"*)
            printf 'Evidence directory must remain outside the repository.\n' >&2
            exit 2
            ;;
    esac
    docker_args+=(--volume "$evidence_directory:/evidence:rw")
    forwarded_args+=(--evidence-directory /evidence)
fi

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
    /work/contrib/workout-game-assets/blender/run_gallery_container.sh \
    "${forwarded_args[@]}"
