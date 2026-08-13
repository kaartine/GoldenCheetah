#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
APPVEYOR_CONFIG="$REPO_ROOT/appveyor.yml"
GITHUB_CONFIG="$REPO_ROOT/.github/workflows/ci.yml"
GITHUB_BUILD="$REPO_ROOT/.github/scripts/build.sh"
GITHUB_INSTALL="$REPO_ROOT/.github/scripts/install.sh"
GITHUB_PACKAGER="$REPO_ROOT/.github/scripts/after_build.sh"
LINUX_PACKAGER="$REPO_ROOT/appveyor/linux/after_build.sh"
LINUX_PACKAGE_PASS="$REPO_ROOT/appveyor/linux/package-appimage-pass.sh"
LINUX_INSTALL="$REPO_ROOT/appveyor/linux/install.sh"
LOCAL_PACKAGER="$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh"
DEV_PACKAGER="$REPO_ROOT/.devcontainer/package-appimage.sh"
DEV_DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"
WINDOWS_REGRESSIONS="$REPO_ROOT/appveyor/windows/run-build-regressions.ps1"
MACOS_REGRESSION="$REPO_ROOT/unittests/Build/appImagePackaging/testMacOSPackaging.py"
MACOS_PACKAGER="$REPO_ROOT/appveyor/macos/after_build.sh"
WINDOWS_PACKAGER="$REPO_ROOT/appveyor/windows/after_build.ps1"
MACOS_INSTALL="$REPO_ROOT/appveyor/macos/install.sh"
WINDOWS_INSTALL="$REPO_ROOT/appveyor/windows/install.ps1"
IMMUTABLE_ACTIONS_TEST="$SCRIPT_DIR/testImmutableActions.py"
APT_SNAPSHOT_TEST="$SCRIPT_DIR/testAptSnapshot.py"
RELEASE_HARDENING_TEST="$SCRIPT_DIR/testReleaseHardening.py"
IMMUTABLE_ACTIONS_CHECKER="$REPO_ROOT/.github/scripts/check-immutable-actions.py"
IMMUTABLE_ACTIONS_LOCK="$REPO_ROOT/.github/scripts/immutable-actions-requirements.lock"
WORKFLOW_POLICY="$REPO_ROOT/.github/workflows/workflow-policy.yml"
ARTIFACT_TRUST_DOCUMENT="$REPO_ROOT/doc/BUILD_ARTIFACT_AUTHENTICITY.md"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

MODE=${1:-full}
[ "$#" -le 1 ] || fail "usage: $0 [--portable]"
case "$MODE" in
    full)
        ;;
    --portable)
        MODE=portable
        ;;
    *)
        fail "usage: $0 [--portable]"
        ;;
esac

assert_contains()
{
    local file=$1
    local pattern=$2
    grep -Fq -- "$pattern" "$file" ||
        fail "$file does not contain: $pattern"
}

POLICY_PYTHONPATH=""
cleanup_policy_python()
{
    if [ -n "$POLICY_PYTHONPATH" ]; then
        rm -rf -- "$POLICY_PYTHONPATH"
    fi
}
trap cleanup_policy_python EXIT

if ! python3 -c 'import yaml' >/dev/null 2>&1; then
    POLICY_PYTHONPATH=$(mktemp -d)
    python3 -m pip install \
        --isolated --disable-pip-version-check --no-input --no-cache-dir \
        --index-url https://pypi.org/simple \
        --no-deps --only-binary=:all: --require-hashes \
        --target "$POLICY_PYTHONPATH" \
        --requirement "$IMMUTABLE_ACTIONS_LOCK"
fi

run_policy_python()
{
    PYTHONDONTWRITEBYTECODE=1 PYTHONPATH="$POLICY_PYTHONPATH" python3 "$@"
}

[ -f "$ARTIFACT_TRUST_DOCUMENT" ] ||
    fail "artifact authenticity boundary is undocumented"
assert_contains "$ARTIFACT_TRUST_DOCUMENT" \
    'authenticated CI artifact transport is the current authenticity boundary'
assert_contains "$ARTIFACT_TRUST_DOCUMENT" \
    'does not provide independently verifiable artifact signatures'

assert_contains "$APPVEYOR_CONFIG" 'RELEASE_INPUTS_VERIFIED: "false"'
assert_contains "$APPVEYOR_CONFIG" \
    "Set-AppveyorBuildVariable -Name 'PUBLISH_BINARIES' -Value false"
assert_contains "$APPVEYOR_CONFIG" '      RELEASE_INPUTS_VERIFIED: true'
if grep -Fq "Set-AppveyorBuildVariable -Name 'PUBLISH_BINARIES' -Value true" \
    "$APPVEYOR_CONFIG"; then
    fail "AppVeyor mutable jobs can enable release publication"
fi
if grep -Eq 'util/add_secrets\.ps1|GC_.*(SECRET|API_KEY|BASIC_AUTH).*:' \
    "$APPVEYOR_CONFIG"; then
    fail "AppVeyor public artifacts receive reusable provider credentials"
fi

assert_contains "$LINUX_PACKAGE_PASS" \
    'require_unconfigured_strava_oauth_build "$BINARY"'
assert_contains "$LINUX_PACKAGE_PASS" \
    'require_unconfigured_strava_oauth_appimage'
for packager in "$LOCAL_PACKAGER" "$DEV_PACKAGER"; do
    assert_contains "$packager" 'GC_APPIMAGE_OAUTH_POLICY=configured'
    assert_contains "$packager" 'reproduce-appimage.sh'
