#!/usr/bin/env bash

GC_APPIMAGE_PACKAGING_SUPPORT_DIR=$(
    cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P
) || return
GC_EMBEDDED_PYTHON_NORMALIZER="${GC_APPIMAGE_PACKAGING_SUPPORT_DIR}/normalize-embedded-python.py"
GC_APPIMAGE_OFFSET_READER="${GC_APPIMAGE_PACKAGING_SUPPORT_DIR}/read-appimage-offset.py"
GC_APPIMAGE_PAYLOAD_VERIFIER="${GC_APPIMAGE_PACKAGING_SUPPORT_DIR}/verify-appimage-payload.py"
GC_BUILD_INPUT_IDENTITY_TOOL="${GC_APPIMAGE_PACKAGING_SUPPORT_DIR}/compute-build-input-identity.py"

PYTHON_APPIMAGE_SERIES="3.11"
PYTHON_APPIMAGE_VERSION="3.11.15"
PYTHON_APPIMAGE_ABI="cp311"
PYTHON_APPIMAGE_PLATFORM="manylinux2014_x86_64"
PYTHON_APPIMAGE_SHA256="2d8ecd8002fae06813d4c92ba5244f573aae9bf84eaf41a1b189b623112e3dec"
PYTHON_APPIMAGE_FILE="python${PYTHON_APPIMAGE_VERSION}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_PLATFORM}.AppImage"
PYTHON_APPIMAGE_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${PYTHON_APPIMAGE_FILE}"

LINUXDEPLOYQT_FILE="linuxdeployqt-build107-20251021-x86_64.AppImage"
LINUXDEPLOYQT_SHA256="974a87457ed26241b793bed7841978fcdf84158d13220e53833a06515f173b0b"
LINUXDEPLOYQT_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${LINUXDEPLOYQT_FILE}"
APPIMAGETOOL_FILE="appimagetool-8c8c91f-build295-x86_64.AppImage"
APPIMAGETOOL_SHA256="a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0"
APPIMAGETOOL_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${APPIMAGETOOL_FILE}"
APPIMAGE_RUNTIME_FILE="runtime-2fca8b44-x86_64"
APPIMAGE_RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d"
APPIMAGE_RUNTIME_URL="https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64"
SRMIO_REVISION="b444b8747317c41607d468ae71a0ecd36a94332e"
SRMIO_SOURCE_FILE="srmio-${SRMIO_REVISION}.tar.gz"
SRMIO_SOURCE_SHA256="16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186"
SRMIO_SOURCE_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${SRMIO_SOURCE_FILE}"
D2XX_LINUX_VERSION="1.4.33"
D2XX_LINUX_SHA256="e260a4594a313583b87bf230c79cec9d46f11db6dcfd7c7d4f963279703214d3"
D2XX_LINUX_SOURCE_URL="https://distfiles.gentoo.org/distfiles/b1/libftd2xx-x86_64-${D2XX_LINUX_VERSION}.tar.gz"
QTKEYCHAIN_LICENSE_SHA256="ca46b73d5159548ab52834db51f195aa3d1f277f020e9dca92f4beb21b468a50"
LGPL21_LICENSE_SHA256="dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"
BOX2D_LICENSE_SHA256="68a3e676d7e94093b102d5cba0d4e04af812040d6f230c3db67a6664574e43d2"

download_file()
{
    local url=$1
    local destination=$2

    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --silent --show-error \
            --output "$destination" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget --no-verbose --output-document="$destination" "$url"
    else
        echo "Neither curl nor wget is available" >&2
        return 1
    fi
}

verify_sha256()
{
    local path=$1
    local expected=$2

    [ -f "$path" ] && [ ! -L "$path" ] || return 1
    [[ "$expected" =~ ^[0-9a-f]{64}$ ]] || return 1
    printf '%s  %s\n' "$expected" "$path" | sha256sum --check --status -
}

download_verified_file()
{
    local url=$1
    local destination=$2
    local expected=$3
    local temporary=

    if verify_sha256 "$destination" "$expected"; then
        return 0
    fi
    rm -f -- "$destination"
    temporary=$(mktemp "${destination}.tmp.XXXXXX") || return
    if ! download_file "$url" "$temporary" ||
       ! verify_sha256 "$temporary" "$expected"; then
        rm -f -- "$temporary"
        return 1
    fi
    mv -- "$temporary" "$destination"
}

download_linuxdeployqt()
{
    download_verified_file "$LINUXDEPLOYQT_URL" "$LINUXDEPLOYQT_FILE" "$LINUXDEPLOYQT_SHA256"
}

download_appimagetool()
{
    download_verified_file "$APPIMAGETOOL_URL" "$APPIMAGETOOL_FILE" "$APPIMAGETOOL_SHA256"
}

download_appimage_runtime()
{
    download_verified_file \
        "$APPIMAGE_RUNTIME_URL" "$APPIMAGE_RUNTIME_FILE" \
        "$APPIMAGE_RUNTIME_SHA256"
}

run_reproducible_git()
(
    local git_path
    git_path=$(command -v git) || {
        echo "git is required for reproducible release builds." >&2
        return 1
    }
    env -i \
        PATH=/usr/local/bin:/usr/bin:/bin \
        HOME=/nonexistent \
        LC_ALL=C LANG=C TZ=UTC \
        GIT_CONFIG_NOSYSTEM=1 \
        GIT_CONFIG_GLOBAL=/dev/null \
        "$git_path" "$@"
)

install_reproducible_build_inputs()
{
    if [ "$#" -ne 2 ]; then
        echo "Usage: install_reproducible_build_inputs INPUT_SOURCE SOURCE_TREE" >&2
        return 2
    fi
    local input_source=$1
    local source_tree=$2
    local input_config="$input_source/src/gcconfig.pri"
    local input_secrets="$input_source/src/Core/GeneratedSecrets.h"
    local input_qwt_config="$input_source/qwt/qwtconfig.pri"
    local default_qwt_config="$source_tree/qwt/qwtconfig.pri.in"
    local output_config="$source_tree/src/gcconfig.pri"
    local output_secrets="$source_tree/src/Core/GeneratedSecrets.h"
    local output_qwt_config="$source_tree/qwt/qwtconfig.pri"
    local output

    [ -d "$input_source" ] && [ ! -L "$input_source" ] &&
        [ -d "$source_tree" ] && [ ! -L "$source_tree" ] &&
        [ -d "$source_tree/src" ] && [ ! -L "$source_tree/src" ] &&
        [ -d "$source_tree/src/Core" ] && [ ! -L "$source_tree/src/Core" ] &&
        [ -d "$source_tree/qwt" ] && [ ! -L "$source_tree/qwt" ] || {
        echo "Reproducible build input directories are unsafe." >&2
        return 1
    }
    for output in "$output_config" "$output_secrets" "$output_qwt_config"; do
        if [ -L "$output" ] ||
           { [ -e "$output" ] && [ ! -f "$output" ]; }; then
            echo "Reproducible build input destination is unsafe: $output" >&2
            return 1
        fi
    done
    [ -f "$input_config" ] && [ ! -L "$input_config" ] || {
        echo "Effective gcconfig.pri is required for reproducible builds." >&2
        return 1
    }
    install -m 0644 -- "$input_config" "$output_config" ||
        return

    if [ -f "$input_secrets" ] && [ ! -L "$input_secrets" ]; then
        install -m 0600 -- "$input_secrets" "$output_secrets" || return
    elif [ -e "$input_secrets" ] || [ -L "$input_secrets" ]; then
        echo "GeneratedSecrets.h is not a regular input file." >&2
        return 1
    elif [ -e "$output_secrets" ] || [ -L "$output_secrets" ]; then
        echo "Unexpected GeneratedSecrets.h exists in the source tree." >&2
        return 1
    fi

    if [ -f "$input_qwt_config" ] && [ ! -L "$input_qwt_config" ]; then
        install -m 0644 -- "$input_qwt_config" \
            "$output_qwt_config"
    elif [ -e "$input_qwt_config" ] || [ -L "$input_qwt_config" ]; then
        echo "qwtconfig.pri is not a regular input file." >&2
        return 1
    elif [ -f "$default_qwt_config" ] && [ ! -L "$default_qwt_config" ]; then
        install -m 0644 -- "$default_qwt_config" \
            "$output_qwt_config"
    else
        echo "Default qwtconfig.pri input is unavailable." >&2
        return 1
    fi
}

run_reproducible_build_tool()
(
    if [ "$#" -lt 4 ]; then
        echo "Usage: run_reproducible_build_tool QT_ROOT HOME TMPDIR COMMAND..." >&2
        return 2
    fi
    local qt_root=$1
    local build_home=$2
    local build_tmp=$3
    shift 3
    local tool_path revision epoch

    [ -d "$qt_root" ] && [ ! -L "$qt_root" ] &&
        [ -d "$build_home" ] && [ ! -L "$build_home" ] &&
        [ -d "$build_tmp" ] && [ ! -L "$build_tmp" ] || {
        echo "Reproducible build environment directories are unsafe." >&2
        return 1
    }
    qt_root=$(cd -- "$qt_root" && pwd -P) || return
    build_home=$(cd -- "$build_home" && pwd -P) || return
    build_tmp=$(cd -- "$build_tmp" && pwd -P) || return
    revision=${GC_SOURCE_REVISION:-}
    epoch=${SOURCE_DATE_EPOCH:-}
    [[ "$revision" =~ ^[0-9a-f]{40}$ ]] &&
        [[ "$epoch" =~ ^[1-9][0-9]*$ ]] || {
        echo "Authenticated source revision and epoch are required." >&2
        return 1
    }
    tool_path="$qt_root/bin:/usr/local/bin:/usr/bin:/bin"
    env -i \
        PATH="$tool_path" \
        HOME="$build_home" \
        TMPDIR="$build_tmp" \
        QTDIR="$qt_root" \
        LD_LIBRARY_PATH="$qt_root/lib" \
        LC_ALL=C LANG=C TZ=UTC \
        SOURCE_DATE_EPOCH="$epoch" \
        GC_SOURCE_REVISION="$revision" \
        ZERO_AR_DATE=1 \
        "$@"
)

run_packaging_appimage()
(
    local packaging_home tool_path
    packaging_home=$(mktemp -d) || return
    trap 'rm -rf -- "$packaging_home"' EXIT
    tool_path=/usr/local/bin:/usr/bin:/bin
    if [ -n "${QTDIR:-}" ]; then
        tool_path="$QTDIR/bin:$tool_path"
    fi
    local -a environment=(
        env -i
        "PATH=$tool_path"
        "HOME=$packaging_home"
        "ARCH=x86_64"
        "LC_ALL=C"
        "LANG=C"
        "TZ=UTC"
        "APPIMAGE_EXTRACT_AND_RUN=1"
    )
    if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
        [[ "$SOURCE_DATE_EPOCH" =~ ^[1-9][0-9]*$ ]] || {
            echo "Invalid SOURCE_DATE_EPOCH for AppImage tooling." >&2
            return 1
        }
        environment+=("SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH")
    fi
    if [ -n "${QTDIR:-}" ]; then
        environment+=("QTDIR=$QTDIR")
    fi
    "${environment[@]}" "$@"
)

run_packaged_appimage_smoke()
(
    local tool_path=/usr/local/bin:/usr/bin:/bin
    local -a environment=(
        env -i
        "PATH=$tool_path"
        "HOME=${HOME:-/nonexistent}"
        "LC_ALL=C"
        "LANG=C"
        "TZ=UTC"
        "APPIMAGE_EXTRACT_AND_RUN=1"
    )
    local variable
    for variable in \
        XDG_CONFIG_HOME XDG_CACHE_HOME XDG_RUNTIME_DIR \
        DISPLAY WAYLAND_DISPLAY QT_QPA_PLATFORM QT_OPENGL \
        LIBGL_ALWAYS_SOFTWARE QTWEBENGINE_DISABLE_SANDBOX; do
        if [ -n "${!variable+x}" ]; then
            environment+=("$variable=${!variable}")
        fi
    done
    "${environment[@]}" timeout "$@"
)

