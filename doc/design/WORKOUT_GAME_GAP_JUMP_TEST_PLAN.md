# Workout Game Three-Line Adaptive Gap Jump Test Plan

## Purpose

This document defines the test-first acceptance contract for a gap-jump trail
feature with three adjacent jump lines. The feature adapts the selected line to
the rider's stable approach speed, evaluates whether the rider supplied the
required effort, moves to the selected line before take-off, follows one bounded
physical flight and lands on the matching transition.

The tests are the specification. Production implementation starts only after
the relevant tests exist and fail for the expected missing behavior. A passing
unit test alone does not make the feature releasable: packaged Qt Quick 3D
evidence, recording agreement, target-laptop performance and visual review are
all required.

## Existing Test Surface

Extend the current layers rather than creating a parallel harness:

| Layer | Existing location | Gap-jump responsibility |
| --- | --- | --- |
| Course and catalog | `unittests/Train/workoutGameCourse`, `workoutGameRoadCourse` | Type, authored parameters, deterministic road-piece expansion and validation |
| Challenge and simulation | `workoutGameFeatureChallenge`, `workoutGameSimulation` | Effort window, speed selection, hysteresis, commitment and fallback |
| Feature runtime | `workoutGameFeatureRuntime` | Phase, line lock, lateral path, jump pose, airtime and result persistence |
| Road and physics | `workoutGameRoadPhysics`, `workoutGameWorld` | Per-line collision surface, take-off, flight and landing contact |
| Mesh and sockets | `workoutGameMesh`, `workoutGame3DGeometry`, `workoutGame3DTerrainProfile` | Three lanes, gap holes, landings, fallback and exact trail/terrain seams |
| Asynchronous renderer data | `workoutGame3DChunkBuilder`, `workoutGame3DView` | Bounded geometry work, immutable selected-line state and Quick 3D presentation |
| Determinism | `workoutGameEngine`, `workoutGameReplay`, `workoutGameRunner` | Fixed-step agreement, bit-exact replay and no stale-generation line changes |
| Feature Lab | `workoutGameFeatureLab` | Short, medium, long and fallback scenarios in a compact deterministic course |
| Packaged UI | `unittests/Gui/preReleaseUi` | Data Generator, trace analysis, recording reconciliation, stills and video |
| Asset policy | `unittests/Build/workoutGameAssets` and `contrib/workout-game-assets` | Authored assets, license/provenance, scale, nodes, pivots and socket metadata |

The current tabletop and drop tests provide patterns for a real hole in trail
indices, Box2D surface absence, bounded flight, landing and a grounded bypass.
The gap jump must not modify tabletop behavior to impersonate three lines.

## Acceptance Model

### Canonical line contract

The first implementation uses one canonical profile shared by course, road,
physics, runtime and rendering. Values below are acceptance constants; changing
one requires changing the specification and tests in the same reviewed commit.

| Line | Stable ID | Centre offset | Gap length | Cold-selection speed |
| --- | --- | ---: | ---: | ---: |
| Short | `short` | `-2.30 m` | `1.80 m` | `4.00` to below `5.20 m/s` (`14.4-18.7 km/h`) |
| Medium | `medium` | `0.00 m` | `3.20 m` | `5.20` to below `6.60 m/s` (`18.7-23.8 km/h`) |
| Long | `long` | `+2.30 m` | `4.70 m` | at least `6.60 m/s` (`23.8 km/h`) |

The offsets are viewed in the canonical road frame. Mirroring a generated tile
may reverse their world-space side but must not change the stable IDs, lengths
or selection result.

The three lines must have visibly different take-off lips, real empty gaps and
matching landing transitions. They leave and rejoin the ordinary `1.36 m`
singletrack through one canonical entry and one canonical exit socket. A
grounded roll-around route is the insufficient-effort fallback; it is not a
fourth gap line and does not count as successful completion.

### Selection state machine

Cold selection uses half-open intervals: below `4.00 m/s` selects fallback,
exactly `4.00 m/s` selects `short`, exactly `5.20 m/s` selects `medium` and
exactly `6.60 m/s` selects `long`. Invalid, negative or non-finite speed fails
closed to the fallback route.

Before commitment, selection uses a `0.35 m/s` Schmitt band:

