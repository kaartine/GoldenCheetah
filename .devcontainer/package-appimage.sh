#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build-devcontainer}"
binary="${BINARY:-${build_dir}/src/GoldenCheetah}"
package_dir="${PACKAGE_DIR:-${build_dir}/package-appimage}"
app_dir="${package_dir}/GoldenCheetah.AppDir"
tools_dir="${build_dir}/appimage-tools"
output="${OUTPUT:-${build_dir}/GoldenCheetah-BLE-PoC-x86_64.AppImage}"
qt_dir="${QTDIR:-/opt/Qt/6.8.3/gcc_64}"

linuxdeployqt_url="${LINUXDEPLOYQT_URL:-https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage}"
appimagetool_url="${APPIMAGETOOL_URL:-https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage}"

# shellcheck source=/dev/null
. "${repo_root}/src/Resources/linux/AppImagePackagingSupport.sh"

download_tool() {
    local url="$1"
    local target="$2"

    if [[ ! -s "${target}" ]]; then
        echo "Downloading $(basename "${target}")"
        curl -fsSL --retry 3 --retry-delay 2 "${url}" -o "${target}.tmp"
        mv "${target}.tmp" "${target}"
    fi
    chmod +x "${target}"
}

extract_tool() {
    local image="$1"
    local target="$2"

    if [[ ! -x "${target}/squashfs-root/AppRun" ]]; then
        rm -rf "${target}"
        mkdir -p "${target}"
        (cd "${target}" && "${image}" --appimage-extract >/dev/null)
    fi
}

if [[ ! -x "${binary}" ]]; then
    echo "GoldenCheetah binary not found: ${binary}" >&2
    exit 1
fi
if [[ ! -x "${qt_dir}/bin/qmake" ]]; then
    echo "Qt installation not found: ${qt_dir}" >&2
    exit 1
fi

strava_oauth_status="$(require_strava_oauth_build "${binary}")"
echo "${strava_oauth_status}"

rm -rf "${app_dir}"
mkdir -p "${app_dir}" "${tools_dir}"
install -m 0755 "${binary}" "${app_dir}/GoldenCheetah"
install -m 0644 "${repo_root}/src/Resources/images/gc.png" "${app_dir}/gc.png"

cat >"${app_dir}/GoldenCheetah.desktop" <<'EOF'
[Desktop Entry]
Version=1.0
Type=Application
Name=GoldenCheetah BLE PoC
Comment=Cycling power analysis and Garmin BLE heart-rate development build
Exec=GoldenCheetah
Icon=gc
Categories=Science;Sports;
EOF

linuxdeployqt_image="${tools_dir}/linuxdeployqt-x86_64.AppImage"
appimagetool_image="${tools_dir}/appimagetool-x86_64.AppImage"
linuxdeployqt_dir="${tools_dir}/linuxdeployqt"
appimagetool_dir="${tools_dir}/appimagetool"

download_tool "${linuxdeployqt_url}" "${linuxdeployqt_image}"
download_tool "${appimagetool_url}" "${appimagetool_image}"
extract_tool "${linuxdeployqt_image}" "${linuxdeployqt_dir}"
extract_tool "${appimagetool_image}" "${appimagetool_dir}"

export QTDIR="${qt_dir}"
export PATH="${qt_dir}/bin:${PATH}"

run_linuxdeployqt_with_keychain_probe \
    "${binary}" "${app_dir}" \
    "${linuxdeployqt_dir}/squashfs-root/AppRun" \
    "${app_dir}/GoldenCheetah" \
    -verbose=2 \
    -bundle-non-qt-libs \
    -exclude-libs=libqsqlmysql,libqsqlpsql,libqsqlmimer,libqsqlodbc,libnss3,libnssutil3,libxcb-dri3.so.0 \
    -unsupported-allow-new-glibc

install_qt_offscreen_plugin "${qt_dir}/bin/qmake" "${app_dir}"

install_linux_keychain_runtime \
    "${app_dir}" "${repo_root}/contrib/qtkeychain/COPYING"

if [[ -x "${app_dir}/libexec/QtWebEngineProcess" ]]; then
    patchelf --set-rpath '$ORIGIN/../lib' "${app_dir}/libexec/QtWebEngineProcess"
fi
if [[ -d "${qt_dir}/resources" ]]; then
    mkdir -p "${app_dir}/resources"
    cp -a "${qt_dir}/resources/." "${app_dir}/resources/"
fi

rm -f "${output}"
ARCH=x86_64 "${appimagetool_dir}/squashfs-root/AppRun" "${app_dir}" "${output}"
strava_oauth_status="$(require_strava_oauth_appimage "${output}")"
echo "${strava_oauth_status}"
keychain_runtime_status="$(require_linux_keychain_appimage "${output}")"
echo "${keychain_runtime_status}"
offscreen_runtime_status="$(
    require_qt_offscreen_appimage "${output}")"
echo "${offscreen_runtime_status}"

echo "AppDir: ${app_dir}"
echo "AppImage: ${output}"
