#!/usr/bin/env bash
set -Eeuo pipefail

err() {
  printf '%s\n' "$*" >&2
  exit 1
}

readonly SAFE_EXTRACT=appveyor/safe-extract.py
readonly R_PACKAGE=R-4.1.1-arm64.pkg
readonly R_PACKAGE_SHA256=cc078658708fdc7ae807f927cf5b3a96d17d287717679b3327540030f625d47b
readonly SRMIO_REVISION=b444b8747317c41607d468ae71a0ecd36a94332e
readonly SRMIO_ARCHIVE=srmio-${SRMIO_REVISION}.tar.gz
readonly SRMIO_SHA256=16359481488476df47de3cd1499787d3947036c06bd9d9b632f6e8a63e654186
readonly SRMIO_URL=https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${SRMIO_ARCHIVE}
readonly D2XX_VERSION=1.4.24
readonly D2XX_ARCHIVE=vendor/D2XX${D2XX_VERSION}.zip
readonly D2XX_SHA256=f59d18c11ecf5dedf0fcbdef24f18823c122ff24189a8e204479f9c408af7704

[[ -f ${SAFE_EXTRACT} ]] || err "safe archive extractor is missing"

download_verified() {
  local url=$1 destination=$2 digest=$3
  if ! printf '%s  %s\n' "${digest}" "${destination}" |
      shasum -a 256 -c - >/dev/null 2>&1; then
    rm -f -- "${destination}" "${destination}.tmp"
    curl --fail --location --retry 3 --silent --show-error \
      --output "${destination}.tmp" "${url}"
    printf '%s  %s\n' "${digest}" "${destination}.tmp" | shasum -a 256 -c -
    mv -- "${destination}.tmp" "${destination}"
  fi
}

download_verified \
  "https://cran.r-project.org/bin/macosx/big-sur-arm64/base/${R_PACKAGE}" \
  "${R_PACKAGE}" "${R_PACKAGE_SHA256}"
sudo installer -pkg "${R_PACKAGE}" -target /
[[ $(Rscript --vanilla -e 'cat(as.character(getRversion()))') == 4.1.1 ]]

mkdir -p srmio
download_verified "${SRMIO_URL}" "srmio/${SRMIO_ARCHIVE}" "${SRMIO_SHA256}"
SRMIO_WORK=$(mktemp -d)
cleanup_srmio() { rm -rf -- "${SRMIO_WORK}"; }
trap cleanup_srmio EXIT
mkdir "${SRMIO_WORK}/build"
/usr/bin/python3 "${SAFE_EXTRACT}" --format tar \
  --archive "srmio/${SRMIO_ARCHIVE}" \
  --destination "${SRMIO_WORK}/source" --strip-components 1
(
  cd "${SRMIO_WORK}/source"
  sh genautomake.sh
)
(
  cd "${SRMIO_WORK}/build"
  "${SRMIO_WORK}/source/configure" \
    --disable-shared --enable-static --prefix=/opt/homebrew
  make -j2 --silent
  make install
)
cleanup_srmio
trap - EXIT

printf '%s  %s\n' "${D2XX_SHA256}" "${D2XX_ARCHIVE}" | shasum -a 256 -c -
D2XX_WORK=$(mktemp -d)
cleanup_d2xx() {
  hdiutil detach "${D2XX_WORK}/mount" >/dev/null 2>&1 || true
  rm -rf -- "${D2XX_WORK}"
}
trap cleanup_d2xx EXIT
/usr/bin/python3 "${SAFE_EXTRACT}" --format zip \
  --archive "${D2XX_ARCHIVE}" --destination "${D2XX_WORK}/archive"
mkdir "${D2XX_WORK}/mount"
hdiutil attach -readonly -nobrowse -mountpoint "${D2XX_WORK}/mount" \
  "${D2XX_WORK}/archive/D2XX${D2XX_VERSION}.dmg"
rm -rf D2XX
mkdir D2XX
for relative in \
  release/build/libftd2xx.${D2XX_VERSION}.dylib \
  release/build/libftd2xx.a \
  release/ftd2xx.h \
  release/WinTypes.h; do
  source="${D2XX_WORK}/mount/${relative}"
  [[ -f ${source} && ! -L ${source} ]] || err "unsafe D2XX payload: ${relative}"
  cp -- "${source}" D2XX/
done
hdiutil detach "${D2XX_WORK}/mount"
rm -rf -- "${D2XX_WORK}"
trap - EXIT
