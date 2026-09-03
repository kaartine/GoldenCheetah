# Workout Game 3D Live-Test Backlog

## Purpose

This is the authoritative list of defects and improvement requests found while
riding development AppImages. An item is closed only after its code change,
focused regression test, rendered evidence and interactive test have all been
recorded. The release checklist remains the release gate; this file preserves
the more detailed rider observations that lead to those gates.

Detailed art direction, visual defects and the environment-element inventory
are maintained in `WORKOUT_GAME_3D_VISUAL_BACKLOG.md`.

Status values are `open`, `in progress`, `verified` and `deferred`.

## Route And Terrain

| ID | Status | Observation | Acceptance |
| --- | --- | --- | --- |
| LIVE-ROUTE-01 | in progress | Converted workouts still contain far too much straight trail, so normal riding remains visually monotonous even after the first route-density pass. | A deterministic route-quality audit limits an ordinary near-straight run to 25 m. Every rolling 100 m of ordinary trail contains at least three deliberate bends in alternating directions and at least 45 degrees of accumulated absolute heading change, including gentle, medium and sharp rideable turns and selected turns approaching 90 degrees. Only a canonical feature approach, action or landing safety zone may exceed the straight-run limit, and that exception remains as short as the feature contract requires. Camera yaw remains bounded across every generated transition. |
| LIVE-ROUTE-02 | in progress | Berms are not visibly present often enough, especially in turns that can carry speed, and must behave as ordinary trail bends instead of advertised features. | Route generation places clearly visible banked terrain in a representative share of medium, sharp and high-speed turns. Berms produce no feature prompt; their radius and banking suit the predicted entry speed, and power relative to target continuously moves and leans the rider around the berm centreline without an abrupt camera turn. The short deterministic review route must contain multiple berms with varied radius and banking. |
| LIVE-ROUTE-03 | open | Regenerating or reopening a workout must not unexpectedly change its route. | Route generation is deterministic from persisted course data and carries an explicit generation version; save/reopen produces identical connectors and turn plan. |
| LIVE-ROUTE-04 | open | Ordinary trail lacks visible humps, climbs, descents and forest-floor relief. | Trail and surrounding terrain visibly undulate while trainer grade and recorded workout targets remain authoritative. |
| LIVE-ROUTE-05 | open | Terrain and surface-type transitions can look abrupt. | Adjacent tiles preserve socket position, heading, grade, width and material transition without a visible step or pop. |
| LIVE-ROUTE-06 | open | A converted or re-saved course such as `test3` can still feel like mostly straight road. | Conversion performs the same deterministic 25 m/100 m curvature audit before persistence and rejects or regenerates a route without enough alternating bends, switchbacks, rollers and elevation variation for its length. Save/reopen preserves the accepted turn plan exactly. |

## Rider, Bicycle And Camera

| ID | Status | Observation | Acceptance |
| --- | --- | --- | --- |
| LIVE-RIDER-01 | in progress | Pedals appear stationary while the rider's legs pedal. | Two visible crank arms and pedal platforms use the same crank phase and contact coordinates as the feet; a rendered sequence makes one full revolution unambiguous. |
| LIVE-RIDER-02 | in progress | The rider disappeared during a live ride and only the rear wheel remained visible. A rendered 7.9% climb also exposed the front wheel sinking halfway through the trail. | Physics pitch is converted to the renderer coordinate convention, extreme visual pitch is bounded, a deterministic full-route camera/frustum audit keeps the complete rider and both wheels in frame, and an interactive replay contains no partial disappearance. |
| LIVE-RIDER-03 | in progress | Rear chase view makes the bicycle and rider difficult to assess. | Start and sustained-idle presentation holds a stable side view long enough to inspect the complete silhouette, then returns smoothly without changing simulation state. |
| LIVE-RIDER-04 | open | Rider and bike need more natural secondary motion. | Pedalling sway, suspension, standing effort, lean and landing motion are bounded, readable and driven by authoritative state rather than render timing. |
| LIVE-RIDER-05 | open | Sharp curves must not produce abrupt camera yaw or motion sickness. | Camera follows route curvature with bounded angular velocity and acceleration while keeping the rider and upcoming line readable. |
| LIVE-RIDER-06 | open | Gear changes still need realistic speed response. | A shift changes torque/cadence response progressively; speed does not jump discontinuously on flat ground or climbs. |
| LIVE-RIDER-07 | open | The bicycle, rider, helmet and tyres need a coherent authored silhouette that reads as a modern full-suspension enduro MTB while remaining legally and visibly distinct from Pole Voima and a real person. | Side-review renders verify suspension layout, wheelbase, black high-volume tyres, helmet, rider proportions and an original frame/body treatment against the approved art brief and provenance record. |
| LIVE-RIDER-08 | open | The bicycle has appeared to travel sideways, float above terrain or lose contact during jumps and slopes. | Wheel contacts, frame pitch, steering yaw and rider root all follow the same authoritative road sample and continuous feature trajectory in trace and rendered motion tests. |

## Forest And Visual World

