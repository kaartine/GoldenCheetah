#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repository="$(cd -- "$script_dir/../.." && pwd -P)"
gallery_script="$script_dir/blender/workout_game_asset_gallery.py"
image="goldencheetah-workout-game-blender:ubuntu24.04"
minimum_opengl_major=4
minimum_opengl_minor=3

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

opengl_version_supported() {
    local version="$1"
    if [[ ! "$version" =~ ^([0-9]+)\.([0-9]+) ]]; then
        return 1
    fi
    local major="${BASH_REMATCH[1]}"
    local minor="${BASH_REMATCH[2]}"
    ((major > minimum_opengl_major ||
        (major == minimum_opengl_major && minor >= minimum_opengl_minor)))
}

probe_opengl_version() {
    local prime="${1:-}"
    local output
    if [[ -n "$prime" ]]; then
        output="$(DRI_PRIME="$prime" glxinfo -B 2>/dev/null)" || return 1
    else
        output="$(env -u DRI_PRIME glxinfo -B 2>/dev/null)" || return 1
    fi
    sed -n \
        's/^[[:space:]]*Max core profile version: \([0-9][0-9.]*\).*/\1/p' \
        <<<"$output" | head -n 1
}

gallery_renderer="${WG_GALLERY_RENDERER:-auto}"
gallery_renderer_detail="hardware (OpenGL probe unavailable)"
case "$gallery_renderer" in
    auto)
        if command -v glxinfo >/dev/null 2>&1 && [[ -n "${DISPLAY:-}" ]]; then
            default_opengl="$(probe_opengl_version || true)"
            if opengl_version_supported "$default_opengl"; then
                gallery_renderer_detail="hardware (OpenGL $default_opengl)"
            else
                prime_opengl="$(probe_opengl_version 1 || true)"
                if opengl_version_supported "$prime_opengl"; then
                    export DRI_PRIME=1
                    gallery_renderer_detail="DRI_PRIME=1 (OpenGL $prime_opengl)"
                else
                    export LIBGL_ALWAYS_SOFTWARE=1
                    export GALLIUM_DRIVER=llvmpipe
                    gallery_renderer_detail="llvmpipe (hardware OpenGL below 4.3)"
                fi
            fi
        fi
        ;;
    hardware)
        gallery_renderer_detail="hardware (forced)"
        ;;
    software)
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
        gallery_renderer_detail="llvmpipe (forced)"
        ;;
    *)
        printf 'Unsupported WG_GALLERY_RENDERER value: %s\n' "$gallery_renderer" >&2
        printf 'Expected auto, hardware or software.\n' >&2
        exit 2
        ;;
esac
printf 'Workout Game gallery renderer: %s\n' "$gallery_renderer_detail" >&2

if command -v blender >/dev/null 2>&1; then
    native_config="${BLENDER_USER_CONFIG:-/tmp/gc-blender-config}"
    export BLENDER_USER_CONFIG="$native_config"
    mkdir -p "$native_config"
    if [[ ! -s "$native_config/userpref.blend" ]]; then
        blender --background --factory-startup --python-expr \
            'import bpy; bpy.context.preferences.view.show_splash = False; bpy.ops.wm.save_userpref()'
    fi
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

for environment_name in DRI_PRIME LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER; do
    if [[ -n "${!environment_name:-}" ]]; then
        docker_args+=(--env "$environment_name=${!environment_name}")
    fi
done

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
if [[ -d /dev/dri && -z "${LIBGL_ALWAYS_SOFTWARE:-}" ]]; then
    docker_args+=(
        --device /dev/dri:/dev/dri
    )
    render_group_ids=()
    for render_device in /dev/dri/renderD*; do
        [[ -e "$render_device" ]] || continue
        render_group_id="$(stat -c '%g' "$render_device")"
        if [[ " ${render_group_ids[*]:-} " != *" $render_group_id "* ]]; then
            docker_args+=(--group-add "$render_group_id")
            render_group_ids+=("$render_group_id")
        fi
    done
fi

exec docker "${docker_args[@]}" \
    --entrypoint /bin/bash "$image" \
    /work/contrib/workout-game-assets/blender/run_gallery_container.sh \
    "${forwarded_args[@]}"
