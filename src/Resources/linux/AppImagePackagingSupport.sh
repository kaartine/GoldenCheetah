#!/usr/bin/env bash

PYTHON_APPIMAGE_SERIES="3.11"
PYTHON_APPIMAGE_VERSION="3.11.15"
PYTHON_APPIMAGE_ABI="cp311"
PYTHON_APPIMAGE_PLATFORM="manylinux2014_x86_64"
PYTHON_APPIMAGE_SHA256="2d8ecd8002fae06813d4c92ba5244f573aae9bf84eaf41a1b189b623112e3dec"
PYTHON_APPIMAGE_FILE="python${PYTHON_APPIMAGE_VERSION}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_ABI}-${PYTHON_APPIMAGE_PLATFORM}.AppImage"
PYTHON_APPIMAGE_URL="https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/${PYTHON_APPIMAGE_FILE}"

LINUXDEPLOYQT_FILE="linuxdeployqt-continuous-x86_64.AppImage"
LINUXDEPLOYQT_URL="https://github.com/probonopd/linuxdeployqt/releases/download/continuous/${LINUXDEPLOYQT_FILE}"
APPIMAGETOOL_FILE="appimagetool-x86_64.AppImage"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/continuous/${APPIMAGETOOL_FILE}"
QTKEYCHAIN_LICENSE_SHA256="ca46b73d5159548ab52834db51f195aa3d1f277f020e9dca92f4beb21b468a50"
LGPL21_LICENSE_SHA256="dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551"

download_file()
{
    local url=$1
    local destination=$2

    if command -v curl >/dev/null 2>&1; then
        curl --fail --location --retry 3 --silent --show-error \
            --output "$destination" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget --no-verbose --output-document="$destination" "$url"
    else
        echo "Neither curl nor wget is available" >&2
        return 1
    fi
}

run_packaging_appimage()
{
    APPIMAGE_EXTRACT_AND_RUN=1 \
        env -u APPDIR -u APPIMAGE -u OWD \
            -u LD_LIBRARY_PATH -u LD_PRELOAD "$@"
}

run_packaged_appimage_smoke()
{
    APPIMAGE_EXTRACT_AND_RUN=1 \
        env -u APPDIR -u APPIMAGE -u OWD \
            -u LD_LIBRARY_PATH -u LD_PRELOAD timeout "$@"
}

install_qt_offscreen_plugin()
{
    local qmake_command=$1
    local app_dir=$2
    local qmake_path qt_plugins_dir source_plugin
    local plugins_dir target_dir target_plugin

    qmake_path=$(command -v -- "$qmake_command") || {
        echo "Qt qmake executable not found: $qmake_command" >&2
        return 1
    }
    if [ ! -x "$qmake_path" ]; then
        echo "Qt qmake executable is not runnable: $qmake_path" >&2
        return 1
    fi
    qt_plugins_dir=$(
        "$qmake_path" -query QT_INSTALL_PLUGINS
    ) || {
        echo "Cannot resolve the Qt plugin directory." >&2
        return 1
    }
    case "$qt_plugins_dir" in
    ""|*'
'*)
        echo "Qt returned an invalid plugin directory." >&2
        return 1
        ;;
    esac

    source_plugin="$qt_plugins_dir/platforms/libqoffscreen.so"
    if [ ! -f "$source_plugin" ] || [ ! -r "$source_plugin" ]; then
        echo "Qt offscreen platform plugin not found: $source_plugin" >&2
        return 1
    fi
    if [ ! -d "$app_dir" ] || [ -L "$app_dir" ]; then
        echo "Invalid AppDir for the Qt offscreen plugin: $app_dir" >&2
        return 1
    fi

    plugins_dir="$app_dir/plugins"
    target_dir="$plugins_dir/platforms"
    if [ -L "$plugins_dir" ] || [ -L "$target_dir" ]; then
        echo "Refusing a linked AppDir platform plugin directory." >&2
        return 1
    fi
    mkdir -p -- "$target_dir" || return
    target_plugin="$target_dir/libqoffscreen.so"
    install -m 0755 -- "$source_plugin" "$target_plugin" || return
    if [ ! -f "$target_plugin" ] ||
       ! cmp -s -- "$source_plugin" "$target_plugin"; then
        echo "Qt offscreen platform plugin was not installed intact." >&2
        return 1
    fi
    echo "Qt offscreen plugin: bundled"
}

require_qt_offscreen_appimage()
(
    local image=$1
    local smoke_duration=${2:-10s}
    local smoke_home= smoke_log=

    cleanup_offscreen_smoke()
    {
        if [ -n "$smoke_home" ]; then
            rm -rf -- "$smoke_home"
            smoke_home=
        fi
        if [ -n "$smoke_log" ]; then
            rm -f -- "$smoke_log"
            smoke_log=
        fi
    }
    trap cleanup_offscreen_smoke EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot smoke-test Qt offscreen runtime in $image" >&2
        return 1
    fi
    smoke_home=$(mktemp -d) || return
    smoke_log=$(mktemp) || return
    mkdir -p "$smoke_home/.config" "$smoke_home/.cache" || return

    local status
    if HOME="$smoke_home" \
       XDG_CONFIG_HOME="$smoke_home/.config" \
       XDG_CACHE_HOME="$smoke_home/.cache" \
       DISPLAY= WAYLAND_DISPLAY= \
       QT_QPA_PLATFORM=offscreen \
       QT_OPENGL=software \
       LIBGL_ALWAYS_SOFTWARE=1 \
       QTWEBENGINE_DISABLE_SANDBOX=1 \
       run_packaged_appimage_smoke \
           --kill-after=2s "$smoke_duration" "$image" \
           >"$smoke_log" 2>&1; then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 124 ]; then
        cat "$smoke_log" >&2
        echo "AppImage Qt offscreen smoke failed with status $status" >&2
        return 1
    fi
    echo "Qt offscreen runtime: available"
)