| ID | Status | Observation | Acceptance |
| --- | --- | --- | --- |
| LIVE-WORLD-01 | in progress | The forest is dominated by repeated trees and can feel like an open field. | Deterministic rocks, stumps, shrubs, fallen wood and other forest-floor debris add near/mid-ground variety without hiding the trail. |
| LIVE-WORLD-02 | open | Trees and props have previously popped in, floated, intersected, or exposed underground parts. | Props are anchored to sampled terrain, preserve trail/camera clearance, use stable world slots and enter/leave through a bounded visibility band. |
| LIVE-WORLD-03 | open | Feature meshes can look pasted onto the trail or intersect adjacent geometry. | Every physical feature shares the trail socket contract and surrounding terrain closes around it without gaps, z-fighting or clipping. |
| LIVE-WORLD-04 | open | Some feature silhouettes remain difficult to identify. | Each feature is recognisable without HUD text in the fixed camera catalog and live approach sequence. |
| LIVE-WORLD-05 | open | Forest density and additional detail must not harm the old laptop. | Added dressing is batched or instanced, deterministic, outside physics, and remains within tested triangle, frame-time and chunk-build budgets. |
| LIVE-WORLD-06 | verified | The X11 render gate required at most 10 detailed trees while the forest-density gate required at least 12 and production allowed 18. | The gate uses the production 18-tree cap and an 80-draw-call ceiling that includes two draws per detailed trunk-and-crown tree; the baseline contradiction is reproduced and the corrected GPU test passes. |
| LIVE-WORLD-07 | open | The scene still needs a denser, more varied Finnish-forest feel instead of an open field. | Rocks, stumps, shrubs, roots, fallen timber and undergrowth vary by stable world slot and depth layer; their scale and occlusion integrate with the terrain instead of looking pasted on. |

## Gameplay And Verification

| ID | Status | Observation | Acceptance |
| --- | --- | --- | --- |
| LIVE-GAME-01 | open | Power/cadence preparation and feature commitment have felt unreliable. | HUD separately explains distance, target power, cadence readiness and committed result, using the same target authority as the Data Generator and trainer. |
| LIVE-GAME-02 | open | Jump, drop, bypass and other actions have at times looked like teleports or been unreadable. | Recorded position is forward-only and each action has a continuous, bounded motion trace matching its trail geometry. |
| LIVE-GAME-03 | open | A test workout must expose all features and useful route shapes quickly. | A short deterministic lab route contains every scored feature plus ordinary banked turns, humps, climbs and descents. |
| LIVE-GAME-04 | open | Visual regressions have escaped launch-only smoke tests. | Pre-release automation records trace counters, stills and video, checks world progress/framing, and runs an interactive isolated-data UI ride without touching production athlete data. |
| LIVE-GAME-05 | open | Testing without a trainer must not require hidden environment knowledge. | A visible, isolated Data Generator follows the workout with controlled over/under-target variation, responds to virtual gears and cannot write production athlete data. |
| LIVE-GAME-06 | open | Earlier builds made workout saving, virtual-gear controls and entry to the Game view difficult to discover or unreliable. | An isolated UI test creates and saves a workout, starts it in Game mode, changes gears through visible controls and keyboard bindings, stops/continues, saves the activity and reopens it. |

## Evidence Log

- 2026-09-01 live production-data inspection used the already-running personal
  AppImage without changing athlete data. The captured view confirmed a mostly
  rear-facing rider, a long near-straight trail and sparse environmental
  variety. The user separately observed temporary rider disappearance and
  stationary-looking pedals; both remain open until reproducible evidence and
  a verified fix exist.
- 2026-09-02 X11/OpenGL captures used an isolated copy of the production MTB
  course. Four crank phases show that the feet, crank arms and pedal platforms
  now share one phase. The course captures show deterministic rocks, stumps
  and shrubs in the existing forest batch. These remain `in progress` pending
  interactive AppImage verification.
- The same capture exposed a renderer-coordinate defect on a 7.9% climb:
  positive Box2D pitch rotated the Quick 3D bicycle nose down, burying half of
  the front wheel. The QML render boundary now reverses that sign while leaving
  the physics snapshot unchanged. A repeated capture at 7.7% keeps both wheels
  above the trail and a focused QML regression test passes.
- The `e85665a` baseline reproduces the old X11 render-budget failure before
  current changes: its production 18-tree cap conflicts with a test expecting
  at most 10, while another test requires at least 12. The corrected contract
  allows all 18 detailed trees and at most 80 draw calls. New forest dressing
  stays in one batch and pedal platforms stay inside the existing crank draw.
- Final scoped regression on 2026-09-02 passed 49 road-course, 32 geometry,
  24 feature-runtime, 8 distance-playback, 10 feature-lab, 20 asset-policy and
  76 real X11/Quick 3D tests. The target-GPU gate measured 149.0 FPS, 7.80 ms
  p95 and 8.10 ms p99 frame time, no skipped simulation ticks and no trainer or recording
  deadline misses. Two world-physics tolerances still fail with exactly the
  same values on `e85665a`: climb vertical step `0.0832901 m` at 7 m/s and
  safe-line rock-garden suspension `0.0850116` versus main-line `0.203219`.
  They are recorded as pre-existing failures rather than hidden by weaker
  physics assertions.
- Independent review then exercised a 20 km persisted smooth section and the
  downstream extents of roots, rock gardens, rock slabs and skinnies. The
  per-section safety cap now allows up to 4096 bounded road pieces, and forest
  dressing excludes each feature's full canonical geometry plus two metres.
  The configured-course audit also projects the top, bottom, front and back of
  both wheel rims on every captured frame instead of accepting jersey pixels
  alone.
