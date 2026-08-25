# Workout Game 3D Asset And Delivery Backlog

## Goal

Turn the Qt Quick 3D renderer prototype into a releasable stylized MTB game
without changing trainer control, recording, deterministic simulation, or the
accepted 0.68 m normal trail half-width.

The visual target is a low-poly 3D world with the color separation, readable
silhouettes, and playful timing of an old arcade game. It is deliberately not
photorealistic. The camera remains a constrained chase view until the complete
feature set is readable and stable.

## Asset Admission Rule

An external asset is not accepted until its manifest records the exact source
URL, author, license and version, attribution text, original file hash, allowed
modifications, redistribution terms, source format, conversion steps, output
hash, triangle count, texture memory, and reviewer. Missing or unclear rights
mean rejection.

Preferred sources are, in order:

1. Project-authored deterministic geometry and artwork.
2. CC0 or equivalent public-domain assets.
3. Clearly redistributable permissive assets.
4. CC-BY assets only when attribution and adaptation marking are practical.

Non-commercial, editorial-only, personal-use, no-derivatives, marketplace-only,
AI models without output rights, and unclear licenses are not accepted. A free
download is not evidence of redistribution rights.

## Object Inventory

### Rider And Bicycle

| ID | Object | Required parts and states | Preferred source strategy |
| --- | --- | --- | --- |
| RB-01 | MTB frame | Frame, fork, handlebar, saddle, pedals and visible drivetrain; metre scale and named pivots | Adapt a permissive low-poly model if topology is clean; otherwise author |
| RB-02 | Wheels | Separate front/rear wheels with axle pivots and readable tire silhouette | Adapt or author |
| RB-03 | Suspension | Fork and optional rear travel with bounded compression | Author onto selected bike |
| RB-04 | Rider body | Helmet, torso, pelvis, upper/lower limbs, hands and feet with a compact rig | Adapt a permissive rigged human, then remodel clothing/proportions |
| RB-05 | Rider materials | High-contrast jersey, helmet, skin, bike and tire palette | Author |
| RB-06 | Animation set | Idle, pedal seated, pedal standing, compress, take-off, air, land, absorb, lean left/right, drop, bypass and recovery | Author for the selected rig |
| RB-07 | Contact markers | Wheel contacts, hands, feet, pelvis, camera target, shadow and collision proxy | Author |

### Canonical Trail And Terrain

These objects must be project-authored because their dimensions and sockets
are simulation contracts. Stock meshes may be visual references only.

| ID | Object | Required variants |
| --- | --- | --- |
| TR-01 | Normal singletrack | Straight, gentle left/right and configurable length; half-width 0.68 m |
| TR-02 | Grade transition | Level-to-climb, climb-to-level, level-to-descent and descent-to-level |
| TR-03 | Width transition | Full trail to narrow feature and back without a seam |
| TR-04 | Split line | Main line plus safe bypass, visible before the decision point |
| TR-05 | Trail shoulders | Bank/cut, drainage edge and embedded ground transition |
| TR-06 | Forest floor tile | Wide camera-safe ground joined to the trail shoulder |
| TR-07 | Distant terrain | Low-cost hills/ridges with uneven horizon and no physics role |
| TR-08 | Surface materials | Dirt, packed dirt, stone, wood and forest floor in one bounded atlas |

Every physical tile uses metre scale, Y up and forward +Z. Entry and exit
sockets include position, heading, elevation, grade, half-width and surface
class. The road course remains authoritative; a GLB cannot define workout
targets, feature decisions, or trainer resistance.

### Technical Trail Features

| ID | Feature | Required authored objects | Key animation/physics cue |
| --- | --- | --- | --- |
| FT-01 | Tabletop | Approach, take-off belly/lip, deck, knuckle, landing and joined side terrain | Compress, lift, bounded air, landing compression |
| FT-02 | Log over | Partly buried tapered log, end grain, root/branch remnants and bypass | Front lift, rear clearance, landing/absorb |
| FT-03 | Bunny hop | Low hurdle or compact kicker, take-off mark and clear landing | Short compress/lift/land sequence |
| FT-04 | Drop | Level approach, sharp lip, exposed face, lower transition and bypass | Forward/down pitch, short air, landing impact |
| FT-05 | Rollers | Three rounded crests and troughs formed in the trail surface; no separate bypass | Pump/absorb with both wheels surface-anchored |
| FT-06 | Berm | Broad bowl-shaped bank, inner edge and joined approach/exit | Shared curved line, rider lean and roll |
| FT-07 | Roots | Accepted project-authored buried network of eight branches, widened same-tread safe line and exact sockets | Box2D suspension drives bounded torso roughness; no scripted air or camera vibration |
| FT-08 | Rock garden | Accepted project-authored group of twelve sunk irregular stones, widened same-tread safe line and exact sockets | Box2D suspension drives bounded torso roughness and line choice; no scripted air or camera vibration |
| FT-09 | Rock slab | Accepted project-authored asymmetric rollover face, exposed buried sides, four fissures, same-tread safe line and exact sockets | Box2D surface and front/rear suspension drive pitch and bounded torso response; no scripted air or camera vibration |
| FT-10 | Skinny | Accepted project-authored deck boards, beams/supports, visible ground and exact width transitions | Deterministic subtle balance lean on the narrow line; grounded packed-dirt safe line |
| FT-11 | Climb | Accepted project-authored rising same-tread face with five embedded rock steps, bounded grade transitions and a readable crest | Grounded seated/standing/walking effort pose, bounded crest release and six-metre result carry |

