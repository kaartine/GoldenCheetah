# TripoSR Blender Cleanup

This directory contains an optional Blender 4.0.2 cleanup stage for a
quarantined TripoSR review mesh. It does not install or rig the candidate and
does not replace any Workout Game runtime asset.

Both the raw GLB and the output directory must resolve outside the repository:

```bash
blender --background --factory-startup \
  --python contrib/workout-game-assets/triposr/blender/cleanup_triposr_glb.py -- \
  --input /private/triposr/raw.glb \
  --output-dir /private/triposr/cleaned \
  --up +Y --forward +Z --scale 1.0
```

`--up` and `--forward` describe the raw glTF coordinate system before
Blender's glTF import conversion. Output uses the GoldenCheetah canonical
orientation in glTF form: `+Y` up, `+Z` forward and `+X` right. The mesh is
centered horizontally, grounded, and exported with identity transforms.

The fixed outputs are:

- `candidate-lod0.glb`, at most 18,000 triangles.
- `candidate-lod1.glb`, at most 6,000 triangles and 40 percent of realized
  LOD0.
- `candidate-collision.glb`, a 12-triangle review proxy with no physics
  authority.
- `cleanup-report.json`, with input hash, removals, budgets and output hashes.

The cleanup writes a sanitized temporary GLB before Blender import. Cameras,
lights, animations, skins, morph targets, extensions, images, textures,
untrusted materials and metadata extras are removed. External geometry buffers
are rejected instead of read. Imported meshes are baked into canonical space,
welded, triangulated and split into loose parts. Only fragments that satisfy
all configured triangle, area and extent limits are removed. LOD reduction is
then performed per loose part before the result is joined, which protects
small silhouette components better than one global decimation pass.

The result remains a monolithic review mesh. It is not the articulated RB-01
runtime rig, and its collision file is not trainer or gameplay authority.

After importing and verifying a candidate descriptor, render it against the
same four fixed rider-bike audit views without copying it into the repository:

```bash
blender --background --factory-startup \
  --python contrib/workout-game-assets/triposr/blender/render_candidate_audit.py -- \
  --candidate-directory /private/candidates/bike-side-001 \
  --output-dir /private/audits/bike-side-001
```

Run the Blender smoke test with the pinned 4.0.2 executable:

```bash
BLENDER=/opt/blender-4.0.2/blender \
  contrib/workout-game-assets/triposr/blender/tests/run_smoke.sh
```

The test creates an external synthetic GLB containing cameras, a light,
animation, skin, morph target and packed image material. It runs cleanup twice,
checks byte-identical outputs, enforces budgets and identity transforms, and
verifies that repository-local input and output paths are rejected.
