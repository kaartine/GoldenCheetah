#!/usr/bin/env bash

PYTHON_APPIMAGE_SERIES="3.11"
PYTHON_APPIMAGE_VERSION="3.11.15"
PYTHON_APPIMAGE_ABI="cp311"
PYTHON_APPIMAGE_PLATFORM="manylinux2014_x86_64"
PYTHON_APPIMAGE_SHA256="2d8ecd8002fae06813d4c92ba5244f573aae9bf84eaf41a1b189b623112e3dec"
PYTHON_APPIMAGE_FILE="python${PYTHON_APPIMAGE_VERSION}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_PLATFORM}.AppImage"
PYTHON_APPIMAGE_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${PYTHON_APPIMAGE_FILE}"

LINUXDEPLOYQT_FILE="linuxdeployqt-continuous-x86_64.AppImage"
LINUXDEPLOYQT_URL="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/${LINUXDEPLOYQT_FILE}"
APPIMAGETOOL_FILE="appimagetool-x86_64.AppImage"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/${APPIMAGETOOL_FILE}"
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

run_packaging_appimage()
{
    APPIMAGE_EXTRACT_AND_RUN=1 "$@"
}

run_packaged_appimage_smoke()
{
    APPIMAGE_EXTRACT_AND_RUN=1 timeout "$@"
}

strava_oauth_build_status()
(
    local executable=$1
    local status_home=

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
    local elf_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        echo "Strava OAuth build status requires an ELF executable." >&2
        return 1
    fi
    local appimage_magic
    appimage_magic=$(dd if="$executable" bs=1 skip=8 count=3 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$appimage_magic" = "414901" ] ||
       [ "$appimage_magic" = "414902" ]; then
        echo "Cannot inspect Strava OAuth configuration in a compressed AppImage;" \
            "inspect its extracted GoldenCheetah executable." >&2
        return 1
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
            LC_ALL=C timeout --signal=TERM --kill-after=2s 10s \
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
    local configured_report unavailable_report
    configured_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=configured"
        printf x
    )
    configured_report=${configured_report%x}
    unavailable_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=unavailable"
        printf x
    )
    unavailable_report=${unavailable_report%x}
    if [ "$report" = "$configured_report" ]; then
        echo "Strava OAuth: configured"
        return
    fi
    if [ "$report" = "$unavailable_report" ]; then
        echo "Strava OAuth: unavailable (credentials not configured)"
        return
    fi
    echo "GoldenCheetah returned an invalid build-status report." >&2
    return 1
)

require_strava_oauth_build()
{
    local executable=$1
    local status

    status=$(strava_oauth_build_status "$executable") || return
    if [ "$status" != "Strava OAuth: configured" ]; then
        echo "$status" >&2
        echo "Refusing to package a release without configured" \
            "Strava OAuth credentials." >&2
        return 1
    fi
    echo "$status"
}

strava_oauth_appimage_status()
(
    local image=$1
    local extract_dir=

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

    local image_dir image_name image_path app_root
    local entrypoint status
    image_dir=$(cd -- "$(dirname -- "$image")" && pwd) || return
    image_name=$(basename -- "$image") || return
    image_path="$image_dir/$image_name"
    extract_dir=$(mktemp -d) || return
    app_root="$extract_dir/squashfs-root"

    if (
        cd "$extract_dir" || exit
        run_packaging_appimage \
            "$image_path" --appimage-extract \
            >/dev/null 2>&1 || exit
    ); then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        return "$status"
    fi

    entrypoint=$(readlink -f "$app_root/AppRun") || return
    case "$entrypoint" in
    "$app_root"/*) ;;
    *)
        echo "AppImage AppRun resolves outside its payload." >&2
        return 1
        ;;
    esac
    strava_oauth_build_status "$app_root/AppRun"
)

require_strava_oauth_appimage()
{
    local image=$1
    local status

    status=$(strava_oauth_appimage_status "$image") || return
    if [ "$status" != "Strava OAuth: configured" ]; then
        echo "$status" >&2
        echo "Refusing to publish an AppImage without configured" \
            "Strava OAuth credentials." >&2
        return 1
    fi
    echo "$status"
}

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
{
    local requirements=$1
    local appdir=$2

    download_file "$PYTHON_APPIMAGE_URL" "$PYTHON_APPIMAGE_FILE"
    printf '%s  %s\n' "$PYTHON_APPIMAGE_SHA256" "$PYTHON_APPIMAGE_FILE" |
        sha256sum --check -
    chmod +x "$PYTHON_APPIMAGE_FILE"

    rm -rf squashfs-root
    "./$PYTHON_APPIMAGE_FILE" --appimage-extract
    rm -f "$PYTHON_APPIMAGE_FILE"

    export PATH="$(pwd)/squashfs-root/usr/bin:$PATH"
    pip install --upgrade pip
    pip install -q -r "$requirements"

    mv squashfs-root/usr "$appdir/usr"
    mv squashfs-root/opt "$appdir/opt"
    rm -rf squashfs-root
}
