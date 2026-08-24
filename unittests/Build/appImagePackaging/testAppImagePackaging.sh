#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
SUPPORT="$REPO_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
LOCAL_PACKAGER="$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh"
CI_PACKAGER="$REPO_ROOT/appveyor/linux/after_build.sh"
CI_PACKAGE_PASS="$REPO_ROOT/appveyor/linux/package-appimage-pass.sh"
CI_BUILD_PASS="$REPO_ROOT/appveyor/linux/build-appimage-pass.sh"
CI_REPRODUCE="$REPO_ROOT/appveyor/linux/reproduce-appimage.sh"
DEV_PACKAGER="$REPO_ROOT/.devcontainer/package-appimage.sh"
DEV_DOCKERFILE="$REPO_ROOT/.devcontainer/Dockerfile"
APPVEYOR_INSTALL="$REPO_ROOT/appveyor/linux/install.sh"
APPVEYOR_MACOS_INSTALL="$REPO_ROOT/appveyor/macos/install.sh"
APPVEYOR_MACOS_PACKAGER="$REPO_ROOT/appveyor/macos/after_build.sh"
APPVEYOR_WINDOWS_INSTALL="$REPO_ROOT/appveyor/windows/install.ps1"
APPVEYOR_WINDOWS_BEFORE_BUILD="$REPO_ROOT/appveyor/windows/before_build.ps1"
APPVEYOR_WINDOWS_PACKAGER="$REPO_ROOT/appveyor/windows/after_build.ps1"
APPVEYOR_WINDOWS_VCPKG="$REPO_ROOT/appveyor/windows/vcpkg.json"
APPVEYOR_CONFIG="$REPO_ROOT/appveyor.yml"
GITHUB_CI_CONFIG="$REPO_ROOT/.github/workflows/ci.yml"
UBUNTU_SNAPSHOT="$REPO_ROOT/appveyor/linux/ubuntu-snapshot.sources.list"
DEV_UBUNTU_SNAPSHOT="$REPO_ROOT/.devcontainer/ubuntu-snapshot.sources.list"
SECRETS_SCRIPT="$REPO_ROOT/util/add_secrets.ps1"
SECRETS_HEADER="$REPO_ROOT/src/Core/Secrets.h"
SECRETS_TEST="$SCRIPT_DIR/testGeneratedSecrets.ps1"
WINDOWS_PACKAGING_TEST="$SCRIPT_DIR/testWindowsPackaging.ps1"
WINDOWS_OPENSSL_TEST="$SCRIPT_DIR/testWindowsOpenSsl.py"
CI_RELEASE_GATES_TEST="$SCRIPT_DIR/testCiReleaseGates.sh"
QT_ARCHIVE_TEST="$SCRIPT_DIR/testVerifiedQtArchives.py"
SAFE_EXTRACTION_TEST="$SCRIPT_DIR/testSafeExtraction.py"
MACOS_PACKAGING_TEST="$SCRIPT_DIR/testMacOSPackaging.py"
SBOM_PROVENANCE_TEST="$SCRIPT_DIR/testSbomProvenance.py"
DIAGNOSTIC_OAUTH_TEST="$SCRIPT_DIR/testDiagnosticOAuth.sh"
UNCONFIGURED_OAUTH_TEST="$SCRIPT_DIR/testUnconfiguredOAuthGate.py"
REQUIREMENTS="$REPO_ROOT/src/Python/requirements.txt"
APPIMAGE_REQUIREMENTS="$REPO_ROOT/src/Python/requirements-appimage.lock"
SBOM_GENERATOR="$REPO_ROOT/src/Resources/linux/generate-appimage-sbom.py"
RUNTIME_PROVENANCE_GENERATOR="$REPO_ROOT/src/Resources/linux/generate-runtime-provenance.py"
LINUXDEPLOYQT_CAPTURE="$REPO_ROOT/src/Resources/linux/capture-linuxdeployqt-transforms.py"
PYTHON_NORMALIZER="$REPO_ROOT/src/Resources/linux/normalize-embedded-python.py"
DEV_CONFIG="$REPO_ROOT/.devcontainer/gcconfig.pri"
MAIN_SOURCE="$REPO_ROOT/src/Core/main.cpp"
LIBSECRET_SOURCE="$REPO_ROOT/contrib/qtkeychain/qtkeychain/libsecret.cpp"
SOURCE_PROJECT="$REPO_ROOT/src/src.pro"
SOURCE_CONFIG_TEMPLATE="$REPO_ROOT/src/gcconfig.pri.in"
ATHLETE_MIGRATION_STUB="$REPO_ROOT/unittests/Core/athleteMigrationSafety/AthleteMigrationTestStubs.cpp"
ATHLETE_MIGRATION_PROJECT="$REPO_ROOT/unittests/Core/athleteMigrationSafety/athleteMigrationSafety.pro"
STRAVA_OAUTH_POLICY_PROJECT="$REPO_ROOT/unittests/Cloud/stravaOAuthPolicy/stravaOAuthPolicy.pro"
STRAVA_ROUTES_PIPELINE_PROJECT="$REPO_ROOT/unittests/Train/stravaRoutesDownloadPipeline/stravaRoutesDownloadPipeline.pro"
DATA_FILTER_ZONES_PROJECT="$REPO_ROOT/unittests/Core/dataFilterZones/dataFilterZones.pro"
INDEND_PLOT_MARKER_PROJECT="$REPO_ROOT/unittests/Charts/indendPlotMarkerMatrix/indendPlotMarkerMatrix.pro"
UNITTEST_CONFIG="$REPO_ROOT/unittests/unittests.pri.in"
CREDENTIAL_SETTINGS_TEST="$REPO_ROOT/unittests/Core/credentialSettings/testCredentialSettings.cpp"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains()
{
    local file=$1
    local pattern=$2
    grep -Fq -- "$pattern" "$file" ||
        fail "$file does not contain: $pattern"
}

[ -r "$SUPPORT" ] || fail "missing shared AppImage packaging support"
[ -r "$PYTHON_NORMALIZER" ] ||
    fail "missing embedded Python normalizer"
[ -x "$LINUXDEPLOYQT_CAPTURE" ] ||
    fail "missing linuxdeployqt transformation capture"
[ -r "$REPO_ROOT/appveyor/safe-extract.py" ] ||
    fail "missing safe cross-platform archive extractor"

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
[ "$D2XX_LINUX_VERSION" = "1.4.33" ] ||
    fail "unexpected Linux D2XX version"
[ "$D2XX_LINUX_SHA256" = \
  "e260a4594a313583b87bf230c79cec9d46f11db6dcfd7c7d4f963279703214d3" ] ||
    fail "Linux D2XX SHA-256 is not the independently verified digest"
[ "$D2XX_LINUX_SOURCE_URL" = \
  "https://distfiles.gentoo.org/distfiles/b1/libftd2xx-x86_64-1.4.33.tar.gz" ] ||
    fail "Linux D2XX source is not the stable reviewed mirror"
[ "$LINUXDEPLOYQT_FILE" = \
  "linuxdeployqt-build107-20251021-x86_64.AppImage" ] ||
    fail "linuxdeployqt filename is not versioned"
[ "$LINUXDEPLOYQT_SHA256" = \
  "974a87457ed26241b793bed7841978fcdf84158d13220e53833a06515f173b0b" ] ||
    fail "linuxdeployqt SHA-256 is not the reviewed release digest"
[ "$LINUXDEPLOYQT_URL" = \
  "https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$LINUXDEPLOYQT_FILE" ] ||
    fail "linuxdeployqt URL is not an immutable project asset"
[ "$APPIMAGETOOL_FILE" = \
  "appimagetool-8c8c91f-build295-x86_64.AppImage" ] ||
    fail "appimagetool filename is not versioned"
[ "$APPIMAGETOOL_SHA256" = \
  "a6d71e2b6cd66f8e8d16c37ad164658985e0cf5fcaa950c90a482890cb9d13e0" ] ||
    fail "appimagetool SHA-256 is not the reviewed release digest"
[ "$APPIMAGETOOL_URL" = \
  "https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$APPIMAGETOOL_FILE" ] ||
    fail "appimagetool URL is not an immutable project asset"
[ "$APPIMAGE_RUNTIME_FILE" = "runtime-2fca8b44-x86_64" ] ||
    fail "AppImage runtime filename is not content-addressed"
[ "$APPIMAGE_RUNTIME_SHA256" = \
  "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d" ] ||
    fail "AppImage runtime SHA-256 is not the reviewed digest"
[ "$APPIMAGE_RUNTIME_URL" = \
  "https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64" ] ||
    fail "unexpected AppImage runtime source"
[ "${QTKEYCHAIN_LICENSE_SHA256:-}" = \
  "ca46b73d5159548ab52834db51f195aa3d1f277f020e9dca92f4beb21b468a50" ] ||
    fail "QtKeychain license digest is not the reviewed content"
[ "${LGPL21_LICENSE_SHA256:-}" = \
  "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551" ] ||
    fail "LGPL-2.1 digest is not the reviewed content"

declare -F download_file >/dev/null || fail "download_file helper is missing"
declare -F download_verified_file >/dev/null ||
    fail "verified download helper is missing"
declare -F download_appimage_runtime >/dev/null ||
    fail "verified AppImage runtime helper is missing"
declare -F create_appimage_sbom >/dev/null ||
    fail "AppImage SBOM helper is missing"
declare -F install_appimage_dir_icon >/dev/null ||
    fail "AppImage directory icon helper is missing"
declare -F set_appimage_source_date_epoch >/dev/null ||
    fail "SOURCE_DATE_EPOCH helper is missing"
declare -F normalize_appdir_mtimes >/dev/null ||
    fail "AppDir mtime normalization helper is missing"
declare -F validate_appimage_sbom >/dev/null ||
    fail "AppImage SBOM validator is missing"
declare -F verify_appimage_sbom >/dev/null ||
    fail "packaged AppImage SBOM verifier is missing"
declare -F require_qt_offscreen_appimage_on_glibc >/dev/null ||
    fail "glibc compatibility smoke helper is missing"
declare -F run_packaging_appimage >/dev/null ||
    fail "run_packaging_appimage helper is missing"
declare -F run_packaged_appimage_smoke >/dev/null ||
    fail "run_packaged_appimage_smoke helper is missing"
declare -F compare_appimage_reproduction >/dev/null ||
    fail "compare_appimage_reproduction helper is missing"
declare -F install_qt_offscreen_plugin >/dev/null ||
    fail "install_qt_offscreen_plugin helper is missing"
declare -F require_qt_offscreen_appimage >/dev/null ||
    fail "require_qt_offscreen_appimage helper is missing"
declare -F install_embedded_python >/dev/null ||
    fail "install_embedded_python helper is missing"
declare -F write_source_revision >/dev/null ||
    fail "write_source_revision helper is missing"
declare -F create_appimage_build_manifest >/dev/null ||
    fail "create_appimage_build_manifest helper is missing"
declare -F install_appimage_build_manifest >/dev/null ||
    fail "install_appimage_build_manifest helper is missing"
declare -F finalize_appimage_manifest >/dev/null ||
    fail "finalize_appimage_manifest helper is missing"
declare -F verify_appimage_manifest >/dev/null ||
    fail "verify_appimage_manifest helper is missing"
declare -F verify_legacy_appimage_manifest >/dev/null ||
    fail "legacy AppImage manifest verifier is missing"
declare -F promote_appimage_release >/dev/null ||
    fail "promote_appimage_release helper is missing"
declare -F strava_oauth_build_status >/dev/null ||
    fail "strava_oauth_build_status helper is missing"
declare -F require_strava_oauth_build >/dev/null ||
    fail "require_strava_oauth_build helper is missing"
declare -F require_configured_strava_oauth_build >/dev/null ||
    fail "configured build-status gate is missing"
declare -F strava_oauth_appimage_status >/dev/null ||
    fail "strava_oauth_appimage_status helper is missing"
declare -F require_strava_oauth_appimage >/dev/null ||
    fail "require_strava_oauth_appimage helper is missing"
declare -F require_configured_strava_oauth_appimage >/dev/null ||
    fail "configured AppImage-status gate is missing"
declare -F require_unconfigured_strava_oauth_build >/dev/null ||
    fail "credential-free build-status gate is missing"
declare -F require_unconfigured_strava_oauth_appimage >/dev/null ||
    fail "credential-free AppImage-status gate is missing"
declare -F install_linux_keychain_runtime >/dev/null ||
    fail "install_linux_keychain_runtime helper is missing"
declare -F linux_keychain_runtime_status >/dev/null ||
    fail "linux_keychain_runtime_status helper is missing"
declare -F linux_keychain_appimage_status >/dev/null ||
    fail "linux_keychain_appimage_status helper is missing"
declare -F require_linux_keychain_appimage >/dev/null ||
    fail "require_linux_keychain_appimage helper is missing"
declare -F linux_keychain_entrypoint_status >/dev/null ||
    fail "linux_keychain_entrypoint_status helper is missing"
declare -F create_linux_keychain_deploy_probe >/dev/null ||
    fail "create_linux_keychain_deploy_probe helper is missing"
declare -F remove_linux_keychain_deploy_probe >/dev/null ||
    fail "remove_linux_keychain_deploy_probe helper is missing"
declare -F run_linuxdeployqt_with_keychain_probe >/dev/null ||
    fail "run_linuxdeployqt_with_keychain_probe helper is missing"

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT

NORMALIZER_FIXTURE="$TEMP_DIR/python-normalizer"
NORMALIZER_ROOT="$NORMALIZER_FIXTURE/opt/python3.11"
NORMALIZER_SITE="$NORMALIZER_ROOT/lib/python3.11/site-packages"
NORMALIZER_FORBIDDEN="$NORMALIZER_FIXTURE/random-build"
NORMALIZER_MANIFEST="$TEMP_DIR/python-normalizer-runtime.json"
mkdir -p "$NORMALIZER_ROOT/bin" \
    "$NORMALIZER_SITE/fixture.dist-info" \
    "$NORMALIZER_SITE/numpy/random" \
    "$NORMALIZER_FIXTURE/usr/lib"
printf 'nested runtime\n' >"$NORMALIZER_SITE/numpy/random/mtrand.so"
printf 'sibling runtime\n' >"$NORMALIZER_FIXTURE/usr/lib/libfixture.so.1"
cat >"$NORMALIZER_ROOT/bin/python3.11" <<'EOF'
#!/bin/sh
exec python3 "$@"
EOF
chmod +x "$NORMALIZER_ROOT/bin/python3.11"
cat >"$NORMALIZER_ROOT/bin/fixture-tool" <<EOF
#!$NORMALIZER_FORBIDDEN/python3.11
print("normalized-ok")
EOF
chmod +x "$NORMALIZER_ROOT/bin/fixture-tool"
cat >"$NORMALIZER_SITE/fixture.dist-info/RECORD" <<'EOF'
../../../bin/fixture-tool,sha256=obsolete,1
fixture.dist-info/RECORD,,
EOF
python3 "$PYTHON_NORMALIZER" \
    --python-root "$NORMALIZER_ROOT" \
    --forbidden-prefix "$NORMALIZER_FORBIDDEN" \
    --payload-root "$NORMALIZER_FIXTURE" \
    --runtime-manifest "$NORMALIZER_MANIFEST" \
    --runtime-sha256 "$PYTHON_APPIMAGE_SHA256"
[ "$("$NORMALIZER_ROOT/bin/fixture-tool")" = "normalized-ok" ] ||
    fail "normalized embedded Python console script is not relocatable"
if grep -R -F -q -- "$NORMALIZER_FORBIDDEN" "$NORMALIZER_ROOT"; then
    fail "embedded Python normalizer retained a build path"
fi
grep -Eq '^\.\./\.\./\.\./bin/fixture-tool,sha256=[A-Za-z0-9_-]{43},[0-9]+$' \
    "$NORMALIZER_SITE/fixture.dist-info/RECORD" ||
    fail "embedded Python normalizer did not update RECORD"
python3 - "$NORMALIZER_MANIFEST" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
assert document["format"] == "goldencheetah-python-source-runtime-2"
assert document["distributions"] == []
paths = [entry["path"] for entry in document["files"]]
assert paths == sorted(set(paths))
assert paths == [
    "opt/python3.11/bin/fixture-tool",
    "opt/python3.11/bin/python3.11",
    "opt/python3.11/lib/python3.11/site-packages/fixture.dist-info/RECORD",
    "opt/python3.11/lib/python3.11/site-packages/numpy/random/mtrand.so",
    "usr/lib/libfixture.so.1",
]
transformations = {entry["path"]: entry["transformation"]
                   for entry in document["files"]}
assert transformations["opt/python3.11/bin/fixture-tool"] == \
    "python-console-script-wrapper-v1"
assert transformations[
    "opt/python3.11/lib/python3.11/site-packages/fixture.dist-info/RECORD"
] == "python-wheel-record-refresh-v1"
PY

printf 'reviewed payload\n' >"$TEMP_DIR/reviewed-download"
REVIEWED_DOWNLOAD_SHA256=$(sha256sum "$TEMP_DIR/reviewed-download" |
    cut -d ' ' -f 1)
download_verified_file \
    "file://$TEMP_DIR/reviewed-download" \
    "$TEMP_DIR/download-cache" "$REVIEWED_DOWNLOAD_SHA256"
