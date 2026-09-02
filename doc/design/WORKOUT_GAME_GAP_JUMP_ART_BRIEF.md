# Workout Game Three-Line Gap Jump Art Brief

## Purpose

Create one unmistakable bike-park gap-jump feature with three adjacent lines.
The rider is guided to the gap length that matches predicted approach speed,
but all three choices remain visible early enough to understand the feature.
The feature must translate the legacy 2.5D view's playful pixel-arcade identity
into coherent low-poly 3D without looking like three blocks placed on a road.

This brief defines the visual asset and review target. Authoritative route
selection, trainer control, scoring and flight physics remain engineering
contracts. The final geometry must be generated from the same line, surface
and collision data used by those systems.

## References And Constraints

- Follow `WORKOUT_GAME_3D_ART_BRIEF.md` for palette, texture, camera and scene
  budgets.
- Follow `WORKOUT_GAME_3D_VISUAL_BACKLOG.md`, especially VIS-01, VIS-06,
  VIS-07, VIS-08, VIS-09, VIS-12 and VIS-14.
- Use the jump anatomy and sight-line principles in Recreation Aotearoa's
  *New Zealand Mountain Bike Trail Design Guidelines*, pp. 37-41:
  <https://sportnz.org.nz/media/3j3pmk0y/new-zealand-mountain-bike-trail-design-guidelines.pdf>.
- Preserve the production trail width of 1.36 m at `SOCKET_IN` and
  `SOCKET_OUT`.
- Use the current bicycle scale as the visual ruler: 0.3775 m wheel radius and
  1.313 m wheelbase.
- Keep normal airborne time compact. The intended visual range is about
  0.45-1.0 s, never a prolonged float.
- The feature is a stylized game representation, not construction guidance
  for a physical trail.

The guideline is an anatomy and risk-awareness reference. It establishes a
clear take-off, open gap, case pad, visible landing transition, adequate
sight-line and speed-matched jump length. The artwork must not reproduce a
specific photographed jump or drawing.

## Art Direction

### Core Read

From 30-40 m away the player must read, in this order:

1. One broad sculpted earthwork rises out of the forest trail.
2. The approach fans into three distinct take-off lips.
3. Three open gaps become progressively longer from left to right.
4. Three landings step farther away and become broader and more substantial.
5. The chosen line is clear without hiding the other two.
6. All lines merge back into the same narrow singletrack.

The silhouette must communicate a gap jump with the HUD hidden. No take-off,
gap edge, case pad or landing may use a cuboid as its final visible form.
Longitudinal profiles use a smooth belly, a deliberate lip, an open concave
gap, a rounded knuckle, a sloping landing sweet spot and an eased runout.
Across the trail, side slopes blend into irregular terrain rather than ending
in vertical walls.

### Legacy 2.5D Translation

- Keep large, simple color regions and a dark readable contour where surfaces
  overlap in the chase camera.
- Use faceted curves with intentional silhouettes, not visibly coarse boxes.
- Retain crisp pixel texture at close range with mipmapped minification in the
  distance.
- Exaggerate the lip and landing highlights by roughly 10-15 percent over a
  physically neutral rendering so the anatomy reads at laptop resolution.
- Keep small asymmetries in side banks, rocks and vegetation. Do not distort
  the actual centreline, lip or landing surface used by physics.
- Preserve the rider's strong legacy silhouette and restrained arcade motion.
  The feature should feel authored for that rider, not imported from a
  realistic asset pack.

## Feature Layout

### Shared Approach

- Enter through one level 1.36 m socket with at least 8 m of visually calm,
  rideable setup before the first line separation.
- Build the approach as packed dirt in a shallow forest cut. Raised outer
  banks and low vegetation frame the route while leaving the full fan visible.
- Begin the fan-out 14-18 m before the average lip. This is early enough for a
  choice but late enough that the three lines still read as one feature.
- Widen continuously. Never snap the rider or trail laterally at the decision.
- Place line centres approximately 2.2-2.5 m apart at the lips. The exact
  spacing may grow with difficulty, but tyres, handlebars and landing skirts
  must retain visible clearance.
- Use a shallow uphill or level approach. Avoid a blind crest and avoid a
  steep downhill that makes the selected length visually misleading.
- Break the fan surface with worn tyre paths and low grass or moss wedges.
  These separators remain below pedal height and never resemble barriers.

