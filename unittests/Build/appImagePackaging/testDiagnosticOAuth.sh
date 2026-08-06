#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd -P)
SUPPORT="$REPOSITORY_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
TEMPORARY=$(mktemp -d)
trap 'rm -rf -- "$TEMPORARY"' EXIT

# shellcheck source=/dev/null
. "$SUPPORT"

cat >"$TEMPORARY/status.c" <<'EOF'
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--goldencheetah-build-status") != 0) {
        return 64;
    }
    fputs(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n"
        "strava_support=enabled\n"
        "strava_oauth=" GC_OAUTH_STATUS "\n",
        stdout
    );
    return 0;
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_OAUTH_STATUS='"configured"' "$TEMPORARY/status.c" \
    -o "$TEMPORARY/configured"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_OAUTH_STATUS='"unavailable"' "$TEMPORARY/status.c" \
    -o "$TEMPORARY/unconfigured"

require_strava_oauth_build "$TEMPORARY/configured" >/dev/null
if require_strava_oauth_build "$TEMPORARY/unconfigured" >/dev/null 2>&1; then
    echo "private build gate accepted unavailable OAuth" >&2
    exit 1
fi
require_unconfigured_strava_oauth_build "$TEMPORARY/unconfigured" >/dev/null
if require_unconfigured_strava_oauth_build "$TEMPORARY/configured" \
    >/dev/null 2>&1; then
    echo "diagnostic build gate accepted embedded OAuth credentials" >&2
    exit 1
fi

printf '\177ELF\000\000\000\000AI\002fixture\n' >"$TEMPORARY/fixture.AppImage"
chmod +x "$TEMPORARY/fixture.AppImage"
GC_TEST_OAUTH_ENTRY="$TEMPORARY/unconfigured"
trusted_appimage_extract()
{
    local image=$1
    local destination=$2
    local app_root="$destination/squashfs-root"

    [ "$image" = "$TEMPORARY/fixture.AppImage" ] || return 64
    [ -d "$destination" ] &&
        [ -z "$(find -P "$destination" -mindepth 1 -print -quit)" ] ||
        return 64
    mkdir -p "$app_root"
    cp "$GC_TEST_OAUTH_ENTRY" "$app_root/GoldenCheetah"
    ln -s GoldenCheetah "$app_root/AppRun"
}

require_unconfigured_strava_oauth_appimage \
    "$TEMPORARY/fixture.AppImage" >/dev/null
if require_strava_oauth_appimage "$TEMPORARY/fixture.AppImage" \
    >/dev/null 2>&1; then
    echo "private AppImage gate accepted unavailable OAuth" >&2
    exit 1
fi
GC_TEST_OAUTH_ENTRY="$TEMPORARY/configured"
require_strava_oauth_appimage "$TEMPORARY/fixture.AppImage" >/dev/null
if require_unconfigured_strava_oauth_appimage \
    "$TEMPORARY/fixture.AppImage" >/dev/null 2>&1; then
    echo "diagnostic AppImage gate accepted embedded OAuth credentials" >&2
    exit 1
fi

echo "PASS: diagnostic and private OAuth package gates are distinct"
