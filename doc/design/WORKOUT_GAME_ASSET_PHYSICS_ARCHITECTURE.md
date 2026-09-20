# ADR: Workout Game Asset And Physics Architecture

- **Status:** Accepted
- **Date:** 2026-09-20
- **Scope:** Workout Game feature assets, canonical collision profiles, course
  materialization, development reload, and persisted course compatibility

## Context

Workout Game already has the correct high-level safety boundary: trainer
control and recording are outside the game, the game consumes snapshots, and
Box2D owns only visual vehicle contact and pose. Asset and physical geometry
do not yet have one implementation-ready data boundary.

The current code has these relevant properties:

- `src/Resources/workout-game-assets.qrc` packages approved QML, meshes,
  textures, and audio. It does not package an asset catalog or physics data.
- `WorkoutGameFeatureCatalog` is a compile-time switch over
  `WorkoutGameTerrainKind`. It contains prop spacing and broad feature flags,
  not asset identity, collision data, or versions.
- `WorkoutGameFeatureGeometry` and the dedicated root, rock, skinny, climb,
  tabletop, and gap classes synthesize physical profiles from `difficulty`
  using `double`. `WorkoutGameRoadCourseBuilder::sample()` calls those profiles
  for road elevation, while `WorkoutGamePhysics` samples the road again to
  create Box2D segments.
- `WorkoutGameRoadPiece` stores terrain, difficulty, anchors, connectors, and
  challenge gates, but no resolved asset or collision-profile identity. A
  `WorkoutGameCourse` can hold an immutable `WorkoutGameRoadPlan`; the plan is
  currently generation version 2 and is limited to 4,096 pieces.
- The coordinated integration base uses course schema 6 and conversion
  algorithm 6, and is
  limited to 8 MiB. It persists the road plan, but not the exact feature
  profiles used to materialize it.
- Asset manifests permit external physics metadata. The validator enforces
  `authority: "external"`, bounded surface values, and either no collision
  proxy or a named GLB node. Runtime development loading validates that data,
  but publishes only GLB sources and material overrides.
- `WorkoutGameDevelopmentAssets` is enabled only by
  `GC_WORKOUT_GAME_ASSET_WORKSPACE`. It watches manifests and generated GLBs,
  validates them on a worker, then atomically publishes a new render-source and
  material revision to `WorkoutGameDevelopmentAsset`/`RuntimeLoader`.
- FT-02 currently has three independently expressed facts. The approved GLB
  and QML have a 0.54 m high, 0.54 m long obstacle core from local Z 0.75 m to
  1.29 m. `WorkoutGame3DFeatureAsset` repeats the 0.75/0.54/0.54 m fitting
  constants. `WorkoutGameFeatureGeometry` creates a 16-sided faceted log with
  radius `0.22 + 0.10 * difficulty` m and scales the visual asset against its
  0.54 m native size.

Consequently, an approved visual can be reloaded without changing contact,
and a C++ profile can change without an explicit asset or persisted-course
version. An active course also has no object that proves exactly which asset
physics definitions it uses.

## Decision

Production will use two immutable layers:

1. A deterministic generated asset/physics catalog, packaged in qrc and loaded
   read-only.
2. A frozen, deduplicated asset/physics snapshot owned by each materialized
   course.

Authoring manifests and GLBs remain build inputs. They are never parsed by the
production simulation. The generated catalog is the process-level lookup
source; the course snapshot is the only asset/physics source consulted after
course materialization.

### Generated qrc catalog

The asset build step will generate
`src/Resources/json/workout-game-asset-catalog.json` and add it to
`workout-game-assets.qrc` as
`:/json/workout-game-asset-catalog.json`. The generated file is committed
so normal qmake builds, packaging tests, and release archives consume the same
bytes. Generation must be byte-for-byte deterministic: UTF-8, sorted asset and
profile IDs, sorted object keys, no insignificant whitespace, decimal integers
only, and a trailing newline.

The generator will consume only validated, approved manifests and their
declared files. It will reject duplicate IDs, unapproved assets, missing file
inventory entries, unknown fields, unsupported profile kinds, and data over
the limits below. Existing license, GLB, node, socket, bounds, and budget
validation remains a prerequisite.

The current resource file contains candidate assets as a legacy exception,
including candidates instantiated by production QML. They are never admitted
to this catalog. Before catalog admission becomes the production packaging
gate, each such asset must either complete review or move to a development/test
resource. Core QML and audio without asset manifests use a narrow, explicit
resource allowlist; that allowlist cannot admit meshes, textures, or physics.