strava_oauth_build_status()
(
    local executable=$1
    local allow_script_entrypoint=${2:-false}
    local status_home=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    cleanup_status_home()
    {
        if [ -n "$status_home" ]; then
            rm -rf -- "$status_home"
            status_home=
        fi
    }
    trap cleanup_status_home EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot inspect Strava OAuth configuration in $executable" >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$allow_script_entrypoint" != "true" ] ||
           [ "$script_magic" != "2321" ]; then
            echo "Strava OAuth build status requires an ELF executable." >&2
            return 1
        fi
    fi
    if [ "$elf_magic" = "7f454c46" ]; then
        local appimage_magic
        appimage_magic=$(dd if="$executable" bs=1 skip=8 count=3 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$appimage_magic" = "414901" ] ||
           [ "$appimage_magic" = "414902" ]; then
            echo "Cannot inspect Strava OAuth configuration in a compressed AppImage;" \
                "inspect its extracted GoldenCheetah executable." >&2
            return 1
        fi
    fi

    local report captured status_file command_status
    status_home=$(mktemp -d) || return
    if ! mkdir -p "$status_home/.config"; then
        return 1
    fi
    status_file="$status_home/build-status"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s \
                "$executable" --goldencheetah-build-status
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    captured=$(
        cat "$status_file" 2>/dev/null
        printf x
    )
    report=${captured%x}
    if [ "$command_status" -ne 0 ]; then
        echo "GoldenCheetah build-status command failed." >&2
        return 1
    fi
    local configured_report unavailable_report
    configured_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=configured"
        printf x
    )
    configured_report=${configured_report%x}
    unavailable_report=$(
        printf '%s\n' \
            "goldencheetah_build_status=1" \
            "application=GoldenCheetah" \
            "strava_support=enabled" \
            "strava_oauth=unavailable"
        printf x
    )
    unavailable_report=${unavailable_report%x}
    if [ "$report" = "$configured_report" ]; then
        echo "Strava OAuth: configured"
        return
    fi
    if [ "$report" = "$unavailable_report" ]; then
        echo "Strava OAuth: unavailable (credentials not configured)"
        return
    fi
    echo "GoldenCheetah returned an invalid build-status report." >&2
    return 1
)

require_strava_oauth_build()
{
    local executable=$1
    local status

    status=$(strava_oauth_build_status "$executable") || return
    if [ "$status" != "Strava OAuth: configured" ]; then
        echo "$status" >&2
        echo "Refusing to package a release without configured" \
            "Strava OAuth credentials." >&2
        return 1
    fi
    echo "$status"
}

