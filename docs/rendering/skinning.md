# Skeletal mesh rendering

Актуальная интеграция: код объединён в корневую ветку и прошёл CPU/GPU/GBV smoke checks. [Результаты и непроверенные области](second-group-integration.md). Авторский статус ниже — исторический, до центральной интеграции.


UNVERIFIED implementation, 2026-11-01. Author instructions prohibit builds,
tests, GPU fixtures, benchmarks, and separate verification/review passes.
This document describes the added code and the future central validation
boundary; it does not claim successful execution.

## Component and global controls

Attach SkeletalMeshRenderComponent and TransformComponent to the same existing
entity. Each component renders every mesh attachment in its SceneAsset,
including rigid attachments, without creating child entities or fake handles.
Multiple components have separate clocks, poses, materials, palettes, vertex
slices and instance records. StaticMeshRenderComponent is not required; adding
both components intentionally renders both sets of geometry.

Only two global CVar exist. Both are Default (runtime writable through
ConsoleModule and startup Config/Engine.ini):

| Control | Default | Meaning |
| --- | --- | --- |
| Render.Skinning.Enabled | true | false freezes the last displayed deformation |
| Render.Skinning.Quality | 2 | 0 Off, 1 Low: 15 Hz, 2 Mid: 30 Hz, 3 High: 60 Hz |

The component owns all remaining settings:

| API | Default / accepted value |
| --- | --- |
| SetScene(SceneHandle) | invalid; no draws/resources until a loaded scene is assigned |
| SetClip(int) | -1 exact bind pose; otherwise a valid imported clip index |
| SetTimeSeconds(double) | 0; any finite seconds, explicit seek resets deformation velocity |
| SetPlaybackSpeed(double) | 1; finite [-100,100], including reverse and zero |
| SetFrozen(bool) | false; hold clock and displayed pose |
| SetLooping(bool) | true; wrap negative/positive time; false clamps to clip duration |
| SetUpdateRateHz(double) | 0 inherits quality; explicit (0,240] Hz persists across presets |
| SetLocalPose(vector<Matrix>) | empty: clip/bind; otherwise one local affine matrix per hierarchy node |
| SetMaterials(vector<GDX12Material*>) | empty: imported slots; otherwise non-null renderer-owned slots indexed by source material |

Corresponding getters expose settings and current playback time. SetLocalPose
replaces the complete local pose; empty restores animation. Scene replacement
does not silently erase clip/pose/material overrides: incompatible indices or
pose sizes fail validation. Parameter edits apply before the next render;
call component APIs on the world/render owner thread, outside ECS jobs.

The clock advances by frame delta * world time scale * component playback
speed only while globally active, unfrozen and unpaused. A newly created
component evaluates its requested initial pose even with the global technique
off, so it can render a coherent initial state. Existing components hold the
last displayed pose while off; edits are retained and evaluated on re-enable.
World pause also holds playback. No quality switch overwrites author settings.

Pose samples are held until their update budget expires; this is actual CPU
evaluation and compute dispatch throttling, not an influence/joint truncation.
A changed pose writes current and last-displayed vertices together. A following
held frame dispatches once to settle previous=current; subsequent held frames
do no compute work. Off/freeze/seek/reload and camera history resets suppress
stale deformation motion. The first changed frame has deformation velocity;
held frames have only any real entity/camera motion.

## Asset API

Use the existing AssetManager/SceneRegistry/MeshRegistry, not ImportSingleMeshAsset
(the latter bakes scene-node transforms for static geometry):

```cpp
auto& assets = Engine::Core::AssetManager::GetInstance();
const std::filesystem::path path = /* absolute model path */;
const auto* scene = assets.LoadScene(path);
if (!scene) throw std::runtime_error("Scene load failed");
const auto handle = assets.Scenes().FindHandle(path);
// On an entity chosen by the caller:
entity.AddComponent<TransformComponent>();
auto& skin = entity.AddComponent<SkeletalMeshRenderComponent>(handle);
skin.SetClip(0); // select by SceneAsset::GetClips()[i].Name
skin.SetPlaybackSpeed(1.0);
skin.SetUpdateRateHz(0); // inherit the global quality preset
```

No caller above was added to Bistro. The pinned Fox payload and attribution
are at Assets/Models/KhronosFox; its SOURCE.md records actual source transforms
and position bounds. All importer coordinates remain source numeric units;
entity scale 1 interprets them as engine centimeters. RH matrices and source
hierarchy transforms are preserved, without an automatic meter-to-cm factor.

SceneAsset imports all hierarchy nodes in parent-first order, exact local bind
matrices, decomposed bind TRS for partial tracks, and named clips with seconds.
CPU evaluation uses linear vector interpolation, shortest-path quaternion
slerp, clamped key endpoints, and bind TRS for missing channels. Nodes without
animation retain exact bind matrices, including shear. Animation tick rates
of zero use Assimp's conventional 25 ticks/sec fallback. Mesh/morph animation
channels are explicitly rejected; this implementation is skeletal LBS.

