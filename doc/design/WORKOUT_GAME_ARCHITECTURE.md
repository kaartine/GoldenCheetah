# Workout Game Architecture

## Goals

Workout Game adds a lightweight 2D riding game to Train view without changing
the workout clock, recording path, or trainer safety model. It must run on old
integrated-GPU laptops and remain useful without a network connection.

The first version uses generated terrain rather than map data. OpenStreetMap
routes can be added later as an optional course source, but exercise execution
must never depend on a map service, network latency, or API credentials.

## Safety Requirements

These are normative requirements for the implementation. Game rules and Box2D
stepping run on the low-priority Workout Game worker; Train timers and fallback
painting remain on the GUI thread. Qt Quick synchronization can briefly block
that thread, so rendering is bounded, optional, and never owns trainer or
recording work. Remaining deviations are listed under
`Current Gaps And Priorities`.

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
their existing owners. A failed or late physics step can therefore reduce only
visual fidelity. The fixed-step runner now provides that scheduling isolation
from the GUI, trainer control, and activity recording paths.

`WorkoutGameCamera` consumes world snapshots and produces a renderer-neutral
camera pose. Smooth terrain, climbs, jumps and drops favor a side view; roots,
rollers and rock gardens favor a three-quarter view. Skinny and berm features
are reserved for a chase camera because their line choice cannot be conveyed
reliably from the side. Camera angles blend over time, while a new physics-world
generation performs an explicit cut instead of interpolating unrelated
coordinates.

The chase camera follows the sampled trail surface, not the airborne chassis.
Vehicle clearance above the nominal loaded ride height is published as the
only visual air gap and is applied once to the rider sprite. This keeps a real
Box2D jump visible without moving the camera and projected ground in the
opposite direction or treating normal chassis clearance as flight.

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
coordinator: it owns the course, runner, competition model, and ghost recorder;
subscribes to existing `Context` signals; and publishes immutable snapshots to
the selected renderer only while the chart is visible. The runner's worker owns
simulation, feature, physics, and camera
state. The window does not own a trainer controller. The existing chart system
allows the game to coexist with current perspectives and switch to full-screen
without restarting the session.

The window selects one renderer at construction:

- `WorkoutGameSceneGraphWindow`: the primary retained Qt Quick scene graph and
  GPU renderer, paced by completed presentation swaps.
- `WorkoutGameOpenGLCanvas`: a `QOpenGLWidget` which uses Qt's OpenGL paint
  engine to composite bundled bitmap assets and the shared scene at 60 FPS.
- `WorkoutGameCanvas`: QPainter fallback using the same render snapshot and
  bundled assets at 30 FPS.

Renderer selection is capability-based. Runtime rendering errors permanently
fall back for the current session instead of repeatedly recreating a GPU
context.

Only the selected renderer receives frames. Hiding the chart drains its newest
frame, stops the GUI drain and scene graph request loops, and suspends that
chart's runner. Showing it re-anchors and resynchronizes the engine at the
current workout position before simulation resumes. This prevents multiple
hidden Workout Game charts from retaining independent 50 Hz Box2D workloads;
Train telemetry, control, and recording continue unchanged.

## Scheduling And Performance

- Trainer command cadence: owned by existing Train code and highest priority.
- Recording cadence: owned by existing Train code and independent of the game.
- Telemetry ingestion: existing cadence, copied into a small latest-value
  snapshot without waiting for rendering.
- Simulation runner: fixed 20 ms deadlines on one worker. Game rules retain
  semi-fixed 50 ms steps and vehicle physics retains fixed 8.33 ms steps. All
  layers cap catch-up work after stalls.
- Rendering: target 60 FPS on GPU and 30 FPS with QPainter.
- The current scene has bounded geometry, four competitor sprites at most, no
  per-frame texture upload, and no unbounded particle list. Automatic
  frame-budget quality scaling remains a future enhancement.

The implementation has no unbounded frame event queue. Simulation and rendering use a
bounded latest-value snapshot exchange, so stale frames are overwritten rather
than accumulated. Render diagnostics are rate-limited to four queued callbacks
per second, and HUD texture rebuilding is limited to 10 Hz.

