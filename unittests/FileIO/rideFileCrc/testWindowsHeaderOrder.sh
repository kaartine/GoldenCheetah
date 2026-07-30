#!/usr/bin/env bash
set -euo pipefail

: "${CXX:?set CXX to a MinGW C++ compiler}"
: "${QT_INCLUDE_DIR:?set QT_INCLUDE_DIR to the Qt include directory}"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)

compile()
{
    "$CXX" \
        -std=gnu++17 \
        -DQT_CORE_LIB \
        -DQT_NO_DEBUG \
        "$@" \
        -I"$SCRIPT_DIR/windowsHeaderOrder" \
        -I"$REPO_ROOT/src" \
        -I"$QT_INCLUDE_DIR" \
        -I"$QT_INCLUDE_DIR/QtCore" \
        -fsyntax-only \
        "$REPO_ROOT/src/FileIO/RideFileCRC.cpp"
}

compile
compile -DWINVER=0x0600 -D_WIN32_WINNT=0x0600