strava_oauth_appimage_status()
(
    local image=$1
    local extract_dir=
    local original_working_dir
    local APPDIR= APPIMAGE= OWD=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    original_working_dir=$(pwd -P) || return

    cleanup_extract_dir()
    {
        if [ -n "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
            extract_dir=
        fi
    }
    trap cleanup_extract_dir EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot inspect Strava OAuth configuration in $image" >&2
        return 1
    fi
    local appimage_magic
    appimage_magic=$(dd if="$image" bs=1 skip=8 count=3 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$appimage_magic" != "414902" ]; then
        echo "Strava OAuth package status requires a Type 2 AppImage." >&2
        return 1
    fi

    local image_dir image_name image_path app_root
    local entrypoint status
    image_dir=$(cd -- "$(dirname -- "$image")" && pwd) || return
    image_name=$(basename -- "$image") || return
    image_path="$image_dir/$image_name"
    extract_dir=$(mktemp -d) || return
    app_root="$extract_dir/squashfs-root"

    if (
        cd "$extract_dir" || exit
        run_packaging_appimage \
            "$image_path" --appimage-extract \
            >/dev/null 2>&1 || exit
    ); then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        return "$status"
    fi

    entrypoint=$(readlink -f "$app_root/AppRun") || return
    case "$entrypoint" in
    "$app_root"/*) ;;
    *)
        echo "AppImage AppRun resolves outside its payload." >&2
        return 1
        ;;
    esac
    APPDIR="$app_root" \
        APPIMAGE="$image_path" \
        OWD="$original_working_dir" \
        strava_oauth_build_status "$app_root/AppRun" true
)

require_strava_oauth_appimage()
{
    local image=$1
    local status

    status=$(strava_oauth_appimage_status "$image") || return
    if [ "$status" != "Strava OAuth: configured" ]; then
        echo "$status" >&2
        echo "Refusing to publish an AppImage without configured" \
            "Strava OAuth credentials." >&2
        return 1
    fi
    echo "$status"
}

file_matches_sha256()
{
    local file=$1
    local expected=$2
    local actual

    actual=$(sha256sum -- "$file" 2>/dev/null |
        awk '{ print $1 }') || return
    [ "$actual" = "$expected" ]
}

libsecret_copyright_is_valid()
{
    local copyright=$1

    grep -Fqx -- "Upstream-Name: libsecret" "$copyright" &&
        grep -Eq '^Copyright:[[:space:]]*[^[:space:]]' "$copyright" &&
        grep -Eq '^License: LGPL-2\.1\+' "$copyright" &&
        grep -Fq -- "GNU Lesser General Public" "$copyright" &&
        grep -Fq -- \
            "License as published by the Free Software Foundation" "$copyright"
}

linux_keychain_payload_library_is_valid()
{
    local app_root=$1
    local library=$2
    local resolved

    if [ -L "$library" ] ||
       [ ! -f "$library" ] || [ ! -r "$library" ]; then
        return 1
    fi
    resolved=$(readlink -f -- "$library") || return
    case "$resolved" in
    "$app_root"/lib/*) [ "$resolved" = "$library" ] ;;
    *) return 1 ;;
    esac
}

linux_system_library_path_is_valid()
{
    local library=$1
    local resolved

    case "$library" in
    /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
    *) return 1 ;;
    esac
    resolved=$(readlink -f -- "$library") || return
    case "$resolved" in
    /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*)
        [ -f "$resolved" ] && [ -r "$resolved" ]
        ;;
    *) return 1 ;;
    esac
}

linux_keychain_runtime_status()
{
    local appdir=$1
    local app_root
    app_root=$(cd -- "$appdir" && pwd -P) || {
        echo "Linux keychain AppDir is missing." >&2
        return 1
    }
    local library="$app_root/lib/libsecret-1.so.0"
    local license_dir="$app_root/usr/share/doc/GoldenCheetah/licenses"
    local libsecret_copyright="$license_dir/libsecret-copyright"
    local qtkeychain_license="$license_dir/QtKeychain-COPYING"
    local libsecret_license="$license_dir/LGPL-2.1"
    local dependencies="
libglib-2.0.so.0
libgio-2.0.so.0
libgobject-2.0.so.0
libgcrypt.so.20
libgpg-error.so.0"

    if [ -L "$library" ] ||
       [ ! -f "$library" ] || [ ! -r "$library" ]; then
        echo "Bundled libsecret runtime is missing." >&2
        return 1
    fi
    local payload resolved_payload
    for payload in \
        "$library" \
        "$libsecret_copyright" \
        "$qtkeychain_license" \
        "$libsecret_license"; do
        if [ -L "$payload" ] || [ ! -f "$payload" ]; then
            echo "Linux keychain payload contains a missing or linked file." >&2
            return 1
        fi
        resolved_payload=$(readlink -f -- "$payload") || return
        case "$resolved_payload" in
        "$app_root"/*) ;;
        *)
            echo "Linux keychain payload resolves outside its AppDir." >&2
            return 1
            ;;
        esac
    done
    local dependency
    for dependency in $dependencies; do
        payload="$app_root/lib/$dependency"
        if [ -L "$payload" ] ||
           [ ! -f "$payload" ] || [ ! -r "$payload" ]; then
            echo "Bundled libsecret dependency is missing." >&2
            return 1
        fi
        resolved_payload=$(readlink -f -- "$payload") || return
        case "$resolved_payload" in
        "$app_root"/lib/*) ;;
        *)
            echo "Bundled libsecret dependency escapes the AppDir." >&2
            return 1
            ;;
        esac
    done

    local elf_magic
    elf_magic=$(dd if="$library" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        echo "Bundled libsecret runtime is not an ELF library." >&2
        return 1
    fi
    if ! LC_ALL=C readelf -d "$library" 2>/dev/null |
        grep -Eq '\(SONAME\).*\[libsecret-1\.so\.0\]'; then
        echo "Bundled libsecret runtime has an unexpected SONAME." >&2
        return 1
    fi
    if ! LC_ALL=C readelf -d "$library" 2>/dev/null |
        grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]'; then
        echo "Bundled libsecret runtime has no local dependency path." >&2
        return 1
    fi
    local dynamic_symbols required_symbol
    dynamic_symbols=$(LC_ALL=C readelf --wide --dyn-syms \
        "$library" 2>/dev/null) || {
        echo "Cannot inspect bundled libsecret symbols." >&2
        return 1
    }
    for required_symbol in \
        secret_password_lookup \
        secret_password_lookup_finish \
        secret_password_store \
        secret_password_store_finish \
        secret_password_clear \
        secret_password_clear_finish \
        secret_password_free \
        secret_error_get_quark; do
        if ! printf '%s\n' "$dynamic_symbols" |
            awk -v symbol="$required_symbol" '
                $4 == "FUNC" && $5 == "GLOBAL" &&
                $7 != "UND" && $8 == symbol { found = 1 }
                END { exit !found }
            '; then
            echo "Bundled libsecret runtime is missing a required symbol." >&2
            return 1
        fi
    done
    local ldd_output expected_dependency
    ldd_output=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD \
        LC_ALL=C ldd "$library" 2>/dev/null) || {
        echo "Cannot resolve bundled libsecret dependencies." >&2
        return 1
    }
    for dependency in $dependencies; do
        expected_dependency="$dependency => $app_root/lib/$dependency "
        if ! printf '%s\n' "$ldd_output" |
            grep -Fq -- "$expected_dependency"; then
            echo "A libsecret dependency resolves outside the AppDir." >&2
            return 1
        fi
    done
    local dependency_arrow dependency_path dependency_tail
    while read -r dependency dependency_arrow dependency_path \
        dependency_tail; do
        if [ -z "$dependency" ]; then
            continue
        fi
        if [ "$dependency_arrow" = "=>" ]; then
            if [ "$dependency_path" = "not" ]; then
                echo "A bundled libsecret dependency is unresolved." >&2
                return 1
            fi
            if linux_keychain_payload_library_is_valid \
                "$app_root" "$dependency_path"; then
                continue
            fi
            case "$dependency" in
            libc.so.6|libdl.so.2|libgcc_s.so.1|libm.so.6|\
            libpthread.so.0|libresolv.so.2|librt.so.1|libz.so.1)
                if linux_system_library_path_is_valid \
                    "$dependency_path"; then
                    continue
                fi
                ;;
            esac
            echo "An unexpected dependency resolves outside the AppDir." >&2
            return 1
        fi
        case "$dependency" in
        linux-vdso.so.1|linux-gate.so.1) continue ;;
        /*)
            if linux_keychain_payload_library_is_valid \
                "$app_root" "$dependency"; then
                continue
            fi
            dependency_path=${dependency##*/}
            case "$dependency_path" in
            ld-linux*.so.*)
                if linux_system_library_path_is_valid "$dependency"; then
                    continue
                fi
                ;;
            esac
            ;;
        esac
        echo "An unrecognized libsecret dependency was reported." >&2
        return 1
    done <<<"$ldd_output"
    if ! libsecret_copyright_is_valid \
        "$libsecret_copyright"; then
        echo "Bundled libsecret copyright is invalid." >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$qtkeychain_license" "$QTKEYCHAIN_LICENSE_SHA256"; then
        echo "Bundled QtKeychain license is invalid." >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$libsecret_license" "$LGPL21_LICENSE_SHA256"; then
        echo "Bundled LGPL-2.1 license is invalid." >&2
        return 1
    fi
    echo "Linux keychain runtime: bundled"
}

