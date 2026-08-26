# Workout Game 3D Release Checklist

## Purpose

This is the master delivery checklist derived from
`WORKOUT_GAME_3D_AB_REVIEW.md`. The object-level sourcing and production work
is expanded separately in `WORKOUT_GAME_3D_ASSET_BACKLOG.md`.

Every task has an implementation requirement, an automated verification, and
a visible acceptance condition. A visually plausible screenshot alone cannot
complete a physics or recording task, and a unit test alone cannot complete a
visual-readability task.

## Completed Audit Foundation

- [x] `AUD-01` Capture matching legacy and Qt Quick 3D catalogs for all eleven
  features at 1280 by 720 and a fixed ten-metre approach.
- [x] `AUD-02` Capture isolated-data legacy and 3D application sessions plus a
  side-by-side comparison video.
- [x] `AUD-03` Provide a deterministic 72-second course containing all eleven
  features with completed and bypassed scenarios.
- [x] `AUD-04` Preserve normal trail half-width at 0.68 m with a geometry
  regression test.
- [x] `AUD-05` Document feature-by-feature graphics, physics, animation, HUD,
  feel, performance and release findings.

## P0 Renderer And Camera

### `CAM-01` Select the fixed chase composition

- [x] Implement low-centre, medium-centre and slight-shoulder audit modes.
- [x] Render matched still images from the same rider and course state.
- [x] Record matched motion videos with terrain turns, climb, tabletop and drop.
- [ ] Select the baseline with user review; `medium-centre` is provisional.

**Implementation:** camera position and target use the authoritative road
sample. The ordinary default has no large lateral offset. Keep all candidates
available only until selection.

**Tests:** verify composition parameters, nonblank captures, visually distinct
catalog images, monotonic camera movement and bounded camera-to-rider distance.

**Done when:** the rider aligns with the trail, the next 25-40 m and elevation
profile are readable, and the view never makes normal forward riding appear
sideways.

### `CAM-02` Prevent camera and scenery intersections

- [x] Define a camera exclusion corridor and near-camera prop bounds.
- [x] Prevent terrain from crossing the near plane.
- [x] Fade, replace or reject a tree that would block rider, cue or feature.
- [x] Smooth camera position/target without adding trainer or recording work.

**Tests:** deterministic course sweep records the maximum occluded trail/rider
area, near-plane intersections, camera displacement and yaw acceleration.

**Done when:** no catalog/video frame is substantially blocked by vegetation,
terrain or a late-streamed floor chunk.

### `RND-01` Keep rendering independent and bounded

- [x] Confirm immutable newest-frame publication remains capacity one.
- [x] Keep GLB/mesh generation, validation and loading out of the frame loop.
- [x] Bound resident terrain, visible props, particles, triangles and draw calls.
- [x] Preserve opaque depth-correct geometry and eliminate background holes.

**Tests:** runner/thread tests, frame-work counters, scene bounds, pixel checks,
memory plateau and target-laptop frame-time telemetry.

**Done when:** rendering cannot delay trainer control or recording and cannot
form an unbounded work queue.

**Current evidence:** `WorkoutGameRunner` still publishes immutable simulation
frames through its single latest-value slot. Runtime floor, roots, climb, rock
garden, rock slab and skinny mesh calculations now run in one lower-priority
`WorkoutGame3DChunkBuilder` thread. Both its pending-request and completed-result
mailboxes have capacity one: newer distance buckets overwrite stale work and a
course generation prevents retired results from being installed. The worker
produces plain immutable byte buffers; only the GUI thread updates the inactive
Quick 3D geometry and atomically exposes it through the existing double buffer.
Its queued GUI wakeup is separately coalesced to one callback.

Resident terrain remains limited to 15 m behind and 130 m ahead. Visible trees
are capped at ten, particles remain absent, and the ViewModel publishes the
actual custom-mesh triangle count. An opt-in Qt Quick 3D extended-statistics
test renders the combined asset/technical-terrain budget on real X11/OpenGL and
rejects more than 30,000 custom triangles or 50 actual draw calls. Capacity,
replacement, cancellation and shutdown pass eight focused tests normally,
under ASan/UBSan and under TSan. The unchanged geometry contract passes all 27
tests normally and under ASan/UBSan. The complete Quick 3D suite retains the
pixel-level clear-color hole checks.

## P0 Asset Pipeline

### `PIPE-01` Approve asset rights and provenance

- [x] Complete allow/conditional/reject license policy.
- [x] Record exact source, author, license/version, attribution and hashes.
- [x] Reject NC, ND, editorial, personal-use, marketplace-only and unclear use.
- [x] Record all modification and conversion steps.

**Tests:** validate every manifest against
`workout_game_asset_manifest.schema.json`; fail packaging for missing,
rejected, unhashed or unreviewed runtime assets.

**Done when:** every AppImage asset can be legally modified and redistributed
and its original source can be reproduced.

### `PIPE-02` Define and validate the GLB contract

- [x] Lock metre scale, +Y up, +Z forward and applied transforms.
- [x] Lock `SOCKET_IN`/`SOCKET_OUT`, pivot, marker, LOD and proxy node names.
- [x] Validate bounds, triangle/material/texture budgets and animation clips.
- [x] Use trusted built-in assets only; do not runtime-load downloaded content.
- [x] Convert accepted GLB files offline to optimized Qt Quick 3D assets.

**Tests:** malformed/untrusted fixture rejection, exact socket comparison,
required-node checks, animation list, budget limits and qrc/AppImage load test.

**Done when:** a fresh build reproducibly converts, packages and renders one
reviewed source asset without a developer machine path.

### `ASSET-01` Produce the object set

- [x] Complete the rider/bike, trail, terrain, feature, environment, effects,
  collision, camera and LOD inventory.
- [x] Research reusable models and document license decisions.
- [x] Select project-authored models instead of unsuitable external adaptations
  and preserve their provenance.
- [x] Custom-author every gameplay-defining socket, silhouette and pivot.

**Tests:** deterministic catalog, asset-manifest validation, Blender/export
reproducibility and AppImage packaging.

**Done when:** the vertical slice uses no built-in cube, cone, cylinder or
sphere as a final visible rider, feature or hero-environment object.

### Current pipeline evidence

- [x] A repository-owned Ubuntu 24.04/Blender 4.0.2 Docker image generates the
  first project-authored tabletop greybox without external assets.
- [x] The generator checks the measured profile, exact socket seams, node and
  material names, finite coordinates, transforms and topology before export.
- [x] The current 9,856-byte GLB has 78 vertices and 96 triangles; Khronos
  glTF Validator 2.0.0-dev.3.10 reports zero errors, warnings, infos and hints.
- [x] Two clean Blender invocations produce the same GLB SHA-256.
- [x] Validate the manifest, file hashes, GLB policy and malformed fixtures in
  the standard cross-platform unit-test inventory.
- [x] Two Qt Balsam 6.8.3 conversions produce byte-identical QML and `.mesh`
  files; the production qrc loads and renders them in a real X11/OpenGL test.
- [x] A clean Ubuntu 22.04/glibc 2.35 release build reproduced the AppImage
  byte for byte, passed the packaged offscreen event-loop smoke, and exposed
  both generated tabletop QML and mesh resource names in the extracted ELF.
- [x] An opt-in X11/Quick 3D motion test captured the three camera candidates
  over the same deterministic 102 m course. Each capture contains 360 frames
  at 960 by 540 and the test rejects blank output or fewer than 90 percent
  visibly changing frame transitions. The encoded 30 FPS videos are exactly
  12 seconds long; `medium-centre` remains the provisional baseline pending
  user review.
