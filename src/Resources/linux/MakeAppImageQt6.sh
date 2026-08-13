#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
repo_root=$(cd -- "$script_dir/../../.." && pwd -P)
reproduce="$repo_root/appveyor/linux/reproduce-appimage.sh"
output=${OUTPUT:-$repo_root/src/GoldenCheetah_v3.8_x64Qt6.AppImage}
version_file=${VERSION_FILE:-$repo_root/src/GCversionLinuxQt6.txt}
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
[ -f "$repo_root/src/gcconfig.pri" ] &&
    [ ! -L "$repo_root/src/gcconfig.pri" ] || {
    echo "Effective src/gcconfig.pri is required." >&2
    exit 1
}

# shellcheck source=/dev/null
. "$script_dir/AppImagePackagingSupport.sh"
mkdir -p -- "$(dirname -- "$output")"
reproduction_output=$(mktemp -d)
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

run_packaging_appimage "$output" --version 2>"$version_file"
write_source_revision "$version_file"
printf 'SHA256 hash of %s:\n' "$(basename -- "$output")" >>"$version_file"
sha256sum "$output" | cut -d ' ' -f 1 >>"$version_file"
verify_appimage_manifest "$output" "$output.manifest"
verify_appimage_sbom "$output" "$output.sbom.cdx.json"
cat "$version_file"