prepare_contained_appdir_directory()
(
    local appdir=$1
    local relative=$2

    if [ -L "$appdir" ] || [ ! -d "$appdir" ]; then
        echo "Linux keychain AppDir must be a regular directory." >&2
        return 1
    fi
    case "$relative" in
    ""|/*|..|../*|*/..|*/../*)
        echo "Invalid Linux keychain AppDir directory path." >&2
        return 1
        ;;
    esac

    local app_root current component resolved
    app_root=$(cd -- "$appdir" && pwd -P) || return
    current=$app_root
    local IFS=/
    for component in $relative; do
        [ -n "$component" ] || continue
        current="$current/$component"
        if [ -L "$current" ]; then
            echo "Linux keychain AppDir directory is a symlink." >&2
            return 1
        fi
        if [ -e "$current" ]; then
            if [ ! -d "$current" ]; then
                echo "Linux keychain AppDir path is not a directory." >&2
                return 1
            fi
        elif ! mkdir -- "$current"; then
            return 1
        fi
    done
    resolved=$(cd -- "$current" && pwd -P) || return
    if [ "$resolved" != "$app_root/$relative" ]; then
        echo "Linux keychain AppDir directory escapes its root." >&2
        return 1
    fi
    printf '%s\n' "$resolved"
)

install_regular_keychain_payload()
{
    local source=$1
    local destination=$2

    if [ -L "$destination" ] ||
       { [ -e "$destination" ] && [ ! -f "$destination" ]; }; then
        echo "Linux keychain payload destination is not a regular file." >&2
        return 1
    fi
    install -m 0644 "$source" "$destination"
}

install_linux_keychain_runtime()
{
    local appdir=$1
    local qtkeychain_license=$2
    local libsecret_copyright=${LIBSECRET_COPYRIGHT_FILE:-}
    local libsecret_license=${LIBSECRET_LICENSE_FILE:-}
    local libsecret_libdir library resolved_library
    local gpg_error_libdir gpg_error_library resolved_gpg_error
    local lib_dir libgcrypt license_dir

    if [ -z "$libsecret_copyright" ]; then
        libsecret_copyright=/usr/share/doc/libsecret-1-0/copyright
    fi
    if [ -z "$libsecret_license" ]; then
        libsecret_license=/usr/share/common-licenses/LGPL-2.1
    fi
    libsecret_libdir=$(pkg-config --variable=libdir libsecret-1) || {
        echo "Cannot resolve the libsecret runtime directory." >&2
        return 1
    }
    library="$libsecret_libdir/libsecret-1.so.0"
    resolved_library=$(readlink -f -- "$library") || {
        echo "Cannot resolve the libsecret runtime: $library" >&2
        return 1
    }
    if [ ! -f "$resolved_library" ] || [ ! -r "$resolved_library" ]; then
        echo "libsecret runtime not found: $resolved_library" >&2
        return 1
    fi
    gpg_error_libdir=$(pkg-config --variable=libdir gpg-error) || {
        echo "Cannot resolve the libgpg-error runtime directory." >&2
        return 1
    }
    gpg_error_library="$gpg_error_libdir/libgpg-error.so.0"
    resolved_gpg_error=$(readlink -f -- "$gpg_error_library") || {
        echo "Cannot resolve the libgpg-error runtime:" \
            "$gpg_error_library" >&2
        return 1
    }
    if [ ! -f "$resolved_gpg_error" ] ||
       [ ! -r "$resolved_gpg_error" ]; then
        echo "libgpg-error runtime not found: $resolved_gpg_error" >&2
        return 1
    fi
    if ! libsecret_copyright_is_valid \
        "$libsecret_copyright"; then
        echo "libsecret copyright file is invalid: $libsecret_copyright" >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$qtkeychain_license" "$QTKEYCHAIN_LICENSE_SHA256"; then
        echo "QtKeychain license is invalid: $qtkeychain_license" >&2
        return 1
    fi
    if ! file_matches_sha256 \
        "$libsecret_license" "$LGPL21_LICENSE_SHA256"; then
        echo "LGPL-2.1 license is invalid: $libsecret_license" >&2
        return 1
    fi

    lib_dir=$(prepare_contained_appdir_directory \
        "$appdir" lib) || return
    license_dir=$(prepare_contained_appdir_directory \
        "$appdir" usr/share/doc/GoldenCheetah/licenses) || return
    libgcrypt="$lib_dir/libgcrypt.so.20"
    if [ -L "$libgcrypt" ] ||
       [ ! -f "$libgcrypt" ] || [ ! -r "$libgcrypt" ]; then
        echo "Bundled libgcrypt runtime is missing." >&2
        return 1
    fi

    install_regular_keychain_payload \
        "$resolved_library" "$lib_dir/libsecret-1.so.0" || return
    install_regular_keychain_payload \
        "$resolved_gpg_error" "$lib_dir/libgpg-error.so.0" || return
    patchelf --set-rpath '$ORIGIN' \
        "$lib_dir/libsecret-1.so.0"
    patchelf --set-rpath '$ORIGIN' "$libgcrypt"
    install_regular_keychain_payload \
        "$libsecret_copyright" \
        "$license_dir/libsecret-copyright" || return
    install_regular_keychain_payload \
        "$qtkeychain_license" \
        "$license_dir/QtKeychain-COPYING" || return
    install_regular_keychain_payload \
        "$libsecret_license" \
        "$license_dir/LGPL-2.1" || return
    linux_keychain_runtime_status "$appdir"
}

create_linux_keychain_deploy_probe()
{
    local executable=$1
    local directory=$2
    local probe="$directory/.goldencheetah-libsecret-deploy-probe"

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot create a libsecret deploy probe from $executable" >&2
        return 1
    fi
    if [ ! -d "$directory" ] || [ -e "$probe" ]; then
        echo "Cannot create the libsecret deploy probe in $directory" >&2
        return 1
    fi
    if ! install -m 0755 "$executable" "$probe"; then
        return 1
    fi
    if ! patchelf --add-needed libsecret-1.so.0 "$probe"; then
        rm -f -- "$probe"
        return 1
    fi
    if ! LC_ALL=C readelf -d "$probe" 2>/dev/null |
        grep -Eq '\(NEEDED\).*\[libsecret-1\.so\.0\]'; then
        rm -f -- "$probe"
        echo "The libsecret deploy probe has no DT_NEEDED entry." >&2
        return 1
    fi
    printf '%s\n' "$probe"
}

remove_linux_keychain_deploy_probe()
{
    local probe=$1
    case "$probe" in
    */.goldencheetah-libsecret-deploy-probe) ;;
    *)
        echo "Refusing to remove an unexpected deploy probe path." >&2
        return 1
        ;;
    esac
    rm -f -- "$probe"
}

run_linuxdeployqt_with_keychain_probe()
(
    local executable=$1
    local appdir=$2
    shift 2
    local probe command_status

    probe=$(create_linux_keychain_deploy_probe \
        "$executable" "$appdir") || return
    cleanup_linux_keychain_deploy_probe()
    {
        if [ -n "$probe" ]; then
            remove_linux_keychain_deploy_probe "$probe"
        fi
    }
    trap cleanup_linux_keychain_deploy_probe EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if run_packaging_appimage "$@" "-executable=$probe"; then
        command_status=0
    else
        command_status=$?
    fi
    if ! remove_linux_keychain_deploy_probe "$probe"; then
        return 1
    fi
    probe=
    return "$command_status"
)

