# Workout Game Architecture

## Goals

Workout Game adds a lightweight 2D riding game to Train view without changing
the workout clock, recording path, or trainer safety model. It must run on old
integrated-GPU laptops and remain useful without a network connection.

The first version uses generated terrain rather than map data. OpenStreetMap
routes can be added later as an optional course source, but exercise execution
must never depend on a map service, network latency, or API credentials.

## Safety Requirements

These are normative requirements for the finished architecture, not guarantees
that the current implementation already provides. In particular, game rules,
Box2D stepping, fallback painting, and Train timers currently share the GUI
thread, and Qt Quick synchronization can block that thread. The deviations and
migration work are listed under `Current Gaps And Priorities`.

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

The builder has no Qt, file-system, map, UI, or trainer dependencies. The
existing workout adapter converts `ErgFile` points into its normalized interval
input.

### WorkoutGameSimulation

The simulation is a pure C++ semi-fixed-step state machine. Inputs are immutable
snapshots containing workout time, power, cadence,
target power, pause state, and virtual gear. Heart rate is displayed by the
renderer but does not affect game rules. Outputs contain course progress,
section state, feature outcome, speed, adherence, and score state.

The simulation uses workout time for longitudinal course progress. Measured
power changes animation speed, line choice, and scoring, but cannot move the
workout timeline. It currently advances in 50 ms quanta plus a final remainder.
Pause preserves score, streak, and transient jump state. Resetting selected
transients after a configurable long pause is a target policy that must be
implemented and tested before it is relied upon.

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
their existing owners. In the target architecture a failed or late physics step
can therefore reduce only visual fidelity. The current shared GUI-thread
execution does not yet provide that scheduling isolation.

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
- Game rules: semi-fixed 50 ms steps. Vehicle physics: fixed 8.33 ms steps. Both
  cap catch-up work after stalls. A future simulation runner may publish at the
  display rate without changing these deterministic internal step sizes.
- Rendering: target 60 FPS on GPU and 30 FPS with QPainter.
- The current scene has bounded geometry, four competitor sprites at most, no
  per-frame texture upload, and no unbounded particle list. Automatic
  frame-budget quality scaling remains a future enhancement.

The target design has no unbounded event queue. Simulation and rendering use a
bounded latest-value snapshot exchange, so stale frames are overwritten rather
than accumulated. The current queued render diagnostics and the proposed Qt
wakeup require explicit coalescing before this is guaranteed.

### Current Execution Model

This section describes the implementation as of August 2026. The following
`Clock And Thread Model` section describes the target architecture; it is not a
claim that a dedicated simulation thread already exists.

GoldenCheetah currently gives Workout Game two execution contexts:

| Context | Current work |
| --- | --- |
| Qt GUI thread | `TrainSidebar` timers, telemetry aggregation, workout position, `WorkoutGameWindow`, game rules, Box2D stepping, camera updates, fallback rendering, HUD image construction, debug capture, and ghost persistence |
| Qt scene graph | `updatePaintNode`, visual interpolation, road timeline sampling, pseudo-3D projection, feature presentation, dynamic vertex generation, GPU node updates, and render diagnostics |

The scene graph context is a separate render thread when Qt selects its
threaded render loop. It may instead be the GUI thread on platforms or graphics
backends using the basic render loop. During threaded scene graph
synchronization Qt blocks the GUI thread, which allows `QQuickItem` state set on
the GUI side to be consumed by `updatePaintNode` without a second application
lock. Code in `updatePaintNode` must still use only scene graph APIs that are
valid in that phase.

There is no Workout Game `QThread`, worker pool, snapshot exchange, or explicit
sequence number yet. Device controllers may use their own implementation
threads, but Workout Game sees only the `Context` signals delivered to its GUI
object and does not depend on controller thread ownership.

The current live path is:

```text
device controllers
       |
       v
TrainSidebar::guiUpdate (nominally 200 ms)
       |
       +--> Context::telemetryUpdate
       |       |
       |       +--> cache latest power/cadence/HR/gear and rebuild HUD
       |
TrainSidebar::loadUpdate (nominally 1000 ms)
       |
       +--> trainer target dispatch and Context::setNow
               |
               v
         WorkoutGameWindow::updateSimulation              [GUI]
               |
               +--> game rules, 50 ms fixed steps
               +--> feature decision
               +--> Box2D vehicle, 8.33 ms fixed steps
               +--> camera and ghost sample
               +--> visual target for each renderer
                              |
                    Qt scene graph synchronization
                              |
                              v
         smooth workout time and sample one road timeline  [render]
               +--> road projection and feature geometry
               +--> QSG geometry/texture node updates
               +--> swap -> request next visible frame
```

Both game rules and vehicle physics cap catch-up at one second. With the normal
one-second workout position cadence, one GUI callback can therefore execute
about 20 game-rule steps and 120 Box2D steps. All those steps use the latest
telemetry value available at callback time; the intermediate 200 ms telemetry
samples are not replayed through simulation. Rendering hides most of this
coarse source cadence by interpolating and briefly predicting workout time,
then maps that single time to road distance, section progress, and feature
presentation.

The primary scene graph renderer schedules another frame from `frameSwapped`
and normally follows display vsync. It rebuilds bounded road and feature vertex
arrays every frame. The OpenGL fallback is a `QOpenGLWidget`: Qt renders it into
an offscreen framebuffer and composites it into the top-level widget rather
than giving it an independently swappable native double buffer. Its scene is
still painted by `QPainter` on the GUI thread. The final
fallback uses a 33 ms GUI timer. Direct PNG capture calls `grabWindow` and writes
the image from the GUI side; it is intentionally a diagnostic path and can
disturb frame pacing.

#### Current Engine State Ownership

The current implementation is a collection of deterministic domain modules
coordinated by `WorkoutGameWindow`, not a separate general-purpose game engine.
Mutable state has the following owners:

| State | Owner and update point |
| --- | --- |
| Selected workout, latest telemetry, pause/session flags | `WorkoutGameWindow` on the GUI thread |
| Score, adherence, section outcomes and nominal speed | `WorkoutGameSimulation::update` on `Context::setNow` |
| Feature decisions and one-shot action IDs | `WorkoutGameFeatureRuntime::update` on the same GUI callback |
| Box2D world, vehicle contacts and suspension | `WorkoutGamePhysics::update` on the same GUI callback |
| Camera target and transition | `WorkoutGameCamera::update` on the same GUI callback |
| Workout-time-to-road mapping | Immutable `WorkoutGameRoadCourse`, sampled by the scene graph |
| Visual interpolation, projected trail and render diagnostics | `WorkoutGameSceneGraphItem::updatePaintNode` during Qt scene graph synchronization |
| HUD source image and telemetry labels | `WorkoutGameSceneGraphItem` on the GUI thread; copied to a scene graph texture during synchronization |
| Saved ghost | `WorkoutGameGhostRecorder` in the window and athlete settings on stop |

Course builders and adapters are pure or value-oriented preparation modules.
They do not participate in the frame loop. The primary renderer keeps QSG nodes
alive between frames but currently regenerates their dynamic vertex contents.
Box2D owns only the local visual bicycle world; the time-to-road timeline owns
the primary rendered longitudinal position. This split is deliberate for
workout safety, but it also means that road position and the Box2D bicycle are
not yet one authoritative simulated body.

The GUI-to-render handoff currently relies on Qt Quick's synchronization phase.
`setFrame`, `setTelemetry`, and `setCourse` mutate the `QQuickItem` on the GUI
thread. `updatePaintNode` reads that state while the GUI thread is blocked by a
threaded render loop, or on the GUI thread under the basic render loop. Render
diagnostics return through queued invocations. This pattern must not be treated
as a general lock-free mailbox: only the Qt-documented synchronization window
protects these item fields, and queued callbacks must not retain scene graph
nodes or textures whose lifetime belongs to the render thread.