cmp "$TEMP_DIR/reviewed-download" "$TEMP_DIR/download-cache" ||
    fail "verified download changed the reviewed payload"
printf 'corrupt cache\n' >"$TEMP_DIR/download-cache"
download_verified_file \
    "file://$TEMP_DIR/reviewed-download" \
    "$TEMP_DIR/download-cache" "$REVIEWED_DOWNLOAD_SHA256"
cmp "$TEMP_DIR/reviewed-download" "$TEMP_DIR/download-cache" ||
    fail "verified download did not replace a corrupt cache entry"
printf 'old destination\n' >"$TEMP_DIR/rejected-download"
if download_verified_file \
    "file://$TEMP_DIR/reviewed-download" \
    "$TEMP_DIR/rejected-download" \
    0000000000000000000000000000000000000000000000000000000000000000 \
    >/dev/null 2>&1; then
    fail "verified download accepted an unexpected digest"
fi
[ ! -e "$TEMP_DIR/rejected-download" ] ||
    fail "failed verified download left a destination behind"
if find "$TEMP_DIR" -maxdepth 1 -name 'rejected-download.tmp.*' |
   grep -q .; then
    fail "failed verified download left a temporary file behind"
fi

printf '#!/bin/sh\nprintf "%%s" "$APPIMAGE_EXTRACT_AND_RUN"\n' \
    >"$TEMP_DIR/check-extract-mode"
chmod +x "$TEMP_DIR/check-extract-mode"
[ "$(run_packaged_appimage_smoke 2s \
      "$TEMP_DIR/check-extract-mode")" = "1" ] ||
    fail "packaged AppImage smoke did not use extraction mode"

mkdir -p \
    "$TEMP_DIR/qt-plugins/platforms" \
    "$TEMP_DIR/offscreen-appdir/plugins/platforms"
printf 'offscreen fixture\n' \
    >"$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so"
cat >"$TEMP_DIR/fake-qmake" <<EOF
#!/bin/sh
test "\$1" = "-query" &&
    test "\$2" = "QT_INSTALL_PLUGINS" || exit 64
printf '%s\\n' "$TEMP_DIR/qt-plugins"
EOF
chmod +x "$TEMP_DIR/fake-qmake"
install_qt_offscreen_plugin \
    "$TEMP_DIR/fake-qmake" "$TEMP_DIR/offscreen-appdir"
cmp \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so" \
    "$TEMP_DIR/offscreen-appdir/plugins/platforms/libqoffscreen.so" ||
    fail "offscreen plugin was not copied from qmake's plugin directory"

mv \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so" \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so.missing"
if install_qt_offscreen_plugin \
    "$TEMP_DIR/fake-qmake" "$TEMP_DIR/offscreen-appdir" \
    >/dev/null 2>&1; then
    fail "missing Qt offscreen plugin was accepted"
fi

cat >"$TEMP_DIR/offscreen-smoke" <<'EOF'
#!/bin/sh
test "$APPIMAGE_EXTRACT_AND_RUN" = "1" || exit 65
test "$QT_QPA_PLATFORM" = "offscreen" || exit 66
test "$QT_OPENGL" = "software" || exit 67
test "$1" = "--goldencheetah-gui-smoke" || exit 68
test "$3" = "SmokeAthlete" || exit 69
test -d "$2/SmokeAthlete" || exit 70
test -f "$2/SmokeAthlete/config/athlete-general.ini" || exit 71
printf '%s\n' 'goldencheetah_gui_smoke=main-window-ready'
EOF
chmod +x "$TEMP_DIR/offscreen-smoke"
[ "$(require_qt_offscreen_appimage \
      "$TEMP_DIR/offscreen-smoke" 0.1s)" = \
  "Qt offscreen runtime: available" ] ||
    fail "offscreen AppImage smoke did not accept a running image"
cat >"$TEMP_DIR/offscreen-hang" <<'EOF'
#!/bin/sh
sleep 5
EOF
chmod +x "$TEMP_DIR/offscreen-hang"
if require_qt_offscreen_appimage \
    "$TEMP_DIR/offscreen-hang" 0.1s \
    >/dev/null 2>&1; then
    fail "offscreen AppImage smoke accepted a hang without a ready marker"
fi
cat >"$TEMP_DIR/offscreen-marker-hang" <<'EOF'
#!/bin/sh
printf '%s\n' 'goldencheetah_gui_smoke=main-window-ready'
sleep 5
EOF
chmod +x "$TEMP_DIR/offscreen-marker-hang"
if require_qt_offscreen_appimage \
    "$TEMP_DIR/offscreen-marker-hang" 0.1s \
    >/dev/null 2>&1; then
    fail "offscreen AppImage smoke accepted a hang after the ready marker"
fi
printf '#!/bin/sh\nexit 0\n' >"$TEMP_DIR/offscreen-no-marker"
chmod +x "$TEMP_DIR/offscreen-no-marker"
if require_qt_offscreen_appimage \
    "$TEMP_DIR/offscreen-no-marker" 0.1s \
    >/dev/null 2>&1; then
    fail "offscreen AppImage smoke accepted success without a ready marker"
fi
mkdir "$TEMP_DIR/glibc-2.35"
cat >"$TEMP_DIR/glibc-2.35/getconf" <<'EOF'
#!/bin/sh
test "$1" = "GNU_LIBC_VERSION" || exit 64
printf 'glibc 2.35\n'
EOF
chmod +x "$TEMP_DIR/glibc-2.35/getconf"
[ "$(PATH="$TEMP_DIR/glibc-2.35:$PATH" \
      require_qt_offscreen_appimage_on_glibc \
          2.35 "$TEMP_DIR/offscreen-smoke" 0.1s)" = \
  "Qt offscreen runtime: available on glibc 2.35" ] ||
    fail "oldest supported glibc smoke did not execute the AppImage"
if PATH="$TEMP_DIR/glibc-2.35:$PATH" \
   require_qt_offscreen_appimage_on_glibc \
       2.34 "$TEMP_DIR/offscreen-smoke" 0.1s \
       >/dev/null 2>&1; then
    fail "AppImage compatibility smoke ran on an unexpected glibc"
fi
printf '#!/bin/sh\nexit 127\n' >"$TEMP_DIR/offscreen-failure"
chmod +x "$TEMP_DIR/offscreen-failure"
if require_qt_offscreen_appimage \
    "$TEMP_DIR/offscreen-failure" 0.1s \
    >/dev/null 2>&1; then
    fail "offscreen AppImage smoke accepted an initialization failure"
fi

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

PROVENANCE_REPO="$TEMP_DIR/provenance-repo"
mkdir -p "$PROVENANCE_REPO/src/Core" "$PROVENANCE_REPO/qwt"
git -C "$PROVENANCE_REPO" init -q
git -C "$PROVENANCE_REPO" config user.name "Packaging Test"
git -C "$PROVENANCE_REPO" config user.email "packaging@example.invalid"
printf '/src/gcconfig.pri\n/src/Core/GeneratedSecrets.h\n/qwt/qwtconfig.pri\n' \
    >"$PROVENANCE_REPO/.gitignore"
printf 'CONFIG += release\n' >"$PROVENANCE_REPO/src/gcconfig.pri"
printf 'QWT_CONFIG += QwtPlot\n' >"$PROVENANCE_REPO/qwt/qwtconfig.pri"
printf 'revision a\n' >"$PROVENANCE_REPO/source.txt"
git -C "$PROVENANCE_REPO" add .gitignore source.txt
git -C "$PROVENANCE_REPO" commit -q -m a
REVISION_A=$(git -C "$PROVENANCE_REPO" rev-parse HEAD)
printf 'revision b\n' >"$PROVENANCE_REPO/source.txt"
git -C "$PROVENANCE_REPO" commit -q -am b
REVISION_B=$(git -C "$PROVENANCE_REPO" rev-parse HEAD)

make_provenance_probe()
{
    local output=$1
    local revision=$2
    local build_inputs
    build_inputs=$(python3 "$REPO_ROOT/src/Resources/linux/compute-build-input-identity.py" \
        "$PROVENANCE_REPO")
    cat >"$output" <<EOF
#!/bin/sh
test "\${1:-}" = "--goldencheetah-build-provenance" || exit 64
cat <<REPORT
goldencheetah_build_provenance=1
application=GoldenCheetah
source_revision=$revision
build_inputs_sha256=$build_inputs
compiler_family=gcc
compiler_version=14.2.0
qt_version=6.8.3
cxx_standard=201703
REPORT
EOF
    chmod +x "$output"
}

make_provenance_probe "$TEMP_DIR/provenance-a" "$REVISION_A"
make_provenance_probe "$TEMP_DIR/provenance-b" "$REVISION_B"
make_provenance_probe "$TEMP_DIR/provenance-unknown" \
    0000000000000000000000000000000000000000
printf '#!/bin/sh\nprintf "malformed\\n"\n' \
    >"$TEMP_DIR/provenance-malformed"
chmod +x "$TEMP_DIR/provenance-malformed"

if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-a" \
        "Strava OAuth: configured" "$TEMP_DIR/mismatch.manifest" \
        >/dev/null 2>&1; then
    fail "binary from revision A was accepted for revision B"
fi
if GC_SOURCE_REVISION=$REVISION_A \
   GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: configured" "$TEMP_DIR/wrong-head.manifest" \
        >/dev/null 2>&1; then
    fail "a non-HEAD source revision was accepted"
fi
if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-unknown" \
        "Strava OAuth: configured" "$TEMP_DIR/unknown.manifest" \
        >/dev/null 2>&1; then
    fail "a binary claiming an unknown revision was accepted"
fi
if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-malformed" \
        "Strava OAuth: configured" "$TEMP_DIR/malformed.manifest" \
        >/dev/null 2>&1; then
    fail "a malformed binary provenance report was accepted"
fi

printf 'dirty\n' >>"$PROVENANCE_REPO/source.txt"
if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: configured" "$TEMP_DIR/dirty.manifest" \
        >/dev/null 2>&1; then
    fail "a dirty tracked source tree was accepted"
fi
git -C "$PROVENANCE_REPO" checkout -q -- source.txt
printf 'untracked\n' >"$PROVENANCE_REPO/untracked.txt"
if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: configured" "$TEMP_DIR/untracked.manifest" \
        >/dev/null 2>&1; then
    fail "a dirty untracked source tree was accepted"
fi
rm "$PROVENANCE_REPO/untracked.txt"

BASE_MANIFEST="$TEMP_DIR/build.manifest"
GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: configured" "$BASE_MANIFEST"
grep -Fxq 'goldencheetah_appimage_manifest=2' "$BASE_MANIFEST" ||
    fail "manifest version is missing"
grep -Fxq "source_revision=$REVISION_B" "$BASE_MANIFEST" ||
    fail "manifest source revision is wrong"
grep -Fxq "raw_elf_sha256=$(sha256sum "$TEMP_DIR/provenance-b" | cut -d ' ' -f 1)" \
    "$BASE_MANIFEST" || fail "manifest raw ELF hash is wrong"
grep -Fxq 'toolchain=gcc-14.2.0_qt-6.8.3_cxx-201703' \
    "$BASE_MANIFEST" || fail "manifest toolchain identity is wrong"
grep -Fxq 'strava_oauth_configured=true' "$BASE_MANIFEST" ||
    fail "manifest OAuth status is not boolean true"

set_appimage_source_date_epoch "$BASE_MANIFEST" "$PROVENANCE_REPO"
EXPECTED_SOURCE_DATE_EPOCH=$(git -C "$PROVENANCE_REPO" show -s --format=%ct \
    "$REVISION_B")
[ "$SOURCE_DATE_EPOCH" = "$EXPECTED_SOURCE_DATE_EPOCH" ] ||
    fail "SOURCE_DATE_EPOCH was not derived from the manifest revision"

REPRO_A="$TEMP_DIR/repro-a"
REPRO_B="$TEMP_DIR/repro-b"
for appdir in "$REPRO_A" "$REPRO_B"; do
    mkdir -p "$appdir/usr/bin" "$appdir/usr/share/reproducibility"
    cat >"$appdir/AppRun" <<'EOF'
#!/bin/sh
exec "$(dirname "$0")/usr/bin/reproducibility-fixture" "$@"
EOF
    cat >"$appdir/reproducibility-fixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Reproducibility Fixture
Exec=reproducibility-fixture
Icon=reproducibility-fixture
Categories=Utility;
EOF
    cat >"$appdir/reproducibility-fixture.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 16 16">
  <rect width="16" height="16" fill="#247a52"/>
</svg>
EOF
    cat >"$appdir/usr/bin/reproducibility-fixture" <<'EOF'
#!/bin/sh
printf '%s\n' reproducible
EOF
    printf 'same payload\n' >"$appdir/usr/share/reproducibility/payload"
    ln -s payload "$appdir/usr/share/reproducibility/current"
    chmod +x "$appdir/AppRun" "$appdir/usr/bin/reproducibility-fixture"
done
touch -d '2020-01-01 00:00:00 UTC' \
    "$REPRO_A/usr/share/reproducibility/payload"
touch -d '2030-01-01 00:00:00 UTC' \
    "$REPRO_B/usr/share/reproducibility/payload"
normalize_appdir_mtimes "$REPRO_A"
normalize_appdir_mtimes "$REPRO_B"

REPRO_TOOL="$TEMP_DIR/$APPIMAGETOOL_FILE"
if [ -n "${GC_PINNED_APPIMAGETOOL:-}" ]; then
    verify_sha256 "$GC_PINNED_APPIMAGETOOL" "$APPIMAGETOOL_SHA256" ||
        fail "GC_PINNED_APPIMAGETOOL is not the reviewed appimagetool"
    install -m 0755 "$GC_PINNED_APPIMAGETOOL" "$REPRO_TOOL"
else
    download_verified_file \
        "$APPIMAGETOOL_URL" "$REPRO_TOOL" "$APPIMAGETOOL_SHA256"
    chmod +x "$REPRO_TOOL"
fi
REPRO_RUNTIME="$TEMP_DIR/$APPIMAGE_RUNTIME_FILE"
if [ -n "${GC_PINNED_APPIMAGE_RUNTIME:-}" ]; then
    verify_sha256 "$GC_PINNED_APPIMAGE_RUNTIME" "$APPIMAGE_RUNTIME_SHA256" ||
        fail "GC_PINNED_APPIMAGE_RUNTIME is not the reviewed runtime"
    install -m 0644 "$GC_PINNED_APPIMAGE_RUNTIME" "$REPRO_RUNTIME"
else
    download_verified_file \
        "$APPIMAGE_RUNTIME_URL" "$REPRO_RUNTIME" "$APPIMAGE_RUNTIME_SHA256"
fi
ARCH=x86_64 run_packaging_appimage \
    "$REPRO_TOOL" --runtime-file "$REPRO_RUNTIME" \
    "$REPRO_A" "$TEMP_DIR/repro-a.AppImage"
ARCH=x86_64 run_packaging_appimage \
    "$REPRO_TOOL" --runtime-file "$REPRO_RUNTIME" \
    "$REPRO_B" "$TEMP_DIR/repro-b.AppImage"
cmp "$TEMP_DIR/repro-a.AppImage" "$TEMP_DIR/repro-b.AppImage" ||
    fail "pinned appimagetool did not produce identical package bytes"

REPRO_PASS_A="$TEMP_DIR/production-pass-a"
REPRO_PASS_B="$TEMP_DIR/production-pass-b"
for pass_dir in "$REPRO_PASS_A" "$REPRO_PASS_B"; do
    mkdir -p "$pass_dir"
    printf 'production manifest\n' >"$pass_dir/build.manifest"
    printf '{"bomFormat":"CycloneDX"}\n' \
        >"$pass_dir/GoldenCheetah.AppImage.sbom.cdx.json"
    printf 'production image\n' >"$pass_dir/GoldenCheetah.AppImage"
    chmod +x "$pass_dir/GoldenCheetah.AppImage"
    image_hash=$(sha256sum "$pass_dir/GoldenCheetah.AppImage" | cut -d ' ' -f 1)
    cp "$pass_dir/build.manifest" \
        "$pass_dir/GoldenCheetah.AppImage.manifest"
    printf 'appimage_sha256=%s\n' "$image_hash" \
        >>"$pass_dir/GoldenCheetah.AppImage.manifest"
done
compare_appimage_reproduction "$REPRO_PASS_A" "$REPRO_PASS_B" ||
    fail "matching production packaging outputs were rejected"
printf 'different SBOM\n' \
    >"$REPRO_PASS_B/GoldenCheetah.AppImage.sbom.cdx.json"
if compare_appimage_reproduction "$REPRO_PASS_A" "$REPRO_PASS_B" \
    >/dev/null 2>&1; then
    fail "production reproduction accepted different SBOM bytes"
fi

PRODUCTION_PACKAGE_PASS="$REPO_ROOT/appveyor/linux/package-appimage-pass.sh"
PRODUCTION_BUILD_PASS="$REPO_ROOT/appveyor/linux/build-appimage-pass.sh"
grep -Fq 'set_appimage_source_date_epoch' "$PRODUCTION_PACKAGE_PASS" ||
    fail "production package pass does not set SOURCE_DATE_EPOCH"
