#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd -- "$script_dir/.." && pwd -P)
build_dir=${BUILD_DIR:-$repo_root/build-devcontainer}
output=${OUTPUT:-$build_dir/GoldenCheetah-BLE-PoC-x86_64.AppImage}
qt_dir=${QTDIR:-/opt/Qt/6.8.3/gcc_64}
reproduce="$repo_root/appveyor/linux/reproduce-appimage.sh"
reproduction_output=

cleanup_reproduction()
{
    if [ -n "$reproduction_output" ]; then
        rm -rf -- "$reproduction_output"
    fi
}
trap cleanup_reproduction EXIT

[ -x "$reproduce" ] && [ ! -L "$reproduce" ] || {
    echo "AppImage reproduction driver is unavailable or unsafe." >&2
    exit 1
}
[ -x "$qt_dir/bin/qmake" ] || {
    echo "Qt installation not found: $qt_dir" >&2
    exit 1
}
[ -f "$repo_root/src/gcconfig.pri" ] &&
    [ ! -L "$repo_root/src/gcconfig.pri" ] || {
    echo "Effective src/gcconfig.pri is required." >&2
    exit 1
}

# shellcheck source=/dev/null
. "$repo_root/src/Resources/linux/AppImagePackagingSupport.sh"
mkdir -p -- "$build_dir" "$(dirname -- "$output")"
reproduction_output=$(mktemp -d "$build_dir/appimage-reproduction.XXXXXX")
export QTDIR="$qt_dir"
export PATH="$qt_dir/bin:$PATH"
GC_APPIMAGE_QMAKE="$qt_dir/bin/qmake" \
GC_APPIMAGE_OAUTH_POLICY=configured \
    "$reproduce" "$repo_root" "$reproduction_output"

install -m 0755 "$reproduction_output/GoldenCheetah.AppImage" "$output"
install -m 0600 "$reproduction_output/GoldenCheetah.AppImage.manifest" \
    "$output.manifest"
install -m 0644 \
    "$reproduction_output/GoldenCheetah.AppImage.sbom.cdx.json" \
    "$output.sbom.cdx.json"
verify_appimage_manifest "$output" "$output.manifest"
verify_appimage_sbom "$output" "$output.sbom.cdx.json"

if [ -n "${PROMOTION_LINK:-}" ]; then
    promoted_image=$(promote_appimage_release \
        "$output" "$output.manifest" "$output.sbom.cdx.json" \
        "$PROMOTION_LINK")
    echo "Promoted AppImage: $promoted_image"
fi

echo "AppImage: $output"
echo "Manifest: $output.manifest"
echo "SBOM: $output.sbom.cdx.json"