- fallback changes to `short` only at or above `4.35 m/s`.
- `short` changes to fallback only below `3.65 m/s`.
- `short` changes to `medium` only at or above `5.55 m/s`.
- `medium` changes to `short` only below `4.85 m/s`.
- `medium` changes to `long` only at or above `6.95 m/s`.
- `long` changes to `medium` only below `6.25 m/s`.
- A direct `short` to `long` sample is allowed only when the filtered speed is
  at or above `6.95 m/s`; the result then locks to `long` without an
  intermediate presented frame.

Selection consumes the authoritative fixed-step filtered speed, not render
FPS, instantaneous cadence-derived speed or wall-clock sample timing. Once the
decision gate is crossed, `{actionId, selectedLine, route, outcome}` is
immutable through landing and recovery. Later speed, power, gear or cadence
changes cannot switch lanes.

### Effort and fallback contract

Speed chooses the suitable gap length; it does not by itself complete the
challenge. The existing feature challenge machinery measures the canonical
six-metre approach window. Completion requires all configured power, cadence
and minimum-speed readiness components to equal `1.0` at the decision gate.

If readiness is below `1.0`, the rider commits to the grounded fallback before
the gap split. The fallback must:

- be selected early enough to be visually understandable;
- contain continuous rideable trail and physics contact;
- never cross any of the three empty gaps;
- never report airborne, landing impact, success or bonus points;
- retain the measured failed component for the HUD and trace result;
- rejoin the exact ordinary-trail exit socket.

No late promotion is allowed after fallback commitment. No hidden rescue,
mid-air snap, gap shortening or landing teleport is allowed.

### Lateral transition contract

Line movement starts `20.0 m` before the take-off and is complete no later than
`6.0 m` before it. The decision gate is therefore upstream of the physical
split. Entry and exit use the same canonical quintic or equivalent C2-continuous
curve in runtime, road surface, collision surface and rendered mesh.

At the fixed `20 ms` simulation step:

- lateral position is continuous and finite;
- the maximum lateral step is `0.12 m`;
- the maximum lateral velocity is `2.0 m/s`;
- heading and curvature remain finite and do not change sign repeatedly;
- the selected line centre is reached before take-off within `0.02 m`;
- the rider remains on that centre through the gap within `0.02 m`;
- the exit returns to the ordinary trail centre with zero position, tangent
  and grade discontinuity within the socket tolerances below.

The `250 ms` diagnostics trace must retain the existing maximum lateral step
gate of `1.0 m`, but fixed-step unit tests enforce the tighter limits above.

### Flight and landing contract

Each completed line has one take-off transition, one airborne interval and one
landing transition. The authoritative physics snapshot owns wheel contact,
air height, pitch and landing impact. QML and rider animation may smooth the
presentation but may not synthesize a second flight or alter physics state.

For supported speeds:

- short-line airtime is `0.25-0.85 s`;
- medium-line airtime is `0.35-1.10 s`;
- long-line airtime is `0.45-1.40 s`;
- every line remains below the global `2.0 s` hard ceiling;
- air height is positive only after the physical lip and before first landing
  contact;
- forward road distance is strictly increasing in flight;
- the apex occurs once and lies between take-off and landing;
- first landing contact is on that line's authored landing ramp, not in its
  gap, another line or ordinary background terrain;
- front/rear contact changes are physically plausible and never produce a
  one-frame all-grounded state in the middle of flight;
- per-step position, elevation and pitch remain within the existing world and
  diagnostics continuity limits;
- landing impact is finite, positive, emitted once and followed by bounded
  suspension compression and recovery;
- recovery is grounded and joins the exit socket without a height correction.

Unsupported speed, invalid geometry or an impossible ballistic solution must
select fallback before take-off. Clamping an impossible launch into a visually
successful jump is a failure.

## Test-First Cases

### 1. Profile, catalog and validation tests

Add failing tests before adding the production enum/profile:

- `gapJumpProfileDefinesThreeOrderedLinesAndFallback`
- `gapJumpProfileHasExactSharedEntryAndExitSockets`
- `gapJumpRejectsDuplicateIdsUnorderedLengthsAndOverlappingLandings`
- `gapJumpRejectsNonFiniteThresholdsOffsetsAndGeometry`
- `gapJumpMirroringPreservesStableLineSemantics`
- `catalogDefinesGapJumpAsAVisibleScoredFeature`
- `courseSerializationRoundTripsEveryGapJumpParameter`
- `olderCoursesWithoutGapJumpRemainByteForByteEquivalentAfterLoad`

