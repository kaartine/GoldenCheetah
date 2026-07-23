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