### Current Execution Model

This section describes the implementation as of August 2026.

GoldenCheetah gives Workout Game three execution contexts:

| Context | Current work |
| --- | --- |
| Qt GUI thread | `TrainSidebar` timers, telemetry aggregation, workout position, `WorkoutGameWindow`, bounded input/output copies, fallback rendering, HUD image construction, debug capture, and ghost persistence |
| Workout Game runner | Fixed clock, game rules, feature decisions, Box2D stepping, camera updates, and complete immutable frame publication |
| Qt scene graph | `updatePaintNode`, two-snapshot visual interpolation, road timeline sampling, pseudo-3D projection, feature presentation, dynamic vertex generation, GPU node updates, and render diagnostics |

The scene graph context is a separate render thread when Qt selects its
threaded render loop. It may instead be the GUI thread on platforms or graphics
backends using the basic render loop. During threaded scene graph
synchronization Qt blocks the GUI thread, which allows `QQuickItem` state set on
the GUI side to be consumed by `updatePaintNode` without a second application
lock. Code in `updatePaintNode` must still use only scene graph APIs that are
valid in that phase.

`WorkoutGameRunner` owns one `std::thread`; there is intentionally no worker
pool. Device controllers keep their existing ownership. The game sees only
bounded input copies from `Context`, never accesses a controller, and never
waits in telemetry, recording, or trainer-command callbacks.

The current live path is:

```text
device controllers
       |
       v
TrainSidebar::guiUpdate (nominally 200 ms)             [GUI]
       |
       +--> Context::telemetryUpdate
       |       |
       |       +--> copy latest power/cadence/HR/gear to runner input slot
       |
TrainSidebar::loadUpdate (nominally 1000 ms)
       |
       +--> trainer target dispatch and Context::setNow
               |
               v
         publish authoritative workout-time anchor          [GUI]
                              |
                              v
         WorkoutGameRunner absolute 20 ms clock             [worker]
               +--> game rules, feature decision and Box2D
               +--> camera update and complete frame snapshot
               +--> overwrite bounded latest-frame slot
                              |
            visible-only 16 ms newest-frame drain           [GUI]
                              |
                    Qt scene graph synchronization
                              |
                              v
         interpolate previous/current frame snapshots       [render]
               +--> sample one road timeline
               +--> road projection and feature geometry
               +--> QSG geometry/texture node updates
               +--> 16 ms presentation request while session is active
```

The runner uses absolute monotonic deadlines so wakeup error does not accumulate
as clock drift. It executes at most four overdue 20 ms publication ticks and
then skips stale deadlines. Simulation snapshots carry their intended
presentation timestamp. Rendering stays one tick behind and interpolates the
previous/current complete pair; it does not extrapolate runner state past the
latest physics result. The current input mailbox deliberately keeps only the
latest telemetry value, so timestamped telemetry replay remains future work.
Lifecycle generation changes and an engine tick share a narrow lifecycle
barrier. A start, pause, resume, stop, or shutdown therefore cannot retire a
generation halfway through a mutating engine update. Telemetry and anchor
publication do not take this barrier; they still copy into the bounded input
slot immediately. A lifecycle action can wait only for the currently executing
bounded tick, never for rendering, the GUI drain, or an event queue.

The primary scene graph renderer requests the next presentation from Qt's
completed `frameSwapped` callback and is normally bounded by display vsync.
The same completed swap timestamps realized FPS and frame-interval metrics.
Requests continue while its visible session is active and for a bounded 250 ms
tail after stopping; isolated frame updates retain a bounded 1.7 second
interpolation window. The renderer
rebuilds bounded road and feature vertex arrays every frame. The OpenGL fallback
is a `QOpenGLWidget`: Qt renders it into an offscreen framebuffer and composites
it into the top-level widget rather than giving it an independently swappable
native double buffer. Its scene is still painted by `QPainter` on the GUI
thread. The final fallback uses a 33 ms GUI timer. Direct PNG capture calls
`grabWindow` and writes the image from the GUI side; it is intentionally a
diagnostic path and can disturb frame pacing.

