#!/bin/bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: package-appimage-pass.sh OUTPUT_DIRECTORY" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=${GC_APPIMAGE_REPOSITORY_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd -P)}
REPOSITORY_ROOT=$(cd -- "$REPOSITORY_ROOT" && pwd -P)
PASS_DIR=$1
SOURCE_DIR="$REPOSITORY_ROOT/src"
BINARY=${GC_APPIMAGE_BINARY:-$SOURCE_DIR/GoldenCheetah}
QMAKE_COMMAND=${GC_APPIMAGE_QMAKE:-qmake}
OAUTH_POLICY=${GC_APPIMAGE_OAUTH_POLICY:-unconfigured}
APPDIR="$PASS_DIR/GoldenCheetah.AppDir"
BUILD_MANIFEST="$PASS_DIR/build.manifest"
PYTHON_INSTALL_REPORT="$PASS_DIR/python-install-report.json"
RUNTIME_TRANSFORM_DIR="$PASS_DIR/runtime-transform"
RUNTIME_TRANSFORM_MANIFEST="$RUNTIME_TRANSFORM_DIR/manifest.json"
IMAGE="$PASS_DIR/GoldenCheetah.AppImage"
MANIFEST="$PASS_DIR/GoldenCheetah.AppImage.manifest"
SBOM="$PASS_DIR/GoldenCheetah.AppImage.sbom.cdx.json"

if [ ! -d "$PASS_DIR" ] || [ -L "$PASS_DIR" ] ||
   [ -n "$(find "$PASS_DIR" -mindepth 1 -print -quit)" ]; then
    echo "AppImage pass directory must be an empty real directory." >&2
    exit 1
fi
if [ ! -x "$BINARY" ]; then
    echo "GoldenCheetah build output is missing: $BINARY" >&2
    exit 1
fi
if [ "${PYTHON_VERSION:-}" != "3.11" ]; then
    echo "Build Python ${PYTHON_VERSION:-unset} does not match packaged Python 3.11" >&2
    exit 1
fi

# shellcheck source=/dev/null
. "$SOURCE_DIR/Resources/linux/AppImagePackagingSupport.sh"

mkdir -p "$APPDIR" "$RUNTIME_TRANSFORM_DIR"
(umask 077 && : >"$PYTHON_INSTALL_REPORT")
cd "$PASS_DIR"

case "$OAUTH_POLICY" in
configured)
    STRAVA_OAUTH_STATUS=$(require_strava_oauth_build "$BINARY")
    ;;
unconfigured)
    STRAVA_OAUTH_STATUS=$(require_unconfigured_strava_oauth_build "$BINARY")
    ;;
*)
    echo "Unknown GC_APPIMAGE_OAUTH_POLICY: $OAUTH_POLICY" >&2
    exit 1
    ;;
esac
echo "$STRAVA_OAUTH_STATUS"
create_appimage_build_manifest \
    "$REPOSITORY_ROOT" "$BINARY" "$STRAVA_OAUTH_STATUS" "$BUILD_MANIFEST"
set_appimage_source_date_epoch "$BUILD_MANIFEST" "$REPOSITORY_ROOT"

install -m 0755 "$BINARY" "$APPDIR/GoldenCheetah"
install_appimage_build_manifest "$BUILD_MANIFEST" "$APPDIR"
install -m 0644 "$SOURCE_DIR/Resources/images/gc.png" "$APPDIR/gc.png"
cat >"$APPDIR/GoldenCheetah.desktop" <<'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=GoldenCheetah
Comment=Cycling Power Analysis Software.
Exec=GoldenCheetah
Icon=gc
Categories=Science;Sports;
EOF

download_linuxdeployqt
download_appimagetool
download_appimage_runtime
chmod a+x "$LINUXDEPLOYQT_FILE" "$APPIMAGETOOL_FILE"

run_linuxdeployqt_with_keychain_probe \
    "$BINARY" "$APPDIR" \
    "./$LINUXDEPLOYQT_FILE" "$APPDIR/GoldenCheetah" \
    -verbose=2 -bundle-non-qt-libs \
    -exclude-libs=libqsqlmysql,libqsqlpsql,libqsqlmimer,libqsqlodbc,libnss3,libnssutil3,libxcb-dri3.so.0 \
    -unsupported-allow-new-glibc

install_qt_offscreen_plugin "$QMAKE_COMMAND" "$APPDIR"
install_embedded_python \
    "$SOURCE_DIR/Python/requirements-appimage.lock" "$APPDIR" \
    "$PYTHON_INSTALL_REPORT"
install_linux_keychain_runtime \
    "$APPDIR" "$REPOSITORY_ROOT/contrib/qtkeychain/COPYING" \
    "$RUNTIME_TRANSFORM_MANIFEST"

patchelf --set-rpath '$ORIGIN/../lib' \
    "$APPDIR/libexec/QtWebEngineProcess"
QT_INSTALL_PREFIX=$("$QMAKE_COMMAND" -query QT_INSTALL_PREFIX)
if [ ! -d "$QT_INSTALL_PREFIX/resources" ]; then
    echo "Qt resources directory is missing: $QT_INSTALL_PREFIX/resources" >&2
    exit 1
fi
cp -a "$QT_INSTALL_PREFIX/resources" "$APPDIR/"

SBOM_IN_APPDIR="$APPDIR/usr/share/goldencheetah/goldencheetah.cdx.json"
create_appimage_sbom \
    "$APPDIR" "$BUILD_MANIFEST" "$SOURCE_DIR/gcconfig.pri" \
    "$SOURCE_DIR/Python/requirements-appimage.lock" \
    "$PYTHON_INSTALL_REPORT" "$SBOM_IN_APPDIR" \
    "$SOURCE_DIR/Resources/linux/generate-appimage-sbom.py" \
    "$SOURCE_DIR/Resources/linux/generate-runtime-provenance.py" \
    "$RUNTIME_TRANSFORM_MANIFEST"
normalize_appdir_mtimes "$APPDIR"

ARCH=x86_64 run_packaging_appimage "./$APPIMAGETOOL_FILE" \
    --runtime-file "./$APPIMAGE_RUNTIME_FILE" "$APPDIR" "$IMAGE"
if [ ! -x "$IMAGE" ]; then
    echo "AppImage was not generated: $IMAGE" >&2
    exit 1
fi
install -m 0644 "$SBOM_IN_APPDIR" "$SBOM"

finalize_appimage_manifest "$IMAGE" "$BUILD_MANIFEST" "$MANIFEST"
verify_appimage_manifest "$IMAGE" "$MANIFEST"
verify_appimage_sbom "$IMAGE" "$SBOM"

if [ "$OAUTH_POLICY" = configured ]; then
    STRAVA_OAUTH_STATUS=$(require_strava_oauth_appimage "$IMAGE")
else
    STRAVA_OAUTH_STATUS=$(require_unconfigured_strava_oauth_appimage "$IMAGE")
fi
echo "$STRAVA_OAUTH_STATUS"
KEYCHAIN_RUNTIME_STATUS=$(require_linux_keychain_appimage "$IMAGE")
echo "$KEYCHAIN_RUNTIME_STATUS"
OFFSCREEN_RUNTIME_STATUS=$(require_qt_offscreen_appimage_on_glibc 2.35 "$IMAGE")
echo "$OFFSCREEN_RUNTIME_STATUS"
verify_appimage_manifest "$IMAGE" "$MANIFEST"
verify_appimage_sbom "$IMAGE" "$SBOM"
