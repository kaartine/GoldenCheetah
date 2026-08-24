# Workout Game Legacy/3D A/B Review

## Verdict

The Qt Quick 3D version is the correct rendering foundation, but the reviewed
build is a renderer prototype rather than a release candidate. It fixes the
legacy renderer's most serious structural failures: trail and objects share a
real world coordinate system, depth ordering is stable, opaque surfaces no
longer expose the background, and presentation is smooth. The accepted trail
scale is also substantially better: a normal half-width of 0.68 m gives the
rider a credible singletrack corridor. That width is now a regression-tested
invariant and must be preserved.

Almost none of the legacy version's arcade readability or authored MTB content
has moved to 3D yet. Most features are absent or represented by one primitive,
the rider has little visible animation, the environment is flat, and the cue
UI does not explain when or how to complete a feature. The new renderer should
replace the legacy implementation only after the acceptance gates below pass.

## Review Method

The comparison used commit `2a616fd` with the same deterministic course,
telemetry, camera-distance target, 1280 by 720 catalog viewport, and feature
outcome in both renderers.

- Eleven legacy and eleven Qt Quick 3D feature images were captured ten metres
  before the physical obstacle.
- Forty-second application sessions were captured at 1600 by 900 and 30 FPS
  with isolated test athlete data and the Data Generator device.
- A side-by-side video and a complete contact sheet were generated.
- The deterministic engine, feature runtime, power profile, road geometry, and
  renderer tests were run in clean remote Docker build directories.
- The application sessions did not use or modify production athlete data.

Persistent review artifacts are in:

`/home/jkaartinen/Documents/personal/gc-workout-game-ab-2a616fd`

The directory contains `feature-contact-sheet.png`, `comparison-session.mp4`,
per-feature image pairs, and both source session videos.

The application session in that artifact uses the five-feature lab included in
the reviewed AppImage. The updated source now provides a 72-second audit course
containing all eleven features. Static catalogs already cover all eleven.

## Preserve These Properties

1. Keep the normal trail half-width at 0.68 m and preserve its current scale
   relative to the rider and bicycle.
2. Keep the road sample as the one authority for trail, feature, rider, and
   camera placement.
3. Keep the deterministic fixed-step simulation and newest-frame publication;
   trainer control and recording must never wait for rendering.
4. Keep Qt Quick 3D/GPU rendering and the absence of depth, clipping, and
   transparency artifacts.
5. Keep features as semantic course tiles with matching entry and exit sockets.
   Visual GLB assets must not become workout or physics authorities.
6. Keep rivals absent until a credible rider, terrain, and feature pass exist.

## Feature Comparison

| Feature | Legacy | Qt Quick 3D | Required 3D correction |
| --- | --- | --- | --- |
| Roots | Recognisable branched roots, although pasted onto the trail. | One thin transverse cylinder; reads as a bar. | Use a buried root network integrated into the tread and forest floor, with low relief and an optional hop line. |
| Rollers | Repeated raised forms are visible. | No distinct prop and the trail looks almost flat. | Author two or three rounded trail-surface crests and troughs. Rider and camera must follow the same continuous surface. |
| Climb | HUD and perspective make the effort understandable, though the road is visually weak. | Looks like ordinary level trail; there is no useful elevation reference. | Exaggerate visible rise, add a crest and side terrain, and show grade in the HUD without changing trainer targets. |
| Rock garden | A group of varied rocks is clearly an obstacle. | One small oval sphere. | Create a group of sunk, overlapping, irregular rocks and deform the rideable surface around them. |
| Bunny hop | A clear obstacle cluster. | A thin dark bar, visually interchangeable with roots/log. | Give it a compact hurdle or kicker, clear take-off point, clean landing, and a short action cue. |
| Drop | The old geometry is imperfect but visibly changes the trail. | Only a subtle trail notch; the ledge, face, air gap, and landing are unreadable. | Build a level approach, sharp lip, exposed face, lower landing transition, and a separate continuous bypass. Preserve negative drop motion instead of clamping it away. |
| Skinny | A raised narrow line is visible. | A small floating-looking cuboid; entry and exit do not explain the line. | Narrow the actual rideable tile, add deck boards/supports and ground clearance, and join it seamlessly to full-width sockets. |
| Berm | A banked curved wall is visible. | An ordinary bend with no readable bank. | Use a broad bowl-shaped bank whose mesh, centre line, lateral rider path, and roll all share the same curve. |
| Log over | One of the clearest legacy obstacles. | A correctly aligned but very thin transverse cylinder. | Increase volume, extend it beyond both edges, partly bury it, and add silhouette/material detail. |
| Tabletop | The jump silhouette and trail change are readable. | Best 3D feature because the trail mesh contains the shape, but lip, deck, knuckle, and landing remain subtle. | Retain the physical trail implementation and expose each named jump part through geometry, material, and side-terrain relief. |
| Rock slab | A large stone riding surface is visible. | One oval sphere, indistinguishable from the rock garden. | Author one asymmetric slab with exposed sides, rollover crest, fissures, and an irregular trail-integrated footprint. |

The current QML has explicit primitive props only for roots/bunny-hop/log,
rock-garden/rock-slab, and skinny. Rollers, climb, drop, berm, and tabletop rely
entirely on road geometry. This explains why several catalog images contain no
feature-specific visual object.

## Graphics And Art Direction

The old renderer has the stronger game identity. Its layered forest, distant
mountains, high-contrast rider, and pixel-style silhouettes communicate an
arcade game immediately. Its failures are technical: intersecting layers,
floating or buried props, abrupt pop-in, and surfaces that can reveal the
background.