trusted_appimage_extract()
(
    if [ "$#" -ne 2 ]; then
        echo "Usage: trusted_appimage_extract APPIMAGE EMPTY_DIRECTORY" >&2
        return 2
    fi
    local image=$1
    local destination=$2
    local offset before after output snapshot_dir snapshot
    local source_hash snapshot_hash extracted_hash

    cleanup_trusted_snapshot()
    {
        if [ -n "${snapshot_dir:-}" ]; then
            rm -rf -- "$snapshot_dir"
        fi
    }
    trap cleanup_trusted_snapshot EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    [ -f "$GC_APPIMAGE_OFFSET_READER" ] &&
        [ ! -L "$GC_APPIMAGE_OFFSET_READER" ] || {
        echo "Trusted AppImage offset reader is unavailable." >&2
        return 1
    }
    command -v python3 >/dev/null 2>&1 &&
        command -v unsquashfs >/dev/null 2>&1 || {
        echo "python3 and unsquashfs are required for trusted AppImage extraction." >&2
        return 1
    }
    [ -f "$image" ] && [ ! -L "$image" ] && [ -r "$image" ] || {
        echo "Refusing to extract an unsafe AppImage." >&2
        return 1
    }
    [ -d "$destination" ] && [ ! -L "$destination" ] &&
        [ -z "$(find -P "$destination" -mindepth 1 -print -quit)" ] || {
        echo "Trusted AppImage extraction requires an empty real directory." >&2
        return 1
    }
    before=$(stat -Lc '%d:%i:%s:%f' -- "$image") || return
    source_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    [[ "$source_hash" =~ ^[0-9a-f]{64}$ ]] || return 1
    snapshot_dir=$(mktemp -d) || return
    snapshot="$snapshot_dir/candidate.AppImage"
    install -m 0600 -- "$image" "$snapshot" || return
    snapshot_hash=$(sha256sum "$snapshot" | cut -d ' ' -f 1) || return
    after=$(stat -Lc '%d:%i:%s:%f' -- "$image") || return
    [ "$before" = "$after" ] && [ "$source_hash" = "$snapshot_hash" ] || {
        echo "AppImage changed while creating a trusted snapshot." >&2
        return 1
    }
    offset=$(python3 "$GC_APPIMAGE_OFFSET_READER" "$snapshot") || return
    [[ "$offset" =~ ^[1-9][0-9]*$ ]] || return 1
    output="$destination/squashfs-root"
    if ! unsquashfs -no-progress -processors 1 -o "$offset" \
        -d "$output" "$snapshot" >/dev/null; then
        rm -rf -- "$output"
        return 1
    fi
    if ! extracted_hash=$(sha256sum "$snapshot" | cut -d ' ' -f 1); then
        rm -rf -- "$output"
        return 1
    fi
    if [ "$snapshot_hash" != "$extracted_hash" ] ||
       [ ! -d "$output" ] || [ -L "$output" ]; then
        rm -rf -- "$output"
        echo "Trusted AppImage snapshot changed during extraction." >&2
        return 1
    fi
)

install_qt_offscreen_plugin()
{
    local qmake_command=$1
    local app_dir=$2
    local qmake_path qt_plugins_dir source_plugin
    local plugins_dir target_dir target_plugin

    qmake_path=$(command -v -- "$qmake_command") || {
        echo "Qt qmake executable not found: $qmake_command" >&2
        return 1
    }
    if [ ! -x "$qmake_path" ]; then
        echo "Qt qmake executable is not runnable: $qmake_path" >&2
        return 1
    fi
    qt_plugins_dir=$(
        "$qmake_path" -query QT_INSTALL_PLUGINS
    ) || {
        echo "Cannot resolve the Qt plugin directory." >&2
        return 1
    }
    case "$qt_plugins_dir" in
    ""|*'
'*)
        echo "Qt returned an invalid plugin directory." >&2
        return 1
        ;;
    esac

    source_plugin="$qt_plugins_dir/platforms/libqoffscreen.so"
    if [ ! -f "$source_plugin" ] || [ ! -r "$source_plugin" ]; then
        echo "Qt offscreen platform plugin not found: $source_plugin" >&2
        return 1
    fi
    if [ ! -d "$app_dir" ] || [ -L "$app_dir" ]; then
        echo "Invalid AppDir for the Qt offscreen plugin: $app_dir" >&2
        return 1
    fi

    plugins_dir="$app_dir/plugins"
    target_dir="$plugins_dir/platforms"
    if [ -L "$plugins_dir" ] || [ -L "$target_dir" ]; then
        echo "Refusing a linked AppDir platform plugin directory." >&2
        return 1
    fi
    mkdir -p -- "$target_dir" || return
    target_plugin="$target_dir/libqoffscreen.so"
    install -m 0755 -- "$source_plugin" "$target_plugin" || return
    if [ ! -f "$target_plugin" ] ||
       ! cmp -s -- "$source_plugin" "$target_plugin"; then
        echo "Qt offscreen platform plugin was not installed intact." >&2
        return 1
    fi
    echo "Qt offscreen plugin: bundled"
}

require_qt_offscreen_appimage()
(
    local image=$1
    local smoke_duration=${2:-10s}
    local ready_marker=goldencheetah_gui_smoke=main-window-ready
    local smoke_home= smoke_log= smoke_library= smoke_athlete=

    cleanup_offscreen_smoke()
    {
        if [ -n "$smoke_home" ]; then
            rm -rf -- "$smoke_home"
            smoke_home=
        fi
        if [ -n "$smoke_log" ]; then
            rm -f -- "$smoke_log"
            smoke_log=
        fi
    }
    trap cleanup_offscreen_smoke EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot smoke-test Qt offscreen runtime in $image" >&2
        return 1
    fi
    smoke_home=$(mktemp -d) || return
    smoke_log=$(mktemp) || return
    mkdir -p "$smoke_home/.config" "$smoke_home/.cache" || return
    smoke_library="$smoke_home/library"
    smoke_athlete="$smoke_library/SmokeAthlete"
    mkdir -p \
        "$smoke_athlete/activities" \
        "$smoke_athlete/tempActivities" \
        "$smoke_athlete/imports" \
        "$smoke_athlete/records" \
        "$smoke_athlete/downloads" \
        "$smoke_athlete/bak" \
        "$smoke_athlete/config" \
        "$smoke_athlete/cache" \
        "$smoke_athlete/calendar" \
        "$smoke_athlete/workouts" \
        "$smoke_athlete/logs" \
        "$smoke_athlete/temp" \
        "$smoke_athlete/quarantine" \
        "$smoke_athlete/planned" \
        "$smoke_athlete/snippets" \
        "$smoke_athlete/media" || return
    printf '%s\n' \
        '[General]' \
        'id={00000000-0000-4000-8000-000000000001}' \
        >"$smoke_athlete/config/athlete-general.ini" || return
    install -m 0600 /dev/null \
        "$smoke_athlete/config/athlete-layout.ini" || return
    install -m 0600 /dev/null \
        "$smoke_athlete/config/athlete-preferences.ini" || return
    install -m 0600 /dev/null \
        "$smoke_athlete/config/athlete-private.ini" || return

    local status
    if HOME="$smoke_home" \
       XDG_CONFIG_HOME="$smoke_home/.config" \
       XDG_CACHE_HOME="$smoke_home/.cache" \
       DISPLAY= WAYLAND_DISPLAY= \
       QT_QPA_PLATFORM=offscreen \
       QT_OPENGL=software \
       LIBGL_ALWAYS_SOFTWARE=1 \
       QTWEBENGINE_DISABLE_SANDBOX=1 \
       run_packaged_appimage_smoke \
           --kill-after=2s "$smoke_duration" "$image" \
           --goldencheetah-gui-smoke "$smoke_library" SmokeAthlete \
           >"$smoke_log" 2>&1; then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        cat "$smoke_log" >&2
        echo "AppImage Qt offscreen smoke failed with status $status" >&2
        return 1
    fi
    if ! grep -Fxq -- "$ready_marker" "$smoke_log"; then
        cat "$smoke_log" >&2
        echo "AppImage Qt offscreen smoke did not reach the event loop" >&2
        return 1
    fi
    echo "Qt offscreen runtime: available"
)

require_qt_offscreen_appimage_on_glibc()
{
    local expected_glibc=$1
    local image=$2
    local smoke_duration=${3:-10s}
    local host_glibc status

    host_glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null) || {
        echo "Cannot determine the host glibc version." >&2
        return 1
    }
    host_glibc=${host_glibc#glibc }
    if [ "$host_glibc" != "$expected_glibc" ]; then
        echo "AppImage compatibility smoke requires glibc" \
            "$expected_glibc; host provides $host_glibc." >&2
        return 1
    fi
    status=$(require_qt_offscreen_appimage "$image" "$smoke_duration") || return
    printf '%s on glibc %s\n' "$status" "$host_glibc"
}

strava_oauth_build_fallback_status()
(
    local executable=$1
    local allow_script_entrypoint=${2:-false}
    local status_home=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    cleanup_status_home()
    {
        if [ -n "$status_home" ]; then
            rm -rf -- "$status_home"
            status_home=
        fi
    }
    trap cleanup_status_home EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot inspect Strava OAuth configuration in $executable" >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$allow_script_entrypoint" != "true" ] ||
           [ "$script_magic" != "2321" ]; then
            echo "Strava OAuth build status requires an ELF executable." >&2
            return 1
        fi
    fi
    if [ "$elf_magic" = "7f454c46" ]; then
        local appimage_magic
        appimage_magic=$(dd if="$executable" bs=1 skip=8 count=3 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$appimage_magic" = "414901" ] ||
           [ "$appimage_magic" = "414902" ]; then
            echo "Cannot inspect Strava OAuth configuration in a compressed AppImage;" \
                "inspect its extracted GoldenCheetah executable." >&2
            return 1
        fi
    fi

    local report captured status_file command_status
    status_home=$(mktemp -d) || return
    if ! mkdir -p "$status_home/.config"; then
        return 1
    fi
    status_file="$status_home/build-status"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s \
                "$executable" --goldencheetah-build-status
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    captured=$(
        cat "$status_file" 2>/dev/null
        printf x
    )
    report=${captured%x}
    if [ "$command_status" -ne 0 ]; then
        echo "GoldenCheetah build-status command failed." >&2
        return 1
    fi
    local fallback_report runtime_only_report
    fallback_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=runtime_credentials" \
            "strava_compile_fallback=configured"
        printf x
    )
    fallback_report=${fallback_report%x}
    runtime_only_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=runtime_credentials" \
            "strava_compile_fallback=unavailable"
        printf x
    )
    runtime_only_report=${runtime_only_report%x}
    if [ "$report" = "$fallback_report" ]; then
        echo "configured"
        return
    fi
    if [ "$report" = "$runtime_only_report" ]; then
        echo "unavailable"
        return
    fi
    echo "GoldenCheetah returned an invalid build-status report." >&2
    return 1
)

strava_oauth_build_status()
{
    local fallback_status

    fallback_status=$(strava_oauth_build_fallback_status "$@") || return
    if [ "$fallback_status" != "configured" ] &&
       [ "$fallback_status" != "unavailable" ]; then
        echo "GoldenCheetah returned an invalid Strava fallback status." >&2
        return 1
    fi
    echo "Strava OAuth: runtime credentials supported"
}

require_strava_oauth_build()
{
    local executable=$1
    local status

    status=$(strava_oauth_build_status "$executable") || return
    if [ "$status" != "Strava OAuth: runtime credentials supported" ]; then
        echo "$status" >&2
        echo "Refusing to package a release without runtime" \
            "Strava OAuth credential support." >&2
        return 1
    fi
    echo "$status"
}

require_unconfigured_strava_oauth_build()
{
    local executable=$1
    local status

    status=$(strava_oauth_build_fallback_status "$executable") || return
    if [ "$status" != "unavailable" ]; then
        echo "$status" >&2
        echo "Refusing to publish a credential-free diagnostic artifact" \
            "with a compile-time Strava OAuth fallback." >&2
        return 1
    fi
    echo "Strava OAuth compile-time fallback: unavailable"
}

