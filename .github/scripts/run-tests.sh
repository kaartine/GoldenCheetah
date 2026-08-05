#!/usr/bin/env bash
# shellcheck enable=all
# shellcheck shell=bash

set -Eeuf -o pipefail

test_output=""

cleanup() {
  if [[ -n "$test_output" ]]; then
    rm -f -- "$test_output"
  fi
}

trap cleanup EXIT

log() {
  printf '%s\n' "$*" >&2
}

err() {
  log "$*"
  exit 1
}

main() {
  local root="${1:-}"
  if [[ -z "$root" ]]; then
    root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
  fi
  [[ -d "$root/unittests" ]] || err "unit-test directory is unavailable"
  cd "$root"

  local registered
  registered="$({
    find unittests -type f -name Makefile \
      ! -path 'unittests/Makefile' \
      -exec grep -l '^check:' {} + || true
  } | wc -l | tr -d '[:space:]')"
  [[ "$registered" =~ ^[0-9]+$ ]] || err "cannot count registered test targets"
  (( registered > 0 )) || err "no unit-test targets were generated"

  test_output="$(mktemp "${TMPDIR:-/tmp}/gc-unit-tests.XXXXXX")"

  local -a make_command=(make)
  if [[ -n "${GC_TEST_MAKE_COMMAND:-}" ]]; then
    [[ -f "$GC_TEST_MAKE_COMMAND" ]] || err "test make command is unavailable"
    make_command=(bash "$GC_TEST_MAKE_COMMAND")
  fi

  local status
  set +e
  QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
    "${make_command[@]}" -j1 check 2>&1 | tee "$test_output"
  status="${PIPESTATUS[0]}"
  set -e
  (( status == 0 )) || exit "$status"

  local suites cases
  suites="$(grep -Ec 'Totals:[[:space:]]+[0-9]+[[:space:]]+passed' "$test_output" || true)"
  cases="$(awk '
    /Totals:[[:space:]]+[0-9]+[[:space:]]+passed/ { total += $2 }
    END { print total + 0 }
  ' "$test_output")"
  (( suites > 0 )) || err "test command completed without a QtTest result"
  (( cases > 0 )) || err "test command reported zero executed test cases"
  log "Executed $cases QtTest cases across $suites suites."
}

main "$@"