### Current Gaps And Priorities

1. **P1: GUI-thread simulation bursts.** Trainer target timers, UI work, game
   rules, Box2D catch-up, and camera updates share one event loop. A slow game
   update can delay trainer/UI callbacks even though the game never calls the
   trainer directly. Move stateful simulation to one fixed-rate owner and keep
   only bounded snapshot publication on the GUI path.
2. **P1: telemetry history is discarded.** Five nominal telemetry samples can
   arrive between two simulation updates, but only the newest sample is applied
   to the whole elapsed second. Introduce a small timestamped input ring and
   consume samples at fixed simulation ticks. Bound its size and collapse old
   data explicitly after stalls.
3. **P1: telemetry time and validity are undefined.** Normalize device and
   ingestion timestamps into one monotonic domain, reject or count out-of-order
   samples, and attach a maximum age to held values. Disconnected or stale power
   must not continue moving the rider, satisfying features, or awarding score.
   Every input and lifecycle command carries the active session generation.
4. **P1: presentation predicts beyond authoritative state.** The monotonic
   visual smoother can predict up to 1.5 seconds while game outcomes and Box2D
   remain at the last workout position. The unified road timeline prevents the
   known backward section transition, but previous/current simulation snapshots
   with render interpolation are the durable solution. The training session's
   workout position remains authoritative; a monotonic scheduler may pace work
   but must not create a second workout timeline.
5. **P2: no snapshot identity or latency accounting.** The current Qt sync is
   safe for GUI-to-scene-graph transfer, but diagnostics cannot state which
   input and simulation tick produced a frame. Add input, simulation, and frame
   sequence numbers plus sensor-to-display latency. A future direct cross-thread
   buffer also needs an explicit ownership and memory-order protocol; a plain
   shared struct plus index would be a data race.
6. **P2: render work scales poorly with richer courses.** Projection sampling,
   temporary vectors, and complete dynamic geometry updates occur every frame.
   The current bounded scene has measured headroom, so optimize only after
   counters show pressure: reuse buffers, retain unchanged chunks, and batch by
   material before adding workers.
7. **P2: the visible scene renders while training is inactive.** The
   `frameSwapped` loop checks window visibility but not session state. Stop the
   continuous loop when inactive and request isolated frames for setup or HUD
   changes.
8. **P2: HUD changes recreate a texture.** Telemetry and changing FPS values
   rebuild a `QImage`, then replace its scene graph texture. Rate-limit debug
   values or use a persistent dynamic texture if profiling shows upload cost.
9. **P2: course creation is synchronous.** ERG normalization and current small
   procedural courses are cheap, but imported long activities and future asset
   decoding must run as cancelable jobs before their immutable result is
   installed on the GUI thread.
10. **P2: physics frequency has not been justified by profiling.** The current
   vehicle path calls Box2D at 120 Hz with four solver substeps, potentially 480
   solver substeps during a one-second catch-up burst. Box2D documents a fixed
   60 Hz primary step with substeps as a common starting point. Compare 60/4,
   120/2, and the current 120/4 against contact quality and CPU time before
   treating the current rate as a requirement.
11. **P2: scene graph pacing assumes working vsync.** The self-scheduled
    `frameSwapped` loop has no software frame cap. It follows the display on the
    normal desktop, but headless X11 traces ran at 118-140 FPS. Record the
    graphics API, render loop, swap interval, and presented-frame cadence, and
    add an inactive or no-vsync limiter if real systems show waste or jitter.

### Clock And Thread Model

The game has three clock domains with explicit ownership:

1. Sensor samples retain their device or ingestion timestamps. Trainer control
   and activity recording continue on the existing Train paths and never wait
   for the game.
