# Workout Game Feature Reference Audit

This audit compares the workout game's eleven generated trail features with
the engineering diagrams and photographs in the Recreation Aotearoa New
Zealand Mountain Bike Trail Design Guidelines. The guideline is an engineering
reference, not a complete art reference: it gives direct visual guidance for
drops, rollers, jumps, berms, structures, and split lines, but not for every
natural trail feature.

Reference:

- Recreation Aotearoa, *New Zealand Mountain Bike Trail Design Guidelines*,
  pages 25, 30, 35-40, 48, and 70:
  <https://sportnz.org.nz/media/3j3pmk0y/new-zealand-mountain-bike-trail-design-guidelines.pdf>
- OpenStax, *University Physics Volume 1*, section 6.3, supplies the standard
  centripetal-force relation used for the speed/curvature rider-lean model:
  <https://openstax.org/books/university-physics-volume-1/pages/6-3-centripetal-force>

## Reproducible Catalog

`TestWorkoutGameSceneGraph::exportsEveryFeatureAtAConsistentViewpoint`
renders every feature at 1280 by 720, ten metres before the obstacle. Set
`GC_WORKOUT_GAME_FEATURE_CATALOG_DIR` to retain the PNG files. The test uses
the production road builder, feature runtime, mesh projection, and scene graph
renderer.

The catalog is deliberately rendered without final textures. This isolates
silhouette, scale, trail integration, and line readability from future art.

## Feature Findings

| Feature | NZ reference | Current assessment | Required correction |
| --- | --- | --- | --- |
| Roots | Track armouring, p. 35; obstacle limits in grading tables | The production tile now embeds eight irregular branches into one widened tread. Five branches cross the main line, the low-relief safe line stays on the same trail, and physics suspension follows the rendered crowns. | Preserve the canonical profile, partial burial and exact sockets when final bark and ground materials replace the current vertex colors. |
| Rollers | Roller profile and spacing, p. 37 | The continuous strip is a sound base, but the shallow, flat-topped bands resemble a boardwalk more than rounded pump rollers. | Use two or three clearly rounded crests and troughs. Keep spacing near the guideline's ten-times-height rule. |
| Climb | Gradient design, p. 25 | Edge rocks are visible, but the mesh does not communicate a climb; the road grade carries all of the meaning. | Give the trail a long visible rising face, transverse rock steps, and a readable crest. |
| Rock garden | Track armouring, p. 35 | The production tile now sinks twelve irregular, rotated stones into one widened tread. Nine stones cross the main line, the low-relief safe line stays on the same trail, and physics suspension follows the same crowns rendered by Quick 3D. | Preserve the canonical profile, partial burial, opaque merged mesh and exact sockets when final rock and ground materials replace the current vertex colors. |
| Bunny hop | Jump anatomy, p. 38 | A separate project-authored practice hurdle now spans the ordinary trail without raising its ground surface. It has a short preload, feature-specific bounded lift and a grounded safe branch. | Preserve the accepted socket, clearance and timing contract when final pixel materials and rider preload/landing clips replace the current low-poly presentation. |
| Drop | Drop sections, p. 36 | A sharp level lip now opens into a real physics and render gap, an exposed faceted face, a lower landing and smooth recovery. The separate safe line stays on ordinary ground. | Preserve the shared gap, collision, socket and bypass contract when final materials replace the approved low-poly face. |
| Skinny | Boardwalk construction, p. 48 | Width and raised line are credible, but the uninterrupted brown strip resembles a narrow road. | Add deck boards, beams, supports, visible ground below, and aligned entry and exit transitions. |
| Berm | Berm cross-section, p. 30 | The production tile now has a 75-degree curved centreline, broad banked bowl, level sockets and an integrated slower inside line. Road, rider, mesh and camera share the same distance profile. | Preserve the canonical profile and socket contract when final ground materials replace the current low-poly shading. |
| Log over | Drop/obstacle examples, p. 36 | The tapered round log is one of the clearest current features. | Extend it beyond the trail, partly bury it, and add end-grain and broken branch details. |
| Tabletop | Jump anatomy and examples, pp. 38-40 | The main proportions are plausible, but the current solid mound hides the take-off belly, lip, knuckle, and landing transition. | Preserve the measured dimensions while exposing those named parts in the silhouette and shading. |
| Rock slab | Track armouring, p. 35 | The production tile now forms one asymmetric rollover face with an irregular footprint, exposed buried sides and fissures. The low-relief safe line stays on the widened tread and physics follows the same surface rendered by Quick 3D. | Preserve the canonical rounded crest, same-tread safe line and exact sockets when final rock and ground materials replace the current vertex colors. |

## Numeric Checks

At catalog difficulty 0.65, the current tabletop is approximately 1.01 m
high. Its 3.3 m take-off is about 3.3 times the height and its 2.6 m deck is
inside the guideline ranges for an intermediate tabletop. Its dimensions are
therefore a reasonable baseline; visual anatomy and trail integration are the
larger problems.

The current 7.2 m roller strip reaches only about 0.26 m. Its height is
credible for an easy roller, but the sampled profile and material bands need
to read as smooth ground rather than separate planks.

The production berm turns 75 degrees over a 5.24 m active centreline with
1.25 m level transitions at each end. Difficulty scales bank from 20 to 30
degrees. The maximum banked tread width is 1.90 m and the slower inside line
moves no more than 0.45 m from centre. These dimensions follow the Grade 2-3
range in the reference while keeping the feature readable in the fixed chase
camera.

The production root tile is 12 m long with a 4 m active bed. Its eight
branching roots are 0.026-0.135 m in radius after difficulty scaling and are
partly buried. The main line reaches at least 0.07 m of relief at catalog
difficulty, while the 0.82 m lateral safe line remains below one quarter of
that relief and reconnects before each 1.36 m-wide socket.

The production rock slab tile is 14 m long with a 7.4 m active face. At
difficulty 0.65 it rises approximately 0.84 m to a single rounded crest. Its
asymmetric footprint varies from 1.44 to 1.76 m wide; the 1.05 m lateral safe
line has at most 2.5 cm of relief and reconnects to each 1.36 m-wide socket.

## Airborne Presentation

The former renderer mixed two vertical scales: the road used perspective while
the rider used a fixed viewport multiplier. A roughly 0.27 m physics jump could
therefore move the rider only about 14 pixels at 720p. It also used an ordinary
pedalling sprite with no ground-fixed shadow, so there was no reliable depth
cue.

`WorkoutGameRiderVisual` now derives the near-field pixels-per-metre scale from
the rendered rider height, keeps a shadow on the trail, and changes shadow size
and opacity through the flight. Screen-space rotation uses roll only; jump
pitch no longer appears as a sideways rotation in the chase camera.

The remaining gameplay issue is independent of rendering: a jump that is
classified as a safe bypass never starts the scripted flight. Power-decision
semantics and their on-screen indication should be revised as a separate,
test-driven change.

## Implementation Priority

1. Align berm geometry with the rider's actual route.
2. Expose tabletop anatomy and round the roller profiles.
3. Integrate climb, roots, rocks, and slab into the trail surface.
4. Show both main and bypass lines before the decision point, following the
   split-line guidance on page 70.
5. Add final materials and feature-specific camera treatment only after the
   geometry and route contracts are stable.