Validation fails closed. It must not silently sort malformed lines, infer
missing sockets or substitute tabletop geometry.

### 2. Deterministic boundary selection

Use table-driven tests around both thresholds with exact binary inputs and
`nextafter` neighbors:

| Initial stable state | Filtered speed | Expected line |
| --- | ---: | --- |
| none | `nextafter(4.00, 0)` | fallback |
| none | `4.00` | short |
| none | `nextafter(5.20, 0)` | short |
| none | `5.20` | medium |
| none | `nextafter(6.60, 0)` | medium |
| none | `6.60` | long |
| short | `5.54` | short |
| short | `5.55` | medium |
| medium | `4.85` | medium |
| medium | `nextafter(4.85, 0)` | short |
| medium | `6.94` | medium |
| medium | `6.95` | long |
| long | `6.25` | long |
| long | `nextafter(6.25, 0)` | medium |

Required tests:

- `gapJumpColdSelectionIsExactAtBoundarySpeeds`
- `gapJumpSelectionUsesSchmittThresholds`
- `gapJumpSelectionIsIndependentOfRenderFrameCadence`
- `gapJumpDecisionLocksForActionId`
- `gapJumpInvalidSpeedFailsClosed`

Run the same speed trace at simulation input batching of 20, 40, 100 and 250
milliseconds. The selected line, decision distance and outcome must be equal.

### 3. Hysteresis and no oscillation

Feed deterministic sawtooth and noisy traces around each boundary for at least
five seconds. Include alternating samples around `4.00`, `5.20` and `6.60 m/s`
plus seeded noise. Assert:

- zero changes while samples remain inside the active Schmitt band;
- exactly one change after a threshold is crossed and held for the canonical
  filter duration;
- no `short-long-short` or `medium-long-medium` sequence without crossing the
  opposite release threshold;
- no change after commitment;
- bit-identical results for repeated runs and replay.

Tests: `gapJumpNoiseCannotOscillateLine`,
`gapJumpSelectionPersistsAcrossPauseResume`, and
`gapJumpReplayRetainsOneCommittedLine`.

### 4. Early transition and no teleport

For every start lane to selected lane combination, sample the fixed-step road,
runtime and world snapshots from `25 m` before take-off until `10 m` after
landing. Assert the lateral contract, monotonic curve progress and exact line
centre at the lip. Repeat for mirrored geometry and fallback.

Cross-check three independently produced values at every sample:

1. runtime rider lateral offset;
2. road/collision line centre;
3. rendered mesh line centre exposed by a test-only geometry query.

They may differ by at most `0.02 m`. Add an engine integration trace that
changes virtual gear during the transition and proves the speed changes through
inertia while the lateral path remains continuous.

Required tests:

- `gapJumpMovesToEachLineBeforeTakeoff`
- `gapJumpTransitionIsC2ContinuousAndBounded`
- `gapJumpFallbackBranchesBeforeTheGap`
- `gapJumpGearChangeCannotTeleportOrReselectLine`
- `gapJumpRuntimeRoadAndMeshUseTheSameBranchCurve`

### 5. Insufficient effort cases

Use controlled inputs where one readiness component at a time fails while
speed would otherwise select each of the three lines:

- adequate speed and cadence, power below threshold;
- adequate speed and power, cadence below threshold;
- adequate power and cadence, speed below the shortest supported launch;
- a late power burst after commitment;
- missing cadence when cadence is required;
- stale telemetry and non-finite telemetry;
- pause spanning part of the measurement window.

Every case must choose fallback, remain grounded and award no completion bonus.
An on-threshold value succeeds; one representable value below it fails. A late
burst inside the valid measurement window may improve readiness, but one after
the decision gate may not alter the committed outcome.

### 6. Physics and runtime flight matrix

Run real road-configured physics, not a scripted vertical-offset helper, at the
minimum, midpoint and maximum accepted speed for each line. Capture take-off,
apex, first contact, maximum clearance, pitch, suspension and impact. Run at
fixed 20 ms steps and again with equivalent batched inputs to prove the fixed
step is authoritative.

Required tests:

- `gapJumpEachLineClearsOnlyItsOwnGap`
- `gapJumpAirtimeScalesWithSelectedGapAndSpeed`
- `gapJumpHasOneTakeoffOneApexAndOneLanding`
- `gapJumpLandingUsesAuthoredRampContact`
- `gapJumpLandingImpactAndSuspensionAreBounded`
- `gapJumpFallbackNeverBecomesAirborne`
- `gapJumpUnsupportedBallisticsFailClosed`
- `gapJumpPresentationDoesNotSynthesizeAirHeight`

