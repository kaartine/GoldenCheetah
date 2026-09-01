# Workout Game Blender Asset Sources

This directory contains deterministic Blender source generators for custom
Workout Game assets. `generate_tabletop.py` creates the first stylized
low-poly tabletop socket tile, `generate_log_over.py` creates a socketed,
partly buried log-over tile, `generate_bunny_hop.py` creates a compact
practice hurdle, `generate_drop.py` creates a faceted drop face, and
`generate_rider_bike.py` creates the articulated low-poly rider and 29er MTB
mesh set, and `generate_conifer_set.py` creates three low-poly forest
silhouettes. `generate_distant_ridges.py` creates the bounded distant-terrain
ring. None of the generators downloads or embeds external assets.

## Distant-ridge contract

- Five closed radial bands cover 42 to 240 metres around the rider with a
  deterministic, uneven forest horizon and no camera-direction dead zone.
- The runtime keeps the ring centred on the rider and 1.2 metres below the
  sampled ground. It is presentation-only and cannot affect course distance,
  road elevation, Box2D contacts, trainer control or recording.
- One UV-mapped mesh, one shared forest material, 256 triangles and one draw
  call provide distant relief. The asset contains no external model or texture.
- Restrained depth fog begins at 68 metres and reaches the scene colour at 260
  metres. Near trail and feature cues therefore remain outside the fog band.
- Blender validates the exact node inventory, finite triangular topology,
  applied transforms, UV map, 240-metre radius and 300-triangle budget before
  export.

## Conifer contract

- One tapered trunk and three crown meshes provide narrow, layered and
  broken-top silhouettes. Runtime selects one crown per tree instance.
- Every source mesh starts at or above `Y = 0`, and `PIVOT_BASE` is exactly at
  the origin. Runtime terrain sampling remains the sole base-height authority.
- The complete set uses 192 triangles and three opaque source materials. It
  contains no external texture, model or built-in Quick 3D primitive.
- Blender checks the node inventory, finite triangle topology, applied
  transforms and 320-triangle budget before export.

## Rider-bike contract

- The bicycle has a `0.755 m` outside wheel diameter, `1.313 m` wheelbase,
  `0.455 m` chainstay and separately articulated main frame, swingarm, fork and
  rear shock. These dimensions use Pole Voima K2's public geometry table as a
  reference without copying branded surfaces, graphics or source assets.
- The project-authored side silhouette uses a deep integrated 750 Wh-class
  battery/down tube, compact mid-drive housing, long twin-beam swingarm, upper
  linkage and rocker, long single-crown fork, hubs, brake rotors and sparse
  low-poly spokes. These are generic visual cues rather than copied Pole
  surfaces or CAD. The independently authored gold frame, black motor and
  component group preserve the Voima K2's readable packaging at the normal
  game-camera distance without reproducing logos or proprietary surfaces.
- The `29 x 2.5` black tires have a `0.0635 m` casing width and distinct
  low-poly front-grip and rear-braking tread roles. They are based on the
  publicly listed Maxxis Assegai and Minion DHR II fitment but contain no
  copied tread mesh, sidewall text or trademark artwork.
- The stylized fictional rider has a generic low-poly face, dark beard,
  wraparound eyewear, blue-white riding clothes and a clearly separate
  white-black open-face enduro helmet with a dark visor. The model is not
  authored as a likeness of a named person and contains no portrait texture,
  face scan or source photograph pixels.
- Rear axle, front axle, crank, steering, pelvis, camera-target and shadow
  pivots are named in the GLB. The runtime QML uses those same measured values.
- Main frame, components, swingarm, fork, rear shock, wheels, crank, torso,
  jersey accents, head, beard, eyewear, helmet shell, helmet accents, reusable
  limb and contact-shadow meshes are separate. Runtime wheel rotation
  follows distance, suspension follows the physics snapshot, and crank and leg
  motion follow the authoritative pedal-cycle value.
