# Workout Game Asset Pipeline

## Purpose

Workout Game uses deterministic trail geometry for simulation, collision,
connectors, visibility, and tests. Generated images or meshes are development
inputs only. They must not become runtime dependencies or replace the canonical
trail profile.

## First reference feature

The first calibrated feature is a small, rollable tabletop based on the New
Zealand Mountain Bike Trail Design Guidelines. The Grade 2 envelope limits the
ramp height to 0.5 m, requires a ramp at least three times its height, and uses
a 1-3 m tabletop. The game baseline is:

| Property | Baseline |
| --- | ---: |
| Ramp height | 0.45 m at difficulty 0.7 |
| Takeoff angle | approximately 15 degrees |
| Takeoff run | approximately 1.87 m |
| Flat deck | approximately 1.10 m |
| Landing run | approximately 1.87 m |
| Total core length | approximately 4.84 m |
| Intended speed | 20 km/h |

The prior model was 9.4 m long and 1.03 m high at the same difficulty. It read
as a wall from the chase camera and did not represent the selected trail grade.

Sources:

- https://www.nzrecreation.org.nz/new-zealand-mountain-bike-trail-design-guidelines
- https://sportnz.org.nz/media/3j3pmk0y/new-zealand-mountain-bike-trail-design-guidelines.pdf

## Roller reference contract

The production roller tile follows the same New Zealand Grade 2 guidance. It
uses three elongated, fully rollable crests below `300 mm`, with spacing close
to ten times their height. This is continuous trail, so failure affects flow
scoring but must not create a side bypass.

| Property | Contract |
| --- | ---: |
| Total socket-to-socket length | 10.50 m |
| Level entry and exit | 0.75 m each |
| Active section | 9.00 m |
| Crest count and spacing | 3 at 3.00 m |
| Height | `0.20 + 0.08 * difficulty` m |
| Trail half-width | 0.68 m |
| Canonical profile | `h/2 * (1 - cos(2*pi*s/3))` |
| Continuous-contact acceptance | 3.33, 5.0 and 7.0 m/s |

The C++ profile is the sole trail-height authority for road generation, Box2D
and Quick 3D. The visible mesh is generated directly from that profile, so a
separate GLB would duplicate the tread and is intentionally not used. Rider
pump pose is tied to distance through the same profile rather than wall-clock
animation time.

## Production workflow

1. Select an owned, generated, or clearly licensed reference.
2. Record measurable dimensions and gameplay intent in a canonical profile.
3. Generate the low-poly mesh from committed C++ or a committed Blender Python
   or Geometry Nodes template.
4. Preserve identical entry and exit sockets for every trail tile.
5. Optionally generate texture concepts from canonical depth, mask, and outline
   renders. Human review selects the final result.
6. Bake approved color and ambient-occlusion detail into a small atlas.
7. Validate geometry, texture provenance, connector continuity, and target-GPU
   performance before importing the asset.

Blender is the preferred offline asset compiler. Its generated artwork is not
subject to Blender's GPL, and its scripting and Geometry Nodes interfaces keep
the process repeatable:

- https://docs.blender.org/manual/en/latest/modeling/geometry_nodes/introduction.html
- https://docs.blender.org/manual/en/latest/render/cycles/baking.html
- https://www.blender.org/about/license/

AI-assisted options are deliberately limited to offline authoring:

- ControlNet can preserve canonical silhouettes and depth while proposing
  texture variants: https://github.com/lllyasviel/ControlNet
- Segment Anything can separate approved concepts into atlas layers:
  https://github.com/facebookresearch/segment-anything
- Depth Anything V2 Small can help split owned forest images into distant 2.5D
  layers. It must not define physical trail geometry:
  https://github.com/DepthAnything/Depth-Anything-V2

Every accepted generated asset records the source image, prompt, model name,
model hash, model license, output license, and cleanup steps. A model's code
license does not automatically grant rights to its checkpoint or training
output.

## Runtime constraints

- Trainer control and recording remain independent of rendering.
- Prefer opaque, unlit, batched geometry and one primary texture atlas.
- Start below 30,000 visible triangles and 50 draw calls, then measure on the
  target Intel GPU.
- Keep the 60 Hz frame budget below 16.7 ms without increasing simulation or
  Bluetooth latency.
- Use deterministic world positions and identities for props.
- Clip partially occluded props instead of switching the whole object on.
- Activate far props before they reach the viewport and retire near props only
  after their bounds have left an expanded viewport.

Qt scene graph timing and batch diagnostics remain the runtime source of truth:

- https://doc.qt.io/qt-6.8/qtquick-visualcanvas-scenegraph-renderer.html
- https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html

## Automated acceptance

- Shared connector vertices compare exactly before projection.
- The main trail surface covers the full feature tile interval with no gaps.
- Only one canonical trail bed owns each interval; raised feature geometry may
  overlay it but may not replace the surrounding ground mask.
- No background pixel is visible between projected terrain and the viewport
  bottom during a camera sweep.
- A partially occluded tree is polygon-clipped and emerges progressively.
- Tree bases match their projected forest-floor height.
- At the action cue, takeoff, deck, and landing remain separately readable.
- Reference captures are produced at approach, action, airborne, landing, and
  bypass phases on a real Qt/OpenGL scene graph.