linux_keychain_entrypoint_status()
(
    local executable=$1
    local status_home=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    cleanup_keychain_status_home()
    {
        if [ -n "$status_home" ]; then
            rm -rf -- "$status_home"
            status_home=
        fi
    }
    trap cleanup_keychain_status_home EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$executable" ] || [ ! -r "$executable" ] ||
       [ ! -x "$executable" ]; then
        echo "Cannot inspect the Linux keychain entrypoint." >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "$script_magic" != "2321" ]; then
            echo "Linux keychain status requires an ELF or script entrypoint." >&2
            return 1
        fi
    fi

    local report captured status_file command_status expected_report
    status_home=$(mktemp -d) || return
    mkdir -p "$status_home/.config" || return
    status_file="$status_home/keychain-status"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            QTKEYCHAIN_BACKEND=libsecret \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s "$executable" \
                --goldencheetah-linux-keychain-status
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    captured=$(
        cat "$status_file" 2>/dev/null
        printf x
    )
    report=${captured%x}
    if [ "$command_status" -ne 0 ]; then
        echo "GoldenCheetah keychain-status command failed." >&2
        return 1
    fi
    expected_report=$(
        printf '%s\n' \
            "goldencheetah_linux_keychain_status=1" \
            "application=GoldenCheetah" \
            "libsecret_compile_support=enabled" \
            "libsecret_runtime=available"
        printf x
    )
    expected_report=${expected_report%x}
    if [ "$report" != "$expected_report" ]; then
        echo "GoldenCheetah returned an unavailable keychain status." >&2
        return 1
    fi
    echo "Linux keychain runtime: available"
)

