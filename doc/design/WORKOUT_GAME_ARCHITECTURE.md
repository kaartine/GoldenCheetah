# Workout Game Architecture

## Goals

Workout Game adds a lightweight 2D riding game to Train view without changing
the workout clock, recording path, or trainer safety model. It must run on old
integrated-GPU laptops and remain useful without a network connection.

The first version uses generated terrain rather than map data. OpenStreetMap
routes can be added later as an optional course source, but exercise execution
must never depend on a map service, network latency, or API credentials.

## Safety Invariants

1. `TrainSidebar` remains the only owner of trainer commands and recording.
2. The workout clock remains authoritative. Game progress cannot pause, extend,
   shorten, or seek the workout.
3. The game consumes telemetry snapshots. It cannot write directly to a trainer.
4. Rendering may drop frames or disable effects. Trainer control and recording
   must not wait for a rendered frame.
5. Standard ERG and slope modes remain available when the game is disabled or
   when a renderer cannot initialize.
6. A missing GPU, asset, or game course produces a functional QPainter or
   standard Train view fallback, never a failed workout start.

## Component Boundaries

### WorkoutGameCourseBuilder

A pure C++ component converts normalized workout intervals and FTP into course
sections. The same normalized input always produces the same seed and course.

Initial mapping:

| Workout shape | Course section |
| --- | --- |
| Low-power recovery | Gravity-assisted descent |
| Long steady aerobic or threshold effort | Flow trail |
| Long hard effort or rising ramp | Climb |
| Short effort above FTP | Sprint jump |
| Opening progressive interval | Warm-up trail |
| Final declining interval | Cool-down descent |

The builder has no Qt, file-system, map, UI, or trainer dependencies. An adapter
will convert `ErgFile` points into its normalized interval input.

### WorkoutGameSimulation

The simulation is a pure C++ fixed-step state machine. Inputs are immutable
snapshots containing workout time, power, cadence,
target power, pause state, and virtual gear. Heart rate is displayed by the
renderer but does not affect game rules. Outputs contain course progress,
section state, feature outcome, speed, adherence, and score state.

The simulation uses workout time for longitudinal course progress. Measured
power changes animation speed, line choice, and scoring, but cannot move the
workout timeline. Long pauses reset transient jump and streak state without
discarding accumulated score.

### WorkoutGameRoadPhysics

`WorkoutGameRoadPhysics` is the deterministic longitudinal vehicle model for
distance-based courses. It consumes only power, grade, brake input, and elapsed
time, and publishes speed, distance, and elevation. It has no Qt, workout-file,
trainer, rendering, map, or persistence dependencies. Trainer control remains
outside this module so road simulation can be tested and tuned independently.

### WorkoutGameDistanceCourse

`WorkoutGameDistanceCourseBuilder` converts normalized workout intervals into
distance and elevation sections by running the road model at target power.
`WorkoutGameDistanceCourseEstimator` replays those sections at a selected power
scale to estimate completion time. Neither class reads files or controls a
trainer. Future ERG and activity importers must adapt their source data to this
course model instead of adding source-specific behavior to physics or rendering.

### WorkoutGameWorld And Camera

`WorkoutGameWorld` owns visual vehicle physics only. Its immutable snapshot
uses trail coordinates (distance, elevation, lateral roll and vehicle pitch)
instead of screen pixels. Workout time, scoring and trainer commands remain in
their existing owners. A failed or late physics step can therefore reduce only
visual fidelity.

`WorkoutGameCamera` consumes world snapshots and produces a renderer-neutral
camera pose. Smooth terrain, climbs, jumps and drops favor a side view; roots,
rollers and rock gardens favor a three-quarter view. Skinny and berm features
are reserved for a chase camera because their line choice cannot be conveyed
reliably from the side. Camera angles blend over time, while a new physics-world
generation performs an explicit cut instead of interpolating unrelated
coordinates.

