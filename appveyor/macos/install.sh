#!/bin/bash
set -euo pipefail

REPOSITORY_ROOT=$(pwd -P)
SAFE_EXTRACT="$REPOSITORY_ROOT/appveyor/safe-extract.py"
test -f "$SAFE_EXTRACT"

readonly HOMEBREW_BREW_COMMIT=67658c8cf6ee685420c531ed94ed46b6e7ba5b2a
readonly HOMEBREW_CORE_COMMIT=2602f7f80784581466deb491f09bf734174ac772
readonly HOMEBREW_BREW_REMOTE=https://github.com/Homebrew/brew.git
readonly HOMEBREW_CORE_REMOTE=https://github.com/Homebrew/homebrew-core.git

export HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALL_FROM_API=1

pin_git_repository()
{
    local repository=$1
    local remote=$2
    local commit=$3

    if [ ! -d "$repository/.git" ]; then
        if [ -e "$repository" ]; then
            echo "Refusing to replace non-Git dependency repository: $repository" >&2
            return 1
        fi
        mkdir -p "$repository"
        git -C "$repository" init
        git -C "$repository" remote add origin "$remote"
    else
        git -C "$repository" remote set-url origin "$remote"
    fi
    git -C "$repository" fetch --force --depth=1 origin "$commit"
    git -C "$repository" checkout --detach --force FETCH_HEAD
    test "$(git -C "$repository" rev-parse HEAD)" = "$commit"
}

assert_formula_version()
{
    local formula=$1
    local expected_version=$2
    local actual

    actual=$(brew list --versions --formula "$formula")
    if [ "$actual" != "$formula $expected_version" ]; then
        echo "Unexpected Homebrew package version: expected '$formula $expected_version', got '$actual'" >&2
        return 1
    fi
}

receipt_matches_core()
{
    local formula=$1
    local receipt

    receipt="$(brew --prefix "$formula")/INSTALL_RECEIPT.json"
    [ -f "$receipt" ] || return 1
    /usr/bin/python3 - "$receipt" "$HOMEBREW_CORE_COMMIT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as receipt_file:
    receipt = json.load(receipt_file)
if receipt.get("source", {}).get("tap_git_head") != sys.argv[2]:
    raise SystemExit(1)
PY
}

install_formula()
{
    local formula=$1
    local expected_version=$2

    # A matching cache receipt alone does not authenticate the installed keg.
    # Reinstall from the formula pinned above; Homebrew then verifies the
    # formula's source/bottle digest before replacing the payload.
    if brew list --versions --formula "$formula" >/dev/null 2>&1; then
        brew reinstall --formula "$formula"
    else
        brew install --formula "$formula"
    fi
    assert_formula_version "$formula" "$expected_version"
    receipt_matches_core "$formula"
}

date
sudo chmod -R +w /usr/local
BREW_REPOSITORY=$(brew --repository)
test -d "$BREW_REPOSITORY/.git"
pin_git_repository "$BREW_REPOSITORY" "$HOMEBREW_BREW_REMOTE" "$HOMEBREW_BREW_COMMIT"
CORE_REPOSITORY="$BREW_REPOSITORY/Library/Taps/homebrew/homebrew-core"
pin_git_repository "$CORE_REPOSITORY" "$HOMEBREW_CORE_REMOTE" "$HOMEBREW_CORE_COMMIT"

install_formula gsl 2.8
install_formula icu4c@78 78.3
install_formula libical 4.0.4
install_formula libusb 1.0.30
install_formula libsamplerate 0.2.2
install_formula openssl@3 3.6.3
install_formula "python@${PYTHON_VERSION}" 3.11.15_4

test "$(git -C "$BREW_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_BREW_COMMIT"
test "$(git -C "$CORE_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_CORE_COMMIT"
test "$("$(brew --prefix "python@${PYTHON_VERSION}")/bin/python${PYTHON_VERSION}" --version)" = "Python 3.11.15"

# GoldenCheetah's generated parser requires the pre-Bison-3 grammar behavior.
BISON_VERSION=2.7.1
BISON_ARCHIVE="bison-$BISON_VERSION.tar.xz"
BISON_SHA256=b409adcbf245baadb68d2f66accf6fdca5e282cafec1b865f4b5e963ba8ea7fb
BISON_PREFIX=/usr/local/opt/gc-bison-2.7.1-b409adcb
BISON_WORK=$(mktemp -d)
cleanup_bison()
{
    rm -rf "$BISON_WORK"
}
trap cleanup_bison EXIT
curl --fail --location --retry 3 --silent --show-error \
    --output "$BISON_WORK/$BISON_ARCHIVE" \
    "https://ftp.gnu.org/gnu/bison/$BISON_ARCHIVE"
printf '%s  %s\n' "$BISON_SHA256" "$BISON_WORK/$BISON_ARCHIVE" | shasum -a 256 -c -
/usr/bin/python3 "$SAFE_EXTRACT" --format tar \
    --archive "$BISON_WORK/$BISON_ARCHIVE" \
    --destination "$BISON_WORK/source" --strip-components 1