grep -Fq 'normalize_appdir_mtimes' "$PRODUCTION_PACKAGE_PASS" ||
    fail "production package pass does not normalize AppDir mtimes"
DIR_ICON_SETUP_LINE=$(grep -nF \
    'install_appimage_dir_icon "$APPDIR" gc.png' \
    "$PRODUCTION_PACKAGE_PASS" | cut -d: -f1 || true)
SBOM_SETUP_LINE=$(grep -n '^create_appimage_sbom' \
    "$PRODUCTION_PACKAGE_PASS" | cut -d: -f1 || true)
if [ -z "$DIR_ICON_SETUP_LINE" ] || [ -z "$SBOM_SETUP_LINE" ] ||
   [ "$DIR_ICON_SETUP_LINE" -ge "$SBOM_SETUP_LINE" ]; then
    fail "production package pass does not anchor .DirIcon before the SBOM"
fi
grep -Fq 'download_appimage_runtime' "$PRODUCTION_PACKAGE_PASS" ||
    fail "production package pass does not acquire the pinned runtime"
grep -Fq -- '--runtime-file' "$PRODUCTION_PACKAGE_PASS" ||
    fail "production package pass does not pass the pinned runtime"
grep -Fq 'SOURCE_DATE_EPOCH=' "$PRODUCTION_BUILD_PASS" ||
    fail "production build pass does not set SOURCE_DATE_EPOCH"
for packager in \
    "$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh" \
    "$REPO_ROOT/.devcontainer/package-appimage.sh" \
    "$REPO_ROOT/appveyor/linux/after_build.sh"; do
    grep -Fq 'reproduce-appimage.sh' "$packager" ||
        fail "$packager bypasses independent release builds"
done

UNCONFIGURED_MANIFEST="$TEMP_DIR/unconfigured-build.manifest"
GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: unavailable (credentials not configured)" \
        "$UNCONFIGURED_MANIFEST"
grep -Fxq 'strava_oauth_configured=false' "$UNCONFIGURED_MANIFEST" ||
    fail "manifest OAuth status is not boolean false"
DIAGNOSTIC_OAUTH_MANIFEST="$TEMP_DIR/diagnostic-oauth-build.manifest"
GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth compile-time fallback: unavailable" \
        "$DIAGNOSTIC_OAUTH_MANIFEST"
grep -Fxq 'strava_oauth_configured=false' "$DIAGNOSTIC_OAUTH_MANIFEST" ||
    fail "diagnostic OAuth status encoded a compile-time fallback"
RUNTIME_OAUTH_MANIFEST="$TEMP_DIR/runtime-oauth-build.manifest"
GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: runtime credentials supported" \
        "$RUNTIME_OAUTH_MANIFEST"
grep -Fxq 'strava_oauth_configured=false' "$RUNTIME_OAUTH_MANIFEST" ||
    fail "runtime-only OAuth manifest encoded a compile-time fallback"
if GC_TEST_BUILD_PROVENANCE_ENTRYPOINT=true \
    create_appimage_build_manifest \
        "$PROVENANCE_REPO" "$TEMP_DIR/provenance-b" \
        "Strava OAuth: unknown" "$TEMP_DIR/unknown-oauth.manifest" \
        >/dev/null 2>&1; then
    fail "an unknown OAuth status was accepted"
fi

MANIFEST_APPDIR="$TEMP_DIR/manifest-appdir"
install_appimage_build_manifest "$BASE_MANIFEST" "$MANIFEST_APPDIR"
cmp "$BASE_MANIFEST" \
    "$MANIFEST_APPDIR/usr/share/goldencheetah/build-manifest" ||
    fail "embedded build manifest changed during installation"

FAKE_APPDIR="$TEMP_DIR/manifest.AppDir"
FAKE_APPIMAGE="$TEMP_DIR/manifest.AppImage"
FAKE_SBOM="$TEMP_DIR/manifest.AppImage.sbom.cdx.json"
mkdir -p "$FAKE_APPDIR/usr/share/goldencheetah"
install -m 0644 "$BASE_MANIFEST" \
    "$FAKE_APPDIR/usr/share/goldencheetah/build-manifest"
cat >"$FAKE_APPDIR/AppRun" <<'EOF'
#!/bin/sh
exit 0
EOF
chmod 0755 "$FAKE_APPDIR/AppRun"
cat >"$FAKE_APPDIR/manifest-fixture.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Manifest Fixture
Exec=AppRun
Icon=manifest-fixture
Categories=Utility;
EOF
cat >"$FAKE_APPDIR/manifest-fixture.svg" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">
  <rect width="16" height="16" fill="#247a52"/>
</svg>
EOF
install_appimage_dir_icon "$FAKE_APPDIR" manifest-fixture.svg
[ "$(readlink -- "$FAKE_APPDIR/.DirIcon")" = manifest-fixture.svg ] ||
    fail "AppImage directory icon target is incorrect"
install_appimage_dir_icon "$FAKE_APPDIR" manifest-fixture.svg ||
    fail "AppImage directory icon setup is not idempotent"
BAD_DIR_ICON_APPDIR="$TEMP_DIR/bad-dir-icon.AppDir"
mkdir -p "$BAD_DIR_ICON_APPDIR"
printf 'icon\n' >"$BAD_DIR_ICON_APPDIR/expected.png"
ln -s unexpected.png "$BAD_DIR_ICON_APPDIR/.DirIcon"
if install_appimage_dir_icon \
       "$BAD_DIR_ICON_APPDIR" expected.png >/dev/null 2>&1; then
    fail "an unexpected AppImage directory icon target was accepted"
fi
python3 - "$FAKE_APPDIR" "$REVISION_B" \
    "$FAKE_APPDIR/usr/share/goldencheetah/goldencheetah.cdx.json" <<'PY'
import hashlib
import json
import os
from pathlib import Path
import stat
import sys

appdir = Path(sys.argv[1])
revision = sys.argv[2]
output = Path(sys.argv[3])
components = []
for path in sorted(appdir.rglob("*")):
    if path == output or path.is_dir():
        continue
    relative = path.relative_to(appdir).as_posix()
    metadata = path.lstat()
    if path.is_symlink():
        component = {
            "bom-ref": "goldencheetah:symlink:" + relative,
            "type": "file",
            "name": relative,
            "properties": [
                {"name": "goldencheetah:role", "value": "payload-symlink"},
                {"name": "goldencheetah:symlink-target", "value": os.readlink(path)},
            ],
        }
    else:
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        component = {
            "bom-ref": f"goldencheetah:file:{relative}:{digest}",
            "type": "file",
            "name": relative,
            "hashes": [{"alg": "SHA-256", "content": digest}],
            "properties": [
                {"name": "goldencheetah:role", "value": "payload-file"},
                {"name": "goldencheetah:size", "value": str(metadata.st_size)},
                {"name": "goldencheetah:mode", "value": f"{stat.S_IMODE(metadata.st_mode):04o}"},
            ],
        }
    components.append(component)
components.sort(key=lambda item: item["bom-ref"])
application_ref = f"pkg:generic/goldencheetah@{revision}"
document = {
    "$schema": "http://cyclonedx.org/schema/bom-1.5.schema.json",
    "bomFormat": "CycloneDX",
    "specVersion": "1.5",
    "version": 1,
    "metadata": {
        "component": {
            "bom-ref": application_ref,
            "type": "application",
            "name": "GoldenCheetah",
            "version": revision,
            "licenses": [{"license": {"id": "GPL-2.0-or-later"}}],
        }
    },
    "components": components,
    "dependencies": [
        {"ref": application_ref, "dependsOn": [item["bom-ref"] for item in components]}
    ],
}
output.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")
PY
install -m 0644 \
    "$FAKE_APPDIR/usr/share/goldencheetah/goldencheetah.cdx.json" \
    "$FAKE_SBOM"
validate_appimage_sbom "$FAKE_SBOM"
normalize_appdir_mtimes "$FAKE_APPDIR"
run_packaging_appimage "$REPRO_TOOL" \
    --runtime-file "$REPRO_RUNTIME" "$FAKE_APPDIR" "$FAKE_APPIMAGE"
[ -x "$FAKE_APPIMAGE" ] || fail "manifest fixture AppImage was not generated"
SIDECAR_MANIFEST="$TEMP_DIR/manifest.AppImage.manifest"
finalize_appimage_manifest \
    "$FAKE_APPIMAGE" "$BASE_MANIFEST" "$SIDECAR_MANIFEST"
[ "$(stat -c '%a' "$SIDECAR_MANIFEST")" = 600 ] ||
    fail "AppImage sidecar manifest is not mode 0600"
grep -Fxq "appimage_sha256=$(sha256sum "$FAKE_APPIMAGE" | cut -d ' ' -f 1)" \
    "$SIDECAR_MANIFEST" || fail "AppImage hash is missing from sidecar"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST"

for unsafe_entry in artifacts sets; do
    UNSAFE_PARENT="$TEMP_DIR/unsafe-$unsafe_entry"
    UNSAFE_LINK="$UNSAFE_PARENT/release"
    UNSAFE_STORE="$UNSAFE_PARENT/.release.store"
    mkdir -p "$UNSAFE_STORE" "$UNSAFE_PARENT/outside"
    chmod 0700 "$UNSAFE_STORE"
    ln -s "$UNSAFE_PARENT/outside" "$UNSAFE_STORE/$unsafe_entry"
    if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
       GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
        promote_appimage_release \
            "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
            "$UNSAFE_LINK" >/dev/null 2>&1; then
        fail "promotion accepted a linked $unsafe_entry directory"
    fi
    [ -z "$(find "$UNSAFE_PARENT/outside" -mindepth 1 -print -quit)" ] ||
        fail "promotion wrote through linked $unsafe_entry directory"
done

for unsafe_entry in artifacts sets; do
    UNSAFE_PARENT="$TEMP_DIR/non-directory-$unsafe_entry"
    UNSAFE_LINK="$UNSAFE_PARENT/release"
    UNSAFE_STORE="$UNSAFE_PARENT/.release.store"
    mkdir -p "$UNSAFE_STORE"
    chmod 0700 "$UNSAFE_STORE"
    printf 'not a directory\n' >"$UNSAFE_STORE/$unsafe_entry"
    if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
       GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
        promote_appimage_release \
            "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
            "$UNSAFE_LINK" >/dev/null 2>&1; then
        fail "promotion accepted a non-directory $unsafe_entry path"
    fi
done

UNSAFE_LOCK_PARENT="$TEMP_DIR/unsafe-lock"
UNSAFE_LOCK_LINK="$UNSAFE_LOCK_PARENT/release"
UNSAFE_LOCK_STORE="$UNSAFE_LOCK_PARENT/.release.store"
mkdir -p "$UNSAFE_LOCK_STORE/artifacts" "$UNSAFE_LOCK_STORE/sets"
chmod 0700 "$UNSAFE_LOCK_STORE" "$UNSAFE_LOCK_STORE/artifacts" \
    "$UNSAFE_LOCK_STORE/sets"
printf 'do not truncate\n' >"$UNSAFE_LOCK_PARENT/outside-lock-target"
cp "$UNSAFE_LOCK_PARENT/outside-lock-target" \
    "$UNSAFE_LOCK_PARENT/outside-lock-expected"
ln -s "$UNSAFE_LOCK_PARENT/outside-lock-target" \
    "$UNSAFE_LOCK_STORE/promotion.lock"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
   GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
        "$UNSAFE_LOCK_LINK" >/dev/null 2>&1; then
    fail "promotion accepted a linked lock file"
fi
cmp "$UNSAFE_LOCK_PARENT/outside-lock-expected" \
    "$UNSAFE_LOCK_PARENT/outside-lock-target" ||
    fail "promotion truncated a linked lock target"

UNSAFE_LOCK_PARENT="$TEMP_DIR/non-file-lock"
UNSAFE_LOCK_LINK="$UNSAFE_LOCK_PARENT/release"
UNSAFE_LOCK_STORE="$UNSAFE_LOCK_PARENT/.release.store"
mkdir -p "$UNSAFE_LOCK_STORE/artifacts" "$UNSAFE_LOCK_STORE/sets" \
    "$UNSAFE_LOCK_STORE/promotion.lock"
chmod 0700 "$UNSAFE_LOCK_STORE" "$UNSAFE_LOCK_STORE/artifacts" \
    "$UNSAFE_LOCK_STORE/sets" "$UNSAFE_LOCK_STORE/promotion.lock"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
   GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
        "$UNSAFE_LOCK_LINK" >/dev/null 2>&1; then
    fail "promotion accepted a non-file lock path"
fi

RELEASE_LINK="$TEMP_DIR/GoldenCheetah-release"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
        "$RELEASE_LINK" >/dev/null
[ -L "$RELEASE_LINK" ] || fail "the release pointer is not a symlink"
cmp -s "$FAKE_APPIMAGE" "$RELEASE_LINK/latest.AppImage" ||
    fail "the first promoted image is not latest"
cmp -s "$FAKE_APPIMAGE" "$RELEASE_LINK/previous.AppImage" ||
    fail "the initial previous image is not valid"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$RELEASE_LINK/latest.AppImage" \
        "$RELEASE_LINK/latest.AppImage.manifest"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$RELEASE_LINK/previous.AppImage" \
        "$RELEASE_LINK/previous.AppImage.manifest"
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    verify_appimage_sbom \
        "$RELEASE_LINK/latest.AppImage" \
        "$RELEASE_LINK/latest.AppImage.sbom.cdx.json"
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    verify_appimage_sbom \
        "$RELEASE_LINK/previous.AppImage" \
        "$RELEASE_LINK/previous.AppImage.sbom.cdx.json"
FIRST_RELEASE_TARGET=$(readlink "$RELEASE_LINK")

SECOND_APPIMAGE="$TEMP_DIR/manifest-second.AppImage"
cp "$FAKE_APPIMAGE" "$SECOND_APPIMAGE"
printf '# second immutable image\n' >>"$SECOND_APPIMAGE"
chmod +x "$SECOND_APPIMAGE"
SECOND_MANIFEST="$SECOND_APPIMAGE.manifest"
finalize_appimage_manifest \
    "$SECOND_APPIMAGE" "$BASE_MANIFEST" "$SECOND_MANIFEST"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$SECOND_APPIMAGE" "$SECOND_MANIFEST" "$FAKE_SBOM" \
        "$RELEASE_LINK" >/dev/null
[ "$(readlink "$RELEASE_LINK")" != "$FIRST_RELEASE_TARGET" ] ||
    fail "release promotion did not rotate the generation pointer"
cmp -s "$SECOND_APPIMAGE" "$RELEASE_LINK/latest.AppImage" ||
    fail "the second promoted image is not latest"
cmp -s "$FAKE_APPIMAGE" "$RELEASE_LINK/previous.AppImage" ||
    fail "the former latest image did not become previous"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$RELEASE_LINK/latest.AppImage" \
        "$RELEASE_LINK/latest.AppImage.manifest"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$RELEASE_LINK/previous.AppImage" \
        "$RELEASE_LINK/previous.AppImage.manifest"
cmp -s "$FAKE_SBOM" \
    "$RELEASE_LINK/latest.AppImage.sbom.cdx.json" ||
    fail "the latest release SBOM was not promoted"
cmp -s "$FAKE_SBOM" \
    "$RELEASE_LINK/previous.AppImage.sbom.cdx.json" ||
    fail "the previous release SBOM was not retained"

LEGACY_RELEASE_LINK="$TEMP_DIR/GoldenCheetah-legacy-release"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" "$FAKE_SBOM" \
        "$LEGACY_RELEASE_LINK" >/dev/null
rm "$LEGACY_RELEASE_LINK/latest.AppImage.sbom.cdx.json" \
    "$LEGACY_RELEASE_LINK/previous.AppImage.sbom.cdx.json"
GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$SECOND_APPIMAGE" "$SECOND_MANIFEST" "$FAKE_SBOM" \
        "$LEGACY_RELEASE_LINK" >/dev/null
cmp -s "$SECOND_APPIMAGE" "$LEGACY_RELEASE_LINK/latest.AppImage" ||
    fail "legacy release migration did not publish the new image"
cmp -s "$SECOND_APPIMAGE" "$LEGACY_RELEASE_LINK/previous.AppImage" ||
    fail "legacy release migration retained an unverifiable rollback image"
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    verify_appimage_sbom \
        "$LEGACY_RELEASE_LINK/latest.AppImage" \
        "$LEGACY_RELEASE_LINK/latest.AppImage.sbom.cdx.json"
GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    verify_appimage_sbom \
        "$LEGACY_RELEASE_LINK/previous.AppImage" \
        "$LEGACY_RELEASE_LINK/previous.AppImage.sbom.cdx.json"

LEGACY_V1_BASE_MANIFEST="$TEMP_DIR/legacy-v1-build.manifest"
{
    printf '%s\n' 'goldencheetah_appimage_manifest=1'
    sed -n \
        -e '/^source_revision=/p' \
        -e '/^raw_elf_sha256=/p' \
        -e '/^toolchain=/p' \
        -e '/^strava_oauth_configured=/p' \
        "$BASE_MANIFEST"
} >"$LEGACY_V1_BASE_MANIFEST"
LEGACY_V1_APPDIR="$TEMP_DIR/legacy-v1.AppDir"
LEGACY_V1_APPIMAGE="$TEMP_DIR/legacy-v1.AppImage"
cp -a "$FAKE_APPDIR" "$LEGACY_V1_APPDIR"
install -m 0644 "$LEGACY_V1_BASE_MANIFEST" \
    "$LEGACY_V1_APPDIR/usr/share/goldencheetah/build-manifest"