### Line Order And Names

Use neutral names in art files and review material:

| Position | Working name | Intended read | Initial speed band for visual tuning |
| --- | --- | --- | --- |
| Left | Short | Compact, forgiving, quick return to ground | 4.0-5.2 m/s, 14-19 km/h |
| Centre | Medium | Hero line and default composition | 5.2-6.6 m/s, 19-24 km/h |
| Right | Long | Fast, committed and visibly longer | 6.6-8.0 m/s, 24-29 km/h |

These bands are art-review starting points, not final gameplay thresholds.
The runtime should derive its recommendation from predicted lip speed and the
same trajectory model that chooses a landing. Use hysteresis near boundaries
so the highlighted line does not flicker when power samples vary.

### Shared Cross-Section

- Each ridden tread is 1.15-1.36 m wide through its belly and lip.
- Each landing grows to 1.55-1.90 m at the sweet spot, then narrows smoothly.
- Earthen shoulders extend at least 0.45 m beyond each tread before joining
  the surrounding terrain.
- Keep the line surface subtly cupped, with a worn centre and compacted edges.
- Exposed side faces slope and terrace into the forest floor. Do not show a
  flat underside, paper-thin edge or vertical extrusion.
- Give each gap a shallow, debris-free concavity and a broad case pad near the
  landing side. It must read as earth, not as a bottomless trench.

## Line Designs

Dimensions are target proportions for the first concept pass. Engineering may
adjust them after trajectory tests, but must preserve the hierarchy and visual
anatomy.

### Short Line

**Silhouette**

- Low rounded belly leading to a mostly linear, friendly take-off.
- Clearly open but compact gap.
- Broad rounded knuckle and long low landing transition.
- The complete line should fit in one chase-camera glance and feel fast rather
  than intimidating.

**Scale target**

| Part | Target |
| --- | --- |
| Belly and take-off run | 2.8-3.4 m |
| Lip rise over approach | 0.55-0.70 m |
| Final take-off angle | 14-17 degrees |
| Clear horizontal gap | 1.6-2.0 m |
| Case pad width along line | 0.55-0.80 m |
| Landing sweet spot | 1.8-2.4 m |
| Landing and recovery | 4.5-5.5 m |

The lip is a rounded wedge with a 0.20-0.30 m crest radius. The landing begins
slightly above or level with the take-off datum so it is visible on approach.
Do not turn the short line into a tabletop. Open sky or dark forest-floor
color must remain visible between lip and case pad.

### Medium Line

**Silhouette**

- Strong S-profile: compressed belly, readable linear face, crisp rounded lip,
  open gap and matching landing transition.
- Highest visual contrast and central composition because this is the hero
  line used to judge the whole feature.
- Landing is visibly longer than the short line and its knuckle is farther
  away, not merely a scaled copy.

**Scale target**

| Part | Target |
| --- | --- |
| Belly and take-off run | 3.4-4.2 m |
| Lip rise over approach | 0.75-0.95 m |
| Final take-off angle | 17-21 degrees |
| Clear horizontal gap | 2.8-3.5 m |
| Case pad width along line | 0.70-1.00 m |
| Landing sweet spot | 2.4-3.2 m |
| Landing and recovery | 5.5-6.8 m |

Use three to five broad longitudinal facets through each transition, supported
by denser invisible or subtle surface sampling where needed for smooth tyre
contact. The visible profile must remain curved when seen from 20 m away.

### Long Line

**Silhouette**

- Longer, slightly lower belly followed by the most assertive lip.
- Largest amount of open negative space and the longest visible flight path.
- Wide landing with a long sweet spot and a low, stable runout.
- More substantial side banks and exposed packed-earth faces establish scale,
  but the line must not resemble a dirt-jump tower.

**Scale target**

| Part | Target |
| --- | --- |
| Belly and take-off run | 4.2-5.2 m |
| Lip rise over approach | 0.95-1.20 m |
| Final take-off angle | 20-24 degrees |
| Clear horizontal gap | 4.2-5.0 m |
| Case pad width along line | 0.90-1.20 m |
| Landing sweet spot | 3.0-4.0 m |
| Landing and recovery | 6.5-8.0 m |

The long line's landing knuckle must be visible from the shared approach. A
slight step-up of 0.15-0.30 m is preferred because it improves the sight-line
and keeps the flight compact. Do not use a blind step-down.

