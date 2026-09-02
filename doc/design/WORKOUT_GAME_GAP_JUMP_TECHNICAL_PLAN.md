# Workout Game Gap Jump Technical Plan

## Status

Implemented and under packaged-release acceptance. This document defines the
implementation contract for the three-line gap jump.

## Scope

Add a `GapJump` technical terrain feature with three adjacent jump lines:

1. short gap on the left;
2. medium gap in the centre;
3. long gap on the right.

The game previews a line from fixed-step telemetry, then evaluates the actual
launch in the final `10-3 m`. It moves toward the current candidate with a
rate-limited continuous path and locks one reachable line at `3 m`. Riders
who do not qualify take a separate rollable bypass. A workout must continue and
record normally regardless of feature outcome.

The first version is a deterministic authored feature, not a collision game.
It has no crash, rewind, time penalty, trainer resistance change, or direct
steering input.

## Existing Architecture Inspected

The implementation must extend the existing path rather than create a second
feature engine:

- `WorkoutGameCourse` and `WorkoutGameWorld` own persisted/runtime feature and
  terrain kinds.
- `WorkoutGameDistanceCourse` and `WorkoutGameCourseDocument` generate and
  persist deterministic MTB courses. The sidecar currently uses schema version
  1 and stores terrain as a string.
- `WorkoutGameRoadCourse` turns sections into connected road pieces. Challenge
  placement, obstacle anchors and bypass sockets are authored here.
- `WorkoutGameFeatureChallenge` measures effort, cadence, speed and adherence.
  Other jump cues use the newest 1.5 seconds of samples in
  `WorkoutGameSimulation`; gap jumps use their dedicated distance-based launch
  window.
- `WorkoutGameSimulation` advances in deterministic 50 ms steps and owns
  outcomes and score. It accepts a gap-jump outcome only while that section is
  active and awards its completion bonus at most once.
- `WorkoutGameFeatureRuntime` measures and locks the gap-jump line, then maps
  the result onto approach, measure, committed, action and recovery phases and
  generates continuous rider offsets.
- `WorkoutGameEngine` commits the runtime's locked gap result into simulation
  in the same frame, keeping the published route, result, readiness and score
  consistent.
- `WorkoutGameTabletopGeometry` is the closest jump reference. It provides
  sockets, a curved surface, speed support, landing planning and a bypass.
- `WorkoutGameTrailBranch` supplies the current smooth split-and-merge curve.
- `WorkoutGame3DGeometry` builds bounded procedural Quick 3D mesh layers,
  including the trail and bypass. `WorkoutGame3DChunkBuilder` can prepare heavy
  geometry away from presentation.
- `WorkoutGame3DViewModel` projects the authoritative road and runtime snapshot
  into rider pose, camera, HUD properties and bounded visible feature data.
- `WorkoutGame3D.qml` owns materials and presentation only. It must not decide
  line selection or feature success.
- `WorkoutGameFeatureLab` supplies deterministic pass and bypass scenarios and
  the all-feature review course.
- `WorkoutGameRunner` runs the optional game worker at lower process priority.
  Device I/O, trainer control and recording stay on GoldenCheetah's existing
  normal-priority paths.

## Invariants

The implementation is accepted only if all of these remain true:

- Simulation decisions depend only on course data, ordered telemetry samples
  and fixed 50 ms simulation steps. Render FPS and wall-clock jitter cannot
  change the selected line or result.
- No new real-time thread is introduced. Gap selection runs in the existing
  game worker; static mesh generation uses the existing chunk path.
- QML consumes immutable snapshots. It cannot feed presentation timing back
  into simulation.
- The feature entry and exit are exact road sockets: position, heading,
  elevation, grade and half-width agree within the existing road tolerances.
- Rider lateral position and ground contact use the same selected branch and
  surface functions as the rendered mesh.
- Lateral steering starts at `12 m` and remains acceleration- and speed-limited.
  Line selection locks at `3 m` and never changes after lock, even if telemetry
  becomes stale or effort changes.
