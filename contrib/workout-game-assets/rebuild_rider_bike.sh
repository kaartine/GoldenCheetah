#!/usr/bin/env bash

set -euo pipefail

repository=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
candidate_relative="build/workout-game-assets/rider-bike-candidate"
candidate="$repository/$candidate_relative"
source_relative="contrib/workout-game-assets/blender/sources/WG_RiderBike.blend"
generator_relative="contrib/workout-game-assets/blender/generate_rider_bike.py"
renderer_relative="contrib/workout-game-assets/blender/render_rider_bike_audit.py"
blender_image=${GC_BLENDER_IMAGE:-goldencheetah-workout-game-blender:ubuntu24.04}
quick3d_image=${GC_QUICK3D_IMAGE:-goldencheetah-dev-quick3d:b6d4dc1}
install=false

usage() {
    printf 'Usage: %s [--install]\n' "$0"
    printf 'Builds a deterministic rider-bike candidate; --install updates GC runtime assets.\n'
}

while (($#)); do
    case "$1" in
        --install) install=true ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

exec 9>/tmp/gc-rider-bike-assets.lock
flock 9

python3 - "$candidate" <<'PY'
from pathlib import Path
import shutil
import sys

candidate = Path(sys.argv[1])
if candidate.exists():
    shutil.rmtree(candidate)
candidate.mkdir(parents=True)
PY

docker_common=(
    run --rm --network=none
    --memory="${GC_ASSET_MEMORY:-2g}"
    --memory-swap="${GC_ASSET_MEMORY_SWAP:-2500m}"
    --cpus="${GC_ASSET_CPUS:-2}"
    --user "$(id -u):$(id -g)"
    --env HOME=/tmp
    --volume "$repository:/work"
    --workdir /work
)

run_blender() {
    docker "${docker_common[@]}" "$blender_image" \
        --background --factory-startup "$@"
}

run_balsam() {
    local output=$1
    docker "${docker_common[@]}" "$quick3d_image" \
        /usr/bin/env LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen \
        /opt/Qt/6.8.3/gcc_64/bin/balsam \
        -o "/work/$output" "/work/$candidate_relative/WG_RiderBike.glb"
}

run_blender --python "/work/$generator_relative" -- \
    --source-blend "/work/$source_relative" \
    --output "/work/$candidate_relative/WG_RiderBike.glb"
run_blender --python "/work/$generator_relative" -- \
    --source-blend "/work/$source_relative" \
    --output "/work/$candidate_relative/WG_RiderBike.verify.glb"
cmp "$candidate/WG_RiderBike.glb" "$candidate/WG_RiderBike.verify.glb"

run_blender --python "/work/$renderer_relative" -- \
    --asset "/work/$candidate_relative/WG_RiderBike.glb" \
    --output-dir "/work/$candidate_relative/audit"
run_blender --python "/work/$renderer_relative" -- \
    --asset "/work/$candidate_relative/WG_RiderBike.glb" \
    --output-dir "/work/$candidate_relative/audit.verify"
for audit_file in RB-01-audit.json RB-01-front.png RB-01-rear.png \
        RB-01-side.png RB-01-chase.png; do
    cmp "$candidate/audit/$audit_file" "$candidate/audit.verify/$audit_file"
done

run_balsam "$candidate_relative/balsam"
run_balsam "$candidate_relative/balsam.verify"
diff --brief --recursive "$candidate/balsam" "$candidate/balsam.verify"

if $install; then
    python3 "$repository/contrib/workout-game-assets/install_rider_bike_asset.py" \
        --candidate-directory "$candidate"
else
    printf 'Validated rider-bike candidate: %s\n' "$candidate"
    printf 'Install it with: %s --install\n' "$0"
fi