- [x] Camera position and target now sample the authoritative road behind and
  ahead of the rider instead of projecting both from one instantaneous yaw.
  The existing presentation smoother remains the single timing authority.
  A deterministic sweep bounds height, displacement, yaw step and yaw
  acceleration, while crown-aware placement rejects trees intersecting the
  camera-to-cue corridor. The complete X11/OpenGL suite passed 13 tests with
  no failures after the change.
- [x] The project-authored log-over candidate is reproducible as a 5,764-byte,
  64-triangle, two-material GLB. Two Blender 4.0.2 and two Balsam 6.8.3 runs
  are byte-identical, Khronos validation reports no issues, and the manifest
  records the exact source and runtime hashes. Its GLB contains only the
  obstacle; common runtime trail, forest floor and branch geometry remain the
  sole ground meshes.

## P0 Vertical Slice

### `VS-01` Build canonical ordinary trail and terrain

- [x] Author normal trail, shoulders, forest-floor tile and distant terrain.
- [x] Preserve the 1.36 m full trail width and exact socket dead zones.
- [x] Add material separation without photorealistic PBR noise.
- [x] Keep the uneven forest horizon and stronger visible terrain relief.

**Tests:** seam vertices, normals, material coverage, camera sweep, background
hole check and trail-width invariant.

**Done when:** tiles form a continuous singletrack embedded in terrain rather
than a ribbon pasted onto a flat floor.

**Current evidence:** `WorkoutGame3DTerrainProfile` now generates a
deterministic eight-vertex cross-section from global course distance and seed.
Its exact 0.68 m trail-edge sockets, shoulder and forest material bands, seven
indexed strips, unit cross-slope normals and chunk-edge continuity are covered
by 21 focused tests. Trees interpolate their base height from the same profile
instead of the former flat base elevation. The complete X11/OpenGL suite passes
59 tests with 12 explicit opt-in artifact exporters skipped, and selected
geometry, scene-contract, render-budget and production-window tests pass with
confirmed ASan/UBSan linkage. A 1280 by 720 capture shows the ordinary trail
embedded in raised near terrain without background holes.
Project-authored `EN-03` adds one rider-centred 42-to-240-metre radial mesh with
five deterministic relief bands, 256 triangles and one material/draw call.
Restrained depth fog starts beyond the near trail at 68 metres and reaches the
scene colour at 260 metres. Real X11/OpenGL still and 72-frame motion review
show an uneven continuous horizon, preserved path readability and no exposed
rider-relative seam. The completed motion artifact SHA-256 is
`eaa6b900edcd0aa9818aa862596e86728e09846ab57b6b4bbe08cb9df971e9da`.

### `VS-02` Build complete tabletop and bypass

- [x] Model belly, lip, deck, knuckle, landing and joined side terrain.
- [x] Model a readable split at the decision and a merge after the feature.
- [x] Keep the canonical measured profile as the physics authority.
- [x] Place prepare, decision, action, lip, apex and landing markers.

**Tests:** exact sockets, named anatomy silhouette, road/contact alignment,
completed/bypass captures and bounded speed-dependent flight.

**Done when:** a rider identifies the jump and both lines without HUD text and
the wheels meet the landing surface at the reported landing event.

**Current evidence:** production now uses one project-authored procedural
tabletop surface shared by road generation, Box2D contact, legacy comparison
geometry and Quick 3D. At catalog difficulty 0.70 it is 0.65 m high, has an
18-degree take-off, a 2.49 m deck, exact 0.68 m half-width sockets and bounded
eased entry and landing transitions. The preparation window starts eight
metres before the decision. The safe branch starts at that decision, clears
the complete mound, stays grounded and merges over a minimum 28 m branch.
Explicit sections shorter than the 36 m preparation-plus-branch contract
preserve their authored distance and omit the challenge instead of clipping a
jump into a socket.

The former deterministic Blender/Balsam greybox remains a reproducible audit
artifact, but is no longer placed in the production scene; this removes its
duplicate mound and incompatible bypass from the rendered and physical trail.
Matched 72-frame X11/OpenGL captures show the completed rider leaving the
ground with a ground-fixed shadow and landing on the visible surface, while
the bypass rider remains grounded. The completed and bypassed 960 by 540,
15 FPS, 4.8-second videos are stored in
`$HOME/Documents/personal/gc-ui-ftr11-final-v2` on the build host.

### `VS-03` Integrate rider and bike

- [x] Select/adapt or author a low-poly 29-inch MTB and articulated rider.
- [x] Correct forward orientation, wheelbase, axle, crank and steering pivots.
- [x] Add high-contrast pixel-textured materials and a ground-fixed shadow.
- [x] Add pedal, coast, preload, air, land, absorb, lean and bypass clips.

**Tests:** node/pivot validation, cadence-to-crank synchronization, wheel
contact, no root motion, animation bounds and catalog/video captures.

**Done when:** pedalling, take-off, air and landing are readable at the normal
camera distance and the bicycle never appears to travel sideways.

**Current evidence:** the project-authored `RB-01` source produces separate
frame, 29-inch wheels, crank, torso, head, helmet, limb and contact-shadow
meshes with named axle, crank, steering, pelvis, camera and shadow pivots. Its
`0.7366 m` wheel diameter, `1.16 m` wheelbase, 636 triangles and six source
materials are manifest-validated. Two clean Blender 4.0.2 exports and two Qt
Balsam 6.8.3 conversions are byte-identical. Runtime wheel rotation follows
distance, while crank and articulated leg transforms follow pedal cycles;
standing, walking, terrain pump, root pitch/roll and ground-fixed airborne
shadow use the existing authoritative ViewModel values. The asset-policy suite
rejects final rider QML containing built-in cube, cylinder, cone or sphere
meshes. Every authored rider mesh now exports one normalized UV channel. Bike,
jersey, shorts and helmet use the shared low-contrast rider pixel tile while
retaining their high-contrast palette; minification is mipmapped and the
airborne shadow remains fixed to the terrain. Asset tests require
`TEXCOORD_0` on every GLB primitive and runtime tests verify all four materials
bind the packaged texture. The ViewModel selects pedal, coast, preload, air,
land, absorb, lean or bypass directly from each authoritative world/feature
snapshot. QML applies bounded 120 ms body and crank blends without changing
the rider root. One test drives all eight states, verifies pose bounds and
exact root/physics agreement, and renders a distinct 960 by 540 image for each
state. The catalog contact sheet SHA-256 is
`3f03a981a6a7d276ba89211ca74f2d441665329cc2a65fed14c987d9fe3479dc`.

### `VS-04` Validate the vertical slice end to end

- [x] Run deterministic complete and bypass sessions with Data Generator.
- [ ] Run a real trainer session without touching production athlete data.
- [ ] Verify trainer target, recording and feature outcome agree.
- [x] Measure target-laptop frame time and simulation skipped ticks.

**Done when:** one tabletop workout can be ridden, understood, saved and
replayed without a visual, physics, recording or performance release blocker.

**Data Generator evidence:** two isolated 70-second pre-release UI sessions use
temporary athlete libraries and exercise the complete Feature Lab with
`OnTarget` and `UnderTarget` telemetry. Both runs pass all nine accessibility
workflows, including Data Generator connection, virtual gears, Workout Game,
stop/continue, workout Save As and graceful shutdown. Each advances 452 m with
zero backward frames, skipped simulation ticks, trace regressions and
unexpected airborne frames. The completed route remains on the main line; the
bypass route's largest 250-ms lateral step is 0.928186 m. Median presentation
rates are 71.8784 and 71.1764 FPS. The archived videos have SHA-256
`5d6de153984af823d9c79c23e86fb9155bc66aca7bb533dbfc840a6c5354d2ba`
and `82563dd5a2ce892a5506aa26d6cdc291f2ca68d9c2dacb5c3abb5fea42b7a78f`;
their machine-readable summaries have SHA-256
`2dab455552f941955e10e359f017d6ef2fea46daa06be3b116a85b729c2306e0`
and `e327a44f0a672110d5180e96b8b42136adc5b0118f82465321a13cb62cce54fa`.