linux_keychain_appimage_status()
(
    local image=$1
    local extract_dir=
    local original_working_dir
    local APPDIR= APPIMAGE= OWD=
    local LD_LIBRARY_PATH= LD_PRELOAD=

    original_working_dir=$(pwd -P) || return

    cleanup_keychain_extract_dir()
    {
        if [ -n "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
            extract_dir=
        fi
    }
    trap cleanup_keychain_extract_dir EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM

    if [ ! -f "$image" ] || [ ! -r "$image" ] ||
       [ ! -x "$image" ]; then
        echo "Cannot inspect the Linux keychain runtime in $image" >&2
        return 1
    fi
    local appimage_magic
    appimage_magic=$(dd if="$image" bs=1 skip=8 count=3 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$appimage_magic" != "414902" ]; then
        echo "Linux keychain package status requires a Type 2 AppImage." >&2
        return 1
    fi

    local image_dir image_name image_path app_root entrypoint status
    image_dir=$(cd -- "$(dirname -- "$image")" && pwd) || return
    image_name=$(basename -- "$image") || return
    image_path="$image_dir/$image_name"
    extract_dir=$(mktemp -d) || return
    app_root="$extract_dir/squashfs-root"

    if (
        cd "$extract_dir" || exit
        run_packaging_appimage \
            "$image_path" --appimage-extract \
            >/dev/null 2>&1 || exit
    ); then
        status=0
    else
        status=$?
    fi
    if [ "$status" -ne 0 ]; then
        return "$status"
    fi
    linux_keychain_runtime_status "$app_root" >/dev/null || return
    entrypoint=$(readlink -f "$app_root/AppRun") || return
    case "$entrypoint" in
    "$app_root"/*) ;;
    *)
        echo "AppImage AppRun resolves outside its payload." >&2
        return 1
        ;;
    esac
    APPDIR="$app_root" \
        APPIMAGE="$image_path" \
        OWD="$original_working_dir" \
        linux_keychain_entrypoint_status "$app_root/AppRun" \
            >/dev/null || return
    echo "Linux keychain runtime: bundled"
)

require_linux_keychain_appimage()
{
    local image=$1
    local status

    status=$(linux_keychain_appimage_status "$image") || return
    if [ "$status" != "Linux keychain runtime: bundled" ]; then
        echo "$status" >&2
        echo "Refusing to publish an AppImage without the Linux" \
            "keychain runtime and licenses." >&2
        return 1
    fi
    echo "$status"
}

build_provenance_report()
(
    local executable=$1
    local status_home= status_file= command_status
    local -a lines=()

    cleanup_build_provenance_home()
    {
        if [ -n "$status_home" ] && [ -d "$status_home" ]; then
            rm -rf -- "$status_home"
        fi
    }
    trap cleanup_build_provenance_home EXIT

    if [ ! -f "$executable" ] || [ ! -x "$executable" ]; then
        echo "Cannot inspect build provenance in $executable" >&2
        return 1
    fi
    local elf_magic script_magic
    elf_magic=$(dd if="$executable" bs=1 count=4 \
        2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$elf_magic" != "7f454c46" ]; then
        script_magic=$(dd if="$executable" bs=1 count=2 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        if [ "${GC_TEST_BUILD_PROVENANCE_ENTRYPOINT:-false}" != true ] ||
           [ "$script_magic" != "2321" ]; then
            echo "Build provenance requires an ELF executable." >&2
            return 1
        fi
    fi

    status_home=$(mktemp -d) || return
    mkdir -p "$status_home/.config" || return
    status_file="$status_home/build-provenance"
    if ! (
        umask 077
        : >"$status_file"
    ); then
        return 1
    fi
    if (
        ulimit -c 0
        ulimit -f 8
        HOME="$status_home" \
            XDG_CONFIG_HOME="$status_home/.config" \
            LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            timeout --signal=TERM --kill-after=2s 10s \
                "$executable" --goldencheetah-build-provenance
    ) >"$status_file" 2>/dev/null; then
        command_status=0
    else
        command_status=$?
    fi
    if [ "$command_status" -ne 0 ] ||
       [ ! -s "$status_file" ] ||
       [ "$(wc -c <"$status_file")" -gt 4096 ] ||
       [ "$(tail -c 1 "$status_file" | od -An -tu1 | tr -d ' \n')" != 10 ]; then
        echo "GoldenCheetah build-provenance command failed." >&2
        return 1
    fi

    if ! LC_ALL=C tr -d '\000' <"$status_file" | cmp -s - "$status_file"; then
        echo "GoldenCheetah returned an invalid build-provenance report." >&2
        return 1
    fi
    mapfile -t lines <"$status_file"
    if [ "${#lines[@]}" -ne 7 ] ||
       [ "${lines[0]}" != "goldencheetah_build_provenance=1" ] ||
       [ "${lines[1]}" != "application=GoldenCheetah" ] ||
       [[ ! "${lines[2]}" =~ ^source_revision=[0-9a-f]{40}$ ]] ||
       [[ ! "${lines[3]}" =~ ^compiler_family=(gcc|clang)$ ]] ||
       [[ ! "${lines[4]}" =~ ^compiler_version=[0-9]+(\.[0-9]+){1,3}$ ]] ||
       [[ ! "${lines[5]}" =~ ^qt_version=[0-9]+(\.[0-9]+){1,3}$ ]] ||
       [[ ! "${lines[6]}" =~ ^cxx_standard=[0-9]+$ ]]; then
        echo "GoldenCheetah returned an invalid build-provenance report." >&2
        return 1
    fi
    printf '%s\n' "${lines[@]}"
)

verified_source_revision()
(
    local source_root=$1
    local git_root revision head status

    git_root=$(git -C "$source_root" rev-parse --show-toplevel 2>/dev/null) || {
        echo "AppImage packaging requires a Git source tree." >&2
        return 1
    }
    if [ "$(cd "$source_root" && pwd -P)" != "$(cd "$git_root" && pwd -P)" ]; then
        echo "The AppImage source root is not the Git worktree root." >&2
        return 1
    fi
    revision=${GC_SOURCE_REVISION:-$(git -C "$git_root" rev-parse HEAD)}
    if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
        echo "GC_SOURCE_REVISION must be a full lowercase Git commit hash" >&2
        return 1
    fi
    git -C "$git_root" cat-file -e "$revision^{commit}" 2>/dev/null || {
        echo "GC_SOURCE_REVISION does not identify a source commit." >&2
        return 1
    }
    head=$(git -C "$git_root" rev-parse HEAD) || return
    if [ "$revision" != "$head" ]; then
        echo "GC_SOURCE_REVISION does not match the source HEAD." >&2
        return 1
    fi
    status=$(git -C "$git_root" status --porcelain=v1 \
        --untracked-files=normal --ignore-submodules=none) || return
    if [ -n "$status" ]; then
        echo "Refusing to package a dirty source worktree." >&2
        return 1
    fi
    printf '%s\n' "$revision"
)

validate_appimage_base_manifest()
{
    local manifest=$1
    local -a lines=()

    [ -f "$manifest" ] && [ ! -L "$manifest" ] || return 1
    [ "$(wc -c <"$manifest")" -le 4096 ] || return 1
    [ "$(tail -c 1 "$manifest" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$manifest"
    [ "${#lines[@]}" -eq 5 ] || return 1
    [ "${lines[0]}" = "goldencheetah_appimage_manifest=1" ] || return 1
    [[ "${lines[1]}" =~ ^source_revision=[0-9a-f]{40}$ ]] || return 1
    [[ "${lines[2]}" =~ ^raw_elf_sha256=[0-9a-f]{64}$ ]] || return 1
    [[ "${lines[3]}" =~ ^toolchain=(gcc|clang)-[0-9]+(\.[0-9]+){1,3}_qt-[0-9]+(\.[0-9]+){1,3}_cxx-[0-9]+$ ]] ||
        return 1
    [[ "${lines[4]}" =~ ^strava_oauth_configured=(true|false)$ ]] ||
        return 1
}

create_appimage_build_manifest()
{
    local source_root=$1
    local binary=$2
    local oauth_status=$3
    local output=$4
    local revision report binary_revision compiler_family compiler_version
    local qt_version cxx_standard raw_hash oauth_configured temporary

    revision=$(verified_source_revision "$source_root") || return
    report=$(build_provenance_report "$binary") || return
    binary_revision=$(printf '%s\n' "$report" | sed -n 's/^source_revision=//p')
    if [ "$binary_revision" != "$revision" ]; then
        echo "The GoldenCheetah binary was not built from the source HEAD." >&2
        return 1
    fi
    compiler_family=$(printf '%s\n' "$report" | sed -n 's/^compiler_family=//p')
    compiler_version=$(printf '%s\n' "$report" | sed -n 's/^compiler_version=//p')
    qt_version=$(printf '%s\n' "$report" | sed -n 's/^qt_version=//p')
    cxx_standard=$(printf '%s\n' "$report" | sed -n 's/^cxx_standard=//p')
    raw_hash=$(sha256sum "$binary" | cut -d ' ' -f 1) || return
    case "$oauth_status" in
    "Strava OAuth: configured") oauth_configured=true ;;
    "Strava OAuth: unavailable (credentials not configured)")
        oauth_configured=false ;;
    *)
        echo "Cannot encode an unknown Strava OAuth status." >&2
        return 1
        ;;
    esac

    mkdir -p "$(dirname "$output")" || return
    temporary=$(mktemp "${output}.tmp.XXXXXX") || return
    if ! (
        umask 077
        printf '%s\n' \
            "goldencheetah_appimage_manifest=1" \
            "source_revision=$revision" \
            "raw_elf_sha256=$raw_hash" \
            "toolchain=${compiler_family}-${compiler_version}_qt-${qt_version}_cxx-${cxx_standard}" \
            "strava_oauth_configured=$oauth_configured" >"$temporary"
        chmod 0600 "$temporary"
    ) || ! validate_appimage_base_manifest "$temporary"; then
        rm -f -- "$temporary"
        return 1
    fi
    mv -f -- "$temporary" "$output"
}

install_appimage_build_manifest()
{
    local manifest=$1
    local appdir=$2
    local target="$appdir/usr/share/goldencheetah/build-manifest"

    validate_appimage_base_manifest "$manifest" || {
        echo "Cannot install an invalid AppImage build manifest." >&2
        return 1
    }
    install -D -m 0644 "$manifest" "$target"
    cmp -s -- "$manifest" "$target"
}

finalize_appimage_manifest()
{
    local image=$1
    local base_manifest=$2
    local sidecar=$3
    local image_hash temporary

    [ -f "$image" ] && [ -x "$image" ] || {
        echo "Cannot finalize a missing AppImage." >&2
        return 1
    }
    validate_appimage_base_manifest "$base_manifest" || {
        echo "Cannot finalize an invalid AppImage build manifest." >&2
        return 1
    }
    image_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    mkdir -p "$(dirname "$sidecar")" || return
    temporary=$(mktemp "${sidecar}.tmp.XXXXXX") || return
    if ! (
        umask 077
        cat "$base_manifest" >"$temporary"
        printf 'appimage_sha256=%s\n' "$image_hash" >>"$temporary"
        chmod 0600 "$temporary"
    ); then
        rm -f -- "$temporary"
        return 1
    fi
    mv -f -- "$temporary" "$sidecar"
}

verify_appimage_manifest()
(
    local image=$1
    local sidecar=$2
    local image_path extract_dir embedded expected_base expected_hash actual_hash
    local -a lines=()

    cleanup_manifest_extract()
    {
        if [ -n "${extract_dir:-}" ] && [ -d "$extract_dir" ]; then
            rm -rf -- "$extract_dir"
        fi
    }
    trap cleanup_manifest_extract EXIT

    [ -f "$image" ] && [ -x "$image" ] || return 1
    [ -f "$sidecar" ] && [ ! -L "$sidecar" ] || return 1
    [ "$(stat -c '%a' "$sidecar")" = 600 ] || return 1
    [ "$(wc -c <"$sidecar")" -le 4352 ] || return 1
    [ "$(tail -c 1 "$sidecar" | od -An -tu1 | tr -d ' \n')" = 10 ] ||
        return 1
    mapfile -t lines <"$sidecar"
    [ "${#lines[@]}" -eq 6 ] || return 1
    expected_hash=${lines[5]#appimage_sha256=}
    [[ "${lines[5]}" =~ ^appimage_sha256=[0-9a-f]{64}$ ]] || return 1
    actual_hash=$(sha256sum "$image" | cut -d ' ' -f 1) || return
    [ "$actual_hash" = "$expected_hash" ] || return 1

    extract_dir=$(mktemp -d) || return
    expected_base="$extract_dir/expected-base"
    (umask 077; printf '%s\n' "${lines[@]:0:5}" >"$expected_base") ||
        return
    validate_appimage_base_manifest "$expected_base" || return

    image_path=$(readlink -f -- "$image") || return
    if [ "${GC_TEST_APPIMAGE_MANIFEST_ENTRYPOINT:-false}" != true ]; then
        local elf_magic appimage_magic
        elf_magic=$(dd if="$image_path" bs=1 count=4 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        appimage_magic=$(dd if="$image_path" bs=1 skip=8 count=3 \
            2>/dev/null | od -An -tx1 | tr -d ' \n')
        [ "$elf_magic" = 7f454c46 ] && [ "$appimage_magic" = 414902 ] ||
            return 1
    fi
    if ! (
        cd "$extract_dir"
        ulimit -c 0
        LC_ALL=C env -u LD_LIBRARY_PATH -u LD_PRELOAD \
            -u APPDIR -u APPIMAGE -u OWD \
            "$image_path" --appimage-extract >/dev/null
    ); then
        return 1
    fi
    embedded="$extract_dir/squashfs-root/usr/share/goldencheetah/build-manifest"
    if [ ! -f "$embedded" ] || [ -L "$embedded" ] ||
       ! validate_appimage_base_manifest "$embedded" ||
       ! cmp -s -- "$expected_base" "$embedded"; then
        return 1
    fi
)

promote_appimage_release()
(
    if [ "$#" -ne 3 ]; then
        echo "Usage: promote_appimage_release IMAGE MANIFEST RELEASE_LINK" >&2
        return 2
    fi

    local image=$1
    local manifest=$2
    local release_link=$3
    local release_parent release_name store artifacts sets
    local image_hash revision artifact_id artifact_dir artifact_temp=
    local current_dir= current_hash= previous_image previous_manifest
    local set_id set_dir set_temp= pointer_temp=
    local old_pointer=

    cleanup_appimage_promotion()
    {
        [ -z "$artifact_temp" ] || rm -rf -- "$artifact_temp"
        [ -z "$set_temp" ] || rm -rf -- "$set_temp"
        [ -z "$pointer_temp" ] || rm -f -- "$pointer_temp"
    }
    trap cleanup_appimage_promotion EXIT

    verify_appimage_manifest "$image" "$manifest" || {
        echo "Refusing to promote an unverified AppImage." >&2
        return 1
    }
    image=$(readlink -f -- "$image") || return
    manifest=$(readlink -f -- "$manifest") || return
    image_hash=$(sed -n 's/^appimage_sha256=//p' "$manifest")
    revision=$(sed -n 's/^source_revision=//p' "$manifest")
    [[ "$image_hash" =~ ^[0-9a-f]{64}$ ]] || return 1
    [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || return 1

    release_parent=$(dirname -- "$release_link")
    release_name=$(basename -- "$release_link")
    [ "$release_name" != . ] && [ "$release_name" != .. ] &&
        [ -n "$release_name" ] || return 1
    mkdir -p -- "$release_parent" || return
    release_parent=$(cd "$release_parent" && pwd -P) || return
    release_link="$release_parent/$release_name"
    if [ -e "$release_link" ] && [ ! -L "$release_link" ]; then
        echo "The release pointer exists and is not a symlink." >&2
        return 1
    fi

    store="$release_parent/.${release_name}.store"
    if [ -e "$store" ]; then
        [ -d "$store" ] && [ ! -L "$store" ] || return 1
    else
        (umask 077; mkdir -- "$store") || return
    fi
    [ "$(stat -c '%u' "$store")" = "$(id -u)" ] || return 1
    if [ $((8#$(stat -c '%a' "$store") & 8#022)) -ne 0 ]; then
        echo "The AppImage release store is writable by another user." >&2
        return 1
    fi
    artifacts="$store/artifacts"
    sets="$store/sets"
    mkdir -m 0700 -p -- "$artifacts" "$sets" || return
    exec 9>"$store/promotion.lock" || return
    flock -x 9 || return

    artifact_id="${revision}-${image_hash}"
    artifact_dir="$artifacts/$artifact_id"
    if [ ! -e "$artifact_dir" ]; then
        artifact_temp=$(mktemp -d "$artifacts/.tmp.XXXXXX") || return
        install -m 0755 "$image" "$artifact_temp/GoldenCheetah.AppImage" ||
            return
        install -m 0600 "$manifest" \
            "$artifact_temp/GoldenCheetah.AppImage.manifest" || return
        verify_appimage_manifest \
            "$artifact_temp/GoldenCheetah.AppImage" \
            "$artifact_temp/GoldenCheetah.AppImage.manifest" || return
        mv -- "$artifact_temp" "$artifact_dir" || return
        artifact_temp=
    fi
    [ -d "$artifact_dir" ] && [ ! -L "$artifact_dir" ] || return 1
    verify_appimage_manifest \
        "$artifact_dir/GoldenCheetah.AppImage" \
        "$artifact_dir/GoldenCheetah.AppImage.manifest" || return
    cmp -s -- "$image" "$artifact_dir/GoldenCheetah.AppImage" || return 1
    cmp -s -- "$manifest" \
        "$artifact_dir/GoldenCheetah.AppImage.manifest" || return 1

    previous_image="$artifact_dir/GoldenCheetah.AppImage"
    previous_manifest="$artifact_dir/GoldenCheetah.AppImage.manifest"
    current_hash=$image_hash
    if [ -L "$release_link" ]; then
        old_pointer=$(readlink -- "$release_link") || return
        current_dir=$(readlink -f -- "$release_link") || return
        [ "$(dirname -- "$current_dir")" = "$sets" ] &&
            [ -d "$current_dir" ] && [ ! -L "$current_dir" ] || return 1
        verify_appimage_manifest \
            "$current_dir/latest.AppImage" \
            "$current_dir/latest.AppImage.manifest" || return
        current_hash=$(sed -n 's/^appimage_sha256=//p' \
            "$current_dir/latest.AppImage.manifest")
        [[ "$current_hash" =~ ^[0-9a-f]{64}$ ]] || return 1
        if [ "$current_hash" = "$image_hash" ]; then
            cmp -s -- "$image" "$current_dir/latest.AppImage" || return 1
            cmp -s -- "$manifest" \
                "$current_dir/latest.AppImage.manifest" || return 1
            printf '%s\n' "$release_link/latest.AppImage"
            return 0
        fi
        previous_image=$(readlink -f -- "$current_dir/latest.AppImage") ||
            return
        previous_manifest="$current_dir/latest.AppImage.manifest"
        case "$previous_image" in
        "$artifacts"/*/GoldenCheetah.AppImage) ;;
        *) return 1 ;;
        esac
    fi

    set_id="${image_hash}-${current_hash}"
    set_dir="$sets/$set_id"
    if [ ! -e "$set_dir" ]; then
        set_temp=$(mktemp -d "$sets/.tmp.XXXXXX") || return
        ln -s "$(realpath --relative-to="$set_temp" \
            "$artifact_dir/GoldenCheetah.AppImage")" \
            "$set_temp/latest.AppImage" || return
        install -m 0600 "$artifact_dir/GoldenCheetah.AppImage.manifest" \
            "$set_temp/latest.AppImage.manifest" || return
        ln -s "$(realpath --relative-to="$set_temp" "$previous_image")" \
            "$set_temp/previous.AppImage" || return
        install -m 0600 "$previous_manifest" \
            "$set_temp/previous.AppImage.manifest" || return
        verify_appimage_manifest \
            "$set_temp/latest.AppImage" \
            "$set_temp/latest.AppImage.manifest" || return
        verify_appimage_manifest \
            "$set_temp/previous.AppImage" \
            "$set_temp/previous.AppImage.manifest" || return
        mv -- "$set_temp" "$set_dir" || return
        set_temp=
    fi
    [ -d "$set_dir" ] && [ ! -L "$set_dir" ] || return 1
    verify_appimage_manifest \
        "$set_dir/latest.AppImage" "$set_dir/latest.AppImage.manifest" ||
        return
    verify_appimage_manifest \
        "$set_dir/previous.AppImage" "$set_dir/previous.AppImage.manifest" ||
        return
    [ "$(sed -n 's/^appimage_sha256=//p' \
        "$set_dir/latest.AppImage.manifest")" = "$image_hash" ] || return 1
    [ "$(sed -n 's/^appimage_sha256=//p' \
        "$set_dir/previous.AppImage.manifest")" = "$current_hash" ] ||
        return 1

    sync -f "$store" || return
    pointer_temp="$release_parent/.${release_name}.tmp.${BASHPID}.${RANDOM}"
    ln -s ".${release_name}.store/sets/$set_id" "$pointer_temp" || return
    mv -Tf -- "$pointer_temp" "$release_link" || return
    pointer_temp=
    if ! sync -f "$release_parent" ||
       ! verify_appimage_manifest \
            "$release_link/latest.AppImage" \
            "$release_link/latest.AppImage.manifest" ||
       ! verify_appimage_manifest \
            "$release_link/previous.AppImage" \
            "$release_link/previous.AppImage.manifest"; then
        echo "AppImage promotion failed after pointer publication; rolling back." >&2
        if [ -n "$old_pointer" ]; then
            pointer_temp="$release_parent/.${release_name}.rollback.${BASHPID}.${RANDOM}"
            ln -s "$old_pointer" "$pointer_temp" &&
                mv -Tf -- "$pointer_temp" "$release_link" || return 1
            pointer_temp=
        else
            rm -f -- "$release_link" || return
        fi
        sync -f "$release_parent" || return
        return 1
    fi
    printf '%s\n' "$release_link/latest.AppImage"
)