The test must fail if a short-line trajectory can be relabeled as long while
using the same geometry, if contact exists across a gap, or if a rider lands on
an adjacent line.

### 7. Trail sockets, seams and occlusion

Geometry tests inspect entry, split, lip, gap, landing, merge and exit for all
three lines and fallback. Acceptance tolerances:

- socket position: `1e-5 m`;
- width and elevation: `1e-5 m`;
- tangent and grade: `1e-5` in normalized representation;
- normal angle: at most `0.1 degree` outside intentionally sharp lips;
- no triangle or Box2D segment spans an authored gap;
- no duplicate coplanar surface at a join;
- no open side/underside visible from the accepted camera envelope;
- UV/material bands continue across sockets without a reset or width jump;
- forest floor joins the outside edge of the expanded feature tile;
- props remain outside every line, landing envelope, fallback and camera cue
  corridor;
- range/chunk builds include the complete active feature and do not clip one
  lane independently.

Required tests:

- `gapJumpAllLinesShareExactTrailSockets`
- `gapJumpIndicesAndPhysicsLeaveThreeRealGaps`
- `gapJumpLandingMeshesDoNotOverlapOrExposeUndersides`
- `gapJumpForestFloorClosesAroundTheExpandedTile`
- `gapJumpChunkRangeCannotSplitActiveGeometry`
- `gapJumpPropsClearAllTrajectoriesAndFallback`

### 8. Physics, render, trace and recording agreement

Add stable trace fields to the Qt Quick 3D trace:

```text
feature_kind=gap-jump candidate_line=medium selected_line=medium
line_locked=1 route=main takeoff_m=... landing_m=...
air_time_ms=... landing_count=1
```

The selected line in the immutable engine frame, world contact profile, runtime
snapshot, ViewModel/QML properties and trace must be identical for an action
ID. The render layer must never derive a line from current FPS or current speed.

The raw training recording remains the authority for power, cadence, heart
rate, virtual gear, target and distance. Extend
`analyze_workout_game.py` so it reconstructs each decision window from matched
recording rows and verifies that recorded speed/effort implies the traced
candidate line, readiness, committed route and outcome. This avoids adding
renderer-only state to the activity file while still proving recording
agreement.

Acceptance requires:

- at least 75 percent of recording rows in the trace window matched;
- existing power, cadence, heart-rate, gear and trainer-target tolerances pass;
- exactly one decision and one selected line per gap-jump action ID;
- recomputed candidate line equals trace, physics and render line;
- completed rows are on `route=main`, readiness is `1.0`, and one landing is
  observed;
- fallback rows are on `route=fallback`, readiness is below `1.0`, airborne
  count and landing count are zero;
- no distance regression, unexpected airborne frame or skipped simulation
  tick occurs.

Add analyzer fixtures for all lines, both boundaries, fallback, a mismatched
line, a changed line after lock, a false landing and a recording whose samples
cannot support the traced decision. Every corrupt fixture must fail with a
specific reason.

### 9. Quick 3D view and visibility without HUD

The Quick 3D suite must instantiate the production QML and production geometry.
Create deterministic exports for:

- approach from `35`, `25`, `15` and `7 m` before take-off with HUD hidden;
- short, medium and long commitment;
- take-off, apex, landing and recovery for each line;
- insufficient-power and insufficient-cadence fallback;
- mirrored tile;
- 1280 by 720 target composition and 1024 by 600 laptop composition.

Automated image checks require nonblank terrain, three spatially separate lip
and landing silhouettes, visible empty space between every lip and landing,
the selected trajectory inside the camera frustum, and no clear-color or
background leak through solid terrain. Pixel checks are structural and use
regions/masks, not a fragile whole-image hash.

Human visual acceptance is performed without feature name, target bars or
route text. A reviewer must identify:

1. that a gap jump is approaching;
2. that three different lengths are available;
3. which line the rider has entered before take-off;
4. whether the rider jumped or used the grounded fallback;
5. the take-off, airborne and landing phases in motion.

Failure to identify any item from the image sequence blocks release even when
all numeric tests pass.

### 10. Feature Lab and Data Generator matrix

