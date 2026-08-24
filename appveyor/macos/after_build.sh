#!/bin/bash
set -euo pipefail

readonly HOMEBREW_BREW_COMMIT=67658c8cf6ee685420c531ed94ed46b6e7ba5b2a
readonly HOMEBREW_CORE_COMMIT=2602f7f80784581466deb491f09bf734174ac772
readonly EXPECTED_QT_VERSION=6.5.3

REPOSITORY_ROOT=$(pwd -P)

export HOMEBREW_NO_ANALYTICS=1
export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_CLEANUP=1
export HOMEBREW_NO_INSTALL_FROM_API=1

assert_formula_version()
{
    local formula=$1
    local expected_version=$2
    local actual
    local receipt

    actual=$(brew list --versions --formula "$formula")
    test "$actual" = "$formula $expected_version"
    receipt="$(brew --prefix "$formula")/INSTALL_RECEIPT.json"
    test -f "$receipt"
    /usr/bin/python3 - "$receipt" "$HOMEBREW_CORE_COMMIT" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as receipt_file:
    receipt = json.load(receipt_file)
if receipt.get("source", {}).get("tap_git_head") != sys.argv[2]:
    raise SystemExit(1)
PY
}

has_rpath()
{
    local binary=$1
    local expected=$2
    local load_commands

    load_commands=$(otool -l "$binary")
    printf '%s\n' "$load_commands" | awk -v expected="$expected" '
        $1 == "path" && $2 == expected { found=1 }
        END { exit(found ? 0 : 1) }
    '
}

ensure_rpath()
{
    local binary=$1
    local rpath=$2

    if has_rpath "$binary" "$rpath"; then
        return
    fi
    install_name_tool -add_rpath "$rpath" "$binary"
}

BREW_REPOSITORY=$(brew --repository)
CORE_REPOSITORY="$BREW_REPOSITORY/Library/Taps/homebrew/homebrew-core"
test "$(git -C "$BREW_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_BREW_COMMIT"
test "$(git -C "$CORE_REPOSITORY" rev-parse HEAD)" = "$HOMEBREW_CORE_COMMIT"
assert_formula_version gsl 2.8
assert_formula_version icu4c@78 78.3
assert_formula_version libical 4.0.4
assert_formula_version libusb 1.0.30
assert_formula_version libsamplerate 0.2.2
assert_formula_version openssl@3 3.6.3
assert_formula_version "python@${PYTHON_VERSION}" 3.11.15_4
test "$(qmake -query QT_VERSION)" = "$EXPECTED_QT_VERSION"

cd src
export PIP_BREAK_SYSTEM_PACKAGES=1
OAUTH_CHECKER="$REPOSITORY_ROOT/appveyor/check-unconfigured-oauth.py"
/usr/bin/python3 "$OAUTH_CHECKER" \
    GoldenCheetah.app/Contents/MacOS/GoldenCheetah

# Homebrew install location
BREW_PYTHON_ROOT=$(brew --prefix "python@${PYTHON_VERSION}")
export PATH="${BREW_PYTHON_ROOT}/bin:$PATH"
BREW_PYTHON_FRAMEWORK="${BREW_PYTHON_ROOT}/Frameworks/Python.framework"
ICU_PREFIX=$(brew --prefix icu4c@78)

echo "About to create dmg file and fix up"
mkdir -p GoldenCheetah.app/Contents/Frameworks

# This is a hack to include libicudata.*.dylib, not handled by macdployqt[fix]
cp "$ICU_PREFIX"/lib/libicudata.*.dylib GoldenCheetah.app/Contents/Frameworks

# Copy python framework and change permissions to fix paths
echo "Copying Python Framework from ${BREW_PYTHON_FRAMEWORK}"
# Remove any old attempts to avoid link confusion
rm -rf GoldenCheetah.app/Contents/Frameworks/Python.framework
rsync -axL "${BREW_PYTHON_FRAMEWORK}/" "GoldenCheetah.app/Contents/Frameworks/Python.framework/"

# Fix the Python Framework structure broken by rsync -L
# rsync -L turns symlinks into directories/files, confusing codesign. We restore the standard structure.
echo "Restoring standard Python Framework structure..."
pushd GoldenCheetah.app/Contents/Frameworks/Python.framework > /dev/null
rm -rf Headers Resources Python Versions/Current
ln -s "${PYTHON_VERSION}" Versions/Current
ln -s Versions/Current/Headers Headers
ln -s Versions/Current/Resources Resources
ln -s Versions/Current/Python Python
popd > /dev/null

