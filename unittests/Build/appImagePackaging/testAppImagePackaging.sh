#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
SUPPORT="$REPO_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
LOCAL_PACKAGER="$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh"
CI_PACKAGER="$REPO_ROOT/appveyor/linux/after_build.sh"
REQUIREMENTS="$REPO_ROOT/src/Python/requirements.txt"
DEV_CONFIG="$REPO_ROOT/.devcontainer/gcconfig.pri"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains()
{
    local file=$1
    local pattern=$2
    grep -Fq "$pattern" "$file" ||
        fail "$file does not contain: $pattern"
}

[ -r "$SUPPORT" ] || fail "missing shared AppImage packaging support"

# shellcheck source=/dev/null
. "$SUPPORT"

[ "$PYTHON_APPIMAGE_SERIES" = "3.11" ] ||
    fail "embedded Python must remain on the supported 3.11 series"
[ "$PYTHON_APPIMAGE_VERSION" = "3.11.15" ] ||
    fail "unexpected embedded Python patch release"
[ "$PYTHON_APPIMAGE_ABI" = "cp311" ] ||
    fail "embedded Python ABI does not match Python 3.11"
[ "$PYTHON_APPIMAGE_PLATFORM" = "manylinux2014_x86_64" ] ||
    fail "embedded Python must retain the glibc-compatible manylinux2014 build"
[ "$PYTHON_APPIMAGE_FILE" = \
  "python3.11.15-cp311-cp311-manylinux2014_x86_64.AppImage" ] ||
    fail "embedded Python filename is inconsistent"
[ "$PYTHON_APPIMAGE_SHA256" = \
  "2d8ecd8002fae06813d4c92ba5244f573aae9bf84eaf41a1b189b623112e3dec" ] ||
    fail "embedded Python SHA-256 is not the reviewed release digest"
[ "$PYTHON_APPIMAGE_URL" = \
  "https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$PYTHON_APPIMAGE_FILE" ] ||
    fail "embedded Python URL is not the project-controlled immutable asset"

declare -F download_file >/dev/null || fail "download_file helper is missing"
declare -F run_packaging_appimage >/dev/null ||
    fail "run_packaging_appimage helper is missing"
declare -F run_packaged_appimage_smoke >/dev/null ||
    fail "run_packaged_appimage_smoke helper is missing"
declare -F install_embedded_python >/dev/null ||
    fail "install_embedded_python helper is missing"
declare -F write_source_revision >/dev/null ||
    fail "write_source_revision helper is missing"
declare -F strava_oauth_build_status >/dev/null ||
    fail "strava_oauth_build_status helper is missing"
declare -F require_strava_oauth_build >/dev/null ||
    fail "require_strava_oauth_build helper is missing"

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT
printf '#!/bin/sh\nprintf "%%s" "$APPIMAGE_EXTRACT_AND_RUN"\n' \
    >"$TEMP_DIR/check-extract-mode"
chmod +x "$TEMP_DIR/check-extract-mode"
[ "$(run_packaged_appimage_smoke 2s \
      "$TEMP_DIR/check-extract-mode")" = "1" ] ||
    fail "packaged AppImage smoke did not use extraction mode"

REVISION=0123456789abcdef0123456789abcdef01234567
GC_SOURCE_REVISION=$REVISION write_source_revision "$TEMP_DIR/revision"
grep -Fxq "commit $REVISION" "$TEMP_DIR/revision" ||
    fail "explicit source revision was not recorded"
if GC_SOURCE_REVISION=invalid write_source_revision \
    "$TEMP_DIR/invalid" 2>/dev/null; then
    fail "invalid source revision was accepted"
fi
if (cd "$TEMP_DIR" && unset GC_SOURCE_REVISION &&
    write_source_revision missing 2>/dev/null); then
    fail "exported source without a revision was accepted"
fi

printf 'binary-prefix%s-binary-suffix' \
    "$STRAVA_CLIENT_SECRET_PLACEHOLDER" >"$TEMP_DIR/unconfigured"
for ((index = 0;
      index < ${#STRAVA_CLIENT_SECRET_PLACEHOLDER};
      ++index)); do
    printf '%s\0' "${STRAVA_CLIENT_SECRET_PLACEHOLDER:index:1}"
done >"$TEMP_DIR/unconfigured-utf16le"
printf 'binary-with-configured-credential' >"$TEMP_DIR/configured"
printf '12345678AI\002compressed-appimage-payload' \
    >"$TEMP_DIR/packaged.AppImage"
[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "placeholder Strava credentials were not reported unavailable"
[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured-utf16le")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "Qt UTF-16LE Strava placeholder was not reported unavailable"
[ "$(strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: configured" ] ||
    fail "configured Strava credentials were not reported"
if strava_oauth_build_status "$TEMP_DIR/missing" >/dev/null 2>&1; then
    fail "missing executable was accepted for Strava status inspection"
fi
if strava_oauth_build_status "$TEMP_DIR/packaged.AppImage" \
    >/dev/null 2>&1; then
    fail "compressed AppImage was incorrectly inspected as a raw executable"
fi
require_strava_oauth_build "$TEMP_DIR/configured" >/dev/null ||
    fail "configured Strava credentials were rejected by the release gate"
if require_strava_oauth_build "$TEMP_DIR/unconfigured" \
    >/dev/null 2>&1; then
    fail "release gate accepted placeholder Strava credentials"
fi
if require_strava_oauth_build "$TEMP_DIR/unconfigured-utf16le" \
    >/dev/null 2>&1; then
    fail "release gate accepted Qt UTF-16LE Strava placeholders"
fi
if require_strava_oauth_build "$TEMP_DIR/missing" \
    >/dev/null 2>&1; then
    fail "release gate accepted a missing executable"
fi

grep -Eq '^sip[[:space:]]*==[[:space:]]*6\.15\.1$' "$REQUIREMENTS" ||
    fail "test must be reviewed when the pinned SIP version changes"

for packager in "$LOCAL_PACKAGER" "$CI_PACKAGER"; do
    bash -n "$packager"
    assert_contains "$packager" \
        '. Resources/linux/AppImagePackagingSupport.sh'
    assert_contains "$packager" 'install_embedded_python'
    assert_contains "$packager" 'run_packaging_appimage'
    assert_contains "$packager" \
        'require_strava_oauth_build ./GoldenCheetah'
done

if grep -Fq 'python3.7' "$LOCAL_PACKAGER"; then
    fail "local AppImage packaging still embeds unsupported Python 3.7"
fi
assert_contains "$LOCAL_PACKAGER" 'write_source_revision'
assert_contains "$LOCAL_PACKAGER" 'run_packaged_appimage_smoke'
assert_contains "$LOCAL_PACKAGER" \
    'run_packaging_appimage "./$FINAL_NAME" --version'
assert_contains "$DEV_CONFIG" \
    '# DEFINES += GC_STRAVA_CLIENT_ID=\\\"your_client_id\\\"'
assert_contains "$DEV_CONFIG" \
    '# DEFINES += GC_STRAVA_CLIENT_SECRET=\\\"your_client_secret\\\"'

bash -n "$SUPPORT"
echo "PASS: AppImage Python runtime and packaging helpers are consistent"