## Lips, Gaps And Landings

### Lips

- Model every lip as part of a compacted earth mound with a convex belly and
  rounded upper edge.
- Maintain a stable final tangent for roughly the last 0.35-0.55 m so the
  take-off direction reads clearly.
- Add a narrow lighter packed band on the ridden crown and a darker damp band
  under the side overhang. These are material changes, not floating decals.
- Vary side erosion and tool marks between lines, while keeping the rideable
  surface exact and clean.

### Gaps And Case Pads

- Shape each gap as a shallow excavated bowl between two built earth forms.
- Continue surrounding terrain through the gap at a lower elevation. Never
  leave a void, transparent opening or exposed mesh underside.
- Keep the gap floor visually quiet and darker than both lip and landing.
- Include a broad, low case pad at the base of the landing face. It must soften
  the silhouette without making the gap appear rollable.
- Place only small embedded stones, drainage texture and sparse moss in the
  bowl. No stump, pointed rock or branch belongs on the expected flight path.

### Landings

- Use a rounded knuckle followed by a long planar-to-concave transition.
- Angle the landing surface to agree with the predicted descent path. The bike
  should touch both the art and the physics surface at the same frame.
- Make landing tops wider than lips and subtly brighten the sweet spot.
- Carry tyre wear down the transition. A darker churned patch beyond it gives
  scale and a believable braking/pumping zone.
- Connect side skirts into natural banks with no straight wall and no visible
  seam against the ordinary terrain mesh.

### Merge And Output Socket

- Keep the three lines separate through their complete landing transitions.
- Start convergence only after every line provides at least 4 m of grounded,
  low-gradient recovery.
- Merge over 9-12 m using broad S-curves with bounded curvature. No line may
  cross another or force a sudden sideways correction.
- The shortest line takes the longest longitudinal merge so all routes reach a
  common recovery gate without a teleport or visual pop.
- Finish on one centred 1.36 m `SOCKET_OUT` with matching width, elevation,
  grade, heading, normals, UV phase and material bands.
- Use low mossy islands and two or three embedded stones to make the merge
  readable. Keep them outside tyre and pedal clearance.

## Forest Integration

The feature sits in a dense Finnish-style mixed conifer forest, not an open
field or a separate bike-park arena.

- Set the shared approach in a shallow natural hollow with uneven forest
  ridges on both sides.
- Use mature spruce and pine behind the landings to silhouette their profile.
  Preserve clear sky or pale fog immediately above every knuckle.
- Use birch saplings, bilberry-like shrubs, ferns, heather and moss in small
  asymmetric groups around the fan and merge.
- Partly bury two or three irregular boulders in side banks. Decorative rocks
  stay outside the landing envelope and must not resemble a rock-garden line.
- Add one cut stump, one fallen decaying log and sparse branch debris well
  outside the ride corridor. No element may copy an identifiable real trail.
- Blend forest-floor geometry and texture continuously over the feature skirts.
  Dirt-to-moss boundaries follow drainage and wear rather than rectangular UV
  islands.
- Keep the camera exclusion corridor and all three flight envelopes free of
  trunks, cards and suddenly spawning foreground props.
- Use deterministic placement and terrain anchoring. Every prop base must be
  buried slightly; no tree may float, reveal roots below ground or clip through
  a landing face.

## Palette And Materials

Use the shared generated surface atlas and a compact legacy-derived palette.
Do not add photographic textures.

| Surface | Base direction | Highlight | Shadow/accent |
| --- | --- | --- | --- |
| Packed riding line | warm muted ochre `#915C34` | dry crown `#DCB163` | rut `#5B4127` |
| Built earth sides | desaturated umber `#6B4B31` | cut soil `#967047` | damp base `#49382D` |
| Forest floor | mid forest green `#26553D` | moss `#4B684E` | deep foliage `#1E3E32` |
| Stone accents | cool grey `#696F65` | top plane `#919365` | buried edge `#434944` |
| Wood accents | subdued brown `#684826` | cut edge `#A6602B` | rot `#4E2F1C` |
| Cue accent | warm yellow `#E8C54E` | pale edge `#F6EFD7` | ink `#141B1F` |

- Use opaque, rough, non-metallic materials for all terrain.
- Limit each surface to stepped lighting with two or three authored value
  bands. Preserve enough normal response to reveal curved transitions.