normalize_appdir_mtimes "$LEGACY_V1_APPDIR"
run_packaging_appimage "$REPRO_TOOL" \
    --runtime-file "$REPRO_RUNTIME" \
    "$LEGACY_V1_APPDIR" "$LEGACY_V1_APPIMAGE"
LEGACY_V1_MANIFEST="$LEGACY_V1_APPIMAGE.manifest"
(
    umask 077
    cat "$LEGACY_V1_BASE_MANIFEST" >"$LEGACY_V1_MANIFEST"
    printf 'appimage_sha256=%s\n' \
        "$(sha256sum "$LEGACY_V1_APPIMAGE" | cut -d ' ' -f 1)" \
        >>"$LEGACY_V1_MANIFEST"
)
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$LEGACY_V1_APPIMAGE" "$LEGACY_V1_MANIFEST" \
        >/dev/null 2>&1; then
    fail "the production manifest verifier accepted legacy manifest version 1"
fi

seed_legacy_v1_release()
{
    local image=$1
    local manifest=$2
    local release_parent=$3
    local release_name=GoldenCheetah-release
    local release_store="$release_parent/.${release_name}.store"
    local image_hash revision artifact_id set_id

    image_hash=$(sed -n 's/^appimage_sha256=//p' "$manifest")
    revision=$(sed -n 's/^source_revision=//p' "$manifest")
    artifact_id="${revision}-${image_hash}"
    set_id="${image_hash}-${image_hash}"
    mkdir -p \
        "$release_store/artifacts/$artifact_id" \
        "$release_store/sets/$set_id"
    chmod 0700 \
        "$release_store" \
        "$release_store/artifacts" \
        "$release_store/artifacts/$artifact_id" \
        "$release_store/sets" \
        "$release_store/sets/$set_id"
    install -m 0755 "$image" \
        "$release_store/artifacts/$artifact_id/GoldenCheetah.AppImage"
    install -m 0600 "$manifest" \
        "$release_store/artifacts/$artifact_id/GoldenCheetah.AppImage.manifest"
    ln -s "../../artifacts/$artifact_id/GoldenCheetah.AppImage" \
        "$release_store/sets/$set_id/latest.AppImage"
    ln -s "../../artifacts/$artifact_id/GoldenCheetah.AppImage" \
        "$release_store/sets/$set_id/previous.AppImage"
    install -m 0600 "$manifest" \
        "$release_store/sets/$set_id/latest.AppImage.manifest"
    install -m 0600 "$manifest" \
        "$release_store/sets/$set_id/previous.AppImage.manifest"
    install -m 0600 /dev/null "$release_store/promotion.lock"
    ln -s ".${release_name}.store/sets/$set_id" \
        "$release_parent/$release_name"
}

LEGACY_V1_PARENT="$TEMP_DIR/legacy-v1-release-parent"
mkdir "$LEGACY_V1_PARENT"
seed_legacy_v1_release \
    "$LEGACY_V1_APPIMAGE" "$LEGACY_V1_MANIFEST" "$LEGACY_V1_PARENT"
LEGACY_V1_RELEASE_LINK="$LEGACY_V1_PARENT/GoldenCheetah-release"
if ! GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
     GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
        promote_appimage_release \
            "$SECOND_APPIMAGE" "$SECOND_MANIFEST" "$FAKE_SBOM" \
            "$LEGACY_V1_RELEASE_LINK" >/dev/null; then
    fail "manifest-version-1 release migration failed"
fi
cmp -s "$SECOND_APPIMAGE" "$LEGACY_V1_RELEASE_LINK/latest.AppImage" ||
    fail "manifest-version-1 migration did not publish the new image"
cmp -s "$SECOND_APPIMAGE" "$LEGACY_V1_RELEASE_LINK/previous.AppImage" ||
    fail "manifest-version-1 migration retained an unverifiable rollback image"

TAMPERED_V1_MANIFEST="$TEMP_DIR/legacy-v1-tampered.manifest"
cp "$LEGACY_V1_MANIFEST" "$TAMPERED_V1_MANIFEST"
sed -i \
    's/^source_revision=.*/source_revision=0000000000000000000000000000000000000000/' \
    "$TAMPERED_V1_MANIFEST"
LEGACY_V1_TAMPERED_PARENT="$TEMP_DIR/legacy-v1-tampered-parent"
mkdir "$LEGACY_V1_TAMPERED_PARENT"
seed_legacy_v1_release \
    "$LEGACY_V1_APPIMAGE" "$TAMPERED_V1_MANIFEST" \
    "$LEGACY_V1_TAMPERED_PARENT"
LEGACY_V1_TAMPERED_LINK="$LEGACY_V1_TAMPERED_PARENT/GoldenCheetah-release"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
   GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$SECOND_APPIMAGE" "$SECOND_MANIFEST" "$FAKE_SBOM" \
        "$LEGACY_V1_TAMPERED_LINK" >/dev/null 2>&1; then
    fail "promotion accepted a legacy sidecar that mismatches its AppImage"
fi
cmp -s "$LEGACY_V1_APPIMAGE" \
    "$LEGACY_V1_TAMPERED_LINK/latest.AppImage" ||
    fail "failed legacy migration changed the active release"

PUBLISHED_TARGET=$(readlink "$RELEASE_LINK")
THIRD_APPIMAGE="$TEMP_DIR/manifest-third.AppImage"
cp "$FAKE_APPIMAGE" "$THIRD_APPIMAGE"
printf '# third immutable image\n' >>"$THIRD_APPIMAGE"
chmod +x "$THIRD_APPIMAGE"
THIRD_MANIFEST="$THIRD_APPIMAGE.manifest"
finalize_appimage_manifest \
    "$THIRD_APPIMAGE" "$BASE_MANIFEST" "$THIRD_MANIFEST"
FAILING_SYNC_DIR="$TEMP_DIR/failing-sync"
mkdir "$FAILING_SYNC_DIR"
REAL_SYNC=$(command -v sync)
cat >"$FAILING_SYNC_DIR/sync" <<EOF
#!/bin/sh
count=0
test ! -f "$TEMP_DIR/sync-count" || count=\$(cat "$TEMP_DIR/sync-count")
count=\$((count + 1))
printf '%s\\n' "\$count" >"$TEMP_DIR/sync-count"
test "\$count" -ne 2 || exit 74
exec "$REAL_SYNC" "\$@"
EOF
chmod +x "$FAILING_SYNC_DIR/sync"
if PATH="$FAILING_SYNC_DIR:$PATH" \
   GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
   GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$THIRD_APPIMAGE" "$THIRD_MANIFEST" "$FAKE_SBOM" \
        "$RELEASE_LINK" \
        >/dev/null 2>&1; then
    fail "promotion reported success after a post-publication sync failure"
fi
[ "$(readlink "$RELEASE_LINK")" = "$PUBLISHED_TARGET" ] ||
    fail "late promotion failure did not restore the previous release"
cmp -s "$SECOND_APPIMAGE" "$RELEASE_LINK/latest.AppImage" ||
    fail "late promotion failure changed latest"
cmp -s "$FAKE_APPIMAGE" "$RELEASE_LINK/previous.AppImage" ||
    fail "late promotion failure changed previous"

printf '# invalid after finalization\n' >>"$SECOND_APPIMAGE"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
   GC_TEST_APPIMAGE_SBOM_ENTRYPOINT=true \
    promote_appimage_release \
        "$SECOND_APPIMAGE" "$SECOND_MANIFEST" "$FAKE_SBOM" \
        "$RELEASE_LINK" \
        >/dev/null 2>&1; then
    fail "a tampered image was promoted"
fi
[ "$(readlink "$RELEASE_LINK")" = "$PUBLISHED_TARGET" ] ||
    fail "failed promotion changed the active release"

cp "$SIDECAR_MANIFEST" "$TEMP_DIR/tampered-sidecar"
sed -i \
    's/^source_revision=.*/source_revision=0000000000000000000000000000000000000000/' \
    "$TEMP_DIR/tampered-sidecar"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest \
        "$FAKE_APPIMAGE" "$TEMP_DIR/tampered-sidecar" \
        >/dev/null 2>&1; then
    fail "a tampered sidecar manifest was accepted"
fi
printf '# tampered\n' >>"$FAKE_APPIMAGE"
if GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT=true \
    verify_appimage_manifest "$FAKE_APPIMAGE" "$SIDECAR_MANIFEST" \
        >/dev/null 2>&1; then
    fail "a modified AppImage was accepted"
fi

cat >"$TEMP_DIR/status-probe.c" <<'EOF'
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
#ifdef GC_STATUS_MODE
    const char *mode = GC_STATUS_MODE;
#else
    const char *mode = argv[0];
#endif

    if (argc != 2) {
        return 64;
    }
    if (strcmp(
            argv[1],
            "--goldencheetah-linux-keychain-status") == 0) {
        fputs(
            "goldencheetah_linux_keychain_status=1\n"
            "application=GoldenCheetah\n",
            stdout);
        if (strstr(mode, "keychain-disabled") != NULL) {
            fputs(
                "libsecret_compile_support=disabled\n"
                "libsecret_runtime=unavailable\n",
                stdout);
        } else if (strstr(
                       mode,
                       "keychain-unavailable") != NULL) {
            fputs(
                "libsecret_compile_support=enabled\n"
                "libsecret_runtime=unavailable\n",
                stdout);
        } else {
            fputs(
                "libsecret_compile_support=enabled\n"
                "libsecret_runtime=available\n",
                stdout);
        }
        return 0;
    }
    if (strcmp(
            argv[1],
            "--goldencheetah-build-status") != 0) {
        return 64;
    }

    fputs(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n",
        stdout);
    if (strstr(mode, "no-strava") == NULL) {
        fputs("strava_support=enabled\n", stdout);
    }
    if (strstr(mode, "oversized") != NULL) {
        for (int index = 0; index < 10000; ++index) {
            fputc('x', stdout);
        }
        return 0;
    }
    if (strstr(mode, "missing-newline") != NULL) {
        fputs(
            "strava_oauth=runtime_credentials\n"
            "strava_compile_fallback=configured",
            stdout);
        return 0;
    }
    if (strstr(mode, "extra-newline") != NULL) {
        fputs(
            "strava_oauth=runtime_credentials\n"
            "strava_compile_fallback=configured\n\n",
            stdout);
        return 0;
    }
    if (strstr(mode, "malformed") != NULL) {
        fputs("strava_oauth=maybe\n", stdout);
    } else {
        fputs("strava_oauth=runtime_credentials\n", stdout);
        fputs(
            strstr(mode, "unconfigured") != NULL
                ? "strava_compile_fallback=unavailable\n"
                : "strava_compile_fallback=configured\n",
            stdout);
    }
    return strstr(mode, "bad-exit") == NULL ? 0 : 1;
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    "$TEMP_DIR/status-probe.c" -o "$TEMP_DIR/configured"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"configured"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/configured-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"unconfigured"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/unconfigured-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"malformed"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/malformed-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"keychain-disabled"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/keychain-disabled-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"keychain-unavailable"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/keychain-unavailable-entry"
chmod +x "$TEMP_DIR/status-probe.c"
cp "$TEMP_DIR/configured" "$TEMP_DIR/unconfigured"
cp "$TEMP_DIR/configured" "$TEMP_DIR/malformed"
cp "$TEMP_DIR/configured" "$TEMP_DIR/no-strava"
cp "$TEMP_DIR/configured" "$TEMP_DIR/bad-exit"
cp "$TEMP_DIR/configured" "$TEMP_DIR/oversized"
cp "$TEMP_DIR/configured" "$TEMP_DIR/missing-newline"
cp "$TEMP_DIR/configured" "$TEMP_DIR/extra-newline"
cp "$TEMP_DIR/configured" "$TEMP_DIR/GoldenCheetah"
cat >"$TEMP_DIR/app-run-wrapper" <<'EOF'
#!/bin/sh
: "${APPDIR:?}" "${APPIMAGE:?}" "${OWD:?}"
[ -d "$APPDIR" ] && [ -f "$APPIMAGE" ] && [ -d "$OWD" ] || exit 65
exec "$APPDIR/GoldenCheetah" "$@"
EOF
chmod +x "$TEMP_DIR/app-run-wrapper"
mkdir "$TEMP_DIR/host-loader-override"
printf 'invalid host override\n' \
    >"$TEMP_DIR/host-loader-override/libc.so.6"
