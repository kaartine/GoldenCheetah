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

declare -F download_file >/dev/null || fail "download_file helper is missing"
declare -F run_packaging_appimage >/dev/null ||
    fail "run_packaging_appimage helper is missing"
declare -F run_packaged_appimage_smoke >/dev/null ||
    fail "run_packaged_appimage_smoke helper is missing"
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
        fputs("strava_oauth=configured", stdout);
        return 0;
    }
    if (strstr(mode, "extra-newline") != NULL) {
        fputs("strava_oauth=configured\n\n", stdout);
        return 0;
    }
    if (strstr(mode, "malformed") != NULL) {
        fputs("strava_oauth=maybe\n", stdout);
    } else if (strstr(mode, "unconfigured") != NULL) {
        fputs("strava_oauth=unavailable\n", stdout);
    } else {
        fputs("strava_oauth=configured\n", stdout);
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

mkdir -p "$TEMP_DIR/libsecret/lib" "$TEMP_DIR/libsecret/bin"
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
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/libsecret-fixture.c" \
    -o "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0"
cat >"$TEMP_DIR/libsecret/incomplete-fixture.c" <<'EOF'
void secret_password_lookup(void) {}
EOF
"${CC:-cc}" -std=c99 -Wall -Wextra -Werror -shared -fPIC \
    -Wl,-soname,libsecret-1.so.0 \
    "$TEMP_DIR/libsecret/incomplete-fixture.c" \
    -o "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0"
ln -s libsecret-1.so.0.0 \
    "$TEMP_DIR/libsecret/lib/libsecret-1.so.0"
cat >"$TEMP_DIR/libsecret/bin/pkg-config" <<EOF
#!/bin/sh
[ "\$1" = "--variable=libdir" ] &&
    [ "\$2" = "libsecret-1" ] || exit 1
printf '%s\n' "$TEMP_DIR/libsecret/lib"
EOF
chmod +x "$TEMP_DIR/libsecret/bin/pkg-config"
printf 'fixture libsecret copyright\n' \
    >"$TEMP_DIR/libsecret/copyright"
printf 'fixture QtKeychain license\n' \
    >"$TEMP_DIR/libsecret/QtKeychain-COPYING"
printf 'fixture complete LGPL-2.1 license\n' \
    >"$TEMP_DIR/libsecret/LGPL-2.1"

KEYCHAIN_APPDIR="$TEMP_DIR/keychain.AppDir"
PATH="$TEMP_DIR/libsecret/bin:$PATH" \
    LIBSECRET_COPYRIGHT_FILE="$TEMP_DIR/libsecret/copyright" \
    LIBSECRET_LICENSE_FILE="$TEMP_DIR/libsecret/LGPL-2.1" \
    install_linux_keychain_runtime \
        "$KEYCHAIN_APPDIR" \
        "$TEMP_DIR/libsecret/QtKeychain-COPYING"
[ "$(linux_keychain_runtime_status "$KEYCHAIN_APPDIR")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "installed Linux keychain runtime was not reported"
cmp "$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0" \
    "$KEYCHAIN_APPDIR/lib/libsecret-1.so.0" ||
    fail "resolved libsecret runtime was not copied exactly"
cmp "$TEMP_DIR/libsecret/copyright" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright" ||
    fail "libsecret copyright was not copied exactly"
cmp "$TEMP_DIR/libsecret/QtKeychain-COPYING" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING" ||
    fail "QtKeychain license was not copied exactly"
cmp "$TEMP_DIR/libsecret/LGPL-2.1" \
    "$KEYCHAIN_APPDIR/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1" ||
    fail "complete LGPL-2.1 license was not copied exactly"

INCOMPLETE_APPDIR="$TEMP_DIR/incomplete-keychain.AppDir"
mkdir -p \
    "$INCOMPLETE_APPDIR/lib" \
    "$INCOMPLETE_APPDIR/usr/share/doc/GoldenCheetah/licenses"
cp "$TEMP_DIR/libsecret/incomplete-libsecret-1.so.0" \
    "$INCOMPLETE_APPDIR/lib/libsecret-1.so.0"
cp "$TEMP_DIR/libsecret/copyright" \
    "$TEMP_DIR/libsecret/QtKeychain-COPYING" \
    "$TEMP_DIR/libsecret/LGPL-2.1" \
    "$INCOMPLETE_APPDIR/usr/share/doc/GoldenCheetah/licenses/"
mv "$INCOMPLETE_APPDIR/usr/share/doc/GoldenCheetah/licenses/copyright" \
    "$INCOMPLETE_APPDIR/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright"
if linux_keychain_runtime_status "$INCOMPLETE_APPDIR" \
    >/dev/null 2>&1; then
    fail "libsecret runtime without required QtKeychain symbols was accepted"
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

printf '\177ELF\002\001\001\000AI\001compressed-appimage-payload' \
    >"$TEMP_DIR/type1.AppImage"
printf '\177ELF\002\001\001\000AI\002compressed-appimage-payload' \
    >"$TEMP_DIR/type2.AppImage"
chmod +x "$TEMP_DIR/type1.AppImage" "$TEMP_DIR/type2.AppImage"

[ "$(strava_oauth_build_status "$TEMP_DIR/unconfigured")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "runtime-unavailable Strava credentials were not reported unavailable"
[ "$(strava_oauth_build_status "$TEMP_DIR/configured")" = \
  "Strava OAuth: configured" ] ||
    fail "configured Strava credentials were not reported"
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
    fail "configured Strava credentials were rejected by the release gate"
if require_strava_oauth_build "$TEMP_DIR/unconfigured" \
    >/dev/null 2>&1; then
    fail "release gate accepted unavailable Strava credentials"
fi
if require_strava_oauth_build "$TEMP_DIR/missing" \
    >/dev/null 2>&1; then
    fail "release gate accepted a missing executable"
fi

GC_TEST_APPIMAGE_SIDECAR="$TEMP_DIR/configured"
GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/configured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="configured-entry"
run_packaging_appimage()
{
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
        cp "$GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/libsecret-copyright
        cp "$GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/QtKeychain-COPYING
        cp "$GC_TEST_APPIMAGE_LIBSECRET_LICENSE" \
            squashfs-root/usr/share/doc/GoldenCheetah/licenses/LGPL-2.1
    fi
}

[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: configured" ] ||
    fail "configured packaged GoldenCheetah was not reported"
require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "configured packaged GoldenCheetah was rejected"

GC_TEST_APPIMAGE_ENTRY="$TEMP_DIR/unconfigured-entry"
GC_TEST_APPIMAGE_ENTRY_NAME="unconfigured-entry"
[ "$(strava_oauth_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Strava OAuth: unavailable (credentials not configured)" ] ||
    fail "unavailable packaged GoldenCheetah was not reported"
if require_strava_oauth_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null 2>&1; then
    fail "release gate accepted an unavailable packaged GoldenCheetah"
fi

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

GC_TEST_APPIMAGE_LIBSECRET="$TEMP_DIR/libsecret/lib/libsecret-1.so.0.0"
GC_TEST_APPIMAGE_LIBSECRET_COPYRIGHT="$TEMP_DIR/libsecret/copyright"
GC_TEST_APPIMAGE_QTKEYCHAIN_LICENSE="$TEMP_DIR/libsecret/QtKeychain-COPYING"
GC_TEST_APPIMAGE_LIBSECRET_LICENSE="$TEMP_DIR/libsecret/LGPL-2.1"
[ "$(linux_keychain_entrypoint_status "$TEMP_DIR/configured-entry")" = \
  "Linux keychain runtime: available" ] ||
    fail "compiled and available Linux keychain entrypoint was not reported"
[ "$(linux_keychain_appimage_status "$TEMP_DIR/type2.AppImage")" = \
  "Linux keychain runtime: bundled" ] ||
    fail "packaged Linux keychain runtime was not reported"
require_linux_keychain_appimage "$TEMP_DIR/type2.AppImage" \
    >/dev/null ||
    fail "packaged Linux keychain runtime was rejected"

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
done

if grep -Fq 'python3.7' "$LOCAL_PACKAGER"; then
    fail "local AppImage packaging still embeds unsupported Python 3.7"
fi
assert_contains "$LOCAL_PACKAGER" 'write_source_revision'
assert_contains "$LOCAL_PACKAGER" 'run_packaged_appimage_smoke'
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
assert_contains "$APPVEYOR_INSTALL" 'pkg-config'

bash -n "$SUPPORT"
echo "PASS: AppImage Python runtime and packaging helpers are consistent"
