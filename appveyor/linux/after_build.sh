#!/bin/bash
set -ev

### This script should be run from GoldenCheetah src directory after build
cd src
if [ ! -x ./GoldenCheetah ]; then
    echo "Build GoldenCheetah and execute from distribution src"
    exit 1
fi

. Resources/linux/AppImagePackagingSupport.sh
STRAVA_OAUTH_STATUS=$(strava_oauth_build_status ./GoldenCheetah)
echo "$STRAVA_OAUTH_STATUS"

if [ "${PYTHON_VERSION:-}" != "$PYTHON_APPIMAGE_SERIES" ]; then
    echo "Build Python ${PYTHON_VERSION:-unset} does not match packaged Python $PYTHON_APPIMAGE_SERIES"
    exit 1
fi

### Create a clean AppDir and start populating
rm -rf appdir
mkdir -p appdir

# Executable
cp GoldenCheetah appdir

# Desktop file
cat >appdir/GoldenCheetah.desktop <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=GoldenCheetah
Comment=Cycling Power Analysis Software.
Exec=GoldenCheetah
Icon=gc
Categories=Science;Sports;
EOF

# Icon
cp Resources/images/gc.png appdir/

### Download current version of linuxdeployqt
download_file "$LINUXDEPLOYQT_URL" "$LINUXDEPLOYQT_FILE"
chmod a+x "$LINUXDEPLOYQT_FILE"

### Deploy to appdir
run_packaging_appimage "./$LINUXDEPLOYQT_FILE" appdir/GoldenCheetah -verbose=2 -bundle-non-qt-libs -exclude-libs=libqsqlmysql,libqsqlpsql,libqsqlmimer,libqsqlodbc,libnss3,libnssutil3,libxcb-dri3.so.0 -unsupported-allow-new-glibc

# Add Python and core modules
install_embedded_python "Python/requirements.txt" "appdir"

# Fix RPATH on QtWebEngineProcess and copy missing resources
patchelf --set-rpath '$ORIGIN/../lib' appdir/libexec/QtWebEngineProcess

# Get Qt resources directory from qmake
QT_INSTALL_PREFIX=$(qmake -query QT_INSTALL_PREFIX 2>/dev/null || echo "")
if [ -n "$QT_INSTALL_PREFIX" ] && [ -d "${QT_INSTALL_PREFIX}/resources" ]; then
    cp -r "${QT_INSTALL_PREFIX}/resources" appdir/
else
    echo "Warning: Could not find Qt resources directory"
fi

# Generate AppImage
download_file "$APPIMAGETOOL_URL" "$APPIMAGETOOL_FILE"
chmod a+x "$APPIMAGETOOL_FILE"
run_packaging_appimage "./$APPIMAGETOOL_FILE" appdir

### Cleanup
rm -f "$LINUXDEPLOYQT_FILE"
rm -f "$APPIMAGETOOL_FILE"
rm -rf appdir

if [ ! -x ./GoldenCheetah*.AppImage ]; then
    echo "AppImage not generated, check the errors"
    exit 1
fi

echo "Renaming AppImage file to version number ready for deploy"
mv GoldenCheetah*.AppImage ../GoldenCheetah_v3.8_x64.AppImage

exit
