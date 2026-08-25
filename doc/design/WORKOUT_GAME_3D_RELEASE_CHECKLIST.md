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

- [ ] Confirm immutable newest-frame publication remains capacity one.
- [ ] Keep GLB/mesh generation, validation and loading out of the frame loop.
- [ ] Bound resident terrain, visible props, particles, triangles and draw calls.
- [ ] Preserve opaque depth-correct geometry and eliminate background holes.

**Tests:** runner/thread tests, frame-work counters, scene bounds, pixel checks,
memory plateau and target-laptop frame-time telemetry.

**Done when:** rendering cannot delay trainer control or recording and cannot
form an unbounded work queue.

## P0 Asset Pipeline

### `PIPE-01` Approve asset rights and provenance

- [x] Complete allow/conditional/reject license policy.
- [ ] Record exact source, author, license/version, attribution and hashes.
- [x] Reject NC, ND, editorial, personal-use, marketplace-only and unclear use.
- [ ] Record all modification and conversion steps.

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
- [ ] Adapt suitable models while preserving their provenance.
- [ ] Custom-author every gameplay-defining socket, silhouette and pivot.

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

- [ ] Author normal trail, shoulders, forest-floor tile and distant terrain.
- [x] Preserve the 1.36 m full trail width and exact socket dead zones.
- [x] Add material separation without photorealistic PBR noise.
- [ ] Keep the uneven forest horizon and stronger visible terrain relief.

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
26 tests with one opt-in video export skipped, and the geometry plus headless
view suites pass with confirmed ASan/UBSan linkage. A 1280 by 720 capture shows
the ordinary trail embedded in raised near terrain without background holes.
Distant terrain art and final relief acceptance remain open.

### `VS-02` Build complete tabletop and bypass

- [x] Model belly, lip, deck, knuckle, landing and joined side terrain.
- [ ] Model a readable split, safe line and merge before the decision marker.
- [x] Keep the canonical measured profile as the physics authority.
- [x] Place prepare, decision, action, lip, apex and landing markers.

**Tests:** exact sockets, named anatomy silhouette, road/contact alignment,
completed/bypass captures and bounded speed-dependent flight.

**Done when:** a rider identifies the jump and both lines without HUD text and
the wheels meet the landing surface at the reported landing event.

**Current evidence:** the deterministic Blender source now generates tapered
joined terrain and a raised bypass in the same 78-vertex/96-triangle mesh.
The production ViewModel derives the asset socket transform and difficulty
scale from the authoritative road profile, and the actual game QML renders
the packaged Balsam component. Khronos validation, two reproducible Blender
and Balsam runs, 13 policy tests, 14 interactive X11 view tests and a
three-camera 1,080-frame motion audit pass. The split and jump silhouette are
still too subdued for HUD-free acceptance, so visual readability remains
open.

### `VS-03` Integrate rider and bike

- [ ] Select/adapt or author a low-poly 29-inch MTB and articulated rider.
- [ ] Correct forward orientation, wheelbase, axle, crank and steering pivots.
- [ ] Add high-contrast pixel-textured materials and a ground-fixed shadow.
- [ ] Add pedal, coast, preload, air, land, absorb, lean and bypass clips.

**Tests:** node/pivot validation, cadence-to-crank synchronization, wheel
contact, no root motion, animation bounds and catalog/video captures.

**Done when:** pedalling, take-off, air and landing are readable at the normal
camera distance and the bicycle never appears to travel sideways.

### `VS-04` Validate the vertical slice end to end

- [ ] Run deterministic complete and bypass sessions with Data Generator.
- [ ] Run a real trainer session without touching production athlete data.
- [ ] Verify trainer target, recording and feature outcome agree.
- [ ] Measure target-laptop frame time and simulation skipped ticks.

**Done when:** one tabletop workout can be ridden, understood, saved and
replayed without a visual, physics, recording or performance release blocker.

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
- [ ] Skinny uses subtle balance lean without random steering.
- [ ] Roots/rocks/slab move suspension and torso without camera vibration.
- [ ] Climb selects seated/standing effort and a bounded crest release.

**Tests:** per-feature pose and contact assertions plus approach/action/recovery
captures.

**Done when:** feature motion is recognisable even with grey materials.

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
- [ ] `FTR-06` Roots: branching embedded network and bounded roughness.
- [ ] `FTR-07` Rock garden: sunk varied rocks and a readable rideable line.
- [ ] `FTR-08` Rock slab: asymmetric mass, crest, sides and surface following.
- [ ] `FTR-09` Skinny: narrow deck, supports, ground clearance and transitions.
- [ ] `FTR-10` Climb: visible rising face, crest and effort pose.
- [ ] `FTR-11` Tabletop: promote the accepted vertical slice to production.

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
`/home/jkaartinen/Documents/personal/gc-ui-ftr04` on the build host.

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

- [ ] Generator follows the selected game/workout target by default.
- [ ] Add deterministic over-target, on-target, under-target and cadence modes.
- [ ] Add completed and bypassed scripts for all eleven features.
- [ ] Expose generator state clearly without affecting normal devices.

**Tests:** target equality, outcome matrix, reproducible telemetry trace and
isolation from real device configuration.

**Done when:** automated UI videos can reliably demonstrate both lines of every
feature without a trainer.

## P1 Art, Environment And Feedback

- [ ] `ART-01` Apply one coherent low-poly/pixel-textured palette and atlas.
- [ ] `ENV-01` Add varied forest silhouettes, uneven terrain and restrained fog.
- [ ] `ENV-02` Eliminate tree pop, floating bases and buried visible geometry.
- [ ] `FX-01` Add bounded contact shadow, landing dust and success feedback.
- [ ] `FX-02` Add restrained feature/camera punctuation after physics is stable.
- [ ] `AUDIO-01` Add optional low-cost feature and landing audio after visuals.

**Acceptance:** art improves depth and arcade identity without hiding the path,
exceeding budgets or changing physics.

## Diagnostics And Performance

- [ ] `DIA-01` Count actual presented frames and report p50/p95/p99 frame time.
- [ ] `DIA-02` Count backward distance, unexplained stationary frames, skipped
  simulation ticks, renderer queue depth and long frame work.
- [ ] `DIA-03` Trace feature phase, route, readiness, action distance, rider
  contacts, camera transform and active asset/LOD identities.
- [ ] `DIA-04` Capture direct snapshots and videos from isolated deterministic
  sessions.
- [ ] `PERF-01` Stay below 30,000 visible triangles and 50 draw calls initially.
- [ ] `PERF-02` Hold 60 Hz presentation budget on the target Intel GPU without
  increasing Bluetooth, trainer-control or recording latency.

## Release And Legacy Retirement

- [ ] `REL-01` Run unit, integration, visual, UI, recording and Bluetooth tests.
- [ ] `REL-02` Verify stop/save/cancel/continue and accidental-stop recovery.
- [ ] `REL-03` Verify AppImage startup, QML/Quick 3D modules, assets, licenses,
  attribution, SBOM and isolated production-like athlete data.
- [ ] `REL-04` Complete user A/B ride on the target laptop and real trainer.
- [ ] `REL-05` Publish a test AppImage to the stable local/remote release path.
- [ ] `REL-06` Remove the legacy renderer only after all acceptance gates pass.

The build is not a release candidate while any P0 task remains open.
