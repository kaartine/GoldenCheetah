# Workout Game Blender Asset Sources

This directory contains deterministic Blender source generators for custom
Workout Game assets. `generate_tabletop.py` creates the first stylized
low-poly tabletop greybox without downloading or embedding external assets.

## Tabletop contract

- Canonical units are metres, with `+Y` up and `+Z` forward in the exported
  glTF scene.
- Ordinary socket half-width is exactly `0.68 m` (`1.36 m` total).
- Entry and exit dead zones are `0.75 m` and remain flat.
- The core is `0.446 m` high, with a `1.87 m` takeoff, `1.10 m` flat deck and
  `1.87 m` landing.
- The joined mesh includes the trail, four-metre side terrain and dark outer,
  front and rear skirts. It uses three opaque greybox materials and no texture.
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
also reports the expected greybox topology of `76` vertices and `122` triangles.
No `.blend`, texture, runtime collision or physics file is produced.

Determinism is expected for the same Blender 4.x patch release and export
settings. Different Blender exporter versions may serialize equivalent GLB
data differently, so cross-version byte hashes are not promised.
