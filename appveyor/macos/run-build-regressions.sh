#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)
TEST_ROOT="$REPOSITORY_ROOT/unittests/Build/appImagePackaging"

python3 "$TEST_ROOT/testMacOSPackaging.py"
bash "$TEST_ROOT/testCiReleaseGates.sh" --portable

echo "PASS: native macOS BUILD-001 regressions"
