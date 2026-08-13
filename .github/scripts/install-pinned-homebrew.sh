#!/usr/bin/env bash
set -Eeuo pipefail

readonly HOMEBREW_BREW_COMMIT=${HOMEBREW_BREW_COMMIT:-67658c8cf6ee685420c531ed94ed46b6e7ba5b2a}
readonly HOMEBREW_CORE_COMMIT=${HOMEBREW_CORE_COMMIT:-2602f7f80784581466deb491f09bf734174ac772}
readonly HOMEBREW_BREW_REMOTE=https://github.com/Homebrew/brew.git
readonly HOMEBREW_CORE_REMOTE=https://github.com/Homebrew/homebrew-core.git

export HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALL_FROM_API=1

pin_repository() {
  local repository=$1 remote=$2 commit=$3
  if [[ ! -d ${repository}/.git ]]; then
    if [[ -e ${repository} && ! -d ${repository} ]]; then
      echo "Refusing non-directory Homebrew repository: ${repository}" >&2
      return 1
    fi
    mkdir -p "${repository}"
    git -C "${repository}" init
    git -C "${repository}" remote add origin "${remote}"
  else
    git -C "${repository}" remote set-url origin "${remote}"
  fi
  git -C "${repository}" fetch --force --depth=1 origin "${commit}"
  git -C "${repository}" checkout --detach --force FETCH_HEAD
  [[ $(git -C "${repository}" rev-parse HEAD) == "${commit}" ]]
}

receipt_matches() {
  local formula=$1 expected_version=$2 receipt
  [[ $(brew list --versions --formula "${formula}" 2>/dev/null || true) == \
    "${formula} ${expected_version}" ]] || return 1
  receipt="$(brew --prefix "${formula}")/INSTALL_RECEIPT.json"
  [[ -f ${receipt} ]] || return 1
  /usr/bin/python3 - "${receipt}" "${HOMEBREW_CORE_COMMIT}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    receipt = json.load(stream)
if receipt.get("source", {}).get("tap_git_head") != sys.argv[2]:
    raise SystemExit(1)
PY
}

install_formula() {
  local specification=$1 formula=${1%%=*} expected_version=${1#*=}
  [[ ${formula} != "${expected_version}" ]] || {
    echo "Formula must include an exact expected version: ${specification}" >&2
    return 1
  }
  # A local receipt describes provenance but does not authenticate the keg's
  # current bytes. Reinstall every existing formula from the pinned tap.
  if brew list --versions --formula "${formula}" >/dev/null 2>&1; then
    brew reinstall --formula "${formula}"
  else
    brew install --formula "${formula}"
  fi
  receipt_matches "${formula}" "${expected_version}"
}

BREW_REPOSITORY=$(brew --repository)
CORE_REPOSITORY="${BREW_REPOSITORY}/Library/Taps/homebrew/homebrew-core"
pin_repository "${BREW_REPOSITORY}" "${HOMEBREW_BREW_REMOTE}" "${HOMEBREW_BREW_COMMIT}"
pin_repository "${CORE_REPOSITORY}" "${HOMEBREW_CORE_REMOTE}" "${HOMEBREW_CORE_COMMIT}"

for specification in "$@"; do
  install_formula "${specification}"
done

[[ $(git -C "${BREW_REPOSITORY}" rev-parse HEAD) == "${HOMEBREW_BREW_COMMIT}" ]]
[[ $(git -C "${CORE_REPOSITORY}" rev-parse HEAD) == "${HOMEBREW_CORE_COMMIT}" ]]