#### Current Engine State Ownership

The current implementation is a collection of deterministic domain modules
coordinated by `WorkoutGameWindow`, not a separate general-purpose game engine.
Mutable state has the following owners:

| State | Owner and update point |
| --- | --- |
| Selected workout, latest telemetry, pause/session flags | `WorkoutGameWindow` on the GUI thread |
| Score, adherence, section outcomes and nominal speed | `WorkoutGameEngine` on the runner thread |
| Feature decisions and one-shot action IDs | `WorkoutGameEngine` on the runner thread |
| Box2D world, vehicle contacts and suspension | `WorkoutGameEngine` on the runner thread |
| Camera target and transition | `WorkoutGameEngine` on the runner thread |
| Workout-time-to-road mapping and sampled surface | Immutable `WorkoutGameRoadCourse`; the engine publishes its authoritative distance, while Box2D and the scene graph sample the same base elevation, feature surface offset, and grade |
| Feature and trail meshes | Immutable course-space values from `WorkoutGameMeshLibrary`; feature meshes are anchored to the base surface and projected by the scene graph so the course feature height is not applied twice |
| Visual interpolation, projected trail and render diagnostics | `WorkoutGameSceneGraphItem::updatePaintNode` during Qt scene graph synchronization |
| Runner input and latest output slot | Separate small mutexes in `WorkoutGameRunner`; heavy simulation occurs outside both critical sections |
| HUD source image and telemetry labels | `WorkoutGameSceneGraphItem` on the GUI thread, rebuilt at most 10 Hz; copied to a scene graph texture during synchronization |
| Saved ghost | `WorkoutGameGhostRecorder` in the window and athlete settings on stop |

Course builders and adapters are pure or value-oriented preparation modules.
They do not participate in the frame loop. The primary renderer keeps QSG nodes
alive between frames but currently regenerates their dynamic vertex contents.

The generated road is a singletrack: its nominal full width is 1.36 metres,
scaled only for terrain-specific features. Each puzzle piece adds bounded,
zero-value and zero-slope relief at both connectors. The connector spline,
continuous relief, technical surface and obstacle profile are summed by
`WorkoutGameRoadCourse::sample()`, and that exact elevation and derivative are
used by both Box2D and scene projection. Feature meshes remain anchored to the
base surface so their obstacle height is not applied twice.

Jump effort is intentionally local to the feature. The simulation measures the
late approach window, while the generated gate clamps the visible preparation
segment to at most six metres before the decision line. Jump obstacles are
placed no more than five metres beyond that line; long workout intervals cannot
therefore turn one short MTB burst into a prolonged power prompt.
The workout-time-to-road timeline owns longitudinal progress. Each engine tick
passes that distance to Box2D and publishes it with the resulting vehicle pose,
feature state, and camera. `WorkoutGameRoadCourse::sample()` supplies the same
surface elevation and grade to collider construction and pseudo-3D projection,
so a visible obstacle and its physical response share course coordinates.
Feature pieces also publish a canonical surface offset derived from their
difficulty. The same dimensions drive the physical road surface and the visible
log, tabletop, rock slab, or drop mesh.

#### Course-Space 2.5D Geometry

`WorkoutGameMesh` is the presentation-neutral geometry contract for reusable
trail pieces and features. Vertices use course-local forward, right, and up
coordinates and carry normalized UV coordinates. Indexed triangles reference
material slots instead of fixed textures. An instance adds course distance,
lateral/elevation offsets, yaw, and per-axis scale before
`WorkoutGameMeshProjector` maps it through the road camera. This allows flat
colors now and atlas textures later without changing course generation.

Each model also exposes entry and exit connectors plus local collision boxes.
Connectors let generated trail tiles meet as puzzle pieces with compatible
width and elevation. Collision boxes are data only in the current release;
feature success remains authoritative in `WorkoutGameFeatureChallenge`. A
future Box2D adapter may install those boxes as fixtures, but rendering must
never decide workout progress, resistance, recording, or feature rewards.

