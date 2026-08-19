#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build-devcontainer}"
jobs="${JOBS:-$(nproc)}"

if command -v ccache >/dev/null 2>&1; then
    export PATH="/usr/lib/ccache:${PATH}"
    export CCACHE_DIR="${CCACHE_DIR:-${HOME}/.cache/ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-${repo_root}}"
    mkdir -p "${CCACHE_DIR}"
    ccache --set-config="max_size=${CCACHE_MAXSIZE:-12G}"
    trap 'ccache --show-stats || true' EXIT
fi

bash "${script_dir}/bootstrap.sh"
cd "${build_dir}"

qmake -recursive "${repo_root}/build.pro"
make -j"${jobs}"

echo "Build completed. Binary: ${build_dir}/src/GoldenCheetah"