(
    cd "$BISON_WORK/source"
    ./configure --prefix="$BISON_PREFIX"
    make -j2 --silent
    sudo rm -rf "$BISON_PREFIX"
    sudo make install
)
if [ -e /usr/local/opt/bison@2.7 ] && [ ! -L /usr/local/opt/bison@2.7 ]; then
    sudo rm -rf /usr/local/opt/bison@2.7
else
    sudo rm -f /usr/local/opt/bison@2.7
fi
sudo ln -s "$BISON_PREFIX" /usr/local/opt/bison@2.7
test "$(/usr/local/opt/bison@2.7/bin/bison --version | sed -n '1p')" = "bison (GNU Bison) 2.7.1"
rm -rf "$BISON_WORK"
trap - EXIT

rm -rf '/usr/local/include/c++'

# R 4.1.1
R_PACKAGE=R-4.1.1.pkg
R_PACKAGE_SHA256=c239e97c3659169447c50474827d9af79176fff67a34249fcd413d8da6ef2414
curl --fail --location --retry 3 --silent --show-error \
    --output "$R_PACKAGE" \
    "https://cran.r-project.org/bin/macosx/base/$R_PACKAGE"
printf '%s  %s\n' "$R_PACKAGE_SHA256" "$R_PACKAGE" | shasum -a 256 -c -
sudo installer -pkg R-4.1.1.pkg -target /
test "$(Rscript --vanilla -e 'cat(as.character(getRversion()))')" = "4.1.1"

# SRMIO
SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e
SRMIO_ARCHIVE=srmio-$SRMIO_REVISION.tar.gz
SRMIO_SHA256=16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186
SRMIO_URL=https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$SRMIO_ARCHIVE
mkdir -p srmio
if ! printf '%s  %s\n' "$SRMIO_SHA256" "srmio/$SRMIO_ARCHIVE" |
     shasum -a 256 -c - >/dev/null 2>&1; then
    rm -f "srmio/$SRMIO_ARCHIVE" "srmio/$SRMIO_ARCHIVE.tmp"
    curl --fail --location --retry 3 --silent --show-error \
        --output "srmio/$SRMIO_ARCHIVE.tmp" "$SRMIO_URL"
    printf '%s  %s\n' "$SRMIO_SHA256" "srmio/$SRMIO_ARCHIVE.tmp" |
        shasum -a 256 -c -
    mv "srmio/$SRMIO_ARCHIVE.tmp" "srmio/$SRMIO_ARCHIVE"
fi
SRMIO_WORK=$(mktemp -d)
cleanup_srmio()
{
    rm -rf "$SRMIO_WORK"
}
trap cleanup_srmio EXIT
mkdir "$SRMIO_WORK/build"
/usr/bin/python3 "$SAFE_EXTRACT" --format tar \
    --archive "srmio/$SRMIO_ARCHIVE" \
    --destination "$SRMIO_WORK/source" --strip-components 1
(
    cd "$SRMIO_WORK/source"
    sh genautomake.sh
)
(
    cd "$SRMIO_WORK/build"
    "$SRMIO_WORK/source/configure" --disable-shared --enable-static
    make -j2 --silent
    sudo make install
)
rm -rf "$SRMIO_WORK"
trap - EXIT

# D2XX
D2XX_ARCHIVE=vendor/D2XX1.4.24.zip
D2XX_SHA256=f59d18c11ecf5dedf0fcbdef24f18823c122ff24189a8e204479f9c408af7704
printf '%s  %s\n' "$D2XX_SHA256" "$D2XX_ARCHIVE" | shasum -a 256 -c -
D2XX_TEMP=$(mktemp -d)
cleanup_d2xx()
{
    hdiutil detach "$D2XX_TEMP/mount" >/dev/null 2>&1 || true
    rm -rf "$D2XX_TEMP"
}
trap cleanup_d2xx EXIT
/usr/bin/python3 "$SAFE_EXTRACT" --format zip \
    --archive "$D2XX_ARCHIVE" --destination "$D2XX_TEMP/archive"
mkdir "$D2XX_TEMP/mount"
hdiutil attach -readonly -nobrowse -mountpoint "$D2XX_TEMP/mount" \
    "$D2XX_TEMP/archive/D2XX1.4.24.dmg"
rm -rf D2XX
mkdir D2XX
for source in \
    "$D2XX_TEMP/mount/release/build/libftd2xx.1.4.24.dylib" \
    "$D2XX_TEMP/mount/release/build/libftd2xx.a" \
    "$D2XX_TEMP/mount/release/ftd2xx.h" \
    "$D2XX_TEMP/mount/release/WinTypes.h"; do
    test -f "$source" && test ! -L "$source"
    cp "$source" D2XX
done
hdiutil detach "$D2XX_TEMP/mount"
rm -rf "$D2XX_TEMP"
trap - EXIT
sudo cp D2XX/libftd2xx.1.4.24.dylib /usr/local/lib

test "$(git -C "$BREW_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_BREW_COMMIT"
test "$(git -C "$CORE_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_CORE_COMMIT"
