# Workout Game 3D Visual Backlog

## Purpose

This is the visual-quality backlog for the Quick 3D Workout Game. Technical
existence or a passing geometry test does not make an item visually complete.
An item is complete only after it has matched stills, motion evidence and an
interactive review in the packaged AppImage.

The target is not photorealism. Preserve the arcade identity, strong rider
silhouette, limited palette and pixel-art character of the legacy 2.5D view
while retaining the Quick 3D renderer's real depth, stable occlusion, curved
trail, terrain contact and GPU rendering.

## Preserve

- Keep the accepted 1.36 m singletrack width and its scale relative to the
  bicycle.
- Keep trail, rider, camera, feature and terrain placement in one 3D world
  coordinate system.
- Keep the deterministic simulation and asynchronous newest-frame rendering;
  graphics must not delay trainer control or recording.
- Keep the legacy view's high-contrast rider, playful proportions, restrained
  pixel palette and immediate arcade readability as visual references.
- Keep opaque, rough materials and a readable trail. Avoid photorealistic PBR,
  glossy ground, dense visual noise and generic asset-pack inconsistency.

## Style Translation

The preferred implementation is stylized 3D, not a flat rider billboard:

- Use low-poly 3D meshes for the rider, bicycle, terrain and physical trail
  features so side presentation, turns, lean, suspension and jumps remain
  spatially correct.
- Redesign the 3D rider and bicycle to match the legacy rider's silhouette,
  color separation and personality from chase distance.
- Use deliberately low-resolution, palette-controlled pixel textures with
  crisp magnification and mipmapped minification.
- Use simple stepped or toon-like lighting and authored color ramps instead of
  realistic materials. Selective dark edge materials may strengthen the rider
  and bicycle silhouette without a full-screen outline pass.
- Use crossed sprite cards or billboards for distant foliage and small forest
  dressing where they improve the legacy layered look. Near objects and
  anything with contact, collision or a camera intersection remain 3D.
- Keep the HUD at native resolution. Pixel character should come from the art,
  not from making telemetry text blurry.

## Visual Defects

Status values are `open`, `in progress`, `review` and `accepted`.