Add a compact gap-jump lab segment before integrating it into the full Feature
Lab course. The compact course contains four actions in this order: short,
medium, long and fallback. It has deterministic start speed/gear anchors and
enough runout to stabilize before the next action.

Exercise every existing Data Generator mode:

| Generator mode | Required evidence |
| --- | --- |
| `follow-target` | Deterministic line sequence and completion using the authored target/gear script |
| `on-target` | Completes short, medium and long fixtures exactly at required effort |
| `over-target` | Completes; speed still chooses the line and cannot be overridden directly by excess watts |
| `under-target` | Uses fallback for all three candidate speeds |
| `cadence-low` | Uses fallback when cadence is required even if power and speed pass |
| `cadence-high` | Completes when power and speed also pass; cadence alone cannot promote a longer line |

For boundary-speed cases, the lab driver sets deterministic initial speed and
virtual gear rather than waiting for incidental acceleration. Repeat every
scenario twice and compare decision events and replay hashes.

The full Feature Lab then includes one representative adaptive gap jump while
retaining the existing eleven feature order and behavior. Existing completed
and bypassed scripts must continue to pass unchanged.

### 11. Packaged AppImage UI evidence

Run only against the packaged AppImage and an isolated temporary athlete. Do
not open, copy or modify the user's normal GoldenCheetah library.

Produce two distinct runs because GPU readback disturbs timing:

1. Evidence run: hardware GL, Qt Quick 3D, HUD-hidden approach/action/recovery
   stills, a 60 FPS video covering short/medium/long/fallback, application log,
   JUnit and artifact manifest.
2. Performance/recording run: hardware GL, Qt Quick 3D, no screenshot or video
   capture, diagnostics trace, analyzer JSON and preserved isolated raw
   recording.

The evidence directory must contain:

```text
application.log
goldencheetah.log
junit.xml
workout-game-summary.json
game-training-recording.csv
renderer-evidence.json
gap-jump-short/{approach,takeoff,apex,landing,recovery}.png
gap-jump-medium/{approach,takeoff,apex,landing,recovery}.png
gap-jump-long/{approach,takeoff,apex,landing,recovery}.png
gap-jump-fallback/{approach,split,pass,merge}.png
gap-jump-all-lines.mp4
sha256sums.txt
```

Video frame count, duration and dimensions are checked with `ffprobe`. Every
PNG is decoded and checked for nonzero dimensions and nonblank RGB variance.
Hashes identify the reviewed evidence but are not golden-image assertions.

### 12. Mandatory Qt Quick 3D evidence guard

No recording may be labeled 3D acceptance merely because
`GC_WORKOUT_GAME_3D=1` was requested. The application log is authoritative.
Before analyzing traces or publishing artifacts, the runner must require all
of the following within the tested process lifetime:

- exact log line `Workout Game renderer selection: Qt Quick 3D`;
- at least one `workout-game-3d-trace` sample;
- accessible canvas name `Workout game 3D canvas`;
- no later `Workout Game renderer fallback: Qt Quick 3D -> SceneGraph`;
- no legacy-only `workout-game-trace` accepted as the source of 3D evidence.

The runner writes `renderer-evidence.json` with requested renderer, selected
renderer, trace marker, fallback status, AppImage SHA-256 and pass/fail. The
analyzer exits nonzero before performance evaluation if this contract fails.

Add tests in `test_analyze_workout_game.py` and the launcher tests for:

- a valid Qt Quick 3D selection and 3D trace passes;
- the legacy `SceneGraph` selection plus `workout-game-trace` fails even when
  `GC_WORKOUT_GAME_3D=1` was exported;
- a Quick 3D selection followed by fallback fails;
- a 3D trace without the exact selection line fails;
- the exact selection line without a 3D trace fails;
- mixed logs use process/session boundaries and cannot borrow a selection line
  from an earlier launch;
- missing or malformed renderer evidence fails closed;
- artifact naming cannot contain `quick3d`, `3d-acceptance` or equivalent when
  the guard fails.

This guard applies to all future Quick 3D acceptance evidence, including the
general pre-release matrix. The known legacy recording
`gc-ui-cc84c34-final-long/session.mp4` must be a negative fixture or explicitly
documented negative example, never a 3D baseline.

### 13. Target old-laptop performance

Measure on the target old laptop with its Intel GPU and normal 1280 by 720
presentation. Keep NVIDIA measurements as additional scalability evidence, not
as a replacement. The test course repeatedly streams all three lanes and
fallback through at least 20 gap-jump actions for a minimum of five minutes.

