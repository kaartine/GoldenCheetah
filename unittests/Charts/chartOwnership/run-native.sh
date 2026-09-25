#!/bin/sh
# Run only in an isolated account/container: production static initialization
# precedes the test main. Never point XDG paths at existing application data.
set -eu
if [ "$#" -lt 2 ]; then
    echo "Usage: sh run-native.sh /absolute/tst_chartOwnership NEW_OUTPUT_DIR [QtTest functions...]" >&2
    exit 2
fi
chart_binary=$1
chart_output=$2
shift 2
case "$chart_binary" in /*) ;; *) echo "Binary path must be absolute" >&2; exit 2 ;; esac
test -f "$chart_binary" && test -x "$chart_binary"
umask 077
mkdir "$chart_output"
chart_output=$(cd "$chart_output" && pwd -P)
mkdir "$chart_output/config" "$chart_output/cache" "$chart_output/data" \
      "$chart_output/state" "$chart_output/runtime" "$chart_output/tmp"
export GC_CHART_OWNERSHIP_ROOT="$chart_output"
export XDG_CONFIG_HOME="$chart_output/config"
export XDG_CACHE_HOME="$chart_output/cache"
export XDG_DATA_HOME="$chart_output/data"
export XDG_STATE_HOME="$chart_output/state"
export XDG_RUNTIME_DIR="$chart_output/runtime"
export TMPDIR="$chart_output/tmp"
export QT_QPA_PLATFORM=offscreen
cd "$chart_output"
"$chart_binary" "$@" -o "$chart_output/qttest.xml",xml \
    -o "$chart_output/qttest.txt",txt > "$chart_output/application.log" 2>&1