The camera pose is continuous: renderers consume interpolated yaw, pitch, zoom,
look-ahead and target coordinates rather than switching between unrelated scene
implementations. A later pseudo-3D chase projection can therefore blend from the
side projection while the same world snapshot, workout clock and recording path
continue uninterrupted. Projection changes must never recreate the physics
world or reset course progress. Skinny and berm remain disabled as generated
features until that projection communicates trail width and lateral line choice
and has deterministic transition tests.

The initial physics backend is vendored Box2D 3.1.1 (MIT license, pinned to
commit `8c661469c9507d3ad6fbd2fea3f1aa71669c2fe3`). Box2D controls terrain
contacts, suspension, vehicle pose and airborne motion. It never controls
workout progress, trainer resistance, scoring or recording.

### WorkoutGameWindow

The game is registered as a Train `GcChartWindow`. The window is the Qt-facing
coordinator: it owns the course, simulation, competition model, and ghost
recorder; subscribes to existing `Context` signals; and publishes immutable
snapshots to both canvases. It does not own a trainer controller. The existing
chart system allows the game to coexist with current perspectives and switch to
full-screen without restarting the session.

The window selects one renderer at construction:

- `WorkoutGameOpenGLCanvas`: a `QOpenGLWidget` which uses Qt's OpenGL paint
  engine to composite bundled bitmap assets and the shared scene at 60 FPS.
- `WorkoutGameCanvas`: QPainter fallback using the same render snapshot and
  bundled assets at 30 FPS.

Renderer selection is capability-based. Runtime rendering errors permanently
fall back for the current session instead of repeatedly recreating a GPU
context.

## Scheduling And Performance

- Trainer command cadence: owned by existing Train code and highest priority.
- Recording cadence: owned by existing Train code and independent of the game.
- Telemetry ingestion: existing cadence, copied into a small latest-value
  snapshot without waiting for rendering.
- Game rules: fixed 50 ms steps. Vehicle physics: fixed 8.33 ms steps. Both
  cap catch-up work after stalls. A future simulation runner may publish at the
  display rate without changing these deterministic internal step sizes.
- Rendering: target 60 FPS on GPU and 30 FPS with QPainter.
- The current scene has bounded geometry, four competitor sprites at most, no
  per-frame texture upload, and no unbounded particle list. Automatic
  frame-budget quality scaling remains a future enhancement.

There is no unbounded event queue. Simulation and rendering use latest-value
snapshots, so stale frames are overwritten rather than accumulated.

### Clock And Thread Model

The game has three clock domains with explicit ownership:

1. Sensor samples retain their device or ingestion timestamps. Trainer control
   and activity recording continue on the existing Train paths and never wait
   for the game.
2. `WorkoutGameClock` uses a monotonic clock to drive one deterministic,
   fixed-step simulation owner. It advances in complete ticks, limits catch-up
   work after a stall, and reports discontinuities instead of integrating one
   unbounded time step.
3. Presentation follows the display refresh. The renderer interpolates between
   the previous and current complete simulation snapshots. It must not
   extrapolate independently smoothed position, section, feature, or camera
   values because those values can then disagree at interval boundaries.

The intended steady-state data flow is:

```text
Train telemetry -> latest input snapshot -> fixed-step simulation
                                             |
                                             v
                                    immutable frame snapshots
                                             |
                                             v
                               Qt scene graph / presentation
```

The simulation owns mutable game and physics state. At the end of a tick it
publishes an immutable `WorkoutGameFrameSnapshot` through a bounded double or
triple buffer with a monotonically increasing sequence number. The renderer
reads the newest complete pair and never waits for a partially written frame.
Old snapshots are overwritten rather than queued.

Qt Quick already owns GUI/render synchronization and can use a dedicated render
thread. Scene graph updates therefore remain small snapshot-to-node copies;
GPU submission stays in Qt's render loop. Course generation, asset decoding,
and other measured heavy work may use worker jobs, but workers publish results
for a later simulation tick and are never joined from trainer control or the
render hot path. The current vehicle simulation should remain one owner until
profiling proves that a phase is large enough to justify job scheduling.

This follows the fixed-timestep and render-interpolation model used by mature
engines while respecting Qt's scene graph ownership rules:

- [Qt Quick Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)
- [Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/)
- [Godot physics interpolation](https://docs.godotengine.org/en/stable/tutorials/physics/interpolation/physics_interpolation_introduction.html)
- [Box2D simulation](https://box2d.org/documentation/md_simulation.html)

### Migration And Acceptance

The thread model is introduced in measured stages rather than changing trainer,
simulation, and rendering ownership at once:

1. Use one workout-time-to-road timeline and instrument frame time, world
   position, regressions, stationary frames, and section state.
2. Extract the monotonic clock and fixed-step simulation runner with fake-clock
   tests for stalls, pause, resume, reset, and bounded catch-up.
3. Publish immutable previous/current snapshots and interpolate presentation
   from only that pair.
4. Profile on the old integrated-GPU laptop before moving any course or physics
   phase to workers or adding GPU effects.

Acceptance requires deterministic replay for the same timestamped input, no
backward world movement during forward riding, no trainer or recording wait on
rendering, bounded work after a one-second UI stall, and no unbounded memory or
event-queue growth. Automated UI traces must report frame intervals, world
distance, section/progress, source/render time, late frames, and regressions.

## Game Rules

Power adherence is measured against the selected workout target. Cadence and
virtual gear affect optional feature execution but do not replace power as the
training objective.

- Flow sections reward sustained target adherence and smooth cadence.
- Climbs reward staying in the target band for the full interval.
- Sprint jumps require sufficient approach power and cadence. Missing the
  threshold selects a safe bypass line and awards no feature points.
- Descents preserve course speed with little or no required power so recovery
  remains recovery.

Feature thresholds are derived from FTP and workout targets, not hard-coded to
one rider. Scoring state is separate from recorded physiological data.

`WorkoutGameCompetition` adds three deterministic AI riders. Their monotonic
pacing curves derive from the course seed, while placement is based on score.
Small visual progress offsets show whether a rider is ahead without changing
the authoritative workout progress or trainer target.

## Arcade Design Direction

The game should adopt the clarity of 1980s arcade games without copying their
punishing failure rules. Each challenge must be visible early, require one
understandable decision, and produce immediate visual and audible feedback.

- Enduro Racer is the primary camera and presentation reference: a lightweight
  pseudo-3D trail, large readable obstacles, a small set of reusable riding
  techniques, checkpoints, and visibly different stages.
- Excitebike is the primary jump reference: approach speed, take-off timing,
  airborne bike angle, and landing quality are separate parts of a jump. A
  workout-earned flow meter can replace its heat/turbo resource without
  encouraging the rider to exceed the prescribed power.
- OutRun is the primary structure reference: route branches and biome changes
  provide replay value while every branch preserves the workout prescription.
- Paperboy is the soft-failure reference: short skill courses can appear during
  recovery, and failure ends only the bonus opportunity rather than the ride.

Before adding more terrain types, the first polished game loop should include:

1. An event timeline that announces the next feature and its input window.
2. Pumped rollers, a timed bunny hop, and controllable landing angle.
3. `Perfect`, `Good`, and `Missed` outcomes with score, animation, and sound.
4. A soft bypass or lost multiplier on failure, never a changed power target.
5. A result view containing per-section adherence, feature outcomes, longest
   streak, personal-best comparison, and ghost delta.

The normal HUD should prioritize the next action, target band, interval
countdown, flow/streak, gear, and route position. Renderer and frame-time
diagnostics belong in an optional debug overlay.

## Future Distance Course Mode

Distance Course is a separate mode from the time-authored ERG Workout Game.
Elapsed time is still recorded and used to integrate physics, but it no longer
determines course progress. The finish condition is a configured route distance,
and the rider advances only through simulated road speed. On level ground the
bike coasts and stops without power; downhill it can accelerate under gravity.

The initial course format is distance-authored and offline:

- total distance in meters;
- piecewise grade and elevation profile by distance;
- terrain, biome, and game-event markers by distance;
- optional target power or power-zone guidance for selected segments; and
- deterministic seed, checkpoints, branches, and finish metadata.

### Unified MTB Course Generator

GoldenCheetah should expose one `Generate MTB Course` workflow from both the
workout library and activity history. It always creates a new editable private
course workout and never modifies the source workout or activity.

The generator supports three source combinations:

- `Workout`: convert an ERG workout into a procedural MTB route. Interval
  duration, target power, ramps, and recoveries determine climbs, descents,
  connectors, and game features.
- `Past activity`: convert a previously synchronized activity into a compressed
  MTB course and derive an optional editable target-power profile from its
  recorded effort.
- `Workout + past activity`: use the ERG file as the authoritative training
  prescription and the activity as the terrain template. Match hard intervals
  to suitable climbs or technical sections and recoveries to descents or easy
  connectors, synthesizing or resizing terrain only where the source activity
  cannot accommodate the workout safely.

All variants produce the same course data model and open in the same editor.
The generated draft records source identifiers, conversion parameters, and a
deterministic seed for reproducibility, but keeps athlete and activity data in
the private athlete directory.

The command is available as `Generate MTB Course...` in the context menu of a
compatible ERG workout or activity. The wizard then allows an optional second
source, terrain style, connector compression, target duration, trainer
difficulty, and generation seed. Its final preview compares source and output
duration, distance, elevation gain, target load, and any sections that were
trimmed, synthesized, or assigned a safe bypass.

An ERG-only source remains fully useful without GPS or Strava: the procedural
generator creates all terrain. An activity without power remains useful as a
terrain-only source. Missing optional streams degrade generation choices rather
than preventing course creation.

This mode must not reinterpret an existing time-based ERG file implicitly. A
separate course file or an explicit conversion creates a stable distance route.
The conversion UI must show that completion time becomes variable.

### Generating A Course From A Timed Workout

A timed workout can be converted explicitly into a distance course whose
nominal completion time matches the original workout duration. FTP classifies
the training intensity, but FTP alone cannot determine gradient or speed. The
generator also needs rider-plus-bike mass, available virtual gearing, trainer
capabilities, and a selected terrain style.

For each workout interval the generator:

1. Classifies its physiological role from duration, target power relative to
   FTP, ramp direction, and surrounding recovery.
2. Selects a suitable feature and bounded grade range. Short sprints favor a
   fast, shallow approach; 30-second to two-minute efforts favor punchy climbs;
   sustained threshold efforts favor longer moderate climbs; and recovery
   favors descents or low-resistance flow trail.
3. Runs the longitudinal model at the prescribed power and solves the segment
   distance that produces the requested nominal duration.
4. Adds grade transitions and approach/exit terrain, then simulates the complete
   course again to correct accumulated timing error.

For example, a `4 x 30 s @ 250 W` workout at an FTP of `190 W` is classified as
four short anaerobic efforts. Rider mass and the speed model determine whether
each effort becomes, for example, an 80- or 130-meter climb; the ratio to FTP
selects the challenge class but does not alone fix the gradient. Recovery
intervals become descents and connectors sized by their nominal recovery time.

The generated course stores both nominal duration and acceptable time bounds
for every prescribed effort. Course movement remains distance based, but the
training target is protected from extreme pacing errors:

- arriving early can extend a neutral crest or exit line until the minimum
  useful exposure is reached;
- falling substantially behind opens a visible safe bypass at the maximum
  exposure time; and
- automatic adjustment never raises target power or silently extends a hard
  interval beyond its configured bound.

Two completion policies are useful:

- `Fixed Course` never changes generated distances and reports a continuously
  updated finish-time estimate.
- `Time Budget` targets a requested session duration by choosing shorter or
  longer recovery, warm-up, cool-down, and scenic connector branches at
  checkpoints. It does not shorten prescribed work or change its target power.

Thus a one-hour source workout should take approximately one hour when targets
are followed. A confidence range is shown before starting and a live ETA during
the ride. When a strict end time matters more than distance, the existing timed
ERG mode remains the correct choice.

### Activity And Route Templates

Past activities can provide terrain rather than merely training targets. A
Strava activity or locally imported FIT/GPX file may supply distance, altitude,
smoothed grade, time, power, cadence, and heart-rate streams. The generator can:

- reuse an activity's elevation shape as a recognizable virtual route;
- select matching climb or descent fragments from prior rides as interval
  templates;
- scale fragment distance and grade within safe limits to match a new workout;
- estimate personal speed ranges from previous clean efforts; and
- offer a direct replay course with new target zones overlaid.

The workout prescription and terrain template remain separate inputs. Inferring
a structured workout solely from noisy outdoor power is optional and must not
replace an explicitly authored workout. Stops, GPS elevation noise, drafting,
wind, traffic, and missing sensor samples are filtered before historical data is
used for calibration.

Previously synchronized GoldenCheetah activities and local FIT/GPX files are the
preferred offline source. Strava synchronization is an optional source, and all
downloaded or derived athlete data stays in the private athlete directory. It
must never be bundled into the application, tests, screenshots, or repository.

### Building A Course From A Past Activity

The primary historical-data workflow starts from an activity already downloaded
by GoldenCheetah synchronization. It does not make another Strava request. The
activity becomes an editable course draft rather than an immutable geographic
replay.

1. Read moving time, distance, altitude, grade, position, power, cadence, and
   heart rate from the local activity where available.
2. Remove stops, GPS jumps, implausible elevation spikes, and sensor dropouts,
   then build a smoothed distance-indexed elevation profile.
3. Detect meaningful climbs, descents, high-effort passages, technical changes,
   and scenic or recovery transitions.
4. Preserve the characteristic sections while shortening low-information,
   low-effort connectors according to a user-selected compression level.
5. Join retained sections with bounded grade and curvature transitions, run the
   complete physics model, and report distance, elevation gain, and expected
   duration before and after compression.

Compression must not simply shorten horizontal distance while retaining the
same elevation change, because that invents steeper gradients. Long flat
connectors can be shortened directly. For rolling or climbing connectors the
generator selects representative sub-sections or removes proportional distance
and elevation before smoothing the join. The editor marks every generated join
so the user can inspect or undo it.

The first draft can derive a simplified target-power profile from the original
ride using change-point detection and FTP-relative zones rather than copying
every noisy power sample. The user then edits that profile independently of the
terrain. Three starting choices are useful:

- `Original effort`: preserve the major effort and recovery pattern;
- `Normalize to zones`: convert the ride into editable FTP-relative blocks; and
- `Route only`: discard historical power and place a different workout on the
  retained terrain.

The course editor displays elevation and target power together over distance,
with predicted time as a secondary scale. A target block can be either
distance-anchored to a climb or duration-anchored after a course marker. Editing
power updates expected speed and ETA immediately. An explicit `Fit to time`
operation may resize only unlocked recovery and connector sections; it never
silently changes locked work targets or characteristic terrain.

The intended UI flow is:

1. Select a synchronized past activity.
2. Preview detected key sections and choose connector compression.
3. Inspect the generated elevation profile and joins.
4. Generate, normalize, or replace the target-power profile.
5. Edit targets and locks while viewing nominal and confidence-range duration.
6. Test with the Data Generator and save as a reusable private course workout.

### Longitudinal Physics

Road speed is integrated at a fixed game-physics cadence from measured power
and opposing forces. The baseline model is:

```text
wheel power = measured power * drivetrain efficiency
drive force = wheel power / max(speed, low-speed threshold)
net force = drive force
          - aerodynamic drag
          - rolling resistance
          - grade force
acceleration = net force / effective mass
```

The model uses rider-plus-bike mass, air density, `CdA`, rolling coefficient,
gradient, drivetrain loss, and rotating-mass compensation. Low-speed launch,
traction, terminal downhill speed, braking, and numerical integration need
explicit bounds. Arcade tuning may smooth or scale forces, but one named
physics profile must remain deterministic and unit tested.

Virtual gear determines the relationship between cadence, trainer torque, and
road feel. It must not directly create energy or distance. The simulated road
speed comes from power and forces; gear selection changes whether producing that
power feels like a slow/high-torque or fast/low-torque effort.

### Trainer Integration

`TrainSidebar` remains the only component allowed to send trainer commands. The
course model publishes the current grade and requested road-feel parameters;
existing Train control code validates and applies them using slope or resistance
control. Unsupported trainers keep a passive speed simulation and display a
clear capability state.

A trainer-difficulty setting may scale the gradient felt at the pedals without
changing the full gradient used by road-speed physics. This lets a steep virtual
climb remain steep while making its gearing practical on limited hardware.

### Safety And Session Bounds

Because finishing time is unknown, Distance Course requires an independent
maximum-duration guard, ordinary pause/continue/stop behavior, and a visible
remaining-distance estimate. Disconnects freeze trainer commands safely and
allow simulated coasting only for a short bounded period before pausing course
progress. Recording remains time based and must be recoverable even when the
course is not completed.

### Tuning And Validation

The first version should use the Data Generator and recorded telemetry replays,
then be calibrated with a real trainer. Tests cover flat-road acceleration,
coasting to a stop, climb deceleration, downhill terminal speed, no negative or
non-finite speed, virtual-gear energy conservation, pause/disconnect behavior,
and deterministic completion time. Reference scenarios should include a flat
kilometer, a sustained climb, rollers, and a climb followed by a recovery
descent.

Implementation should proceed as a non-rendered physics prototype first. A
speed-versus-time and distance-versus-time plot will make model errors easier to
identify before the mode is connected to trainer resistance or game graphics.

## Determinism And Persistence

Course seeds use a specified integer hash over normalized interval values. They
must not use platform-dependent `std::hash`. Simulation uses a specified small
pseudo-random generator only for visual variants.

The ghost recorder samples score and route changes at a bounded five-second
cadence. Its versioned ASCII codec rejects oversized, malformed, non-monotonic,
or wrong-course data. At stop, a better replay replaces the prior replay for
the same deterministic course seed. Better means greater workout coverage,
then greater final score. Athlete-private settings retain at most 12 course
replays, each capped at 256 KiB. This does not change the activity CSV schema
or recorded physiological data.

## Test Strategy

1. Pure unit tests cover course classification, invalid inputs, deterministic
   seeds, simulation stepping, scoring, pause recovery, and fixed-step catch-up.
2. Qt tests cover `ErgFile` adaptation, lifecycle signals, renderer fallback,
   and perspective/full-screen state.
3. Golden image tests cover a small deterministic scene in OpenGL and QPainter.
4. AppImage smoke tests verify both `xcb` and bundled `offscreen` platforms.
5. Manual release testing uses a copied athlete directory and verifies trainer
   connection, start, pause, continue, stop, recording import, and live mode
   switching without modifying production athlete data.
6. Hardware-free UI testing uses the `Data Generator` training device. It
   follows ERG targets and emits deterministic power, heart rate, cadence, and
   speed telemetry without advertising trainer-control capabilities or sending
   commands to physical hardware.

## Delivery Stages

1. Deterministic course builder and fixed-step simulation.
2. Train chart integration with QPainter fallback.
3. OpenGL compositing and QPainter fallback.
4. Scoring, feature bypasses, and deterministic AI riders.
5. Athlete-private best-attempt ghost rider.
6. Optional recorded-activity metadata and external route adapters.
7. Complete the initial side/three-quarter feature set in this order: roots,
   rollers, climb/hike, rock garden, bunny hop, and drop. Each feature receives
   deterministic physics, success/failure rules, animation, and a rendered UI
   regression test before the next feature is enabled.
8. Add a lightweight GPU pseudo-3D chase projection with perspective trail
   strips, distance-scaled 2D sprites, curves, hills, and a rear/three-quarter
   rider sprite. Keep the bounded QPainter side-view fallback.
9. Blend camera pose during live side, three-quarter, and chase transitions.
   Transition tests must cover workout continuity, renderer fallback, frame
   bounds, and fixed-step physics independence.
10. Enable skinny and berm only after chase-view line choice, trail edges, and
    low-end GPU frame-budget tests pass.
