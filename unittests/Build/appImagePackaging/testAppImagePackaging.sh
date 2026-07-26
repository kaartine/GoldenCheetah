#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
SUPPORT="$REPO_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
LOCAL_PACKAGER="$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh"
CI_PACKAGER="$REPO_ROOT/appveyor/linux/after_build.sh"
DEV_PACKAGER="$REPO_ROOT/.devcontainer/package-appimage.sh"
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
declare -F strava_oauth_appimage_status >/dev/null ||
    fail "strava_oauth_appimage_status helper is missing"
declare -F require_strava_oauth_appimage >/dev/null ||
    fail "require_strava_oauth_appimage helper is missing"

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

cat >"$TEMP_DIR/status-probe.c" <<'EOF'
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2
        || strcmp(
            argv[1],
            "--goldencheetah-build-status") != 0) {
        return 64;
    }

    fputs(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n",
        stdout);
    if (strstr(argv[0], "no-strava") == NULL) {
        fputs("strava_support=enabled\n", stdout);
    }
    if (strstr(argv[0], "malformed") != NULL) {
        fputs("strava_oauth=maybe\n", stdout);
    } else if (strstr(argv[0], "unconfigured") != NULL) {
        fputs("strava_oauth=unavailable\n", stdout);
    } else {
        fputs("strava_oauth=configured\n", stdout);
    }
    return strstr(argv[0], "bad-exit") == NULL ? 0 : 1;
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    "$TEMP_DIR/status-probe.c" -o "$TEMP_DIR/configured"
cp "$TEMP_DIR/configured" "$TEMP_DIR/unconfigured"
cp "$TEMP_DIR/configured" "$TEMP_DIR/malformed"
cp "$TEMP_DIR/configured" "$TEMP_DIR/no-strava"
cp "$TEMP_DIR/configured" "$TEMP_DIR/bad-exit"

printf '\177ELF\002\001\001\000AI\001compressed-appimage-payload' \
    >"$TEMP_DIR/type1.AppImage"
printf '\177ELF\002\001\001\000AI\002compressed-appimage-payload' \
    >"$TEMP_DIR/type2.AppImage"
chmod +x "$TEMP_DIR/type1.AppImage" "$TEMP_DIR/type2.AppImage"

[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "runtime-unavailable Strava credentials were not reported unavailable"
[ "$(strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: configured" ] ||
    fail "configured Strava credentials were not reported"
if strava_oauth_build_status "$TEMP_DIR/missing" >/dev/null 2>&1; then
    fail "missing executable was accepted for Strava status inspection"
fi
if strava_oauth_build_status /bin/true >/dev/null 2>&1; then
    fail "an unrelated ELF executable was accepted as GoldenCheetah"
fi
if strava_oauth_build_status "$TEMP_DIR/malformed" \
    >/dev/null 2>&1; then
    fail "a malformed build-status response was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/no-strava" \
    >/dev/null 2>&1; then
    fail "a binary without reported Strava support was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/bad-exit" \
    >/dev/null 2>&1; then
    fail "a failed build-status command was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/status-probe.c" \
    >/dev/null 2>&1; then
    fail "a non-ELF file was accepted for Strava status inspection"
fi
if strava_oauth_build_status "$TEMP_DIR/type1.AppImage" \
    >/dev/null 2>&1; then
    fail "a Type 1 AppImage was inspected as a raw executable"
fi
if strava_oauth_build_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "a Type 2 AppImage was inspected as a raw executable"
fi
require_strava_oauth_build "$TEMP_DIR/configured" >/dev/null ||
    fail "configured Strava credentials were rejected by the release gate"
if require_strava_oauth_build "$TEMP_DIR/unconfigured" \
    >/dev/null 2>&1; then
    fail "release gate accepted unavailable Strava credentials"
fi
if require_strava_oauth_build "$TEMP_DIR/missing" \
    >/dev/null 2>&1; then
    fail "release gate accepted a missing executable"
fi

GC_TEST_APPIMAGE_INNER="$TEMP_DIR/configured"
run_packaging_appimage()
{
    [ "$2" = "--appimage-extract" ] ||
        fail "AppImage status did not request extraction"
    [ "$3" = "GoldenCheetah" ] ||
        fail "AppImage status extracted an unexpected payload"
    mkdir -p squashfs-root
    cp "$GC_TEST_APPIMAGE_INNER" \
        squashfs-root/GoldenCheetah
}

[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: configured" ] ||
    fail "configured packaged GoldenCheetah was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "configured packaged GoldenCheetah was rejected"

GC_TEST_APPIMAGE_INNER="$TEMP_DIR/unconfigured"
[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "unavailable packaged GoldenCheetah was not reported"
if require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "release gate accepted an unavailable packaged GoldenCheetah"
fi

GC_TEST_APPIMAGE_INNER="$TEMP_DIR/malformed"
if strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "malformed packaged GoldenCheetah status was accepted"
fi
if strava_oauth_appimage_status "$TEMP_DIR/type1.AppImage" \
    >/dev/null 2>&1; then
    fail "unsupported Type 1 AppImage extraction was accepted"
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

for packager in "$LOCAL_PACKAGER" "$CI_PACKAGER" "$DEV_PACKAGER"; do
    bash -n "$packager"
    assert_contains "$packager" \
        'require_strava_oauth_appimage'
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