- The rider never teleports, travels backwards, falls into the gap, or leaves
  the course. Failure means a visible safe bypass and no feature bonus.
- Supported airtime is approximately 0.25-1.40 seconds and never exceeds the
  global 2.0-second gap-jump ceiling. Longer arcade flights belong to a later
  special feature, not this speed-matched line choice.
- Gap logic cannot modify trainer targets, workout time, recording samples or
  device connection state.

## User Experience

### Approach

At approximately 45 m before take-off, all three lips and landing ramps must be
readable. The HUD shows `GAP JUMP`, predicted approach speed, and the provisional
line (`SHORT`, `MEDIUM`, `LONG`, or `BYPASS`). The preview may change only under
the hysteresis rules below. It does not move the rider.

At `12 m` before take-off the rider starts a restrained, rate-limited move
toward the provisional line. `BUILD SPEED` remains a preview until the launch
window starts at `10 m`. The best complete rolling `500 ms` speed and a
continuous `500 ms` at-or-above-target power hold are measured only in the
`10-3 m` window. At `3 m` the highest speed-matched line that can still be
reached without a lateral snap locks, and the HUD changes to `LINE LOCKED`.

### Action

The three jump lines have visibly different gaps and landing positions. The
rider follows the selected line's take-off socket, authored air path and landing
socket. A successful landing transitions into the shared runout before all
branches merge.

The safe bypass is outside the three jump lines, visibly rollable, and uses the
existing `SafeBypass` result vocabulary. It is not presented as a fourth jump.

### Result

A completed jump awards the existing feature bonus once. The result text also
names the line, for example `MEDIUM GAP CLEAN`. A bypass displays `SAFE LINE`
and awards no feature bonus. Neither result interrupts the workout.

## Data Model

### Persisted Course Schema

Append `WorkoutGameTerrainKind::GapJump` to the terrain enum. Appending preserves
the numeric values currently consumed by QML. Use `gap-jump` in sidecar JSON and
`Gap jump` in CRS cue text and UI labels. It remains a
`WorkoutGameFeature::SprintJump`; a new high-level workout feature enum is not
needed.

The sidecar already persists terrain by name, so adding `gap-jump` does not by
itself change the schema shape and does not require a schema-version bump.
Existing version-1 files and their encoded CRS output remain unchanged. Never
reinterpret an old `Tabletop` as a gap jump during load. Regeneration may choose
a gap only when the user explicitly regenerates a course with the current
generator.

Do not persist a selected line. It depends on live telemetry. Persist only the
existing terrain, difficulty, visual variant and seed. Geometry and thresholds
are pure deterministic derivatives of those fields.

### Authored Profile

Add a pure, header-only `WorkoutGameGapJumpGeometry` module, parallel to
`WorkoutGameTabletopGeometry`:

```cpp
enum class WorkoutGameGapJumpLine : std::uint8_t {
    None,
    Short,
    Medium,
    Long
};

struct WorkoutGameGapJumpLineDefinition {
    WorkoutGameGapJumpLine id;
    double lateralMeters;
    double gapLengthMeters;
    double minimumAcquireSpeedKph;
    double minimumHoldSpeedKph;
    double nominalFlightSeconds;
    double lipHeightMeters;
    double landingDropMeters;
};

struct WorkoutGameGapJumpGeometryProfile {
    bool ready;
    double difficulty;
    double socketHalfWidthMeters;
    double prepareLeadMeters;
    double launchWindowLeadMeters;
    double lockLeadMeters;
    double splitLengthMeters;
    double mergeLengthMeters;
    double bypassLateralMeters;
    double featureStartMeters;
    double featureEndMeters;
    int speedWindowMilliseconds;
    int powerHoldMilliseconds;
    std::array<WorkoutGameGapJumpLineDefinition, 3> lines;
};
```

Initial values at difficulty 0.5 are:

| Line | Centre | Gap | Acquire | Hold | Nominal air |
| --- | ---: | ---: | ---: | ---: | ---: |
| Short | -2.3 m | 1.8 m | 14.4 km/h | 13.1 km/h | 0.55 s |
| Medium | 0.0 m | 3.2 m | 18.7 km/h | 17.5 km/h | 0.78 s |
| Long | +2.3 m | 4.7 m | 23.8 km/h | 22.5 km/h | 1.05 s |

Difficulty may vary gap length by at most +/-15%, acquire/hold speed by at most
+/-10%, and lip height by at most +/-20%. Line ordering, centres and exact
sockets do not vary. Clamp computed flight time to `[0.25, 1.40]` seconds and
reject non-finite or inverted profiles in tests.

### Compiled Road Data

Add a road-owned gate rather than overloading the single generic bypass fields:

```cpp
struct WorkoutGameRoadGapJumpGate {
    bool enabled = false;
    double prepareDistanceMeters = 0.0;
    double launchWindowStartDistanceMeters = 0.0;
    double lockDistanceMeters = 0.0;
    double splitStartDistanceMeters = 0.0;
    double mergeEndDistanceMeters = 0.0;
    std::array<WorkoutGameRoadGapJumpLine, 3> lines;
};
```

Each compiled line stores absolute take-off and landing distances, lateral
centre, acquire/hold threshold and flight duration. `WorkoutGameRoadPiece`
owns this gate beside `challenge`. The generic challenge still owns scoring,
measurement and `SafeBypass`; the gap gate owns multi-line geometry and choice.

Extend `WorkoutGameSimulationSnapshot` and
`WorkoutGameFeatureRuntimeSnapshot` with:

- provisional and locked `WorkoutGameGapJumpLine`;
- `gapLineLocked`;
- predicted approach speed;
- selected gap length and flight duration.

Keep `WorkoutGameRoute` as `MainLine` or `SafeBypass`. All three jump lines are
main-line variants. This avoids changing route semantics for every existing
feature.

## Selection Algorithm

Use `WorkoutGameGapJumpSelector` for the long-range preview and a separate pure
`WorkoutGameGapJumpLaunchWindow` in `WorkoutGameFeatureRuntime` for the final
decision. Both reset on section change, seek and backwards time/distance. They
receive sanitized fixed-step samples and never use presentation timestamps.

### Predictor

Maintain a bounded 2.0-second ring of `(speedKph, effortRatio, durationMs)`.
Compute duration-weighted means for the newest 1.0 second and preceding 1.0
second. The deterministic prediction is:

```text
trendKphPerSecond = clamp(newMeanSpeed - oldMeanSpeed, -3.0, +3.0)
etaSeconds = clamp(timeToTakeoffSeconds, 0.0, 2.0)
effortCorrectionKph = clamp((meanEffortRatio - 1.0) * 6.0, -2.0, +3.0)
predictedSpeedKph = clamp(
    newMeanSpeed + trendKphPerSecond * etaSeconds + effortCorrectionKph,
    0.0, 60.0)
```

Before a full history exists, use the duration-weighted available history;
with no valid speed, recommend bypass. Sanitize NaN, infinity and negative
values before storing them. Use integer milliseconds for all window accounting.

### Launch-window qualification

Effort is a qualification condition, not a replacement for speed. From `10 m`
until `3 m`, retain the best complete duration-weighted rolling `500 ms` speed
average. A single sample cannot select a longer line. In the same interval,
actual power must stay at or above target continuously for `500 ms`; one sample
below target resets that hold. Invalid or stale telemetry clears both histories.
Cadence is displayed but is not a gap-jump gate in version 1.

Select the longest line whose acquire speed is met. If effort is below target or
short-line speed is not met, select bypass. This preserves the simple rule that
the user succeeds by meeting the current workout target while speed decides
which gap is appropriate.

### Preview, reachability and locking