write_source_revision()
{
    local output=$1
    local revision=${GC_SOURCE_REVISION:-}

    if [ -n "$revision" ]; then
        if [[ ! "$revision" =~ ^[0-9a-f]{40}$ ]]; then
            echo "GC_SOURCE_REVISION must be a full lowercase Git commit hash" >&2
            return 1
        fi
        printf 'commit %s\n' "$revision" >>"$output"
    elif git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        git log -1 >>"$output"
    else
        echo "Set GC_SOURCE_REVISION when packaging exported source" >&2
        return 1
    fi
}

install_embedded_python()
{
    local requirements=$1
    local appdir=$2

    download_file "$PYTHON_APPIMAGE_URL" "$PYTHON_APPIMAGE_FILE"
    printf '%s  %s\n' "$PYTHON_APPIMAGE_SHA256" "$PYTHON_APPIMAGE_FILE" |
        sha256sum --check -
    chmod +x "$PYTHON_APPIMAGE_FILE"

    rm -rf squashfs-root
    "./$PYTHON_APPIMAGE_FILE" --appimage-extract
    rm -f "$PYTHON_APPIMAGE_FILE"

    export PATH="$(pwd)/squashfs-root/usr/bin:$PATH"
    pip install --upgrade pip
    pip install -q -r "$requirements"

    mv squashfs-root/usr "$appdir/usr"
    mv squashfs-root/opt "$appdir/opt"
    rm -rf squashfs-root
}