The 3D renderer solves those technical failures but currently resembles an
engineering visualizer. Trees are cone/cylinder pairs, the ground and sky are
flat colors, props are built-in primitives, lighting has no shadows, and there
are no textures or authored silhouettes. Port the old mood rather than its
projection technique: use a low-poly/pixel-textured forest palette, controlled
color ramps, fogged depth layers, and a small modular GLB asset set. Avoid
photorealism. The path must remain the visual focus and retain its current
width.

Live video also shows foreground trees covering a large part of the viewport.
Tree placement needs a camera corridor plus distance-based fade or substitution
so scenery can frame the trail but never hide the rider, action cue, or next
feature.

The floor currently streams 15 m behind and 130 m ahead while tree placement
updates in buckets. This is bounded and efficient, but transitions need visual
validation to prevent late tree pop and ground-chunk changes. Authored terrain
tiles should replace the flat ribbon without adding per-frame mesh rebuilds.

## Physics And Motion

The old and new views consume the same deterministic engine and road course,
so renderer A/B differences are not evidence of different trainer physics.
Automated replay is finite, deterministic, and forward-only. Completed log and
tabletop actions produce measurable airborne arcs, bypasses do not jump, and
tabletop flight increases with speed within bounds.

Presentation still has important defects:

- The 3D view takes the maximum of Box2D air height and scripted feature air.
  Two vertical authorities can create a discontinuity unless their ownership
  is made explicit per action.
- Negative scripted vertical offset is clamped to zero, so a drop cannot show
  the intended downward movement directly.
- Only jump, absorb, and drop have meaningful motion policies. Berm, skinny,
  climb, rollers, and rough surfaces need feature-specific rider pose and
  suspension/camera response.
- Wheel nodes rotate, but cylindrical symmetry makes the motion nearly
  invisible. The primitive rider has no articulated legs, cranks, steering,
  suspension, take-off compression, landing compression, or recovery pose.
- A fixed offset chase camera ignores the existing dynamic camera state. That
  is acceptable for the first release, but it still needs restrained take-off,
  landing, crest, and berm reactions after geometry is stable.

Speed and virtual gearing are shared simulation concerns, not 3D renderer
work. Their separate acceptance test must prove that low gear is slow on flat
and climb, and that a gear change changes acceleration/torque progressively
instead of teleporting speed.

## HUD And Gameplay Feel

The top telemetry strip is compact and readable, and actual FPS, elapsed time,
distance, watts, target, cadence, heart rate, speed, and gear are present.
Compared with the requested training view it is still missing the workout
power profile and grade.

The bottom cue is not sufficient for riding:

- `Match target for <feature> N%` combines power and cadence into an unexplained
  percentage.
- It does not show distance to the decision point or separate power and cadence
  readiness.
- It cannot distinguish prepare, act now, committed, completed, and bypassed
  strongly enough at a glance.
- The live Data Generator session produced about 110 W while the feature lab
  expected 210 W. The rider therefore took the safe line even though the test
  was intended to demonstrate a successful jump. Test telemetry and game
  target selection need one authority.

Replace the cue with a compact distance countdown, separate power/cadence
state, a short high-contrast action window, and immediate success/bypass
feedback. Add the workout power profile with a current-position cursor. This
is training instrumentation first and game decoration second.

The 3D version currently feels smoother and spatially coherent, but less like
a game than the legacy renderer. It has no sound, impact response, particles,
reward feedback, camera punctuation, or authored feature anticipation. Those
effects should follow correct geometry and reliable decisions, not conceal
them.

## Prioritized Work

### P0: Release blockers

1. Define and validate the modular tile/GLB socket contract, then build one
   seamless ordinary trail tile and one complete tabletop tile as the vertical
   slice.
2. Replace every primitive placeholder with a recognisable, trail-integrated
   mesh for all eleven features; keep deterministic catalog images.
3. Replace the primitive rider with a correctly oriented, articulated low-poly
   GLB and animation states for pedal, compress, air, land, absorb, lean, and
   safe bypass.
4. Fix drop vertical ownership and make one physics source authoritative for
   each airborne action.
5. Redesign feature guidance and add grade plus workout power profile.
6. Make the Data Generator follow the active game target and add deterministic
   completed/bypassed UI captures for every feature.
7. Add diagnostics and release tests for backward distance, stalls, actual
   presented FPS, simulation skipped ticks, prop pop-in, and tile joins.

### P1: Content and feel

1. Add the low-poly/pixel-textured forest art kit, terrain relief, material
   variation, fog/depth treatment, and conservative low-end lighting.
2. Add short feature-specific camera and suspension reactions, landing impact,
   dust/debris, and clear success feedback.
3. Validate progressive virtual gearing and speed response with generated and
   real trainer input.
4. Test the 72-second all-feature course on the target laptop and record a
   successful and bypassed pass without production athlete data.

### P2: Later additions

Dynamic camera transitions, tailwhip variants, richer audio/particles, rivals,
and route-specific decorative sets belong here. They must not precede readable
features, stable recording, or trainer-control validation.

## Release Acceptance Gates

The 3D renderer is ready for a user release only when:

1. All eleven catalog images identify the intended feature without HUD text.
2. Every tile joins ordinary trail without cracks, floating geometry, buried
   props, or scale changes.
3. Completed and bypassed live runs are visibly different and agree with the
   recorded outcome for every feature.
4. Rider wheels stay on the sampled surface except during an intentional,
   bounded jump or drop; no teleporting is visible.
5. The target laptop completes the test route without backward movement,
   unexplained pauses, trainer-control delay, recording gaps, or unbounded
   frame work.
6. The workout profile, grade, power, cadence, heart rate, target, gear, speed,
   time, and feature action state remain readable during riding.
7. AppImage and native tests cover startup, save/cancel/continue, renderer
   selection, fallback, asset packaging, and an isolated-data UI session.