Before the launch window, retain the current preview line while predicted speed remains
above that line's hold threshold. Promote only after the next line's acquire
threshold has been met continuously for 300 ms. Demote after its hold threshold
has been missed continuously for 500 ms. Bypass-to-short uses the same 300/500
ms timing. This hysteresis affects preview presentation only. Ties choose the
shorter line.

During the launch window, choose from the best complete rolling speed window.
At the `3 m` lock, downgrade a late promotion to the longest line whose centre
is reachable with the bounded lateral velocity; never teleport to honor an
unreachable candidate.

At `lockDistanceMeters`, copy the provisional line to the locked line exactly
once. If history is insufficient, telemetry is stale, or any selector input is
invalid, lock `None` and use `SafeBypass`. Never unlock or change line until the
next gap feature. Replay and different render frame rates must produce the same
locked line for identical fixed-step inputs.

## Branch and Socket Geometry

Add `WorkoutGameGapJumpBranch`, a pure geometry/path module. It provides the
centre and contact surface for each jump line and the bypass from absolute road
distance. It should reuse the mathematical policy of `WorkoutGameTrailBranch`
but use a quintic smoothstep (`6t^5 - 15t^4 + 10t^3`) so position, velocity and
acceleration are all continuous at split and merge.

- The shared entry socket remains the normal 1.36 m singletrack.
- All branches start at the exact entry centre, heading, grade and elevation.
- Each branch reaches its fixed lateral centre over 18 m.
- Take-off lips begin after the split transition has completed.
- Each landing has an authored downslope and at least 12 m of straight runout.
- Branches merge only after the longest landing and its runout.
- Every branch returns exactly to the shared exit socket.
- The bypass uses the same split and merge endpoints but stays outside the
  short line with enough tyre and handlebar clearance.

Use one source of truth for branch centre, tangent, surface elevation and local
normal. Road sampling, rider placement, physics contact and mesh vertices call
that same module. Do not duplicate the curve in QML or asset transforms.

Add `WorkoutGame3DGeometry::Layer::GapJump`. Its one bounded mesh contains the
three take-off ramps, three real voids, three landing ramps, line edges and the
shared split/merge trail. Suppress the ordinary `Trail` layer only inside the
gap-owned corridor so it cannot bridge the holes or z-fight. The mesh must own
the complete visible socket transition; decorative rocks and plants remain
outside its clearance envelope.

The initial implementation should be procedural and material-compatible with
the accepted low-poly/pixel-textured style. A later GLB art pass may replace
decorative faces, but never the authoritative contact surface or sockets.

## Airtime and Motion

The selected line owns a deterministic authored trajectory. Horizontal motion
continues with course progress; vertical motion uses a bounded asymmetric arc
with a 35% ascent and 65% descent, landing at the selected line's exact landing
socket. This deliberately avoids an unbounded point-mass ballistic arc and
keeps the compact flight aligned with the authored landing.

`WorkoutGameFeatureRuntime` emits take-off once using the existing `actionId`.
The physics layer consumes the selected line, take-off/landing distances and
flight duration. It must preload suspension, release at the lip, report
airborne only between the two contact events, pitch nose-up then level the bike,
and absorb landing without a second launch. Maximum visual apex is 2.4 m above
the line joining lip and landing even when flight time reaches the hard ceiling.

The world remains authoritative for airborne state, suspension and landing
impact. The ViewModel must not add a second jump arc on top of it.

## Runtime Phases and Safe Behavior

Map the gap onto existing feature phases:

- `Approach`: before the 45 m prepare point; HUD may remain hidden.
- `Measure`: prepare point through the `10-3 m` launch window; show preview,
  rolling speed, power hold and provisional line.
- `Committed`: line locked, rider follows the continuous split.
- `Action`: selected lip to selected landing, or the equivalent bypass span.
- `Recovery`: landing/runout through the exact merge socket.