- The crank mesh includes both pedal platforms, so feet, crank arms and pedals
  share one authoritative phase without adding per-pedal draw calls.
- The complete source asset has 2,836 triangles, eight opaque flat-color materials,
  no texture payload and no external source. The runtime component adds only a
  bounded translucent contact-shadow material.
- Blender validates topology, applied transforms, dimensions, pivots and the
  3,600-triangle budget before export. Asset-policy tests additionally reject
  built-in Quick 3D primitives in the final rider component.

## Drop contract

- The `24.0 m` metadata tile matches the runtime profile from `-10.0 m` to
  `+14.0 m`, with the physical lip at local `10.0 m`.
- The asset contributes only a narrow, faceted rock face below the upper lip.
  The streamed socket trail owns the approach, actual gap, lower landing,
  recovery and safe branch, so no duplicate ground can overlap or bridge the
  drop.
- The source face is `0.70 m` deep and scales only vertically to the
  difficulty-dependent `0.60-1.00 m` runtime depth. Its rough edge extends
  beyond both sides of the `1.36 m` tread.
- Prepare, decision, action, lip, air, land and recovery markers are visual
  metadata. Box2D remains the sole source of rider flight, pitch and landing
  impact.
- The generator validates exact sockets, topology, finite coordinates, two
  opaque materials, the lip envelope and applied transforms before export.

## Bunny-hop contract

- The `3.58 m` visual tile uses exact `0.68 m` ordinary-trail sockets and a
  self-contained project-authored obstacle. It does not duplicate trail or
  terrain geometry.
- The crossbar is `0.20 m` high and extends to `X = +/-1.02 m`; its supports
  remain outside the `1.36 m` tread. Runtime difficulty scales height from
  `0.10 m` to `0.20 m` without changing the socket length.
- Named prepare, decision, action, preload, takeoff, apex and landing markers
  are visual authoring metadata. The road gate, Box2D impulse and outcome
  remain authoritative runtime data.
- The ordinary trail remains flat under the hurdle. A completed line uses a
  bounded feature-specific lift, while the common safe branch stays grounded
  and rejoins the main trail continuously.
- The generator validates topology, finite coordinates, two opaque materials,
  socket placement, marker inventory and applied transforms before export.

## Log-over contract

- Canonical units, axes and the exact `0.68 m` socket half-width match the
  tabletop contract.
- The `2.04 m` tile has `0.75 m` flat entry and exit dead zones around a
  `0.54 m` long physical core.
- The visible log reaches `0.54 m` above the tread, extends past both trail
  edges and has a small buried lower hull. Its upper 16-segment chords use the
  same normalized profile as `WorkoutGameFeatureGeometry`.
- The continuous runtime trail and forest floor remain the only ground meshes.
  The asset contributes only the obstacle; shared runtime branch geometry
  follows the canonical bypass curve and terrain surface. This avoids
  coplanar trail geometry, floating bypasses and broad terrain wedges.
- Exact socket nodes plus named prepare, decision, action, apex and land
  markers remain visual metadata. The script validates topology, finite
  coordinates, socket placement, profile continuity, buried volume, material
  order and applied transforms before exporting.

## Tabletop contract

- Canonical units are metres, with `+Y` up and `+Z` forward in the exported
  glTF scene.
- Ordinary socket half-width is exactly `0.68 m` (`1.36 m` total).
- Entry and exit dead zones are `0.75 m` and remain flat.
- The core is `0.446 m` high, with a `1.87 m` takeoff, `1.10 m` flat deck and
  `1.87 m` landing.
- The joined mesh includes the trail, tapered side terrain and a raised safe
  bypass. Side terrain narrows at the
  socket ends so the tile joins the streamed floor without a rectangular lip.
  The continuous streamed forest floor closes the underside in production,
  avoiding a visible perimeter skirt. The tile uses three opaque greybox
  materials and no texture.
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

The expected output is one self-contained GLB at the requested path. Blender's
console output reports the expected topology for the selected generator.
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
