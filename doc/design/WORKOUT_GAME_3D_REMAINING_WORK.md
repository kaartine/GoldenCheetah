# Workout Game 3D Remaining Work

## Purpose

This is the canonical, deduplicated list of remaining Workout Game 3D work.
The release checklist defines the final gates. The live and visual backlogs are
supporting observation and acceptance registers; their source IDs map to one
work package here and must not be scheduled as separate duplicate tasks.

Status values are:

- `partial`: relevant implementation exists, but one or more acceptance gates
  still fail or lack interactive evidence.
- `not started`: no production implementation was found for the requested
  behavior.
- `blocked`: the work is deliberately waiting for prerequisite gates.
- `deferred`: explicitly outside the current release goal.

An item is complete only when its focused tests, deterministic rendered
evidence, packaged-AppImage check and any required physical-device or user
review all pass. A checked technical task in the release checklist does not by
itself close a visual or live-ride requirement.

The consolidated inventory contains 16 partially complete release packages,
one blocked retirement package, four post-release gameplay packages, four
adjacent integration/exploration packages and four architecture packages.
Across all sections, 20 packages are partial, five are not started, one is
blocked and three are deferred.

## Release Critical Path

Work these items in order unless a later item can be implemented and tested
independently without delaying an earlier gate.

The practical execution order is WG-16's required-test inventory and known
failures first, then WG-02, WG-03 through WG-15, the physical WG-01 gate and
finally WG-17. This keeps independently automatable work moving while the real
trainer/user gate is unavailable.

## Execution Batches

Each batch begins with a focused design and test-impact review. Small related
changes share one build and regression run; a batch is committed only after its
focused tests and the listed aggregate gate pass. Source remains canonical on
the local workstation. The remote Docker host is a bounded build/test helper,
uses at most ten CPUs, and publishes an accepted development AppImage to
`/home/jkaartinen/Documents/personal/GoldenCheetah-latest.AppImage`.

| Batch | Scope | Focused verification | Aggregate gate |
| --- | --- | --- | --- |
| B0 Test foundation | WG-16 inventory, the two known world failures, gap-jump contract conflict and stale release evidence | Required-test inventory reconciliation; failing world cases; gap branch/runtime tests | All current non-visual unit projects, then ASan/UBSan for changed pure modules |
| B1 Route and world | WG-03, WG-04, WG-06 and WG-15 | Route-quality windows, persisted road plan/version, berm frequency/line, socket continuity, terrain anchoring and render budgets | Course conversion, document/store, road, geometry, terrain and deterministic render suites |
| B2 Motion and gameplay | WG-05, WG-07 through WG-14 | Camera/frustum, crank/feet, contact, feature traces, guidance, gap lines and progressive gearing | Simulation, engine, runner, ViewModel/QML and feature-lab suites plus sanitizer subset |
| B3 Presentation performance | WG-02 and remaining WG-10/WG-12/WG-15 visual acceptance | Cold first-ten-second frame trace, per-feature still/motion catalog, HUD bounds and prop sweep | Isolated packaged-AppImage Intel/NVIDIA video, trace and frame-budget matrix |
| B4 Workflow and release | Final WG-16 workflow, AppImage provenance and stable publication | Create/save/start/gears/continue/save/reopen with visible Data Generator and no production athlete data | Full required inventory, isolated UI matrix, package manifest/SBOM/secret scan and clean-tree check |
| B5 Physical acceptance | WG-01 followed by WG-17 | Isolated real-trainer recording reconciliation and user A/B ride | Remove legacy renderers only after every preceding gate passes; rebuild and repeat B4 |

Post-release gameplay, adjacent integrations and conditional architecture
optimizations are not allowed to interrupt B0-B4. A change may be pulled
forward only when it directly removes a release blocker and receives the same
test coverage as its owning batch.

