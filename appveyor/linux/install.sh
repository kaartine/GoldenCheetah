#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_INPUT_PATHS="$SCRIPT_DIR/build-input-paths.sh"
[ -f "$BUILD_INPUT_PATHS" ] && [ ! -L "$BUILD_INPUT_PATHS" ]
# shellcheck source=/dev/null
. "$BUILD_INPUT_PATHS"
prepare_appveyor_build_inputs
SAFE_EXTRACT="$REPOSITORY_ROOT/appveyor/safe-extract.py"
[ -f "$SAFE_EXTRACT" ] && [ ! -L "$SAFE_EXTRACT" ]
APT_SNAPSHOT_VERIFY="$REPOSITORY_ROOT/appveyor/linux/verify-apt-snapshot.py"
[ -f "$APT_SNAPSHOT_VERIFY" ] && [ ! -L "$APT_SNAPSHOT_VERIFY" ]
APT_GET="$REPOSITORY_ROOT/appveyor/linux/apt-get-fail-closed.sh"
[ -x "$APT_GET" ] && [ ! -L "$APT_GET" ]

UBUNTU_SNAPSHOT=20260801T000000Z
sudo find /etc/apt/sources.list.d -mindepth 1 -maxdepth 1 \
    -exec rm -rf -- {} +
sudo install -m 0644 appveyor/linux/ubuntu-snapshot.sources.list \
    /etc/apt/sources.list
printf 'APT::Snapshot "%s";\n' "$UBUNTU_SNAPSHOT" |
    sudo tee /etc/apt/apt.conf.d/50goldencheetah-snapshot >/dev/null
sudo tee /etc/apt/preferences.d/goldencheetah-snapshot >/dev/null <<'EOF'
Package: *
Pin: origin "snapshot.ubuntu.com"
Pin-Priority: 1001
EOF
sudo rm -rf /var/lib/apt/lists/*
sudo "$APT_GET" update --error-on=any -qq
sudo find /var/lib/apt/lists -maxdepth 1 \
    \( -name '*_InRelease' -o -name '*_Packages*' \) \
    ! -name "snapshot.ubuntu.com_ubuntu_${UBUNTU_SNAPSHOT}_*" -delete
APT_VERSION=$("$APT_GET" --version | sed -n '1s/^apt \([^ ]*\).*/\1/p')
python3 "$APT_SNAPSHOT_VERIFY" \
    --sources /etc/apt/sources.list \
    --lists /var/lib/apt/lists \
    --snapshot "$UBUNTU_SNAPSHOT" \
    --apt-version "$APT_VERSION" \
    --series jammy \
    --architecture "$(dpkg --print-architecture)"
QT_BUILD_VERSION=6.8.3
test "$(qmake -query QT_VERSION)" = "$QT_BUILD_VERSION"
sudo "$APT_GET" install -qq flex libpulse-dev
sudo "$APT_GET" install -qq pkg-config libsecret-1-dev libgpg-error-dev
sudo "$APT_GET" install -qq libcap2 libgnutls30
sudo "$APT_GET" install -qq libglu1-mesa-dev libxcb-cursor-dev
sudo "$APT_GET" install -qq libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
sudo "$APT_GET" install -qq libsamplerate0-dev
sudo "$APT_GET" install -qq libical-dev

# R from the reviewed Ubuntu snapshot
sudo "$APT_GET" install -qq r-base-dev
R --version

# D2XX
D2XX_ARCHIVE=libftd2xx-x86_64-1.4.27.tgz
D2XX_SHA256=537fc9db6e1eea110dd7661982dc49a28de22a4514b588e8a33a21110a5b6b4c
D2XX_URL=https://ftdichip.com/wp-content/uploads/2022/07/$D2XX_ARCHIVE
if ! printf '%s  %s\n' "$D2XX_SHA256" "$GC_D2XX_ROOT/$D2XX_ARCHIVE" |
     sha256sum --check --status -; then
    rm -f "$GC_D2XX_ROOT/$D2XX_ARCHIVE" \
        "$GC_D2XX_ROOT/$D2XX_ARCHIVE.tmp"
    wget --no-verbose \
        --output-document="$GC_D2XX_ROOT/$D2XX_ARCHIVE.tmp" \
        "$D2XX_URL"
    printf '%s  %s\n' "$D2XX_SHA256" \
        "$GC_D2XX_ROOT/$D2XX_ARCHIVE.tmp" |
        sha256sum --check --status -
    mv "$GC_D2XX_ROOT/$D2XX_ARCHIVE.tmp" \
        "$GC_D2XX_ROOT/$D2XX_ARCHIVE"
fi
if [ -e "$GC_D2XX_ROOT/release" ] || [ -L "$GC_D2XX_ROOT/release" ]; then
    echo "Refusing to replace an existing D2XX release directory." >&2
    exit 1