## P0 Physics And Motion

### `PHY-01` Make airborne ownership explicit

- [x] Select Box2D or scripted feature motion as authority for each action.
- [x] Remove the `max(physicsAir, featureAir)` ambiguity.
- [x] Preserve negative drop offset instead of clamping it to zero.
- [x] Keep bypasses surface-anchored and intentional air time bounded.

**Tests:** take-off/apex/landing continuity, no vertical teleport, bounded
flight duration, drop descent, bypass no-air and wheel/surface clearance.

**Done when:** every airborne frame has one explainable source and visual
landing agrees with the runtime event.

**Current evidence:** a ready Box2D world snapshot is now the sole airborne
authority in both the 3D ViewModel and shared rider visual. Scripted feature
height remains only as a fallback in the shared legacy rider visual when no
world snapshot exists; the 3D path does not synthesize one. Calibrated Box2D
launch speeds retain the previous readability
thresholds; tests cap tabletop height at 1.8 m, continuous flight at 2 s and
per-20 ms height change at 0.25 m. Drop depth remains in the authoritative
road surface rather than a second
scripted rider offset, and a 3D regression verifies that completed-drop rider
height follows that negative surface. Safe bypass continues to force Box2D
ground following and cannot activate either airborne visual path.

### `PHY-02` Add feature-specific rider response

- [x] Rollers pump/absorb while both wheels track the surface.
- [x] Berm uses the same curved path for trail, rider lateral position and roll.
- [x] Skinny uses subtle balance lean without random steering.
- [x] Roots/rocks/slab move suspension and torso without camera vibration.
- [x] Climb selects seated/standing effort and a bounded crest release.

**Tests:** per-feature pose and contact assertions plus approach/action/recovery
captures.

**Done when:** feature motion is recognisable even with grey materials.

**Current Climb evidence:** the canonical climb profile selects seated pedalling
at ordinary cadence and effort, blends into a bounded standing pose for high
effort/low cadence, switches to walking only through the authoritative world
state, and releases the standing pose through the five-metre crest. All three
poses remain grounded. A 240-frame capture for each pose verifies visible
motion through the same climb and crest without lateral movement or airtime.

### `PHY-03` Make virtual gearing and speed progressive

- [x] Low gear remains slow on flat and climb.
- [ ] Gear changes alter torque/cadence response, not speed instantaneously.
- [x] Grade, gravity, cadence, power and inertia contribute consistently.
- [x] Keep manual virtual gears usable outside game mode.

**Tests:** fixed-input acceleration traces for every gear, flat/climb/descent
comparisons, gear-change continuity and trainer/non-trainer cases.

**Done when:** no gear input causes a visible speed teleport and representative
traces match agreed cycling ranges.

**Current evidence:** the deterministic 50 ms acceptance trace now composes
the road-force model, all twelve drivetrain ratios and the game speed gate for
flat, eight-percent climb and eight-percent descent cases. Gear one remains
below 8 km/h on both flat and climb, every non-gravity speed sample stays
within the 7.2 km/h/s acceleration bound, and descents can freewheel beyond the
selected cadence ratio. Trainer-authoritative and power-estimate fallback
paths use the same cadence/gear limit; the fallback previously jumped directly
to 26 km/h in gear one. Existing runtime tests cover Data Generator target
scaling, Up/W and Down/S command routing, the always-available Train gear
selector and recorded virtual-gear telemetry. Real-trainer torque/road-feel
response remains open and must be validated before the second item is checked.

## P0 Feature Production

Every feature task includes main line, safe bypass where applicable, socketed
trail/terrain, rider response, catalog stills, completed/bypassed motion video,
and a runtime/geometry regression test.

- [x] `FTR-01` Log over: buried volumetric log and front/rear clearance.
- [x] `FTR-02` Bunny hop: distinct hurdle/kicker and short preload window.
- [x] `FTR-03` Drop: sharp ledge, exposed face, lower landing and downward pose.
- [x] `FTR-04` Rollers: rounded continuous crests/troughs and pump motion.
- [x] `FTR-05` Berm: broad banked bowl and shared curved rider line.
- [x] `FTR-06` Roots: branching embedded network and bounded roughness.
- [x] `FTR-07` Rock garden: sunk varied rocks and a readable rideable line.
- [x] `FTR-08` Rock slab: asymmetric mass, crest, sides and surface following.
- [x] `FTR-09` Skinny: narrow deck, supports, ground clearance and transitions.
- [x] `FTR-10` Climb: visible rising face, crest and effort pose.
- [x] `FTR-11` Tabletop: promote the accepted vertical slice to production.

**FTR-01 evidence:** a 16-sided transverse log now extends beyond both
trail edges, includes end-grain faces and a buried lower hull, and scales from
the authoritative difficulty profile. The faceted physical/visual profile is
continuous at both trail joins, with no start/end height teleport. A shared
`visualGroundElevationMeters()` contract keeps the runtime trail, forest-floor
socket, camera and asset root on rolling ground while leaving the authored
obstacle and rider response separate. The former duplicate 7.2-metre terrain
wedge is removed; matched 1280 by 720 before/after captures show unchanged
surrounding terrain and a readable log. The safe line begins at the decision
gate, reaches at least 1.67 m lateral clearance at the obstacle and uses the
same canonical branch blend and generated terrain profile for mesh and rider.
Its exact trail-height sockets, packed-soil tread and three joined strips avoid
teleports, floating ground and a pasted rectangular tile. Explicit geometry
updates keep newly built and cleared buffers synchronized with Quick 3D.

The integrated X11/OpenGL acceptance captures compare completed, bypassed and
branch-hidden scenes and reject a visually absent branch. Deterministic 72-frame
captures at 960 by 540 show the main rider clearing and landing beyond the log
while the bypass rider stays grounded, clears the log and merges back over the
same 22 m audit window. Asset validation passes 14 tests; road, feature runtime,
mesh, simulation and geometry pass 23, 15, 22, 29 and 14 tests. The full
interactive view suite and its ASan/UBSan run pass, and the opt-in motion export
passes both 72-frame routes. The generated GLB is therefore approved for this
feature slice.

**FTR-02 evidence:** bunny hop no longer dispatches to the log mesh or a QML
primitive. A deterministic project-authored `4,864`-byte GLB supplies a
distinct `0.10-0.20 m` timber practice hurdle with two materials, 28 triangles,
exact `3.50 m` ordinary-trail sockets and named prepare, decision, action,
preload, takeoff, apex and landing markers. The asset contains no ground, so
the authoritative trail remains flat under the independent obstacle; runtime
difficulty scales only its height.

The preload window is capped at three metres. Scripted presentation reaches
at most `0.42 m`, while Box2D uses its own `3.0 m/s` launch impulse; regression
tests prove both air paths are lower and shorter than log-over, land within the
bounded window and cannot retrigger the same anchored action. The existing
terrain-following safe branch clears the hurdle, remains grounded and rejoins
without a lateral teleport.

Two clean Blender 4.0.2 exports and two Qt Balsam 6.8.3 conversions are
byte-identical. Khronos validation reports zero errors, warnings, infos and
hints. Asset, road, runtime, mesh, world and direct placement suites pass 15,
24, 16, 24, 30 and 6 tests. The 36-test X11/OpenGL view suite passes with only
three explicit opt-in video skips; the bunny opt-in completed and bypassed
72-frame exports both pass. ASan/UBSan passes the changed core suites and the
packaged/integrated Quick 3D bunny render.

