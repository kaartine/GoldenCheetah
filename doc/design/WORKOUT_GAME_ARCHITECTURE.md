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

The simulation is a pure C++ fixed-step state machine. Planned update frequency
is 20 Hz. Inputs are immutable snapshots containing workout time, power,
cadence, target power, heart rate, and virtual gear. Outputs contain rider pose,
camera progress, section state, feature outcome, and score state.

The simulation uses workout time for longitudinal course progress. Measured
power changes animation speed, line choice, and scoring, but cannot move the
workout timeline. Long pauses reset transient jump and streak state without
discarding accumulated score.

### WorkoutGameSession

This Qt-facing coordinator owns the course and simulation. It subscribes to
existing `Context` workout and telemetry signals and publishes immutable render
snapshots. It does not own a trainer controller and does not block on rendering.

### WorkoutGameWindow

The game is registered as a Train `GcChartWindow`, allowing it to coexist with
current perspectives and switch to full-screen through the existing chart
system. Entering or leaving full-screen does not restart the session.

The window selects one renderer at construction:

- `WorkoutGameOpenGLRenderer`: sprite-atlas rendering in a `QOpenGLWidget`.
  It batches terrain tiles and effects and avoids per-frame texture uploads.
- `WorkoutGamePainterRenderer`: QPainter fallback using the same render
  snapshot and asset atlas, with fewer layers and effects.

Renderer selection is capability-based. Runtime rendering errors permanently
fall back for the current session instead of repeatedly recreating a GPU
context.

## Scheduling And Performance

- Trainer command cadence: owned by existing Train code and highest priority.
- Recording cadence: owned by existing Train code and independent of the game.
- Telemetry ingestion: existing cadence, copied into a small latest-value
  snapshot without waiting for rendering.
- Simulation: fixed 50 ms steps, capped catch-up work after UI stalls.
- Rendering: target 60 FPS on GPU and 30 FPS with QPainter.
- Frame budget: 8 ms warning threshold. Repeated overruns reduce particles,
  parallax layers, and then frame rate.

There is no unbounded event queue. Simulation and rendering use latest-value
snapshots, so stale frames are overwritten rather than accumulated.

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

## Determinism And Persistence

Course seeds use a specified integer hash over normalized interval values. They
must not use platform-dependent `std::hash`. Simulation uses a specified small
pseudo-random generator only for visual variants.

The first release stores game results as activity metadata after recording has
completed. It does not change the CSV schema beyond already recorded virtual
gear data. A future version may persist a compact replay stream for ghost riders.

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

## Delivery Stages

1. Deterministic course builder and fixed-step simulation.
2. Train chart integration with QPainter fallback.
3. OpenGL sprite renderer and automatic quality degradation.
4. Scoring, feature bypasses, and deterministic AI riders.
5. Recorded-activity ghost rider and optional external route adapters.