| ID | Priority | Status | Defect | Acceptance |
| --- | --- | --- | --- | --- |
| VIS-01 | P0 | open | The Quick 3D world and the legacy view do not yet look like the same game. | A matched contact sheet demonstrates one coherent palette, pixel treatment, shape language and contrast hierarchy in both chase and side views. |
| VIS-02 | P0 | open | The 3D rider has less personality and weaker chase-view separation than the legacy rider. | Helmet, torso, limbs and pose remain readable against dirt, grass, stone and trees at the normal camera distance without relying on the HUD. |
| VIS-03 | P0 | open | The bicycle is difficult to inspect from behind and still needs a distinctive modern full-suspension silhouette. | Opening and idle side views clearly show original frame geometry, fork and rear suspension, moving cranks and pedals, drivetrain, saddle, handlebar and large black tyres. |
| VIS-04 | P0 | open | Ordinary terrain is too smooth and open, so the ride can resemble a path across a field. | Every review segment contains readable near-, mid- and distant terrain relief plus forest-floor variation while preserving a clear riding line. |
| VIS-05 | P0 | open | Long sections remain too straight and visually calm for an MTB trail. | Ordinary near-straight trail is limited to 25 m, and each rolling 100 m shows at least three alternating bends and 45 degrees of accumulated heading change. The review route combines smooth gentle, medium and near-90-degree turns, ordinary banked bends, rollers, short rises and descents with no abrupt camera yaw. Feature safety zones are the only bounded exception. |
| VIS-06 | P0 | open | Technical features can be too small, too late to read or visually interchangeable. | Each feature is identifiable without its name at approach distance and retains a distinct silhouette through action and recovery. |
| VIS-07 | P0 | open | Some features and props can look placed on top of the trail instead of formed into it. | Entry, feature and exit share exact trail sockets, material bands and surrounding terrain with no seam, floating edge, clipping, z-fighting or exposed underside. |
| VIS-08 | P0 | open | Rider motion does not yet communicate every terrain action strongly enough. | Pedalling, steering, lean, seated/standing effort, suspension, preload, take-off, air, landing, pump and rough-surface absorption are recognizable in motion with the HUD hidden. |
| VIS-09 | P0 | open | Camera composition does not always expose terrain shape and the upcoming feature. | The medium-centre camera keeps the complete bicycle in frame, shows 25-40 m of useful trail and gives climbs, drops and jumps readable side relief without motion sickness. |
| VIS-10 | P1 | open | Repeated low-poly trees dominate the environment and weaken the Finnish-forest feeling. | Multiple tree ages and silhouettes, undergrowth and dead wood form depth layers without blocking the rider, cue or trail. |
| VIS-11 | P1 | open | Lighting, sky and distance treatment are flat and do not separate depth reliably. | Bounded lighting, contact shadows, restrained fog and an uneven forest horizon separate rider, trail, foreground and distance on bright and dark terrain. |
| VIS-12 | P1 | open | Ground props may pop, intersect, float or reveal below-ground geometry. | A deterministic camera sweep shows stable terrain anchoring, bounded LOD transitions and no visible spawn, underground portion or camera intersection. |
| VIS-13 | P1 | open | Surface materials repeat visibly and do not explain trail condition. | Dirt, packed line, loose edge, rock, root, wood and forest floor remain coherent but vary without shimmer or obvious short tiling. |
| VIS-14 | P1 | open | Feature success, bypass and landing lack enough visual punctuation. | Short bounded dust, debris, suspension and success feedback make the result clear without obscuring telemetry or changing simulation timing. |
| VIS-15 | P1 | open | The HUD occupies a large dark band and competes with the landscape. | Essential live values and workout profile remain readable while the trail and horizon retain enough unobstructed vertical space at laptop resolution. |
| VIS-16 | P0 | open | The game has no progressive gap-jump choice matched to approach speed. | A shared approach fans smoothly into short, medium and long adjacent gap lines with visibly different lips, gaps and landings; the selected line is locked early from predicted take-off speed, while an unsafe approach uses a separate grounded route. |
| VIS-17 | P0 | open | Berms are currently absent or too subtle in ordinary riding, including turns where approach speed should make banking visually important. | Medium, sharp and high-speed bends use unmistakable bowl-shaped banks joined to the surrounding trail and terrain. A deterministic review route shows multiple radii and banking strengths; the rider follows and leans around the same curved surface, while the camera turns smoothly and no feature banner appears. |
| VIS-18 | P1 | open | Successful jumps do not yet show a recognisable MTB tailwhip whose amplitude reflects launch speed. | A motion catalog compares low, medium and high safe launch speeds. Rear-wheel lateral rotation grows continuously up to a bounded 35-degree peak, reads clearly from the chase camera, includes restrained rider counter-motion and returns the bicycle to within 2 degrees of the travel direction before tyre contact. There is no roll-based side flip, teleport, physics-root movement or tailwhip on a grounded bypass. |

## Environment Elements

These are visual building blocks, not automatically physics obstacles. All
placements are deterministic, terrain-anchored, camera-safe and batched or
instanced within the existing render budget.

| Group | First-pass elements | Notes |
| --- | --- | --- |
| Trees | Mature spruce and pine, birch, saplings, dead standing trunk | Use several asymmetric silhouettes and age classes rather than only scale variants. |
| Undergrowth | Bilberry-like shrubs, ferns, heather, grass tufts, moss patches | Prefer small clusters and sprite-card hybrids; keep the trail edge readable. |
| Forest floor | Needles, leaves, twigs, cones, bark and subtle color patches | Mostly texture/decal variation; no dense individual-object cost. |
| Ground forms | Embedded stones, boulders, exposed bedrock, small banks and hollows | Decorative stones must remain visually distinct from scored rock features. |
| Dead wood | Stumps, broken trunks, fallen logs and branch piles | Partly bury and align them to terrain; never expose a flat cut through the ground. |
| Trail surface | Shallow rut, worn centre line, loose edge, root patches, puddle or damp patch | Variants share the same physical road unless explicitly promoted to a feature. |
| Trail context | Drainage dip, cut slope, raised outer edge and occasional trail marker | Use sparingly so the forest remains natural and the route stays obvious. |
| Distance | Layered forest, irregular ridges and occasional exposed rock face | Low-cost silhouettes establish scale and remove the flat horizon. |