| ID | Priority | Status | Consolidated work | Done when | Source requirements |
| --- | --- | --- | --- | --- | --- |
| WG-01 | P0 | partial | Real-trainer end-to-end acceptance | An isolated athlete session on the target laptop proves trainer target, recording and feature outcome agree, followed by the user A/B ride. | `VS-04`, `REL-04` |
| WG-02 | P0 | partial | Cold-start frame continuity | The packaged AppImage meets the first-10-second frame limits on Intel and NVIDIA with no startup pause, backward motion, skipped simulation tick or trainer/recording deadline miss. | `LIVE-GAME-07`, `VIS-19` |
| WG-03 | P0 | partial | Interesting, deterministic persisted routes | Ordinary trail meets the 25 m straight-run and rolling-100 m curvature rules. Converted and re-saved courses such as `test3` persist the accepted final road pieces, turns and relief under an explicit generation version instead of relying only on a seed and the current algorithm. | `LIVE-ROUTE-01`, `LIVE-ROUTE-03`, `LIVE-ROUTE-06`, `VIS-05` |
| WG-04 | P0 | partial | Berms as ordinary trail turns | Medium, sharp, near-90-degree and high-speed turns include varied integrated banks. Power moves the rider and lean around the same banked centreline, and no feature prompt appears. | `LIVE-ROUTE-02`, `VIS-17` |
| WG-05 | P0 | partial | Stable, readable camera composition | The medium-centre camera keeps the complete bicycle visible, exposes 25-40 m of useful trail and shows vertical relief. Route-following yaw has bounded velocity and acceleration through sharp turns without motion sickness. | `LIVE-RIDER-05`, `VIS-09` |
| WG-06 | P0 | partial | Strong terrain relief and seamless transitions | Ordinary trail visibly climbs, descends and undulates; every tile transition preserves socket position, heading, grade, width and material without a step, pop or exposed background. Trainer grade and workout targets remain authoritative. | `LIVE-ROUTE-04`, `LIVE-ROUTE-05`, `VIS-04` |
| WG-07 | P0 | partial | Coherent rider, bicycle and arcade art direction | Chase and side views share the legacy view's readable pixel-arcade identity. The original, provenance-recorded rider and modern full-suspension bicycle show a helmet, drivetrain, suspension and large black tyres while remaining visibly distinct from Pole Voima and a real person. | `LIVE-RIDER-03`, `LIVE-RIDER-07`, `VIS-01`, `VIS-02`, `VIS-03` |
| WG-08 | P0 | partial | Natural rider and bicycle animation | Feet, cranks and pedals share phase; pedalling sway, steering, lean, standing effort, suspension, preload, air, landing and rough-surface absorption remain bounded, state-driven and readable with the HUD hidden. | `LIVE-RIDER-01`, `LIVE-RIDER-04`, `VIS-08` |
| WG-09 | P0 | partial | Authoritative terrain contact and rider framing | Both wheels and the whole rider remain framed and attached to one authoritative road/feature trajectory without sideways travel, floating, sinking, partial disappearance or a render-only teleport. | `LIVE-RIDER-02`, `LIVE-RIDER-08` |
| WG-10 | P0 | partial | Distinct feature geometry integrated into trail tiles | Every supported feature is recognizable without HUD text at approach distance and joins exact trail/terrain sockets without pasted-on edges, clipping, gaps, z-fighting or visible undersides. | `LIVE-WORLD-03`, `LIVE-WORLD-04`, `VIS-06`, `VIS-07` |
| WG-11 | P0 | partial | Continuous feature action physics | Jump, drop, main-line, bypass and recovery traces remain continuous, bounded and forward-only, and wheel contact agrees with the rendered trail geometry. | `LIVE-GAME-02` |
| WG-12 | P0 | partial | Reliable feature guidance and outcome feedback | Distance, target power, cadence readiness, commitment, completion and bypass use the same target authority as trainer and generator. Short visual feedback makes the outcome obvious without changing simulation timing. | `LIVE-GAME-01`, `VIS-14` |
| WG-13 | P0 | partial | Progressive gap-jump choice | One approach fans into clearly different short, medium and long gaps. Predicted speed from the final 10-3 m selects and locks a reachable line; unsafe speed uses a grounded route; take-off, flight, landing and merge are visually and physically continuous. | `FTR-12`, `VIS-16` |
| WG-14 | P0 | partial | Progressive virtual gearing and trainer feel | A shift changes cadence/torque response through inertia on flats and climbs instead of changing speed discontinuously, while manual virtual gears remain usable outside Game view. | `PHY-03`, `LIVE-RIDER-06` |
| WG-15 | P0 | partial | Forest density, materials, depth and stable placement | A varied Finnish forest uses terrain-anchored trees, rocks, stumps, shrubs, roots, dead wood and undergrowth with coherent surfaces, depth and lighting. Nothing visibly pops, floats, intersects or exposes its buried part, and the target laptop stays within measured draw, triangle, chunk-build and frame budgets. | `LIVE-WORLD-01`, `LIVE-WORLD-02`, `LIVE-WORLD-05`, `LIVE-WORLD-07`, `VIS-10`, `VIS-11`, `VIS-12`, `VIS-13` |
| WG-16 | P0 | partial | Complete, repeatable UI and visual verification | A short persisted lab course exercises every scored feature plus berms and terrain forms. Visible isolated Data Generator controls, workout creation/save, Game entry, gears, stop/continue, activity save/reopen, compact HUD/profile and automated trace/still/video checks all work without production athlete data or hidden environment variables. Every current unit-test project is present in the required inventory and the full required suite passes. | `LIVE-GAME-03`, `LIVE-GAME-04`, `LIVE-GAME-05`, `LIVE-GAME-06`, `VIS-15`, `REL-01` |
| WG-17 | P0 | blocked | Retire legacy renderers | Remove the legacy painter, OpenGL and scene-graph paths only after WG-01 through WG-16 pass and the migration A/B gates are recorded. | `REL-06`, migration retirement gates |