The scene graph projects only the bounded near/far course window. Source mesh
triangles that cross either plane are clipped before projection instead of
being dropped. Projected geometry is then clipped at the nearest visible crest
and all road, shoulder, ground, cue, and feature triangles are depth-sorted into
one terrain node. This avoids geometry from separate QSG nodes painting through
nearer terrain. Camera elevation comes from the interpolated presentation
snapshot, so visual suspension motion does not reintroduce fixed-step jitter.

The GUI-to-render handoff currently relies on Qt Quick's synchronization phase.
`setFrame`, `setTelemetry`, and `setCourse` mutate the `QQuickItem` on the GUI
thread. `updatePaintNode` reads that state while the GUI thread is blocked by a
threaded render loop, or on the GUI thread under the basic render loop. Render
diagnostics return through queued invocations. This pattern must not be treated
as a general lock-free mailbox: only the Qt-documented synchronization window
protects these item fields, and queued callbacks must not retain scene graph
nodes or textures whose lifetime belongs to the render thread.

### Current Gaps And Priorities

1. **P1: telemetry history is still collapsed to the newest sample.** The
   current monotonic timestamp and two-second expiry prevent disconnected power,
   cadence, speed, or heart rate from remaining active indefinitely. A bounded
   timestamped ring is still needed to consume multiple samples in order, count
   rejected/out-of-order values, and support field-specific validity periods.
2. **P1: end-to-end scheduling and frame-pacing evidence is incomplete.** Add a
   virtual-clock runner/executor test and retain a bounded raw presentation
   timestamp series. Release analysis now reports observed p95/p99/max plus the
   renderer's rolling p95 and maximum; missed-refresh and burst-sequence
   reporting remain.
3. **P2: incomplete snapshot identity and latency accounting.** Runner frames
   have publication sequences and timestamps, but diagnostics cannot yet state
   which telemetry sample produced a frame. Add input sequences plus
   sensor-to-display latency.
4. **P2: generation does not yet identify telemetry inputs.** Runner lifecycle,
   output publication, render snapshots, and interpolation are generation-tagged,
   so resume/restart cannot blend two sessions. Timestamped telemetry still needs
   the same identity before stale-session rejection is independently testable
   from device ingestion through presentation.
5. **P2: direct render handoff remains Qt synchronized.** A future direct
   cross-thread buffer needs an explicit ownership and memory-order protocol; a
   plain shared struct plus index would be a data race.
6. **P2: render work scales poorly with richer courses.** Projection sampling,
   temporary vectors, and complete dynamic geometry updates occur every frame.
   The current bounded scene has measured headroom, so optimize only after
   counters show pressure: reuse buffers, retain unchanged chunks, and batch by
   material before adding workers.
7. **P2: HUD changes recreate a texture.** Telemetry and realized FPS values
   rebuild a `QImage` at most 10 Hz, then replace its scene graph texture. Use a
   persistent dynamic texture only if production profiling still shows pressure.
8. **P2: course creation is synchronous.** ERG normalization and current small
   procedural courses are cheap, but imported long activities and future asset
   decoding must run as cancelable jobs before their immutable result is
   installed on the GUI thread.
9. **P2: physics frequency has not been justified by profiling.** The vehicle
   path calls Box2D at 120 Hz with four solver substeps. Box2D documents a fixed
   60 Hz primary step with substeps as a common starting point. Compare 60/4,
   120/2, and the current 120/4 against contact quality and CPU time before
   treating the current rate as a requirement.
10. **P2: presentation cadence needs production telemetry.** The render request
    loop follows completed swaps while the runner drain has a 16 ms cap. Record
    graphics API, Qt render loop, swap interval, requested frames, presented
    frames, and sensor-to-display latency before adding adaptive quality or a
    display-rate-specific scheduler.

### Clock And Thread Model

The game has three clock domains with explicit ownership:

1. Sensor samples retain their device or ingestion timestamps. Trainer control
   and activity recording continue on the existing Train paths and never wait
   for the game.