[ "$(LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
      strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "Strava status inherited an external library override"
[ "$(LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
      linux_keychain_entrypoint_status "$TEMP_DIR/configured-entry")" = \
  "Linux keychain runtime: available" ] ||
    fail "keychain status inherited an external library override"

DEPLOY_PROBE=$(create_linux_keychain_deploy_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR")
[ -x "$DEPLOY_PROBE" ] ||
    fail "Linux keychain deploy probe was not created"
LC_ALL=C readelf -d "$DEPLOY_PROBE" |
    grep -Eq '\(NEEDED\).*\[libsecret-1\.so\.0\]' ||
    fail "Linux keychain deploy probe does not require libsecret"
remove_linux_keychain_deploy_probe "$DEPLOY_PROBE"
[ ! -e "$DEPLOY_PROBE" ] ||
    fail "Linux keychain deploy probe was not removed"

run_packaging_appimage()
{
    local argument probe=
    for argument in "$@"; do
        case "$argument" in
        -executable=*) probe=${argument#-executable=} ;;
        esac
    done
    [ -f "$probe" ]
}
run_linuxdeployqt_with_keychain_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR" fake-linuxdeployqt ||
    fail "successful deployment with a temporary probe was rejected"
[ ! -e "$TEMP_DIR/.goldencheetah-libsecret-deploy-probe" ] ||
    fail "successful deployment left its temporary probe behind"

run_packaging_appimage()
{
    local argument probe=
    for argument in "$@"; do
        case "$argument" in
        -executable=*) probe=${argument#-executable=} ;;
        esac
    done
    rm -f -- "$probe"
    mkdir -- "$probe"
}
if run_linuxdeployqt_with_keychain_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR" fake-linuxdeployqt \
    >/dev/null 2>&1; then
    fail "deployment accepted a probe cleanup failure"
fi
rm -rf "$TEMP_DIR/.goldencheetah-libsecret-deploy-probe"

mkdir -p "$TEMP_DIR/libsecret/lib" "$TEMP_DIR/libsecret/bin"

build_fixture_dependency()
{
    local soname=$1
    local symbol=$2
    local source="$TEMP_DIR/libsecret/${soname}.c"

    printf 'void %s(void) {}\n' "$symbol" >"$source"
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
        -Wl,-soname,"$soname" \
        "$source" -o "$TEMP_DIR/libsecret/lib/$soname"
}

build_fixture_dependency \
    libglib-2.0.so.0 gc_glib_dependency
build_fixture_dependency \
    libgio-2.0.so.0 gc_gio_dependency
build_fixture_dependency \
    libgobject-2.0.so.0 gc_gobject_dependency
build_fixture_dependency \
    libgpg-error.so.0 gc_gpg_error_dependency
build_fixture_dependency \
    libgc-absolute.so.1 gc_absolute_dependency
build_fixture_dependency \
    libgc-linked.so.1 gc_linked_dependency
cat >"$TEMP_DIR/libsecret/libgcrypt-fixture.c" <<'EOF'
extern void gc_gpg_error_dependency(void);
void gc_gcrypt_dependency(void)
{
    gc_gpg_error_dependency();
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libgcrypt.so.20 \
    "$TEMP_DIR/libsecret/libgcrypt-fixture.c" \
    "$TEMP_DIR/libsecret/lib/libgpg-error.so.0" \
    -o "$TEMP_DIR/libsecret/lib/libgcrypt.so.20"

cat >"$TEMP_DIR/libsecret/libsecret-fixture.c" <<'EOF'
#define LIBSECRET_SYMBOL(name) void name(void) {}
LIBSECRET_SYMBOL(secret_password_lookup)
LIBSECRET_SYMBOL(secret_password_lookup_finish)
LIBSECRET_SYMBOL(secret_password_store)
LIBSECRET_SYMBOL(secret_password_store_finish)
LIBSECRET_SYMBOL(secret_password_clear)
LIBSECRET_SYMBOL(secret_password_clear_finish)
LIBSECRET_SYMBOL(secret_password_free)
LIBSECRET_SYMBOL(secret_error_get_quark)
extern void gc_glib_dependency(void);
extern void gc_gio_dependency(void);
extern void gc_gobject_dependency(void);
extern void gc_gcrypt_dependency(void);
void gc_use_dependencies(void)
{
    gc_glib_dependency();
    gc_gio_dependency();
    gc_gobject_dependency();
    gc_gcrypt_dependency();
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/libsecret-fixture.c" \
    "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    -o "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0"
cat >"$TEMP_DIR/libsecret/incomplete-fixture.c" <<'EOF'
void secret_password_lookup(void) {}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/incomplete-fixture.c" \
    -o "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0"
patchelf --set-rpath '$ORIGIN' \
    "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0"
cat >"$TEMP_DIR/libsecret/undefined-fixture.c" <<'EOF'
#define LIBSECRET_REFERENCE(name) \
    extern void name(void); \
    void *name##_reference = (void *)&name
LIBSECRET_REFERENCE(secret_password_lookup);
LIBSECRET_REFERENCE(secret_password_lookup_finish);
LIBSECRET_REFERENCE(secret_password_store);
LIBSECRET_REFERENCE(secret_password_store_finish);
LIBSECRET_REFERENCE(secret_password_clear);
LIBSECRET_REFERENCE(secret_password_clear_finish);
LIBSECRET_REFERENCE(secret_password_free);
LIBSECRET_REFERENCE(secret_error_get_quark);
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/undefined-fixture.c" \
    -o "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0"
patchelf --set-rpath '$ORIGIN' \
    "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0"
ln -s libsecret-1.so.0.0 \
    "$TEMP_DIR/libsecret/lib/libsecret-1.so.0"
cat >"$TEMP_DIR/libsecret/bin/pkg-config" <<EOF
#!/bin/sh
[ "\$1" = "--variable=libdir" ] || exit 1
case "\$2" in
libsecret-1|gpg-error) ;;
*) exit 1 ;;
esac
printf '%s\n' "$TEMP_DIR/libsecret/lib"
EOF
chmod +x "$TEMP_DIR/libsecret/bin/pkg-config"
cat >"$TEMP_DIR/libsecret/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: libsecret
Files: *
Copyright: 2009-2024 The libsecret authors
License: LGPL-2.1+
 GNU Lesser General Public
 License as published by the Free Software Foundation
EOF
QTKEYCHAIN_LICENSE_FIXTURE="$REPO_ROOT/contrib/qtkeychain/COPYING"
LGPL21_LICENSE_FIXTURE="/usr/share/common-licenses/LGPL-2.1"
[ -r "$QTKEYCHAIN_LICENSE_FIXTURE" ] ||
    fail "reviewed QtKeychain license fixture is missing"
[ -r "$LGPL21_LICENSE_FIXTURE" ] ||
    fail "reviewed LGPL-2.1 license fixture is missing"

KEYCHAIN_APPDIR="$TEMP_DIR/keychain.AppDir"
KEYCHAIN_TRANSFORM_MANIFEST="$TEMP_DIR/keychain-transformations.json"
mkdir -p "$KEYCHAIN_APPDIR/lib" \
    "$KEYCHAIN_APPDIR/qml/QtQml/Models"
cp "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    "$KEYCHAIN_APPDIR/lib/"
printf 'prior authenticated source\n' >"$TEMP_DIR/prior-source.so.1"
printf 'prior transformed output\n' >"$KEYCHAIN_APPDIR/lib/libprior.so.1"
printf 'Qt QML plugin source\n' >"$TEMP_DIR/libmodelsplugin.so"
printf 'transformed Qt QML plugin\n' \
    >"$KEYCHAIN_APPDIR/qml/QtQml/Models/libmodelsplugin.so"
python3 - \
    "$TEMP_DIR/prior-source.so.1" \
    "$KEYCHAIN_APPDIR/lib/libprior.so.1" \
    "$TEMP_DIR/libmodelsplugin.so" \
    "$KEYCHAIN_APPDIR/qml/QtQml/Models/libmodelsplugin.so" \
    "$KEYCHAIN_TRANSFORM_MANIFEST" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

source = Path(sys.argv[1]).resolve()
output = Path(sys.argv[2]).resolve()
qml_source = Path(sys.argv[3]).resolve()
qml_output = Path(sys.argv[4]).resolve()
manifest = Path(sys.argv[5])
document = {
    "format": "goldencheetah-transformed-runtime-1",
    "libraries": [
        {
            "output_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
            "path": "lib/libprior.so.1",
            "source_path": str(source),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "transformation": (
                "linuxdeployqt-no-strip:"
                + "d" * 64
                + ":rpath=$ORIGIN"
            ),
        },
        {
            "output_sha256": hashlib.sha256(qml_output.read_bytes()).hexdigest(),
            "path": "qml/QtQml/Models/libmodelsplugin.so",
            "source_path": str(qml_source),
            "source_sha256": hashlib.sha256(qml_source.read_bytes()).hexdigest(),
            "transformation": (
                "linuxdeployqt-no-strip:"
                + "d" * 64
                + ":rpath=relative-lib"
            ),
        },
    ],
}
manifest.write_text(json.dumps(document), encoding="utf-8")
PY
PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBGCRYPT_RUNTIME_FILE="$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$KEYCHAIN_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE" \
        "$KEYCHAIN_TRANSFORM_MANIFEST"
[ -f "$KEYCHAIN_TRANSFORM_MANIFEST" ] ||
    fail "Linux keychain transformation manifest was not created"
python3 - "$KEYCHAIN_APPDIR" "$KEYCHAIN_TRANSFORM_MANIFEST" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

appdir = Path(sys.argv[1])
manifest = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
assert manifest["format"] == "goldencheetah-transformed-runtime-1"
assert [entry["path"] for entry in manifest["libraries"]] == [
    "lib/libgcrypt.so.20",
    "lib/libprior.so.1",
    "lib/libsecret-1.so.0",
    "qml/QtQml/Models/libmodelsplugin.so",
]
for entry in manifest["libraries"]:
    if entry["path"].startswith("qml/"):
        assert entry["transformation"].endswith("rpath=relative-lib")
    elif entry["path"] == "lib/libprior.so.1":
        assert entry["transformation"].startswith("linuxdeployqt-no-strip:")
    else:
        assert entry["transformation"] == "patchelf-set-rpath:$ORIGIN"
    output = appdir / entry["path"]
    source = Path(entry["source_path"])
    assert source.is_file() and not source.is_symlink()
    assert hashlib.sha256(output.read_bytes()).hexdigest() == entry["output_sha256"]
    assert hashlib.sha256(source.read_bytes()).hexdigest() == entry["source_sha256"]
PY
[ "$(linux_keychain_runtime_status "$KEYCHAIN_APPDIR")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "installed Linux keychain runtime was not reported"
cmp "$TEMP_DIR/libsecret/lib/libgpg-error.so.0" \
    "$KEYCHAIN_APPDIR/lib/libgpg-error.so.0" ||
    fail "transitive libgpg-error runtime was not installed"
cmp "$TEMP_DIR/libsecret/copyright" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright" ||
    fail "libsecret copyright was not copied exactly"
cmp "$QTKEYCHAIN_LICENSE_FIXTURE" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING" ||
    fail "QtKeychain license was not copied exactly"
cmp "$LGPL21_LICENSE_FIXTURE" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1" ||
    fail "complete LGPL-2.1 license was not copied exactly"
LC_ALL=C readelf -d "$KEYCHAIN_APPDIR/lib/libsecret-1.so.0" |
    grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]' ||
    fail "bundled libsecret does not resolve dependencies from its directory"
LC_ALL=C readelf -d "$KEYCHAIN_APPDIR/lib/libgcrypt.so.20" |
    grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]' ||
    fail "bundled libgcrypt does not resolve dependencies from its directory"
KEYCHAIN_LDD_OUTPUT=$(
    env -u LD_LIBRARY_PATH LC_ALL=C \
        ldd "$KEYCHAIN_APPDIR/lib/libsecret-1.so.0"
)
for dependency in \
    libglib-2.0.so.0 \
    libgio-2.0.so.0 \
    libgobject-2.0.so.0 \
    libgcrypt.so.20 \
    libgpg-error.so.0; do
    printf '%s\n' "$KEYCHAIN_LDD_OUTPUT" |
        grep -Fq -- \
            "$dependency => $KEYCHAIN_APPDIR/lib/$dependency " ||
        fail "$dependency did not resolve from the AppDir"
done

LINKED_LIB_APPDIR="$TEMP_DIR/linked-lib-keychain.AppDir"
LINKED_LIB_OUTSIDE="$TEMP_DIR/linked-lib-outside"
mkdir -p "$LINKED_LIB_APPDIR" "$LINKED_LIB_OUTSIDE"
ln -s "$LINKED_LIB_OUTSIDE" "$LINKED_LIB_APPDIR/lib"
if PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBGCRYPT_RUNTIME_FILE="$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$LINKED_LIB_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE" \
        "$TEMP_DIR/linked-lib-transformations.json" \
        >/dev/null 2>&1; then
    fail "installer accepted a linked AppDir library directory"
fi
[ ! -e "$LINKED_LIB_OUTSIDE/libsecret-1.so.0" ] ||
    fail "installer wrote libsecret outside a linked AppDir"

LINKED_LICENSE_APPDIR="$TEMP_DIR/linked-license-keychain.AppDir"
LINKED_LICENSE_OUTSIDE="$TEMP_DIR/linked-license-outside"
mkdir -p \
    "$LINKED_LICENSE_APPDIR/lib" \
    "$LINKED_LICENSE_APPDIR/usr/share/doc/GoldenCheetah" \
    "$LINKED_LICENSE_OUTSIDE"
cp "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    "$LINKED_LICENSE_APPDIR/lib/"
ln -s "$LINKED_LICENSE_OUTSIDE" \
    "$LINKED_LICENSE_APPDIR/usr/share/doc/GoldenCheetah/licenses"
if PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBGCRYPT_RUNTIME_FILE="$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$LINKED_LICENSE_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE" \
        "$TEMP_DIR/linked-license-transformations.json" \
        >/dev/null 2>&1; then
    fail "installer accepted a linked AppDir license directory"
fi
[ ! -e "$LINKED_LICENSE_OUTSIDE/libsecret-copyright" ] ||
    fail "installer wrote a copyright outside a linked AppDir"

INCOMPLETE_APPDIR="$TEMP_DIR/incomplete-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$INCOMPLETE_APPDIR"
cp "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0" \
    "$INCOMPLETE_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$INCOMPLETE_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime without required QtKeychain symbols was accepted"
fi

UNDEFINED_APPDIR="$TEMP_DIR/undefined-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$UNDEFINED_APPDIR"
cp "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0" \
    "$UNDEFINED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$UNDEFINED_APPDIR" \
    >/dev/null 2>&1; then
    fail "undefined QtKeychain symbols were accepted as implementations"
fi

MISSING_DEPENDENCY_APPDIR="$TEMP_DIR/missing-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$MISSING_DEPENDENCY_APPDIR"
rm "$MISSING_DEPENDENCY_APPDIR/lib/libgcrypt.so.20"
if linux_keychain_runtime_status "$MISSING_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with a host-only dependency was accepted"
fi

MISSING_TRANSITIVE_APPDIR="$TEMP_DIR/missing-transitive-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$MISSING_TRANSITIVE_APPDIR"
rm "$MISSING_TRANSITIVE_APPDIR/lib/libgpg-error.so.0"
if linux_keychain_runtime_status "$MISSING_TRANSITIVE_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with host-only libgpg-error was accepted"
fi

UNRESOLVED_APPDIR="$TEMP_DIR/unresolved-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$UNRESOLVED_APPDIR"
patchelf --add-needed libgc-missing.so.1 \
    "$UNRESOLVED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$UNRESOLVED_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with an unresolved dependency was accepted"
fi

HOST_DEPENDENCY_APPDIR="$TEMP_DIR/host-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$HOST_DEPENDENCY_APPDIR"
patchelf --add-needed libstdc++.so.6 \
    "$HOST_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$HOST_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with an unexpected host dependency was accepted"
fi

ABSOLUTE_DEPENDENCY_APPDIR="$TEMP_DIR/absolute-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$ABSOLUTE_DEPENDENCY_APPDIR"
patchelf --add-needed \
    "$TEMP_DIR/libsecret/lib/libgc-absolute.so.1" \
    "$ABSOLUTE_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$ABSOLUTE_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "an absolute dependency outside the AppDir was accepted"
fi

LINKED_DEPENDENCY_APPDIR="$TEMP_DIR/linked-dependency-keychain.AppDir"
LINKED_DEPENDENCY_OUTSIDE="$TEMP_DIR/linked-dependency-outside"
cp -a "$KEYCHAIN_APPDIR" "$LINKED_DEPENDENCY_APPDIR"
mkdir "$LINKED_DEPENDENCY_OUTSIDE"
cp "$TEMP_DIR/libsecret/lib/libgc-linked.so.1" \
    "$LINKED_DEPENDENCY_OUTSIDE/libgc-linked.so.1"
ln -s "$LINKED_DEPENDENCY_OUTSIDE/libgc-linked.so.1" \
    "$LINKED_DEPENDENCY_APPDIR/lib/libgc-linked.so.1"
patchelf --add-needed libgc-linked.so.1 \
    "$LINKED_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$LINKED_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "a linked dependency escaping the AppDir was accepted"
fi

ESCAPED_APPDIR="$TEMP_DIR/escaped-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$ESCAPED_APPDIR"
rm "$ESCAPED_APPDIR/lib/libsecret-1.so.0"
ln -s "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0" \
    "$ESCAPED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$ESCAPED_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret symlink escaping the AppDir was accepted"
fi

rm "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status "$KEYCHAIN_APPDIR" \
    >/dev/null 2>&1; then
    fail "Linux keychain runtime without its copyright was accepted"
fi
cp "$TEMP_DIR/libsecret/copyright" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"

WRONG_LICENSE_APPDIR="$TEMP_DIR/wrong-license-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$WRONG_LICENSE_APPDIR"
printf 'wrong\n' \
    >"$WRONG_LICENSE_APPDIR/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1"
if linux_keychain_runtime_status "$WRONG_LICENSE_APPDIR" \
    >/dev/null 2>&1; then
    fail "incorrect LGPL-2.1 license content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" "$TEMP_DIR/wrong-qt-license-keychain.AppDir"
printf 'wrong\n' \
    >"$TEMP_DIR/wrong-qt-license-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING"
if linux_keychain_runtime_status \
    "$TEMP_DIR/wrong-qt-license-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "incorrect QtKeychain license content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" "$TEMP_DIR/wrong-copyright-keychain.AppDir"
printf 'wrong\n' \
    >"$TEMP_DIR/wrong-copyright-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status \
    "$TEMP_DIR/wrong-copyright-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "incorrect libsecret copyright content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir"
sed -i '/^Copyright:/d' \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "libsecret copyright without a copyright notice was accepted"
fi

printf '\177ELF\002\001\001\000AI\001compressed-appimage-payload' \
    >"$TEMP_DIR/type1.AppImage"
printf '\177ELF\002\001\001\000AI\002compressed-appimage-payload' \
    >"$TEMP_DIR/type2.AppImage"
chmod +x "$TEMP_DIR/type1.AppImage" "$TEMP_DIR/type2.AppImage"

[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime-only Strava support was not reported"
[ "$(strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime Strava support with a fallback was not reported"
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
if strava_oauth_build_status "$TEMP_DIR/oversized" \
    >/dev/null 2>&1; then
    fail "an oversized build-status response was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/missing-newline" \
    >/dev/null 2>&1; then
    fail "a build-status response without its final newline was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/extra-newline" \
    >/dev/null 2>&1; then
    fail "a build-status response with trailing blank lines was accepted"
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
    fail "runtime Strava support was rejected by the release gate"
require_strava_oauth_build "$TEMP_DIR/unconfigured" >/dev/null ||
    fail "runtime-only Strava support was rejected by the release gate"
require_configured_strava_oauth_build "$TEMP_DIR/configured" >/dev/null ||
    fail "private build rejected a configured compile fallback"
if require_configured_strava_oauth_build "$TEMP_DIR/unconfigured" \
    >/dev/null 2>&1; then
    fail "private build accepted an unavailable compile fallback"
fi
require_unconfigured_strava_oauth_build "$TEMP_DIR/unconfigured" >/dev/null ||
    fail "credential-free build rejected an unavailable compile fallback"
if require_unconfigured_strava_oauth_build "$TEMP_DIR/configured" \
    >/dev/null 2>&1; then
    fail "credential-free build accepted an embedded compile fallback"
fi
if require_strava_oauth_build "$TEMP_DIR/missing" \
    >/dev/null 2>&1; then
    fail "release gate accepted a missing executable"
fi

GC_TEST_APPIMAGE_SIDECAR="$TEMP_DIR/configured"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
trusted_appimage_extract()
{
    local image=$1
    local destination=$2
    local app_root="$destination/squashfs-root"

    [ "$image" = "$TEMP_DIR/type2.AppImage" ] ||
        fail "AppImage status extracted an unexpected fixture"
    [ -d "$destination" ] &&
        [ -z "$(find "$destination" -mindepth 1 -print -quit)" ] ||
        fail "AppImage status did not provide an empty extraction directory"
    mkdir -p "$app_root"
    cp "$GC_TEST_APPIMAGE_SIDECAR" \
        "$app_root/GoldenCheetah"
    cp "$GC_TEST_APPIMAGE_ENTRY" \
        "$app_root/$GC_TEST_APPIMAGE_ENTRY_NAME"
    ln -s "$GC_TEST_APPIMAGE_ENTRY_NAME" \
        "$app_root/AppRun"
    if [ -n "${GC_TEST_APPIMAGE_LIBSECRET:-}" ]; then
        mkdir -p \
            "$app_root/lib" \
            "$app_root/usr/share/doc/GoldenCheetah/licenses"
        cp "$GC_TEST_APPIMAGE_LIBSECRET" \
            "$app_root/lib/libsecret-1.so.0"
        cp \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libglib-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgio-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgobject-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgcrypt.so.20" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgpg-error.so.0" \
            "$app_root/lib/"
        cp "$GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT" \
            "$app_root/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
        cp "$GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE" \
            "$app_root/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING"
        cp "$GC_TEST_APPIMAGE_LIBSECRET_LICENSE" \
            "$app_root/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1"
    fi
}

[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime Strava support in the package was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "configured packaged GoldenCheetah was rejected"
require_configured_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "private AppImage rejected a configured compile fallback"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/app-run-wrapper"
GC_TEST_APPIMAGE_ENTRY_NAME="app-run-wrapper"
WRAPPED_STRAVA_STATUS=$(
    APPDIR="$TEMP_DIR/stale-appdir" \
        APPIMAGE="$TEMP_DIR/stale.AppImage" \
        OWD="$TEMP_DIR/stale-owd" \
        LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
        LD_PRELOAD="$KEYCHAIN_APPDIR/lib/libglib-2.0.so.0" \
        strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage"
)
[ "$WRAPPED_STRAVA_STATUS" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "a valid shell AppRun wrapper was rejected for Strava status"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/unconfigured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="unconfigured-entry"
[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime-only packaged GoldenCheetah was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "release gate rejected runtime-only Strava support"
if require_configured_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "private AppImage accepted an unavailable compile fallback"
fi
require_unconfigured_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "credential-free AppImage rejected an unavailable compile fallback"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
if require_unconfigured_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "credential-free AppImage accepted an embedded compile fallback"
fi
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/malformed-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="malformed-entry"
if strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "malformed packaged GoldenCheetah status was accepted"
fi
if strava_oauth_appimage_status "$TEMP_DIR/type1.AppImage" \
    >/dev/null 2>&1; then
    fail "unsupported Type 1 AppImage extraction was accepted"
fi

GC_TEST_APPIMAGE_LIBSECRET="$KEYCHAIN_APPDIR/lib/libsecret-1.so.0"
GC_TEST_APPIMAGE_DEPENDENCY_DIR="$KEYCHAIN_APPDIR/lib"
GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/libsecret/copyright"
GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE="$QTKEYCHAIN_LICENSE_FIXTURE"
GC_TEST_APPIMAGE_LIBSECRET_LICENSE="$LGPL21_LICENSE_FIXTURE"
[ "$(linux_keychain_entrypoint_status "$TEMP_DIR/configured-entry")" = \
  "Linux keychain runtime: available" ] ||
    fail "compiled and available Linux keychain entrypoint was not reported"
[ "$(APPDIR="$TEMP_DIR" \
      APPIMAGE="$TEMP_DIR/type2.AppImage" \
      OWD="$TEMP_DIR" \
      linux_keychain_entrypoint_status "$TEMP_DIR/app-run-wrapper")" = \
  "Linux keychain runtime: available" ] ||
    fail "a valid shell AppRun wrapper was rejected for keychain status"
[ "$(linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "packaged Linux keychain runtime was not reported"
require_linux_keychain_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "packaged Linux keychain runtime was rejected"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/app-run-wrapper"
GC_TEST_APPIMAGE_ENTRY_NAME="app-run-wrapper"
WRAPPED_KEYCHAIN_STATUS=$(
    APPDIR="$TEMP_DIR/stale-appdir" \
        APPIMAGE="$TEMP_DIR/stale.AppImage" \
        OWD="$TEMP_DIR/stale-owd" \
        linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage"
)
[ "$WRAPPED_KEYCHAIN_STATUS" = \
  "Linux keychain runtime: bundled" ] ||
    fail "packaged shell AppRun wrapper was rejected for keychain status"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"

GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/missing-copyright"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged Linux keychain runtime without copyright was accepted"
fi
if require_linux_keychain_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "release gate accepted a keychain runtime without copyright"
fi

GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/libsecret/copyright"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/keychain-disabled-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="keychain-disabled-entry"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged binary without compiled libsecret support was accepted"
fi
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/keychain-unavailable-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="keychain-unavailable-entry"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged binary with unavailable libsecret runtime was accepted"
fi

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
GC_TEST_APPIMAGE_LIBSECRET=
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged image without libsecret was accepted"
fi

assert_contains "$SUPPORT" \
    'trap cleanup_status_home EXIT'
assert_contains "$SUPPORT" \
    'trap cleanup_extract_dir EXIT'

grep -Eq '^sip[[:space:]]*==[[:space:]]*6\.15\.1$' "$REQUIREMENTS" ||
    fail "test must be reviewed when the pinned SIP version changes"

[ -r "$APPIMAGE_REQUIREMENTS" ] ||
    fail "missing hash-locked AppImage Python requirements"
if grep -Ev '^[[:space:]]*(#|$|--hash=sha256:)' \
       "$APPIMAGE_REQUIREMENTS" |
   grep -Ev '^[[:space:]]*[A-Za-z0-9_.-]+==[^[:space:]\\]+[[:space:]]*\\?$' \
       >/dev/null; then
    fail "AppImage Python requirements contain an unpinned entry"
fi
grep -Fq -- '--hash=sha256:' "$APPIMAGE_REQUIREMENTS" ||
    fail "AppImage Python requirements contain no artifact hashes"
if grep -Ev '^[[:space:]]*(#|$)' "$REQUIREMENTS" |
   grep -Ev '^[[:space:]]*[A-Za-z0-9_.-]+[[:space:]]*==[[:space:]]*[^[:space:]]+[[:space:]]*$' \
       >/dev/null; then
    fail "cross-platform Python requirements contain an unpinned entry"
fi

assert_contains "$SUPPORT" \
    'download_verified_file "$LINUXDEPLOYQT_URL" "$LINUXDEPLOYQT_FILE" "$LINUXDEPLOYQT_SHA256"'
assert_contains "$SUPPORT" \
    'download_verified_file "$APPIMAGETOOL_URL" "$APPIMAGETOOL_FILE" "$APPIMAGETOOL_SHA256"'
assert_contains "$SUPPORT" 'PIP_CONFIG_FILE=/dev/null'
assert_contains "$SUPPORT" 'PYTHONDONTWRITEBYTECODE=1'
assert_contains "$SUPPORT" \
    'pip install -q --isolated --disable-pip-version-check --no-input'
assert_contains "$SUPPORT" '--no-cache-dir --no-compile'
assert_contains "$SUPPORT" '--only-binary=:all:'
assert_contains "$SUPPORT" \
    '--report "$report_path" -r "$requirements_path"'
if grep -Fq 'pip install --upgrade pip' "$SUPPORT"; then
    fail "AppImage packaging upgrades pip from an unpinned index target"
fi

PYTHON_PATH_CHECK="$TEMP_DIR/python-path-check"
mkdir -p "$PYTHON_PATH_CHECK/appdir"
printf 'fixture==1.0 --hash=sha256:%064d\n' 0 >"$PYTHON_PATH_CHECK/lock"
: >"$PYTHON_PATH_CHECK/report"
ln -s lock "$PYTHON_PATH_CHECK/linked-lock"
ln -s appdir "$PYTHON_PATH_CHECK/linked-appdir"
ln -s report "$PYTHON_PATH_CHECK/linked-report"
if install_embedded_python \
    "$PYTHON_PATH_CHECK/linked-lock" "$PYTHON_PATH_CHECK/appdir" \
    "$PYTHON_PATH_CHECK/report" >/dev/null 2>&1; then
    fail "embedded Python accepted a linked requirements lock"
fi
if install_embedded_python \
    "$PYTHON_PATH_CHECK/lock" "$PYTHON_PATH_CHECK/linked-appdir" \
    "$PYTHON_PATH_CHECK/report" >/dev/null 2>&1; then
    fail "embedded Python accepted a linked AppDir"
fi
if install_embedded_python \
    "$PYTHON_PATH_CHECK/lock" "$PYTHON_PATH_CHECK/appdir" \
    "$PYTHON_PATH_CHECK/linked-report" >/dev/null 2>&1; then
    fail "embedded Python accepted a linked pip report"
fi
mkdir "$PYTHON_PATH_CHECK/outside-usr" \
    "$PYTHON_PATH_CHECK/outside-nested" \
    "$PYTHON_PATH_CHECK/appdir-with-nested-link" \
    "$PYTHON_PATH_CHECK/appdir-with-nested-link/usr"
ln -s "$PYTHON_PATH_CHECK/outside-usr" \
    "$PYTHON_PATH_CHECK/appdir/usr"
ln -s "$PYTHON_PATH_CHECK/outside-nested" \
    "$PYTHON_PATH_CHECK/appdir-with-nested-link/usr/lib"
if install_embedded_python \
    "$PYTHON_PATH_CHECK/lock" "$PYTHON_PATH_CHECK/appdir" \
    "$PYTHON_PATH_CHECK/report" >/dev/null 2>&1; then
    fail "embedded Python accepted a linked usr payload root"
fi
if install_embedded_python \
    "$PYTHON_PATH_CHECK/lock" \
    "$PYTHON_PATH_CHECK/appdir-with-nested-link" \
    "$PYTHON_PATH_CHECK/report" >/dev/null 2>&1; then
    fail "embedded Python accepted a nested payload symlink"
fi
[ -z "$(find "$PYTHON_PATH_CHECK/outside-usr" \
              "$PYTHON_PATH_CHECK/outside-nested" \
              -mindepth 1 -print -quit)" ] ||
    fail "embedded Python wrote through a linked payload root"

[ -r "$SBOM_GENERATOR" ] || fail "missing AppImage SBOM generator"
[ -r "$RUNTIME_PROVENANCE_GENERATOR" ] ||
    fail "missing AppImage runtime provenance generator"
SBOM_APPDIR="$TEMP_DIR/sbom-appdir"
SBOM_PYTHON_SITE="$SBOM_APPDIR/opt/python3.11/lib/python3.11/site-packages"
mkdir -p "$SBOM_APPDIR/usr/lib" "$SBOM_APPDIR/usr/share/goldencheetah" \
    "$SBOM_PYTHON_SITE/fixture_package-1.2.3.dist-info"
install_box2d_license \
    "$SBOM_APPDIR" "$REPO_ROOT/vendor/box2d-3.1.1/LICENSE"
printf 'application binary\n' >"$SBOM_APPDIR/GoldenCheetah"
printf 'runtime library\n' >"$SBOM_APPDIR/usr/lib/libfixture.so"
printf 'Qt runtime library\n' >"$SBOM_APPDIR/usr/lib/libQt6Core.so.6.8.3"
printf 'libsecret runtime library\n' \
    >"$SBOM_APPDIR/usr/lib/libsecret-1.so.0.0.0"
ln -s libfixture.so "$SBOM_APPDIR/usr/lib/libfixture-current.so"
ln -s usr/lib "$SBOM_APPDIR/lib"
cp "$BASE_MANIFEST" \
    "$SBOM_APPDIR/usr/share/goldencheetah/build-manifest"
SBOM_LOCK="$TEMP_DIR/sbom-requirements.lock"
cat >"$SBOM_LOCK" <<'EOF'
fixture_package==1.2.3 \
    --hash=sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa \
    --hash=sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
EOF
SBOM_PIP_REPORT="$TEMP_DIR/sbom-pip-report.json"
cat >"$SBOM_PIP_REPORT" <<'EOF'
{
  "version": "1",
  "pip_version": "26.1.2",
  "install": [
    {
      "download_info": {
        "url": "https://files.pythonhosted.org/packages/fixture_package-1.2.3-py3-none-any.whl",
        "archive_info": {
          "hashes": {
            "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
          }
        }
      },
      "metadata": {"name": "Fixture_Package", "version": "1.2.3"}
    }
  ]
}
EOF
cat >"$SBOM_PYTHON_SITE/fixture_package-1.2.3.dist-info/METADATA" <<'EOF'
Metadata-Version: 2.1
Name: Fixture_Package
Version: 1.2.3
License-Expression: MIT
EOF
SBOM_PYTHON_METADATA_SHA256=$(sha256sum \
    "$SBOM_PYTHON_SITE/fixture_package-1.2.3.dist-info/METADATA" |
    cut -d' ' -f1)
cat >"${SBOM_PIP_REPORT}.runtime-libraries.json" <<EOF
{
  "format": "goldencheetah-python-source-runtime-2",
  "source_sha256": "$PYTHON_APPIMAGE_SHA256",
  "distributions": [],
  "files": [],
  "symlinks": []
}
EOF
SBOM_LOCK_SHA256=$(sha256sum "$SBOM_LOCK" | cut -d' ' -f1)
cat >"${SBOM_PIP_REPORT}.wheel-records.json" <<EOF
{
  "format": "goldencheetah-python-wheel-records-1",
  "requirements_lock_sha256": "$SBOM_LOCK_SHA256",
  "packages": [
    {
      "artifact": "fixture_package-1.2.3-py3-none-any.whl",
      "license": "MIT",
      "metadata_path": "opt/python3.11/lib/python3.11/site-packages/fixture_package-1.2.3.dist-info/METADATA",
      "metadata_sha256": "$SBOM_PYTHON_METADATA_SHA256",
      "name": "fixture-package",
      "record_sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
      "runtime_libraries": [],
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "version": "1.2.3"
    }
  ]
}
EOF
SBOM_ONE="$TEMP_DIR/one.cdx.json"
SBOM_TWO="$TEMP_DIR/two.cdx.json"
SBOM_ENABLED="$TEMP_DIR/enabled.cdx.json"
SBOM_BUILD_CONFIG="$TEMP_DIR/sbom-gcconfig.pri"
SBOM_ENABLED_CONFIG="$TEMP_DIR/sbom-enabled-gcconfig.pri"
SBOM_PACKAGE_INDEX="$TEMP_DIR/sbom-package-index.json"
SBOM_TRANSFORMATIONS="$TEMP_DIR/sbom-transformations.json"
cat >"$SBOM_BUILD_CONFIG" <<'EOF'
GSL_INCLUDES = /usr/include
GSL_LIBS = -lgsl -lgslcblas -lm
EOF
cat >"$SBOM_ENABLED_CONFIG" <<'EOF'
GSL_INCLUDES = /usr/include
GSL_LIBS = -lgsl -lgslcblas -lm
SRMIO_INSTALL = /usr/local
D2XX_INCLUDE = ../D2XX/release
EOF
SBOM_FIXTURE_SHA256=$(sha256sum "$SBOM_APPDIR/usr/lib/libfixture.so" | cut -d' ' -f1)
SBOM_LIBSECRET_SHA256=$(sha256sum "$SBOM_APPDIR/usr/lib/libsecret-1.so.0.0.0" | cut -d' ' -f1)
SBOM_QT_SHA256=$(sha256sum "$SBOM_APPDIR/usr/lib/libQt6Core.so.6.8.3" | cut -d' ' -f1)
cat >"$SBOM_PACKAGE_INDEX" <<EOF
{
  "format": "goldencheetah-runtime-fixture-index-1",
  "libraries": [
    {
      "license": "LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only",
      "name": "Qt6Core",
      "path": "usr/lib/libQt6Core.so.6.8.3",
      "provenance": "fixture-qt-spdx",
      "purl": "pkg:generic/qt6core@6.8.3",
      "sha256": "$SBOM_QT_SHA256",
      "version": "6.8.3"
    },
    {
      "license": "MIT",
      "name": "fixture-runtime",
      "path": "usr/lib/libfixture.so",
      "provenance": "dpkg:fixture-runtime=9.1.0",
      "purl": "pkg:deb/ubuntu/fixture-runtime@9.1.0",
      "sha256": "$SBOM_FIXTURE_SHA256",
      "version": "9.1.0"
    },
    {
      "license": "LGPL-2.1-or-later",
      "name": "libsecret-1-0",
      "path": "usr/lib/libsecret-1.so.0.0.0",
      "provenance": "dpkg:libsecret-1-0=0.21.4-1",
      "purl": "pkg:deb/ubuntu/libsecret-1-0@0.21.4-1",
      "sha256": "$SBOM_LIBSECRET_SHA256",
      "version": "0.21.4-1"
    }
  ]
}
EOF
cat >"$SBOM_TRANSFORMATIONS" <<'EOF'
{
  "format": "goldencheetah-transformed-runtime-1",
  "libraries": []
}
EOF
SBOM_TOOL_DIR="$TEMP_DIR/sbom-tools"
SBOM_QT_ROOT="$TEMP_DIR/sbom-qt"
mkdir "$SBOM_TOOL_DIR"
mkdir -p "$SBOM_QT_ROOT/sbom"
cat >"$SBOM_QT_ROOT/sbom/fixture-6.8.3.spdx.json" <<'EOF'
{
  "spdxVersion": "SPDX-2.3",
  "packages": [],
  "files": [],
  "relationships": []
}
EOF
cat >"$SBOM_TOOL_DIR/pkg-config" <<'EOF'
#!/bin/sh
test "$1" = --modversion && test "$2" = libsecret-1 || exit 64
printf '%s\n' 0.21.4
EOF
cat >"$SBOM_TOOL_DIR/qmake" <<'EOF'
#!/bin/sh
test "$1" = -query && test "$2" = QT_INSTALL_PREFIX || exit 64
printf '%s\n' "${GC_TEST_SBOM_QT_ROOT:?}"
EOF
chmod +x "$SBOM_TOOL_DIR/pkg-config" "$SBOM_TOOL_DIR/qmake"
SBOM_TEST_RUNTIME_GENERATOR="$TEMP_DIR/test-runtime-provenance.py"
cat >"$SBOM_TEST_RUNTIME_GENERATOR" <<EOF
#!/usr/bin/env python3
import hashlib
import importlib.util
import os
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "runtime_provenance", r"$RUNTIME_PROVENANCE_GENERATOR"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
arguments = module.parse_arguments()
fixture_path = Path(os.environ["GC_INTERNAL_TEST_FIXTURE_INDEX"]).resolve()
expected = os.environ["GC_INTERNAL_TEST_FIXTURE_SHA256"]
if hashlib.sha256(fixture_path.read_bytes()).hexdigest() != expected:
    raise ValueError("internal fixture authorization mismatch")
fixture = module._load_test_fixture_package_index(
    fixture_path, arguments.appdir.resolve(), expected
)
module.atomic_write(
    arguments.output,
    module.build_document(arguments, fixture_index=fixture),
)
EOF
SBOM_ORIGINAL_PATH=$PATH
export GC_INTERNAL_TEST_FIXTURE_INDEX="$SBOM_PACKAGE_INDEX"
GC_INTERNAL_TEST_FIXTURE_SHA256=$(sha256sum \
    "$SBOM_PACKAGE_INDEX" | cut -d' ' -f1)
export GC_INTERNAL_TEST_FIXTURE_SHA256
export GC_TEST_SBOM_QT_ROOT="$SBOM_QT_ROOT"
PATH="$SBOM_TOOL_DIR:$PATH"
export PATH
if GC_RUNTIME_PROVENANCE_FIXTURE_INDEX="$SBOM_PACKAGE_INDEX" \
GC_RUNTIME_PROVENANCE_TEST_MODE=false \
GC_TEST_SBOM_QT_ROOT="$SBOM_QT_ROOT" \
PATH="$SBOM_TOOL_DIR:$PATH" create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$SBOM_ONE" "$SBOM_GENERATOR" \
    "$RUNTIME_PROVENANCE_GENERATOR" "$SBOM_TRANSFORMATIONS" \
    >/dev/null 2>&1; then
    fail "production SBOM generation accepted a fixture package index"
fi
GC_TEST_SBOM_QT_ROOT="$SBOM_QT_ROOT" \
PATH="$SBOM_TOOL_DIR:$PATH" create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$SBOM_ONE" "$SBOM_GENERATOR" \
    "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS"
GC_TEST_SBOM_QT_ROOT="$SBOM_QT_ROOT" \
PATH="$SBOM_TOOL_DIR:$PATH" create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$SBOM_TWO" "$SBOM_GENERATOR" \
    "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS"
GC_TEST_SBOM_QT_ROOT="$SBOM_QT_ROOT" \
PATH="$SBOM_TOOL_DIR:$PATH" create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_ENABLED_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$SBOM_ENABLED" "$SBOM_GENERATOR" \
    "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS"
cmp "$SBOM_ONE" "$SBOM_TWO" ||
    fail "identical AppDirs produced different SBOM data"
validate_appimage_sbom "$SBOM_ONE" ||
    fail "generated CycloneDX SBOM did not pass strict validation"
if [ -n "${GC_TEST_SBOM_OUTPUT:-}" ] &&
   [ "${GC_TEST_PYTHON_LOCK_INSTALL:-false}" != true ]; then
    install -m 0644 "$SBOM_ONE" "$GC_TEST_SBOM_OUTPUT"
fi
python3 - "$SBOM_ONE" "$REVISION_B" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
assert document["bomFormat"] == "CycloneDX"
assert document["specVersion"] == "1.5"
assert document["metadata"]["component"]["version"] == sys.argv[2]
assert document["metadata"]["component"]["licenses"] == [
    {"license": {"id": "GPL-2.0-or-later"}}
]
components = document["components"]
names = {component["name"] for component in components}
assert "fixture-package" in names
assert "usr/lib/libfixture.so" in names
assert "lib" in names
assert "linuxdeployqt" in names
assert "appimagetool" in names
assert "srmio" not in names
assert "d2xx-linux" not in names
assert "Qt6Core" in names
assert "libsecret-1-0" in names
assert "box2d" in names
assert "usr/share/doc/GoldenCheetah/licenses/Box2D-LICENSE" in names
qt = next(component for component in components
          if component["name"] == "Qt6Core")
assert qt["version"] == "6.8.3"
assert qt["purl"] == "pkg:generic/qt6core@6.8.3"
libsecret = next(component for component in components
                 if component["name"] == "libsecret-1-0")
assert libsecret["version"] == "0.21.4-1"
assert libsecret["purl"] == "pkg:deb/ubuntu/libsecret-1-0@0.21.4-1"
runtime = {
    next(prop["value"] for prop in component["properties"]
         if prop["name"] == "goldencheetah:runtime-path"): component
    for component in components
    if any(prop == {
        "name": "goldencheetah:role",
        "value": "identified-runtime-dependency",
    } for prop in component.get("properties", []))
}
assert set(runtime) == {
    "usr/lib/libQt6Core.so.6.8.3",
    "usr/lib/libfixture.so",
    "usr/lib/libsecret-1.so.0.0.0",
}
for component in runtime.values():
    assert component["version"]
    assert component["licenses"]
    assert any(prop["name"] == "goldencheetah:provenance"
               and prop["value"] for prop in component["properties"])
fixture = next(component for component in components
               if component["name"] == "fixture-package")
assert fixture["hashes"] == [{
    "alg": "SHA-256",
    "content": "a" * 64,
}]
box2d = next(component for component in components
             if component["name"] == "box2d")
assert box2d["version"] == "3.1.1"
assert box2d["licenses"] == [{"license": {"id": "MIT"}}]
assert box2d["purl"].startswith("pkg:github/erincatto/box2d@")
PY
python3 - "$SBOM_ENABLED" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
components = {component["name"]: component for component in document["components"]}
assert components["srmio"]["licenses"] == [{"license": {"id": "MIT"}}]
assert components["d2xx-linux"]["licenses"] == [
    {"license": {"name": "FTDI D2XX Driver License"}}
]
PY
python3 - "$SBOM_ONE" "$TEMP_DIR/broken-reference.cdx.json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    document = json.load(stream)
document["dependencies"][0]["dependsOn"].append("missing:component")
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump(document, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
if validate_appimage_sbom "$TEMP_DIR/broken-reference.cdx.json"; then
    fail "SBOM validation accepted an unresolved dependency reference"
fi
if grep -Fq -- "$TEMP_DIR" "$SBOM_ONE"; then
    fail "AppImage SBOM contains a build-machine path"
fi

cp "$SBOM_PIP_REPORT" "$TEMP_DIR/unreviewed-pip-report.json"
sed -i 's/"a\{64\}"/"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"/' \
    "$TEMP_DIR/unreviewed-pip-report.json"
if create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$TEMP_DIR/unreviewed-pip-report.json" \
    "$TEMP_DIR/unreviewed.cdx.json" "$SBOM_GENERATOR" \
        "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS" \
    >/dev/null 2>&1; then
    fail "SBOM accepted a Python artifact outside the reviewed lock"
fi

printf 'outside payload\n' >"$TEMP_DIR/outside-payload"
ln -s "$TEMP_DIR/outside-payload" "$SBOM_APPDIR/absolute-link"
if create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$TEMP_DIR/absolute.cdx.json" "$SBOM_GENERATOR" \
        "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS" \
    >/dev/null 2>&1; then
    fail "SBOM accepted an absolute AppDir symlink"
fi
unlink "$SBOM_APPDIR/absolute-link"
ln -s ../outside-payload "$SBOM_APPDIR/escaping-link"
if create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$TEMP_DIR/escaping.cdx.json" "$SBOM_GENERATOR" \
        "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS" \
    >/dev/null 2>&1; then
    fail "SBOM accepted an AppDir symlink escaping the payload"
fi
unlink "$SBOM_APPDIR/escaping-link"
ln -s missing-target "$SBOM_APPDIR/dangling-link"
if create_appimage_sbom \
    "$SBOM_APPDIR" "$BASE_MANIFEST" "$SBOM_BUILD_CONFIG" "$SBOM_LOCK" \
    "$SBOM_PIP_REPORT" "$TEMP_DIR/dangling.cdx.json" "$SBOM_GENERATOR" \
        "$SBOM_TEST_RUNTIME_GENERATOR" "$SBOM_TRANSFORMATIONS" \
    >/dev/null 2>&1; then
    fail "SBOM accepted a dangling AppDir symlink"
fi
unlink "$SBOM_APPDIR/dangling-link"
PATH=$SBOM_ORIGINAL_PATH
export PATH
unset GC_INTERNAL_TEST_FIXTURE_INDEX GC_INTERNAL_TEST_FIXTURE_SHA256 \
    GC_TEST_SBOM_QT_ROOT

for packager in "$LOCAL_PACKAGER" "$DEV_PACKAGER"; do
    bash -n "$packager"
    assert_contains "$packager" 'reproduce-appimage.sh'
    assert_contains "$packager" 'GC_APPIMAGE_OAUTH_POLICY=configured'
    assert_contains "$packager" 'verify_appimage_manifest'
    assert_contains "$packager" 'verify_appimage_sbom'
done

for packager in "$CI_PACKAGE_PASS"; do
    assert_contains "$packager" \
        'require_configured_strava_oauth_appimage'
    assert_contains "$packager" \
        'require_unconfigured_strava_oauth_appimage'
    assert_contains "$packager" \
        'install_linux_keychain_runtime'
    assert_contains "$packager" \
        'require_linux_keychain_appimage'
    assert_contains "$packager" \
        'run_linuxdeployqt_with_keychain_probe'
    assert_contains "$packager" \
        'install_qt_offscreen_plugin'
    assert_contains "$packager" \
        'require_qt_offscreen_appimage'
    assert_contains "$packager" \
        'create_appimage_build_manifest'
    assert_contains "$packager" \
        'install_appimage_build_manifest'
    assert_contains "$packager" \
        'finalize_appimage_manifest'
    assert_contains "$packager" \
        'verify_appimage_manifest'
    assert_contains "$packager" \
        'create_appimage_sbom'
    assert_contains "$packager" \
        'verify_appimage_sbom'
done
assert_contains "$CI_PACKAGE_PASS" \
    'require_unconfigured_strava_oauth_build "$BINARY"'
assert_contains "$CI_PACKAGE_PASS" \
    'require_unconfigured_strava_oauth_appimage'
assert_contains "$DEV_PACKAGER" 'promote_appimage_release'

if grep -Fq 'python3.7' "$LOCAL_PACKAGER"; then
    fail "local AppImage packaging still embeds unsupported Python 3.7"
fi
assert_contains "$LOCAL_PACKAGER" 'write_source_revision'
assert_contains "$LOCAL_PACKAGER" \
    'run_packaging_appimage "$output" --version'
assert_contains "$DEV_CONFIG" \
    '# DEFINES += GC_STRAVA_CLIENT_ID=\\\"your_client_id\\\"'
assert_contains "$DEV_CONFIG" \
    'src/Core/GeneratedSecrets.h'
if grep -Eq 'DEFINES.*(SECRET|API_KEY|BASIC_AUTH)' \
    "$DEV_CONFIG" "$REPO_ROOT/src/gcconfig.pri.in"; then
    fail "tracked qmake configuration still recommends compiler-line secrets"
fi
assert_contains "$MAIN_SOURCE" \
    '--goldencheetah-linux-keychain-status'
assert_contains "$MAIN_SOURCE" \
    '--goldencheetah-build-provenance'
assert_contains "$MAIN_SOURCE" \
    '--goldencheetah-gui-smoke'
assert_contains "$MAIN_SOURCE" \
    'goldencheetah_gui_smoke=main-window-ready'
assert_contains "$MAIN_SOURCE" 'scheduleGuiSmokeCompletion'
assert_contains "$MAIN_SOURCE" \
    'configureBundledLinuxRuntime'
assert_contains "$LIBSECRET_SOURCE" \
    'GC_QTKEYCHAIN_LIBSECRET_PATH'
assert_contains "$APPVEYOR_INSTALL" 'libsecret-1-dev'
assert_contains "$APPVEYOR_INSTALL" 'libgpg-error-dev'
assert_contains "$APPVEYOR_INSTALL" 'pkg-config'
bash -n "$APPVEYOR_INSTALL"
bash -n "$APPVEYOR_MACOS_INSTALL"
assert_contains "$APPVEYOR_INSTALL" \
    'SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e'
assert_contains "$APPVEYOR_MACOS_INSTALL" \
    'SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e'
assert_contains "$APPVEYOR_INSTALL" \
    'SRMIO_SHA256=16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186'
assert_contains "$APPVEYOR_MACOS_INSTALL" \
    'SRMIO_SHA256=16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186'
assert_contains "$APPVEYOR_INSTALL" 'SRMIO_WORK=$(mktemp -d)'
assert_contains "$APPVEYOR_MACOS_INSTALL" 'SRMIO_WORK=$(mktemp -d)'
assert_contains "$APPVEYOR_INSTALL" '--strip-components 1'
assert_contains "$APPVEYOR_MACOS_INSTALL" '--strip-components 1'
assert_contains "$APPVEYOR_INSTALL" \
    'D2XX_ARCHIVE=libftd2xx-x86_64-1.4.33.tar.gz'
assert_contains "$APPVEYOR_INSTALL" \
    'D2XX_SHA256=e260a4594a313583b87bf230c79cec9d46f11db6dcfd7c7d4f963279703214d3'
assert_contains "$APPVEYOR_INSTALL" \
    'D2XX_URL=https://distfiles.gentoo.org/distfiles/b1/$D2XX_ARCHIVE'
assert_contains "$APPVEYOR_INSTALL" \
    '--skip-link linux-x86_64/libftd2xx.so.1.4.33'
assert_contains "$APPVEYOR_INSTALL" 'libftd2xx.so'
assert_contains "$APPVEYOR_INSTALL" 'README.pdf'
assert_contains "$APPVEYOR_MACOS_INSTALL" \
    'D2XX_SHA256=f59d18c11ecf5dedf0fcbdef24f18823c122ff24189a8e204479f9c408af7704'
assert_contains "$APPVEYOR_MACOS_INSTALL" 'brew reinstall --formula "$formula"'
assert_contains "$APPVEYOR_MACOS_PACKAGER" \
    'codesign --verify --deep --strict GoldenCheetah.app'
assert_contains "$APPVEYOR_MACOS_PACKAGER" 'validate-payload.py'
assert_contains "$APPVEYOR_MACOS_PACKAGER" '--forbidden-prefix "$HOME"'
assert_contains "$APPVEYOR_CONFIG" '  - Ubuntu2204'
assert_contains "$CI_PACKAGE_PASS" \
    'require_qt_offscreen_appimage_on_glibc 2.35'
assert_contains "$CI_PACKAGE_PASS" \
    'require_qt_offscreen_appimage_on_glibc 2.35 "$IMAGE" 30s'
test -x "$CI_PACKAGE_PASS" ||
    fail "production AppImage pass script is missing or not executable"
bash -n "$CI_PACKAGE_PASS"
for production_step in \
    'create_appimage_build_manifest' \
    'run_linuxdeployqt_with_keychain_probe' \
    'install_embedded_python' \
    'install_linux_keychain_runtime' \
    'create_appimage_sbom' \
    'normalize_appdir_mtimes' \
    'run_packaging_appimage' \
    'finalize_appimage_manifest' \
    'verify_appimage_manifest' \
    'verify_appimage_sbom'; do
    assert_contains "$CI_PACKAGE_PASS" "$production_step"
done
test -x "$CI_BUILD_PASS" ||
    fail "independent AppImage build pass is missing or not executable"
test -x "$CI_REPRODUCE" ||
    fail "AppImage reproduction driver is missing or not executable"
bash -n "$CI_BUILD_PASS" "$CI_REPRODUCE"
assert_contains "$CI_PACKAGER" 'reproduce-appimage.sh'
assert_contains "$CI_REPRODUCE" 'for label in one two'
assert_contains "$CI_REPRODUCE" 'ELF_ONE_SHA256=$(sha256sum'
assert_contains "$CI_REPRODUCE" '! cmp -s -- "$ELF_ONE" "$ELF_TWO"'
assert_contains "$CI_REPRODUCE" 'compare_appimage_reproduction'
assert_contains "$CI_BUILD_PASS" '"$MAKE_COMMAND" -j"$BUILD_JOBS" sub-src'
assert_contains "$CI_PACKAGE_PASS" 'if [ ! -x "$IMAGE" ]'
if grep -Fq '[ ! -x ./GoldenCheetah*.AppImage ]' "$CI_PACKAGE_PASS"; then
    fail "Linux release packaging still validates AppImage output with a glob"
fi
assert_contains "$APPVEYOR_CONFIG" 'GClinuxAppImageSbom'
assert_contains "$SECRETS_SCRIPT" \
    '[Parameter(Position=0)] [string]$f = "src/Core/GeneratedSecrets.h"'
if grep -Fq 'DEFINES +=' "$SECRETS_SCRIPT"; then
    fail "CI secrets are still placed in compiler definitions"
fi
assert_contains "$SECRETS_SCRIPT" 'ConvertTo-CEncodedUtf8'
assert_contains "$SECRETS_HEADER" '__has_include("GeneratedSecrets.h")'
git -C "$REPO_ROOT" check-ignore -q src/Core/GeneratedSecrets.h ||
    fail "generated CI secret header is not ignored by Git"
git -C "$REPO_ROOT" check-ignore -q \
    src/Core/GeneratedSecrets.h.tmp-fixture ||
    fail "generated CI secret temporary files are not ignored by Git"
[ -r "$SECRETS_TEST" ] || fail "missing generated-secret behavior test"
[ -r "$WINDOWS_PACKAGING_TEST" ] ||
    fail "missing Windows packaging behavior test"
[ -x "$CI_RELEASE_GATES_TEST" ] || fail "missing executable CI release-gate test"
"$CI_RELEASE_GATES_TEST"
python3 "$QT_ARCHIVE_TEST"
python3 "$SAFE_EXTRACTION_TEST"
python3 "$MACOS_PACKAGING_TEST"
python3 "$SBOM_PROVENANCE_TEST"
python3 "$UNCONFIGURED_OAUTH_TEST"
python3 "$WINDOWS_OPENSSL_TEST"
bash "$DIAGNOSTIC_OAUTH_TEST"
if command -v pwsh >/dev/null 2>&1; then
    pwsh -NoLogo -NoProfile -File "$SECRETS_TEST" \
        -RepositoryRoot "$REPO_ROOT"
    pwsh -NoLogo -NoProfile -File "$WINDOWS_PACKAGING_TEST" \
        -RepositoryRoot "$REPO_ROOT"
fi
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
    fail "AppVeyor public artifacts still receive reusable provider credentials"
fi
assert_contains "$GITHUB_CI_CONFIG" '  contents: read'
assert_contains "$GITHUB_CI_CONFIG" '  trusted-macos-release:'
assert_contains "$GITHUB_CI_CONFIG" '      - name: Add Secrets'
assert_contains "$GITHUB_CI_CONFIG" \
    'uses: actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a'
if grep -Eq '^[[:space:]]*-[[:space:]]+C:\\(LIBS|JOM|R|Python|tools\\vcpkg)' \
    "$APPVEYOR_CONFIG"; then
    fail "AppVeyor still restores unauthenticated Windows dependency trees"
fi
[ -r "$UBUNTU_SNAPSHOT" ] || fail "missing Ubuntu release snapshot sources"
assert_contains "$UBUNTU_SNAPSHOT" \
    '[snapshot=20260801T000000Z]'
[ -r "$DEV_UBUNTU_SNAPSHOT" ] ||
    fail "missing development Ubuntu snapshot sources"
assert_contains "$DEV_UBUNTU_SNAPSHOT" \
    '[snapshot=20260801T000000Z]'
assert_contains "$DEV_DOCKERFILE" \
    'FROM ubuntu:24.04@sha256:c4a8d5503dfb2a3eb8ab5f807da5bc69a85730fb49b5cfca2330194ebcc41c7b'
assert_contains "$DEV_DOCKERFILE" 'ARG UBUNTU_SNAPSHOT=20260801T000000Z'
if grep -Fq 'Acquire::https::Verify-Peer "false"' "$DEV_DOCKERFILE"; then
    fail "development container disables TLS peer verification"
fi
assert_contains "$DEV_DOCKERFILE" 'verify-apt-snapshot.py'
assert_contains "$DEV_DOCKERFILE" '--series noble'
assert_contains "$DEV_DOCKERFILE" 'apt-get update --error-on=any'
assert_contains "$DEV_DOCKERFILE" 'autoconf'
assert_contains "$DEV_DOCKERFILE" 'automake'
assert_contains "$DEV_DOCKERFILE" 'libtool'
assert_contains "$APPVEYOR_INSTALL" 'UBUNTU_SNAPSHOT=20260801T000000Z'
assert_contains "$APPVEYOR_INSTALL" \
    'sudo find /etc/apt/sources.list.d'
assert_contains "$APPVEYOR_INSTALL" \
    'sudo rm -rf /var/lib/apt/lists/*'
assert_contains "$APPVEYOR_INSTALL" \
    'sudo "$APT_GET" update --error-on=any -qq'
assert_contains "$APPVEYOR_INSTALL" 'QT_BUILD_VERSION=6.8.3'
assert_contains "$APPVEYOR_INSTALL" \
    'PYTHON_SOURCE_SHA256=f4de1b10bd6c70cbb9fa1cd71fc5038b832747a74ee59d599c69ce4846defb50'
assert_contains "$APPVEYOR_INSTALL" 'sudo rm -rf -- "$PYTHON_WORK"'
if grep -Eq 'add-apt-repository|apt-key' "$APPVEYOR_INSTALL"; then
    fail "Linux release setup still adds a moving package repository"
fi
assert_contains "$APPVEYOR_MACOS_INSTALL" \
    'R_PACKAGE_SHA256=c239e97c3659169447c50474827d9af79176fff67a34249fcd413d8da6ef2414'
assert_contains "$APPVEYOR_MACOS_INSTALL" \
    'test "$("$(brew --prefix "python@${PYTHON_VERSION}")/bin/python${PYTHON_VERSION}" --version)" = "Python 3.11.15"'
assert_contains "$APPVEYOR_MACOS_PACKAGER" '--require-hashes'
assert_contains "$APPVEYOR_MACOS_PACKAGER" \
    'requirements-appimage.lock'
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" \
    "\$pythonSha256 = '90b4e5b9898b72d744650524bff92377c367f44bd5fbd09e3148656c080ad907'"
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" \
    "\$getPipSha256 = 'fb24e693bab954209a063d90953621412ccad4a500905a726286e038f508ddf6'"
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" '--require-hashes'
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" '--no-compile'
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" 'Assert-NoFileContainsText'
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" \
    "Remove-Item -LiteralPath \$pythonRoot -Recurse -Force"
assert_contains "$APPVEYOR_MACOS_PACKAGER" 'PYTHON_PACKAGE_STAGE=$(mktemp -d)'
assert_contains "$APPVEYOR_MACOS_PACKAGER" '--no-compile'
assert_contains "$APPVEYOR_MACOS_PACKAGER" \
    'rm -rf "$SITE_PACKAGES"'
if grep -Fq 'Test-DependencyCache' "$APPVEYOR_WINDOWS_INSTALL" ||
   grep -Fq '.gc-dependency-complete' "$APPVEYOR_WINDOWS_INSTALL" \
       "$APPVEYOR_WINDOWS_PACKAGER"; then
    fail "Windows release acquisition still trusts a marker-based payload"
fi
assert_contains "$APPVEYOR_WINDOWS_INSTALL" \
    'Remove-Item -LiteralPath $Root -Recurse -Force'
assert_contains "$APPVEYOR_WINDOWS_VCPKG" '"zlib"'
assert_contains "$SOURCE_CONFIG_TEMPLATE" '#ZLIB_INCLUDE ='
assert_contains "$SOURCE_CONFIG_TEMPLATE" '#ZLIB_LIBS ='
assert_contains "$APPVEYOR_WINDOWS_BEFORE_BUILD" \
    'ZLIB_INCLUDE = c:\tools\vcpkg\installed\x64-windows\include'
assert_contains "$APPVEYOR_WINDOWS_BEFORE_BUILD" \
    'ZLIB_LIBS = -Lc:\tools\vcpkg\installed\x64-windows\lib -lzlib'
assert_contains "$APPVEYOR_WINDOWS_PACKAGER" \
    '[IO.Path]::DirectorySeparatorChar'
assert_contains "$SOURCE_PROJECT" '!isEmpty(ZLIB_INCLUDE)'
assert_contains "$SOURCE_PROJECT" '!isEmpty(ZLIB_LIBS)'
assert_contains "$SOURCE_PROJECT" '$$[QT_INSTALL_PREFIX]/src/3rdparty/zlib'
assert_contains "$ATHLETE_MIGRATION_STUB" \
    'interactiveErrors(false)'
assert_contains "$ATHLETE_MIGRATION_PROJECT" '$${GSL_INCLUDES}'
assert_contains "$ATHLETE_MIGRATION_PROJECT" '$${GSL_LIBS}'
assert_contains "$ATHLETE_MIGRATION_PROJECT" '$${ZLIB_INCLUDE}'
assert_contains "$ATHLETE_MIGRATION_PROJECT" '$${ZLIB_LIBS}'
assert_contains "$ATHLETE_MIGRATION_STUB" 'MeasuresGroup::getFieldValue('
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideFile *RideItem::ride('
assert_contains "$ATHLETE_MIGRATION_STUB" 'QString Leaf::toString()'
assert_contains "$ATHLETE_MIGRATION_STUB" 'PMCData::PMCData('
assert_contains "$ATHLETE_MIGRATION_STUB" 'Banister::Banister('
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideImportWizard::RideImportWizard('
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideImportWizard::closeEvent('
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideImportWizard::continueTrainingClicked()'
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideDelegate::commitAndCloseTimeEditor()'
assert_contains "$ATHLETE_MIGRATION_STUB" 'AthleteBackup::AthleteBackup('
assert_contains "$ATHLETE_MIGRATION_STUB" 'void Banister::refresh()'
assert_contains "$ATHLETE_MIGRATION_STUB" 'void PMCData::refresh()'
assert_contains "$ATHLETE_MIGRATION_STUB" 'MainWindow::saveSilent('
assert_contains "$ATHLETE_MIGRATION_STUB" 'RideCache::saveActivities('
assert_contains "$ATHLETE_MIGRATION_STUB" 'QColor GCColor::invertColor('
assert_contains "$STRAVA_OAUTH_POLICY_PROJECT" 'QT += charts'
assert_contains "$STRAVA_OAUTH_POLICY_PROJECT" '$${GSL_INCLUDES}'
strava_config_line=$(grep -nF 'include(../../unittests.pri)' \
    "$STRAVA_OAUTH_POLICY_PROJECT" | cut -d: -f1)
strava_gsl_line=$(grep -nF '$${GSL_INCLUDES}' \
    "$STRAVA_OAUTH_POLICY_PROJECT" | cut -d: -f1)
if [ "$strava_config_line" -ge "$strava_gsl_line" ]; then
    fail "Strava OAuth policy test expands GSL_INCLUDES before loading gcconfig.pri"
fi
assert_contains "$STRAVA_ROUTES_PIPELINE_PROJECT" '$${GSL_INCLUDES}'
assert_contains "$DATA_FILTER_ZONES_PROJECT" '$${GSL_INCLUDES}'
assert_contains "$INDEND_PLOT_MARKER_PROJECT" 'QT += testlib widgets svg'
assert_contains "$UNITTEST_CONFIG" 'INCLUDEPATH += $${GSL_INCLUDES}'
assert_contains "$CREDENTIAL_SETTINGS_TEST" 'double dpiXFactor = 1.0;'
assert_contains "$CREDENTIAL_SETTINGS_TEST" 'double dpiYFactor = 1.0;'
if grep -Fq '$${QT_INSTALL_PREFIX}/src/3rdparty/zlib' "$SOURCE_PROJECT"; then
    fail "Windows zlib fallback still expands an unset qmake variable"
fi
if grep -Eq 'pip(3|\.exe| -m pip)?.*install.*--upgrade|pip install --upgrade' \
    "$APPVEYOR_MACOS_INSTALL" "$APPVEYOR_MACOS_PACKAGER" \
    "$APPVEYOR_WINDOWS_PACKAGER"; then
    fail "release packaging still upgrades Python tooling from a moving target"
fi
if grep -Fq 'releases/download/continuous' "$SUPPORT"; then
    fail "AppImage packaging still uses a moving tool release"
fi
if grep -Eq 'git (clone|fetch).*(github.com/rclasen/srmio|SRMIO_REVISION)' \
    "$APPVEYOR_INSTALL" "$APPVEYOR_MACOS_INSTALL"; then
    fail "release setup still executes an unverified SRMIO checkout"
fi

if [ "${GC_TEST_PYTHON_LOCK_INSTALL:-false}" = true ]; then
    for iteration in 1 2; do
        PYTHON_LOCK_INSTALL="$TEMP_DIR/python-lock-install-$iteration"
        mkdir -p "$PYTHON_LOCK_INSTALL/appdir"
        (
            cd "$PYTHON_LOCK_INSTALL"
            PYTHON_LOCK_REPORT="$PYTHON_LOCK_INSTALL/pip-report.json"
            (umask 077 && : >"$PYTHON_LOCK_REPORT")
            install_embedded_python \
                "$APPIMAGE_REQUIREMENTS" "$PYTHON_LOCK_INSTALL/appdir" \
                "$PYTHON_LOCK_REPORT"
            if find "$PYTHON_LOCK_INSTALL/appdir" -type f -name '*.pyc' \
                -print -quit | grep -q .; then
                fail "embedded Python payload contains path-dependent bytecode"
            fi
            if grep -R -F -l -- "$PYTHON_LOCK_INSTALL" \
                "$PYTHON_LOCK_INSTALL/appdir" >/dev/null 2>&1; then
                fail "embedded Python payload retains its build path"
            fi
            [ -x "$PYTHON_LOCK_INSTALL/appdir/opt/python3.11/bin/f2py" ] ||
                fail "relocatable embedded Python console script is missing"
            PYTHONDONTWRITEBYTECODE=1 \
                "$PYTHON_LOCK_INSTALL/appdir/opt/python3.11/bin/f2py" -v \
                    >/dev/null
            PYTHONDONTWRITEBYTECODE=1 \
                "$PYTHON_LOCK_INSTALL/appdir/opt/python3.11/bin/python3.11" -c \
                'import importlib_metadata, jinja2, lmfit, numpy, pandas, plotly, scipy'
            if find "$PYTHON_LOCK_INSTALL/appdir" -type f -name '*.pyc' \
                -print -quit | grep -q .; then
                fail "embedded Python smoke generated bytecode in the payload"
            fi
            create_appimage_sbom \
                "$PYTHON_LOCK_INSTALL/appdir" "$BASE_MANIFEST" \
                "$SBOM_BUILD_CONFIG" "$APPIMAGE_REQUIREMENTS" \
                "$PYTHON_LOCK_REPORT" \
                "$PYTHON_LOCK_INSTALL/runtime.cdx.json" "$SBOM_GENERATOR" \
                "$RUNTIME_PROVENANCE_GENERATOR" "$SBOM_TRANSFORMATIONS"
            validate_appimage_sbom "$PYTHON_LOCK_INSTALL/runtime.cdx.json"
        )
    done
    if ! cmp "$TEMP_DIR/python-lock-install-1/runtime.cdx.json" \
        "$TEMP_DIR/python-lock-install-2/runtime.cdx.json"; then
        if [ -n "${GC_TEST_SBOM_OUTPUT:-}" ]; then
            install -m 0644 \
                "$TEMP_DIR/python-lock-install-1/runtime.cdx.json" \
                "${GC_TEST_SBOM_OUTPUT}.first"
            install -m 0644 \
                "$TEMP_DIR/python-lock-install-2/runtime.cdx.json" \
                "${GC_TEST_SBOM_OUTPUT}.second"
        fi
        diff -u \
            "$TEMP_DIR/python-lock-install-1/runtime.cdx.json" \
            "$TEMP_DIR/python-lock-install-2/runtime.cdx.json" |
            sed -n '1,120p' >&2 || true
        fail "repeated locked Python payload builds produced different SBOM data"
    fi
    if [ -n "${GC_TEST_SBOM_OUTPUT:-}" ]; then
        install -m 0644 \
            "$TEMP_DIR/python-lock-install-1/runtime.cdx.json" \
            "$GC_TEST_SBOM_OUTPUT"
    fi
fi

if [ "${GC_TEST_SRMIO_SOURCE_BUILD:-false}" = true ]; then
    SRMIO_CHECK="$TEMP_DIR/srmio-source-build"
    mkdir -p "$SRMIO_CHECK/source" "$SRMIO_CHECK/build"
    printf 'hostile stale cache\n' >"$SRMIO_CHECK/$SRMIO_SOURCE_FILE"
    download_verified_file \
        "$SRMIO_SOURCE_URL" "$SRMIO_CHECK/$SRMIO_SOURCE_FILE" \
        "$SRMIO_SOURCE_SHA256"
    tar xzf "$SRMIO_CHECK/$SRMIO_SOURCE_FILE" \
        --strip-components=1 -C "$SRMIO_CHECK/source"
    (
        cd "$SRMIO_CHECK/source"
        sh genautomake.sh
    )
    (
        cd "$SRMIO_CHECK/build"
        "$SRMIO_CHECK/source/configure" \
            --disable-shared --enable-static
        make --silent -j2
    )
    [ -f "$SRMIO_CHECK/build/.libs/libsrmio.a" ] ||
        fail "reviewed SRMIO source did not produce the static library"
fi

bash -n "$SUPPORT"
echo "PASS: AppImage Python runtime and packaging helpers are consistent"
