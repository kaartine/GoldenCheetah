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

## P0 Vertical Slice

### `VS-01` Build canonical ordinary trail and terrain

- [ ] Author normal trail, shoulders, forest-floor tile and distant terrain.
- [ ] Preserve the 1.36 m full trail width and exact socket dead zones.
- [ ] Add material separation without photorealistic PBR noise.
- [ ] Keep the uneven forest horizon and stronger visible terrain relief.

**Tests:** seam vertices, normals, material coverage, camera sweep, background
hole check and trail-width invariant.

**Done when:** tiles form a continuous singletrack embedded in terrain rather
than a ribbon pasted onto a flat floor.

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
height remains only as a fallback for isolated render fixtures with no world
snapshot. Calibrated Box2D launch speeds retain the previous readability
thresholds; tests cap tabletop height at 1.8 m, continuous flight at 2 s and
per-20 ms height change at 0.25 m. Drop depth remains in the authoritative
road surface rather than a second
scripted rider offset, and a 3D regression verifies that completed-drop rider
height follows that negative surface. Safe bypass continues to force Box2D
ground following and cannot activate either airborne visual path.

### `PHY-02` Add feature-specific rider response

- [ ] Rollers pump/absorb while both wheels track the surface.
- [ ] Berm uses the same curved path for trail, rider lateral position and roll.
- [ ] Skinny uses subtle balance lean without random steering.
- [ ] Roots/rocks/slab move suspension and torso without camera vibration.
- [ ] Climb selects seated/standing effort and a bounded crest release.

**Tests:** per-feature pose and contact assertions plus approach/action/recovery
captures.

**Done when:** feature motion is recognisable even with grey materials.

### `PHY-03` Make virtual gearing and speed progressive

- [ ] Low gear remains slow on flat and climb.
- [ ] Gear changes alter torque/cadence response, not speed instantaneously.
- [ ] Grade, gravity, cadence, power and inertia contribute consistently.
- [ ] Keep manual virtual gears usable outside game mode.

**Tests:** fixed-input acceleration traces for every gear, flat/climb/descent
comparisons, gear-change continuity and trainer/non-trainer cases.

**Done when:** no gear input causes a visible speed teleport and representative
traces match agreed cycling ranges.

## P0 Feature Production

Every feature task includes main line, safe bypass where applicable, socketed
trail/terrain, rider response, catalog stills, completed/bypassed motion video,
and a runtime/geometry regression test.

- [ ] `FTR-01` Log over: buried volumetric log and front/rear clearance.
- [ ] `FTR-02` Bunny hop: distinct hurdle/kicker and short preload window.
- [ ] `FTR-03` Drop: sharp ledge, exposed face, lower landing and downward pose.
- [ ] `FTR-04` Rollers: rounded continuous crests/troughs and pump motion.
- [ ] `FTR-05` Berm: broad banked bowl and shared curved rider line.
- [ ] `FTR-06` Roots: branching embedded network and bounded roughness.
- [ ] `FTR-07` Rock garden: sunk varied rocks and a readable rideable line.
- [ ] `FTR-08` Rock slab: asymmetric mass, crest, sides and surface following.
- [ ] `FTR-09` Skinny: narrow deck, supports, ground clearance and transitions.
- [ ] `FTR-10` Climb: visible rising face, crest and effort pose.
- [ ] `FTR-11` Tabletop: promote the accepted vertical slice to production.

**Feature acceptance:** each feature is identifiable without its name, joins
ordinary trail without a crack or width jump, and produces a visibly correct
completed and bypassed outcome.

## P0 HUD And Testability

### `HUD-01` Replace the combined readiness percentage

- [ ] Show feature name and distance to decision/action point.
- [ ] Show power and cadence readiness separately.
- [ ] Use distinct prepare, act-now, committed, complete and bypass states.
- [ ] Keep cue duration short and aligned to physical feature position.

**Tests:** state-machine text/value tests, 720p/1080p screenshot bounds, long
translation layout and deterministic timing captures.

**Done when:** a rider can explain what to do, when to do it and why a line was
selected without reading logs.

### `HUD-02` Restore complete training instrumentation

- [ ] Add workout power profile and current-position cursor.
- [ ] Add current grade while preserving watts, target, cadence, heart rate,
  speed, gear, time, distance and actual presented FPS.
- [ ] Keep data readable over bright/dark terrain and at laptop resolution.

**Tests:** value binding, cursor progression, missing-sensor states and desktop/
laptop/mobile-aspect screenshots.

**Done when:** game mode does not remove information needed to execute and
review the workout.

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