2. Train publishes an authoritative anchor
   `{workoutPosition, monotonicAnchor, running}` whenever position or lifecycle
   state changes. `WorkoutGameClock` uses monotonic time to schedule
   one deterministic fixed-step owner and, only while `running`, derives a tick's
   workout position from that anchor. A newer anchor corrects drift. Backward
   movement, a large correction, seek, reset, and workout replacement reset or
   cut interpolation through an explicit session generation carried in render
   snapshots. This gives smooth positions between the current
   one-second Train updates without creating a second independent workout clock.
3. Presentation follows the display refresh. The renderer interpolates between
   the previous and current complete simulation snapshots. It must not
   extrapolate independently smoothed position, section, feature, or camera
   values because those values can then disagree at interval boundaries.

The implemented steady-state data flow is:

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
publishes a complete `WorkoutGameEngineFrame` with a monotonically increasing
sequence number into a one-element latest-frame slot. A 16 ms GUI timer copies
only the newest frame, so a stalled GUI cannot accumulate simulation events. It
then relies on the existing Qt scene graph synchronization for GUI-to-render
transfer. Both input and output critical sections copy bounded values only;
clock, rules, camera and Box2D work occur without those locks held. The separate
lifecycle barrier covers one engine tick plus publication so a rejected
old-generation frame cannot leave unpublished mutations behind. It is not used
by normal telemetry or trainer-target paths.

If profiling later justifies a direct simulation-to-render exchange, use a
bounded single-producer/single-consumer triple buffer with explicit slot
ownership and release/acquire publication. The producer must never overwrite a
slot that the consumer can still read. The renderer reads the newest complete
pair and never waits for a partially written frame; old snapshots are
overwritten rather than queued. A plain non-atomic active index is not a valid
implementation.

The thread ownership is:

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
if stale deadlines were skipped:
    resynchronize rules and world position at the first retained deadline
    without advancing Box2D or publishing an intermediate frame
for each due tick:
    tick_workout_position = derive_from(anchor, tick_monotonic_time)
    apply newest bounded input snapshot
    advance rules, feature state, Box2D and camera to tick_workout_position
publish the newest complete snapshot into the latest-frame slot
if more ticks were due:
    skip stale publication deadlines and increment the skip counter