strava_oauth_appimage_status()
(
    local image=$1
    local status_kind=${2:-support}
    local extract_dir=
    local original_working_dir
    local APPDIR= APPIMAGE= OWD=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    original_working_dir=$(pwd -P) || return
    if [ "$status_kind" != "support" ] &&
       [ "$status_kind" != "fallback" ]; then
        echo "Unknown Strava OAuth AppImage status kind." >&2
        return 2
    fi

    cleanup_extract_dir()
    {
        if [ -n "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
            extract_dir=
        fi
    }
    trap cleanup_extract_dir EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot inspect Strava OAuth configuration in $image" >&2
        return 1
    fi
    local appimage_magic
    appimage_magic=$(dd if="$image" bs=1 skip=8 count=3 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$appimage_magic" != "414902" ]; then
        echo "Strava OAuth package status requires a Type 2 AppImage." >&2
        return 1
    fi

    local image_dir image_name image_path app_root entrypoint
    image_dir=$(cd -- "$(dirname -- "$image")" && pwd) || return
    image_name=$(basename -- "$image") || return
    image_path="$image_dir/$image_name"
    extract_dir=$(mktemp -d) || return
    app_root="$extract_dir/squashfs-root"

    trusted_appimage_extract "$image_path" "$extract_dir" || return

    entrypoint=$(readlink -f "$app_root/AppRun") || return
    case "$entrypoint" in
    "$app_root"/*) ;;
    *)
        echo "AppImage AppRun resolves outside its payload." >&2
        return 1
        ;;
    esac
    if [ "$status_kind" = "support" ]; then
        APPDIR="$app_root" \
            APPIMAGE="$image_path" \
            OWD="$original_working_dir" \
            strava_oauth_build_status "$app_root/AppRun" true
    else
        APPDIR="$app_root" \
            APPIMAGE="$image_path" \
            OWD="$original_working_dir" \
            strava_oauth_build_fallback_status "$app_root/AppRun" true
    fi
)

require_strava_oauth_appimage()
{
    local image=$1
    local status

    status=$(strava_oauth_appimage_status "$image") || return
    if [ "$status" != "Strava OAuth: runtime credentials supported" ]; then
        echo "$status" >&2
        echo "Refusing to publish an AppImage without runtime" \
            "Strava OAuth credential support." >&2
        return 1
    fi
    echo "$status"
}

require_unconfigured_strava_oauth_appimage()
{
    local image=$1
    local status

    status=$(strava_oauth_appimage_status "$image" fallback) || return
    if [ "$status" != "unavailable" ]; then
        echo "$status" >&2
        echo "Refusing to publish a credential-free diagnostic AppImage" \
            "with a compile-time Strava OAuth fallback." >&2
        return 1
    fi
    echo "Strava OAuth compile-time fallback: unavailable"
}

file_matches_sha256()
{
    local file=$1
    local expected=$2
    local actual

    actual=$(sha256sum -- "$file" 2>/dev/null |
        awk '{ print $1 }') || return
    [ "$actual" = "$expected" ]
}

libsecret_copyright_is_valid()
{
    local copyright=$1

    grep -Fqx -- "Upstream-Name: libsecret" "$copyright" &&
        grep -Eq '^Copyright:[[:space:]]*[^[:space:]]' "$copyright" &&
        grep -Eq '^License: LGPL-2\.1\+' "$copyright" &&
        grep -Fq -- "GNU Lesser General Public" "$copyright" &&
        grep -Fq -- \
            "License as published by the Free Software Foundation" "$copyright"
}

linux_keychain_payload_library_is_valid()
{
    local app_root=$1
    local library=$2
    local resolved

    if [ -L "$library" ] ||
       [ ! -f "$library" ] || [ ! -r "$library" ]; then
        return 1
    fi
    resolved=$(readlink -f -- "$library") || return
    case "$resolved" in
    "$app_root"/lib/*) [ "$resolved" = "$library" ] ;;
    *) return 1 ;;
    esac
}

linux_system_library_path_is_valid()
{
    local library=$1
    local resolved

    case "$library" in
    /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
    *) return 1 ;;
    esac
    resolved=$(readlink -f -- "$library") || return
    case "$resolved" in
    /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*)
        [ -f "$resolved" ] && [ -r "$resolved" ]
        ;;
    *) return 1 ;;
    esac
}

linux_keychain_runtime_status()
{
    local appdir=$1
    local app_root
    app_root=$(cd -- "$appdir" && pwd -P) || {
        echo "Linux keychain AppDir is missing." >&2
        return 1
    }
    local library="$app_root/lib/libsecret-1.so.0"
    local license_dir="$app_root/usr/share/doc/GoldenCheetah/licenses"
    local libsecret_copyright="$license_dir/libsecret-copyright"
    local qtkeychain_license="$license_dir/QtKeychain-COPYING"
    local libsecret_license="$license_dir/LGPL-2.1"
    local dependencies="
libglib-2.0.so.0
libgio-2.0.so.0
libgobject-2.0.so.0
libgcrypt.so.20
libgpg-error.so.0"

    if [ -L "$library" ] ||
       [ ! -f "$library" ] || [ ! -r "$library" ]; then
        echo "Bundled libsecret runtime is missing." >&2
        return 1
    fi
    local payload resolved_payload
    for payload in \
        "$library" \
        "$libsecret_copyright" \
        "$qtkeychain_license" \
        "$libsecret_license"; do
        if [ -L "$payload" ] || [ ! -f "$payload" ]; then
            echo "Linux keychain payload contains a missing or linked file." >&2
            return 1
        fi
        resolved_payload=$(readlink -f -- "$payload") || return
        case "$resolved_payload" in
        "$app_root"/*) ;;
        *)
            echo "Linux keychain payload resolves outside its AppDir." >&2
            return 1
            ;;
        esac
    done
    local dependency
    for dependency in $dependencies; do
        payload="$app_root/lib/$dependency"
        if [ -L "$payload" ] ||
           [ ! -f "$payload" ] || [ ! -r "$payload" ]; then
            echo "Bundled libsecret dependency is missing." >&2
            return 1
        fi
        resolved_payload=$(readlink -f -- "$payload") || return
        case "$resolved_payload" in
        "$app_root"/lib/*) ;;
        *)
            echo "Bundled libsecret dependency escapes the AppDir." >&2
            return 1
            ;;
        esac
    done

    local elf_magic
    elf_magic=$(dd if="$library" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        echo "Bundled libsecret runtime is not an ELF library." >&2
        return 1
    fi
    if ! LC_ALL=C readelf -d "$library" 2>/dev/null |
        grep -Eq '\(SONAME\).*\[libsecret-1\.so\.0\]'; then
        echo "Bundled libsecret runtime has an unexpected SONAME." >&2
        return 1
    fi
    if ! LC_ALL=C readelf -d "$library" 2>/dev/null |
        grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]'; then
        echo "Bundled libsecret runtime has no local dependency path." >&2
        return 1
    fi
    local dynamic_symbols required_symbol
    dynamic_symbols=$(LC_ALL=C readelf --wide --dyn-syms \
        "$library" 2>/dev/null) || {
        echo "Cannot inspect bundled libsecret symbols." >&2
        return 1
    }
    for required_symbol in \
        secret_password_lookup \
        secret_password_lookup_finish \
        secret_password_store \
        secret_password_store_finish \
        secret_password_clear \
        secret_password_clear_finish \
        secret_password_free \
        secret_error_get_quark; do
        if ! printf '%s\n' "$dynamic_symbols" |
            awk -v symbol="$required_symbol" '
                $4 == "FUNC" && $5 == "GLOBAL" &&
                $7 != "UND" && $8 == symbol { found = 1 }
                END { exit !found }
            '; then
            echo "Bundled libsecret runtime is missing a required symbol." >&2
            return 1
        fi
    done
    local ldd_output expected_dependency
    ldd_output=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD \
        LC_ALL=C ldd "$library" 2>/dev/null) || {
        echo "Cannot resolve bundled libsecret dependencies." >&2
        return 1
    }
    for dependency in $dependencies; do
        expected_dependency="$dependency => $app_root/lib/$dependency "
        if ! printf '%s\n' "$ldd_output" |
            grep -Fq -- "$expected_dependency"; then
            echo "A libsecret dependency resolves outside the AppDir." >&2
            return 1
        fi
    done
    local dependency_arrow dependency_path dependency_tail
    while read -r dependency dependency_arrow dependency_path \
        dependency_tail; do
        if [ -z "$dependency" ]; then
            continue
        fi
        if [ "$dependency_arrow" = "=>" ]; then
            if [ "$dependency_path" = "not" ]; then
                echo "A bundled libsecret dependency is unresolved." >&2
                return 1
            fi
            if linux_keychain_payload_library_is_valid \
                "$app_root" "$dependency_path"; then
                continue
            fi
            case "$dependency" in
            libc.so.6|libdl.so.2|libgcc_s.so.1|libm.so.6|\
            libpthread.so.0|libresolv.so.2|librt.so.1|libz.so.1)
                if linux_system_library_path_is_valid \
                    "$dependency_path"; then
                    continue
                fi
                ;;
            esac
            echo "An unexpected dependency resolves outside the AppDir." >&2
            return 1
        fi
        case "$dependency" in
        linux-vdso.so.1|linux-gate.so.1) continue ;;
        /*)
            if linux_keychain_payload_library_is_valid \
                "$app_root" "$dependency"; then
                continue
            fi
            dependency_path=${dependency##*/}
            case "$dependency_path" in
            ld-linux*.so.*)
                if linux_system_library_path_is_valid "$dependency"; then
                    continue
                fi
                ;;
            esac
            ;;
        esac
        echo "An unrecognized libsecret dependency was reported." >&2
        return 1
    done <<<"$ldd_output"
    if ! libsecret_copyright_is_valid \
        "$libsecret_copyright"; then
        echo "Bundled libsecret copyright is invalid." >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$qtkeychain_license" "$QTKEYCHAIN_LICENSE_SHA256"; then
        echo "Bundled QtKeychain license is invalid." >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$libsecret_license" "$LGPL21_LICENSE_SHA256"; then
        echo "Bundled LGPL-2.1 license is invalid." >&2
        return 1
    fi
    echo "Linux keychain runtime: bundled"
}

