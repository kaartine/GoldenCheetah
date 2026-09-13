#!/usr/bin/env bash

set -euo pipefail

test_directory=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
blender_directory=$(cd "$test_directory/.." && pwd)
repository=$(cd "$blender_directory/../../../.." && pwd)
blender=${BLENDER:-blender}
temporary=$(mktemp -d "${TMPDIR:-/tmp}/gc-triposr-cleanup.XXXXXX")
trap 'rm -rf "$temporary"' EXIT

raw="$temporary/raw-synthetic.glb"
first="$temporary/first"
second="$temporary/second"
alternate="$temporary/alternate-axes"

"$blender" --background --factory-startup \
    --python "$test_directory/create_synthetic_input.py" -- \
    --output "$raw"

for output in "$first" "$second"; do
    "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$raw" \
        --output-dir "$output" \
        --up +Y \
        --forward +Z \
        --scale 1.0
    python3 "$test_directory/inspect_cleanup_outputs.py" \
        --raw "$raw" \
        --output-dir "$output"
done

for name in candidate-lod0.glb candidate-lod1.glb \
        candidate-collision.glb cleanup-report.json; do
    cmp "$first/$name" "$second/$name"
done

"$blender" --background --factory-startup \
    --python "$blender_directory/cleanup_triposr_glb.py" -- \
    --input "$raw" \
    --output-dir "$alternate" \
    --up +Z \
    --forward +Y \
    --scale 2.0
python3 "$test_directory/inspect_cleanup_outputs.py" \
    --output-dir "$alternate" \
    --expected-up +Z \
    --expected-forward +Y \
    --expected-scale 2.0 \
    --max-lod-bound-error 0.20

if "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$repository/contrib/workout-game-assets/generated/WG_RiderBike.glb" \
        --output-dir "$temporary/rejected-input" >/dev/null 2>&1; then
    printf 'Cleanup unexpectedly accepted an input inside the repository\n' >&2
    exit 1
fi

if "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$raw" \
        --output-dir "$repository/build/rejected-output" >/dev/null 2>&1; then
    printf 'Cleanup unexpectedly accepted an output inside the repository\n' >&2
    exit 1
fi

ln -s "$raw" "$temporary/raw-link.glb"
if "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$temporary/raw-link.glb" \
        --output-dir "$temporary/rejected-linked-input" >/dev/null 2>&1; then
    printf 'Cleanup unexpectedly accepted a symlinked input\n' >&2
    exit 1
fi

mkdir "$temporary/real-output"
ln -s "$temporary/real-output" "$temporary/linked-output"
if "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$raw" \
        --output-dir "$temporary/linked-output" >/dev/null 2>&1; then
    printf 'Cleanup unexpectedly accepted a symlinked output directory\n' >&2
    exit 1
fi

truncate -s 67108865 "$temporary/oversized.glb"
if "$blender" --background --factory-startup \
        --python "$blender_directory/cleanup_triposr_glb.py" -- \
        --input "$temporary/oversized.glb" \
        --output-dir "$temporary/rejected-oversized-input" >/dev/null 2>&1; then
    printf 'Cleanup unexpectedly accepted an input over 64 MiB\n' >&2
    exit 1
fi

printf 'TripoSR Blender cleanup smoke test passed\n'
