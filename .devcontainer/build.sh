#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build-devcontainer}"
jobs="${JOBS:-$(nproc)}"
qmake_arguments=(-recursive "${repo_root}/build.pro")

if command -v ccache >/dev/null 2>&1; then
    export PATH="/usr/lib/ccache:${PATH}"
    export CCACHE_DIR="${CCACHE_DIR:-${HOME}/.cache/ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-${repo_root}}"
    mkdir -p "${CCACHE_DIR}"
    ccache --set-config="max_size=${CCACHE_MAXSIZE:-12G}"
    ccache --set-config="sloppiness=pch_defines,time_macros"
    qmake_arguments+=("QMAKE_CXXFLAGS+=-fpch-preprocess")
    trap 'ccache --show-stats || true' EXIT
fi

bash "${script_dir}/bootstrap.sh"
mkdir -p "${build_dir}"
cd "${build_dir}"

qmake "${qmake_arguments[@]}"

# qmake tracks PCH header dependencies but not changes to compiler options. Use
# the generated app Makefile and toolchain versions as an options signature so
# unchanged development builds retain their PCH and remain incremental.
app_makefile="${build_dir}/src/Makefile"
pch_signature_file="${build_dir}/.goldencheetah-pch-signature"
if [[ ! -f "${app_makefile}" ]]; then
    echo "Generated application Makefile is missing: ${app_makefile}" >&2
    exit 1
fi

compiler_line="$(sed -n -E 's/^CXX[[:space:]]*=[[:space:]]*(.*)$/\1/p' \
    "${app_makefile}" | head -n 1)"
read -r -a compiler_command <<< "${compiler_line}"
if (( ${#compiler_command[@]} == 0 )); then
    echo "Cannot determine the C++ compiler from ${app_makefile}" >&2
    exit 1
fi

pch_signature="$({
    sed -n -E \
        '/^(CXX|DEFINES|CXXFLAGS|INCPATH|QMAKE)[[:space:]]*=/p' \
        "${app_makefile}"
    command -v "${compiler_command[0]}"
    "${compiler_command[@]}" --version
    qmake -v
} | sha256sum | awk '{print $1}')"

previous_pch_signature=""
if [[ -f "${pch_signature_file}" ]]; then
    previous_pch_signature="$(<"${pch_signature_file}")"
fi

if [[ "${pch_signature}" != "${previous_pch_signature}" ]]; then
    find -P "${build_dir}" -type d -name 'GoldenCheetah.gch' -prune \
        -exec rm -rf -- {} +
    make -C "${build_dir}/src" -j"${jobs}" GoldenCheetah.gch/c++
    pch_signature_tmp="${pch_signature_file}.tmp.$$"
    printf '%s\n' "${pch_signature}" > "${pch_signature_tmp}"
    mv -f -- "${pch_signature_tmp}" "${pch_signature_file}"
fi

make -j"${jobs}"

echo "Build completed. Binary: ${build_dir}/src/GoldenCheetah"