fi
D2XX_WORK=$(mktemp -d "$GC_D2XX_ROOT/.extract.XXXXXX")
cleanup_d2xx()
{
    rm -rf "$D2XX_WORK"
}
trap cleanup_d2xx EXIT
python3 "$SAFE_EXTRACT" --format tar \
    --archive "$GC_D2XX_ROOT/$D2XX_ARCHIVE" \
    --destination "$D2XX_WORK/release" --strip-components 1
for required in \
    ftd2xx.h \
    WinTypes.h \
    build/libftd2xx.so.1.4.27; do
    [ -f "$D2XX_WORK/release/$required" ] &&
        [ ! -L "$D2XX_WORK/release/$required" ]
done
mv "$D2XX_WORK/release" "$GC_D2XX_ROOT/release"
rm -rf "$D2XX_WORK"
trap - EXIT

# SRMIO
SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e
SRMIO_ARCHIVE=srmio-$SRMIO_REVISION.tar.gz
SRMIO_SHA256=16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186
SRMIO_URL=https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$SRMIO_ARCHIVE
if ! printf '%s  %s\n' "$SRMIO_SHA256" "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE" |
     sha256sum --check --status -; then
    rm -f "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE" \
        "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE.tmp"
    wget --no-verbose \
        --output-document="$GC_SRMIO_ROOT/$SRMIO_ARCHIVE.tmp" \
        "$SRMIO_URL"
    printf '%s  %s\n' "$SRMIO_SHA256" \
        "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE.tmp" |
        sha256sum --check --status -
    mv "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE.tmp" \
        "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE"
fi
sudo "$APT_GET" install -qq autoconf automake libtool build-essential
SRMIO_WORK=$(mktemp -d)
cleanup_srmio()
{
    rm -rf "$SRMIO_WORK"
}
trap cleanup_srmio EXIT
mkdir "$SRMIO_WORK/build"
python3 "$SAFE_EXTRACT" --format tar \
    --archive "$GC_SRMIO_ROOT/$SRMIO_ARCHIVE" \
    --destination "$SRMIO_WORK/source" --strip-components 1
(
    cd "$SRMIO_WORK/source"
    sh genautomake.sh
)
(
    cd "$SRMIO_WORK/build"
    "$SRMIO_WORK/source/configure" --disable-shared --enable-static
    make --silent -j2
    sudo make install
)
rm -rf "$SRMIO_WORK"
trap - EXIT

# LIBUSB
sudo "$APT_GET" install -qq libusb-1.0-0-dev libudev-dev

# GSL
sudo "$APT_GET" -qq install libgsl-dev

# Python 3.11 headers and shared library for the application build
PYTHON_BUILD_VERSION=3.11.15
PYTHON_SOURCE=Python-$PYTHON_BUILD_VERSION.tgz
PYTHON_SOURCE_SHA256=f4de1b10bd6c70cbb9fa1cd71fc5038b832747a74ee59d599c69ce4846defb50
PYTHON_SOURCE_URL=https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$PYTHON_SOURCE
if ! printf '%s  %s\n' "$PYTHON_SOURCE_SHA256" \
       "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE" |
       sha256sum --check --status -; then
    rm -f "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE" \
        "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE.tmp"
    wget --no-verbose \
        --output-document="$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE.tmp" \
        "$PYTHON_SOURCE_URL"
    printf '%s  %s\n' "$PYTHON_SOURCE_SHA256" \
        "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE.tmp" |
        sha256sum --check --status -
    mv "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE.tmp" \
        "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE"
fi
sudo "$APT_GET" install -qq libbz2-dev libffi-dev liblzma-dev libreadline-dev \
    libsqlite3-dev libssl-dev tk-dev uuid-dev zlib1g-dev
PYTHON_WORK=$(mktemp -d)
PYTHON_BUILD="$PYTHON_WORK/source"
cleanup_python_build()
{
    rm -rf "$PYTHON_WORK"
}
trap cleanup_python_build EXIT
python3 "$SAFE_EXTRACT" --format tar \
    --archive "$GC_PYTHON_SOURCE_ROOT/$PYTHON_SOURCE" \
    --destination "$PYTHON_BUILD" --strip-components 1
(
    cd "$PYTHON_BUILD"
    ./configure --prefix=/usr/local --enable-shared --with-ensurepip=install
    make --silent -j2
    sudo make altinstall
)
sudo ldconfig
rm -rf "$PYTHON_WORK"
trap - EXIT
test "$(python${PYTHON_VERSION} --version)" = \
    "Python $PYTHON_BUILD_VERSION"

# AppImage runtime, payload inspection, and QtWebEngine RPATH tooling.
sudo "$APT_GET" install -qq libfuse2 patchelf squashfs-tools

exit
