#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../../.." && pwd)
SUPPORT="$REPO_ROOT/src/Resources/linux/AppImagePackagingSupport.sh"
LOCAL_PACKAGER="$REPO_ROOT/src/Resources/linux/MakeAppImageQt6.sh"
CI_PACKAGER="$REPO_ROOT/appveyor/linux/after_build.sh"
DEV_PACKAGER="$REPO_ROOT/.devcontainer/package-appimage.sh"
APPVEYOR_INSTALL="$REPO_ROOT/appveyor/linux/install.sh"
REQUIREMENTS="$REPO_ROOT/src/Python/requirements.txt"
DEV_CONFIG="$REPO_ROOT/.devcontainer/gcconfig.pri"
MAIN_SOURCE="$REPO_ROOT/src/Core/main.cpp"
LIBSECRET_SOURCE="$REPO_ROOT/contrib/qtkeychain/qtkeychain/libsecret.cpp"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains()
{
    local file=$1
    local pattern=$2
    grep -Fq -- "$pattern" "$file" ||
        fail "$file does not contain: $pattern"
}

[ -r "$SUPPORT" ] || fail "missing shared AppImage packaging support"

# shellcheck source=/dev/null
. "$SUPPORT"

[ "$PYTHON_APPIMAGE_SERIES" = "3.11" ] ||
    fail "embedded Python must remain on the supported 3.11 series"
[ "$PYTHON_APPIMAGE_VERSION" = "3.11.15" ] ||
    fail "unexpected embedded Python patch release"
[ "$PYTHON_APPIMAGE_ABI" = "cp311" ] ||
    fail "embedded Python ABI does not match Python 3.11"
[ "$PYTHON_APPIMAGE_PLATFORM" = "manylinux2014_x86_64" ] ||
    fail "embedded Python must retain the glibc-compatible manylinux2014 build"
[ "$PYTHON_APPIMAGE_FILE" = \
  "python3.11.15-cp311-cp311-manylinux2014_x86_64.AppImage" ] ||
    fail "embedded Python filename is inconsistent"
[ "$PYTHON_APPIMAGE_SHA256" = \
  "2d8ecd8002fae06813d4c92ba5244f573aae9bf84eaf41a1b189b623112e3dec" ] ||
    fail "embedded Python SHA-256 is not the reviewed release digest"
[ "$PYTHON_APPIMAGE_URL" = \
  "https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1/$PYTHON_APPIMAGE_FILE" ] ||
    fail "embedded Python URL is not the project-controlled immutable asset"
[ "${QTKEYCHAIN_LICENSE_SHA256:-}" = \
  "ca46b73d5159548ab52834db51f195aa3d1f277f020e9dca92f4beb21b468a50" ] ||
    fail "QtKeychain license digest is not the reviewed content"
[ "${LGPL21_LICENSE_SHA256:-}" = \
  "dc626520dcd53a22f727af3ee42c770e56c97a64fe3adb063799d8ab032fe551" ] ||
    fail "LGPL-2.1 digest is not the reviewed content"

declare -F download_file >/dev/null || fail "download_file helper is missing"
declare -F run_packaging_appimage >/dev/null ||
    fail "run_packaging_appimage helper is missing"
declare -F run_packaged_appimage_smoke >/dev/null ||
    fail "run_packaged_appimage_smoke helper is missing"
declare -F install_qt_offscreen_plugin >/dev/null ||
    fail "install_qt_offscreen_plugin helper is missing"
declare -F require_qt_offscreen_appimage >/dev/null ||
    fail "require_qt_offscreen_appimage helper is missing"
declare -F install_embedded_python >/dev/null ||
    fail "install_embedded_python helper is missing"
declare -F write_source_revision >/dev/null ||
    fail "write_source_revision helper is missing"
declare -F strava_oauth_build_status >/dev/null ||
    fail "strava_oauth_build_status helper is missing"
declare -F require_strava_oauth_build >/dev/null ||
    fail "require_strava_oauth_build helper is missing"
declare -F strava_oauth_appimage_status >/dev/null ||
    fail "strava_oauth_appimage_status helper is missing"
declare -F require_strava_oauth_appimage >/dev/null ||
    fail "require_strava_oauth_appimage helper is missing"
declare -F install_linux_keychain_runtime >/dev/null ||
    fail "install_linux_keychain_runtime helper is missing"
declare -F linux_keychain_runtime_status >/dev/null ||
    fail "linux_keychain_runtime_status helper is missing"
declare -F linux_keychain_appimage_status >/dev/null ||
    fail "linux_keychain_appimage_status helper is missing"
declare -F require_linux_keychain_appimage >/dev/null ||
    fail "require_linux_keychain_appimage helper is missing"
declare -F linux_keychain_entrypoint_status >/dev/null ||
    fail "linux_keychain_entrypoint_status helper is missing"
declare -F create_linux_keychain_deploy_probe >/dev/null ||
    fail "create_linux_keychain_deploy_probe helper is missing"
declare -F remove_linux_keychain_deploy_probe >/dev/null ||
    fail "remove_linux_keychain_deploy_probe helper is missing"