With video and pixel capture disabled, acceptance requires:

- median presented FPS at least `50`;
- observed p95 frame interval at most `25 ms`;
- observed p99 frame interval at most `40 ms`;
- maximum reported stall at most `150 ms` after the opening warm-up window;
- zero backward frames, trace regressions and unexpected airborne frames;
- zero skipped simulation ticks;
- geometry queue returns to zero and never exceeds the existing bounded mailbox
  capacity;
- no steady growth in resident memory, geometry objects or QML objects;
- gap-jump visible triangles and draw calls stay within an explicit budget
  recorded by the geometry/view tests;
- p95 presentation work and trainer/recording deadlines do not regress by more
  than 10 percent from the same-build no-gap control course.

Run the existing analyzer hard gates as well. A faster NVIDIA run cannot waive
an Intel failure. A capture run cannot be used as frame-pacing evidence.

### 14. Regression coverage

Before release, run the complete existing Workout Game suites, not only new
gap tests. Specific protected behavior includes:

- tabletop remains one centre jump plus its existing bypass and keeps its
  current flight calibration;
- drop retains its real gap, lower landing and grounded bypass;
- log over and bunny hop retain their distinct shorter jump arcs;
- roots, rock garden, rock slab and rollers never gain scripted airtime;
- skinny remains on its main deck without a chicken line;
- berm remains an ambient bank line and not a scored feature;
- virtual gears alter speed through inertia, never by immediate teleport;
- pause/resume, stale telemetry, finish and course replacement cannot reuse a
  prior action ID or line lock;
- fixed-step simulation and newest-frame publication remain independent of GUI
  render cadence;
- recording save/discard/continue behavior remains unchanged;
- painter and legacy SceneGraph comparison paths may still run their existing
  tests, but neither can satisfy the Quick 3D evidence gate.

Run focused tests under ASan/UBSan. Run runner, chunk-builder and relevant
mailbox tests under TSan when the existing environment supports it. Repeated
construction, immediate shutdown and course replacement must not leave stale
gap geometry or selected-line state.

## Implementation Gate Order

Each gate is committed only with its tests. Do not defer all tests to the end.

1. **Red: profile and selection.** Add profile validation, exact boundary and
   hysteresis tests. Confirm they fail because gap-jump behavior is absent.
2. **Green: pure model.** Implement only enough canonical profile and selection
   state to pass deterministic headless tests.
3. **Red: branch and physics.** Add transition, gap absence, flight, landing
   and fallback tests. Confirm meaningful failures.
4. **Green: world integration.** Connect one immutable selected-line snapshot
   to road, runtime and physics. Pass normal and sanitizer tests.
5. **Red: geometry and sockets.** Add three-line mesh, seam, chunk and prop
   clearance tests before creating production geometry/assets.
6. **Green: Quick 3D presentation.** Render the canonical geometry and consume
   authoritative physics without deriving decisions in QML.
7. **Red/green: analyzer and renderer guard.** Land negative legacy-evidence
   fixtures before generating any new acceptance video.
8. **Feature Lab.** Add compact all-outcome scenarios, then include one gap
   jump in the full deterministic lab.
9. **Packaged acceptance.** Build AppImage, run isolated video/stills and a
   separate trace/recording/performance pass.
10. **Review.** QA, developer and visual reviewer sign the artifact manifest;
    target-laptop interactive review is the final gate.

## Definition Of Done

The three-line adaptive gap jump is accepted only when all of these are true:

- all deterministic boundary and hysteresis cases pass;
- selected line locks once and lateral movement is early and continuous;
- insufficient effort takes a visible grounded fallback;
- each completed line has one bounded physical flight and matching landing;
- physics, runtime, rendered line, trace and recorded telemetry agree;
- every trail/terrain/collision socket is exact and no gap is accidentally
  bridged;
- all three choices and the result are readable without HUD text;
- all Data Generator modes have deterministic expected outcomes;
- isolated packaged AppImage stills and video have been reviewed;
- evidence proves the selected renderer was Qt Quick 3D and contains no
  fallback to legacy SceneGraph;
- target old-laptop performance and recording deadlines pass separately from
  capture;
- complete existing Workout Game regression suites and focused sanitizers pass;
- no production athlete data was touched and every evidence artifact identifies
  the tested AppImage hash.