- Pixel blocks remain visually consistent with the current four-pixel atlas
  language. Avoid high-frequency gravel noise, realistic displacement and
  glossy wet mud.
- Use one shared dirt material across ordinary trail, fan, lips, landings and
  merge. Variation comes from vertex color, atlas region and geometry.
- Keep cue yellow out of permanent dirt geometry. It belongs only to temporary
  guidance and small diegetic markers.

## Preview And Line-Selection Cue

The cue explains three choices without replacing the visible jump anatomy.

### Diegetic Preview

- At 30-40 m, show three small carved trail-marker tabs beside the fan: one
  notch for Short, two for Medium and three for Long.
- Marker silhouettes are original, logo-free and low enough not to block a
  landing.
- At 18-24 m, emphasize the recommended tyre path with a restrained warm crown
  and two short edge ticks. Do not paint the whole approach yellow.
- At 10-14 m, the chosen line receives one brief ground-hugging pulse that
  travels toward its lip. Other lines remain fully visible in normal colors.
- Stop all selection pulses before the front wheel reaches the belly. The lip
  itself must stay readable as terrain.

### HUD Companion

- Use a compact three-arc or three-gap glyph, ordered left-to-right like the
  world geometry.
- Highlight the recommended line and show the predicted lip speed. Avoid
  unexplained `PUSH NOW` text.
- If speed is below Short, indicate `BUILD SPEED` and keep Short highlighted.
- If speed exceeds Long's calibrated range, indicate `SETTLE` and keep Long
  highlighted. Never direct the rider toward empty space outside the fan.
- The world cue and HUD must change from the same stable recommendation state.

## Camera Readability

- Use the approved medium-centre chase camera as the primary review camera.
- By 35 m, frame the complete fan and at least the upper half of every landing.
- Raise or widen framing gradually by only the amount needed to contain the
  outer lips. Do not snap to an overhead view.
- Before the decision, aim near the Medium landing knuckle so the three gaps
  display useful parallax and the Long landing remains visible.
- After selection, blend the target toward the chosen lip and landing over at
  least 0.5 s. Camera yaw and lateral position must follow the curved branch,
  never jump directly to its centre.
- Keep the full rider and both wheel contact regions in frame from preload to
  landing. Preserve 15 percent headroom above the expected apex.
- During flight, do not orbit, zoom dramatically or pitch with every frame of
  bike rotation. A restrained 3-5 percent camera lead toward the landing is
  enough.
- At landing, let the rider move slightly down-screen as suspension compresses,
  then ease back to the normal chase anchor through recovery.
- Validate every line at 1280x720 and at the target laptop's native viewport.
  The three choices must remain distinguishable without relying on tiny text.

## Rider And Bicycle Performance

Use the articulated full-suspension rider from `WorkoutGameRiderBike.qml` as
the base. The visual sequence must remain driven by authoritative feature and
physics state.

### Approach

- Rider becomes standing and centred as the fan begins.
- Pedalling continues while power is requested; upper-body sway stays bounded.
- Head and helmet point toward the selected landing, not toward the camera.
- Steering and bike lean follow the selected branch continuously.

### Preload And Take-Off

- Enter a visible compression 0.35-0.55 s before the lip.
- Fork and rear shock compress together, with hips low and elbows bent.
- Release through the lip: legs extend, front wheel becomes light first and
  torso moves slightly forward. Both tyres leave at their actual contacts.
- Do not use a side flip. The base jump is straight with at most a small
  speed-dependent tabletop-style bike yaw reserved for a later trick system.

### Flight

- Short uses a compact neutral pose with only slight nose-up attitude.
- Medium uses a stronger extension followed by a level bike and bent elbows.
- Long uses the deepest preload, clearest hip shift and most visible bike level
  correction, but remains plausible and controlled.
- Stop crank-driven pedalling while coasting in the air unless the simulation
  explicitly requests it. Feet remain attached to moving pedals.
- The ground-fixed shadow remains under the trajectory, shrinks and softens at
  apex, then strengthens toward landing.

### Landing And Recovery

- Align both bike pitch and wheel tangent with the selected landing.
- Rear and front suspension respond independently to actual wheel contact.
- Use a fast first compression, a smaller rebound and no repeated pogo motion.
- Rider absorbs with bent knees and elbows, then returns to a neutral standing
  pose over 0.6-0.9 s.
