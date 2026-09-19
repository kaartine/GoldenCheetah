# Optional TripoSR candidate workflow

This directory defines an offline, review-only boundary between TripoSR and
GoldenCheetah's authored Workout Game assets. It does not install TripoSR,
download model weights, invoke inference, edit `RB-01`, run Balsam or change
runtime QML. A validated candidate is still untrusted input for a separate
Blender cleanup, topology, rigging, licensing and visual-review process.

## Repository boundary

- Keep the TripoSR checkout, Python environment, model cache and weights
  outside the GoldenCheetah repository.
- Keep every source/reference image outside the repository, including images
  downloaded from product or photography sites. Record only its SHA-256,
  media type, rights basis and optional public HTTPS source URI.
- Keep generated GLBs and candidate directories outside the repository. The
  importer rejects inputs and destinations that resolve inside this checkout.
- Never commit model weights, copyrighted reference images, raw TripoSR GLBs
  or candidate output. The local `.gitignore` is defence in depth; the Python
  boundary is authoritative.
- Do not treat a public image as permission to redistribute either that image
  or a derivative model. Use `unverified-review-only` until rights are reviewed.

## Reproducible inference record

Run TripoSR from an external, clean checkout pinned to a full commit hash. Pin
the Python dependency lock/container separately and retain its digest in the
private work record. Use explicit inference values, for example:

```bash
cd ~/Documents/personal/triposr-workbench/TripoSR
git status --porcelain
git rev-parse HEAD
python run.py /private/reference.png \
  --output-dir /private/triposr-output \
  --mc-resolution 192 \
  --chunk-size 4096
```

TripoSR and its CLI evolve independently, so verify the pinned revision's
supported arguments. Hash the exact reference, weight file and output before
importing:

```bash
sha256sum /private/reference.png /private/model.ckpt \
  /private/triposr-output/0/mesh.glb /private/lod/candidate-lod1.glb
```

The importer requires those expected hashes instead of silently trusting the
files. The seed and inference settings are recorded for auditability; exact GPU
floating-point output can still vary across CUDA, PyTorch and hardware builds.

## Import and verification

Create a quarantined candidate outside the repository:

```bash
python3 contrib/workout-game-assets/triposr/triposr_candidate.py import \
  --candidate-id bike-side-001 \
  --target-asset-id RB-01 \
  --target-role rider-bike \
  --reference /private/reference.png \
  --reference-sha256 REF_SHA256 \
  --reference-rights unverified-review-only \
  --source-uri https://example.invalid/original-reference \
  --lod0-glb /private/triposr-output/0/mesh.glb \
  --lod0-glb-sha256 LOD0_GLB_SHA256 \
  --lod1-glb /private/lod/candidate-lod1.glb \
  --lod1-glb-sha256 LOD1_GLB_SHA256 \
  --collision-proxy-glb /private/lod/candidate-collision.glb \
  --collision-proxy-glb-sha256 COLLISION_GLB_SHA256 \
  --collision-proxy-generator "Blender 4.0.2" \
  --candidate-dir /private/candidates/bike-side-001 \
  --triposr-revision FULL_40_CHARACTER_COMMIT \
  --model-id stabilityai/TripoSR \
  --model-weights /private/model.ckpt \
  --model-weights-sha256 WEIGHTS_SHA256 \
  --model-license MIT \
  --seed 0 --mc-resolution 192 --chunk-size 4096 \
  --foreground-removal \
  --unit-meters 1.0 --up-axis +Y --forward-axis +Z \
  --max-triangles-lod0 18000 --max-triangles-lod1 9000 \
  --max-materials 16 \
  --max-textures 0 --max-texture-bytes 0
```

This emits only `candidate-lod0.glb`, `candidate-lod1.glb`, optional
`candidate-collision.glb` and `candidate.json` in the external candidate
directory. The LOD budgets remain
configurable, with defaults of 18,000 triangles for LOD0 and
9,000 for LOD1. LOD1 is mandatory and must have strictly fewer triangles than
LOD0. The importer does not create the LOD; use an external Blender decimation
and cleanup step, then supply and hash both results. `candidate.json` follows
`candidate-descriptor.schema.json`. It has no local paths or timestamps, so
identical inputs and declarations produce identical descriptor bytes.

The collision proxy is optional. If used, its GLB, SHA-256 and generator CLI
arguments are all mandatory. It is a separate, embedded GLB with at most 100
triangles and 4 MiB,
`POSITION` geometry only, identity transforms, and no materials, textures or
other render payload. Its descriptor provenance records the collision generator,
exact proxy hash and exact source LOD0 hash. It is review metadata only and is
not installed.

Independently recheck a candidate before opening it in Blender:

```bash
python3 contrib/workout-game-assets/triposr/triposr_candidate.py verify \
  /private/candidates/bike-side-001
```

Validation covers the reference and GLB hashes, GLB 2.0 chunk and embedded
buffer structure, finite accessors, identity transforms, nonempty indexed or
non-indexed triangle geometry, every attribute and index reference, scene
reachability, declared units/orthogonal axes, and hard byte, triangle and
material budgets. Inputs are read once into bounded snapshots; those exact
validated bytes are written to a private staging directory and atomically
renamed into place. The candidate directory must contain exactly the declared
files.

The safe profile rejects textures, external URIs, unknown extensions, cameras,
animations, skins, morph targets, mesh instancing, non-identity transforms,
unreferenced data and unknown object fields. Only `POSITION`, `NORMAL` and
vertex `COLOR_0` attributes are accepted for render candidates. The known
TripoSR/trimesh `extras: {"processed": true}` marker is the only permitted
`extras` value. Hard limits cannot be raised by CLI arguments or a candidate
descriptor: render GLBs are at most 64 MiB, LOD0 18,000 triangles, LOD1 9,000
triangles, 16 materials, 128 accessors/primitives/nodes and zero textures.
Reference images are capped at 64 MiB and the hashed model-weight file at
2 GiB.

`source-uri` is either `local-private` or HTTPS without user information,
query parameters or fragments. This prevents credentials and access tokens
from entering `candidate.json`.

## Promotion boundary

Candidate acceptance is intentionally outside this tool. A human must review
source rights, shape and texture provenance, visual quality, topology, UVs,
scale, axes, materials, LODs, sockets, pivots, rigging and performance. Any
accepted geometry must then go through the existing authored Blender source,
asset manifest, fixed-view audit and runtime tests in a separate change.

The descriptor always contains:

```text
status: review-required
installation.automatic: false
installation.targetAssetId: EN-10-mossy-log
installation.targetRole: prop
installation.decision: prohibited-pending-human-review
```

`import` requires `--target-asset-id` and `--target-role`. These identify the
intended canonical asset without installing or replacing it. Descriptor v2 is
generic; the verifier continues to accept existing v1 `RB-01` review bundles.

There is deliberately no `--install` option and no import of
`install_rider_bike_asset.py`.

Run focused tests with:

```bash
python3 -m unittest discover \
  -s contrib/workout-game-assets/triposr/tests -v
```
