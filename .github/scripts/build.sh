#!/usr/bin/env bash
# shellcheck enable=all
# shellcheck shell=bash

set -Eeuf -o pipefail

log() {
  printf '%s\n' "$*" >&2
}

err() {
  log "$*"
  exit 1
}

main() {
  readonly HOMEBREW_BREW_COMMIT=67658c8cf6ee685420c531ed94ed46b6e7ba5b2a
  readonly HOMEBREW_CORE_COMMIT=2602f7f80784581466deb491f09bf734174ac772
  export HOMEBREW_BREW_COMMIT HOMEBREW_CORE_COMMIT
  case "${1:-}" in
    "")
      :
      ;;
    clean)
      make clean || true
      git clean -fdX
      local artifacts=(
        GoldenCheetah_v3.8_arm64.dmg
        src/GoldenCheetah.app
      )
      rm -rf -- "${artifacts[@]}"
      ;;
    clean-all)
      make clean || true
      git clean -fdX
      local cached_downloads=(
        D2XX1.4.24.dmg
        D2XX1.4.24.zip
        D2XX
        R-4.1.1-arm64.pkg
        srmio
      )
      rm -rf -- "${cached_downloads[@]}"
      ;;
    *)
      err "unrecognized argument: $1"
      ;;
  esac

  export \
    HOMEBREW_NO_AUTO_UPDATE=1 \
    HOMEBREW_NO_INSTALL_CLEANUP=1 \
    PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/python@${PYTHON_VERSION}/libexec/bin:${PATH}

  local brewdeps=(
    automake=1.18.1_1
    bison=3.8.2
    gsl=2.8
    icu4c@78=78.3
    libical=4.0.4
    libsamplerate=0.2.2
    libtool=2.6.2
    libusb=1.0.30
    openssl@3=3.6.3
    python@"${PYTHON_VERSION}"=3.11.15_4
    qt=6.11.1
    dbus=1.16.2
  )

  .github/scripts/install-pinned-homebrew.sh "${brewdeps[@]}"

  .github/scripts/install.sh
  .github/scripts/before_build.sh

  cp unittests/unittests.pri.in unittests/unittests.pri

  qmake build.pro -r \
    QMAKE_CXXFLAGS_WARN_ON+="-Wno-unused-private-field -Wno-c++11-narrowing -Wno-deprecated-declarations -Wno-deprecated-register -Wno-nullability-completeness -Wno-sign-compare -Wno-inconsistent-missing-override" \
    QMAKE_CFLAGS_WARN_ON+="-Wno-deprecated-declarations -Wno-sign-compare"
  if [[ ! -f qwt/lib/libqwt.a ]]; then
    make -j2 sub-qwt
  fi
  make -j2 sub-src
  make -j2 sub-unittests

  .github/scripts/after_build.sh
}

main "$@"