2. Train publishes an authoritative anchor
   `{workoutPosition, monotonicAnchor, running, generation}` whenever position or
   lifecycle state changes. `WorkoutGameClock` uses monotonic time to schedule
   one deterministic fixed-step owner and, only while `running`, derives a tick's
   workout position from that anchor. A newer anchor corrects drift; backward
   movement, a large correction, seek, reset, and workout replacement are
   explicit generation discontinuities. This gives smooth positions between the
   current one-second Train updates without creating a second independent
   workout clock.
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
publishes an immutable `WorkoutGameFrameSnapshot` with a generation and
monotonically increasing sequence number. The first implementation should use a
bounded latest-snapshot slot and at most one outstanding queued Qt wakeup. The
producer replaces the slot, atomically marks a wakeup pending, and queues only
when changing that flag from clear to set. The GUI drains the newest value,
clears the flag, and rearms if publication raced with draining. It then relies
on the existing Qt scene graph synchronization for GUI-to-render transfer.
Queued invocation per simulation tick is not acceptable because a stalled GUI
would accumulate Qt events.

If profiling later justifies a direct simulation-to-render exchange, use a
bounded single-producer/single-consumer triple buffer with explicit slot
ownership and release/acquire publication. The producer must never overwrite a
slot that the consumer can still read. The renderer reads the newest complete
pair and never waits for a partially written frame; old snapshots are
overwritten rather than queued. A plain non-atomic active index is not a valid
implementation.

The target thread ownership is:

| Thread or execution context | Responsibilities | Must not do |
| --- | --- | --- |
| Existing device/Train paths | Read sensors, control resistance, record activity and timestamp telemetry | Wait for game simulation or rendering |
| Qt GUI thread | Own widgets/windows, translate user commands, install completed courses, publish bounded inputs and handle session lifecycle | Step Box2D, run catch-up loops or wait for workers |
| One simulation thread | Own all mutable rules, feature, Box2D and camera state; consume inputs; run fixed ticks; publish complete snapshots | Access QObjects, QSG resources, athlete storage or trainer controllers |
| Qt scene graph render context | Interpolate published snapshots, update retained QSG nodes and submit GPU work | Mutate simulation state or block for a newer snapshot |
| Optional worker jobs | Build long courses and decode assets into immutable results | Publish partial results or join from Train/render hot paths |

There is intentionally no thread per subsystem. The current Box2D world and
game rules are small, ordered and stateful; splitting rules, physics and camera
across workers would add synchronization and nondeterministic ordering without
removing measured work. Box2D stepping remains on one owner even if Box2D's
internal worker support is evaluated later.

#### Fixed-Step Loop

The simulation thread sleeps or waits for input until the next absolute
monotonic deadline. Monotonic elapsed time decides how many ticks are due, while
the Train anchor determines each tick's authoritative workout position. A
conceptual iteration is:

```text
anchor = newest Train lifecycle/position anchor
due_ticks = bounded_ticks_since_absolute_deadline(monotonic_now)
for each due tick:
    tick_workout_position = derive_from(anchor, tick_monotonic_time)
    apply timestamped inputs valid at tick_monotonic_time
    advance rules, feature state, Box2D and camera to tick_workout_position
publish the newest complete snapshot and coalesce one GUI wakeup
if more ticks were due:
    apply the documented catch-up-discard policy and report a discontinuity
```

The initial discard policy realigns positional state to the current Train
anchor without synthesizing score or feature success for the skipped interval.
It invalidates an active challenge whose decision window was crossed and emits
one discontinuity event. It does not aggregate missed power into a later tick.

The loop uses absolute monotonic deadlines to avoid accumulating timer drift.
Waking the thread is not itself a simulation tick: early and spurious wakes only
refresh inputs or lifecycle commands. Pause freezes game-time integration while
telemetry and recording continue through Train. Resume resets the monotonic
deadline so paused wall time is never caught up. Seek, workout replacement and
session restart increment a generation, reset incompatible transient state and
publish a non-interpolated snapshot; the renderer never blends snapshots from
different generations.