### Environment And Effects

| ID | Object | Required variants | Source strategy |
| --- | --- | --- | --- |
| EN-01 | Conifer trees | 5-8 silhouettes, 2-3 sizes, camera-safe near variants | Search/adapt coherent permissive pack |
| EN-02 | Deciduous trees | 3-5 silhouettes for visual variation | Search/adapt |
| EN-03 | Shrubs/ferns/grass | Small atlas-driven clusters; never hide the trail | Search/adapt or author |
| EN-04 | Rocks/debris | 8-12 irregular props distinct from physical feature rocks | Search/adapt |
| EN-05 | Logs/stumps | 4-6 decorative variants | Search/adapt |
| EN-06 | Distant forest/hills | Low-poly or billboard depth layers | Author from owned/generated work |
| EN-07 | Sky/fog | Flat-color arcade sky and restrained depth fog | Author |
| FX-01 | Contact shadow | Ground-fixed rider/bike shadow with airborne scaling | Author |
| FX-02 | Dirt/dust | Small bounded landing and rear-wheel effects | Author |
| FX-03 | Success/bypass cue | World-space marker plus HUD response | Author |
| FX-04 | Feature markers | Prepare, act-now and landing indicators integrated into trail | Author |

### Non-Visual Support Assets

| ID | Object | Purpose |
| --- | --- | --- |
| SV-01 | Collision proxy | Simple named box/capsule/hull separate from rendered mesh |
| SV-02 | Camera exclusion volume | Prevent trees/terrain from obscuring or intersecting camera |
| SV-03 | LOD/instancing metadata | Keep draw calls and visible triangle count bounded |
| SV-04 | Catalog cameras | Fixed low-centre, medium-centre and slight-shoulder viewpoints |
| SV-05 | License manifest | Machine-readable provenance and attribution per imported file |
| SV-06 | Asset validation report | Scale, sockets, bounds, pivots, nodes, animation, material and budget checks |

## Delivery Tasks

### Phase 1: Inventory And Research

- [x] `INV-01` Record the complete first-pass object inventory.
- [x] `LIC-00` Research provider terms and draft the conservative license policy.
- [ ] `LIC-01` Approve an allow/conditional/reject license policy.
- [x] `SRC-01` Rank reusable rider and MTB models.
- [x] `SRC-02` Rank reusable vegetation, terrain and natural prop packs.
- [x] `SRC-03` Search each technical feature and identify custom-model gaps.
- [ ] `SRC-04` Record candidate assets in a machine-readable manifest.
- [ ] `ART-01` Approve one shared low-poly/pixel-textured art brief.

### Phase 2: Pipeline And Camera

- [x] `PIPE-00` Add a repository-owned Blender 4.0.2 Docker environment and a
  deterministic, self-checking tabletop GLB generator.
- [x] `PIPE-01` Define the GLB node, pivot, material, socket and animation schema.
- [x] `PIPE-02` Add license/provenance manifest validation.
- [x] `PIPE-03` Add GLB bounds, scale, triangle, texture and node validation.
- [x] `PIPE-04A` Package and render one Balsam-converted asset through the
  production qrc.
- [x] `PIPE-04B` Verify the packaged asset from an extracted AppImage.
- [x] `CAM-01` Implement low-centre, medium-centre and slight-shoulder candidates.
- [x] `CAM-02A` Capture matched stills on the same deterministic state.
- [x] `CAM-02B` Capture matched videos on the same deterministic route.
- [ ] `CAM-03` Select and regression-test the fixed baseline camera.
- [x] `CAM-04` Add camera/vegetation exclusion and terrain intersection tests.