declare -F run_linuxdeployqt_with_keychain_probe >/dev/null ||
    fail "run_linuxdeployqt_with_keychain_probe helper is missing"

TEMP_DIR=$(mktemp -d)
trap 'rm -rf "$TEMP_DIR"' EXIT
printf '#!/bin/sh\nprintf "%%s" "$APPIMAGE_EXTRACT_AND_RUN"\n' \
    >"$TEMP_DIR/check-extract-mode"
chmod +x "$TEMP_DIR/check-extract-mode"
[ "$(run_packaged_appimage_smoke 2s \
      "$TEMP_DIR/check-extract-mode")" = "1" ] ||
    fail "packaged AppImage smoke did not use extraction mode"

mkdir -p \
    "$TEMP_DIR/qt-plugins/platforms" \
    "$TEMP_DIR/offscreen-appdir/plugins/platforms"
printf 'offscreen fixture\n' \
    >"$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so"
cat >"$TEMP_DIR/fake-qmake" <<EOF
#!/bin/sh
test "\$1" = "-query" &&
    test "\$2" = "QT_INSTALL_PLUGINS" || exit 64
printf '%s\\n' "$TEMP_DIR/qt-plugins"
EOF
chmod +x "$TEMP_DIR/fake-qmake"
install_qt_offscreen_plugin \
    "$TEMP_DIR/fake-qmake" "$TEMP_DIR/offscreen-appdir"
cmp \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so" \
    "$TEMP_DIR/offscreen-appdir/plugins/platforms/libqoffscreen.so" ||
    fail "offscreen plugin was not copied from qmake's plugin directory"

mv \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so" \
    "$TEMP_DIR/qt-plugins/platforms/libqoffscreen.so.missing"
if install_qt_offscreen_plugin \
    "$TEMP_DIR/fake-qmake" "$TEMP_DIR/offscreen-appdir" \
    >/dev/null 2>&1; then
    fail "missing Qt offscreen plugin was accepted"
fi

cat >"$TEMP_DIR/offscreen-smoke" <<'EOF'
#!/bin/sh
test "$APPIMAGE_EXTRACT_AND_RUN" = "1" || exit 65
test "$QT_QPA_PLATFORM" = "offscreen" || exit 66
test "$QT_OPENGL" = "software" || exit 67
sleep 5
EOF
chmod +x "$TEMP_DIR/offscreen-smoke"
[ "$(require_qt_offscreen_appimage \
      "$TEMP_DIR/offscreen-smoke" 0.1s)" = \
  "Qt offscreen runtime: available" ] ||
    fail "offscreen AppImage smoke did not accept a running image"
printf '#!/bin/sh\nexit 127\n' >"$TEMP_DIR/offscreen-failure"
chmod +x "$TEMP_DIR/offscreen-failure"
if require_qt_offscreen_appimage \
    "$TEMP_DIR/offscreen-failure" 0.1s \
    >/dev/null 2>&1; then
    fail "offscreen AppImage smoke accepted an initialization failure"
fi

REVISION=0123456789abcdef0123456789abcdef01234567
GC_SOURCE_REVISION=$REVISION write_source_revision "$TEMP_DIR/revision"
grep -Fxq "commit $REVISION" "$TEMP_DIR/revision" ||
    fail "explicit source revision was not recorded"
if GC_SOURCE_REVISION=invalid write_source_revision \
    "$TEMP_DIR/invalid" 2>/dev/null; then
    fail "invalid source revision was accepted"
fi
if (cd "$TEMP_DIR" && unset GC_SOURCE_REVISION &&
    write_source_revision missing 2>/dev/null); then
    fail "exported source without a revision was accepted"
fi

cat >"$TEMP_DIR/status-probe.c" <<'EOF'
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
#ifdef GC_STATUS_MODE
    const char *mode = GC_STATUS_MODE;
#else
    const char *mode = argv[0];