If no jump line qualifies at lock, finalize the challenge as `Bypassed`, route
the rider to `SafeBypass`, force ground following and suppress jump triggering.
If telemetry expires after a jump line is locked, keep the line and authored
flight. This is safer than a late cross-course move and deterministic because
the achievement was already decided. The stale telemetry still becomes zero in
the existing runner and recording/device behavior remains unchanged.

Course construction must reject a gap profile that cannot fit its complete
prepare, split, longest landing, runout and merge span. Generated courses fall
back deterministically to `Tabletop`, then `LogOver` if needed. Explicit invalid
course data fails validation; it must never render a partial gap or silently
change sockets at runtime.

## Course Generation

Integrate the feature in three controlled places:

1. Add one `GapJump` after the current tabletop in Feature Lab, followed by its
   normal recovery. Feature Lab pass input must qualify for at least medium;
   add explicit short, medium, long and bypass scenarios for focused tests.
2. In `WorkoutGameDistanceCourse::applyTechnicalPalette`, high-technicality
   `SprintJump` sections choose deterministically among `LogOver`, `Tabletop`
   and `GapJump` using `visualVariant % 3`. Balanced and Workout First do not
   gain gaps in the first release.
3. After section distance is known, apply deterministic fit normalization. A
   gap requires the profile's full minimum length; otherwise fall back without
   changing seed, duration, target power or adjacent section sockets.

Generated geometry remains stable for the same persisted seed, difficulty and
visual variant. Reopening a saved course cannot reshuffle line order or gaps.

## ViewModel, QML and HUD

Add a constant `gapJumpGeometry` property and a `gapJumpGeometryModel` using the
existing dirt material family. Add read-only scene properties for provisional
line, locked line, predicted speed, selected gap length and line-lock state.

The HUD must communicate the decision without adding steering controls:

- before lock: predicted speed plus recommended line;
- after lock: selected line and gap length;
- bypass: `SAFE LINE`;
- action: existing power and cadence values remain visible;
- recovery: line-specific clean result.

Use compact text and three small line indicators, ordered left-to-right exactly
like the trail. The selected indicator changes color; no flashing, large modal
or full-screen effect. QML numeric terrain checks must use a named value exposed
by the ViewModel or registration helper for the new code; do not add another
raw enum integer comparison.

Camera look-ahead must include the longest landing during approach. Camera yaw,
target and FOV remain under the existing presentation controller and receive no
per-render-frame line-selection logic.

## Trainer, Recording and Threading

Gap processing has a strict presentation-only budget:

- selector work is bounded O(1) per fixed step with at most 2 seconds of samples;
- no allocation occurs in the step after selector buffers are configured;
- no locks are added to trainer or recording paths;
- no QML or render-thread call is made from the game worker;
- static gap mesh generation is bounded by existing sample/triangle budgets and
  published through the existing newest-result chunk mechanism;
- the lower-priority game worker may skip presentation catch-up, but identical
  accepted simulation ticks still produce identical choices;
- telemetry expiry, pause, resume, seek and stop follow existing Runner lifecycle
  generation and publication-epoch rules.

A game slowdown may lower visual update frequency. It must not delay trainer
commands or lose recorded watts, cadence, heart rate or speed.

## Test-First Strategy

Write each failing test before its production change. Keep tests deterministic;
do not use sleep timing for selector or physics assertions.

### 1. Profile and Schema Tests

- Add `testWorkoutGameGapJumpGeometry` for finite ordered lines, difficulty
  bounds, exact sockets, non-overlapping gaps and bounded flight limits.
- Extend `testWorkoutGameCourseDocument` with a version-1 `gap-jump` round trip,
  malformed value rejection and unchanged existing version-1 CRS output.
- Extend distance-course tests for deterministic palette selection and short
  section fallback.

### 2. Selector Tests

- Add `testWorkoutGameGapJumpSelector` with table-driven traces for bypass,
  short, medium and long.
- Assert 300 ms promotion and 500 ms demotion hysteresis for long-range preview.
- Assert exact `10 m` launch start, `3 m` lock, best rolling `500 ms` speed,
  continuous `500 ms` power, invalid telemetry reset and late-line reachability.