Each local geometry subasset retains its original material indices, all bones'
own inverse binds, variable per-vertex influence ranges and normalized positive
weights. No four-influence or 96-bone ceiling exists. A vertex with no positive
weights follows its attachment node. Bone/track names must resolve uniquely
within the hierarchy; missing or ambiguous targets fail import. Geometry,
influence ranges, indices, times, finite matrices, and palette sizes are
validated at their consuming boundaries. Singular blended linear transforms
use a finite normal fallback rather than divide by zero.

Registry GetContentRevision changes when data is cached or unloaded, even
when Unload keeps the same handle/generation. Renderer state re-resolves scene
and mesh handles every frame and compares content revisions. Unloaded inputs
remove their draws; explicit reload recreates state from the actual registry.
No SceneAsset/Mesh/SubMesh pointer is retained by skeletal runtime state.
GPUMesh's existing CPU metadata is refreshed when a changed mesh is submitted.
Source geometry ranges and dynamic slices are reused on registry-slot reload;
vertex-buffer growth preserves live GPU-deformed slices.

Materials reuse WorldLoader::BuildMaterialSlots and the existing texture/PBR
pipeline. It resolves real imported texture paths, neutral default textures,
source colors and material classification. Runtime material overrides are
renderer-owned, as with StaticMeshRenderComponent. This path uses external
texture files; embedded-texture extraction remains the existing importer's
limitation. The supplied Fox glTF uses an external Texture.png.

## Renderer seams and resource lifetime

- Core owners: Types/SceneTypes, Types/MeshTypes, IO/SceneImporter,
  IO/MeshImporter, Registries/TypedAssetRegistry.
- ECS/API: SkeletalMeshRenderComponent is registered in WorldECS.
  RenderModuleSkinning prepares component state before CollectReflectionScene.
  Existing transform jobs still produce World/PrevWorld for the same entity.
  Material uploads occur once in OnRender after component preparation, so lazy
  material creation cannot decrement frame-dirty counts twice in the same slot.
- GPU: GDX12Skinning lazily creates a compute PSO only with mesh consumers.
  Per-attachment source/range/influence uploads are immutable; palettes have
  one mapped upload per frame slot. OnUpdate waits that slot's fence before
  uploads are written. Vertex outputs use non-overlapping slices of the
  existing GDX12GeometryBuffer, with explicit UAV -> IA transitions.
- Compute runs on the single direct queue before culling and raster passes.
  Current/previous outputs are written in one dispatch. Queue ordering protects
  the common geometry buffer across queued frames; growth/cancellation/rebuild
  flush before resource destruction or shared indirect-template mutation.
  Component removal/replacement and world unsubscribe retire buffers and zero
  their shared indirect commands before recycling slices/instance indices.
- Vertex ABI now includes PreviousPosition (56-byte Vertex, offset 44).
  GeometryBuffer initializes static previous positions to current positions.
  The default tangent element uses offsetof(Vertex,Tangent), correcting its
  previous offset 36 to the actual offset 32.
- The same current IA geometry reaches depth-only DepthVS/DepthPS and early-AO
  prepass VS/PrepassPS, main G-buffer, CSM, local shadows, reflection capture and
  its shadows, and WBOIT. Masked alpha tests and G-buffer Equal/no-depth-write
  behavior remain in their existing passes. G-buffer motion uses previous
  deformed position * PrevWorld and prevNDC-currentNDC without raster jitter.
- Inverse-transpose of the blended LBS linear matrix transforms normals;
  tangents use its linear transform and orthogonalization. Entity normal
  transformation remains the existing PBR inverse-transpose path.
- Culling bounds union transformed source AABBs for every bone plus the rigid
  attachment, then apply the entity world transform. Positive normalized LBS
  is within this conservative box. Skeletal instances are reflection-dynamic;
  poses, entity transforms, source revisions and active materials invalidate
  dynamic probes. Existing static-only probe rules continue to exclude them.
- No HDR pass is added or reordered: deferred -> SSSR/denoiser and unified
  local-probe specular resolve -> WBOIT -> DoF -> motion blur -> HDR XeSS ->
  one tone mapping. No specular contribution is added by skinning.

## Deferred central validation and limits

No build, CPU/GPU tests, benchmarks, separate review or verification pass was
run. Existing tests were not expanded and no verification infrastructure or
demo scene was created. The central checker still needs actual Assimp/Fox
import/units, CPU bind/interpolation oracle, >4 influences, >96 joints,
multi-mesh and rigid zero-weight attachments, nonuniform/singular transforms,
two independently configured components, unload/reload/reuse, all prepass/
shadow/probe/motion paths, Off/freeze/seek/cuts, resize/frame-slot transitions,
debug-layer checks and Low/Mid/High workload timings.

LBS can collapse joints; there is no dual-quaternion skinning, morph targets,
clip blending, root-motion extraction or GPU child/subemitter work. Key
interpolation is the linear/slerp data exposed by the current Assimp path.
Bounds are conservative full-source boxes per bone, not tight per-bone
vertex bounds. Topology/source changes use a queue flush; ordinary playback
does not. Embedded texture extraction and multi-GPU are outside this change.
WBOIT still has the base renderer's single opaque depth/velocity representation:
transparent skinned surfaces render their current pose but do not add a separate
transparent motion layer. No successful build, image quality, timing or long-run
stability claim is made.