Timestamped telemetry uses a bounded single-producer/single-consumer mailbox or
ring after timestamps have been normalized to the monotonic scheduler domain.
The simulation consumes samples in timestamp order and holds the newest valid
value only until its configured maximum age. Overflow, out-of-order input, and
stale-value invalidation have separate counters. Overflow drops or coalesces the
oldest unconsumed telemetry; it cannot grow memory or block sensor ingestion.
Lifecycle commands such as stop, reset and course replacement use an ordered
command channel, carry the same generation, and take precedence over telemetry
coalescing. The ordering barrier prevents a sample from an old session being
applied after a reset from another channel.

#### Snapshot Publication And Lifetime

Each frame snapshot contains at least generation, simulation sequence,
simulation time, source-input sequence, monotonic publication time, rules,
world, camera and presentation state. Publication uses release/acquire ordering
or a small locked handoff whose critical section copies only bounded values.
The producer never overwrites a slot still being copied by the consumer. A
plain shared index plus two mutable slots is insufficient without a proven
ownership protocol; triple buffering or immutable shared ownership is safer
for the first implementation.

The renderer keeps the latest two timestamped snapshots from the same
generation and interpolates presentation state using the simulation clock and
display time. A skipped snapshot sequence is expected with latest-value
publication and does not by itself disable interpolation; generation mismatch,
reversed time, excessive snapshot age, or an invalid interval does. Discrete
sound, jump, result, and one-shot animation events are not numerically
interpolated. They use a small bounded journal with cumulative event sequence
IDs retained until the consumer acknowledges or passes them, so overwriting a
frame snapshot cannot erase an event. If only one valid snapshot exists, the
renderer displays it without extrapolating. Missing frames reduce visual
smoothness but cannot feed back into physics, scoring, trainer control or
recording.

Qt Quick already owns GUI/render synchronization and can use a dedicated render
thread. Scene graph updates therefore remain small snapshot-to-node copies;
GPU submission stays in Qt's render loop. Course generation, asset decoding,
and other measured heavy work may use worker jobs, but workers publish results
for a later simulation tick and are never joined from trainer control or the
render hot path. The current vehicle simulation should remain one owner until
profiling proves that a phase is large enough to justify job scheduling.
Course changes, seeks, and shutdown increment the generation and cancel pending
jobs without holding a trainer or render-path lock. Shutdown first rejects new
inputs, requests runner interruption, invalidates queued callbacks by generation,
joins the runner outside Train/render locks, and only then destroys its QObject
facade. A final snapshot is optional and must carry the closing generation. A
snapshot or event from an older generation is discarded. Box2D state is created,
used, and destroyed only by the simulation owner.

This follows the fixed-timestep and render-interpolation model used by mature
engines while respecting Qt's scene graph ownership rules:

- [Qt Quick Scene Graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)
- [QQuickItem scene graph update rules](https://doc.qt.io/qt-6/qquickitem.html#updatePaintNode)
- [QOpenGLWidget composition](https://doc.qt.io/qt-6/qopenglwidget.html)
- [Qt queued invocation](https://doc.qt.io/qt-6/qmetaobject.html)
- [Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/)
- [Godot physics interpolation](https://docs.godotengine.org/en/stable/tutorials/physics/interpolation/physics_interpolation_introduction.html)
- [Box2D simulation](https://box2d.org/documentation/md_simulation.html)
- [Box2D foundation and threading](https://box2d.org/documentation/md_foundation.html)

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
The matrix must exercise Qt's `basic` and `threaded` render loops, working and
missing vsync, stale and out-of-order telemetry, pause/resume, GUI stalls, and
shutdown during catch-up. Replay verification records the application revision,
course seed, Box2D configuration, input log, and either a stable state hash or
documented numeric tolerances.

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

`WorkoutGameCompetition` can generate deterministic AI riders whose pacing
curves derive from the course seed and whose placement is based on score. The
live window currently publishes an empty competition snapshot, so AI riders are
disabled while their terrain placement and presentation are redesigned. When
reenabled, small visual progress offsets may show whether a rider is ahead
without changing authoritative workout progress or the trainer target.

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