The catalog root contains:

- `schemaVersion`, initially `1`;
- `generatorVersion`, an integer that changes when canonicalization changes;
- sorted `assets` and sorted `profiles` arrays.

An asset entry owns identity and presentation binding: `assetId`, role,
packaged QML URL, native transform/bounds, optional variant keys, and a
`profileId`. A profile entry owns collision and surface behavior. Several
assets may name one profile. Catalog loading validates the complete document
before publishing it; there is no partial catalog. An unavailable or invalid
catalog removes catalog-backed features from newly generated courses. It must
not prevent a workout from starting in the standard Train view.

### Canonical integer profile contract

Catalog and course documents use integers for canonical physical data. Metres,
floating-point transforms, and Box2D values exist only at consumer boundaries.
Version 1 has this logical shape:

```text
ProfileV1 {
    profileId: ASCII string
    profileVersion: uint32
    kind: "height-offset-polyline"
    operation: "add-obstacle" | "replace-surface"
    chains: [ Chain { points: [ Point { forwardMm: int32,
                                       heightMm: int32 } ] } ]
    surface: Surface { coulombFrictionMilli: uint16,
                       restitutionMilli: uint16 }
    difficulty: DifficultyScaleV1 | null
}

DifficultyScaleV1 {
    nativeExtentMm: uint32
    baseExtentMm: uint32
    difficultyExtentMm: int32
}
```

The catalog contract is interpreted as follows. The first persisted snapshot
implementation deliberately accepts only one `add-obstacle` chain. Catalog
profiles using `replace-surface` or multiple chains remain reserved for a
later snapshot version and are rejected before playback until gap-aware road
sampling and Box2D segment emission are implemented end to end.

- `forwardMm` is course-forward distance relative to the feature's obstacle
  anchor. `heightMm` is vertical offset from the authoritative road surface.
  The authored coordinate conversion is GLB +Z to `forwardMm` and GLB +Y to
  `heightMm`; GLB +X remains lateral and is not represented by this profile.
- Each chain has at least two points. `forwardMm` is strictly increasing within
  a chain. Adjacent points are joined linearly. Chains do not join each other;
  their gaps represent absent rideable surface. Overlapping forward ranges are
  invalid in version 1. The Euclidean distance between adjacent points must be
  greater than 5 mm so the frozen profile cannot create a segment at or below
  Box2D's linear slop.
- The profile is a height function. Loops, vertical segments, overhangs,
  backtracking, dynamic bodies, and multiple heights at one forward coordinate
  are not representable and must be rejected rather than approximated.
- Interactive obstacle overlays start and end at `heightMm == 0`. Features
  such as drops that intentionally remove or offset trail use separate chains
  and explicit socket checks; they must not smuggle a gap in as a steep edge.
- `coulombFrictionMilli / 1000` and `restitutionMilli / 1000` preserve the
  manifest ranges. Longitudinal rolling resistance remains owned by
  `WorkoutGameRoadPhysicsParameters`; an asset cannot override it. A consumer
  that does not implement one of the asset contact properties must ignore it
  explicitly; it must not reinterpret it. The FT-02 pilot changes collision
  geometry only and retains the current Box2D terrain friction of 1.1.
- Course difficulty is clamped and quantized once as
  `difficultyPermille = round(clamp(difficulty, 0, 1) * 1000)`. If a difficulty
  scale is present, the resolved extent is
  `baseExtentMm + round(difficultyExtentMm * difficultyPermille / 1000)`.
  Except for the frozen FT-02 v1 adapter described below, every native point is
  scaled by the exact rational
  `resolvedExtentMm / nativeExtentMm` and rounded to the nearest millimetre,
  with half values away from zero. Scaling is complete before validation,
  canonical comparison, or deduplication.
- FT-02 v1 uses its separately versioned faceted-log evaluator to emit the
  resolved integer polyline directly. It brackets steep facet changes only
  when every final segment, after the course grade is applied and local
  rebased coordinates are converted to `float`, remains longer than Box2D's
  5 mm linear slop. The resolver adds a 6 mm flat support segment at each end.
  The world preflights every segment in an obstacle-local coordinate frame,
  excludes segments outside the active terrain window in `double` precision,
  and only then converts in-window coordinates to Box2D `float` values. A
  failed initial build or rebase permanently disables that physics instance;
  it cannot continue with destroyed Box2D bodies or joints. Failure is closed
  rather than delegated to a Box2D assertion. This evaluator is checked at
  every difficulty permille for valid final segments and is covered at
  difficulty 0, 0.5, and 1 by the 2 mm legacy parity gate.
  Production writing remains on schema 6 until legacy migration calls this
  frozen evaluator and stores its complete resolved definition.
