# Workout Game 3D Migration

## Scope

The Workout Game presentation is moving from custom pseudo-3D QSG/QPainter
drawing to Qt Quick 3D. Trainer control, activity recording, workout timing,
feature decisions, road generation and deterministic physics do not move into
the renderer.

The legacy renderer remains available only for an A/B validation period. Set
`GC_WORKOUT_GAME_3D=1` to select the new renderer with the same workout,
telemetry and simulation snapshots. A Qt Quick 3D load failure falls back to
the scene graph renderer during this period. The old renderers and code used
only by them are retired after the acceptance gates below pass.

## Module Boundaries

- `WorkoutGameRunner` owns the simulation thread and publishes capacity-one
  immutable `WorkoutGameEngineFrame` snapshots. It never waits for rendering.
- `WorkoutGameRoadCourse` remains the semantic course and physics source. Its
  metre-based connectors are also the authoritative 3D path.
- `WorkoutGame3DGeometry` converts a complete road course into immutable trail
  and forest-floor triangle meshes. It does no work in the frame loop.
- `WorkoutGameBermGeometry` owns the distance-parameterized berm centreline,
  bank, tread, safe line and speed-derived rider roll. `WorkoutGame3DGeometry`
  extrudes it into a bounded immutable tile instead of placing a model over the
  ordinary trail.
- `WorkoutGame3DTerrainProfile` generates the deterministic eight-vertex
  course-space cross-section shared by forest geometry and grounded prop
  anchors. Global course distance and seed make independently streamed chunks
  meet without mutable state.
- `WorkoutGame3DViewModel` converts a visual snapshot to scene transforms and
  bounded nearby-prop lists. It has no trainer, recording or athlete-storage
  dependencies.
- `WorkoutGame3DWindow` owns the Qt Quick view, presentation-driven
  interpolation and QML
  context. It does not mutate game rules.
- `WorkoutGame3D.qml` owns the camera, materials, lights, 3D model hierarchy
  and two-dimensional telemetry overlay.

Course files remain `.gcmtb`/CRS-derived semantic documents. glTF 2.0 binary
(`.glb`) is the interchange format for authored visual tiles, props and the
rider. A GLB must never become the source of workout targets, collision rules
or feature success criteria.

## Coordinates

The shared world uses metres, Y up. `WorkoutGameRoadConnector::xMeters` maps to
X, `elevationMeters` maps to Y and `zMeters` maps to Z. Heading zero points
toward positive Z. The trail geometry samples the same
`WorkoutGameRoadCourseBuilder::sample()` function used to place the rider and
features, preventing independently estimated paths from drifting apart.
Each sample exposes both connector-derived base terrain and the final physical
trail surface. The terrain profile preserves the exact trail-edge sockets,
then interpolates authored shoulders and broad forest relief from that sample.
Forest triangles and grounded props query the same profile, preventing floating
or buried bases. The trail, rider and feature physics continue to use the
authoritative surface with relief and obstacles applied.

Each authored course tile has entry and exit sockets at its local Z ends. A
socket contains position, heading, width, grade and semantic surface class.
Build-time validation rejects tiles whose sockets do not match the declared
metadata. Runtime course assembly transforms complete validated tiles; it does
not edit mesh vertices to hide mismatches.

## Frame And Thread Model

1. Existing Train code reads devices, controls resistance and records samples.
2. The simulation worker consumes the newest input at a fixed timestep and
   publishes one complete engine frame into the existing capacity-one output.
3. The GUI thread drains only the newest frame and passes it to the selected
   renderer. Stale presentation work cannot form an unbounded queue.
4. `WorkoutGame3DWindow` keeps the existing two-snapshot visual interpolator.
   Actual `frameSwapped` delivery schedules the next GUI-thread sample, so the
   display presentation loop provides pacing without an independent timer
   competing with vsync. QML changes never block for a GPU frame.
5. Qt Quick 3D owns scene graph synchronization and GPU submission. Depending
   on the platform and Qt render-loop choice, this runs on Qt's render thread
   or the GUI thread. Application code does not access render resources from
   the simulation worker.

