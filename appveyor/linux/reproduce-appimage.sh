#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "Usage: reproduce-appimage.sh SOURCE_ROOT EMPTY_OUTPUT_ROOT" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
SOURCE_ROOT=$(cd -- "$1" && pwd -P)
OUTPUT_ROOT=$2
BUILD_PASS=${GC_APPIMAGE_BUILD_PASS_SCRIPT:-$SCRIPT_DIR/build-appimage-pass.sh}
PACKAGE_PASS=${GC_APPIMAGE_PACKAGE_PASS_SCRIPT:-$SCRIPT_DIR/package-appimage-pass.sh}
SUPPORT="$SCRIPT_DIR/../../src/Resources/linux/AppImagePackagingSupport.sh"
WORK_ROOT=
PASS_SOURCES=()

cleanup_reproduction_worktrees()
{
    local source
    for source in "${PASS_SOURCES[@]}"; do
        run_reproducible_git -C "$SOURCE_ROOT" worktree remove --force "$source" \
            >/dev/null 2>&1 || true
    done
    if [ -n "$WORK_ROOT" ]; then
        rm -rf -- "$WORK_ROOT"
    fi
}
trap cleanup_reproduction_worktrees EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ -f "$SUPPORT" ] && [ ! -L "$SUPPORT" ] || exit 1

# shellcheck source=/dev/null
. "$SUPPORT"

[ -x "$BUILD_PASS" ] && [ ! -L "$BUILD_PASS" ] || {
    echo "Independent AppImage build pass is unavailable or unsafe." >&2
    exit 1
}
[ -x "$PACKAGE_PASS" ] && [ ! -L "$PACKAGE_PASS" ] || {
    echo "AppImage package pass is unavailable or unsafe." >&2
    exit 1
}
if [ -e "$OUTPUT_ROOT" ]; then
    [ -d "$OUTPUT_ROOT" ] && [ ! -L "$OUTPUT_ROOT" ] &&
        [ -z "$(find -P "$OUTPUT_ROOT" -mindepth 1 -print -quit)" ] || {
        echo "Reproduction output directory must be empty." >&2
        exit 1
    }
else
    mkdir -p -- "$OUTPUT_ROOT"
fi
OUTPUT_ROOT=$(cd -- "$OUTPUT_ROOT" && pwd -P)

REVISION=$(run_reproducible_git -C "$SOURCE_ROOT" rev-parse --verify HEAD)
[[ "$REVISION" =~ ^[0-9a-f]{40}$ ]] || exit 1
if [ -n "$(run_reproducible_git -C "$SOURCE_ROOT" status --porcelain=v1 \
    --untracked-files=normal --ignore-submodules=none)" ]; then
    echo "Reproducible AppImage builds require a clean source worktree." >&2
    exit 1
fi

WORK_ROOT=$(mktemp -d)
for label in one two; do
    pass_source="$WORK_ROOT/source-$label"
    pass_build="$WORK_ROOT/build-$label"
    pass_output="$WORK_ROOT/package-$label"
    run_reproducible_git -C "$SOURCE_ROOT" worktree add --quiet --detach \
        "$pass_source" "$REVISION"
    PASS_SOURCES+=("$pass_source")
    mkdir -p -- "$pass_build" "$pass_output"
    "$BUILD_PASS" "$pass_source" "$pass_build" "$SOURCE_ROOT"
done

ELF_ONE="$WORK_ROOT/build-one/src/GoldenCheetah"
ELF_TWO="$WORK_ROOT/build-two/src/GoldenCheetah"
[ -f "$ELF_ONE" ] && [ ! -L "$ELF_ONE" ] &&
    [ -f "$ELF_TWO" ] && [ ! -L "$ELF_TWO" ] || {
    echo "Independent GoldenCheetah ELF output is missing or unsafe." >&2
    exit 1
}
ELF_ONE_SHA256=$(sha256sum "$ELF_ONE" | cut -d ' ' -f 1)
ELF_TWO_SHA256=$(sha256sum "$ELF_TWO" | cut -d ' ' -f 1)
if [ "$ELF_ONE_SHA256" != "$ELF_TWO_SHA256" ] ||
   ! cmp -s -- "$ELF_ONE" "$ELF_TWO"; then
    echo "independent GoldenCheetah ELF mismatch:" \
        "$ELF_ONE_SHA256 != $ELF_TWO_SHA256" >&2
    exit 1
fi

for label in one two; do
    pass_source="$WORK_ROOT/source-$label"
    pass_build="$WORK_ROOT/build-$label"
    pass_output="$WORK_ROOT/package-$label"
    GC_APPIMAGE_REPOSITORY_ROOT="$pass_source" \
        GC_APPIMAGE_BINARY="$pass_build/src/GoldenCheetah" \
        "$PACKAGE_PASS" "$pass_output"
done

compare_appimage_reproduction \
    "$WORK_ROOT/package-one" "$WORK_ROOT/package-two"
install -m 0755 "$WORK_ROOT/package-one/GoldenCheetah.AppImage" \
    "$OUTPUT_ROOT/GoldenCheetah.AppImage"
install -m 0600 "$WORK_ROOT/package-one/GoldenCheetah.AppImage.manifest" \
    "$OUTPUT_ROOT/GoldenCheetah.AppImage.manifest"
install -m 0644 "$WORK_ROOT/package-one/GoldenCheetah.AppImage.sbom.cdx.json" \
    "$OUTPUT_ROOT/GoldenCheetah.AppImage.sbom.cdx.json"
install -m 0600 "$WORK_ROOT/package-one/build.manifest" \
    "$OUTPUT_ROOT/build.manifest"