- Runtime conversion is exactly `meters = millimeters / 1000.0`. Interpolation
  occurs between converted adjacent integer points. No consumer may refit,
  smooth, resample, or infer collision from the render mesh.

Each asset route binding stores integer render-fitting data separately from
collision: optional variant key, native forward origin relative to the obstacle
anchor, and native up/forward extent. Materialization resolves the same
rational difficulty scale for render fitting and collision. Lateral placement,
road heading, grade, and world elevation still come from the authoritative
road sample. A piece stores its obstacle anchor canonically as the nearest
millimetre plus a signed micrometre remainder in `[-500, 500]`. Consumers
reconstruct it as
`(obstacleAnchorMm * 1000 + obstacleAnchorMicrometerRemainder) / 1,000,000`
metres. Profile coordinates remain integer millimetres; the remainder prevents
the course anchor itself from moving by almost half a millimetre during
snapshot materialization.

The FT-02 pilot has one packaged visual coordinate contract: an empty variant,
`nativeForwardOriginMm == -1020`, `nativeForwardExtentMm == 540`, and
`nativeUpExtentMm == 540`. Runtime render fitting rejects any persisted FT-02
binding that claims different native geometry. With those fixed values, the
packaged mesh obstacle at native forward coordinates 0.75--1.29 m and the
procedural fallback both span the resolved collision interval symmetrically
about the micrometre-precise obstacle anchor.

### Collision-proxy conversion

A manifest collision proxy remains a build-time assertion, never runtime
physics authority. Conversion of a named GLB node to `ProfileV1` is allowed
only when all of these constraints hold:

1. The node is in the manifest node inventory and is reachable from the
   declared scene. Its complete parent transform is finite and decomposable,
   with metre units, +Y up, and +Z forward.
2. The node is static triangle geometry with applied transforms. Skins, morph
   targets, animation channels, non-triangle primitives, negative scale, and
   external buffers are rejected.
3. The proxy spans the declared rideable width and is laterally invariant
   within 1 mm after quantization. A center slice cannot be taken from an
   arbitrary 3D rock or decorative mesh.
4. The upper rideable boundary is a single-valued forward/height polyline.
   Quantization must not reverse points, merge a nonzero segment, create a
   self-intersection, close a deliberate gap, or move a socket.
5. Deterministic collinear-point removal is permitted only when its maximum
   vertical error is at most 1 mm. No other simplification, convex hull, or
   collision decomposition is permitted.
6. Entry and exit agree with declared sockets to 1 mm in forward position,
   elevation, and half-width. The generated profile and render-fit bounds must
   remain inside the manifest bounds.

Profiles not satisfying these constraints must be authored directly as the
integer polyline and checked against a visual proxy. General 3D mesh collision
is outside this ADR. Box2D continues to receive 2D segments; the adapter emits
the frozen chain vertices and may subdivide a segment for numeric stability,
but subdivision must remain exactly collinear.

`add-obstacle` profiles are emitted as separate static Box2D segments above
the ordinary road surface; they are not baked into the base terrain chain.
When feature runtime commits a successful main-line jump, it removes only the
current piece's obstacle shapes before the front wheel reaches them. The
ordinary road remains continuous underneath. An uncommitted main line retains
physical obstacle contact. This split prevents a successful jump from
receiving a pre-launch wheel impulse and keeps the pinned legacy airborne
transitions deterministic.

### Frozen deduplicated course snapshot

`WorkoutGameCourse` will gain a
`shared_ptr<const WorkoutGameCourseAssetPhysicsSnapshot>`, parallel to its
existing immutable road plan. Materialization occurs after the road plan and
challenge anchors are final and before `WorkoutGameEngine::configure()` can
publish the course.

The snapshot contains:

```text
CourseAssetPhysicsSnapshotV1 {
    snapshotVersion: uint32
    catalogSchemaVersion: uint32
    profiles: [ResolvedProfileV1]
    assets: [ResolvedAssetBindingV1]
    pieces: [PieceBindingV1]
}
```

`PieceBindingV1` is parallel to road-plan pieces and contains a profile index,
asset-binding index, integer obstacle anchor, and flags. `no profile` and
`no asset` are explicit sentinel values. A resolved profile includes all
points, chains, and effective surface values; it has no catalog pointer.

