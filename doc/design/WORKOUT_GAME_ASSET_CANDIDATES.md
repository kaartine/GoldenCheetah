# Workout Game Asset Candidates

## Status

This is a research shortlist, not an approval or download manifest. An asset
enters the repository only after its exact archive, revision, license text,
hashes and conversion steps pass `WORKOUT_GAME_ASSET_LICENSE_POLICY.md` and
`workout_game_asset_manifest.schema.json`.

No license-clean ready-made pack covers the complete modular MTB trail. The
rideable surfaces, connectors, main/bypass lines and gameplay silhouettes must
be project-authored. Carefully selected CC0 models can provide editable props,
prototype topology and distant environment objects.

## Technical Feature Candidates

| Feature | Candidate | License | Assessment and decision |
| --- | --- | --- | --- |
| Roots | [Poly Haven Pine Roots](https://polyhaven.com/a/pine_roots), Rob Tuytel | CC0-1.0 | Approximately 163k triangles and 354.6 MB with realistic PBR materials. Reference/retopology only; custom low-poly buried root network required. |
| Rollers | [Kenney Racing Kit](https://kenney.nl/assets/racing-kit), `roadBump` | CC0-1.0 | Approximately 94 triangles and useful profile reference. Generate a custom parameterized rounded trail-width roller chain. |
| Climb/crest | [Kenney Racing Kit](https://kenney.nl/assets/racing-kit), `roadRampLong` | CC0-1.0 | Approximately 18 triangles and useful blockout. Custom variable-grade surface and rounded crest remain mandatory. |
| Rock garden | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit), path rocks and individual rocks | CC0-1.0 | Strong first candidate: roughly 16-102 triangles per part. Deterministically vary, rotate and partly embed into a custom rideable surface. |
| Bunny hop | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit) log plus [Racing Kit](https://kenney.nl/assets/racing-kit) ramp | CC0-1.0 | Low-cost reusable prop/blockout. Custom trail transition, action marker and collision profile required. |
| Drop/bypass | [Kenney Racing Kit](https://kenney.nl/assets/racing-kit) ramp and split-road references | CC0-1.0 | References only. They do not form a real ledge, lower landing or correctly timed bypass. Custom branched module mandatory. |
| Skinny | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit), `bridge_woodNarrow` | CC0-1.0 | Closest reusable feature at roughly 220 triangles. Adapt into repeatable centre/end pieces with exact trail width transitions. |
| Skinny alternative | [OpenGameArt Wooden Bridge](https://opengameart.org/content/wooden-bridge-0), JamesWhite | CC0-1.0 | Editable Blender source and small texture; candidate only after exact archive/revision verification. |
| Berm | [Kenney Racing Kit](https://kenney.nl/assets/racing-kit), sand corner | CC0-1.0 | Useful plan-view topology only; approximately 24 triangles and no banking. Custom banked spline extrusion mandatory. |
| Log over | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit), `log_large` | CC0-1.0 | Strong reusable prop at roughly 96 triangles after scaling, embedding and material adaptation. Custom trail tile and proxy remain required. |
| Tabletop | [Kenney Racing Kit](https://kenney.nl/assets/racing-kit), straight/curved ramps | CC0-1.0 | Transition references only. Custom calibrated take-off/deck/landing module mandatory and already assigned to the Blender authoring pipeline. |
| Rock slab | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit), `cliff_blockSlope_rock` | CC0-1.0 | Useful low-poly visual base at roughly 70 triangles. Requires custom rideable surface, rollover crest and socket transitions. |
| Rock reference | [Poly Haven Rock Moss Set 01](https://polyhaven.com/a/rock_moss_set_01), Kless Gyzen | CC0-1.0 | Approximately 63k triangles with realistic PBR. Reference or aggressive retopology/bake only, not direct runtime use. |

The quoted costs were measured during research from the then-current provider
downloads. Acquisition must measure and record the selected revision again.
Provider page/archive version differences must be resolved in the manifest.

## Controlled Acquisition Audit (2026-08-25)

The official Kenney download archives were fetched into a temporary quarantine;
no model has been added to the repository or production resources.

| Archive | Official download | SHA-256 | Embedded license |
| --- | --- | --- | --- |
| Nature Kit | `https://kenney.nl/media/pages/assets/nature-kit/37ac38a37b-1677698939/kenney_nature-kit.zip` | `fa7974a0d342bfe63c38664ba9f8ec1a4aab8ea25f099bdc56870e33588c4d9d` | Nature Kit 2.1, CC0-1.0 |
| Racing Kit | `https://kenney.nl/media/pages/assets/racing-kit/933b8fd9fd-1677580949/kenney_racing-kit.zip` | `8a71ea16219315a01d00d5a90c4f6b5c090faddbc56d80ecf727e2b3b853c6c0` | Racing Kit 2.0, CC0-1.0 |

The archive path audit found no absolute or parent-traversal entries. Selected
files were validated with Khronos `gltf-validator` 2.0.0-dev.3.10. Racing Kit
`roadBump`, `roadCurvedSplit`, `roadRampLong` and `roadRampLongCurved` reported
zero errors. The selected Nature Kit bridge, slab, log and rock GLBs each
reported `SCENE_NON_ROOT_NODE`; they are candidates only after Blender/Balsam
repair and a zero-error revalidation. Unused UV/node information was also
reported but is not a validator error.

## Environment Candidate

[KayKit Forest Nature Pack](https://kaylousberg.itch.io/kaykit-forest) by Kay
Lousberg is a promising coherent CC0 low-poly forest set with FBX, glTF and OBJ
files plus one 1024-square gradient atlas. The editable Blender source and
modular terrain are in paid tiers. It is a style/environment candidate, not a
source of rideable feature geometry. Verify the exact downloaded tier and
license archive before use.

## Rider And Bicycle Candidates

No reviewed pack supplies an MTB, rigged rider and cycling animations together.
Every option still requires a custom mechanical bike rig, hand/foot constraints
and cycling/feature clips.

| Rank | Candidate | License | Assessment and decision |
| --- | --- | --- | --- |
| 1 rider | [Quaternius Universal Base Characters](https://quaternius.com/packs/universalbasecharacters.html) | CC0-1.0 | Best production base: humanoid rig, textured glTF/FBX and multiple bodies, but approximately 13k triangles before adaptation. Acquire and test a decimated 4-6k-triangle rider with custom helmet/clothes and cycling clips. |
| 2 rider/prototype | [Kenney Blocky Characters](https://kenney.nl/assets/blocky-characters) | CC0-1.0 | Extremely low cost (research sample roughly 72 triangles) and native GLB with rigid-node animations. Strong stylized vertical-slice/fallback candidate, but not a skinned rig and still needs cycling pose/leg linkage. |
| 3 rider | [Plewr 3D Rigged Character](https://plewr.itch.io/3d-rigged-character) | CC0-1.0 | Compact rigged GLB/Blend/FBX alternative. Topology, weights, polygon count and texture budget need archive inspection. |
| 1 bike | [Mountain Bike (MTB) Low Poly](https://sketchfab.com/3d-models/mountain-bike-mtb-low-poly-853eacaf1a4f4f99816aa2dd54dd2ae5), guillaumemariette156 | CC-BY-4.0 | Excellent lightweight geometry candidate at roughly 1,624 triangles. Conditional only: inspect exact archive, attribute, mark changes and separate wheels/crank/pedals/bar/fork with correct pivots. |
| 2 bike/reference | [Ultra Low Poly Bicycles](https://sketchfab.com/3d-models/ultra-low-poly-bicycles-set-of-3-fca3293dcac3417ebce7625f5261275c), MechanicalOnion | CC-BY-4.0 | Strong low-cost style/topology reference with a 256-square atlas, but road/commuter geometry needs substantial MTB remodeling. |
| 3 bike/prototype | [OpenGameArt 32 Low Poly Models](https://opengameart.org/content/32-low-poly-models), Drummyfish bicycle | CC0-1.0 | Approximately 274 triangles with Blender/OBJ source. Placeholder/topology starting point only; no moving parts, UVs or MTB detail. |

[Quaternius Universal Animation Library](https://quaternius.com/packs/universalanimationlibrary.html)
is CC0 and may supply off-bike, recovery or celebration clips. It does not
document a cycling clip and therefore does not close the core animation gap.

Rejected rider/bike examples included high-cost static cyclists, a roughly
205k-triangle MTB, fan art with unclear underlying rights, and marketplace
licenses that forbid extractable source/AppImage redistribution. Mixamo is not
used because its terms do not clearly cover redistributing editable animation
source in this public project.

## Environment And Texture Shortlist

| Rank | Candidate | License | Assessment and decision |
| --- | --- | --- | --- |
| 1 | [KayKit Forest Nature Pack](https://kaylousberg.itch.io/kaykit-forest), Kay Lousberg | CC0-1.0 | Best coherent performance-oriented base. Free tier has 100+ models; Extra has modular terrain and 200+ unique models. Uses one 1024-square gradient atlas that can be reduced. Exact paid/free tier must be recorded. |
| 2 | [Quaternius Stylized Nature MegaKit](https://quaternius.com/packs/stylizednaturemegakit.html) | CC0-1.0 | Richest stylized hero-vegetation candidate with glTF/FBX/OBJ. Use selected assets only; measure dense foliage and replace engine-specific wind shaders. |
| 3 | [Quaternius Ultimate Stylized Nature](https://quaternius.com/packs/ultimatestylizednature.html) | CC0-1.0 | Smaller coherent alternative with 63 models and easier audit surface. Measure textures/triangles before approval. |
| 4 | [Quaternius low-cost fillers](https://poly.pizza/u/Quaternius) | CC0-1.0 per shortlisted asset | Pine, fern and 194-717-triangle mountain silhouettes are promising for distant depth and small fillers. Manifest each original listing separately. |
| 5 | [OpenGameArt N64 Texture Pack](https://opengameart.org/content/n64-texture-pack), n64guy | CC0-1.0 | Strong retro-style reference: mostly tileable 32-square PNGs in a small archive. Test soft bilinear/trilinear filtering and rebuild a project-owned bounded atlas. |
| 6 | [OpenGameArt Tiny Texture Pack](https://opengameart.org/content/tiny-texture-pack), Screaming Brain Studios | CC0-1.0 | Useful grass/wood sources; select the 128-square variants and atlas only reviewed tiles. |
| 7 | [ambientCG](https://ambientcg.com/) selected ground/leaves/HDRI | CC0-1.0 | Technically clean but photorealistic and comparatively heavy. Reference/recolor/pixel-bake only; avoid runtime HDR preprocessing. |

The final forest should start from KayKit, use only a few compatible
Quaternius silhouettes, and apply a custom restrained palette/low-resolution
treatment. Rideable trail, feature-integrated rocks/roots, terrain joins,
distant ridge ring, collision proxies and camera exclusion volumes remain
custom work.

## Rejected Research Examples

| Candidate | Reason for rejection |
| --- | --- |
| [Sketchfab MTB Jump](https://sketchfab.com/3d-models/mtb-jump-9fba00cc9d9e44318a0af547253f4dfc) | CC-BY-NC-SA and roughly 255k triangles: non-commercial restriction and excessive cost. |
| [Sketchfab Skatepark](https://sketchfab.com/3d-models/skatepark-17e32c89b5574d48b10f06a57d2500df) | CC-BY but roughly 339k triangles and two 16k textures: nonmodular and inappropriate for target hardware/style. |

Editorial, personal-use, marketplace-restricted and unspecified-license models
were excluded without further evaluation.

## Current Build/Reuse Decision

| Category | Decision |
| --- | --- |
| Ordinary trail, sockets and terrain join | Custom authored |
| Rollers, climb/crest, drop/bypass, berm and tabletop | Custom authored |
| Root network and feature-integrated rock surfaces | Custom authored, with CC0 reference allowed |
| Rocks, logs and bridge parts | Adapt shortlisted Kenney CC0 assets after manifest approval |
| Rider | Inspect Quaternius and Kenney candidates; custom cycling/feature animation mandatory |
| Bike | Inspect the lightweight CC-BY MTB and CC0 prototype; custom mechanical rig/remodel mandatory |
| Forest props and materials | KayKit first, selected Quaternius additions and custom retro atlas |

## Project-Authored Tabletop Greybox

`contrib/workout-game-assets/blender/generate_tabletop.py` is the first custom
asset source. It uses no external model, texture or material. The deterministic
Blender 4.0.2 audit output has 76 vertices, 122 triangles, three opaque
materials and the required sockets and gameplay markers. Khronos glTF Validator
2.0.0-dev.3.10 reports zero errors, warnings, infos and hints.

The output is not yet a runtime asset. It remains an audit artifact until it
has a reviewed manifest, safe-line/bypass geometry, optimized Qt Quick 3D
conversion, qrc packaging and an AppImage rendering test.
