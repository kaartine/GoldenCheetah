#!/bin/bash
set -ev

### This script should be run from GoldenCheetah src directory after build
if [ ! -x ./GoldenCheetah ]
then echo "Build GoldenCheetah and execute from distribution src"; exit 1
fi

. Resources/linux/AppImagePackagingSupport.sh

qmake --version

echo "Checking GoldenCheetah.app can execute"
./GoldenCheetah --version
STRAVA_OAUTH_STATUS=$(require_strava_oauth_build ./GoldenCheetah)
echo "$STRAVA_OAUTH_STATUS"

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

### Add vlc 3
#mkdir appdir/lib
#cp -r /usr/lib/x86_64-linux-gnu/vlc appdir/lib/vlc
#sudo appdir/lib/vlc/vlc-cache-gen appdir/lib/vlc/plugins

### Download current version of linuxdeployqt
download_file "$LINUXDEPLOYQT_URL" "$LINUXDEPLOYQT_FILE"
chmod a+x "$LINUXDEPLOYQT_FILE"

### Deploy to appdir
run_linuxdeployqt_with_keychain_probe \
    ./GoldenCheetah appdir \
    "./$LINUXDEPLOYQT_FILE" appdir/GoldenCheetah \
    -verbose=2 -bundle-non-qt-libs \
    -exclude-libs=libqsqlmysql,libqsqlpsql,libqsqlmimer,libqsqlodbc,libnss3,libnssutil3,libxcb-dri3.so.0 \
    -unsupported-allow-new-glibc

# linuxdeployqt only detects the desktop xcb backend.
install_qt_offscreen_plugin qmake appdir

# Add Python and core modules
install_embedded_python "Python/requirements.txt" "appdir"

# Add the dynamically loaded Linux credential-vault runtime and licenses
install_linux_keychain_runtime \
    "appdir" "../contrib/qtkeychain/COPYING"

# Fix RPATH on QtWebEngineProcess and copy missing resources
patchelf --set-rpath '$ORIGIN/../lib' appdir/libexec/QtWebEngineProcess
cp -r `qmake -v|awk '/Qt/ { print $6 "/../resources" }' -` appdir

# Generate AppImage
download_file "$APPIMAGETOOL_URL" "$APPIMAGETOOL_FILE"
chmod a+x "$APPIMAGETOOL_FILE"
run_packaging_appimage "./$APPIMAGETOOL_FILE" appdir

### Cleanup
rm -f "$LINUXDEPLOYQT_FILE"
rm -f "$APPIMAGETOOL_FILE"
rm -rf appdir

if [ ! -x ./GoldenCheetah-x86_64.AppImage ]
then echo "AppImage not generated, check the errors"; exit 1
fi

echo "Renaming AppImage file to branch and build number ready for deploy"
export FINAL_NAME=GoldenCheetah_v3.8_x64Qt6.AppImage
mv -f GoldenCheetah-x86_64.AppImage $FINAL_NAME
ls -l $FINAL_NAME
STRAVA_OAUTH_STATUS=$(require_strava_oauth_appimage "./$FINAL_NAME")
echo "$STRAVA_OAUTH_STATUS"
KEYCHAIN_RUNTIME_STATUS=$(require_linux_keychain_appimage "./$FINAL_NAME")
echo "$KEYCHAIN_RUNTIME_STATUS"
OFFSCREEN_RUNTIME_STATUS=$(
    require_qt_offscreen_appimage "./$FINAL_NAME")
echo "$OFFSCREEN_RUNTIME_STATUS"

### Generate version file with SHA
run_packaging_appimage "./$FINAL_NAME" --version \
    2>GCversionLinuxQt6.txt
write_source_revision GCversionLinuxQt6.txt
echo "$STRAVA_OAUTH_STATUS" >> GCversionLinuxQt6.txt
echo "SHA256 hash of $FINAL_NAME:" >> GCversionLinuxQt6.txt
shasum -a 256 $FINAL_NAME | cut -f 1 -d ' '  >> GCversionLinuxQt6.txt
cat GCversionLinuxQt6.txt