#endif

    if (argc != 2) {
        return 64;
    }
    if (strcmp(
            argv[1],
            "--goldencheetah-linux-keychain-status") == 0) {
        fputs(
            "goldencheetah_linux_keychain_status=1\n"
            "application=GoldenCheetah\n",
            stdout);
        if (strstr(mode, "keychain-disabled") != NULL) {
            fputs(
                "libsecret_compile_support=disabled\n"
                "libsecret_runtime=unavailable\n",
                stdout);
        } else if (strstr(
                       mode,
                       "keychain-unavailable") != NULL) {
            fputs(
                "libsecret_compile_support=enabled\n"
                "libsecret_runtime=unavailable\n",
                stdout);
        } else {
            fputs(
                "libsecret_compile_support=enabled\n"
                "libsecret_runtime=available\n",
                stdout);
        }
        return 0;
    }
    if (strcmp(
            argv[1],
            "--goldencheetah-build-status") != 0) {
        return 64;
    }

    fputs(
        "goldencheetah_build_status=1\n"
        "application=GoldenCheetah\n",
        stdout);
    if (strstr(mode, "no-strava") == NULL) {
        fputs("strava_support=enabled\n", stdout);
    }
    if (strstr(mode, "oversized") != NULL) {
        for (int index = 0; index < 10000; ++index) {
            fputc('x', stdout);
        }
        return 0;
    }
    if (strstr(mode, "missing-newline") != NULL) {
        fputs(
            "strava_oauth=runtime_credentials\n"
            "strava_compile_fallback=configured",
            stdout);
        return 0;
    }
    if (strstr(mode, "extra-newline") != NULL) {
        fputs(
            "strava_oauth=runtime_credentials\n"
            "strava_compile_fallback=configured\n\n",
            stdout);
        return 0;
    }
    if (strstr(mode, "malformed") != NULL) {
        fputs("strava_oauth=maybe\n", stdout);
    } else {
        fputs("strava_oauth=runtime_credentials\n", stdout);
        fputs(
            strstr(mode, "unconfigured") != NULL
                ? "strava_compile_fallback=unavailable\n"
                : "strava_compile_fallback=configured\n",
            stdout);
    }
    return strstr(mode, "bad-exit") == NULL ? 0 : 1;
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    "$TEMP_DIR/status-probe.c" -o "$TEMP_DIR/configured"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"configured"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/configured-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"unconfigured"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/unconfigured-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"malformed"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/malformed-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"keychain-disabled"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/keychain-disabled-entry"
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror \
    -DGC_STATUS_MODE='"keychain-unavailable"' \
    "$TEMP_DIR/status-probe.c" \
    -o "$TEMP_DIR/keychain-unavailable-entry"
chmod +x "$TEMP_DIR/status-probe.c"
cp "$TEMP_DIR/configured" "$TEMP_DIR/unconfigured"
cp "$TEMP_DIR/configured" "$TEMP_DIR/malformed"
cp "$TEMP_DIR/configured" "$TEMP_DIR/no-strava"
cp "$TEMP_DIR/configured" "$TEMP_DIR/bad-exit"
cp "$TEMP_DIR/configured" "$TEMP_DIR/oversized"
cp "$TEMP_DIR/configured" "$TEMP_DIR/missing-newline"
cp "$TEMP_DIR/configured" "$TEMP_DIR/extra-newline"
cp "$TEMP_DIR/configured" "$TEMP_DIR/GoldenCheetah"
cat >"$TEMP_DIR/app-run-wrapper" <<'EOF'
#!/bin/sh
: "${APPDIR:?}" "${APPIMAGE:?}" "${OWD:?}"
[ -d "$APPDIR" ] && [ -f "$APPIMAGE" ] && [ -d "$OWD" ] || exit 65
exec "$APPDIR/GoldenCheetah" "$@"
EOF
chmod +x "$TEMP_DIR/app-run-wrapper"
mkdir "$TEMP_DIR/host-loader-override"
printf 'invalid host override\n' \
    >"$TEMP_DIR/host-loader-override/libc.so.6"