- The first implementation does not simulate a crash or case. An approach that
  cannot safely clear Short commits to the grounded bypass before the split.

## Dust And Landing Effects

- Emit dust from tyre contact, not from the rider origin or screen centre.
- Use 6-10 blocky dust cards or low-poly clods for Short, 10-16 for Medium and
  14-22 for Long at a clean landing. Scale effect strength by impact energy,
  not by line name alone.
- Keep effect duration between 0.35 and 0.60 s. Most particles remain below
  axle height and behind the rider.
- Reuse the dirt palette: `#B07F48`, `#F0D38B` and `#8D6038`, with restrained
  alpha. Do not create a realistic smoke cloud.
- Add one brief tyre-contact streak down the landing and two or three tumbling
  clods on stronger impacts.
- A clean landing gets a short pale-yellow rim pulse near the contact patch.
  The grounded bypass gets no landing effect or celebratory flash.
- Effects may clarify the result but may not hide the rider, next trail, power
  profile or telemetry.

## Asset Topology And Handoff

Treat the complete three-line feature as one socketed trail piece. Artists may
author modular source objects, but the runtime result must share a common
coordinate system and exact boundaries.

Required logical groups:

- `ROOT_GapJumpThreeLine`
- `SOCKET_IN` and `SOCKET_OUT`
- `GEO_GapJumpGround_LOD0..2`
- `GEO_GapJumpTread_LOD0..2`
- `GEO_GapJumpAccents_LOD0..2`
- `MARKER_DECISION`
- `MARKER_SHORT_LIP`, `MARKER_SHORT_APEX`, `MARKER_SHORT_LAND`
- `MARKER_MEDIUM_LIP`, `MARKER_MEDIUM_APEX`, `MARKER_MEDIUM_LAND`
- `MARKER_LONG_LIP`, `MARKER_LONG_APEX`, `MARKER_LONG_LAND`
- `MARKER_MERGE_START` and `MARKER_RECOVERY`

Author one centreline and surface profile per line. Render mesh, tyre contact,
collision proxy, camera path and flight markers must be derived from those
canonical profiles. Decorative skirts may deviate outside the rideable
surface, but they must preserve continuous normals and hide all joins.

Use named material slots shared with the existing atlas rather than separate
materials per line. Keep markers and collision proxies non-rendering.

## Performance Budget

The feature must fit inside the existing scene budget of fewer than 30,000
visible custom triangles and 50 target draw calls. The current automated hard
ceiling of 80 draw calls is not the art target.

### Geometry

| Level | Switch intent | Complete three-line tile budget |
| --- | --- | --- |
| LOD0 | Action range, roughly 0-28 m | <= 3,600 visible triangles |
| LOD1 | Approach range, roughly 28-65 m | <= 1,800 visible triangles |
| LOD2 | Preview range, beyond roughly 65 m | <= 650 visible triangles |

- Count all three lines, skirts, gap bowls and permanent accents in the tile
  budget.
- Preserve lip, gap and landing silhouettes at every LOD. Remove small erosion,
  stones and underside subdivisions first.
- Keep collision and tyre-contact sampling independent from visible LOD so a
  switch cannot alter physics.
- Hide LOD changes with bounded distance hysteresis or a short fade that does
  not double the full-detail geometry for more than a few frames.

### Draw Calls And Effects

- Steady feature contribution: no more than 5 actual draw calls.
- Peak contribution with selection cue and landing effect: no more than 7.
- Use at most three terrain material batches: shared dirt, forest floor/cut
  earth, and shared stone/wood accents.
- Batch all permanent cue posts and minor accents where practical.
- Landing particles add no more than 44 triangles at peak when represented as
  22 camera-aware quads, or 132 triangles as simple clods.
- Add no runtime file lookup, texture generation or unbounded particle emitter.
- Feature rendering, LOD preparation and effects must never block trainer
  control, recording or the authoritative fixed physics step.

## Matched Concept-Review Frames

Every review frame is a pair:

1. an approved concept paint-over using the legacy 2.5D palette and shape
   language; and
2. a production Quick 3D capture with the same viewport, camera transform,
   rider state, selected line, distance and lighting.

Use 1280x720 PNG, lossless color, HUD hidden unless the frame explicitly tests
the cue. Record camera and world values beside each pair. Do not approve from
an isolated asset turntable alone.

