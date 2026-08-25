# Workout Game Blender Asset Sources

This directory contains deterministic Blender source generators for custom
Workout Game assets. `generate_tabletop.py` creates the first stylized
low-poly tabletop socket tile without downloading or embedding external assets.

## Tabletop contract

- Canonical units are metres, with `+Y` up and `+Z` forward in the exported
  glTF scene.
- Ordinary socket half-width is exactly `0.68 m` (`1.36 m` total).
- Entry and exit dead zones are `0.75 m` and remain flat.
- The core is `0.446 m` high, with a `1.87 m` takeoff, `1.10 m` flat deck and
  `1.87 m` landing.
- The joined mesh includes the trail, tapered side terrain, a raised safe
  bypass and dark outer, front and rear skirts. Side terrain narrows at the
  socket ends so the tile joins the streamed floor without a rectangular lip.
  It uses four opaque greybox materials and no texture.
- `SOCKET_IN`, `SOCKET_OUT`, `MARKER_PREPARE`, `MARKER_DECISION`,
  `MARKER_ACTION`, `MARKER_LIP`, `MARKER_APEX` and `MARKER_LAND` are exported
  as named nodes with custom metadata.
- Marker positions are visual authoring references. Workout Game course,
  trainer, outcome and physics data remain authoritative outside the GLB.

The script clears the Blender scene, creates deterministic geometry and names,
checks dimensions, socket seams, profile continuity, finite coordinates,
required nodes, material order and applied mesh transforms, then exports a
GLB. A failed check exits nonzero.

## Native headless invocation

Use an installed Blender 4.x executable:

```bash
blender --background --factory-startup --python-exit-code 1 \
  --python contrib/workout-game-assets/blender/generate_tabletop.py -- \
  --output "$PWD/build/workout-game-assets/WG_Tabletop_Greybox.glb"
```

The `--` separator is required because arguments after it belong to the
generator rather than Blender.

## Docker invocation

Build the repository-owned image. Package installation happens only inside the
image and does not modify the host:

```bash
docker build -t goldencheetah-workout-game-blender:ubuntu24.04 \
  contrib/workout-game-assets/blender

docker run --rm --user "$(id -u):$(id -g)" \
  -v "$PWD:/work" -w /work \
  goldencheetah-workout-game-blender:ubuntu24.04 \
  --background --factory-startup --python-exit-code 1 \
  --python contrib/workout-game-assets/blender/generate_tabletop.py -- \
  --output /work/build/workout-game-assets/WG_Tabletop_Greybox.glb
```

The expected output is one self-contained
`build/workout-game-assets/WG_Tabletop_Greybox.glb`. Blender's console output
also reports the expected topology of `108` vertices and `156` triangles.
No `.blend`, texture, runtime collision or physics file is produced.

Determinism is expected for the same Blender 4.x patch release and export
settings. Different Blender exporter versions may serialize equivalent GLB
data differently, so cross-version byte hashes are not promised.

## Validation and runtime conversion

The reviewed GLB is committed under `contrib/workout-game-assets/generated`.
Validate its manifest, provenance, hashes, structure, sockets and budgets with:

```bash
python3 contrib/workout-game-assets/validate_assets.py --root "$PWD"
```

Qt Balsam 6.8.3 converts the GLB offline. The reviewed output is committed in
`src/Train/qml/assets` and packaged by
`src/Resources/workout-game-assets.qrc`. Runtime code must load this built-in
resource; it must not load the authored GLB or downloaded content dynamically.

```bash
QT_QPA_PLATFORM=offscreen balsam \
  -o build/workout-game-assets/balsam \
  contrib/workout-game-assets/generated/WG_Tabletop_Greybox.glb
```

For this toolchain, repeated Balsam runs must match the manifest hashes. The
`Build/workoutGameAssets` tests cover policy failures, and the
`Train/workoutGame3DView` tests load and render the production qrc output.