[ "$(LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
      strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "Strava status inherited an external library override"
[ "$(LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
      linux_keychain_entrypoint_status "$TEMP_DIR/configured-entry")" = \
  "Linux keychain runtime: available" ] ||
    fail "keychain status inherited an external library override"

DEPLOY_PROBE=$(create_linux_keychain_deploy_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR")
[ -x "$DEPLOY_PROBE" ] ||
    fail "Linux keychain deploy probe was not created"
LC_ALL=C readelf -d "$DEPLOY_PROBE" |
    grep -Eq '\(NEEDED\).*\[libsecret-1\.so\.0\]' ||
    fail "Linux keychain deploy probe does not require libsecret"
remove_linux_keychain_deploy_probe "$DEPLOY_PROBE"
[ ! -e "$DEPLOY_PROBE" ] ||
    fail "Linux keychain deploy probe was not removed"

run_packaging_appimage()
{
    local argument probe=
    for argument in "$@"; do
        case "$argument" in
        -executable=*) probe=${argument#-executable=} ;;
        esac
    done
    [ -f "$probe" ]
}
run_linuxdeployqt_with_keychain_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR" fake-linuxdeployqt ||
    fail "successful deployment with a temporary probe was rejected"
[ ! -e "$TEMP_DIR/.goldencheetah-libsecret-deploy-probe" ] ||
    fail "successful deployment left its temporary probe behind"

run_packaging_appimage()
{
    local argument probe=
    for argument in "$@"; do
        case "$argument" in
        -executable=*) probe=${argument#-executable=} ;;
        esac
    done
    rm -f -- "$probe"
    mkdir -- "$probe"
}
if run_linuxdeployqt_with_keychain_probe \
    "$TEMP_DIR/configured" "$TEMP_DIR" fake-linuxdeployqt \
    >/dev/null 2>&1; then
    fail "deployment accepted a probe cleanup failure"
fi
rm -rf "$TEMP_DIR/.goldencheetah-libsecret-deploy-probe"

mkdir -p "$TEMP_DIR/libsecret/lib" "$TEMP_DIR/libsecret/bin"

build_fixture_dependency()
{
    local soname=$1
    local symbol=$2
    local source="$TEMP_DIR/libsecret/${soname}.c"

    printf 'void %s(void) {}\n' "$symbol" >"$source"
    "${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
        -Wl,-soname,"$soname" \
        "$source" -o "$TEMP_DIR/libsecret/lib/$soname"
}

build_fixture_dependency \
    libglib-2.0.so.0 gc_glib_dependency
build_fixture_dependency \
    libgio-2.0.so.0 gc_gio_dependency
build_fixture_dependency \
    libgobject-2.0.so.0 gc_gobject_dependency
build_fixture_dependency \
    libgpg-error.so.0 gc_gpg_error_dependency
build_fixture_dependency \
    libgc-absolute.so.1 gc_absolute_dependency
build_fixture_dependency \
    libgc-linked.so.1 gc_linked_dependency
cat >"$TEMP_DIR/libsecret/libgcrypt-fixture.c" <<'EOF'
extern void gc_gpg_error_dependency(void);
void gc_gcrypt_dependency(void)
{
    gc_gpg_error_dependency();
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libgcrypt.so.20 \
    "$TEMP_DIR/libsecret/libgcrypt-fixture.c" \
    "$TEMP_DIR/libsecret/lib/libgpg-error.so.0" \
    -o "$TEMP_DIR/libsecret/lib/libgcrypt.so.20"

cat >"$TEMP_DIR/libsecret/libsecret-fixture.c" <<'EOF'
#define LIBSECRET_SYMBOL(name) void name(void) {}
LIBSECRET_SYMBOL(secret_password_lookup)
LIBSECRET_SYMBOL(secret_password_lookup_finish)
LIBSECRET_SYMBOL(secret_password_store)
LIBSECRET_SYMBOL(secret_password_store_finish)
LIBSECRET_SYMBOL(secret_password_clear)
LIBSECRET_SYMBOL(secret_password_clear_finish)
LIBSECRET_SYMBOL(secret_password_free)
LIBSECRET_SYMBOL(secret_error_get_quark)
extern void gc_glib_dependency(void);
extern void gc_gio_dependency(void);
extern void gc_gobject_dependency(void);
extern void gc_gcrypt_dependency(void);
void gc_use_dependencies(void)
{
    gc_glib_dependency();
    gc_gio_dependency();
    gc_gobject_dependency();
    gc_gcrypt_dependency();
}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/libsecret-fixture.c" \
    "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    -o "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0"
cat >"$TEMP_DIR/libsecret/incomplete-fixture.c" <<'EOF'
void secret_password_lookup(void) {}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/incomplete-fixture.c" \
    -o "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0"
patchelf --set-rpath '$ORIGIN' \
    "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0"
cat >"$TEMP_DIR/libsecret/undefined-fixture.c" <<'EOF'
#define LIBSECRET_REFERENCE(name) \
    extern void name(void); \
    void *name##_reference = (void *)&name
LIBSECRET_REFERENCE(secret_password_lookup);
LIBSECRET_REFERENCE(secret_password_lookup_finish);
LIBSECRET_REFERENCE(secret_password_store);
LIBSECRET_REFERENCE(secret_password_store_finish);
LIBSECRET_REFERENCE(secret_password_clear);
LIBSECRET_REFERENCE(secret_password_clear_finish);
LIBSECRET_REFERENCE(secret_password_free);
LIBSECRET_REFERENCE(secret_error_get_quark);
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/undefined-fixture.c" \
    -o "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0"
patchelf --set-rpath '$ORIGIN' \
    "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0"
ln -s libsecret-1.so.0.0 \
    "$TEMP_DIR/libsecret/lib/libsecret-1.so.0"
cat >"$TEMP_DIR/libsecret/bin/pkg-config" <<EOF
#!/bin/sh
[ "\$1" = "--variable=libdir" ] || exit 1
case "\$2" in
libsecret-1|gpg-error) ;;
*) exit 1 ;;
esac
printf '%s\n' "$TEMP_DIR/libsecret/lib"
EOF
chmod +x "$TEMP_DIR/libsecret/bin/pkg-config"
cat >"$TEMP_DIR/libsecret/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: libsecret
Files: *
Copyright: 2009-2024 The libsecret authors
License: LGPL-2.1+
 GNU Lesser General Public
 License as published by the Free Software Foundation
EOF
QTKEYCHAIN_LICENSE_FIXTURE="$REPO_ROOT/contrib/qtkeychain/COPYING"
LGPL21_LICENSE_FIXTURE="/usr/share/common-licenses/LGPL-2.1"
[ -r "$QTKEYCHAIN_LICENSE_FIXTURE" ] ||
    fail "reviewed QtKeychain license fixture is missing"
[ -r "$LGPL21_LICENSE_FIXTURE" ] ||
    fail "reviewed LGPL-2.1 license fixture is missing"

KEYCHAIN_APPDIR="$TEMP_DIR/keychain.AppDir"
mkdir -p "$KEYCHAIN_APPDIR/lib"
cp "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    "$KEYCHAIN_APPDIR/lib/"
PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$KEYCHAIN_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE"
[ "$(linux_keychain_runtime_status "$KEYCHAIN_APPDIR")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "installed Linux keychain runtime was not reported"
cmp "$TEMP_DIR/libsecret/lib/libgpg-error.so.0" \
    "$KEYCHAIN_APPDIR/lib/libgpg-error.so.0" ||
    fail "transitive libgpg-error runtime was not installed"
cmp "$TEMP_DIR/libsecret/copyright" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright" ||
    fail "libsecret copyright was not copied exactly"
cmp "$QTKEYCHAIN_LICENSE_FIXTURE" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING" ||
    fail "QtKeychain license was not copied exactly"
cmp "$LGPL21_LICENSE_FIXTURE" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1" ||
    fail "complete LGPL-2.1 license was not copied exactly"
LC_ALL=C readelf -d "$KEYCHAIN_APPDIR/lib/libsecret-1.so.0" |
    grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]' ||
    fail "bundled libsecret does not resolve dependencies from its directory"
LC_ALL=C readelf -d "$KEYCHAIN_APPDIR/lib/libgcrypt.so.20" |
    grep -Eq '\((RPATH|RUNPATH)\).*\[\$ORIGIN\]' ||
    fail "bundled libgcrypt does not resolve dependencies from its directory"
KEYCHAIN_LDD_OUTPUT=$(
    env -u LD_LIBRARY_PATH LC_ALL=C \
        ldd "$KEYCHAIN_APPDIR/lib/libsecret-1.so.0"
)
for dependency in \
    libglib-2.0.so.0 \
    libgio-2.0.so.0 \
    libgobject-2.0.so.0 \
    libgcrypt.so.20 \
    libgpg-error.so.0; do
    printf '%s\n' "$KEYCHAIN_LDD_OUTPUT" |
        grep -Fq -- \
            "$dependency => $KEYCHAIN_APPDIR/lib/$dependency " ||
        fail "$dependency did not resolve from the AppDir"
done

LINKED_LIB_APPDIR="$TEMP_DIR/linked-lib-keychain.AppDir"
LINKED_LIB_OUTSIDE="$TEMP_DIR/linked-lib-outside"
mkdir -p "$LINKED_LIB_APPDIR" "$LINKED_LIB_OUTSIDE"
ln -s "$LINKED_LIB_OUTSIDE" "$LINKED_LIB_APPDIR/lib"
if PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$LINKED_LIB_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE" \
        >/dev/null 2>&1; then
    fail "installer accepted a linked AppDir library directory"
fi
[ ! -e "$LINKED_LIB_OUTSIDE/libsecret-1.so.0" ] ||
    fail "installer wrote libsecret outside a linked AppDir"

LINKED_LICENSE_APPDIR="$TEMP_DIR/linked-license-keychain.AppDir"
LINKED_LICENSE_OUTSIDE="$TEMP_DIR/linked-license-outside"
mkdir -p \
    "$LINKED_LICENSE_APPDIR/lib" \
    "$LINKED_LICENSE_APPDIR/usr/share/doc/GoldenCheetah" \
    "$LINKED_LICENSE_OUTSIDE"
cp "$TEMP_DIR/libsecret/lib/libglib-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgio-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgobject-2.0.so.0" \
    "$TEMP_DIR/libsecret/lib/libgcrypt.so.20" \
    "$LINKED_LICENSE_APPDIR/lib/"
ln -s "$LINKED_LICENSE_OUTSIDE" \
    "$LINKED_LICENSE_APPDIR/usr/share/doc/GoldenCheetah/licenses"
if PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$LGPL21_LICENSE_FIXTURE" \
    install_linux_keychain_runtime \
        "$LINKED_LICENSE_APPDIR" \
        "$QTKEYCHAIN_LICENSE_FIXTURE" \
        >/dev/null 2>&1; then
    fail "installer accepted a linked AppDir license directory"
fi
[ ! -e "$LINKED_LICENSE_OUTSIDE/libsecret-copyright" ] ||
    fail "installer wrote a copyright outside a linked AppDir"

INCOMPLETE_APPDIR="$TEMP_DIR/incomplete-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$INCOMPLETE_APPDIR"
cp "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0" \
    "$INCOMPLETE_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$INCOMPLETE_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime without required QtKeychain symbols was accepted"
fi

UNDEFINED_APPDIR="$TEMP_DIR/undefined-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$UNDEFINED_APPDIR"
cp "$TEMP_DIR/libsecret/undefined-libsecret-1.so.0" \
    "$UNDEFINED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$UNDEFINED_APPDIR" \
    >/dev/null 2>&1; then
    fail "undefined QtKeychain symbols were accepted as implementations"
fi

MISSING_DEPENDENCY_APPDIR="$TEMP_DIR/missing-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$MISSING_DEPENDENCY_APPDIR"
rm "$MISSING_DEPENDENCY_APPDIR/lib/libgcrypt.so.20"
if linux_keychain_runtime_status "$MISSING_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with a host-only dependency was accepted"
fi

MISSING_TRANSITIVE_APPDIR="$TEMP_DIR/missing-transitive-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$MISSING_TRANSITIVE_APPDIR"
rm "$MISSING_TRANSITIVE_APPDIR/lib/libgpg-error.so.0"
if linux_keychain_runtime_status "$MISSING_TRANSITIVE_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with host-only libgpg-error was accepted"
fi

UNRESOLVED_APPDIR="$TEMP_DIR/unresolved-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$UNRESOLVED_APPDIR"
patchelf --add-needed libgc-missing.so.1 \
    "$UNRESOLVED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$UNRESOLVED_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with an unresolved dependency was accepted"
fi

HOST_DEPENDENCY_APPDIR="$TEMP_DIR/host-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$HOST_DEPENDENCY_APPDIR"
patchelf --add-needed libstdc++.so.6 \
    "$HOST_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$HOST_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime with an unexpected host dependency was accepted"
fi

ABSOLUTE_DEPENDENCY_APPDIR="$TEMP_DIR/absolute-dependency-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$ABSOLUTE_DEPENDENCY_APPDIR"
patchelf --add-needed \
    "$TEMP_DIR/libsecret/lib/libgc-absolute.so.1" \
    "$ABSOLUTE_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$ABSOLUTE_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "an absolute dependency outside the AppDir was accepted"
fi

LINKED_DEPENDENCY_APPDIR="$TEMP_DIR/linked-dependency-keychain.AppDir"
LINKED_DEPENDENCY_OUTSIDE="$TEMP_DIR/linked-dependency-outside"
cp -a "$KEYCHAIN_APPDIR" "$LINKED_DEPENDENCY_APPDIR"
mkdir "$LINKED_DEPENDENCY_OUTSIDE"
cp "$TEMP_DIR/libsecret/lib/libgc-linked.so.1" \
    "$LINKED_DEPENDENCY_OUTSIDE/libgc-linked.so.1"
ln -s "$LINKED_DEPENDENCY_OUTSIDE/libgc-linked.so.1" \
    "$LINKED_DEPENDENCY_APPDIR/lib/libgc-linked.so.1"
patchelf --add-needed libgc-linked.so.1 \
    "$LINKED_DEPENDENCY_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$LINKED_DEPENDENCY_APPDIR" \
    >/dev/null 2>&1; then
    fail "a linked dependency escaping the AppDir was accepted"
fi

ESCAPED_APPDIR="$TEMP_DIR/escaped-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$ESCAPED_APPDIR"
rm "$ESCAPED_APPDIR/lib/libsecret-1.so.0"
ln -s "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0" \
    "$ESCAPED_APPDIR/lib/libsecret-1.so.0"
if linux_keychain_runtime_status "$ESCAPED_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret symlink escaping the AppDir was accepted"
fi

rm "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status "$KEYCHAIN_APPDIR" \
    >/dev/null 2>&1; then
    fail "Linux keychain runtime without its copyright was accepted"
fi
cp "$TEMP_DIR/libsecret/copyright" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"

WRONG_LICENSE_APPDIR="$TEMP_DIR/wrong-license-keychain.AppDir"
cp -a "$KEYCHAIN_APPDIR" "$WRONG_LICENSE_APPDIR"
printf 'wrong\n' \
    >"$WRONG_LICENSE_APPDIR/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1"
if linux_keychain_runtime_status "$WRONG_LICENSE_APPDIR" \
    >/dev/null 2>&1; then
    fail "incorrect LGPL-2.1 license content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" "$TEMP_DIR/wrong-qt-license-keychain.AppDir"
printf 'wrong\n' \
    >"$TEMP_DIR/wrong-qt-license-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING"
if linux_keychain_runtime_status \
    "$TEMP_DIR/wrong-qt-license-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "incorrect QtKeychain license content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" "$TEMP_DIR/wrong-copyright-keychain.AppDir"
printf 'wrong\n' \
    >"$TEMP_DIR/wrong-copyright-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status \
    "$TEMP_DIR/wrong-copyright-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "incorrect libsecret copyright content was accepted"
fi
cp -a "$KEYCHAIN_APPDIR" \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir"
sed -i '/^Copyright:/d' \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status \
    "$TEMP_DIR/missing-copyright-line-keychain.AppDir" \
    >/dev/null 2>&1; then
    fail "libsecret copyright without a copyright notice was accepted"
fi

printf '\177ELF\002\001\001\000AI\001compressed-appimage-payload' \
    >"$TEMP_DIR/type1.AppImage"
printf '\177ELF\002\001\001\000AI\002compressed-appimage-payload' \
    >"$TEMP_DIR/type2.AppImage"
chmod +x "$TEMP_DIR/type1.AppImage" "$TEMP_DIR/type2.AppImage"

[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime-only Strava support was not reported"
[ "$(strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime Strava support with a fallback was not reported"
if strava_oauth_build_status "$TEMP_DIR/missing" >/dev/null 2>&1; then
    fail "missing executable was accepted for Strava status inspection"
fi
if strava_oauth_build_status /bin/true >/dev/null 2>&1; then
    fail "an unrelated ELF executable was accepted as GoldenCheetah"
fi
if strava_oauth_build_status "$TEMP_DIR/malformed" \
    >/dev/null 2>&1; then
    fail "a malformed build-status response was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/no-strava" \
    >/dev/null 2>&1; then
    fail "a binary without reported Strava support was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/bad-exit" \
    >/dev/null 2>&1; then
    fail "a failed build-status command was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/oversized" \
    >/dev/null 2>&1; then
    fail "an oversized build-status response was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/missing-newline" \
    >/dev/null 2>&1; then
    fail "a build-status response without its final newline was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/extra-newline" \
    >/dev/null 2>&1; then
    fail "a build-status response with trailing blank lines was accepted"
fi
if strava_oauth_build_status "$TEMP_DIR/status-probe.c" \
    >/dev/null 2>&1; then
    fail "a non-ELF file was accepted for Strava status inspection"
fi
if strava_oauth_build_status "$TEMP_DIR/type1.AppImage" \
    >/dev/null 2>&1; then
    fail "a Type 1 AppImage was inspected as a raw executable"
fi
if strava_oauth_build_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "a Type 2 AppImage was inspected as a raw executable"
fi
require_strava_oauth_build "$TEMP_DIR/configured" >/dev/null ||
    fail "runtime Strava support was rejected by the release gate"
require_strava_oauth_build "$TEMP_DIR/unconfigured" >/dev/null ||
    fail "runtime-only Strava support was rejected by the release gate"
if require_strava_oauth_build "$TEMP_DIR/missing" \
    >/dev/null 2>&1; then
    fail "release gate accepted a missing executable"
fi

GC_TEST_APPIMAGE_SIDECAR="$TEMP_DIR/configured"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
run_packaging_appimage()
{
    [ -z "${LD_LIBRARY_PATH:-}" ] ||
        fail "AppImage extraction inherited LD_LIBRARY_PATH"
    [ -z "${LD_PRELOAD:-}" ] ||
        fail "AppImage extraction inherited LD_PRELOAD"
    [ -z "${APPDIR:-}" ] ||
        fail "AppImage extraction inherited APPDIR"
    [ -z "${APPIMAGE:-}" ] ||
        fail "AppImage extraction inherited APPIMAGE"
    [ -z "${OWD:-}" ] ||
        fail "AppImage extraction inherited OWD"
    [ "$2" = "--appimage-extract" ] ||
        fail "AppImage status did not request extraction"
    mkdir -p squashfs-root
    cp "$GC_TEST_APPIMAGE_SIDECAR" \
        squashfs-root/GoldenCheetah
    cp "$GC_TEST_APPIMAGE_ENTRY" \
        "squashfs-root/$GC_TEST_APPIMAGE_ENTRY_NAME"
    ln -s "$GC_TEST_APPIMAGE_ENTRY_NAME" \
        squashfs-root/AppRun
    if [ -n "${GC_TEST_APPIMAGE_LIBSECRET:-}" ]; then
        mkdir -p \
            squashfs-root/lib \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses
        cp "$GC_TEST_APPIMAGE_LIBSECRET" \
            squashfs-root/lib/libsecret-1.so.0
        cp \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libglib-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgio-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgobject-2.0.so.0" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgcrypt.so.20" \
            "$GC_TEST_APPIMAGE_DEPENDENCY_DIR/libgpg-error.so.0" \
            squashfs-root/lib/
        cp "$GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright
        cp "$GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING
        cp "$GC_TEST_APPIMAGE_LIBSECRET_LICENSE" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1
    fi
}

[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime Strava support in the package was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "configured packaged GoldenCheetah was rejected"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/app-run-wrapper"
GC_TEST_APPIMAGE_ENTRY_NAME="app-run-wrapper"
WRAPPED_STRAVA_STATUS=$(
    APPDIR="$TEMP_DIR/stale-appdir" \
        APPIMAGE="$TEMP_DIR/stale.AppImage" \
        OWD="$TEMP_DIR/stale-owd" \
        LD_LIBRARY_PATH="$TEMP_DIR/host-loader-override" \
        LD_PRELOAD="$KEYCHAIN_APPDIR/lib/libglib-2.0.so.0" \
        strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage"
)
[ "$WRAPPED_STRAVA_STATUS" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "a valid shell AppRun wrapper was rejected for Strava status"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/unconfigured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="unconfigured-entry"
[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: runtime credentials supported" ] ||
    fail "runtime-only packaged GoldenCheetah was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "release gate rejected runtime-only Strava support"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/malformed-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="malformed-entry"
if strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "malformed packaged GoldenCheetah status was accepted"
fi
if strava_oauth_appimage_status "$TEMP_DIR/type1.AppImage" \
    >/dev/null 2>&1; then
    fail "unsupported Type 1 AppImage extraction was accepted"
fi

GC_TEST_APPIMAGE_LIBSECRET="$KEYCHAIN_APPDIR/lib/libsecret-1.so.0"
GC_TEST_APPIMAGE_DEPENDENCY_DIR="$KEYCHAIN_APPDIR/lib"
GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/libsecret/copyright"
GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE="$QTKEYCHAIN_LICENSE_FIXTURE"
GC_TEST_APPIMAGE_LIBSECRET_LICENSE="$LGPL21_LICENSE_FIXTURE"
[ "$(linux_keychain_entrypoint_status "$TEMP_DIR/configured-entry")" = \
  "Linux keychain runtime: available" ] ||
    fail "compiled and available Linux keychain entrypoint was not reported"
[ "$(APPDIR="$TEMP_DIR" \
      APPIMAGE="$TEMP_DIR/type2.AppImage" \
      OWD="$TEMP_DIR" \
      linux_keychain_entrypoint_status "$TEMP_DIR/app-run-wrapper")" = \
  "Linux keychain runtime: available" ] ||
    fail "a valid shell AppRun wrapper was rejected for keychain status"
[ "$(linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "packaged Linux keychain runtime was not reported"
require_linux_keychain_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "packaged Linux keychain runtime was rejected"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/app-run-wrapper"
GC_TEST_APPIMAGE_ENTRY_NAME="app-run-wrapper"
WRAPPED_KEYCHAIN_STATUS=$(
    APPDIR="$TEMP_DIR/stale-appdir" \
        APPIMAGE="$TEMP_DIR/stale.AppImage" \
        OWD="$TEMP_DIR/stale-owd" \
        linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage"
)
[ "$WRAPPED_KEYCHAIN_STATUS" = \
  "Linux keychain runtime: bundled" ] ||
    fail "packaged shell AppRun wrapper was rejected for keychain status"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"

GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/missing-copyright"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged Linux keychain runtime without copyright was accepted"
fi
if require_linux_keychain_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "release gate accepted a keychain runtime without copyright"
fi

GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/libsecret/copyright"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/keychain-disabled-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="keychain-disabled-entry"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged binary without compiled libsecret support was accepted"
fi
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/keychain-unavailable-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="keychain-unavailable-entry"
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged binary with unavailable libsecret runtime was accepted"
fi

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
GC_TEST_APPIMAGE_LIBSECRET=
if linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "packaged image without libsecret was accepted"
fi

assert_contains "$SUPPORT" \
    'trap cleanup_status_home EXIT'
assert_contains "$SUPPORT" \
    'trap cleanup_extract_dir EXIT'

grep -Eq '^sip[[:space:]]*==[[:space:]]*6\.15\.1$' "$REQUIREMENTS" ||
    fail "test must be reviewed when the pinned SIP version changes"

for packager in "$LOCAL_PACKAGER" "$CI_PACKAGER"; do
    bash -n "$packager"
    assert_contains "$packager" \
        '. Resources/linux/AppImagePackagingSupport.sh'
    assert_contains "$packager" 'install_embedded_python'
    assert_contains "$packager" 'run_packaging_appimage'
    assert_contains "$packager" \
        'require_strava_oauth_build ./GoldenCheetah'
done

for packager in "$LOCAL_PACKAGER" "$CI_PACKAGER" "$DEV_PACKAGER"; do
    bash -n "$packager"
    assert_contains "$packager" \
        'require_strava_oauth_appimage'
    assert_contains "$packager" \
        'install_linux_keychain_runtime'
    assert_contains "$packager" \
        'require_linux_keychain_appimage'
    assert_contains "$packager" \
        'run_linuxdeployqt_with_keychain_probe'
    assert_contains "$packager" \
        'install_qt_offscreen_plugin'
    assert_contains "$packager" \
        'require_qt_offscreen_appimage'
done

if grep -Fq 'python3.7' "$LOCAL_PACKAGER"; then
    fail "local AppImage packaging still embeds unsupported Python 3.7"
fi
assert_contains "$LOCAL_PACKAGER" 'write_source_revision'
assert_contains "$LOCAL_PACKAGER" \
    'run_packaging_appimage "./$FINAL_NAME" --version'
assert_contains "$DEV_CONFIG" \
    '# DEFINES += GC_STRAVA_CLIENT_ID=\\\"your_client_id\\\"'
assert_contains "$DEV_CONFIG" \
    '# DEFINES += GC_STRAVA_CLIENT_SECRET=\\\"your_client_secret\\\"'
assert_contains "$MAIN_SOURCE" \
    '--goldencheetah-linux-keychain-status'
assert_contains "$MAIN_SOURCE" \
    'configureBundledLinuxRuntime'
assert_contains "$LIBSECRET_SOURCE" \
    'GC_QTKEYCHAIN_LIBSECRET_PATH'
assert_contains "$APPVEYOR_INSTALL" 'libsecret-1-dev'
assert_contains "$APPVEYOR_INSTALL" 'libgpg-error-dev'
assert_contains "$APPVEYOR_INSTALL" 'pkg-config'

bash -n "$SUPPORT"
echo "PASS: AppImage Python runtime and packaging helpers are consistent"