`LIVE-WORLD-06` is already verified and therefore is not remaining work. Its
18-tree/80-draw-call evidence remains in the live backlog.

## Known Concrete Release Failures

- The required inventory now includes
  `Train/workoutGameGapJumpLaunchWindow` and reconciles 172/172 projects. Its
  five focused gap suites pass 73/73, but `REL-01` remains open until the full
  current inventory has been rerun.
- The formerly documented climb-continuity and rock-garden bypass failures are
  stale. Current `workoutGameWorld` passes 45/45 normally and under ASan/UBSan;
  both regressions also pass 25 repeated runs. Keep the strict tests, but do not
  schedule duplicate implementation work for them.
- Current gap-jump evidence proves an implemented three-line system, but lacks
  the complete per-line image/action/endurance matrix. Its recorded p95/max
  frame intervals also exceed the stated limit unless shown to belong solely
  to the excluded warm-up. WG-13 therefore remains partial.
- No automated evidence substitutes for WG-01's physical trainer ride and
  user A/B acceptance.

## Post-Release Gameplay

These requested features are valuable but do not block the first accepted 3D
release unless the release scope is explicitly expanded.

| ID | Priority | Status | Consolidated work | Done when | Source requirements |
| --- | --- | --- | --- | --- | --- |
| POST-01 | P1 | not started | Speed-driven MTB tailwhip | Successful jumps interpolate a bounded 0-35-degree rear-bike whip from launch speed, peak near mid-flight and align within 2 degrees before landing, without changing physics, line choice or recording. Bypasses never whip. | `LIVE-RIDER-09`, `VIS-18` |
| POST-02 | P1 | not started | Wall ride feature | A separately authored, readable and licensed wall-ride tile has continuous entry/action/exit geometry, deterministic physics, rider pose, complete/bypass behavior and the normal feature evidence set. | user feature request; absent from the current terrain catalog |
| POST-03 | P1 | partial | Results and progression presentation | Existing competition state is exposed as a compact end-of-ride result showing feature outcomes, streak or score and comparable previous/ghost performance without distracting from live training. | original reward-for-good-riding goal; competition engine exists but no accepted Quick 3D result view was found |
| POST-04 | P2 | deferred | Feature-specific dynamic camera | Add bounded action-aware camera transitions only after the fixed chase camera, terrain contact and motion-sickness gates are accepted. The normal ride must remain stable and predictable. | user camera exploration; fixed camera remains the release baseline |

## Adjacent Product Work

These are real requests, but they have separate ownership and acceptance from
the Workout Game renderer. They must not be hidden inside a graphics task.

| ID | Priority | Status | Consolidated work | Done when | Evidence |
| --- | --- | --- | --- | --- | --- |
| INT-01 | P1 | not started | Synced Strava activity to editable MTB course | A previously synced activity can supply a trimmed/editable effort profile to the same deterministic MTB conversion and persistence pipeline as an ERG workout. Current conversion accepts workout intervals only. | `WorkoutGameCourseSourceAdapter`; Train selection flow |
| INT-02 | P1 | partial | Strava description live acceptance | Existing automatic append/update logic preserves the remote description and adds the generated training summary exactly once on a real synchronized activity using stored credentials. | `StravaActivityDescription`; live-account acceptance pending |
| INT-03 | P1 | partial | Suunto and generic BLE heart-rate endurance validation | A long physical ride proves independent heart-rate reconnect and stale-stream recovery without repeated manual disconnect/connect. Generic BLE support exists; the intermittent Suunto case lacks a completed endurance gate. | BLE telemetry/reconnect implementation; physical test pending |
| EXP-01 | P2 | deferred | Map-derived terrain or route input | Evaluate OSM or another license-compatible source only after generated/persisted courses are accepted. Google imagery/data is not assumed reusable. | user exploration request |