Deduplication uses the canonical bytes of the fully resolved profile, including
profile contract version, chains, points, and effective surface. Asset ID is
not part of the collision key, so physically identical assets share one
profile. Asset bindings are deduplicated separately by asset ID, variant, and
resolved native-to-course fit. Arrays preserve first-use road-piece order;
hash tables are construction details and cannot affect output order.

After publication, the snapshot and its vectors are immutable. Road sampling,
road-plan validation, Box2D terrain construction, Quick 3D placement, fallback
rendering, replay, and tests receive the snapshot or a resolved piece binding.
They do not query the process catalog, manifests, environment, filesystem, or
development service. Copying a course shares the snapshot. Reconfiguring the
engine creates a new world generation from one complete course and snapshot;
there is no mixed generation.

The road remains authoritative for course distance, base elevation, heading,
grade, sockets, challenge gates, and bypass. A profile contributes only its
declared local surface offset/gaps and surface coefficients. It cannot change
workout time, target power, trainer resistance, scoring rules, or recording.

### Ownership

| Owner | Responsibility |
| --- | --- |
| Asset manifest and source GLB | Authoring intent, provenance, file inventory, sockets, visual proxy, and source physics declaration |
| Asset validator/catalog generator | Full validation, collision conversion, integer canonicalization, sorting, and deterministic catalog output |
| qrc catalog loader | Bounded parse and all-or-nothing publication of the immutable production catalog |
| Course/road builder | Asset selection, difficulty resolution, anchor checks, and snapshot deduplication by canonical equality |
| Course document codec | Persisted snapshot encoding, limits, version dispatch, and explicit migration |
| Road sampler | Base road plus frozen profile interpolation and rideable-surface gaps |
| `WorkoutGamePhysics` | Conversion of frozen chains to static Box2D segments and vehicle contact only |
| Renderers/view models | Placement of the frozen asset binding; visual fallback when an asset cannot render |
| Development asset service | Validated render/material preview and candidate catalog publication for future sessions only |
| Train/recording code | Unchanged; never depends on catalog or asset success |

### Version and migration rule

Catalog schema, profile contract, profile content, road-plan generation,
conversion algorithm, course-document schema, and snapshot schema are separate
versions.

- Additive catalog fields that old readers can ignore require a generator
  version change. A changed meaning, required field, integer unit, rounding
  rule, or canonical encoding requires a catalog schema/profile contract version.
- Any change to a profile's points, gaps, surface values, difficulty scaling,
  sockets, or render fit requires that profile's `profileVersion` to increase,
  even if its `profileId` is unchanged.
- A change capable of altering generated road-piece validity, challenge
  placement, or persisted course output also bumps the road-plan generation
  and conversion algorithm.
- The integration commit increments the then-current road-plan generation,
  conversion algorithm, and course-document schema exactly once; it must not
  assume that the values observed while this ADR was written are still the
  integration base. Course snapshot schema starts at 1. Existing generations
  must never be reinterpreted as the new format.
- Older course documents continue through their existing decoders and
  pinned legacy profile adapter. Loading them creates an in-memory snapshot
  that reproduces the legacy C++ profiles; it does not bind them to the newest
  catalog. Only an explicit save/conversion writes the new schema.
- The new schema persists the complete deduplicated snapshot, not only catalog IDs.
  It therefore replays identically when a later application ships a changed
  catalog. Catalog, snapshot, and per-definition hashes are not persisted;
  Git identifies the source revision and bounded canonical data is sufficient
  for replay.
- Unknown newer schemas or profile contracts return `UnsupportedVersion`.
  Invalid indices, limits, or geometry return `InvalidDocument` or
  `ResourceLimit`. They never trigger best-effort regeneration.

### Hot reload boundary

Release builds use only the qrc catalog and packaged assets. They do not watch
or reload physics.

The existing development service may continue to replace GLB sources and
materials immediately because those values are presentation-only. A future
development catalog watcher may validate and atomically publish a complete
candidate catalog, but that revision is visible only to the next course
materialization or explicit session restart. It must not mutate an active
course snapshot, rebuild an active Box2D world, change a replay, or change the
physics paired with already placed visuals.

When a visual-only reload is incompatible with the active frozen render fit,
the loader rejects that visual and keeps the last good visual or packaged
fallback. Physics remains unchanged. The UI may report that a physics change
is pending restart; it must not offer an in-place physics reload.

