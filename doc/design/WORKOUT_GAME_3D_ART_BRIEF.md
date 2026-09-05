# Workout Game 3D Art Brief

## Direction

Workout Game uses a readable low-poly forest with restrained pixel-textured
surfaces. It is intentionally arcade-like rather than photorealistic. The
singletrack, rider, next feature and workout feedback must remain the visual
hierarchy in that order; scenery may establish depth but must not obscure
those elements.

## Palette And Materials

- Forest floor uses muted mid greens, dirt uses warm low-saturation ochres,
  stone uses neutral cool greys and wood uses subdued browns.
- The rider and bike use a separate high-contrast accent ramp so their
  silhouette remains visible against every terrain material.
- Surface variation is low contrast. Geometry, lighting and vertex colors
  carry large-scale shape; textures add only close-range material identity.
- Materials are opaque and rough. Metallic or glossy surfaces are reserved
  for a later measured effect with a clear gameplay purpose.
- Features reuse the same dirt, stone and wood source tiles as ordinary trail
  pieces. A feature must look joined to the trail, not pasted over it.

## Texture Contract

`contrib/workout-game-assets/generate_surface_atlas.py` is the authoritative,
deterministic source for the shared 96 by 64 atlas and its five 32 by 32
runtime tiles. The tiles contain four-pixel blocks and use no external image
input. Runtime uses repeat wrapping, nearest magnification, linear minification
and mipmaps. This keeps close pixels crisp while suppressing distant shimmer.

The current atlas is limited to 16 KiB, four source materials and no runtime
generation or file-system lookup. Changes must reproduce byte for byte, pass
the asset manifest tests, render through the production qrc and receive a
real OpenGL still and motion review.

## Geometry And Composition

- Trail width remains 1.36 m and its edges are continuously socketed.
- Terrain and trees use authored asymmetric silhouettes and terrain-anchored
  bases. Repetition must be broken without unbounded object counts.
- A foreground tree anchor may contain merged low saplings inside its tested
  crown-clearance radius. It must still render as only one trunk and one crown
  model so near-ground density does not multiply draw calls.
- The approved medium-centre camera keeps the rider low and central while
  showing the next 25-40 m. Vegetation must respect its exclusion corridor.
- Fog, particles and shadows must remain restrained and bounded. They may add
  depth or event punctuation but may not hide feature anatomy or HUD cues.

## Performance Budget

The scene stays below 30,000 visible custom triangles and 50 actual draw calls.
Textures are packaged resources prepared offline. Rendering and visual effects
must never add work to trainer control, recording or the authoritative physics
step.
