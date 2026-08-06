#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../.." && pwd)
SUPPORT="$REPO_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
SECRETS_SCRIPT="$REPO_ROOT/util/add_secrets.ps1"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

for workflow in "$REPO_ROOT"/.github/workflows/*.yml \
                "$REPO_ROOT"/.github/workflows/*.yaml \
                "$REPO_ROOT/appveyor.yml"; do
    [ -e "$workflow" ] || continue
    if grep -Fq 'GC_STRAVA_CLIENT_SECRET' "$workflow"; then
        fail "public release workflow injects GC_STRAVA_CLIENT_SECRET: $workflow"
    fi
    if grep -Fq 'IncludePrivateStravaCredentials' "$workflow"; then
        fail "public release workflow enables private Strava credentials: $workflow"
    fi
done

grep -Fq 'IncludePrivateStravaCredentials' "$SECRETS_SCRIPT" ||
    fail "personal-build Strava credentials do not require an explicit switch"

TEMP_DIR=$(mktemp -d)
trap 'rm -rf -- "$TEMP_DIR"' EXIT
cat >"$TEMP_DIR/runtime-only.c" <<'EOF'
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(
            argv[1], "--goldencheetah-build-status") != 0) {
        return 64;
    }
    fputs(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n"
        "strava_support=enabled\n"
        "strava_oauth=runtime_credentials\n"
        "strava_compile_fallback=unavailable\n",
        stdout);
    return 0;
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    "$TEMP_DIR/runtime-only.c" -o "$TEMP_DIR/runtime-only"

# shellcheck source=/dev/null
. "$SUPPORT"
[ "$(require_strava_oauth_build "$TEMP_DIR/runtime-only")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime-only public build was rejected by the release gate"

echo "PASS: public releases use runtime-only Strava credentials"