# This ensures every level of the path is a real directory
mkdir -p "GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/lib/python${PYTHON_VERSION}"

chmod -R +w GoldenCheetah.app/Contents/Frameworks

PYTHON_BIN="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/bin/python${PYTHON_VERSION}"

echo "Installing requirements into bundle..."
if [ -f "$PYTHON_BIN" ]; then
    # Fix RPATH early
    ensure_rpath "$PYTHON_BIN" "@executable_path/../.."

    # Re-sign the binary immediately because install_name_tool invalidated it
    codesign --force --sign - "$PYTHON_BIN"

    echo "Running pip install using bundled python: $PYTHON_BIN"
    # Install the reviewed Python dependency set.
    # Build third-party packages in an empty directory. The copied Homebrew
    # framework may contain unrelated site-packages that must not leak into the
    # release payload.
    SITE_PACKAGES="$(pwd)/GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/Current/lib/python3.11/site-packages"
    if [ ! -d "$SITE_PACKAGES" ]; then
         SITE_PACKAGES_PARENT=$(find "$(pwd)/GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/Current/lib" -name "python3.*" -type d -print -quit)
         test -n "$SITE_PACKAGES_PARENT"
         SITE_PACKAGES="$SITE_PACKAGES_PARENT/site-packages"
    fi
    echo "Installing Python packages to bundle target: $SITE_PACKAGES"

    PYTHON_PACKAGE_STAGE=$(mktemp -d)
    cleanup_python_package_stage()
    {
        rm -rf "$PYTHON_PACKAGE_STAGE"
    }
    trap cleanup_python_package_stage EXIT
    PIP_CONFIG_FILE=/dev/null PYTHONDONTWRITEBYTECODE=1 \
      "${BREW_PYTHON_ROOT}/bin/python${PYTHON_VERSION}" -B -m pip install \
        --isolated --disable-pip-version-check --no-input --no-cache-dir \
        --no-compile --target "$PYTHON_PACKAGE_STAGE" --ignore-installed \
        --break-system-packages --require-hashes --only-binary :all: \
        --index-url=https://pypi.org/simple \
        -r ../src/Python/requirements-appimage.lock
    rm -rf "$PYTHON_PACKAGE_STAGE/bin"
    find "$PYTHON_PACKAGE_STAGE" -type d -name __pycache__ -prune \
        -exec rm -rf {} +
    find "$PYTHON_PACKAGE_STAGE" -type f -name '*.pyc' -delete
    if grep -R -F -I -l -- "$PYTHON_PACKAGE_STAGE" \
       "$PYTHON_PACKAGE_STAGE" >/dev/null 2>&1; then
        echo "Python payload retains its temporary installation path." >&2
        exit 1
    fi
    if grep -R -F -I -l -- "$(pwd)" \
       "$PYTHON_PACKAGE_STAGE" >/dev/null 2>&1; then
        echo "Python payload retains its build workspace path." >&2
        exit 1
    fi
    rm -rf "$SITE_PACKAGES"
    mkdir -p "$(dirname "$SITE_PACKAGES")"
    mv "$PYTHON_PACKAGE_STAGE" "$SITE_PACKAGES"
    PYTHON_PACKAGE_STAGE=
    trap - EXIT

    PYTHONDONTWRITEBYTECODE=1 "$PYTHON_BIN" -B - \
        ../src/Python/requirements-appimage.lock "$SITE_PACKAGES" <<'PY'
from importlib import metadata
from pathlib import Path
import re
import sys

expected = {}
for line in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    match = re.fullmatch(r"([A-Za-z0-9_.-]+)==([^\s\\]+)\s*\\?", line.strip())
    if match:
        expected[re.sub(r"[-_.]+", "-", match.group(1)).lower()] = match.group(2)
actual = {
    re.sub(r"[-_.]+", "-", distribution.metadata["Name"]).lower():
        distribution.version
    for distribution in metadata.distributions(path=[sys.argv[2]])
}
if actual != expected:
    raise SystemExit("bundled Python distributions do not match the lock")