Course mesh generation and GLB validation are cold-path work. Initial immutable
course construction remains outside an active session frame loop. During a
ride, `WorkoutGame3DChunkBuilder` calculates the six streamed terrain layers on
one lower-priority worker. Its pending request and completed result are both
capacity-one mailboxes, and only a complete generation-tagged result is
installed into inactive Quick 3D geometry on the GUI thread. Trainer control,
recording, input publication and simulation never join or wait for that worker.

## Resource Bounds

- Trail and ground geometry use at most 16,000 path samples each.
- The forest floor is a bounded 28-metre-wide, eight-vertex cross-section split
  into seven indexed strips and streamed in a
  10-metre streaming window. Only 15 metres behind and 130 metres ahead are
  resident, preventing remote course loops from intersecting the foreground.
  Chunk replacement uses two geometry buffers so the render thread never sees
  an in-use mesh cleared in place. CPU mesh formation occurs on the chunk
  worker; the GUI thread only installs completed buffers. Vertex colors separate trail seams,
  shoulders and forest without adding texture work to the frame loop.
- The frame loop changes transforms and small telemetry values; it does not
  rebuild the course mesh.
- Trees and feature props are bounded distance windows and update only when the
  rider crosses a placement bucket. At most ten two-part tree placeholders are
  resident until authored instanced vegetation replaces them.
- Root networks are project-authored procedural geometry with at most 512
  segments in the resident range. Their 40 vertices and 512 triangles per tile
  are rebuilt into one of two buffers only when the floor streaming bucket
  changes; no root vertex data is uploaded from the per-frame pose path.
- Rock gardens are project-authored procedural geometry with at most 256 stones
  in the resident range. Twelve stones produce 180 vertices and 252 triangles
  per tile in one merged draw range. That range uses the same double-buffered
  floor-bucket lifecycle and is never rebuilt by the per-frame pose path.
- Rock slabs are project-authored procedural geometry with at most 12 slabs in
  the resident range. A slab uses 147 vertices and 224 triangles in one merged
  draw range. The range follows the same double-buffered floor-bucket lifecycle;
  only rider transform and suspension-derived torso response change per frame.
- Skinnies are project-authored procedural geometry with at most 12 tiles in the
  resident range. One tile uses 1,008 vertices and 504 triangles for spaced deck
  boards, beams, supports, ground infill and its packed-dirt safe line. The
  canonical profile owns deck and route coordinates; one double-buffered merged
  range changes only when the floor streaming bucket changes. Per-frame work is
  limited to rider route, height and deterministic balance roll transforms.
- Textures use mipmaps and explicit size budgets. Repeated props use instancing
  once authored assets replace the initial primitives.
- Shadows and expensive post-processing default off on the low-power profile.

## Asset Pipeline

1. Author low-poly assets in Blender with metres, Y up and reviewed socket
   metadata.
2. Export glTF 2.0 binary with embedded buffers and textures.
3. Validate the GLB structure, bounds, triangle/material counts, texture sizes,
   node names, license manifest and socket metadata in CI.
4. Import trusted built-in assets at build time where possible. Runtime loading
   is restricted to reviewed built-in resources; untrusted downloaded model
   files are not loaded into the application process.
5. Render a deterministic catalog course from fixed viewpoints and compare
   images before release.

## Acceptance And Legacy Retirement

The legacy rendering files are removed only after all of the following pass:

1. The same feature-lab and saved `test3` course complete in both renderers
   without changing distance, feature outcomes, recording or trainer targets.
2. Real X11/OpenGL captures prove a visible trail, rider, terrain relief,
   features and HUD at desktop and laptop resolutions.
3. A recorded run has no backward world movement, unexplained pause or rider
   clearance mismatch in diagnostics.
4. Frame-time p95 meets the target laptop budget and the render loop does not
   increase simulation skipped ticks.
5. AppImage verification confirms all required Qt Quick 3D libraries, QML
   modules, plugins, licenses and SBOM entries are present.
6. A user A/B test confirms the new fixed chase view is at least as readable as
   the legacy view.

After that gate, remove `WorkoutGameSceneGraphWindow`, `WorkoutGameOpenGLCanvas`,
`WorkoutGameCanvas`, `WorkoutGameRendererPolicy`, their visual-only projection
helpers, old raster assets and renderer-specific tests. Keep renderer-neutral
course, physics, feature, diagnostics and interpolation tests.
