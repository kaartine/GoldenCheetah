#!/bin/sh
set -eu

if [ "$#" -ne 6 ]; then
    echo "Usage: $0 SOURCES LISTS SNAPSHOT APT_VERSION SERIES ARCHITECTURE" >&2
    exit 2
fi

sources=$1
lists=$2
snapshot=$3
apt_version=$4
series=$5
architecture=$6

fail()
{
    echo "APT snapshot bootstrap verification failed: $*" >&2
    exit 1
}

case "$snapshot" in
    [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z) ;;
    *) fail "invalid snapshot timestamp" ;;
esac
case "$series" in
    *[!a-z0-9-]*|'') fail "invalid Ubuntu series" ;;
esac
case "$architecture" in
    *[!a-z0-9-]*|'') fail "invalid Debian architecture" ;;
esac
dpkg --compare-versions "$apt_version" ge 2.4.11 ||
    fail "apt does not support fail-closed Ubuntu snapshots"
[ -f "$sources" ] && [ ! -L "$sources" ] || fail "unsafe source list"
[ -d "$lists" ] && [ ! -L "$lists" ] || fail "unsafe index directory"
source_parts=$(dirname -- "$sources")/sources.list.d
if [ -e "$source_parts" ] || [ -L "$source_parts" ]; then
    [ -d "$source_parts" ] && [ ! -L "$source_parts" ] ||
        fail "unsafe source-parts directory"
    for source_part in \
        "$source_parts"/* \
        "$source_parts"/.[!.]* \
        "$source_parts"/..?*; do
        if [ ! -e "$source_part" ] && [ ! -L "$source_part" ]; then
            continue
        fi
        [ -f "$source_part" ] && [ ! -L "$source_part" ] ||
            fail "unsafe source part"
        [ ! -s "$source_part" ] || fail "nonempty source part"
    done
fi
apt_helper=/usr/lib/apt/apt-helper
[ -f "$apt_helper" ] && [ ! -L "$apt_helper" ] && [ -x "$apt_helper" ] ||
    fail "trusted apt-helper is unavailable"

work=$(mktemp -d)
expected_sources=$work/expected-sources
observed_sources=$work/observed-sources
decompressed_index=$work/Packages
cleanup()
{
    rm -rf -- "$work"
}
trap cleanup EXIT HUP INT TERM

{
    printf 'deb [snapshot=%s] http://archive.ubuntu.com/ubuntu %s main restricted universe multiverse\n' "$snapshot" "$series"
    printf 'deb [snapshot=%s] http://archive.ubuntu.com/ubuntu %s-updates main restricted universe multiverse\n' "$snapshot" "$series"
    printf 'deb [snapshot=%s] http://archive.ubuntu.com/ubuntu %s-backports main restricted universe multiverse\n' "$snapshot" "$series"
    printf 'deb [snapshot=%s] http://security.ubuntu.com/ubuntu %s-security main restricted universe multiverse\n' "$snapshot" "$series"
} | LC_ALL=C sort >"$expected_sources"
awk 'NF && $1 !~ /^#/ { sub(/^[[:space:]]+/, ""); sub(/[[:space:]]+$/, ""); print }' \
    "$sources" | LC_ALL=C sort >"$observed_sources"
cmp -s "$expected_sources" "$observed_sources" ||
    fail "sources differ from the reviewed snapshot set"

snapshot_iso=$(printf '%s' "$snapshot" | sed -E \
    's/^([0-9]{4})([0-9]{2})([0-9]{2})T([0-9]{2})([0-9]{2})([0-9]{2})Z$/\1-\2-\3 \4:\5:\6 UTC/')
snapshot_epoch=$(date -u -d "$snapshot_iso" +%s) || fail "invalid snapshot date"
release_count=0
package_count=0
for suite in "$series" "$series-updates" "$series-backports" "$series-security"; do
    release="$lists/snapshot.ubuntu.com_ubuntu_${snapshot}_dists_${suite}_InRelease"
    [ -f "$release" ] && [ ! -L "$release" ] ||
        fail "missing signed release index for $suite"
    [ "$(sed -n '1p' "$release")" = "-----BEGIN PGP SIGNED MESSAGE-----" ] ||
        fail "release index is not an InRelease document for $suite"
    [ "$(sed -n 's/^Suite: //p' "$release" | head -n 1)" = "$suite" ] ||
        fail "release suite mismatch for $suite"
    [ "$(sed -n 's/^Components: //p' "$release" | head -n 1)" = \
        "main restricted universe multiverse" ] ||
        fail "release components mismatch for $suite"
    release_date=$(sed -n 's/^Date: //p' "$release" | head -n 1)
    [ -n "$release_date" ] || fail "release date is missing for $suite"
    release_epoch=$(date -u -d "$release_date" +%s) ||
        fail "release date is invalid for $suite"
    [ "$release_epoch" -le "$snapshot_epoch" ] ||
        fail "release index is newer than the requested snapshot"
    release_count=$((release_count + 1))

    for component in main restricted universe multiverse; do
        package_path="$component/binary-$architecture/Packages"
        package_metadata=$(awk -v package_path="$package_path" '
            $0 == "SHA256:" { in_sha256 = 1; next }
            in_sha256 && $0 !~ /^[[:space:]]/ { in_sha256 = 0 }
            in_sha256 && NF == 3 && $3 == package_path {
                if (length($1) != 64 || $1 ~ /[^0-9a-f]/ ||
                    $2 !~ /^[0-9]+$/) exit 2
                count++
                digest = $1
                size = $2
            }
            END {
                if (count != 1) exit 2
                print digest " " size
            }
        ' "$release") ||
            fail "missing signed package metadata for $suite/$component"
        package_digest=${package_metadata%% *}
        package_size=${package_metadata#* }
        stem="$lists/snapshot.ubuntu.com_ubuntu_${snapshot}_dists_${suite}_${component}_binary-${architecture}_Packages"
        set -- "$stem" "$stem.gz" "$stem.lz4" "$stem.xz"
        found=
        for candidate do
            if [ -e "$candidate" ] || [ -L "$candidate" ]; then
                [ -z "$found" ] ||
                    fail "duplicate package index for $suite/$component"
                found=$candidate
            fi
        done
        if [ "$package_size" -eq 0 ] && [ -z "$found" ]; then
            continue
        fi
        [ -n "$found" ] && [ -f "$found" ] && [ ! -L "$found" ] ||
            fail "missing or unsafe package index for $suite/$component"
        "$apt_helper" cat-file "$found" >"$decompressed_index" ||
            fail "apt-helper could not read package index for $suite/$component"
        actual_size=$(wc -c <"$decompressed_index" | tr -d '[:space:]')
        actual_digest=$(sha256sum "$decompressed_index")
        actual_digest=${actual_digest%% *}
        [ "$actual_size" = "$package_size" ] &&
            [ "$actual_digest" = "$package_digest" ] ||
            fail "package index differs from signed metadata for $suite/$component"
        rm -f -- "$decompressed_index"
        package_count=$((package_count + 1))
    done
done

observed_release_count=$(find "$lists" -maxdepth 1 \
    -name '*_InRelease' -print | wc -l)
[ "$observed_release_count" -eq "$release_count" ] ||
    fail "snapshot release index set contains unexpected entries"
observed_package_count=$(find "$lists" -maxdepth 1 \
    -name '*_Packages*' -print | wc -l)
[ "$observed_package_count" -eq "$package_count" ] ||
    fail "package index set contains unexpected entries"
