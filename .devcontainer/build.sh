#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build-devcontainer}"
jobs="${JOBS:-$(nproc)}"

bash "${script_dir}/bootstrap.sh"
cd "${build_dir}"

qmake -recursive "${repo_root}/build.pro"
make -j"${jobs}"

echo "Build completed. Binary: ${build_dir}/src/GoldenCheetah"
