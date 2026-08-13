#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
TEST_ROOT="$REPOSITORY_ROOT/unittests/Build/appImagePackaging"

: "${PYTHON_VERSION:?PYTHON_VERSION must be set}"
BREW_PYTHON_ROOT=$(brew --prefix "python@${PYTHON_VERSION}")
export PATH="$BREW_PYTHON_ROOT/libexec/bin:$PATH"
observed_python=$(python3 -c \
    'import sys; print(".".join(map(str, sys.version_info[:2])))')
if [ "$observed_python" != "$PYTHON_VERSION" ]; then
    echo "Expected Python $PYTHON_VERSION, found $observed_python" >&2
    exit 1
fi

python3 "$TEST_ROOT/testMacOSPackaging.py"
bash "$TEST_ROOT/testCiReleaseGates.sh" --portable

echo "PASS: native macOS BUILD-001 regressions"
