# Workout Game Asset License Policy

## Status And Scope

This is a conservative engineering policy, not legal advice. It covers models,
textures, materials, HDRIs, sounds, generated derivatives and editable source
files distributed in the GoldenCheetah source tree or AppImage.

The default is a project-authored or verified CC0 asset. Approval always
applies to one exact asset, creator, revision and download. A marketplace name,
free price or download button is not evidence of redistribution rights.

Qt Quick 3D is available to open-source users under GPLv3. GoldenCheetah code
is GPLv2-or-later, so every distributed Quick 3D build and bundled component
must remain compatible with distribution under GPLv3:

- https://doc.qt.io/qt-6/licensing.html
- https://doc.qt.io/qt-6/qtquick3d-index.html

## License Decisions

### Allow

- Project-authored assets contributed under a reviewed compatible agreement.
- `CC0-1.0` assets with reliable creator, source and revision provenance.
- Public-domain assets with equivalent documented provenance.

Keep the original license and provenance even when attribution is not legally
required.

### Conditional

- `CC-BY-4.0` only with complete attribution, canonical license link, change
  notice and confirmation that no DRM or platform term restricts source and
  AppImage redistribution. The asset remains CC-BY; it is not relabelled GPL.
- Custom/permissive licenses only after explicit review of modification,
  redistribution, source-repository and binary-bundle rights.

### Legal Review Required

- CC-BY-SA, OGA-BY and artwork under GPL/LGPL.
- Custom or dual licenses whose asset/binary treatment is ambiguous.
- Recognisable trademarks, branded products, identifiable people, protected
  architecture or incorporated third-party artwork.

CC-BY-SA 4.0 has one-way compatibility to GPLv3, not GPLv2:
https://creativecommons.org/2015/10/08/cc-by-sa-4-0-now-one-way-compatible-with-gplv3/

### Reject

- Non-commercial, no-derivatives, editorial-only or personal-use terms.
- A restriction against extractable, standalone or source redistribution.
- A marketplace-only right that cannot accompany a public source/AppImage.
- “Free download”, unclear/custom terms, missing creator or missing provenance.
- AI-generated work without documented model, input and output rights.

CC0 does not waive trademark, privacy or publicity rights:
https://creativecommons.org/publicdomain/zero/1.0/

## Provider Policy

| Provider | Decision | Conditions |
| --- | --- | --- |
| Poly Haven | Allow | Downloaded models, textures and HDRIs are CC0. Do not copy logos, site text or user/preview renders. https://polyhaven.com/license |
| Kenney | Allow | Asset-page game assets are CC0. Keep included license; do not use the Kenney logo. https://kenney.nl/support |
| Quaternius | Allow | Models are CC0 and may be modified and redistributed. https://quaternius.com/faq.html |
| ambientCG | Allow | Downloadable assets and material previews are CC0, including raw-file redistribution. https://docs.ambientcg.com/license/ |
| BlenderKit | CC0 only | Reject Royalty Free assets because public source/AppImages are extractable. https://www.blenderkit.com/docs/licenses/licensing-faq/ |
| Sketchfab | Per asset | CC0 allowed; CC-BY conditional. Reject Standard and Editorial. https://sketchfab.com/licenses |
| OpenGameArt | Per asset | CC0 allowed; CC-BY conditional. Other licenses require review. https://opengameart.org/content/faq |
| Khronos glTF samples | Per model | No blanket asset license; use each model README and reject ownership/marking issues. https://github.com/KhronosGroup/glTF-Sample-Assets |

Bundled art and GPL code may sometimes be mere aggregation, but this is
fact-dependent and must not be used to justify a questionable license:
https://www.gnu.org/licenses/old-licenses/gpl-2.0-faq.en.html

## Acquisition And Provenance

1. Download only from an official asset page or authenticated official API.
2. Archive provider metadata and exact license text at acquisition time.
3. Record creator, provider, page URL, download URL, provider revision and date.
4. Hash the untouched archive and preferred editable source with SHA-256.
5. Keep original and generated files separate.
6. Record every modification, author, date, tool version and conversion command.
7. Hash every generated GLB, texture, QML and `.mesh` file.
8. Record permitted distribution in source, AppImage and screenshots/video.
9. Never import from image search, mirrors, social media or unknown copies.
10. Do not assume a current license applies to an older unverified download.

Each candidate and approved asset must validate against
`workout_game_asset_manifest.schema.json`.

## Automated Release Gates

- Every distributed runtime/source asset has exactly one approved manifest.
- CI rejects unknown, NC, ND, editorial, personal-use and unapproved licenses.
- CI rejects missing attribution, license text, source revision or hash mismatch.
- CI generates `THIRD_PARTY_ASSETS.md` and an in-application attribution view.
- AppImage extraction tests verify both the assets and notices are present.
- The Khronos glTF Validator must report no errors:
  https://github.com/KhronosGroup/glTF-Validator
- GLB parsing rejects non-glTF-2.0 data, external/network/file URIs, unexpected
  cameras/lights and non-allowlisted required extensions.
- Archive import rejects absolute paths, traversal entries, symlinks and
  excessive expanded size.
- CI enforces node, triangle, material, texture-size and decoded-memory budgets.
- Generated assets are rebuilt and compared by hash or canonical structure.

## Qt Quick 3D Import Policy

GLB is the authored interchange format, not the preferred production runtime
format. Trusted bundled assets are converted offline with Qt's Balsam tool to
QML and optimized `.mesh` files:

- https://doc.qt.io/qt-6/qtquick3d-tool-balsam.html
- https://doc.qt.io/qt-6/quick3d-asset-intro.html

`RuntimeLoader` supports glTF 2.0 `.gltf`/`.glb`, but Qt documents it as less
efficient than build-time conversion and warns that malformed content is not
sandboxed. It is permitted only for trusted development previews, never for
arbitrary downloaded content:
https://doc.qt.io/qt-6/qml-qtquick3d-assetutils-runtimeloader.html

Start with glTF 2.0 core features. Draco, Meshopt, KTX2/BasisU and material
extensions require explicit target Qt/AppImage tests before admission. Prefer
opaque materials. Use GPU instancing for repeated trees/rocks after measuring
the vertical slice.

Packaging an asset into qrc, `.mesh`, GLB or AppImage does not make the content
non-extractable for license purposes.