### Resource limits

Limits are checked by the generator, catalog loader, snapshot builder, and
course decoder before allocation proportional to input:

| Resource | Limit |
| --- | ---: |
| Generated catalog bytes | 1 MiB |
| Assets / profiles in catalog | 256 / 512 |
| UTF-8 ID length | 128 bytes |
| Chains per catalog profile | 8 |
| Chains per persisted snapshot v1 definition | 1 |
| Points per profile | 256 |
| Total catalog profile points | 32,768 |
| Absolute local forward coordinate | 64,000 mm |
| Absolute local height coordinate | 16,000 mm |
| Minimum adjacent forward delta | 1 mm |
| Minimum adjacent snapshot segment length | greater than 5 mm |
| Course piece bindings | 4,096 (existing road-plan limit) |
| Unique resolved profiles per snapshot | 64 |
| Total resolved snapshot points | 4,096 |
| Unique asset bindings per snapshot | 64 |
| Encoded asset/physics snapshot | 256 KiB |

Existing 1 MiB development-manifest, 64 MiB development-GLB, per-manifest
technical budgets, and 8 MiB course-document limits remain in force. The new
encoder must fail rather than exceed the 8 MiB document limit. Generated
courses omit a catalog-backed feature when snapshot limits would be exceeded;
persisted documents over a limit are rejected.

## Rejected alternatives

### Parse manifests or GLBs in production

This adds filesystem, JSON, glTF traversal, security limits, and nondeterminism
to workout startup. It also makes packaging behavior differ from validation.
Only the generated qrc catalog crosses into production.

### Use QML or render meshes as collision authority

Render loading can fail or fall back and development visuals can reload. QML
also runs on the wrong ownership side of the runner/Box2D boundary. Collision
must remain available without Quick 3D and cannot depend on a rendered frame.

### Keep all feature profiles as hand-written C++

That preserves the current duplication between approved asset dimensions,
placement constants, and collision. Legacy adapters remain for old documents,
but new catalog-backed features use generated integer data.

### Store only catalog IDs in a course

An ID would resolve differently after a catalog update and would make replay
and persisted courses depend on the installed application version. The course
stores the resolved profile and binding.

### Copy a profile into every road piece

Courses can contain 4,096 pieces, and repeated features commonly resolve to
identical geometry. Separate canonical arrays plus indices provide immutability
without unbounded duplication.

### Store canonical geometry as JSON floating point

Float spelling, parser conversion, platform math, and tolerance-based equality
make canonical comparison, deduplication, and migration ambiguous. Millimetre integers are
sufficient for the current authored geometry and produce exact canonical
bytes.

### Apply physics hot reload to an active session

Replacing collision under a moving Box2D vehicle changes contacts, outcomes,
and replay semantics midway through a workout. Development convenience does
not justify violating the immutable-snapshot boundary.

### Support arbitrary 3D mesh collision in the first contract

The current vehicle world is a distance/elevation Box2D model. General 3D
collision would require a different physics and course representation. Version
1 deliberately accepts only bounded 2D height profiles and explicit gaps.

## Staged implementation

1. **Generator and contract.** Extend the manifest schema and both validators
   with versioned integer profile/render-fit source fields. Add deterministic
   collision-proxy conversion, canonical JSON generation, qrc packaging, and
   limit tests. Keep production consumers unchanged.
2. **Catalog loader.** Add a small read-only loader with typed values and
   all-or-nothing validation. Load it before course generation and expose no
   mutable Qt model to simulation code.
3. **Snapshot model.** Add the immutable snapshot types, deterministic resolver
   and deduplicator. Attach the snapshot to `WorkoutGameCourse` and carry it
   through road materialization, engine configuration, worker copies, and view
   models without changing physics behavior.
4. **FT-02 parity pilot.** Generate and consume only the log-over profile and
   render fit through the new path. Retain the legacy implementation behind a
   test-only A/B adapter. Do not migrate another feature until the parity gates
   below pass.
5. **Persisted format.** Increment road-plan generation and course schema from
   their values at integration time; keep the conversion algorithm unchanged
   for a persistence-only change. Add snapshot encoding/decoding, limits, and
   legacy adapters. Add explicit
   conversion tests before making the new schema the writer default.
6. **Consumer convergence.** Make road sampling, Box2D segment creation,
   feature placement, Quick 3D, and fallback renderers consume resolved
   bindings. Remove FT-02 constants from `WorkoutGameFeatureGeometry` and
   `WorkoutGame3DFeatureAsset` only after no production call site uses them.