**FTR-03 evidence:** Drop now uses a level ten-metre approach, a sharp lip,
an actual `1.25 m` gap, a difficulty-scaled `0.35-0.70 m` lower landing and
a seven-metre smooth recovery. Trail indices and Box2D segments are both
absent through the same authoritative gap. The safe route regenerates Box2D
against the ordinary continuous surface, remains grounded and cannot report a
false landing impact.

The project-authored `4,736`-byte GLB contributes only the exposed faceted
rock face below the lip. It has 20 triangles, two opaque materials, exact
`22.0 m` profile sockets and named prepare, decision, action, lip, air,
landing and recovery markers. The streamed trail owns approach, gap, landing,
recovery and bypass geometry, preventing duplicate ground and pasted-on
tiles. Runtime difficulty scales only the face depth.

Two clean Blender 4.0.2 exports and two Qt Balsam 6.8.3 conversions are
byte-identical. Khronos validation reports zero errors, warnings, infos and
hints, and the repository asset validator passes all 16 policy and fixture
tests. Asset placement, geometry, mesh, road, runtime, world and X11/OpenGL
view suites pass `7 + 15 + 25 + 25 + 16 + 32 + 41` tests. The same changed
C++ suites pass ASan/UBSan. Completed and grounded-bypass stills plus both
72-frame motion exports pass; the completed route uses the real Box2D
snapshot and the bypass uses the real alternate collision surface. Regression
limits keep flight at `0.2-1.2 s`, clearance below `1.10 m`, airborne pitch
inside `-18..+4 degrees`, per-frame elevation change below `0.20 m`, and
landing impact inside `0.05-0.65`.

**FTR-04 evidence:** Rollers are one project-authored procedural trail tile,
not a decorative model over ordinary ground. The `10.50 m` canonical profile
has exact `0.75 m` level entry and exit sockets around a `9.00 m` active section.
Three raised-cosine crests are spaced `3.00 m` apart and scale from `0.20` to
`0.28 m`; the same profile supplies road sampling, Quick 3D mesh rows and Box2D
segments. Exact crest and trough samples prevent the renderer from flattening
the feature between generic spacing points.

Rollers remain the centre trail for both successful and weak efforts. They do
not generate a side branch or switch Box2D to an ordinary flat bypass surface;
a weak effort only misses its flow bonus. Active curvature weighting and a
bounded pitch controller keep both tyres in real Box2D contact without adding
longitudinal energy. Acceptance traces stay grounded at `3.33`, `5.0` and
`7.0 m/s`, exercise at least `0.08` suspension travel and keep pitch inside the
`+/-18 degree` design envelope. The rider torso pose is distance-anchored:
approximately `0.10 m` compression over each crest and `0.06 m` extension in
each trough, with measured suspension providing only fine motion.

The X11/OpenGL feature catalog confirms three readable crests, exact trail
joins and no roller bypass. The opt-in 72-frame production render uses real
Box2D snapshots at `7.0 m/s`; it reports zero airborne frames, over `0.05 m`
visible torso range and changing pixels in more than four fifths of frame
transitions. Road, simulation, runtime, world, mesh, geometry and X11/OpenGL
view suites pass `26 + 30 + 17 + 33 + 26 + 15 + 42` tests both normally and
under ASan/UBSan; the five normal suite skips are explicitly opt-in exports and
the roller export passes separately. Review artifacts are `rollers-completed.mp4`,
`rollers-contact-sheet.png`, `rollers-crest.png` and the full catalog under
the build host's `gc-ui-ftr04` artifact directory.

**FTR-05 evidence:** Berm is one project-owned procedural trail tile driven by
`WorkoutGameBermGeometry`. Its `7.74 m` canonical profile has `1.25 m` level
entry and exit sockets, a C2-continuous `75 degree` centreline turn, a
difficulty-scaled `20-30 degree` bank and a bowl-shaped five-point tread. Two
closing edge skirts join the forest-floor sockets; ordinary trail and the
forest centre strip are suppressed under the tile, preventing duplicate
surfaces, cracks and background holes. At most 624 opaque triangles are
generated at 0.15-metre spacing.

Road heading, trail width, bank, mesh, rider height and rider roll all consume
that profile. A weak effort uses a bounded 0.45-metre inside line on the same
tread; it does not create a second bypass ribbon or change the Box2D surface.
Main-line roll follows speed and signed road curvature, the inside line is
capped at eight degrees, and presented roll changes by at most 1.5 degrees per
frame. A socket-blended tangent chase camera keeps both lines inside the
central view through the apex without changing the accepted camera outside the
tile.

Road, geometry, runtime, world and headless view suites pass
`29 + 17 + 18 + 34 + 32` tests normally and under ASan/UBSan. The complete
X11/OpenGL view suite passes 44 tests with only six explicit opt-in exports
skipped. The berm export separately passes both 96-frame routes, including
bounded lateral steps and roll; both videos are 3.2 seconds at 30 FPS. Focused
interactive ASan/UBSan rendering also passes. Physics remains grounded at 3,
5 and 7 m/s. Review artifacts are
`berm-main-line.mp4`, `berm-safe-line.mp4`, their frames and catalog images in
the `gc-ui-ftr05` artifact directory on the build host.
The design uses the Recreation Aotearoa NZ trail guidelines for dimensional
reference and OpenStax's centripetal-force relation for lean; no diagram, code
or third-party model is copied into the repository.

**FTR-06 evidence:** Roots are a project-owned procedural 12-metre trail tile
driven only by `WorkoutGameRootGeometry`. Five of its eight irregular branches
cross the main tread, radii scale with difficulty and seven percent burial
keeps the network embedded instead of floating above the trail. The active
four-metre root bed widens from 0.68-metre socket half-width to 1.25 metres and
returns through quintic transitions. A 0.82-metre safe line remains on that
same tread, rejoins the exact entry and exit sockets and has less than one
quarter of the main-line surface relief.

Road elevation, trail width, Quick 3D tubes, legacy comparison geometry,
runtime line choice and Box2D contact all consume the canonical profile.
Ordinary trail and forest datum are suppressed beneath the raised root
surface, while adaptive 0.04-metre collider samples preserve the short crowns.
Roots never create scripted airtime or camera shake; real front/rear suspension
drives a bounded, 80-ms-filtered torso pump. The Quick 3D network is 512 opaque
triangles per tile and is rebuilt into one of two buffers only when its bounded
streaming range changes.

Road, geometry, runtime, mesh and world suites pass
`31 + 19 + 19 + 27 + 35` tests normally and under ASan/UBSan. The complete
X11/OpenGL view suite passes 45 tests with seven explicit opt-in exports
skipped, and focused sanitizer rendering passes the same gate. The separate
192-frame main- and safe-line export passes at 60 Hz with zero airborne frames,
bounded lateral movement, changing pixels in more than four fifths of frame
transitions and safe-line suspension travel below 25 percent of the main line.
The rendered HUD also verifies the 220-watt main target instead of inventing a
cadence requirement. Review videos and approach/core/exit images are in
the `gc-ui-ftr06` artifact directory on the build host. The final main-line and
safe-line MP4 SHA-256 values are respectively
`bb7766231198aaec48b33438be823e06e98425d276f405c5775326b8b164c658` and
`8a33f9bb61d114b88522010aca14ae1c6a7506d2cf78fe6fee178a0a17b25f46`.
No external model, image or texture was added.

**FTR-07 evidence:** The rock garden is a project-owned procedural 14-metre
trail tile driven only by `WorkoutGameRockGardenGeometry`. Twelve deterministic
elliptical stones vary in footprint, yaw and height; nine intersect the main
line and all are buried by 18 percent. The active 6.5-metre bed widens from a
0.68-metre socket half-width to 1.35 metres. Its 0.88-metre safe line remains
on the same tread, rejoins through bounded cubic transitions and has no more
than 3.5 centimetres of relief.

