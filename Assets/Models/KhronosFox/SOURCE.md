# Khronos Fox

Unmodified files from KhronosGroup/glTF-Sample-Assets, pinned at
`4995a638c96dbc94376d3d6164e70fee3ac7f7a2`:
https://github.com/KhronosGroup/glTF-Sample-Assets/tree/4995a638c96dbc94376d3d6164e70fee3ac7f7a2/Models/Fox

Payload: glTF/Fox.gltf, glTF/Fox.bin and glTF/Texture.png.
Upstream LICENSE.md, metadata.json, README.md and README.body.md are retained.
The local SOURCE.md and .gitattributes are packaging additions.

Attribution: model by PixelMannen (2014), CC0-1.0; rig and animation by
tomkranis (2014), CC-BY-4.0; glTF conversion by @AsoboStudio and @scurest
(2017), CC-BY-4.0. See LICENSE.md and metadata.json for original license URLs,
copyright owners, and provenance. The payload has not been modified.

The glTF contains 26 nodes, 24 skin joints and Survey/Walk/Run clips.
Root, fox and _rootJoint have no explicit scale/matrix; b_Root_00 carries
an approximately -90-degree X rotation. POSITION accessor 0 records min
(-12.592718, -0.121745, -88.095001), max (12.592718, 78.907188, 66.624863).
These are source numbers, not a runtime import or GPU measurement.
The engine preserves these numbers and node transforms: identity entity
scale interprets them as engine centimeters (roughly 79 cm tall).
It intentionally does not multiply this asset by 100. glTF's nominal meter
convention is not applied as an automatic importer conversion; callers choose
a different unit policy through their entity TransformComponent.Scale.

This asset is available for later isolated validation only. It is not loaded
by Bistro or any new demo scene. No import executable, CPU oracle, GPU fixture,
build or benchmark was run by the author.
