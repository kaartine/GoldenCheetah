#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
PACKAGE_PASS="$SCRIPT_DIR/package-appimage-pass.sh"
REPRODUCE_APPIMAGE="$SCRIPT_DIR/reproduce-appimage.sh"
REPRODUCTION_OUTPUT=$(mktemp -d)
IMAGE="$REPOSITORY_ROOT/GoldenCheetah_v3.8_x64.AppImage"
MANIFEST="$IMAGE.manifest"
SBOM="$IMAGE.sbom.cdx.json"

cleanup_reproduction()
{
    rm -rf -- "$REPRODUCTION_OUTPUT"
}
trap cleanup_reproduction EXIT

if [ ! -x "$REPRODUCE_APPIMAGE" ] || [ -L "$REPRODUCE_APPIMAGE" ]; then
    echo "Production AppImage reproduction driver is unavailable or unsafe." >&2
    exit 1
fi

# shellcheck source=/dev/null
. "$REPOSITORY_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"

rm -f -- "$IMAGE" "$MANIFEST" "$SBOM"
"$REPRODUCE_APPIMAGE" "$REPOSITORY_ROOT" "$REPRODUCTION_OUTPUT"

install -m 0755 "$REPRODUCTION_OUTPUT/GoldenCheetah.AppImage" "$IMAGE"
install -m 0600 "$REPRODUCTION_OUTPUT/GoldenCheetah.AppImage.manifest" "$MANIFEST"
install -m 0644 \
    "$REPRODUCTION_OUTPUT/GoldenCheetah.AppImage.sbom.cdx.json" "$SBOM"

verify_appimage_manifest "$IMAGE" "$MANIFEST"
verify_appimage_sbom "$IMAGE" "$SBOM"
