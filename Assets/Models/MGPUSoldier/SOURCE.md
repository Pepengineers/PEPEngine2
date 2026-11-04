# MGPU soldier

Converted from the user's existing `D:/Project/Multi-GPU-Render-DX12` checkout:
`MGPU-Animations/Models/soldier.m3d` and its eight referenced diffuse/normal DDS
textures under `MGPU-Animations/Textures`.

The original sample's `SkinnedMeshApp.cpp` carries this attribution:
`SkinnedMeshApp.cpp by Frank Luna (C) 2015 All Rights Reserved.`
No separate model or texture license was found in that sample. This record retains
the existing provenance; it does not grant or claim additional distribution rights.

`Soldier.gltf`, `Soldier.bin` and the PNGs are reproducible conversions. Geometry,
five material sections, 58-bone hierarchy, skin weights, inverse binds and the
original `Take1` animation are retained. DDS BGRA32 pixels are converted losslessly
to PNG without an added dependency. Source SHA-256 hashes are recorded in
`source-sha256.json`.

From the PEPEngine2 root:

```powershell
python tools/convert-mgpu-soldier.py D:/Project/Multi-GPU-Render-DX12/MGPU-Animations Assets/Models/MGPUSoldier
```

The converter validates the source's expected counts, subset indices, normalized
skin weights, bone hierarchy, affine inverse binds and changing animation. The
application imports the resulting standard glTF through its existing Assimp path.
The M3D triangle order is clockwise relative to its authored normals. Conversion
reverses each triangle to match the glTF/engine counter-clockwise front-face
convention; copying the original order renders the soldier's interior surfaces.
`python tools/tests/test_mgpu_soldier.py` checks every material's front faces
against those normals and fails on the previously inverted asset. A CPU import
through the actual Assimp engine flags also confirmed the original UVs and all
five diffuse/normal material slots remain intact.
Source units are preserved; the Bistro App applies a positive 2.4 scale for an
approximately 179 cm human, with a 4.6 cm source-foot offset. The source is already
right-handed, matching the engine's scene importer; no reflected runtime transform
or separate skeletal pipeline is introduced.

Bistro route calibration samples the actual OBJ triangles. The human capsule
`x=[-1090,-970], z=[-630,-270]` stays on `Paris_Street_4` pavement at y=23.162 cm;
the entity's y=27.762 cm includes the source-foot lift. The fox's foyer capsule
`x=[80,480], z=[-130,-30]` stays on the interior hexagonal floor at y=39.622 cm;
its y=39.722 cm includes the fox's source-foot lift. Additional instances share
these verified routes at distinct stable phases. Source inverse-bind ankle/toe
positions confirm bind-pose forward is local +Z. `Take1` rotates the body root
approximately 171 degrees about Y, so the animated soldier faces approximately
local -Z. The App adds 180 degrees to human route yaw to keep the animated body
facing its movement; the fox retains its original route yaw. The imported-pose
regression compares the animated body forward with the actual route displacement.
The human route was checked at 16 evenly spaced positions and at each position's
four cardinal offsets of 25 cm: all 80 samples hit only that flat pavement, with
no higher curb or foliage layer. All 16 fox route centers hit the stated floor.
