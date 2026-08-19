#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 3 ]; then
    echo "Usage: build-appimage-pass.sh SOURCE_TREE BUILD_TREE INPUT_SOURCE" >&2
    exit 2
fi

SOURCE_TREE=$(cd -- "$1" && pwd -P)
BUILD_TREE=$2
INPUT_SOURCE=$(cd -- "$3" && pwd -P)
QMAKE_COMMAND=${GC_APPIMAGE_QMAKE:-qmake}
BUILD_JOBS=${GC_BUILD_JOBS:-2}
SUPPORT="$SOURCE_TREE/src/Resources/linux/AppImagePackagingSupport.sh"
QMAKE_CACHE_ARGUMENTS=()

[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || {
    echo "GC_BUILD_JOBS must be a positive integer." >&2
    exit 1
}
[ -d "$BUILD_TREE" ] && [ ! -L "$BUILD_TREE" ] &&
    [ -z "$(find -P "$BUILD_TREE" -mindepth 1 -print -quit)" ] || {
    echo "Independent AppImage build directory must be empty." >&2
    exit 1
}
[ -f "$SUPPORT" ] && [ ! -L "$SUPPORT" ] || {
    echo "AppImage packaging support is unavailable." >&2
    exit 1
}

# shellcheck source=/dev/null
. "$SUPPORT"
BUILD_TREE=$(cd -- "$BUILD_TREE" && pwd -P)
QMAKE_COMMAND=$(command -v -- "$QMAKE_COMMAND") || {
    echo "qmake is unavailable." >&2
    exit 1
}
MAKE_COMMAND=$(command -v make) || {
    echo "make is unavailable." >&2
    exit 1
}
if [ -n "${GC_APPIMAGE_CCACHE_DIR:-}" ]; then
    CCACHE_COMMAND=$(PATH=/usr/local/bin:/usr/bin:/bin command -v ccache) || {
        echo "ccache was requested but is unavailable." >&2
        exit 1
    }
    QMAKE_CACHE_ARGUMENTS=(
        "QMAKE_CC=$CCACHE_COMMAND gcc"
        "QMAKE_CXX=$CCACHE_COMMAND g++"
        "QMAKE_CXXFLAGS+=-fpch-preprocess"
    )
fi
QT_ROOT=$(cd -- "$(dirname -- "$QMAKE_COMMAND")/.." && pwd -P)
BUILD_HOME="$BUILD_TREE/.build-home"
BUILD_TMP="$BUILD_TREE/.build-tmp"
mkdir -m 0700 -- "$BUILD_HOME" "$BUILD_TMP"

install_reproducible_build_inputs "$INPUT_SOURCE" "$SOURCE_TREE"

REVISION=$(run_reproducible_git -C "$SOURCE_TREE" rev-parse --verify HEAD)
SOURCE_DATE_EPOCH=$(run_reproducible_git -C "$SOURCE_TREE" show -s --format=%ct "$REVISION")
[[ "$REVISION" =~ ^[0-9a-f]{40}$ ]] &&
    [[ "$SOURCE_DATE_EPOCH" =~ ^[1-9][0-9]*$ ]] || exit 1
export GC_SOURCE_REVISION="$REVISION" SOURCE_DATE_EPOCH
BUILD_INPUTS=$(run_reproducible_build_tool \
    "$QT_ROOT" "$BUILD_HOME" "$BUILD_TMP" \
    python3 \
    "$SOURCE_TREE/src/Resources/linux/compute-build-input-identity.py" \
    "$SOURCE_TREE")
[[ "$BUILD_INPUTS" =~ ^[0-9a-f]{64}$ ]] || exit 1
PREFIX_MAP_FLAGS="-ffile-prefix-map=$SOURCE_TREE=/usr/src/goldencheetah -fdebug-prefix-map=$SOURCE_TREE=/usr/src/goldencheetah -fmacro-prefix-map=$SOURCE_TREE=/usr/src/goldencheetah -ffile-prefix-map=$BUILD_TREE=/usr/src/goldencheetah-build -fdebug-prefix-map=$BUILD_TREE=/usr/src/goldencheetah-build"

(
    cd "$BUILD_TREE"
    run_reproducible_build_tool \
        "$QT_ROOT" "$BUILD_HOME" "$BUILD_TMP" \
        "$QMAKE_COMMAND" "$SOURCE_TREE/build.pro" -r \
        QMAKE_CXXFLAGS_WARN_ON+="-Wno-unused-private-field -Wno-c++11-narrowing -Wno-deprecated-declarations -Wno-deprecated-register -Wno-nullability-completeness -Wno-sign-compare -Wno-inconsistent-missing-override" \
        QMAKE_CFLAGS_WARN_ON+="-Wno-deprecated-declarations -Wno-sign-compare" \
        QMAKE_CFLAGS+="$PREFIX_MAP_FLAGS" \
        QMAKE_CXXFLAGS+="$PREFIX_MAP_FLAGS" \
        "${QMAKE_CACHE_ARGUMENTS[@]}"
    run_reproducible_build_tool \
        "$QT_ROOT" "$BUILD_HOME" "$BUILD_TMP" \
        "$MAKE_COMMAND" -j"$BUILD_JOBS" sub-qwt
    run_reproducible_build_tool \
        "$QT_ROOT" "$BUILD_HOME" "$BUILD_TMP" \
        "$MAKE_COMMAND" -j"$BUILD_JOBS" sub-src
)

[ -x "$BUILD_TREE/src/GoldenCheetah" ] || {
    echo "Independent GoldenCheetah build output is missing." >&2
    exit 1
}