- Assert effort just below/at target, missing target fallback, stale telemetry,
  NaN/inf/negative sanitization and incomplete history.
- Feed identical telemetry with different presentation-frame groupings and
  assert the same provisional history, locked line and result.
- Assert seek/reset cannot leak a prior line and that post-lock telemetry cannot
  change it.

### 3. Road and Socket Tests

- Extend `testWorkoutGameRoadCourse` to verify fit/fallback, absolute anchors,
  exact entry/exit connectors and stable output for the same seed.
- Sample every branch densely. Assert finite centre/tangent/normal, monotonic
  forward distance, C2 split/merge continuity, no branch crossing, and exact
  return to the shared socket.
- Assert the bypass clears all three gaps and every landing has its minimum
  runout.

### 4. Simulation and Runtime Tests

- Extend `testWorkoutGameSimulation` with fixed-step line lock, one bonus only,
  safe bypass, pause/resume and catch-up/seek traces.
- Extend `testWorkoutGameFeatureRuntime` for phase boundaries, selected-line
  lateral continuity, correct selected take-off/landing, one `actionId`, and no
  jump on bypass.
- At 50 ms steps, assert lateral displacement is continuous and below 0.05 m per
  step at the maximum supported course rate.
- Assert airborne duration is within the selected line's bounded range, begins
  at the selected lip, ends at the selected landing, and cannot retrigger on
  recovery.
- Extend engine/runner tests to prove telemetry staleness after lock does not
  switch line and does not affect lifecycle publication guarantees.

### 5. Geometry and View Tests

- Extend `testWorkoutGame3DGeometry` for bounded vertex/triangle counts, three
  distinct holes, no trail bridge, socket-coincident vertices, valid bounds and
  deterministic byte-identical mesh data.
- Extend chunk-builder tests so gap geometry is generated and discarded under
  the same generation rules as other layers.
- Extend `testWorkoutGame3DView` to assert rider position equals the selected
  branch centre/contact surface, geometry is present, all lines are readable,
  HUD ordering matches physical left/centre/right ordering, and bypass differs
  visibly without moving the camera abruptly.
- Add offscreen image checks at approach, lock, apex, landing and merge. Use
  image-difference thresholds only for structural visibility; retain human
  review for art quality.

### 6. Feature Lab and Pre-release UI

- Extend Feature Lab ordering and duration assertions with `GapJump`.
- Add deterministic synthetic traces that force all four outcomes.
- Extend `analyze_workout_game.py` trace assertions with selected line, lock
  distance, lateral step, take-off, landing, airborne duration, backwards
  movement and unexpected airborne counters.
- Record a Quick 3D Feature Lab video and stills for short, medium, long and
  bypass from an isolated temporary athlete. The log must explicitly report
  `Workout Game renderer selection: Qt Quick 3D`.
- Run a longer packaged-AppImage synthetic session and then a physical trainer
  session. Verify trainer targets and recorded samples independently from the
  visual trace before release acceptance.

## Implementation Order

1. Land profile, enum and schema migration tests; then implement the persisted
   type and pure geometry profile.
2. Land selector tests; then implement deterministic prediction, hysteresis and
   locking without rendering.
3. Land road/socket tests; then compile gap gates and shared branch surfaces.
4. Land simulation/runtime tests; then integrate outcome, line lock, lateral
   path and bounded authored flight.
5. Land mesh tests; then add the gap Quick 3D geometry layer and suppress the
   underlying trail only in the owned corridor.
6. Land ViewModel/QML tests; then add HUD and visual highlighting.
7. Add Feature Lab and generator integration only after focused short/medium/
   long/bypass tests pass.
8. Run all Workout Game unit suites, ASan/UBSan where available, offscreen UI,
   packaged AppImage synthetic acceptance and physical trainer acceptance.
9. Commit each green milestone separately. Do not combine schema, selector,
   geometry and UI into one unreviewable change.