Road elevation and width, runtime line choice, Box2D tyre contact, legacy
comparison geometry and the merged Quick 3D mesh all consume that canonical
profile. The physics adapter samples the active bed at 0.04-metre spacing. One
Quick 3D draw range contains 180 vertices and 252 opaque triangles per tile,
is bounded to 256 resident stones and is rebuilt into one of two buffers only
when the floor streaming range changes. Ordinary trail relief is suppressed
under the stones, so neither visual nor physical duplicate geometry can lift
the rider. Rock gardens never synthesize airtime, camera vibration or cadence
requirements; Box2D front/rear suspension drives a 70-ms-filtered torso pump.

Road, geometry, runtime, mesh and world suites pass
`33 + 21 + 20 + 28 + 36` tests normally and under ASan/UBSan. The complete
X11/OpenGL view suite passes 46 tests with eight explicit opt-in exports
skipped under both configurations. Coverage includes deterministic profile
scaling, exact sockets, 3/5/7 m/s contact, bounded safe-line suspension and HUD
target authority. The separate 288-frame main- and safe-line export passes at
60 Hz normally and under ASan/UBSan with zero airborne frames, lateral steps
below five centimetres and changing pixels in more than four fifths of frame
transitions. Review videos and approach/core/exit images are in the
`gc-ui-ftr07` artifact directory on the build host. Both videos are 960 by 540,
4.8 seconds and 288 frames. Their SHA-256 values are respectively
`f7ed58649c25407d9955b887b1b376b57500c4fe87352fd5f8aff7f82a14fc7b` and
`00fb385a528b8c4216a041e6f25a0c2f80ff88bf0c73fdc21b64a8bad50c5db7`.
No external model, image or texture was added.

**FTR-08 evidence:** The rock slab is a project-owned procedural 14-metre
same-tread tile driven only by `WorkoutGameRockSlabGeometry`. Its active face
runs from -3.8 to +3.6 metres around a rounded crest at +0.25 metres. Height
scales from 0.62 to 0.96 metres, the irregular asymmetric stone footprint is
approximately 1.44-1.76 metres wide, and exposed buried sides plus four dark
fissures make the mass readable without a texture. The widened trail reaches a
1.42-metre half-width while the low-relief safe line stays 1.05 metres right of
centre and joins the exact 0.68-metre entry and exit sockets.

Road elevation and width, runtime line choice, Box2D tyre contact, legacy
comparison geometry and one merged Quick 3D draw range consume the canonical
profile. The active physics surface is sampled at 0.04-metre spacing. One slab
uses 147 vertices and 224 opaque triangles; at most twelve are resident, and
the range is rebuilt into one of two buffers only when the floor streaming
bucket changes. Ordinary trail relief is suppressed under the stone. The slab
never synthesizes airtime or camera vibration; real front/rear suspension
drives a 75-ms-filtered torso response while the safe line remains grounded.

Road, geometry, runtime, mesh and world suites pass
`35 + 23 + 21 + 29 + 37` tests normally and under ASan/UBSan. The complete
X11/OpenGL view suite passes 47 tests with nine explicit opt-in exports skipped
under both configurations. The separate 288-frame main- and safe-line export
passes normally and under ASan/UBSan with zero airborne frames, lateral steps
below five centimetres and changing pixels in more than 90 percent of frame
transitions. Review videos, baseline/catalog comparisons and approach/core/exit
images are in `$HOME/Documents/personal/gc-ui-ftr08` on the build
host. Both videos are 960 by 540, 60 FPS and 4.8 seconds. Their SHA-256 values
are respectively
`dd1efa6beb2c527c4b0123e54534d303a624268a9691f1dc89b1c1c901602001` and
`f0208a9cd8261ef1e6ea73d638ac61302ca95ef70979e945b3e1389b511e5fd8`.
No external model, image or texture was added.

**FTR-09 evidence:** Skinny is a project-owned procedural 18-metre tile driven
only by `WorkoutGameSkinnyGeometry`. Its 14-metre active section rises through
ramps below six degrees to a seven-metre deck. Difficulty scales the deck from
0.62 to 0.50 metres wide and from 0.28 to 0.36 metres high. Sixty individually
spaced boards, two longitudinal beams and ten ground-reaching supports expose
real clearance below the deck. The 1.05-metre lateral packed-dirt safe line
stays within the same widened tile and both routes rejoin exact 1.36-metre-wide
entry and exit sockets through four-metre transitions.

Road elevation and width, runtime line choice, Box2D ground contact, legacy
comparison geometry and the merged Quick 3D mesh all consume that canonical
profile. Ordinary centre trail is suppressed only under the dedicated active
tile. A shared ground datum and a bounded 0.30-metre render seam overlap prevent
double height offsets and clear-color gaps without changing the physical trail
width. One tile uses 1,008 vertices and 504 opaque triangles; at most twelve are
resident in one double-buffered model/material range. Skinny never creates
scripted air, random steering or camera vibration. Main-line balance roll is a
deterministic one-to-two degrees, while the safe line remains level and
grounded. A distance-station cache reuses road and terrain-profile samples
during each cold-path mesh rebuild, and the bounded vertex/index storage is
reserved once instead of repeatedly growing on the GUI thread.

Road, feature-runtime, legacy-mesh, world, Quick 3D geometry, terrain-profile,
scene-graph and X11/OpenGL view suites pass
`37 + 22 + 31 + 38 + 25 + 10 + 20 + 49` tests normally and under real
ASan/UBSan instrumentation. The complete view suite has ten explicit opt-in
exports skipped. The separate main- and safe-line export passes normally and
under ASan/UBSan with 288 frames per route, zero airborne frames, lateral steps
below five centimetres, vertical steps below ten centimetres and changing
pixels in more than 90 percent of frame transitions. Its midpoint regression
also rejects the renderer clear color inside the trail. Review videos and
approach/core/exit images are in `$HOME/Documents/personal/gc-ui-ftr09` on the
build host. Both videos are 960 by 540, 60 FPS and 4.8 seconds. Their SHA-256
values are respectively
`f51e2eab77dc085db0f3e568f9975a21c95774993c057ecb2b063703c52c8ed0` and
`b3108494537f3de22f98cb6a6d253b900e03b9767315bbc64eafebaa6ef62928`.
No external model, image or texture was added.

**FTR-10 evidence:** Climb is a project-owned procedural same-tread feature.
The road builder preserves the requested section rise while splitting every
climb into bounded five-metre entry and crest grade transitions around a
sustained grade. Short authored climbs normalize to a safe 14-metre minimum,
and even adversarial explicit lengths have a bounded piece count. The final
14 metres contain five transverse rock steps, an exact 1.36-metre-wide socket
and a readable crest. Difficulty scales the step heights from approximately
0.08 to 0.14 metres at the catalog setting.

Road elevation, Box2D contact, the legacy comparison renderer and Quick 3D all
consume `WorkoutGameClimbGeometry`. Its yaw-aware footprint supplies the same
plateaus and 0.24-metre smoother-step approach/runout ramps used by the merged
opaque mesh. Rendered top and ramp vertices are checked against that canonical
contact surface. Trail relief is carved below the dedicated stones, so the
680-vertex, 340-triangle range remains visible without z-fighting; range
filtering includes the rotated ramp footprint. Climb has no artificial bypass,
lateral teleport or airtime. Missing the target retains the main line, loses
only the bonus and publishes a distinct `NO BONUS` result for the first six
metres of an unchallenged runout. The result path is tested end to end from the
real simulation through runtime, HUD and QML.

