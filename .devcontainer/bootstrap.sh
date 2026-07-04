#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"

if [[ ! -f "${repo_root}/qwt/qwtconfig.pri" ]]; then
    cp "${repo_root}/qwt/qwtconfig.pri.in" "${repo_root}/qwt/qwtconfig.pri"
fi

if [[ ! -f "${repo_root}/src/gcconfig.pri" ]]; then
    cp "${script_dir}/gcconfig.pri" "${repo_root}/src/gcconfig.pri"
fi

mkdir -p "${repo_root}/build-devcontainer"

qmake --version
echo "GoldenCheetah development files are ready in ${repo_root}."