## Expected Production Modules Touched

New modules:

- `src/Train/WorkoutGameGapJumpGeometry.h`
- `src/Train/WorkoutGameGapJumpBranch.h`
- `src/Train/WorkoutGameGapJumpSelector.h`
- `src/Train/WorkoutGameGapJumpSelector.cpp`

Existing model and persistence:

- `src/Train/WorkoutGameWorld.h`
- `src/Train/WorkoutGameCourseDocument.h/.cpp`
- `src/Train/WorkoutGameCourseCrsExporter.cpp`
- `src/Train/WorkoutGameDistanceCourse.cpp`
- `src/Train/WorkoutGameDistancePlayback.cpp`
- `src/Train/WorkoutGameFeatureCatalog.h`
- `src/Train/WorkoutGameFeatureChallenge.cpp`

Existing road, simulation and presentation:

- `src/Train/WorkoutGameRoadCourse.h/.cpp`
- `src/Train/WorkoutGameSimulation.h/.cpp`
- `src/Train/WorkoutGameFeatureRuntime.h/.cpp`
- `src/Train/WorkoutGameEngine.cpp`
- `src/Train/WorkoutGame3DGeometry.h/.cpp`
- `src/Train/WorkoutGame3DChunkBuilder.cpp`
- `src/Train/WorkoutGame3DViewModel.h/.cpp`
- `src/Train/WorkoutGameFeatureHud.h/.cpp`
- `src/Train/WorkoutGameFeatureLab.cpp`
- `src/Train/qml/WorkoutGame3D.qml`
- relevant qmake source lists and unit-test `.pro` files.

Tests expected to be added or extended:

- `unittests/Train/workoutGameGapJumpGeometry/`
- `unittests/Train/workoutGameGapJumpSelector/`
- course document, distance course, road course, simulation, feature runtime,
  engine, runner, 3D geometry, chunk builder, 3D view, Feature Lab, HUD and
  pre-release UI analyzer suites.

## Risks and Mitigations

| Risk | Mitigation |
| --- | --- |
| Recommendation flickers near a threshold | Timed acquire/hold hysteresis for preview; rolling `500 ms` speed for launch. |
| Late line change teleports rider | Rate-limited steering from `12 m`, reachability downgrade at `3 m`, stable action ID and immutable post-lock line. |
| Mesh and rider disagree | One branch module supplies mesh, road sample, contact and rider centre. |
| Base trail fills the gap | Gap layer owns a tested corridor and ordinary trail rendering is suppressed there. |
| Long airtime creates absurd height | Authored bounded arc, 2.4 m apex cap and 2 s hard time cap. |
| Gap does not fit a generated interval | Deterministic post-length fallback to tabletop/log-over. |
| New schema breaks old courses | Decode and migrate v1 unchanged; append enum; never regenerate on load. |
| Feature work delays trainer/recording | O(1) selector, bounded meshes, existing lower-priority worker and no device-path locks. |
| Raw enum integers drift in QML | Expose named terrain/line values; preserve existing numeric enum ordering. |
| Art looks pasted onto the trail | Procedural socket-owned earth shapes and shared materials before decorative asset work. |
| Synthetic success hides physical issues | Separate visual trace, recorded telemetry comparison and physical trainer gate. |

## Acceptance Gate

Production release requires all automated tests above plus evidence that:

- short, medium, long and bypass are selected by the specified traces;
- no trace contains a lateral step over 0.05 m, backwards movement, duplicate
  take-off, unexpected airborne frame or two-second airtime-ceiling violation;
- every rendered line is identifiable before lock and visibly joins the same
  trail at both sockets;
- the packaged Quick 3D AppImage completes synthetic and physical-trainer runs
  without changing trainer targets or recorded telemetry;
- the target laptop remains responsive and frame/geometry budgets do not regress;
- interactive review accepts the line spacing, camera readability, jump feel
  and safe bypass before general course generation is enabled.