prepare_contained_appdir_directory()
(
    local appdir=$1
    local relative=$2

    if [ -L "$appdir" ] || [ ! -d "$appdir" ]; then
        echo "Linux keychain AppDir must be a regular directory." >&2
        return 1
    fi
    case "$relative" in
    ""|/*|..|../*|*/..|*/../*)
        echo "Invalid Linux keychain AppDir directory path." >&2
        return 1
        ;;
    esac

    local app_root current component resolved
    app_root=$(cd -- "$appdir" && pwd -P) || return
    current=$app_root
    local IFS=/
    for component in $relative; do
        [ -n "$component" ] || continue
        current="$current/$component"
        if [ -L "$current" ]; then
            echo "Linux keychain AppDir directory is a symlink." >&2
            return 1
        fi
        if [ -e "$current" ]; then
            if [ ! -d "$current" ]; then
                echo "Linux keychain AppDir path is not a directory." >&2
                return 1
            fi
        elif ! mkdir -- "$current"; then
            return 1
        fi
    done
    resolved=$(cd -- "$current" && pwd -P) || return
    if [ "$resolved" != "$app_root/$relative" ]; then
        echo "Linux keychain AppDir directory escapes its root." >&2
        return 1
    fi
    printf '%s\n' "$resolved"
)

install_regular_keychain_payload()
{
    local source=$1
    local destination=$2

    if [ -L "$destination" ] ||
       { [ -e "$destination" ] && [ ! -f "$destination" ]; }; then
        echo "Linux keychain payload destination is not a regular file." >&2
        return 1
    fi
    install -m 0644 "$source" "$destination"
}

install_box2d_license()
{
    if [ "$#" -ne 2 ]; then
        echo "Usage: install_box2d_license APPDIR LICENSE" >&2
        return 2
    fi
    local appdir=$1
    local source=$2
    local license_dir destination

    file_matches_sha256 "$source" "$BOX2D_LICENSE_SHA256" || {
        echo "Box2D license is missing or invalid." >&2
        return 1
    }
    license_dir=$(prepare_contained_appdir_directory \
        "$appdir" usr/share/doc/GoldenCheetah/licenses) || return
    destination="$license_dir/Box2D-LICENSE"
    if [ -L "$destination" ] ||
       { [ -e "$destination" ] && [ ! -f "$destination" ]; }; then
        echo "Box2D license destination is not a regular file." >&2
        return 1
    fi
    install -m 0644 "$source" "$destination" || return
    cmp -s -- "$source" "$destination"
}

install_linux_keychain_runtime()
{
    if [ "$#" -ne 3 ]; then
        echo "Usage: install_linux_keychain_runtime APPDIR QTKEYCHAIN_LICENSE TRANSFORMATION_MANIFEST" >&2
        return 2
    fi
    local appdir=$1
    local qtkeychain_license=$2
    local transformation_manifest=$3
    local libsecret_copyright=${LIBSECRET_COPYRIGHT_FILE:-}
    local libsecret_license=${LIBSECRET_LICENSE_FILE:-}
    local libsecret_libdir library resolved_library
    local gpg_error_libdir gpg_error_library resolved_gpg_error
    local libgcrypt_source=${LIBGCRYPT_RUNTIME_FILE:-}
    local lib_dir libgcrypt license_dir transformation_sources

    if [ -z "$libsecret_copyright" ]; then
        libsecret_copyright=/usr/share/doc/libsecret-1-0/copyright
    fi
    if [ -z "$libsecret_license" ]; then
        libsecret_license=/usr/share/common-licenses/LGPL-2.1
    fi
    libsecret_libdir=$(pkg-config --variable=libdir libsecret-1) || {
        echo "Cannot resolve the libsecret runtime directory." >&2
        return 1
    }
    library="$libsecret_libdir/libsecret-1.so.0"
    resolved_library=$(readlink -f -- "$library") || {
        echo "Cannot resolve the libsecret runtime: $library" >&2
        return 1
    }
    if [ ! -f "$resolved_library" ] || [ ! -r "$resolved_library" ]; then
        echo "libsecret runtime not found: $resolved_library" >&2
        return 1
    fi
    gpg_error_libdir=$(pkg-config --variable=libdir gpg-error) || {
        echo "Cannot resolve the libgpg-error runtime directory." >&2
        return 1
    }
    gpg_error_library="$gpg_error_libdir/libgpg-error.so.0"
    resolved_gpg_error=$(readlink -f -- "$gpg_error_library") || {
        echo "Cannot resolve the libgpg-error runtime:" \
            "$gpg_error_library" >&2
        return 1
    }
    if [ ! -f "$resolved_gpg_error" ] ||
       [ ! -r "$resolved_gpg_error" ]; then
        echo "libgpg-error runtime not found: $resolved_gpg_error" >&2
        return 1
    fi
    if [ -z "$libgcrypt_source" ]; then
        libgcrypt_source=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD LC_ALL=C \
            ldd "$resolved_library" 2>/dev/null | awk '
                $1 == "libgcrypt.so.20" && $2 == "=>" { print $3 }
            ') || return
        if [ "$(printf '%s\n' "$libgcrypt_source" | sed '/^$/d' | wc -l)" \
             -ne 1 ]; then
            echo "Cannot identify the libgcrypt runtime source." >&2
            return 1
        fi
    fi
    libgcrypt_source=$(readlink -f -- "$libgcrypt_source") || {
        echo "Cannot resolve the libgcrypt runtime source." >&2
        return 1
    }
    if [ ! -f "$libgcrypt_source" ] || [ ! -r "$libgcrypt_source" ]; then
        echo "libgcrypt runtime source not found: $libgcrypt_source" >&2
        return 1
    fi
    if ! libsecret_copyright_is_valid \
        "$libsecret_copyright"; then
        echo "libsecret copyright file is invalid: $libsecret_copyright" >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$qtkeychain_license" "$QTKEYCHAIN_LICENSE_SHA256"; then
        echo "QtKeychain license is invalid: $qtkeychain_license" >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$libsecret_license" "$LGPL21_LICENSE_SHA256"; then
        echo "LGPL-2.1 license is invalid: $libsecret_license" >&2
        return 1
    fi

    lib_dir=$(prepare_contained_appdir_directory \
        "$appdir" lib) || return
    license_dir=$(prepare_contained_appdir_directory \
        "$appdir" usr/share/doc/GoldenCheetah/licenses) || return
    libgcrypt="$lib_dir/libgcrypt.so.20"
    if [ -L "$libgcrypt" ] ||
       [ ! -f "$libgcrypt" ] || [ ! -r "$libgcrypt" ]; then
        echo "Bundled libgcrypt runtime is missing." >&2
        return 1
    fi

    if [ -L "$transformation_manifest" ] ||
       [ ! -f "$transformation_manifest" ] ||
       [ ! -r "$transformation_manifest" ]; then
        echo "Linux keychain transformation manifest is unavailable." >&2
        return 1
    fi
    transformation_sources="${transformation_manifest}.sources"
    if [ -L "$transformation_sources" ] ||
       [ -e "$transformation_sources" ] ||
       ! mkdir -m 0700 -- "$transformation_sources"; then
        echo "Cannot create Linux keychain transformation sources." >&2
        return 1
    fi

    install_regular_keychain_payload \
        "$resolved_library" "$lib_dir/libsecret-1.so.0" || return
    install_regular_keychain_payload \
        "$resolved_gpg_error" "$lib_dir/libgpg-error.so.0" || return
    install_regular_keychain_payload \
        "$libgcrypt_source" "$libgcrypt" || return
    install_regular_keychain_payload \
        "$lib_dir/libsecret-1.so.0" \
        "$transformation_sources/libsecret-1.so.0" || return
    install_regular_keychain_payload \
        "$libgcrypt" \
        "$transformation_sources/libgcrypt.so.20" || return
    patchelf --set-rpath '$ORIGIN' \
        "$lib_dir/libsecret-1.so.0" || return
    patchelf --set-rpath '$ORIGIN' "$libgcrypt" || return
    python3 - \
        "$appdir" "$transformation_manifest" "$transformation_sources" <<'PY'
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
import sys


SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
LINUXDEPLOYQT_TRANSFORMATION_RE = re.compile(
    r"^linuxdeployqt-no-strip:[0-9a-f]{64}:rpath=\$ORIGIN$"
)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def checked_output(relative):
    if (
        not isinstance(relative, str)
        or not relative.isascii()
        or "\\" in relative
        or "\x00" in relative
    ):
        raise ValueError("invalid Linux keychain transformation path")
    parsed = PurePosixPath(relative)
    if (
        parsed.is_absolute()
        or not parsed.parts
        or any(part in ("", ".", "..") for part in parsed.parts)
    ):
        raise ValueError("invalid Linux keychain transformation path")
    output = appdir.joinpath(*parsed.parts)
    if output.is_symlink() or not output.is_file():
        raise ValueError("Linux keychain transformation output is unavailable")
    resolved = output.resolve(strict=True)
    if resolved != output.absolute():
        raise ValueError("Linux keychain transformation output is unsafe")
    try:
        resolved.relative_to(appdir)
    except ValueError as error:
        raise ValueError(
            "Linux keychain transformation output escapes AppDir"
        ) from error
    return resolved


def checked_source(value):
    if not isinstance(value, str) or not value or "\x00" in value:
        raise ValueError("invalid Linux keychain transformation source")
    source = Path(value)
    if not source.is_absolute() or source.is_symlink():
        raise ValueError("Linux keychain transformation source is unsafe")
    resolved = source.resolve(strict=True)
    if resolved != source or not resolved.is_file():
        raise ValueError("Linux keychain transformation source is unsafe")
    return resolved


appdir_argument = Path(sys.argv[1])
appdir = appdir_argument.resolve(strict=True)
if appdir_argument.is_symlink() or not appdir.is_dir():
    raise ValueError("Linux keychain AppDir is unsafe")
manifest_argument = Path(sys.argv[2]).absolute()
if manifest_argument.is_symlink() or not manifest_argument.is_file():
    raise ValueError("Linux keychain transformation manifest is unsafe")
if manifest_argument.stat().st_size > 16 * 1024 * 1024:
    raise ValueError("Linux keychain transformation manifest is too large")
manifest = manifest_argument.resolve(strict=True)
if manifest != manifest_argument:
    raise ValueError("Linux keychain transformation manifest is not canonical")
parent = manifest.parent
sources = Path(sys.argv[3]).resolve(strict=True)
if Path(sys.argv[3]).is_symlink() or not sources.is_dir():
    raise ValueError("Linux keychain transformation sources are unavailable")

with manifest.open(encoding="utf-8") as stream:
    document = json.load(stream)
if (
    not isinstance(document, dict)
    or set(document) != {"format", "libraries"}
    or document["format"] != "goldencheetah-transformed-runtime-1"
    or not isinstance(document["libraries"], list)
    or len(document["libraries"]) > 100000
):
    raise ValueError("invalid Linux keychain transformation manifest")

expected_keys = {
    "output_sha256",
    "path",
    "source_path",
    "source_sha256",
    "transformation",
}
replacement_paths = {"lib/libgcrypt.so.20", "lib/libsecret-1.so.0"}
entries = []
input_paths = []
for entry in document["libraries"]:
    if not isinstance(entry, dict) or set(entry) != expected_keys:
        raise ValueError("invalid Linux keychain transformation entry")
    relative = entry["path"]
    output = checked_output(relative)
    source = checked_source(entry["source_path"])
    source_digest = entry["source_sha256"]
    output_digest = entry["output_sha256"]
    transformation = entry["transformation"]
    if (
        not isinstance(source_digest, str)
        or not SHA256_RE.fullmatch(source_digest)
        or not isinstance(output_digest, str)
        or not SHA256_RE.fullmatch(output_digest)
        or not isinstance(transformation, str)
        or (
            transformation != "patchelf-set-rpath:$ORIGIN"
            and not LINUXDEPLOYQT_TRANSFORMATION_RE.fullmatch(transformation)
        )
    ):
        raise ValueError("invalid Linux keychain transformation entry")
    if sha256_file(source) != source_digest:
        raise ValueError("Linux keychain transformation source digest mismatch")
    input_paths.append(relative)
    if relative in replacement_paths:
        continue
    if sha256_file(output) != output_digest:
        raise ValueError("Linux keychain transformation output digest mismatch")
    entries.append(dict(entry))
if input_paths != sorted(set(input_paths)):
    raise ValueError("Linux keychain transformations are not unique and sorted")

for relative in ("lib/libgcrypt.so.20", "lib/libsecret-1.so.0"):
    output = checked_output(relative)
    source = sources / Path(relative).name
    if source.is_symlink():
        raise ValueError("Linux keychain transformation payload is a symlink")
    if not source.is_file():
        raise ValueError("Linux keychain transformation payload is unavailable")
    entries.append(
        {
            "output_sha256": sha256_file(output),
            "path": relative,
            "source_path": str(source),
            "source_sha256": sha256_file(source),
            "transformation": "patchelf-set-rpath:$ORIGIN",
        }
    )
entries.sort(key=lambda entry: entry["path"])
if len(entries) != len({entry["path"] for entry in entries}):
    raise ValueError("Linux keychain transformations conflict")

document = {
    "format": "goldencheetah-transformed-runtime-1",
    "libraries": entries,
}
descriptor, temporary_name = tempfile.mkstemp(
    prefix=manifest.name + ".tmp.", dir=parent
)
temporary = Path(temporary_name)
try:
    with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o600)
    os.replace(temporary, manifest)
finally:
    try:
        temporary.unlink()
    except FileNotFoundError:
        pass
PY
    install_regular_keychain_payload \
        "$libsecret_copyright" \
        "$license_dir/libsecret-copyright" || return
    install_regular_keychain_payload \
        "$qtkeychain_license" \
        "$license_dir/QtKeychain-COPYING" || return
    install_regular_keychain_payload \
        "$libsecret_license" \
        "$license_dir/LGPL-2.1" || return
    linux_keychain_runtime_status "$appdir"
}

create_linux_keychain_deploy_probe()
{
    local executable=$1
    local directory=$2
    local probe="$directory/.goldencheetah-libsecret-deploy-probe"

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot create a libsecret deploy probe from $executable" >&2
        return 1
    fi
    if [ ! -d "$directory" ] || [ -e "$probe" ]; then
        echo "Cannot create the libsecret deploy probe in $directory" >&2
        return 1
    fi
    if ! install -m 0755 "$executable" "$probe"; then
        return 1
    fi
    if ! patchelf --add-needed libsecret-1.so.0 "$probe"; then
        rm -f -- "$probe"
        return 1
    fi
    if ! LC_ALL=C readelf -d "$probe" 2>/dev/null |
        grep -Eq '\(NEEDED\).*\[libsecret-1\.so\.0\]'; then
        rm -f -- "$probe"
        echo "The libsecret deploy probe has no DT_NEEDED entry." >&2
        return 1
    fi
    printf '%s\n' "$probe"
}

remove_linux_keychain_deploy_probe()
{
    local probe=$1
    case "$probe" in
    */.goldencheetah-libsecret-deploy-probe) ;;
    *)
        echo "Refusing to remove an unexpected deploy probe path." >&2
        return 1
        ;;
    esac
    rm -f -- "$probe"
}

run_linuxdeployqt_with_keychain_probe()
(
    local executable=$1
    local appdir=$2
    shift 2
    local probe command_status

    probe=$(create_linux_keychain_deploy_probe \
        "$executable" "$appdir") || return
    cleanup_linux_keychain_deploy_probe()
    {
        if [ -n "$probe" ]; then
            remove_linux_keychain_deploy_probe "$probe"
        fi
    }
    trap cleanup_linux_keychain_deploy_probe EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if run_packaging_appimage "$@" "-executable=$probe"; then
        command_status=0
    else
        command_status=$?
    fi
    if ! remove_linux_keychain_deploy_probe "$probe"; then
        return 1
    fi
    probe=
    return "$command_status"
)

linux_keychain_entrypoint_status()
(
    local executable=$1
    local status_home=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    cleanup_keychain_status_home()
    {
        if [ -n "$status_home" ]; then
            rm -rf -- "$status_home"
            status_home=
        fi
    }
    trap cleanup_keychain_status_home EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot inspect the Linux keychain entrypoint." >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$script_magic" != "2321" ]; then
            echo "Linux keychain status requires an ELF or script entrypoint." >&2
            return 1
        fi
    fi

    local report captured status_file command_status expected_report
    status_home=$(mktemp -d) || return
    mkdir -p "$status_home/.config" || return
    status_file="$status_home/keychain-status"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            QTKEYCHAIN_BACKEND=libsecret \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s "$executable" \
                --goldencheetah-linux-keychain-status
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    captured=$(
        cat "$status_file" 2>/dev/null
        printf x
    )
    report=${captured%x}
    if [ "$command_status" -ne 0 ]; then
        echo "GoldenCheetah keychain-status command failed." >&2
        return 1
    fi
    expected_report=$(
        printf '%s\n' \
            "goldencheetah_linux_keychain_status=1" \
            "application=GoldenCheetah" \
            "libsecret_compile_support=enabled" \
            "libsecret_runtime=available"
        printf x
    )
    expected_report=${expected_report%x}
    if [ "$report" != "$expected_report" ]; then
        echo "GoldenCheetah returned an unavailable keychain status." >&2
        return 1
    fi
    echo "Linux keychain runtime: available"
)

