# Meta Bistro local shadows prepared by CPU jobs

Ordinary Bistro now preserves authored point/spot shadow flags and prepares
their draw lists through JobSystem. Other deferred effects remain outside this
migration step. Temporal shadow-map caching remains deferred by agreement.

`SceneLocalShadows` builds a balanced BVH from the original scene indirect
templates and world-space instance bounds. It excludes objects that do not cast
shadows, blended materials and probe previews. Scene-buffer revision changes
invalidate the tree. Camera-visible draw lists are not used as caster input.

One parallel job item per light first rejects lights whose range AABB misses
the camera frustum. It traverses the BVH using range and face-frustum tests and
produces one list for a spot or six lists for a point light. A dependent packing
job assigns contiguous depth-array slices and packs all draw commands. Job
scheduling and completion stay on the JobSystem owner thread; workers only
prepare data. Sun preparation overlaps the local jobs before their completion.

Each frame slot owns its upload/default command buffers, effective light data
and depth array. One packed command copy feeds ExecuteIndirect with CPU-known
counts. There is no GPU culling pass, count readback, depth packing or queue
flush in this path. The existing `LocalShadowPass.hlsl`, depth bias and lighting
filtering are reused. Selected maps are cleared and redrawn every frame.
Light data uploads to VRAM only when the effective snapshot changes; commands
are uploaded each frame. This path therefore still has CPU-to-GPU traffic.

The initial local resolution is 256 texels per face, matching App's default.
`Render.Shadows.Local.Resolution=0` disables it. Slice storage grows per frame
slot as needed and is reused, without reusing old map contents. Selection is
conservative: it can retain extra lights and casters, and includes casters
outside the camera. There is no occlusion culling or per-face camera rejection.

Validation: Release x64 build passed. SDK DXC compiled the unbatched local-shadow
vertex shader and opaque deferred-lighting compute variant. Source review found
no issues; `git diff --check` passed. No new tests, automatic
runtime checks, captures or performance measurements were run, as requested.
The ordinary application was launched for the user's visual and performance check;
CPU-bound behavior, GPU utilization and runtime correctness remain unverified.