## Feature Art Pass

Review every feature separately at approach, action and recovery positions.
Berms are ordinary route geometry and do not receive a challenge banner.

| Feature | Required visual signature |
| --- | --- |
| Roots | A partly buried branching network that grows out of the surrounding forest floor, not parallel bars. |
| Rollers | Two or three continuous rounded crests and troughs with visible side relief. |
| Climb | A clearly rising line, side bank/cut, embedded steps where configured and a visible crest. |
| Rock garden | A broad cluster of sunk irregular stones with a readable but rough main line. |
| Bunny hop | A compact obstacle or kicker, immediate take-off reference and clean landing zone. |
| Drop | Level approach, sharp lip, visible face, short air gap and lower landing transition. |
| Skinny | A long narrow deck with visible boards, supports and ground clearance; no chicken line. |
| Banked turn | A bowl-shaped outer bank whose curve, rider line and lean agree; no feature prompt. |
| Log over | A substantial partly buried trunk extending beyond both trail edges with bark and end detail. |
| Tabletop | Belly, lip, deck, knuckle and landing readable from the chase camera before take-off. |
| Gap jump | Three adjacent, progressively longer real gaps with a common approach, early fan-out, distinct landings and a smooth merge; the selected line remains obvious before commitment. |
| Rock slab | One asymmetric exposed stone face with a rollover crest, fissures and buried side edges. |

## Proposed Presentation Elements

- Opening side presentation that shows the complete rider and bicycle before
  moving smoothly to the chase camera.
- Idle side presentation after pedalling stops, cancelled immediately when
  riding resumes or a feature needs the chase view.
- Subtle tyre contact shadow and short rear-wheel dust on loose surfaces.
- Brief landing compression, dirt puff and trail-space success marker.
- Restrained suspension and body motion over ordinary trail relief so even the
  transitions between scored features feel alive.
- Terrain-aware ambient details rather than rivals. Other riders remain out of
  scope until the main rider and world are visually coherent.

## Review Order

1. Produce a matched legacy/Quick 3D style reference using the same rider
   state, camera framing and trail section; approve palette and silhouette.
2. Complete the rider and bicycle style pass, including side presentation and
   all visible pedal/suspension motion.
3. Build one dense but camera-safe Finnish-forest kit and one varied ordinary
   trail segment.
4. Redraw and review each feature using the approved world kit and exact tile
   sockets.
5. Add lighting, fog, contact shadows and bounded effects.
6. Tune HUD footprint only after camera framing and feature readability are
   stable.

## Evidence Required

- Matched 1280 by 720 stills from the legacy and Quick 3D renderers.
- A side-view rider/bicycle sheet covering idle and a complete crank cycle.
- Approach/action/recovery stills for every feature with HUD text hidden.
- A short all-feature Quick 3D video and a longer ordinary-course video from
  the packaged AppImage.
- Frame-time, draw-call, triangle, pop-in, camera-clearance and forward-motion
  reports captured separately from video recording.
- Interactive user review on the target laptop before an item becomes
  `accepted`.

## Baseline Evidence

The 2026-09-02 packaged baseline explicitly selected Quick 3D and logged
`Workout Game renderer selection: Qt Quick 3D`. It used an isolated temporary
athlete, Feature Lab and the Data Generator; it did not open production athlete
data. The run advanced 767.782 m with no backward frames, skipped simulation
ticks, trace regressions or unexpected airborne frames. Median presented rate
was 61.676 FPS, observed p95 frame interval was 19 ms and p99 was 21 ms.

The external baseline artifact directory is named
`gc-ui-cc84c34-quick3d-review`.

The older `gc-ui-cc84c34-final-long/session.mp4` selected the legacy Scene
Graph renderer and must not be used as Quick 3D acceptance evidence.