Road, simulation, feature-runtime, legacy-mesh, world, Quick 3D geometry, HUD
and replay/engine suites pass `40 + 32 + 24 + 32 + 39 + 27 + 13 + 13` tests
normally and under ASan/UBSan. The complete X11/OpenGL view suite passes 52
tests with eleven explicit opt-in exports skipped under both configurations.
The separate seated, standing and walking exports pass normally and under
ASan/UBSan. Each video is 960 by 540, 40 FPS, six seconds and 240 frames; more
than four fifths of frame transitions visibly change. Review artifacts are in
`$HOME/Documents/personal/gc-ui-ftr10-final` on the build host. Video SHA-256
values are respectively
`10c28b8a217aeb277ae5fdfe23bf962e009c5216cd040add10e969b91cb52f7b`,
`13b8e6121c76dfd0f5da13574aa34cc72ea345cec5c5f31229774fd97517f694`
and `2027d76c4de9ae704715322bde5e36332ec8a2238f2486ebc0907c244b7e2ea9`.
No external model, image or texture was added.

**FTR-11 evidence:** Tabletop is a project-owned procedural jump driven by
`WorkoutGameTabletopGeometry`. One canonical profile supplies its sockets,
belly, 18-degree lip, deck, knuckle, landing, safe-line clearance and launch
calibration. The road, terrain mesh and Box2D world therefore expose the same
surface; the old rigid GLB is retained only as an isolated comparison asset
and is not instantiated by production QML. The main rider preloads during the
last 0.9 m, follows speed-dependent Box2D flight and absorbs the measured
landing impact. A ground-fixed shadow makes the bounded air phase readable.
The bypass mesh, rider anchor and Box2D contact use the same deterministic
side-terrain surface and publish no airborne frames.

Clean focused suites pass `44 + 24 + 32 + 42 + 12 + 7` road, runtime, mesh,
world, terrain-profile and asset tests. Simulation, engine, Quick 3D geometry
and legacy comparison scene pass `33 + 15 + 27 + 20` tests. The same suites
pass under ASan/UBSan. Real road-configured Box2D runs at 3, 5 and 7 m/s land
on the authored landing ramp within two seconds, remain below 1.8 m and
produce speed-dependent flight. Unsupported launch speeds are explicitly
gated, while natural tyre contact remains physical. Engine integration also
checks the authoritative-timeline landing point and bounds every 20 ms bypass
lateral step. The complete X11/OpenGL view suite passes 54
tests with 12 explicit opt-in exporters skipped; the tabletop still and motion
exporters pass separately. Their completed and bypassed 72-frame videos show
235/220 W completion and 150/220 W bypass outcomes. Video SHA-256 values are
respectively
`b98ace83b0d75d9c9449015309f2211c0ec2bbaa61212397b5f3134fcaccf813`
and
`42b5b6e1863d187ef3939318072f6d4856270fe2cc0ecbd9203748d4efe70e49`.
No external model, image or texture was added.

**Feature acceptance:** each feature is identifiable without its name, joins
ordinary trail without a crack or width jump, and produces a visibly correct
completed outcome plus a bypassed outcome where that feature has a safe branch.

## P0 HUD And Testability

### `HUD-01` Replace the combined readiness percentage

- [x] Show feature name and distance to decision/action point.
- [x] Show power and cadence readiness separately.
- [x] Use distinct prepare, act-now, committed, complete and bypass states.
- [x] Keep cue duration short and aligned to physical feature position.

**Tests:** state-machine text/value tests, 720p/1080p screenshot bounds, long
translation layout and deterministic timing captures.

**Done when:** a rider can explain what to do, when to do it and why a line was
selected without reading logs.

**Current evidence:** `WorkoutGameFeatureHud` converts immutable runtime and
simulation snapshots into a presentation-only state. Twelve headless tests
cover every state, independent requirement values, malformed telemetry, a
12-metre prepare lead and a six-metre result tail; the same suite passes under
ASan and UBSan. ViewModel and QML tests cover state reset on course changes,
bounded bars and labels at 360 by 640, 1280 by 720 and 1920 by 1080. The complete
X11/OpenGL suite passes 20 tests, including nonblank production rendering and
motion, with only the explicitly opt-in video export skipped.

### `HUD-02` Restore complete training instrumentation

- [x] Add workout power profile and current-position cursor.
- [x] Add current grade while preserving watts, target, cadence, heart rate,
  speed, gear, time, distance and actual presented FPS.
- [x] Keep data readable over bright/dark terrain and at laptop resolution.

**Tests:** value binding, cursor progression, missing-sensor states and desktop/
laptop/mobile-aspect screenshots.

**Done when:** game mode does not remove information needed to execute and
review the workout.

**Current evidence:** the 3D ViewModel builds bounded normalized profile
segments only when the course changes, while each immutable visual frame updates
only cursor and grade scalars. `WorkoutGameTrainingHud.qml` presents ten
instrument values, an interval profile and cursor in opaque high-contrast
bands. Missing heart-rate and cadence sensors display `--` instead of zero.
Binding and cursor-clamp tests pass, and real X11/OpenGL captures at 360 by 640,
1024 by 600 and 1920 by 1080 verify bounded nonblank layouts. The production
window test also waits for a nonzero value derived from `frameSwapped`, proving
the displayed FPS is measured presentation rate. The full interactive suite
passes 25 tests; the complete headless suite passes under ASan and UBSan.

### `GEN-01` Give Data Generator the active target authority

- [x] Generator follows the selected game/workout target by default.
- [x] Add deterministic over-target, on-target, under-target and cadence modes.
- [x] Add completed and bypassed scripts for all eleven features.
- [x] Expose generator state clearly without affecting normal devices.

**Tests:** target equality, outcome matrix, reproducible telemetry trace and
isolation from real device configuration.

**Done when:** automated UI videos can reliably demonstrate both lines of every
feature without a trainer.

**Current evidence:** the device wizard stores one of `follow-target`,
`on-target`, `over-target`, `under-target`, `cadence-low` or `cadence-high`
only in a Data Generator device profile. Feature Lab publishes its canonical
target through `Context`; `TrainSidebar` routes that command exclusively to
active `DEV_NULL` controllers, and the selected generator power source copies
its authoritative load and bounded source label into aggregate telemetry.
Engine no longer replaces a received target. The 16-test generator suite covers
profile parsing, exact mode contracts, target normalization, real-device
isolation, aggregate telemetry publication, reproducibility, bounded source
names and fractional-target survival through integer `RealtimeData`. The
17-test Engine suite drives integer-quantized `OnTarget` and `UnderTarget`
generator samples at FTP 190 through all eleven Feature Lab sections and
observes 11 `Completed` plus 11 `Bypassed` outcomes. It also enforces expected
air time, a zero main-line lateral offset and a sub-one-metre safe-line step at
the 250-ms trace cadence. The generator state is visible as a HUD badge only
for generator telemetry; the 3D View suite verifies shown/hidden state and
bounded layouts at 360 by 640, 1024 by 600 and 1920 by 1080. All 54 enabled 3D
View tests pass on real X11/OpenGL; 12 exporter-only tests remain explicit
opt-in skips. Isolated 70-second X11 Scene Graph UI recordings cover every
Feature Lab section in both modes. `OnTarget` produced zero lateral movement
and `UnderTarget` bounded the largest 250-ms lateral step to 0.929 m; both runs
reported zero backwards frames, skipped ticks, trace regressions and
unexpected airborne frames while sustaining about 71 FPS median.

## P1 Art, Environment And Feedback

- [x] `ART-01` Apply one coherent low-poly/pixel-textured palette and atlas.
- [x] `ENV-01` Add varied forest silhouettes, uneven terrain and restrained fog.
- [x] `ENV-02` Eliminate tree pop, floating bases and buried visible geometry.
- [x] `FX-01` Add bounded contact shadow, landing dust and success feedback.
- [x] `FX-02` Add restrained feature/camera punctuation after physics is stable.
- [x] `AUDIO-01` Add optional low-cost feature and landing audio after visuals.

**Acceptance:** art improves depth and arcade identity without hiding the path,
exceeding budgets or changing physics.