done
assert_contains "$LINUX_PACKAGE_PASS" \
    'OAUTH_POLICY=${GC_APPIMAGE_OAUTH_POLICY:-unconfigured}'
assert_contains "$DEV_PACKAGER" '$repo_root/src/gcconfig.pri'
if grep -Fq '${repo_root}/.devcontainer/gcconfig.pri' "$DEV_PACKAGER"; then
    fail "development SBOM reads a template instead of the effective build config"
fi

assert_contains "$DEV_DOCKERFILE" 'install-verified-qt.py'
if grep -Fq '/opt/aqt/bin/aqt install-qt' "$DEV_DOCKERFILE"; then
    fail "Docker Qt setup still lets aqt extract archives before pinned verification"
fi

assert_contains "$GITHUB_CONFIG" 'permissions:'
assert_contains "$GITHUB_CONFIG" '  contents: read'
assert_contains "$GITHUB_CONFIG" '  trusted-macos-release:'
assert_contains "$GITHUB_CONFIG" \
    "if: \${{ github.event_name == 'push' && github.ref == 'refs/heads/master' && contains("
assert_contains "$GITHUB_CONFIG" '      contents: write'
assert_contains "$GITHUB_CONFIG" '      - name: Add Secrets'
assert_contains "$GITHUB_CONFIG" '        run: ./util/add_secrets.ps1'
if grep -Fq 'GC_STRAVA_CLIENT_SECRET: ${{ secrets.' "$GITHUB_CONFIG"; then
    fail "GitHub release build still injects a shared Strava client secret"
fi
assert_contains "$GITHUB_CONFIG" 'concurrency:'
assert_contains "$GITHUB_CONFIG" 'cancel-in-progress: true'
assert_contains "$GITHUB_CONFIG" 'appveyor/macos/run-build-regressions.sh'

run_policy_python "$IMMUTABLE_ACTIONS_CHECKER" \
    --workflows "$REPO_ROOT/.github/workflows"
assert_contains "$WORKFLOW_POLICY" 'pull_request_target:'
assert_contains "$WORKFLOW_POLICY" '--require-hashes'
assert_contains "$WORKFLOW_POLICY" '--only-binary=:all:'
assert_contains "$WORKFLOW_POLICY" \
    'Workflow policy / immutable actions'
assert_contains "$GITHUB_CONFIG" \
    'actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803'
assert_contains "$GITHUB_CONFIG" \
    'actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a'
assert_contains "$GITHUB_CONFIG" \
    'softprops/action-gh-release@3d0d9888cb7fd7b750713d6e236d1fcb99157228'

for packager in "$MACOS_PACKAGER" "$WINDOWS_PACKAGER" "$GITHUB_PACKAGER"; do
    assert_contains "$packager" 'check-unconfigured-oauth.py'
done
assert_contains "$MACOS_INSTALL" 'appveyor/safe-extract.py'
assert_contains "$WINDOWS_INSTALL" 'safe-extract.py'
assert_contains "$WINDOWS_PACKAGER" 'appveyor/check-unconfigured-oauth.py'
assert_contains "$GITHUB_INSTALL" 'appveyor/safe-extract.py'
assert_contains "$GITHUB_INSTALL" \
    'SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e'
assert_contains "$GITHUB_INSTALL" \
    'R_PACKAGE_SHA256=cc078658708fdc7ae807f927cf5b3a96d17d287717679b3327540030f625d47b'
assert_contains "$LINUX_INSTALL" 'SAFE_EXTRACT='
assert_contains "$LINUX_INSTALL" 'squashfs-tools'
assert_contains "$DEV_DOCKERFILE" 'squashfs-tools'
for archive in \
    '$GC_D2XX_ROOT/$D2XX_ARCHIVE' \
    '$GC_SRMIO_ROOT/$SRMIO_ARCHIVE' \
    '$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE'; do
    assert_contains "$LINUX_INSTALL" "--archive \"$archive\""
done
if grep -Eq '^[[:space:]]*tar[[:space:]]' "$LINUX_INSTALL"; then
    fail "AppVeyor Linux still extracts a downloaded archive with tar"
fi
if grep -Eq 'git clone .*srmio|pip install --upgrade|curl[^\n]*\|[[:space:]]*(sh|bash)' \
    "$GITHUB_BUILD" "$GITHUB_INSTALL"; then
    fail "GitHub build executes a dependency from an unpinned input"
fi
assert_contains "$GITHUB_BUILD" \
    'HOMEBREW_BREW_COMMIT=67658c8cf6ee685420c531ed94ed46b6e7ba5b2a'
assert_contains "$GITHUB_BUILD" \
    'HOMEBREW_CORE_COMMIT=2602f7f80784581466deb491f09bf734174ac772'

run_policy_python "$IMMUTABLE_ACTIONS_TEST"
python3 "$RELEASE_HARDENING_TEST"
if [ "$MODE" = "full" ]; then
    python3 "$APT_SNAPSHOT_TEST"
fi

[ -r "$WINDOWS_REGRESSIONS" ] ||
    fail "native Windows build regression runner is missing"
[ -r "$MACOS_REGRESSION" ] ||
    fail "native macOS packaging behavior test is missing"
assert_contains "$APPVEYOR_CONFIG" 'run-build-regressions.ps1'
assert_contains "$APPVEYOR_CONFIG" 'appveyor/macos/run-build-regressions.sh'

echo "PASS: candidate artifacts are credential-free and trusted releases are isolated"
