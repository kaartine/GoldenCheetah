#!/bin/sh
# As run-pre-release-ui.sh: the session bus, gdbus and the AT-SPI services use the
# system libraries; only the application (here: Valgrind + control) gets the
# AppDir's libraries. Call inside dbus-run-session started without them.
set -eu
reply=$(gdbus call --session --dest org.a11y.Bus --object-path /org/a11y/bus \
    --method org.a11y.Bus.GetAddress)
AT_SPI_BUS_ADDRESS=$(printf '%s\n' "$reply" | sed -n "s/^('\(.*\)',)$/\1/p")
[ -n "$AT_SPI_BUS_ADDRESS" ] || { echo "no AT-SPI bus: $reply" >&2; exit 70; }
export AT_SPI_BUS_ADDRESS QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 QT_ACCESSIBILITY=1
exec env LD_LIBRARY_PATH="$APP_LD_LIBRARY_PATH" QT_PLUGIN_PATH="$APP_QT_PLUGIN_PATH" "$@"