**AUDIO-01 evidence:** Workout Game sounds are controlled by a separate global
training preference that defaults off. Enabling it lazily creates two
`QSoundEffect` instances; playback performs no file access, waiting or work on
the simulation, trainer-control or recording paths. The simulation owner
publishes feature-measure and authoritative landing-impact rising edges through
an eight-entry fixed-capacity journal with event ids and reset epochs. Every
newest-frame publication carries that journal, so replacement of intermediate
mailbox frames cannot lose a cue. Pause/resume does not replay consumed events,
while a real simulation reset or backward seek starts a new epoch. Both PCM
mono 22,050 Hz cues are project-authored from the checked-in deterministic
FFmpeg recipe; their packaged sizes are 8,016 and 9,780 bytes and SHA-256
hashes are `3ef844884060a047e757a3610a0b4bfc84cde81aaf95985a7eabddabf147ee48`
and `aa0252211d25de24a455f17c92da84244a7a5f4f20df79afbc01bcc308ca5e04`.
The ten-test audio suite covers packaged RIFF/WAVE resources, unavailable and
invalid input, one cue per action, landing thresholds and strength, overwritten
frames, capacity, reset and backward seek; it passes normally and under
ASan/UBSan. The 17-test Engine
suite observes exactly eleven feature cues plus real landing cues over the
complete deterministic Feature Lab, and the 11-test Runner lifecycle suite
passes unchanged. The full production target compiles in the release build
container.

**ENV-02 evidence:** the project-authored `EN-01` set replaces the former
built-in cone and cylinder trees with narrow, layered and broken-top conifer
silhouettes. All source vertices are at or above a named zero-height base;
runtime placement still interpolates the authoritative terrain profile and the
camera-to-cue corridor still rejects occluding crowns. Trees fade over the
resident window's six-metre rear and ten-metre forward edge bands with a
bounded 320 ms presentation transition. The complete set is 192 triangles,
contains no external content, reproduces byte for byte through Blender and
Balsam, passes manifest and primitive-rejection tests, and remains inside the
existing ten-tree/50-draw-call runtime budgets.

**ENV-01 evidence:** `EN-01` supplies three deterministic conifer silhouettes,
the resident near-terrain profile supplies bounded cross-slope and forest-floor
relief, and approved project-authored `EN-03` closes the horizon with five
uneven radial bands. The distant mesh is 27,600 bytes, 256 triangles and one
material/draw call; two Blender 4.0.2 and two Balsam 6.8.3 runs are
byte-identical. Fog is depth-only from 68 to 260 metres, so it adds depth
without obscuring the near trail or cues. Asset-policy, manifest, real
X11/OpenGL contract, render-budget and 72-frame completed/bypassed motion tests
cover the packaged result. The complete real-X11 suite passes 59 tests with no
failures, selected tests pass under ASan/UBSan, and the production target
compiles in the release container.

**ART-01 evidence:** the approved
`WORKOUT_GAME_3D_ART_BRIEF.md` fixes the visual hierarchy, palette, sampling,
socket continuity and performance rules. Project-authored `TR-08` generates a
96 by 64 master atlas and five 32 by 32 forest, dirt, stone, wood and rider
runtime tiles without external input. The 1,561-byte package reproduces byte
for byte, passes 20 asset-policy tests and manifest validation, and loads
through the production qrc. Runtime repeat wrapping, nearest magnification,
linear minification and mipmaps retain close pixel blocks without distant
shimmer.
Real X11/OpenGL still and 72-frame completed/bypassed tabletop tests preserve
path readability, opaque coverage and render budgets. The rider, bike, jersey,
shorts and helmet use the rider tile through deterministic GLB UVs while
retaining their high-contrast silhouettes. The reviewed completed motion
artifact is 960 by 540, 15 FPS and 4.8 seconds with SHA-256
`ead5742155c468cbaa9908a2aaa46338529d52164d7eb4b8be5e7afb160e9f21`.

**FX-01 evidence:** the rider retains its ground-fixed authored contact shadow.
The ViewModel now emits monotonic presentation event IDs only on a world
landing-impact rising edge above 0.08 and on a new nonzero completed-recovery
`actionId`; repeated frames and bypass outcomes cannot retrigger feedback. Three
preallocated pixel-puff items project the authoritative rider ground point
through the explicitly assigned Quick 3D camera, spread and fade over 460 ms,
and create no runtime list or 3D draw call. A 900 ms unframed `FEATURE CLEAN`
pulse stays between the responsive training and feature HUDs. Real X11/OpenGL
tests verify payload ordering, deduplication, bounded opacity, viewport
projection, 360 by 640 layout, pixel changes, duration and render budgets. The
complete suite passes 60 tests with no failures, and nine focused tests pass
under ASan/UBSan. The reviewed 16-frame combined landing/success artifact has
SHA-256 `43ae5851316d33b1525768189321e35cfdb0c3dda061761626c11915c8a41578`.

**FX-02 evidence:** Quick 3D now derives a bounded 46.35-to-48.3-degree field
of view from the immutable preload, physical-air and landing snapshots. It
animates only that presentation property over 120 ms; the authoritative camera
root and target remain unchanged, normal riding stays at 47 degrees, and the
safe bypass cannot trigger punctuation. A real X11/OpenGL test verifies every
state, transition bound, neutral restoration and invariant camera position. Its
opt-in catalog captures baseline, preload, air, landing and bypass at 960 by
540; the reviewed contact sheet has SHA-256
`5a446ed3fbfd716eb243f86a67242c15ae7d0564032b4ab0d5f34792a8b611b7`.
The complete real-X11 suite passes 61 tests with no failures, three focused
tests pass under ASan/UBSan, and the production target compiles in the release
container.

## Diagnostics And Performance

- [x] `DIA-01` Count actual presented frames and report p50/p95/p99 frame time.
- [x] `DIA-02` Count backward distance, unexplained stationary frames, skipped
  simulation ticks, renderer queue depth and long frame work.
- [x] `DIA-03` Trace feature phase, route, readiness, action distance, rider
  contacts, camera transform and active asset/LOD identities.
- [x] `DIA-04` Capture direct snapshots and videos from isolated deterministic
  sessions.
- [x] `PERF-01` Stay below 30,000 visible triangles and 50 draw calls initially.
- [x] `PERF-02` Hold 60 Hz presentation budget on the target Intel GPU without
  increasing Bluetooth, trainer-control or recording latency.

**PERF-02 evidence (2026-08-26):** Quick 3D now uses a session-scoped QML
`FrameAnimation`; its lifecycle test proves that the pulse starts and stops with
the session instead of depending on 50 Hz input snapshots or an independent GUI
timer. Presentation callbacks retain the completed `frameSwapped` timestamp,
coalesce to the newest frame and enter the GUI queue at low priority. The 3D
window requests swap interval zero before creating its native surface so the
Qt render thread cannot hold the GUI thread behind a display-vsync wait; this
setting is scoped to the Workout Game window. Unchanged normalized telemetry
also no longer republishes the same HUD bindings every frame.

The opt-in 1280 by 720 target test renders the full eleven-feature budget course
with High MSAA and 50 Hz moving snapshots while comparing against an idle
baseline. On the directly accelerated Intel HD Graphics 4000/crocus path and
the laptop's physical 1920 by 1080, 59.9988 Hz display, the production setting
produced 121.8 presented FPS, p50 8.07 ms, p95 10.36 ms and p99 11.88 ms. Its
20 ms telemetry probe reported p95 1 ms and max 3 ms; the 100 ms real
trainer-target coordinator path reported p95 1 ms and max 3 ms; the 250 ms real
temporary-file write/flush path reported p95 1 ms and max 1 ms. None missed a
deadline, simulation skipped no ticks and the geometry queue remained empty.
Qt timing confirmed that the former bottleneck was a 9-11 ms GUI wait while
the render thread spent about 15 ms in a vsynced swap, not scene rendering,
which took about 1 ms. The complete real-X11 suite now passes 64 tests with 13
explicit opt-in skips; the production target compiles in the release container.

