#!/usr/bin/env bash
# shellcheck enable=all
# shellcheck shell=bash

set -Eeuf -o pipefail

readonly test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly repository="$(cd "$test_dir/../../.." && pwd)"
readonly runner="$test_dir/../../../.github/scripts/run-tests.sh"
readonly temporary="$(mktemp -d "${TMPDIR:-/tmp}/gc-ci-test-runner.XXXXXX")"
trap 'rm -rf -- "$temporary"' EXIT

require_literal() {
  local text="$1"
  local file="$2"
  if ! grep -Fq -- "$text" "$file"; then
    printf 'missing CI test wiring %s in %s\n' "$text" "$file" >&2
    exit 1
  fi
}

require_literal 'cp unittests/unittests.pri.in unittests/unittests.pri' \
  "$repository/.github/scripts/build.sh"
require_literal 'make -j2 sub-unittests' \
  "$repository/.github/scripts/build.sh"
require_literal './.github/scripts/run-tests.sh' \
  "$repository/.github/workflows/ci.yml"
require_literal 'Build/ciTestRunner' \
  "$repository/unittests/unittests.pro"
require_literal 'linux:SUBDIRS += Build/appImagePackaging' \
  "$repository/unittests/unittests.pro"

make_fixture() {
  local name="$1"
  local body="$2"
  local root="$temporary/$name"
  mkdir -p "$root/unittests/Fake/Test" "$root/bin"
  printf 'check:\n' > "$root/unittests/Fake/Test/Makefile"
  printf '#!/usr/bin/env bash\n%s\n' "$body" > "$root/bin/make"
  chmod +x "$root/bin/make"
  printf '%s\n' "$root"
}

expect_failure() {
  local root="$1"
  if GC_TEST_MAKE_COMMAND="$root/bin/make" \
      "$runner" "$root" >/dev/null 2>&1; then
    printf 'expected runner failure for %s\n' "$root" >&2
    exit 1
  fi
}

missing="$temporary/missing"
mkdir -p "$missing/unittests" "$missing/bin"
printf '#!/usr/bin/env bash\nexit 0\n' > "$missing/bin/make"
chmod +x "$missing/bin/make"
expect_failure "$missing"

no_results="$(make_fixture no-results 'exit 0')"
expect_failure "$no_results"

zero_cases="$(make_fixture zero-cases 'printf "Totals: 0 passed, 0 failed, 0 skipped, 0 blacklisted, 0ms\\n"')"
expect_failure "$zero_cases"

make_failure="$(make_fixture make-failure 'exit 7')"
set +e
GC_TEST_MAKE_COMMAND="$make_failure/bin/make" \
  "$runner" "$make_failure" >/dev/null 2>&1
status=$?
set -e
if (( status != 7 )); then
  printf 'expected make status 7, got %d\n' "$status" >&2
  exit 1
fi

success="$(make_fixture success 'printf "Totals: 2 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms\\n"')"
GC_TEST_MAKE_COMMAND="$success/bin/make" "$runner" "$success" >/dev/null