linux_keychain_appimage_status()
(
    local image=$1
    local extract_dir=
    local original_working_dir
    local APPDIR= APPIMAGE= OWD=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    original_working_dir=$(pwd -P) || return

    cleanup_keychain_extract_dir()
    {
        if [ -n "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
            extract_dir=
        fi
    }
    trap cleanup_keychain_extract_dir EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot inspect the Linux keychain runtime in $image" >&2
        return 1
    fi
    local appimage_magic
    appimage_magic=$(dd if="$image" bs=1 skip=8 count=3 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$appimage_magic" != "414902" ]; then
        echo "Linux keychain package status requires a Type 2 AppImage." >&2
        return 1
    fi

    local image_dir image_name image_path app_root entrypoint
    image_dir=$(cd -- "$(dirname -- "$image")" && pwd) || return
    image_name=$(basename -- "$image") || return
    image_path="$image_dir/$image_name"
    extract_dir=$(mktemp -d) || return
    app_root="$extract_dir/squashfs-root"

    trusted_appimage_extract "$image_path" "$extract_dir" || return
    linux_keychain_runtime_status "$app_root" >/dev/null || return
    entrypoint=$(readlink -f "$app_root/AppRun") || return
    case "$entrypoint" in
    "$app_root"/*) ;;
    *)
        echo "AppImage AppRun resolves outside its payload." >&2
        return 1
        ;;
    esac
    APPDIR="$app_root" \
        APPIMAGE="$image_path" \
        OWD="$original_working_dir" \
        linux_keychain_entrypoint_status "$app_root/AppRun" \
            >/dev/null || return
    echo "Linux keychain runtime: bundled"
)

require_linux_keychain_appimage()
{
    local image=$1
    local status

    status=$(linux_keychain_appimage_status "$image") || return
    if [ "$status" != "Linux keychain runtime: bundled" ]; then
        echo "$status" >&2
        echo "Refusing to publish an AppImage without the Linux" \
            "keychain runtime and licenses." >&2
        return 1
    fi
    echo "$status"
}

build_provenance_report()
(
    local executable=$1
    local status_home= status_file= command_status
    local -a lines=()

    cleanup_build_provenance_home()
    {
        if [ -n "$status_home" ] && [ -d "$status_home" ]; then
            rm -rf -- "$status_home"
        fi
    }
    trap cleanup_build_provenance_home EXIT

    if [ ! -f "$executable" ] || [ ! -x "$executable" ]; then
        echo "Cannot inspect build provenance in $executable" >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "${GC_TEST_BUILD_PROVENANCE_ENTRYPOINT:-false}" != true ] ||
           [ "$script_magic" != "2321" ]; then
            echo "Build provenance requires an ELF executable." >&2
            return 1
        fi
    fi

    status_home=$(mktemp -d) || return
    mkdir -p "$status_home/.config" || return
    status_file="$status_home/build-provenance"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s \
                "$executable" --goldencheetah-build-provenance
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    if [ "$command_status" -ne 0 ] ||
       [ ! -s "$status_file" ] ||
       [ "$(wc -c <"$status_file")" -gt 4096 ] ||
       [ "$(tail -c 1 "$status_file" | od -An -tu1 | tr -d ' \n')" != 10 ]; then
        echo "GoldenCheetah build-provenance command failed." >&2
        return 1
    fi

    if ! LC_ALL=C tr -d '\000' <"$status_file" | cmp -s - "$status_file"; then
        echo "GoldenCheetah returned an invalid build-provenance report." >&2
        return 1
    fi
    mapfile -t lines <"$status_file"
    if [ "${#lines[@]}" -ne 8 ] ||
       [ "${lines[0]}" != "goldencheetah_build_provenance=1" ] ||
       [ "${lines[1]}" != "application=GoldenCheetah" ] ||
       [[ ! "${lines[2]}" =~ ^source_revision=[0-9a-f]{40}$ ]] ||
       [[ ! "${lines[3]}" =~ ^build_inputs_sha256=[0-9a-f]{64}$ ]] ||
       [[ ! "${lines[4]}" =~ ^compiler_family=(gcc|clang)$ ]] ||
       [[ ! "${lines[5]}" =~ ^compiler_version=[0-9]+(\.[0-9]+){1,3}$ ]] ||
       [[ ! "${lines[6]}" =~ ^qt_version=[0-9]+(\.[0-9]+){1,3}$ ]] ||
       [[ ! "${lines[7]}" =~ ^cxx_standard=[0-9]+$ ]]; then
        echo "GoldenCheetah returned an invalid build-provenance report." >&2
        return 1
    fi
    printf '%s\n' "${lines[@]}"
)

verified_source_revision()
(
    local source_root=$1
    local git_root revision head status

    git_root=$(git -C "$source_root" rev-parse --show-toplevel 2>/dev/null) || {
        echo "AppImage packaging requires a Git source tree." >&2
        return 1
    }
    if [ "$(cd "$source_root" && pwd -P)" != "$(cd "$git_root" && pwd -P)" ]; then
        echo "The AppImage source root is not the Git worktree root." >&2
        return 1
    fi
    revision=${GC_SOURCE_REVISION:-$(git -C "$git_root" rev-parse HEAD)}
    if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
        echo "GC_SOURCE_REVISION must be a full lowercase Git commit hash" >&2
        return 1
    fi
    git -C "$git_root" cat-file -e "$revision^{commit}" 2>/dev/null || {
        echo "GC_SOURCE_REVISION does not identify a source commit." >&2
        return 1
    }
    head=$(git -C "$git_root" rev-parse HEAD) || return
    if [ "$revision" != "$head" ]; then
        echo "GC_SOURCE_REVISION does not match the source HEAD." >&2
        return 1
    fi
    status=$(git -C "$git_root" status --porcelain=v1 \
        --untracked-files=normal --ignore-submodules=none) || return
    if [ -n "$status" ]; then
        echo "Refusing to package a dirty source worktree." >&2
        return 1
    fi
    printf '%s\n' "$revision"
)

validate_appimage_base_manifest()
{
    local manifest=$1
    local -a lines=()

    [ -f "$manifest" ] && [ ! -L "$manifest" ] || return 1
    [ "$(wc -c <"$manifest")" -le 4096 ] || return 1
    [ "$(tail -c 1 "$manifest" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$manifest"
    [ "${#lines[@]}" -eq 6 ] || return 1
    [ "${lines[0]}" = "goldencheetah_appimage_manifest=2" ] || return 1
    [[ "${lines[1]}" =~ ^source_revision=[0-9a-f]{40}$ ]] || return 1
    [[ "${lines[2]}" =~ ^build_inputs_sha256=[0-9a-f]{64}$ ]] || return 1
    [[ "${lines[3]}" =~ ^raw_elf_sha256=[0-9a-f]{64}$ ]] || return 1
    [[ "${lines[4]}" =~ ^toolchain=(gcc|clang)-[0-9]+(\.[0-9]+){1,3}_qt-[0-9]+(\.[0-9]+){1,3}_cxx-[0-9]+$ ]] ||
        return 1
    [[ "${lines[5]}" =~ ^strava_oauth_configured=(true|false)$ ]] ||
        return 1
}

validate_legacy_appimage_base_manifest()
{
    local manifest=$1
    local -a lines=()

    [ -f "$manifest" ] && [ ! -L "$manifest" ] || return 1
    [ "$(wc -c <"$manifest")" -le 4096 ] || return 1
    [ "$(tail -c 1 "$manifest" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$manifest"
    [ "${#lines[@]}" -eq 5 ] || return 1
    [ "${lines[0]}" = "goldencheetah_appimage_manifest=1" ] || return 1
    [[ "${lines[1]}" =~ ^source_revision=[0-9a-f]{40}$ ]] || return 1
    [[ "${lines[2]}" =~ ^raw_elf_sha256=[0-9a-f]{64}$ ]] || return 1
    [[ "${lines[3]}" =~ ^toolchain=(gcc|clang)-[0-9]+(\.[0-9]+){1,3}_qt-[0-9]+(\.[0-9]+){1,3}_cxx-[0-9]+$ ]] ||
        return 1
    [[ "${lines[4]}" =~ ^strava_oauth_configured=(true|false)$ ]] ||
        return 1
}

set_appimage_source_date_epoch()
{
    local manifest=$1
    local source_root=$2
    local revision epoch

    validate_appimage_base_manifest "$manifest" || {
        echo "Cannot derive SOURCE_DATE_EPOCH from an invalid manifest." >&2
        return 1
    }
    [ -d "$source_root" ] && [ ! -L "$source_root" ] || return 1
    revision=$(sed -n 's/^source_revision=//p' "$manifest") || return
    git -C "$source_root" cat-file -e "$revision^{commit}" 2>/dev/null || {
        echo "Build manifest revision is absent from the source repository." >&2
        return 1
    }
    epoch=$(git -C "$source_root" show -s --format=%ct "$revision") || return
    [[ "$epoch" =~ ^[1-9][0-9]*$ ]] || {
        echo "Source revision has an invalid commit timestamp." >&2
        return 1
    }
    SOURCE_DATE_EPOCH=$epoch
    export SOURCE_DATE_EPOCH
}

install_appimage_dir_icon()
{
    if [ "$#" -ne 2 ]; then
        echo "Usage: install_appimage_dir_icon APPDIR ROOT_ICON" >&2
        return 2
    fi
    local appdir=$1
    local icon=$2
    local link="$appdir/.DirIcon"

    [ -d "$appdir" ] && [ ! -L "$appdir" ] || {
        echo "Cannot install metadata in an unsafe AppDir." >&2
        return 1
    }
    case "$icon" in
    ""|.|..|*/*|*$'\n'*|*$'\r'*)
        echo "AppImage root icon name is unsafe." >&2
        return 1
        ;;
    esac
    [ -f "$appdir/$icon" ] && [ ! -L "$appdir/$icon" ] || {
        echo "AppImage root icon is missing or unsafe." >&2
        return 1
    }
    if [ -L "$link" ]; then
        [ "$(readlink -- "$link")" = "$icon" ] || {
            echo "AppImage directory icon has an unexpected target." >&2
            return 1
        }
    elif [ -e "$link" ]; then
        echo "AppImage directory icon path is unsafe." >&2
        return 1
    else
        (cd -- "$appdir" && ln -s -- "$icon" .DirIcon) || return
    fi
    [ -L "$link" ] && [ "$(readlink -- "$link")" = "$icon" ]
}

normalize_appdir_mtimes()
{
    local appdir=$1

    [ -d "$appdir" ] && [ ! -L "$appdir" ] || {
        echo "Cannot normalize an unsafe AppDir." >&2
        return 1
    }
    [[ "${SOURCE_DATE_EPOCH:-}" =~ ^[1-9][0-9]*$ ]] || {
        echo "SOURCE_DATE_EPOCH is not set to a valid timestamp." >&2
        return 1
    }
    find -P "$appdir" -depth -exec \
        touch -h --date="@${SOURCE_DATE_EPOCH}" -- {} +
}

compare_appimage_reproduction()
{
    if [ "$#" -ne 2 ]; then
        echo "Usage: compare_appimage_reproduction PASS_ONE PASS_TWO" >&2
        return 2
    fi
    local first=$1
    local second=$2
    local directory relative first_file second_file first_hash second_hash
    local embedded_hash

    for directory in "$first" "$second"; do
        [ -d "$directory" ] && [ ! -L "$directory" ] || {
            echo "Invalid AppImage reproduction directory: $directory" >&2
            return 1
        }
    done

    for relative in \
        build.manifest \
        GoldenCheetah.AppImage \
        GoldenCheetah.AppImage.manifest \
        GoldenCheetah.AppImage.sbom.cdx.json; do
        first_file="$first/$relative"
        second_file="$second/$relative"
        [ -f "$first_file" ] && [ ! -L "$first_file" ] &&
            [ -f "$second_file" ] && [ ! -L "$second_file" ] || {
                echo "Missing reproducibility output: $relative" >&2
                return 1
            }
        first_hash=$(sha256sum "$first_file" | cut -d ' ' -f 1) || return
        second_hash=$(sha256sum "$second_file" | cut -d ' ' -f 1) || return
        if [ "$first_hash" != "$second_hash" ] ||
           ! cmp -s -- "$first_file" "$second_file"; then
            echo "AppImage reproduction mismatch: $relative" >&2
            return 1
        fi
    done

    first_hash=$(sha256sum "$first/GoldenCheetah.AppImage" |
        cut -d ' ' -f 1) || return
    for relative in "$first" "$second"; do
        embedded_hash=$(sed -n 's/^appimage_sha256=//p' \
            "$relative/GoldenCheetah.AppImage.manifest") || return
        [ "$embedded_hash" = "$first_hash" ] || {
            echo "Final manifest does not identify reproduced AppImage." >&2
            return 1
        }
    done
    printf 'Reproduced AppImage SHA-256: %s\n' "$first_hash"
}

create_appimage_build_manifest()
{
    local source_root=$1
    local binary=$2
    local oauth_status=$3
    local output=$4
    local revision report binary_revision binary_build_inputs build_inputs
    local compiler_family compiler_version
    local qt_version cxx_standard raw_hash oauth_configured temporary

    revision=$(verified_source_revision "$source_root") || return
    report=$(build_provenance_report "$binary") || return
    binary_revision=$(printf '%s\n' "$report" | sed -n 's/^source_revision=//p')
    if [ "$binary_revision" != "$revision" ]; then
        echo "The GoldenCheetah binary was not built from the source HEAD." >&2
        return 1
    fi
    [ -f "$GC_BUILD_INPUT_IDENTITY_TOOL" ] &&
        [ ! -L "$GC_BUILD_INPUT_IDENTITY_TOOL" ] || return 1
    build_inputs=$(python3 "$GC_BUILD_INPUT_IDENTITY_TOOL" "$source_root") ||
        return
    binary_build_inputs=$(printf '%s\n' "$report" |
        sed -n 's/^build_inputs_sha256=//p')
    if [ "$binary_build_inputs" != "$build_inputs" ]; then
        echo "The GoldenCheetah binary build inputs do not match the release source." >&2
        return 1
    fi
    compiler_family=$(printf '%s\n' "$report" | sed -n 's/^compiler_family=//p')
    compiler_version=$(printf '%s\n' "$report" | sed -n 's/^compiler_version=//p')
    qt_version=$(printf '%s\n' "$report" | sed -n 's/^qt_version=//p')
    cxx_standard=$(printf '%s\n' "$report" | sed -n 's/^cxx_standard=//p')
    raw_hash=$(sha256sum "$binary" | cut -d ' ' -f 1) || return
    case "$oauth_status" in
    "Strava OAuth: configured") oauth_configured=true ;;
    "Strava OAuth: runtime credentials supported")
        oauth_configured=false ;;
    "Strava OAuth: unavailable (credentials not configured)")
        oauth_configured=false ;;
    "Strava OAuth compile-time fallback: unavailable")
        oauth_configured=false ;;
    *)
        echo "Cannot encode an unknown Strava OAuth status." >&2
        return 1
        ;;
    esac

    mkdir -p "$(dirname "$output")" || return
    temporary=$(mktemp "${output}.tmp.XXXXXX") || return
    if ! (
        umask 077
        printf '%s\n' \
            "goldencheetah_appimage_manifest=2" \
            "source_revision=$revision" \
            "build_inputs_sha256=$build_inputs" \
            "raw_elf_sha256=$raw_hash" \
            "toolchain=${compiler_family}-${compiler_version}_qt-${qt_version}_cxx-${cxx_standard}" \
            "strava_oauth_configured=$oauth_configured" >"$temporary"
        chmod 0600 "$temporary"
    ) || ! validate_appimage_base_manifest "$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi
    mv -f -- "$temporary" "$output"
}