The first `CAM-01` catalog is retained as a build artifact. The provisional
default is `medium-centre`; `low-centre` and `shoulder` remain audit modes via
`GC_WORKOUT_GAME_3D_CAMERA`. The matched 102 m motion audit records 360 frames
per camera at 960 by 540 and 30 FPS. Automated checks require nonblank output
and visible changes in more than 90 percent of frame transitions. Final
selection still requires user review of the encoded 12-second videos.
The post-exclusion comparison video has SHA-256
`ecbf4e840eea5adee10fe06c3f054ca09da6e963b6f89626fe5156d6d63445c2`.
All three source videos contain exactly 360 frames at 30 FPS and the complete
interactive X11/OpenGL view suite passes 13 tests with no failures.

The first generated tabletop audit artifact is stored outside the source tree
and is reproducible from
`contrib/workout-game-assets/blender/generate_tabletop.py`. Blender 4.0.2
produced a 9,856-byte GLB with 78 vertices and 96 triangles. Its SHA-256 is
`2fc4babe4143a3d8152b067bd5b1e51684bc4ada383a8f085cf4fe81270f4d3e`.
Khronos glTF Validator 2.0.0-dev.3.10 reported zero errors, warnings, infos and
hints. A second clean export produced the same SHA-256. Two Balsam 6.8.3 runs
also produced byte-identical QML and `.mesh` outputs. The cross-platform policy
validator and thirteen policy/fixture tests pass, and the production qrc
renders the committed mesh under X11/OpenGL. The asset remains a vertical-slice
candidate until safe-line geometry and the extracted-AppImage gate pass.

### Phase 3: Vertical Slice

- [ ] `VS-01` Build ordinary trail, shoulder and forest-floor tiles.
- [ ] `VS-02` Build the complete tabletop tile and safe bypass.
- [ ] `VS-03` Integrate the selected bike/rider rig and core animation set.
- [ ] `VS-04` Add the first coherent tree/rock/ground art set.
- [ ] `VS-05` Fix feature guidance and Data Generator target authority.
- [ ] `VS-06` Record complete and bypassed tabletop sessions.
- [ ] `VS-07` Verify recording, trainer priority, deterministic outcome and target-laptop frame budget.

### Phase 4: Feature Production

- [ ] `PROD-01` Log over and bunny hop.
- [x] `PROD-02` Drop and its safe bypass.
- [x] `PROD-03` Rollers and berm.
- [x] `PROD-04` Roots, rock garden and rock slab.
- [x] `PROD-05` Skinny and width transitions.
- [x] `PROD-06` Climb, grade transitions and crest response.
- [ ] `PROD-07` Feature-specific rider/camera/effect states.
- [ ] `PROD-08` Completed/bypass catalog and live UI tests for all eleven features.

`PROD-04` is accepted as deterministic project-authored procedural geometry.
Roots, rock garden and rock slab use dedicated canonical course-space profiles,
bounded merged geometry and no external asset or additional license obligation.

`PROD-05` is accepted as deterministic project-authored procedural geometry.
Skinny uses one canonical socketed profile for deck height, width transitions,
same-tile safe line, physics contact and both renderers. Its merged Quick 3D
range is bounded to 1,008 vertices and 504 triangles per tile with at most
twelve resident tiles and no external asset or additional license obligation.

`PROD-06` is accepted as deterministic project-authored procedural geometry.
Climb uses one canonical yaw-aware profile for physical contact, visible step
plateaus and ramps, grade transitions, sockets, effort pose and crest release.
Its merged Quick 3D range is bounded to 680 vertices and 340 triangles per
tile, with no external asset or additional license obligation.

### Phase 5: Release

- [ ] `REL-01` Add workout profile, grade and final action guidance to the HUD.
- [ ] `REL-02` Run isolated-data UI, save/continue, recording and Bluetooth regressions.
- [ ] `REL-03` Measure frame-time p50/p95/p99 and simulation skipped ticks on the target laptop.
- [ ] `REL-04` Verify AppImage assets, plugins, attributions, licenses and SBOM.
- [ ] `REL-05` Run user A/B testing before retiring the legacy renderer.

## Vertical Slice Acceptance

The project does not expand from tabletop to the remaining ten features until:

1. The rider, tabletop, landing and bypass are identifiable without HUD text.
2. Trail, feature and forest-floor sockets have no visible gap or scale change.
3. Rider contacts follow the authoritative road surface except during bounded
   intentional air time.
4. The chosen camera shows the next 25-40 m, does not make the rider appear
   sideways and cannot be blocked by a tree.
5. Data Generator can deterministically demonstrate completed and bypassed
   outcomes using the same active target as the game.
6. The target laptop stays within the measured frame budget without increasing
   trainer-control or recording latency.
7. Every distributed asset passes provenance, license and packaging checks.