## Architecture Debt

These gaps remain in `WORKOUT_GAME_ARCHITECTURE.md`. They are kept separate
where they have a different owner or acceptance method from visible gameplay.

| ID | Priority | Status | Work | Architecture gaps |
| --- | --- | --- | --- | --- |
| TECH-01 | P1 | not started | Replace newest-only telemetry consumption with a bounded timestamped history, ordered rejection counters and field-specific validity periods. | 1 |
| TECH-02 | P1 | partial | Complete virtual-clock, publication/burst, missed-refresh, raw frame-pacing and sensor-to-display latency evidence with telemetry input identity. Cold-start acceptance remains in WG-02. | 2, 3, 4, 10 |
| TECH-03 | P2 | deferred | Profile before changing the Qt-synchronized render handoff, HUD texture lifecycle or Box2D 120 Hz/four-substep policy. Implement only when measurements justify it. | 5, 7, 9 |
| TECH-04 | P2 | not started | Move long activity conversion and future asset decoding into cancelable jobs that publish one immutable GUI-thread result. This is a prerequisite for scaling INT-01. | 8 |

Architecture gap 6 is retired with WG-17 rather than optimized in the legacy
renderer. Quick 3D batching/instancing pressure belongs to WG-15.

## Specification Decisions And Conflicts

- The gap-jump 50 ms lateral-step limit is `0.15 m`: the exact displacement at
  the existing `3.0 m/s` lateral-velocity ceiling. The former `0.05 m` wording
  conflicted with reaching a selected line and is retired. The separate 250 ms
  diagnostics ceiling remains `1.0 m`.
- The `50` draw-call value is the optimization target and `80` is the current
  hard acceptance ceiling for the complete measured scene. Do not compare one
  as if it superseded the other.
- The production foreground cap is `18` detailed trunk-and-crown trees. Extra
  forest dressing must be batched/instanced and still satisfy WG-15; historical
  10-tree assertions are obsolete.
- Current Feature Lab terminology means eleven non-berm feature terrain types,
  ten scored challenges and six ambient berm segments. Berm remains a terrain
  module but is not an advertised/scored feature.
- Quick 3D is the production target. Legacy renderers and older 2D/fallback
  architecture text are comparison and migration material only.
- Project-authored production models take precedence over old external asset
  candidate suggestions. Candidate documents remain license research, not the
  selected art implementation.

## Explicitly Deferred Or Closed

- Historical GitHub Actions failures are CI infrastructure debt. They were
  explicitly excluded from the current feature goal and are not a Workout Game
  acceptance substitute.
- Open Low-severity entries in `AUDIT_FINDINGS.md` remain deferred by explicit
  decision. They are not fixed and must not be silently marked accepted.
- Strava image upload was dropped because normal API access does not provide
  the desired partner-style media workflow. Rich descriptions remain the
  selected integration.
- Rival riders were removed from the current design. They are not unfinished
  release work.
- OSM/map-derived scenery is exploration, not a dependency of generated MTB
  courses.

## Source-List Rules

- `WORKOUT_GAME_3D_RELEASE_CHECKLIST.md` owns release gates and evidence.
- `WORKOUT_GAME_3D_LIVE_BACKLOG.md` owns detailed ride observations and their
  evidence log, but each active ID is scheduled through exactly one `WG-*`
  package above.
- `WORKOUT_GAME_3D_VISUAL_BACKLOG.md` owns detailed art direction and visual
  acceptance language, but each `VIS-*` defect is scheduled through exactly one
  `WG-*` package above.
- `WORKOUT_GAME_3D_ASSET_BACKLOG.md` is a historical delivery plan. Its stale
  unchecked boxes are not current status.
- New observations first update an existing canonical package. Create a new
  package only when its implementation and acceptance cannot coherently belong
  to an existing one.