7. **Development boundary.** Add candidate-catalog watching if needed, scoped
   to new materializations. Preserve current immediate visual/material reload
   and last-good fallback.
8. **Further migration.** Move one profile family at a time. Bump individual
   profile versions for content changes and remove a legacy adapter only when
   no supported course schema can reference it.

## Tests

The implementation is not complete without the following automated coverage:

- Generator golden test: two clean generations produce identical bytes;
  asset/profile/key ordering is stable; qrc contains the exact file.
- Generator and loader negative tests for every collision constraint, unknown
  versions/fields, duplicate IDs, integer overflow, invalid UTF-8,
  non-monotonic points, merged gaps, and every resource limit.
- Integer contract tests for difficulty quantization, signed half-away-from-zero
  rounding, rational scaling, interpolation, chain gaps, and metre conversion.
- Snapshot tests proving deterministic first-use ordering, collision and asset
  deduplication, stable canonical encoding, valid sentinel/index handling, immutable shared
  ownership, and rejection before oversized allocation.
- Course codec round trips for the new schema and golden decode/migration tests
  for every older supported schema. Unknown future versions and tampered snapshots must fail with
  the specified status and must not regenerate from the installed catalog.
- Ownership test that changes or removes the process catalog after engine
  configuration and proves road samples, Box2D snapshots, render bindings, and
  replay remain unchanged.
- Hot reload tests proving visual/material revisions can replace a render
  source, a candidate physics revision affects only a newly materialized
  course, and an active session never recreates its world.
- Renderer tests for packaged fallback, missing development visual, correct
  sockets/anchor/scale, and equivalent placement in Quick 3D and fallback
  renderers.
- End-to-end safety tests proving catalog/profile failure falls back to a
  functional standard Train workout and never changes trainer command or
  recording cadence.

## FT-02 first parity pilot

FT-02 is the first and only feature enabled in the initial catalog-backed
release. Its generated native profile is the upper half of the existing
16-segment faceted log:

- native extent: 540 mm;
- thirteen ordered upper-boundary points from forward -276 mm through the apex
  to +276 mm. The first and last 6 mm segments are flat supports around the
  -270 mm through +270 mm legacy core. The frozen v1 quantizer brackets the two
  steep outer facet changes with integer points and emits one point for each
  remaining facet, keeping every final Box2D segment above linear slop while
  preserving the legacy 16-segment surface within the parity gate;
- native render origin: -1,020 mm relative to the obstacle anchor, preserving
  the current 0.75 m dead zone plus 0.27 m half core;
- native visual/core height and forward extent: 540 mm;
- difficulty extent: `440 + round(200 * difficultyPermille / 1000)` mm,
  preserving the current 0.44-0.64 m range and the exact 0.54 m midpoint;
- packaged visual: `qrc:/qml/assets/Wg_LogOver_Greybox.qml`, unchanged during
  the pilot;
- obstacle anchor, challenge decision, bypass, launch request, scoring, and
  road selection: unchanged.

The pilot parity gates are:

1. At difficulty 0, 0.5, and 1, and at every native facet boundary plus 1 mm on
   either side, new surface offsets differ from the legacy faceted-log profile
   by no more than 2 mm. The core endpoints at -270 mm and +270 mm remain
   exactly zero, the outer support endpoints are -276 mm and +276 mm, and the
   midpoint extent is exactly 540 mm.
2. For deterministic FT-02 courses at the existing test speeds, legacy and new
   paths select the same piece, route, challenge outcome, jump request, and
   feature action ID. Box2D grounded/airborne transitions occur within one
   8.33 ms physics step and rider elevation within 5 mm.
3. Asset placement preserves the current world anchor, yaw, pitch, and uniform
   `resolvedExtent / 540` Y/Z scale. Socket positions differ by no more than
   1 mm and packaged/development visual fallback behavior is unchanged.
4. A repeated FT-02 course stores one resolved profile for each distinct
   quantized difficulty, references it by index from every matching piece, and
   produces byte-identical canonical snapshot data across repeated builds.
5. Loading a legacy course continues to use the pinned FT-02 adapter; loading
   a new-schema pilot course uses its embedded snapshot even when the installed
   catalog's FT-02 profile version is newer.

Only after these gates pass may the test-only A/B switch be removed and another
feature migrate. Any intentional FT-02 behavior change is a separate tuning
change with a profile-version bump; it is not part of this architecture pilot.