install_appimage_build_manifest()
{
    local manifest=$1
    local appdir=$2
    local target="$appdir/usr/share/goldencheetah/build-manifest"

    validate_appimage_base_manifest "$manifest" || {
        echo "Cannot install an invalid AppImage build manifest." >&2
        return 1
    }
    install -D -m 0644 "$manifest" "$target"
    cmp -s -- "$manifest" "$target"
}

create_appimage_sbom()
(
    if [ "$#" -ne 9 ]; then
        echo "Usage: create_appimage_sbom APPDIR MANIFEST BUILD_CONFIG LOCK PIP_REPORT OUTPUT GENERATOR RUNTIME_GENERATOR TRANSFORMATION_MANIFEST" >&2
        return 2
    fi

    local appdir=$1
    local manifest=$2
    local build_config=$3
    local requirements_lock=$4
    local python_install_report=$5
    local python_runtime_manifest="${python_install_report}.runtime-libraries.json"
    local python_wheel_manifest="${python_install_report}.wheel-records.json"
    local output=$6
    local generator=$7
    local runtime_generator=$8
    local transformed_runtime_manifest=$9
    local qt_version qt_root runtime_provenance

    validate_appimage_base_manifest "$manifest" || {
        echo "Cannot create an SBOM from an invalid build manifest." >&2
        return 1
    }
    [ -d "$appdir" ] && [ ! -L "$appdir" ] || return 1
    [ -f "$build_config" ] && [ ! -L "$build_config" ] || return 1
    [ -f "$requirements_lock" ] && [ ! -L "$requirements_lock" ] ||
        return 1
    [ -f "$python_install_report" ] &&
        [ ! -L "$python_install_report" ] || return 1
    [ -f "$python_runtime_manifest" ] &&
        [ ! -L "$python_runtime_manifest" ] || return 1
    [ -f "$python_wheel_manifest" ] &&
        [ ! -L "$python_wheel_manifest" ] || return 1
    [ -f "$generator" ] && [ ! -L "$generator" ] || return 1
    [ -f "$runtime_generator" ] && [ ! -L "$runtime_generator" ] || return 1
    [ -f "$transformed_runtime_manifest" ] &&
        [ ! -L "$transformed_runtime_manifest" ] || return 1
    qt_version=$(sed -n \
        's/^toolchain=.*_qt-\([0-9][0-9.]*\)_cxx-.*/\1/p' "$manifest")
    [[ "$qt_version" =~ ^[0-9]+(\.[0-9]+){1,3}$ ]] || return 1
    qt_root=$(qmake -query QT_INSTALL_PREFIX) || return
    [ -d "$qt_root" ] && [ ! -L "$qt_root" ] || return 1
    runtime_provenance=$(mktemp) || return
    trap 'rm -f -- "$runtime_provenance"' EXIT
    if [ -n "${GC_RUNTIME_PROVENANCE_PACKAGE_INDEX:-}" ]; then
        echo "Legacy runtime package indexes are disabled." >&2
        return 1
    fi
    if [ -n "${GC_RUNTIME_PROVENANCE_FIXTURE_INDEX:-}" ] ||
       [ -n "${GC_RUNTIME_PROVENANCE_FIXTURE_INDEX_SHA256:-}" ] ||
       [ -n "${GC_RUNTIME_PROVENANCE_TEST_MODE:-}" ]; then
        echo "Runtime provenance fixtures cannot be enabled by packaging callers." >&2
        return 1
    fi

    python3 "$runtime_generator" \
        --appdir "$appdir" --output "$runtime_provenance" \
        --qt-version "$qt_version" \
        --qt-root "$qt_root" \
        --qt-provenance "qmake-reported-qt-sdk:${qt_version}" \
        --python-version "$PYTHON_APPIMAGE_VERSION" \
        --python-provenance "${PYTHON_APPIMAGE_URL}#sha256=${PYTHON_APPIMAGE_SHA256}" \
        --python-runtime-manifest "$python_runtime_manifest" \
        --python-runtime-sha256 "$PYTHON_APPIMAGE_SHA256" \
        --python-wheel-manifest "$python_wheel_manifest" \
        --requirements-lock "$requirements_lock" \
        --python-install-report "$python_install_report" \
        --d2xx-version "$D2XX_LINUX_VERSION" \
        --d2xx-provenance "${D2XX_LINUX_SOURCE_URL}#sha256=${D2XX_LINUX_SHA256}" \
        --linuxdeployqt-sha256 "$LINUXDEPLOYQT_SHA256" \
        --transformed-runtime-manifest "$transformed_runtime_manifest"

    python3 "$generator" \
        --appdir "$appdir" \
        --build-manifest "$manifest" \
        --requirements-lock "$requirements_lock" \
        --python-install-report "$python_install_report" \
        --python-wheel-manifest "$python_wheel_manifest" \
        --build-config "$build_config" \
        --runtime-provenance "$runtime_provenance" \
        --output "$output" \
        --linuxdeployqt-file "$LINUXDEPLOYQT_FILE" \
        --linuxdeployqt-sha256 "$LINUXDEPLOYQT_SHA256" \
        --appimagetool-file "$APPIMAGETOOL_FILE" \
        --appimagetool-sha256 "$APPIMAGETOOL_SHA256" \
        --appimage-runtime-file "$APPIMAGE_RUNTIME_FILE" \
        --appimage-runtime-sha256 "$APPIMAGE_RUNTIME_SHA256" \
        --python-runtime-file "$PYTHON_APPIMAGE_FILE" \
        --python-runtime-sha256 "$PYTHON_APPIMAGE_SHA256" \
        --srmio-revision "$SRMIO_REVISION" \
        --srmio-source-sha256 "$SRMIO_SOURCE_SHA256" \
        --d2xx-linux-version "$D2XX_LINUX_VERSION" \
        --d2xx-linux-sha256 "$D2XX_LINUX_SHA256"
)

validate_appimage_sbom()
{
    local sbom=$1

    [ -f "$sbom" ] && [ ! -L "$sbom" ] || return 1
    [ "$(wc -c <"$sbom")" -le 67108864 ] || return 1
    python3 - "$sbom" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
if set(document) != {
    "$schema", "bomFormat", "specVersion", "version", "metadata",
    "components", "dependencies",
}:
    raise SystemExit(1)
if document.get("$schema") != \
        "http://cyclonedx.org/schema/bom-1.5.schema.json":
    raise SystemExit(1)
if document.get("bomFormat") != "CycloneDX":
    raise SystemExit(1)
if document.get("specVersion") != "1.5" or document.get("version") != 1:
    raise SystemExit(1)
component = document.get("metadata", {}).get("component", {})
if set(document.get("metadata", {})) != {"component"}:
    raise SystemExit(1)
if component.get("name") != "GoldenCheetah" or \
        component.get("type") != "application":
    raise SystemExit(1)
if not re.fullmatch(r"[0-9a-f]{40}", component.get("version", "")):
    raise SystemExit(1)
application_ref = component.get("bom-ref")
if application_ref != "pkg:generic/goldencheetah@{}".format(
        component["version"]):
    raise SystemExit(1)
if component.get("licenses") != [{"license": {"id": "GPL-2.0-or-later"}}]:
    raise SystemExit(1)
components = document.get("components")
if not isinstance(components, list) or not components:
    raise SystemExit(1)
refs = [entry.get("bom-ref") for entry in components]
if any(not isinstance(ref, str) or not ref for ref in refs):
    raise SystemExit(1)
if len(refs) != len(set(refs)):
    raise SystemExit(1)
if refs != sorted(refs):
    raise SystemExit(1)
allowed_types = {"application", "file", "framework", "library"}
hash_pattern = re.compile(r"[0-9a-f]{64}")
for entry in components:
    if entry.get("type") not in allowed_types:
        raise SystemExit(1)
    if not isinstance(entry.get("name"), str) or not entry["name"]:
        raise SystemExit(1)
    if "hashes" in entry:
        hashes = entry["hashes"]
        if not isinstance(hashes, list) or not hashes:
            raise SystemExit(1)
        if any(set(item) != {"alg", "content"} or
               item["alg"] != "SHA-256" or
               not hash_pattern.fullmatch(item["content"])
               for item in hashes):
            raise SystemExit(1)
    if "purl" in entry and not re.fullmatch(
            r"pkg:(pypi|github|generic|deb)/[^@\s]+@[^\s]+", entry["purl"]):
        raise SystemExit(1)
    properties = entry.get("properties", [])
    if not isinstance(properties, list) or any(
            set(item) != {"name", "value"} or
            not isinstance(item["name"], str) or
            not item["name"].startswith("goldencheetah:") or
            not isinstance(item["value"], str)
            for item in properties):
        raise SystemExit(1)
    roles = {
        item["value"] for item in properties
        if item["name"] == "goldencheetah:role"
    }
    if "identified-runtime-dependency" in roles:
        if not isinstance(entry.get("version"), str) or not entry["version"]:
            raise SystemExit(1)
        licenses = entry.get("licenses")
        if not isinstance(licenses, list) or len(licenses) != 1:
            raise SystemExit(1)
        license_value = licenses[0].get("license", {})
        if set(license_value) not in ({"id"}, {"name"}) or \
           not all(isinstance(value, str) and value
                   for value in license_value.values()):
            raise SystemExit(1)
        provenance = [
            item["value"] for item in properties
            if item["name"] == "goldencheetah:provenance"
        ]
        runtime_paths = [
            item["value"] for item in properties
            if item["name"] == "goldencheetah:runtime-path"
        ]
        if len(provenance) != 1 or len(runtime_paths) != 1:
            raise SystemExit(1)
dependencies = document.get("dependencies")
if not isinstance(dependencies, list) or len(dependencies) != 1:
    raise SystemExit(1)
dependency = dependencies[0]
if set(dependency) != {"ref", "dependsOn"} or \
        dependency["ref"] != application_ref:
    raise SystemExit(1)
depends_on = dependency["dependsOn"]
if not isinstance(depends_on, list) or depends_on != sorted(set(depends_on)):
    raise SystemExit(1)
if any(reference not in set(refs) for reference in depends_on):
    raise SystemExit(1)

private_path = re.compile(r"(?:^|[^A-Za-z])/(?:home|Users)/|[A-Za-z]:\\Users\\")
def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for key, item in value.items():
            yield key
            yield from strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from strings(item)

if any(private_path.search(value) for value in strings(document)):
    raise SystemExit(1)
PY
}