### Required Contact Sheet

| Frame | State | Required evidence |
| --- | --- | --- |
| A | 38 m before decision, no selection | Whole earthwork, all lips and all landing tops read as one feature in forest context. |
| B | 22 m before decision, Medium recommended | Three-line fan and restrained world cue are legible; no line is hidden. |
| C | 10 m before Short lip | Short gap is visibly open, compact and aligned with rider and camera. |
| D | Short apex | Tyres clear the gap, shadow establishes height, landing tangent matches bike pitch. |
| E | 10 m before Medium lip | Hero S-profile, case pad and landing sweet spot read without labels. |
| F | Medium apex | Rider pose and suspension release are clear against sky or pale fog. |
| G | 12 m before Long lip | Extra gap length and stronger landing are obvious without scaling the bike. |
| H | Long apex | Full rider remains framed; trajectory, shadow and visible landing explain the flight. |
| I | Medium clean landing | Both wheel contacts, suspension compression, dust origin and trail tangent agree. |
| J | Grounded bypass | The safe route remains continuous, outside all three gaps and visually secondary. |
| K | 6 m after merge begins | Three routes converge as broad continuous curves with no seam or crossing. |
| L | Recovery at output socket | Ordinary 1.36 m singletrack resumes with matching terrain, UV and forest density. |

### Diagnostic Frames

- One 3/4 overhead frame with canonical centrelines and marker names overlaid.
- One side orthographic anatomy frame per line with dimensions, lip tangent,
  predicted trajectory envelope, case pad and landing sweet spot.
- One wireframe frame showing socket continuity, skirts and no exposed
  underside.
- One LOD0/LOD1/LOD2 silhouette strip from the same 55 m camera.
- One cue-on/cue-off pair proving the feature remains understandable without
  UI assistance.
- One dark-ground and one bright-ground contrast pair proving rider and lip
  readability in both conditions.

Reject a review set if the render selected the legacy Scene Graph renderer.
Evidence logs must state `Workout Game renderer selection: Qt Quick 3D`.

## Acceptance Criteria

The art feature is ready for implementation acceptance only when:

- all three lines look like sculpted bike-park gap jumps rather than blocks,
  ramps on top of a road or three scaled copies;
- Short, Medium and Long are distinguishable at 30 m with the HUD hidden;
- predicted speed selects a stable, spatially matching line;
- lip, flight, wheel contact, landing and result effect agree in every matched
  frame and in motion;
- fan-out and merge contain no lateral teleport, crossing or abrupt camera
  correction;
- every visible surface is opaque, grounded and socketed, with no clipping,
  z-fighting, exposed underside or scenery pop in the camera corridor;
- forest density and relief match the surrounding course and never resemble an
  open field;
- the rider remains recognizable, fully framed and faithful to the accepted
  legacy arcade character;
- LOD, triangle and draw-call budgets pass in the packaged AppImage on the
  target laptop;
- a short video shows one clean ride on every line plus one grounded bypass,
  followed by interactive user review.

## Originality And Legal Safety

- Build geometry, textures, marker symbols and paint-overs specifically for
  GoldenCheetah from this brief and project-owned procedural sources.
- Use engineering diagrams only to understand generic jump anatomy and safe
  sight-line principles. Do not trace their contours, copy measurements as one
  complete design, reproduce photographs or recreate a named real location.
- Do not copy commercial game art, bike-park branding, trail logos, sponsor
  marks, signature colorways or recognizable signage.
- Use generic earthwork construction and deliberately original asymmetry,
  forest dressing, wear patterns and marker shapes.
- Record source files, author, license and generation steps in the asset
  manifest. External assets require a GPL-compatible license and retained
  attribution before review; an unclear license means the asset is rejected.
- Concept references may guide category, silhouette and mood, but final review
  must compare for unwanted similarity as well as visual quality.

## Delivery Package

The art handoff is complete when it contains:

- editable source scene with scale and axes documented;
- deterministic exported LOD meshes and material-slot list;
- canonical centreline, surface, marker and socket data;
- collision and flight-envelope debug views;
- texture source or deterministic generation inputs;
- triangle, material and draw-call report for each LOD;
- the complete matched concept-review contact sheet;
- one Quick 3D approach/action/recovery video for each line;
- asset provenance and license record.