**DIA-04/PERF-01 evidence:** opt-in real-X11 tests capture direct PNG sequences
and encoded motion for camera compositions, all eleven features, rider action
states, environment motion and landing/success feedback from deterministic
snapshots. Production rendering exposes custom-mesh triangle counts and Qt
Quick 3D extended draw-call statistics; the all-feature budget course rejects
more than 30,000 visible custom triangles or 50 actual draw calls and passes
normally and under ASan/UBSan.

**DIA-01/DIA-02 evidence:** the shared frame counter uses completed
`frameSwapped` nanosecond timestamps and a bounded one-second window to publish
realized FPS plus nearest-rank p50/p95/p99 intervals. Quick 3D now compares its
latest source snapshot with the smoothed presented snapshot and feeds the
existing diagnostics module with road distance, section/progress, skipped
simulation ticks, the bounded geometry queue and measured GUI presentation
work. The module counts backward, stationary, over-25-ms and over-8-ms work
frames and keeps only cumulative counters and maxima. Both Quick 3D and Scene
Graph expose the values through tests, trace output and opt-in HUDs. Real
X11/OpenGL tests cover active, resized and stopped lifecycle states; the
reviewed 960 by 540 Quick 3D HUD capture has SHA-256
`a810a170296b2b78cdf7a39a4f949775f2b8c6aa36d9ede301d7aa6edfaa2e19`.
The complete Quick 3D suite passes 62 tests and the complete Scene Graph suite
passes 20 tests without failures. Focused tests pass under ASan/UBSan for both
renderers, and the production target compiles in the release container.

**DIA-03 evidence:** the opt-in Quick 3D trace now identifies the presented
feature phase, selected route, readiness, action distance and action id. It
also publishes authoritative Box2D rear/front wheel contacts, suspension and
airborne state, camera position and target, rider/surface/near/distant asset
manifest ids, feature geometry, the current single resident LOD and visible
triangle count. Wheel contacts participate in deterministic replay hashing,
and production tabletop tests prove grounded-to-airborne-to-grounded contact
transitions without inferring them from animation. The trace retains no frame
history and remains limited to four records per second. The complete world,
replay and real-X11 Quick 3D suites pass 43, 6 and 62 tests respectively;
focused world, replay and Quick 3D tests also pass under ASan/UBSan. A direct
trace-emission run produced one complete record without an unbounded log
stream, and the production target compiles in the release container.

## Release And Legacy Retirement

The repository-hosted `ci.yml` workflow is tracked separately as CI
infrastructure debt and is not an acceptance gate for this private-use release.
The release evidence below must come from the controlled local and remote
build/test environments even when that historical workflow remains red.

- [x] `REL-01` Run unit, integration, visual, UI, recording and Bluetooth tests.
- [x] `REL-02` Verify stop/save/cancel/continue and accidental-stop recovery.
- [x] `REL-03` Verify AppImage startup, QML/Quick 3D modules, assets, licenses,
  attribution, SBOM and isolated production-like athlete data.
- [ ] `REL-04` Complete user A/B ride on the target laptop and real trainer.
- [x] `REL-05` Publish a test AppImage to the stable local/remote release path.
- [ ] `REL-06` Remove the legacy renderer only after all acceptance gates pass.

**REL-01 evidence (2026-08-26):** the controlled remote Linux shadow build
reconciled all 168 eligible projects from the required-test inventory. The
release run completed 164 QtTest suites with 5,632 passing cases after
test-first repairs to missing production link dependencies, stale feature-gate
expectations and source/fixture paths that had incorrectly depended on an
in-source build. The same binaries passed the full remaining inventory under
Xvfb, `xcb` and software OpenGL, including the 20-case Scene Graph visual suite
that produces a blank framebuffer under Qt's `offscreen` plugin. The inventory
also covers Quick 3D, deterministic replay, workout adaptation, recording,
durable save/removal, FTMS readiness, Bluetooth telemetry, ANT lifecycle and
USBXpress safety. Together with the isolated AppImage UI matrices and recording
recovery evidence below, this closes the automated release-test gate. Real
trainer A/B acceptance remains separately open under `REL-04`.

**REL-03/REL-05 evidence (`56faa71`):** two independent clean release builds
produced byte-identical ELF binaries and AppImages. The promoted AppImage is
267,504,120 bytes with SHA-256
`1e1a33ba95995961f6c3ebd1eed458bbc65de7dbccc5c1aa9f5f64391ae15d4a`.
Its sidecar manifest has SHA-256
`7b9dc00d137ea651190ea589dd303c0c85c42ed97845cb2bd373f279b6a82480`,
its CycloneDX SBOM has SHA-256
`22ed401a39f7f055a2c26fa970ad6e83dcccf84c7d3eb17cd0dae5e5c65e867b`,
and the verified manifest records the exact source revision, Qt 6.8.3 and a
configured private Strava OAuth fallback without exposing either credential.
The package passed its embedded manifest, dependency provenance, attribution,
QML module, Quick 3D asset, keychain, OAuth and isolated GUI smoke gates.

The promoted package then passed 10/10 QPainter and 10/10 Scene Graph pre-release
UI cases covering startup, navigation, isolated workout import, Train controls,
Data Generator, virtual gears, Workout Game, stop/continue/discard, recording
save, workout Save As and clean shutdown. The stop workflow proves that
continuing resumes growth of the same raw CSV, explicit Cancel removes it and
Save publishes a new activity JSON in the isolated athlete library. The Scene
Graph trace advanced 126.59 m at 115.87 median FPS with an 8.98 ms reported
p95, zero backward frames, zero skipped simulation ticks and zero unexpected
airborne frames. Quick 3D passed its eight renderer-dependent
UI cases under desktop OpenGL. Its 466-sample trace advanced 716.71 m at 56.22
median FPS, reached a 347 W target, reported a 10.79 ms stable p95 and 103 ms
maximum frame, and recorded no backward frames, simulation skips or unexpected
airborne frames. The forced-software Xvfb comparison exposed one 166 ms stall;
the desktop-OpenGL acceptance run did not reproduce it. Focused Quick 3D tests
also passed 62 executed cases with 13 platform skips, and the pre-release
analyzer/harness passed 22 Python regression tests.

Promotion used the verified release store. `GoldenCheetah-latest.AppImage`
points atomically to the new release, while the prior working image remains at
`GoldenCheetah-previous.AppImage`.

**REL-05 refresh (`aeef5e1`, 2026-08-26):** the performance-priority change
was rebuilt twice from independent clean worktrees in the Ubuntu Jammy,
glibc 2.35, Qt 6.8.3 Quick 3D release container. Both ELF and AppImage passes
were byte-identical. The promoted 267,504,120-byte AppImage has SHA-256
`d1fdc9ffd3d8fcf8c38d9da88db0b4bd64845efb54eb6d12c5fad478fb642299`;
its manifest has SHA-256
`4967752f51f080803cd2bc60fc0c08a921af466c23d9faa73be6c7b967dd8cfe`
and its CycloneDX SBOM has SHA-256
`dd6a553c42ad3b5d96028e344976b88f43a3eff836315232317e759e698da951`.
Packaging and the exact promoted local image passed configured Strava OAuth,
Linux keychain and isolated offscreen event-loop smoke gates. Local and remote
stable paths resolve to the same verified artifact; the former direct local
image is retained as `GoldenCheetah-before-aeef5e1.AppImage` and the remote
release store retains `56faa71` as its previous verified generation.

The build is not a release candidate while any P0 task remains open.