```

The publication clock realigns to its absolute deadline after retaining at most
four deadlines. When a stall exceeds that bound, the first retained deadline is
a paused resynchronization point and only the remaining deadlines advance rules
and Box2D. A one-second scheduler stall therefore cannot be transformed into a
one-second Box2D burst. Game rules and Box2D retain their own bounded catch-up
policies for ordinary source-time changes inside a retained tick.
Because the current mailbox contains only the latest telemetry value, that value
may still cover a bounded elapsed interval after a stall. Timestamped input
replay and explicit invalidation of a crossed challenge window remain P1 work.

The loop uses absolute monotonic deadlines to avoid accumulating timer drift.
Waking the thread is not itself a simulation tick: early and spurious wakes only
refresh inputs or lifecycle commands. Pause freezes game-time integration while
telemetry and recording continue through Train. Resume resets the monotonic
deadline so paused wall time is never caught up. Workout replacement and session
restart reset the engine; the smoother also cuts on detected timeline or world
discontinuities. Runner lifecycle commands and render snapshots carry a
generation, and publication
rejects a completed frame when pause, stop, restart, replacement, or shutdown
has invalidated that generation. Extending this identity through timestamped
telemetry remains P2 work.
Anchor corrections also carry a separate publication epoch. They retire queued
or in-flight pre-anchor frames without forcing the renderer to cut interpolation
for routine forward synchronization. Stop retires publication and takes the
latest unconsumed frame atomically, so final display and ghost state are not
lost by clearing the one-slot mailbox first.

The next telemetry iteration should use a bounded single-producer/single-consumer
ring after timestamps have been normalized to the monotonic scheduler domain.
The simulation consumes samples in timestamp order and holds the newest valid
value only until its configured maximum age. Overflow, out-of-order input, and
stale-value invalidation have separate counters. Overflow drops or coalesces the
oldest unconsumed telemetry; it cannot grow memory or block sensor ingestion.
That iteration should also move lifecycle commands such as stop, reset and
course replacement onto an ordered command channel. Commands should carry the
same generation and take precedence over telemetry coalescing. The ordering
barrier then prevents a sample from an old session being applied after a reset
from another channel.

#### Snapshot Publication And Lifetime

Each current frame contains a publication sequence, simulation time, monotonic
presentation time, rules, world, camera and HUD telemetry. Publication uses a
small locked handoff whose critical section copies only bounded values.
The producer never overwrites a slot still being copied by the consumer. A
plain shared index plus two mutable slots would be insufficient without a proven
ownership protocol.

The renderer keeps a bounded history of timestamped snapshots from the same
session generation and interpolates presentation state using the simulation clock and
display time. A skipped snapshot sequence is expected with latest-value
publication and does not by itself disable interpolation; reversed time, an
invalid interval, or a detected world reset does. Discrete action IDs are copied
from the newer snapshot instead of numerically interpolated. A bounded event
journal is still required before adding sound or any one-shot event that must
survive an overwritten frame. If only one valid snapshot exists, the renderer
displays it without extrapolating. Missing frames reduce visual smoothness but
cannot feed back into physics, scoring, trainer control or recording.

Qt Quick already owns GUI/render synchronization and can use a dedicated render
thread. Scene graph updates therefore remain small snapshot-to-node copies;
GPU submission stays in Qt's render loop. Course generation, asset decoding,
and other measured heavy work may use worker jobs, but workers publish results
for a later simulation tick and are never joined from trainer control or the
render hot path. The current vehicle simulation should remain one owner until
profiling proves that a phase is large enough to justify job scheduling.
Course replacement and shutdown signal the runner and join it without holding a
trainer, input-mailbox, output-mailbox, or render-path lock. The output slot is
cleared before a replacement is configured, so a failed course cannot expose a
stale frame. A generation check also prevents work that was already in flight
at pause or shutdown from being published afterwards. Configuration and all
subsequent Box2D state creation, use, reset, and destruction happen on the
simulation owner. Explicit generation-tagged cancellation is still required if
asynchronous course jobs are added.

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

#### Engine Source Review

The implementation choices above were checked against upstream engine source,
not copied from an engine or added as a new runtime dependency:

| Project | Relevant upstream implementation | Practical conclusion | License |
| --- | --- | --- | --- |
| Godot | [`main/main_timer_sync.cpp`](https://github.com/godotengine/godot/blob/9ba32b09e0dfa4a6c1b82312554894615c716cce/main/main_timer_sync.cpp), [`main/main.cpp`](https://github.com/godotengine/godot/blob/9ba32b09e0dfa4a6c1b82312554894615c716cce/main/main.cpp), and [`scene/main/scene_tree_fti.cpp`](https://github.com/godotengine/godot/blob/9ba32b09e0dfa4a6c1b82312554894615c716cce/scene/main/scene_tree_fti.cpp) | Accumulate real time, execute a bounded number of fixed physics steps, retain the remainder as an interpolation fraction, and interpolate previous/current transforms. Keep a maximum step count to avoid a stall-induced spiral. | [MIT](https://github.com/godotengine/godot/blob/9ba32b09e0dfa4a6c1b82312554894615c716cce/LICENSE.txt) |
| Bevy | [`crates/bevy_time/src/fixed.rs`](https://github.com/bevyengine/bevy/blob/70a1fb4fddc57972c722d1f49919b771687b940d/crates/bevy_time/src/fixed.rs), [`examples/movement/physics_in_fixed_timestep.rs`](https://github.com/bevyengine/bevy/blob/70a1fb4fddc57972c722d1f49919b771687b940d/examples/movement/physics_in_fixed_timestep.rs), and [`crates/bevy_render/src/pipelined_rendering.rs`](https://github.com/bevyengine/bevy/blob/70a1fb4fddc57972c722d1f49919b771687b940d/crates/bevy_render/src/pipelined_rendering.rs) | Keep physical previous/current positions separate from rendered transforms. Use fixed-clock overstep for interpolation and a capacity-one channel when pipelining simulation and rendering so stale work cannot queue without bound. | [MIT or Apache-2.0](https://github.com/bevyengine/bevy/tree/70a1fb4fddc57972c722d1f49919b771687b940d#license) |
| raylib | [`src/rcore.c`](https://github.com/raysan5/raylib/blob/af3045377a4faf2d8a5125feed88cdd3f375f1ca/src/rcore.c) | Buffer swap, frame waiting and measured frame-time averaging belong to presentation. This is useful as a minimal pacing reference, but its single-loop design is not appropriate for trainer control ownership. | [zlib/libpng](https://github.com/raysan5/raylib/blob/af3045377a4faf2d8a5125feed88cdd3f375f1ca/LICENSE) |
| Javascript Racer | [`v4.final.html`](https://github.com/jakesgordon/javascript-racer/blob/master/v4.final.html) | Use one longitudinal position for update and projection, process a bounded segment window, and clip road behind visible crests. These rules are now implemented by the canonical road projection and crest clipper. The implementation is a reference for algorithms only; its third-party art and audio are not used. | [MIT for code](https://github.com/jakesgordon/javascript-racer/blob/master/LICENSE) |
| TrackGenerator | [`TrackGenerator`](https://github.com/rob1997/TrackGenerator) | A sampled center spline with shared entry and exit tangents is a useful model for joining generated course pieces without grade or heading discontinuities. | [MIT](https://github.com/rob1997/TrackGenerator/blob/master/LICENSE) |
| Redrock | [`redrock`](https://github.com/StarKnightt/redrock) | A common sampled surface for visible geometry and collision avoids independent road truths. Only the architectural idea is used. | [ISC](https://github.com/StarKnightt/redrock/blob/master/LICENSE) |

Workout Game therefore keeps exactly one low-priority simulation owner, a
one-frame latest-value publication slot, two render snapshots, and Qt's own
scene graph buffering. Triple buffering is justified only if profiling shows
that Qt synchronization blocks snapshot extraction; it is not a default cure
for clock jitter. Feature meshes and collision proxies remain immutable course
data and do not participate in frame-clock ownership.

### Migration And Acceptance

The thread model is introduced in measured stages rather than changing trainer,
simulation, and rendering ownership at once:

1. **Implemented:** use one workout-time-to-road timeline and instrument frame
   time, world position, regressions, stationary frames, and section state.
2. **Implemented:** extract the monotonic clock and fixed-step simulation runner
   with tests for stalls, pause, resume, reset, and bounded catch-up.
3. **Implemented:** publish complete previous/current snapshots and interpolate
   presentation from only that pair.
4. **Implemented:** run timestamped inputs through `WorkoutGameReplayHarness`,
   reject invalid or regressing streams, hash every complete engine frame, and
   compare repeated pass, bypass, pause/resume, and input-mutation replays.
5. **Implemented for pre-release gating:** trace realized frame intervals,
   cumulative stalls, skipped simulation ticks, forward road progress, telemetry,
   virtual gear, and workout target power. The packaged UI gate explicitly loads
   a prepared ERG workout, connects the data generator, proves that two running
   frames differ, and validates the trace. It runs once through the QPainter
   fallback and once through the Scene Graph path against separate temporary
   athlete libraries. Production sessions are still needed before moving course
   or physics phases to more workers or adding adaptive GPU effects.

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
   seeds, simulation stepping, scoring, pause recovery, fixed-step catch-up, and
   complete replay hashes.
2. Qt tests cover `ErgFile` adaptation, lifecycle signals, renderer fallback,
   and perspective/full-screen state.
3. Current image tests assert nonblank, changing, and feature-distinct output in
   OpenGL/Qt Scene Graph and QPainter. Pinned golden-master comparisons remain a
   release-test enhancement.
4. AppImage smoke tests must verify `xcb`; bundled `offscreen` becomes a required
   second path only after the package includes and supports that Qt plugin.
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