PY
else
    echo "ERROR: Bundled python binary not found at $PYTHON_BIN"
    exit 1
fi

SITE_PACKAGES_SRC=$( PYTHONDONTWRITEBYTECODE=1 "$PYTHON_BIN" -B -c "import numpy; import os; print(os.path.dirname(os.path.dirname(numpy.__file__)))" )
echo "Verified Site Packages at: $SITE_PACKAGES_SRC"

# Remove direct_url.json metadata which may contain absolute paths to the build machine
find "$SITE_PACKAGES_SRC" -name "direct_url.json" -delete

# Update deployed Python framework path
# Change the ID of the library itself so it knows it lives in the app now
install_name_tool -id "@executable_path/../Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python" "./GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python"

# Update GoldenCheetah binary to reference deployed lib
# We replace the absolute path to the brew framework with the relative path inside the bundle
# Update GoldenCheetah binary to reference deployed lib
# We replace the absolute path to the brew framework with the relative path inside the bundle
GC_BIN="./GoldenCheetah.app/Contents/MacOS/GoldenCheetah"
GC_DEPENDENCIES=$(otool -L "$GC_BIN")
OLD_GC_PATH=$(printf '%s\n' "$GC_DEPENDENCIES" | awk '
    $1 ~ /Python\.framework/ && $1 !~ /@executable_path/ { print $1; exit }
')
if [ -n "$OLD_GC_PATH" ]; then
    echo "Updating GoldenCheetah binary dependency from $OLD_GC_PATH"
    install_name_tool -change "$OLD_GC_PATH" "@executable_path/../Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python" "$GC_BIN"
else
    echo "GoldenCheetah binary already uses relative path or Python framework not found."
fi

# Update Python binary to reference deployed lib instead of the Cellar one
PYTHON_BIN="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/bin/python${PYTHON_VERSION}"
if [ -f "$PYTHON_BIN" ]; then
    echo "Debugging dependencies for $PYTHON_BIN"
    otool -L "$PYTHON_BIN"
    # Iterate over all dependencies that look like Python and are absolute paths (not already @executable_path)
    PYTHON_DEPENDENCIES=$(otool -L "$PYTHON_BIN")
    OLD_PYTHON_PATHS=$(printf '%s\n' "$PYTHON_DEPENDENCIES" | awk '
        $1 ~ /Python/ && $1 ~ /\// && $1 !~ /@executable_path/ { print $1 }
    ')
    if [ -n "$OLD_PYTHON_PATHS" ]; then
        while IFS= read -r OLD_PATH; do
            echo "Updating python binary dependency from $OLD_PATH"
            install_name_tool -change "$OLD_PATH" "@executable_path/../Python" "$PYTHON_BIN"
        done <<< "$OLD_PYTHON_PATHS"
    fi
else
    echo "Python binary not found at $PYTHON_BIN" >&2
    exit 1
fi

# Same for the Python app stub if it exists
PYTHON_APP_BIN="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Resources/Python.app/Contents/MacOS/Python"
if [ -f "$PYTHON_APP_BIN" ]; then
    # Fix the app stub dependency too!
    PYTHON_APP_DEPENDENCIES=$(otool -L "$PYTHON_APP_BIN")
    OLD_PYTHON_APP_PATHS=$(printf '%s\n' "$PYTHON_APP_DEPENDENCIES" | awk '
        $1 ~ /Python/ && $1 ~ /\// && $1 !~ /@executable_path/ { print $1 }
    ')
    if [ -n "$OLD_PYTHON_APP_PATHS" ]; then
        while IFS= read -r OLD_PATH_APP; do
            install_name_tool -change "$OLD_PATH_APP" "@executable_path/../../../../Python" "$PYTHON_APP_BIN"
        done <<< "$OLD_PYTHON_APP_PATHS"
    fi
fi

# The clean locked target intentionally excludes pip itself. Remove copied
# Homebrew launchers rather than shipping commands that cannot resolve pip.
echo "Removing non-runtime pip launchers..."
PYTHON_BIN_DIR="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/bin"
if [ -d "$PYTHON_BIN_DIR" ]; then
    for PIP_BIN in pip pip3 pip${PYTHON_VERSION}; do
        rm -f "$PYTHON_BIN_DIR/$PIP_BIN"
    done
fi

# Patch _sysconfigdata to remove absolute paths from build machine
echo "Patching _sysconfigdata..."
SYSCONFIG_FILE=$(find "GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/lib/python${PYTHON_VERSION}" -name "_sysconfigdata_*.py" -print -quit)
if [ -f "$SYSCONFIG_FILE" ]; then
    echo "Patching $SYSCONFIG_FILE to be relocatable..."
    # Use python to safely replace the string literal, avoiding sed quoting issues
    # BREW_PYTHON_ROOT is set at top of script
    python3 <<EOF
import sys
import os

filepath = "$SYSCONFIG_FILE"
prefix = "$BREW_PYTHON_ROOT"

if os.path.exists(filepath):
    with open(filepath, 'r') as f:
        content = f.read()

    # Ensure sys is imported
    if "import sys" not in content:
        content = "import sys\n" + content

    # Replace absolute prefix with f-string using sys.prefix
    # This preserves implicit string concatenation (e.g. f'...' '...')
    content = content.replace("'" + prefix, "f'{sys.prefix}")
    content = content.replace('"' + prefix, 'f"{sys.prefix}')

    with open(filepath, 'w') as f:
        f.write(content)
EOF
fi

# Fix libpython dependencies inside the framework to point to the internal framework binary
# This prevents macdeployqt from chasing external links and failing with path errors
LIBPYTHON_FILES=$(find GoldenCheetah.app/Contents/Frameworks/Python.framework -name "libpython*.dylib" -type f)
test -n "$LIBPYTHON_FILES"
while IFS= read -r LIB; do
    echo "Fixing library dependency for: $LIB"
    LIB_DEPENDENCIES=$(otool -L "$LIB")
    OLD_LIB_PATH=$(printf '%s\n' "$LIB_DEPENDENCIES" | awk '
        $1 ~ /Python\.framework/ && $1 !~ /@executable_path/ { print $1; exit }
    ')
    if [ -n "$OLD_LIB_PATH" ]; then
        echo "  Changing $OLD_LIB_PATH to @executable_path/../Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python"
        install_name_tool -change "$OLD_LIB_PATH" "@executable_path/../Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python" "$LIB"
    fi
    # Also fix the ID of the dylib itself if needed (macdeployqt likes valid IDs)
    install_name_tool -id "@executable_path/../Frameworks/Python.framework/Versions/${PYTHON_VERSION}/lib/$(basename "$LIB")" "$LIB"
done <<< "$LIBPYTHON_FILES"

# OpenSSL bundling for Python _ssl module
echo "Bundling OpenSSL libraries for Python..."
OPENSSL_PREFIX=$(brew --prefix openssl@3)
if [ -d "$OPENSSL_PREFIX" ]; then
    echo "Found OpenSSL at $OPENSSL_PREFIX"

    # Destination for OpenSSL libs (same level as Python framework usually, or inside it)
    # Putting them in Frameworks/ is standard
    DEST_FRAMEWORKS="GoldenCheetah.app/Contents/Frameworks"

    # Copy the dylibs
    cp "$OPENSSL_PREFIX/lib/libssl.3.dylib" "$DEST_FRAMEWORKS/"
    cp "$OPENSSL_PREFIX/lib/libcrypto.3.dylib" "$DEST_FRAMEWORKS/"

    # Make them writable for install_name_tool
    chmod +w "$DEST_FRAMEWORKS/libssl.3.dylib"
    chmod +w "$DEST_FRAMEWORKS/libcrypto.3.dylib"

    # Fix IDs of the copied libs
    # Fix IDs of the copied libs
    install_name_tool -id "@loader_path/libssl.3.dylib" "$DEST_FRAMEWORKS/libssl.3.dylib"
    install_name_tool -id "@loader_path/libcrypto.3.dylib" "$DEST_FRAMEWORKS/libcrypto.3.dylib"

    # Fix dependency of libssl on libcrypto - use @loader_path (they are in same dir)
    install_name_tool -change "$OPENSSL_PREFIX/lib/libcrypto.3.dylib" "@loader_path/libcrypto.3.dylib" "$DEST_FRAMEWORKS/libssl.3.dylib"

    # Now find the python extensions that need them (_ssl, _hashlib)
    # They are in lib-dynload
    DYNLOAD_DIR="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/lib/python${PYTHON_VERSION}/lib-dynload"

    if [ -d "$DYNLOAD_DIR" ]; then
        for EXT in _ssl _hashlib; do
            # Find the actual so file (e.g. _ssl.cpython-311-darwin.so)
            EXT_FILE=$(find "$DYNLOAD_DIR" -name "${EXT}.*.so" -print -quit)

            if [ -n "$EXT_FILE" ]; then
                echo "Patching $EXT_FILE"
                # Update linkage to find libssl/libcrypto relative to _ssl.so
                # _ssl.so is in .../lib/python3.11/lib-dynload
                # libssl is in .../Frameworks
                # Path is @loader_path/../../../../../../libssl.3.dylib
                install_name_tool -change "$OPENSSL_PREFIX/lib/libssl.3.dylib" "@loader_path/../../../../../../libssl.3.dylib" "$EXT_FILE"
                install_name_tool -change "$OPENSSL_PREFIX/lib/libcrypto.3.dylib" "@loader_path/../../../../../../libcrypto.3.dylib" "$EXT_FILE"
            else
                echo "Could not find extension for $EXT in $DYNLOAD_DIR" >&2
                exit 1
            fi
        done
        # Also patch hashlib
    else
        echo "Error: lib-dynload directory not found at $DYNLOAD_DIR"
        exit 1
    fi

else
    echo "Error: Could not find openssl@3 prefix"
    exit 1
fi

# Fix missing QtDBus framework (required by QtGui but missed by macdeployqt)
# We copy it manually so it's present when we sign
echo "Manually copying QtDBus framework..."
cp -R "${QTDIR}/lib/QtDBus.framework" GoldenCheetah.app/Contents/Frameworks/

# Deployment using macdeployqt - prepare bundle only
macdeployqt GoldenCheetah.app -verbose=2 \
    -executable=GoldenCheetah.app/Contents/MacOS/GoldenCheetah \
    -qmldir=Train/qml

### MANUAL LEAK PATCHING ###
echo "Starting manual leak patching..."

# Helper: Fix the ID of a binary to be relative (@rpath)
fix_binary_id() {
    local BINARY="$1"
    local BINARY_ID
    local BINARY_IDS

    BINARY_IDS=$(otool -D "$BINARY")
    BINARY_ID=$(printf '%s\n' "$BINARY_IDS" | awk '$0 !~ /:$/ { print; exit }')

    # Ensure writable
    chmod +w "$BINARY"

    # Check if ID is a system path (excluding /System/Library)
    if [[ "$BINARY_ID" == *"/opt/homebrew"* ]] || [[ "$BINARY_ID" == *"/usr/local"* ]] || [[ "$BINARY_ID" == *"/Users"* ]] || [[ "$BINARY_ID" == *"/Library/Frameworks"* ]]; then
        local NEW_ID=""
        if [[ "$BINARY" == *".framework"* ]]; then
            # Extract framework relative path: .../Foo.framework/Versions/A/Foo -> Foo.framework/Versions/A/Foo
            local REL_PATH
            REL_PATH=$(printf '%s\n' "$BINARY" | sed -E 's/.*\/([^\/]+\.framework.*)/\1/')
            NEW_ID="@rpath/$REL_PATH"
        else
            # Flat lib: libfoo.dylib
            local LIB_NAME
            LIB_NAME=$(basename "$BINARY")
            NEW_ID="@rpath/$LIB_NAME"
        fi

        echo "  Fixing ID for $BINARY"
        echo "    Old: $BINARY_ID"
        echo "    New: $NEW_ID"
        install_name_tool -id "$NEW_ID" "$BINARY"
    fi
}

# Helper: Fix dependencies of a binary
fix_binary_deps() {
    local BINARY="$1"
    local BINARY_DEPENDENCIES
    local LEAK_PATHS

    BINARY_DEPENDENCIES=$(otool -L "$BINARY")
    LEAK_PATHS=$(printf '%s\n' "$BINARY_DEPENDENCIES" | awk '
        $1 ~ /(\/usr\/local\/|\/opt\/homebrew\/|\/Users\/|\/Library\/Frameworks\/)/ &&
        $1 !~ /\/System\// { print $1 }
    ')
    if [ -z "$LEAK_PATHS" ]; then
        return
    fi

    while IFS= read -r LEAK_PATH; do
        local DEST_REL=""
        if [[ "$LEAK_PATH" == *".framework"* ]]; then
             local REL_PATH
             REL_PATH=$(printf '%s\n' "$LEAK_PATH" | sed -E 's/.*\/([^\/]+\.framework.*)/\1/')
             DEST_REL="@rpath/$REL_PATH"
        else
             local LIB_NAME
             LIB_NAME=$(basename "$LEAK_PATH")
             if [ -f "GoldenCheetah.app/Contents/Frameworks/$LIB_NAME" ]; then
                  DEST_REL="@rpath/$LIB_NAME"
             else
                  if [ -f "$LEAK_PATH" ]; then
                       echo "  Copying missing lib $LIB_NAME to bundle from $LEAK_PATH..."
                       cp "$LEAK_PATH" "GoldenCheetah.app/Contents/Frameworks/"
                       chmod +w "GoldenCheetah.app/Contents/Frameworks/$LIB_NAME"
                       # Recursively fix the new lib
                       fix_binary_id "GoldenCheetah.app/Contents/Frameworks/$LIB_NAME"
                       fix_binary_deps "GoldenCheetah.app/Contents/Frameworks/$LIB_NAME"
                       DEST_REL="@rpath/$LIB_NAME"
                  else
                       echo "Library $LIB_NAME not found in bundle or system ($LEAK_PATH)" >&2
                       return 1
                  fi
             fi
        fi

        if [ -n "$DEST_REL" ]; then
             echo "  Relinking dep $LEAK_PATH -> $DEST_REL in $BINARY"
             install_name_tool -change "$LEAK_PATH" "$DEST_REL" "$BINARY"
        fi
    done <<< "$LEAK_PATHS"
}


# 0. cleanup static archives
find GoldenCheetah.app/Contents/Frameworks -name "*.a" -delete

# 1. QtWebEngineProcess
QWEBVIEW_APP="GoldenCheetah.app/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
if [ -f "$QWEBVIEW_APP" ]; then
    echo "Patching QtWebEngineProcess..."
    ensure_rpath "$QWEBVIEW_APP" "@executable_path/../../../../../../../"
    fix_binary_deps "$QWEBVIEW_APP"
fi

# 2. Python Binaries
echo "Patching Python binaries..."
# Manual RPATH fix for python binaries so they can find @rpath/Python.framework...
# In AppVeyor script, PYTHON_VERSION is used instead of PYTHON_FULL_VER
PYTHON_BIN="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/bin/python${PYTHON_VERSION}"
if [ -f "$PYTHON_BIN" ]; then
    ensure_rpath "$PYTHON_BIN" "@executable_path/../.."
    fix_binary_deps "$PYTHON_BIN"
fi

PYTHON_APP_BIN="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Resources/Python.app/Contents/MacOS/Python"
if [ -f "$PYTHON_APP_BIN" ]; then
     ensure_rpath "$PYTHON_APP_BIN" "@executable_path/../../../../../../.."
     fix_binary_deps "$PYTHON_APP_BIN"
fi

# 3. Mass Scan
echo "Scanning entire bundle for other leaks..."
set +v
BUNDLE_BINARIES=$(find GoldenCheetah.app/Contents/MacOS GoldenCheetah.app/Contents/Frameworks \( -name "GoldenCheetah" -o -name "*.dylib" -o -name "*.so" -o -perm +111 \) -type f | sort -u)
test -n "$BUNDLE_BINARIES"
while IFS= read -r BINARY; do
    if file "$BINARY" | grep -q "Mach-O"; then
        fix_binary_id "$BINARY"
        fix_binary_deps "$BINARY"
    fi
done <<< "$BUNDLE_BINARIES"
set -v

echo "Resigning application bundle..."
# Explicitly sign the Python components first to fix invalid Ad-Hoc signatures caused by install_name_tool
echo "Forcing signature refresh on Python components..."
codesign --force --sign - "GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Python"
codesign --force --sign - "GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/bin/python${PYTHON_VERSION}"
if [ -f "$PYTHON_APP_BIN" ]; then
    codesign --force --sign - "$PYTHON_APP_BIN"
fi

# Explicitly resign all .so and .dylib files in the framework (e.g. in lib-dynload)
# codesign --deep on the app bundle often skips these or fails to resign them properly
echo "Resigning all dynamic libraries in Python framework AND Contents/Frameworks..."
find "GoldenCheetah.app/Contents/Frameworks" -type f \( -name "*.dylib" -o -name "*.so" \) -exec codesign --force --sign - {} \;

# Sign the nested Python.app if it exists (Inside-Out signing)
PYTHON_APP="GoldenCheetah.app/Contents/Frameworks/Python.framework/Versions/${PYTHON_VERSION}/Resources/Python.app"
if [ -d "$PYTHON_APP" ]; then
    echo "Signing nested Python.app..."
    codesign --force --preserve-metadata=identifier,entitlements --sign - "$PYTHON_APP"
fi

# Sign the Python framework itself
# We verified structure earlier.
echo "Signing Python.framework..."
codesign --force --sign - "GoldenCheetah.app/Contents/Frameworks/Python.framework"

# - sign with ad-hoc identity check, this is free and works for local dev
# - force rewrite of existing signatures (invalidated by install_name_tool)
# - deep sign frameworks and plugins
echo "Signing final GoldenCheetah.app..."
codesign --force --deep --sign - GoldenCheetah.app
codesign --verify --deep --strict GoldenCheetah.app

echo "Validating final macOS payload paths and provenance..."
while IFS= read -r BINARY; do
    if file "$BINARY" | grep -q "Mach-O"; then
        if { otool -L "$BINARY"; otool -D "$BINARY"; otool -l "$BINARY"; } |
           grep -E '(/usr/local/|/opt/homebrew/|/Users/|/Library/Frameworks/)' |
           grep -v -E '/System/Library/|/usr/lib/' >/dev/null; then
            echo "Final Mach-O payload retains a non-system absolute path: $BINARY" >&2
            exit 1
        fi
    fi
done <<< "$BUNDLE_BINARIES"

MACOS_PROVENANCE=../GoldenCheetah_v3.8_x64.macos-provenance.json
MACOS_VALIDATOR="$REPOSITORY_ROOT/appveyor/macos/validate-payload.py"
test -f "$MACOS_VALIDATOR"
/usr/bin/python3 "$MACOS_VALIDATOR" \
    --bundle GoldenCheetah.app --output "$MACOS_PROVENANCE" \
    --homebrew-core-commit "$HOMEBREW_CORE_COMMIT" \
    --qt-version "$EXPECTED_QT_VERSION" \
    --formula "gsl=2.8=GPL-3.0-or-later=$(brew --prefix gsl)/INSTALL_RECEIPT.json" \
    --formula "icu4c@78=78.3=Unicode-3.0=$(brew --prefix icu4c@78)/INSTALL_RECEIPT.json" \
    --formula "libical=4.0.4=LGPL-2.1-only OR MPL-2.0=$(brew --prefix libical)/INSTALL_RECEIPT.json" \
    --formula "libusb=1.0.30=LGPL-2.1-or-later=$(brew --prefix libusb)/INSTALL_RECEIPT.json" \
    --formula "libsamplerate=0.2.2=BSD-2-Clause=$(brew --prefix libsamplerate)/INSTALL_RECEIPT.json" \
    --formula "openssl@3=3.6.3=Apache-2.0=$(brew --prefix openssl@3)/INSTALL_RECEIPT.json" \
    --formula "python@${PYTHON_VERSION}=3.11.15_4=PSF-2.0=$(brew --prefix "python@${PYTHON_VERSION}")/INSTALL_RECEIPT.json" \
    --forbidden-prefix "$REPOSITORY_ROOT" \
    --forbidden-prefix "$HOME" \
    --forbidden-prefix "$BREW_REPOSITORY" \
    --forbidden-prefix "$(brew --cellar)" \
    --forbidden-prefix "$QTDIR" \
    --forbidden-prefix "${TMPDIR:-/private/tmp}"

echo "Creating dmg file..."
/usr/bin/python3 "$OAUTH_CHECKER" \
    GoldenCheetah.app/Contents/MacOS/GoldenCheetah
# Manually create DMG since we removed -dmg from macdeployqt
hdiutil create -volname GoldenCheetah -srcfolder GoldenCheetah.app -ov -format UDZO GoldenCheetah.dmg

echo "Renaming dmg file to branch and build number ready for deploy"
mv GoldenCheetah.dmg ../GoldenCheetah_v3.8_x64.dmg