verify_appimage_sbom()
(
    local image=$1
    local sbom=$2
    local image_path extract_dir= embedded

    cleanup_sbom_extract()
    {
        if [ -n "$extract_dir" ] && [ -d "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
        fi
    }
    trap cleanup_sbom_extract EXIT

    [ -f "$image" ] && [ -x "$image" ] || return 1
    validate_appimage_sbom "$sbom" || return
    image_path=$(readlink -f -- "$image") || return

    extract_dir=$(mktemp -d) || return
    trusted_appimage_extract "$image_path" "$extract_dir" || return
    embedded="$extract_dir/squashfs-root/usr/share/goldencheetah/goldencheetah.cdx.json"
    validate_appimage_sbom "$embedded" || return
    cmp -s -- "$sbom" "$embedded" || return
    [ -f "$GC_APPIMAGE_PAYLOAD_VERIFIER" ] &&
        [ ! -L "$GC_APPIMAGE_PAYLOAD_VERIFIER" ] || return 1
    python3 "$GC_APPIMAGE_PAYLOAD_VERIFIER" \
        "$extract_dir/squashfs-root" "$embedded"
)

finalize_appimage_manifest()
{
    local image=$1
    local base_manifest=$2
    local sidecar=$3
    local image_hash temporary

    [ -f "$image" ] && [ -x "$image" ] || {
        echo "Cannot finalize a missing AppImage." >&2
        return 1
    }
    validate_appimage_base_manifest "$base_manifest" || {
        echo "Cannot finalize an invalid AppImage build manifest." >&2
        return 1
    }
    image_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    mkdir -p "$(dirname "$sidecar")" || return
    temporary=$(mktemp "${sidecar}.tmp.XXXXXX") || return
    if ! (
        umask 077
        cat "$base_manifest" >"$temporary"
        printf 'appimage_sha256=%s\n' "$image_hash" >>"$temporary"
        chmod 0600 "$temporary"
    ); then
        rm -f -- "$temporary"
        return 1
    fi
    mv -f -- "$temporary" "$sidecar"
}

verify_appimage_manifest()
(
    local image=$1
    local sidecar=$2
    local image_path extract_dir payload_dir embedded expected_base
    local expected_hash actual_hash
    local -a lines=()

    cleanup_manifest_extract()
    {
        if [ -n "${extract_dir:-}" ] && [ -d "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
        fi
    }
    trap cleanup_manifest_extract EXIT

    [ -f "$image" ] && [ -x "$image" ] || return 1
    [ -f "$sidecar" ] && [ ! -L "$sidecar" ] || return 1
    [ "$(stat -c '%a' "$sidecar")" = 600 ] || return 1
    [ "$(wc -c <"$sidecar")" -le 4352 ] || return 1
    [ "$(tail -c 1 "$sidecar" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$sidecar"
    [ "${#lines[@]}" -eq 7 ] || return 1
    expected_hash=${lines[6]#appimage_sha256=}
    [[ "${lines[6]}" =~ ^appimage_sha256=[0-9a-f]{64}$ ]] || return 1
    actual_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    [ "$actual_hash" = "$expected_hash" ] || return 1

    extract_dir=$(mktemp -d) || return
    expected_base="$extract_dir/expected-base"
    payload_dir="$extract_dir/payload"
    mkdir "$payload_dir" || return
    (umask 077; printf '%s\n' "${lines[@]:0:6}" >"$expected_base") ||
        return
    validate_appimage_base_manifest "$expected_base" || return

    image_path=$(readlink -f -- "$image") || return
    trusted_appimage_extract "$image_path" "$payload_dir" || return
    embedded="$payload_dir/squashfs-root/usr/share/goldencheetah/build-manifest"
    if [ ! -f "$embedded" ] || [ -L "$embedded" ] ||
       ! validate_appimage_base_manifest "$embedded" ||
       ! cmp -s -- "$expected_base" "$embedded"; then
        return 1
    fi
)

verify_legacy_appimage_manifest()
(
    local image=$1
    local sidecar=$2
    local image_path extract_dir payload_dir embedded expected_base
    local expected_hash actual_hash
    local -a lines=()

    cleanup_legacy_manifest_extract()
    {
        if [ -n "${extract_dir:-}" ] && [ -d "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
        fi
    }
    trap cleanup_legacy_manifest_extract EXIT

    [ -f "$image" ] && [ -x "$image" ] || return 1
    [ -f "$sidecar" ] && [ ! -L "$sidecar" ] || return 1
    [ "$(stat -c '%a' "$sidecar")" = 600 ] || return 1
    [ "$(wc -c <"$sidecar")" -le 4352 ] || return 1
    [ "$(tail -c 1 "$sidecar" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$sidecar"
    [ "${#lines[@]}" -eq 6 ] || return 1
    expected_hash=${lines[5]#appimage_sha256=}
    [[ "${lines[5]}" =~ ^appimage_sha256=[0-9a-f]{64}$ ]] || return 1
    actual_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    [ "$actual_hash" = "$expected_hash" ] || return 1

    extract_dir=$(mktemp -d) || return
    expected_base="$extract_dir/expected-base"
    payload_dir="$extract_dir/payload"
    mkdir "$payload_dir" || return
    (umask 077; printf '%s\n' "${lines[@]:0:5}" >"$expected_base") ||
        return
    validate_legacy_appimage_base_manifest "$expected_base" || return

    image_path=$(readlink -f -- "$image") || return
    trusted_appimage_extract "$image_path" "$payload_dir" || return
    embedded="$payload_dir/squashfs-root/usr/share/goldencheetah/build-manifest"
    if [ ! -f "$embedded" ] || [ -L "$embedded" ] ||
       ! validate_legacy_appimage_base_manifest "$embedded" ||
       ! cmp -s -- "$expected_base" "$embedded"; then
        return 1
    fi
)

promote_appimage_release()
(
    if [ "$#" -ne 4 ]; then
        echo "Usage: promote_appimage_release IMAGE MANIFEST SBOM RELEASE_LINK" >&2
        return 2
    fi

    local image=$1
    local manifest=$2
    local sbom=$3
    local release_link=$4
    local release_parent release_name store artifacts sets
    local image_hash revision artifact_id artifact_dir artifact_temp=
    local current_dir= current_hash= previous_image previous_manifest
    local previous_sbom
    local current_has_sbom=true
    local set_id set_dir set_temp= pointer_temp=
    local old_pointer=
    local lock_path

    cleanup_appimage_promotion()
    {
        [ -z "$artifact_temp" ] || rm -rf -- "$artifact_temp"
        [ -z "$set_temp" ] || rm -rf -- "$set_temp"
        [ -z "$pointer_temp" ] || rm -f -- "$pointer_temp"
    }
    trap cleanup_appimage_promotion EXIT

    require_private_release_directory()
    {
        local path=$1

        if [ -L "$path" ] || { [ -e "$path" ] && [ ! -d "$path" ]; }; then
            echo "Unsafe AppImage release-store directory: $path" >&2
            return 1
        fi
        if [ ! -e "$path" ]; then
            (umask 077; mkdir -- "$path") 2>/dev/null ||
                { [ -d "$path" ] && [ ! -L "$path" ]; } || return
        fi
        [ -d "$path" ] && [ ! -L "$path" ] || return 1
        [ "$(stat -c '%u' "$path")" = "$(id -u)" ] || return 1
        if [ $((8#$(stat -c '%a' "$path") & 8#022)) -ne 0 ]; then
            echo "AppImage release-store directory is writable by another user: $path" >&2
            return 1
        fi
    }

    verify_appimage_manifest "$image" "$manifest" || {
        echo "Refusing to promote an unverified AppImage." >&2
        return 1
    }
    verify_appimage_sbom "$image" "$sbom" || {
        echo "Refusing to promote an AppImage without its verified SBOM." >&2
        return 1
    }
    image=$(readlink -f -- "$image") || return
    manifest=$(readlink -f -- "$manifest") || return
    sbom=$(readlink -f -- "$sbom") || return
    image_hash=$(sed -n 's/^appimage_sha256=//p' "$manifest")
    revision=$(sed -n 's/^source_revision=//p' "$manifest")
    [[ "$image_hash" =~ ^[0-9a-f]{64}$ ]] || return 1
    [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || return 1

    release_parent=$(dirname -- "$release_link")
    release_name=$(basename -- "$release_link")
    [ "$release_name" != . ] && [ "$release_name" != .. ] &&
        [ -n "$release_name" ] || return 1
    mkdir -p -- "$release_parent" || return
    release_parent=$(cd "$release_parent" && pwd -P) || return
    release_link="$release_parent/$release_name"
    if [ -e "$release_link" ] && [ ! -L "$release_link" ]; then
        echo "The release pointer exists and is not a symlink." >&2
        return 1
    fi

    store="$release_parent/.${release_name}.store"
    if [ -e "$store" ]; then
        [ -d "$store" ] && [ ! -L "$store" ] || return 1
    else
        (umask 077; mkdir -- "$store") || return
    fi
    [ "$(stat -c '%u' "$store")" = "$(id -u)" ] || return 1
    if [ $((8#$(stat -c '%a' "$store") & 8#022)) -ne 0 ]; then
        echo "The AppImage release store is writable by another user." >&2
        return 1
    fi
    artifacts="$store/artifacts"
    sets="$store/sets"
    require_private_release_directory "$artifacts" || return
    require_private_release_directory "$sets" || return
    lock_path="$store/promotion.lock"
    if [ -L "$lock_path" ] ||
       { [ -e "$lock_path" ] && [ ! -f "$lock_path" ]; }; then
        echo "Unsafe AppImage promotion lock: $lock_path" >&2
        return 1
    fi
    if [ ! -e "$lock_path" ]; then
        (umask 077; set -o noclobber; : >"$lock_path") 2>/dev/null ||
            { [ -f "$lock_path" ] && [ ! -L "$lock_path" ]; } || return
    fi
    [ -f "$lock_path" ] && [ ! -L "$lock_path" ] || return 1
    [ "$(stat -c '%u' "$lock_path")" = "$(id -u)" ] || return 1
    if [ $((8#$(stat -c '%a' "$lock_path") & 8#022)) -ne 0 ]; then
        echo "AppImage promotion lock is writable by another user." >&2
        return 1
    fi
    exec 9>>"$lock_path" || return
    flock -x 9 || return

    artifact_id="${revision}-${image_hash}"
    artifact_dir="$artifacts/$artifact_id"
    if [ ! -e "$artifact_dir" ]; then
        artifact_temp=$(mktemp -d "$artifacts/.tmp.XXXXXX") || return
        install -m 0755 "$image" "$artifact_temp/GoldenCheetah.AppImage" ||
            return
        install -m 0600 "$manifest" \
            "$artifact_temp/GoldenCheetah.AppImage.manifest" || return
        install -m 0644 "$sbom" \
            "$artifact_temp/GoldenCheetah.AppImage.sbom.cdx.json" || return
        verify_appimage_manifest \
            "$artifact_temp/GoldenCheetah.AppImage" \
            "$artifact_temp/GoldenCheetah.AppImage.manifest" || return
        verify_appimage_sbom \
            "$artifact_temp/GoldenCheetah.AppImage" \
            "$artifact_temp/GoldenCheetah.AppImage.sbom.cdx.json" || return
        mv -- "$artifact_temp" "$artifact_dir" || return
        artifact_temp=
    fi
    [ -d "$artifact_dir" ] && [ ! -L "$artifact_dir" ] || return 1
    verify_appimage_manifest \
        "$artifact_dir/GoldenCheetah.AppImage" \
        "$artifact_dir/GoldenCheetah.AppImage.manifest" || return
    verify_appimage_sbom \
        "$artifact_dir/GoldenCheetah.AppImage" \
        "$artifact_dir/GoldenCheetah.AppImage.sbom.cdx.json" || return
    cmp -s -- "$image" "$artifact_dir/GoldenCheetah.AppImage" || return 1
    cmp -s -- "$manifest" \
        "$artifact_dir/GoldenCheetah.AppImage.manifest" || return 1
    cmp -s -- "$sbom" \
        "$artifact_dir/GoldenCheetah.AppImage.sbom.cdx.json" || return 1

    previous_image="$artifact_dir/GoldenCheetah.AppImage"
    previous_manifest="$artifact_dir/GoldenCheetah.AppImage.manifest"
    previous_sbom="$artifact_dir/GoldenCheetah.AppImage.sbom.cdx.json"
    current_hash=$image_hash
    if [ -L "$release_link" ]; then
        old_pointer=$(readlink -- "$release_link") || return
        current_dir=$(readlink -f -- "$release_link") || return
        [ "$(dirname -- "$current_dir")" = "$sets" ] &&
            [ -d "$current_dir" ] && [ ! -L "$current_dir" ] || return 1
        if [ -f "$current_dir/latest.AppImage.sbom.cdx.json" ] &&
           [ ! -L "$current_dir/latest.AppImage.sbom.cdx.json" ]; then
            verify_appimage_manifest \
                "$current_dir/latest.AppImage" \
                "$current_dir/latest.AppImage.manifest" || return
            verify_appimage_sbom \
                "$current_dir/latest.AppImage" \
                "$current_dir/latest.AppImage.sbom.cdx.json" || return
        elif [ -e "$current_dir/latest.AppImage.sbom.cdx.json" ] ||
             [ -L "$current_dir/latest.AppImage.sbom.cdx.json" ]; then
            return 1
        else
            if ! verify_appimage_manifest \
                    "$current_dir/latest.AppImage" \
                    "$current_dir/latest.AppImage.manifest" &&
               ! verify_legacy_appimage_manifest \
                    "$current_dir/latest.AppImage" \
                    "$current_dir/latest.AppImage.manifest"; then
                return 1
            fi
            current_has_sbom=false
        fi
        current_hash=$(sed -n 's/^appimage_sha256=//p' \
            "$current_dir/latest.AppImage.manifest")
        [[ "$current_hash" =~ ^[0-9a-f]{64}$ ]] || return 1
        if [ "$current_has_sbom" = true ] &&
           [ "$current_hash" = "$image_hash" ]; then
            cmp -s -- "$image" "$current_dir/latest.AppImage" || return 1
            cmp -s -- "$manifest" \
                "$current_dir/latest.AppImage.manifest" || return 1
            cmp -s -- "$sbom" \
                "$current_dir/latest.AppImage.sbom.cdx.json" || return 1
            printf '%s\n' "$release_link/latest.AppImage"
            return 0
        fi
        if [ "$current_has_sbom" = true ]; then
            previous_image=$(readlink -f -- "$current_dir/latest.AppImage") ||
                return
            previous_manifest="$current_dir/latest.AppImage.manifest"
            previous_sbom="$current_dir/latest.AppImage.sbom.cdx.json"
            case "$previous_image" in
            "$artifacts"/*/GoldenCheetah.AppImage) ;;
            *) return 1 ;;
            esac
        else
            echo "Migrating a legacy AppImage release without an SBOM sidecar." >&2
            current_hash=$image_hash
        fi
    fi

    set_id="${image_hash}-${current_hash}"
    set_dir="$sets/$set_id"
    if [ ! -e "$set_dir" ]; then
        set_temp=$(mktemp -d "$sets/.tmp.XXXXXX") || return
        ln -s "$(realpath --relative-to="$set_temp" \
            "$artifact_dir/GoldenCheetah.AppImage")" \
            "$set_temp/latest.AppImage" || return
        install -m 0600 "$artifact_dir/GoldenCheetah.AppImage.manifest" \
            "$set_temp/latest.AppImage.manifest" || return
        install -m 0644 \
            "$artifact_dir/GoldenCheetah.AppImage.sbom.cdx.json" \
            "$set_temp/latest.AppImage.sbom.cdx.json" || return
        ln -s "$(realpath --relative-to="$set_temp" "$previous_image")" \
            "$set_temp/previous.AppImage" || return
        install -m 0600 "$previous_manifest" \
            "$set_temp/previous.AppImage.manifest" || return
        install -m 0644 "$previous_sbom" \
            "$set_temp/previous.AppImage.sbom.cdx.json" || return
        verify_appimage_manifest \
            "$set_temp/latest.AppImage" \
            "$set_temp/latest.AppImage.manifest" || return
        verify_appimage_manifest \
            "$set_temp/previous.AppImage" \
            "$set_temp/previous.AppImage.manifest" || return
        verify_appimage_sbom \
            "$set_temp/latest.AppImage" \
            "$set_temp/latest.AppImage.sbom.cdx.json" || return
        verify_appimage_sbom \
            "$set_temp/previous.AppImage" \
            "$set_temp/previous.AppImage.sbom.cdx.json" || return
        mv -- "$set_temp" "$set_dir" || return
        set_temp=
    fi
    [ -d "$set_dir" ] && [ ! -L "$set_dir" ] || return 1
    verify_appimage_manifest \
        "$set_dir/latest.AppImage" "$set_dir/latest.AppImage.manifest" ||
        return
    verify_appimage_manifest \
        "$set_dir/previous.AppImage" "$set_dir/previous.AppImage.manifest" ||
        return
    verify_appimage_sbom \
        "$set_dir/latest.AppImage" \
        "$set_dir/latest.AppImage.sbom.cdx.json" || return
    verify_appimage_sbom \
        "$set_dir/previous.AppImage" \
        "$set_dir/previous.AppImage.sbom.cdx.json" || return
    [ "$(sed -n 's/^appimage_sha256=//p' \
        "$set_dir/latest.AppImage.manifest")" = "$image_hash" ] || return 1
    [ "$(sed -n 's/^appimage_sha256=//p' \
        "$set_dir/previous.AppImage.manifest")" = "$current_hash" ] ||
        return 1

    sync -f "$store" || return
    pointer_temp="$release_parent/.${release_name}.tmp.${BASHPID}.${RANDOM}"
    ln -s ".${release_name}.store/sets/$set_id" "$pointer_temp" || return
    mv -Tf -- "$pointer_temp" "$release_link" || return
    pointer_temp=
    if ! sync -f "$release_parent" ||
       ! verify_appimage_manifest \
            "$release_link/latest.AppImage" \
            "$release_link/latest.AppImage.manifest" ||
       ! verify_appimage_manifest \
            "$release_link/previous.AppImage" \
            "$release_link/previous.AppImage.manifest" ||
       ! verify_appimage_sbom \
            "$release_link/latest.AppImage" \
            "$release_link/latest.AppImage.sbom.cdx.json" ||
       ! verify_appimage_sbom \
            "$release_link/previous.AppImage" \
            "$release_link/previous.AppImage.sbom.cdx.json"; then
        echo "AppImage promotion failed after pointer publication; rolling back." >&2
        if [ -n "$old_pointer" ]; then
            pointer_temp="$release_parent/.${release_name}.rollback.${BASHPID}.${RANDOM}"
            ln -s "$old_pointer" "$pointer_temp" &&
                mv -Tf -- "$pointer_temp" "$release_link" || return 1
            pointer_temp=
        else
            rm -f -- "$release_link" || return
        fi
        sync -f "$release_parent" || return
        return 1
    fi
    printf '%s\n' "$release_link/latest.AppImage"
)

write_source_revision()
{
    local output=$1
    local revision=${GC_SOURCE_REVISION:-}

    if [ -n "$revision" ]; then
        if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
            echo "GC_SOURCE_REVISION must be a full lowercase Git commit hash" >&2
            return 1
        fi
        printf 'commit %s\n' "$revision" >>"$output"
    elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git log -1 >>"$output"
    else
        echo "Set GC_SOURCE_REVISION when packaging exported source" >&2
        return 1
    fi
}

install_embedded_python()
(
    if [ "$#" -ne 3 ]; then
        echo "Usage: install_embedded_python LOCK APPDIR PIP_REPORT" >&2
        return 2
    fi

    local requirements=$1
    local appdir=$2
    local python_install_report=$3
    local work_dir= requirements_path appdir_path report_path
    local runtime_manifest_path wheel_manifest_path wheelhouse

    [ -f "$requirements" ] && [ ! -L "$requirements" ] || return 1
    requirements_path=$(readlink -f -- "$requirements") || return
    [ -f "$requirements_path" ] || return 1
    [ -d "$appdir" ] && [ ! -L "$appdir" ] || return 1
    appdir_path=$(cd -- "$appdir" && pwd -P) || return
    local payload_root
    for payload_root in "$appdir_path/usr" "$appdir_path/opt"; do
        if [ -L "$payload_root" ] ||
           { [ -e "$payload_root" ] && [ ! -d "$payload_root" ]; }; then
            echo "Refusing an unsafe embedded Python payload root." >&2
            return 1
        fi
        if [ -d "$payload_root" ] &&
           [ -n "$(find "$payload_root" -type l -print -quit)" ]; then
            echo "Refusing a linked embedded Python payload tree." >&2
            return 1
        fi
    done
    [ -f "$python_install_report" ] &&
        [ ! -L "$python_install_report" ] || return 1
    report_path=$(readlink -f -- "$python_install_report") || return
    runtime_manifest_path="${report_path}.runtime-libraries.json"
    wheel_manifest_path="${report_path}.wheel-records.json"
    if [ -L "$runtime_manifest_path" ] ||
       { [ -e "$runtime_manifest_path" ] &&
         [ ! -f "$runtime_manifest_path" ]; }; then
        echo "Refusing an unsafe embedded Python runtime manifest." >&2
        return 1
    fi
    if [ -L "$wheel_manifest_path" ] ||
       { [ -e "$wheel_manifest_path" ] &&
         [ ! -f "$wheel_manifest_path" ]; }; then
        echo "Refusing an unsafe embedded Python wheel manifest." >&2
        return 1
    fi
    work_dir=$(mktemp -d) || return
    cleanup_embedded_python()
    {
        if [ -n "$work_dir" ] && [ -d "$work_dir" ]; then
            rm -rf -- "$work_dir"
        fi
    }
    trap cleanup_embedded_python EXIT
    cd "$work_dir" || return

    download_verified_file \
        "$PYTHON_APPIMAGE_URL" "$PYTHON_APPIMAGE_FILE" \
        "$PYTHON_APPIMAGE_SHA256"
    chmod +x "$PYTHON_APPIMAGE_FILE"

    "./$PYTHON_APPIMAGE_FILE" --appimage-extract
    rm -f "$PYTHON_APPIMAGE_FILE"

    export PATH="$(pwd)/squashfs-root/usr/bin:$PATH"
    wheelhouse="$work_dir/wheelhouse"
    mkdir -m 0700 -- "$wheelhouse"
    PIP_CONFIG_FILE=/dev/null PIP_DISABLE_PIP_VERSION_CHECK=1 \
        PYTHONDONTWRITEBYTECODE=1 \
        pip download -q --isolated --disable-pip-version-check --no-input \
            --no-cache-dir --require-hashes --only-binary=:all: \
            --index-url=https://pypi.org/simple --dest "$wheelhouse" \
            -r "$requirements_path"

    [ -f "$GC_EMBEDDED_PYTHON_NORMALIZER" ] &&
        [ ! -L "$GC_EMBEDDED_PYTHON_NORMALIZER" ] || return 1
    PYTHONDONTWRITEBYTECODE=1 \
        squashfs-root/opt/python3.11/bin/python3.11 \
        "$GC_EMBEDDED_PYTHON_NORMALIZER" \
        --python-root \
        "$work_dir/squashfs-root/opt/python3.11" \
        --forbidden-prefix "$work_dir" \
        --payload-root "$work_dir/squashfs-root" \
        --runtime-manifest "$runtime_manifest_path" \
        --runtime-sha256 "$PYTHON_APPIMAGE_SHA256" \
        --wheelhouse "$wheelhouse" \
        --requirements-lock "$requirements_path" \
        --wheel-manifest "$wheel_manifest_path" \
        --remove-locked-source-distributions

    PIP_CONFIG_FILE=/dev/null PIP_DISABLE_PIP_VERSION_CHECK=1 \
        PYTHONDONTWRITEBYTECODE=1 \
        pip install -q --isolated --disable-pip-version-check --no-input \
            --no-cache-dir --no-compile --ignore-installed --require-hashes \
            --only-binary=:all: --no-index --find-links "$wheelhouse" \
            --report "$report_path" -r "$requirements_path"

    PYTHONDONTWRITEBYTECODE=1 \
        squashfs-root/opt/python3.11/bin/python3.11 \
        "$GC_EMBEDDED_PYTHON_NORMALIZER" \
        --python-root \
        "$work_dir/squashfs-root/opt/python3.11" \
        --forbidden-prefix "$work_dir"

    mkdir -p "$appdir_path/usr" "$appdir_path/opt"
    cp -a squashfs-root/usr/. "$appdir_path/usr/"
    cp -a squashfs-root/opt/. "$appdir_path/opt/"
)
