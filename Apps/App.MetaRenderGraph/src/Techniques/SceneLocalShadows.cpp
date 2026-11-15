#include "SceneLocalShadows.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstring>

namespace Meta
{
namespace
{
float Distance(const XMFLOAT4& plane, const XMFLOAT3& point)
{
    return plane.x * point.x + plane.y * point.y + plane.z * point.z + plane.w;
}

std::optional<BoundingBox> VisibleBounds(const BoundingBox& box, const std::array<XMFLOAT4, 6>& planes,
    const std::array<XMFLOAT3, 8>& cameraCorners)
{
    bool contained = true;
    for (const auto& p : planes)
    {
        const float distance = Distance(p, box.Center);
        const float radius = std::abs(p.x) * box.Extents.x + std::abs(p.y) * box.Extents.y + std::abs(p.z) * box.Extents.z;
        if (distance + radius < 0) return std::nullopt;
        contained &= distance >= radius;
    }
    if (contained) return box;
    const Vector3 lo = Vector3(box.Center) - Vector3(box.Extents), hi = Vector3(box.Center) + Vector3(box.Extents);
    const std::array<XMFLOAT4, 6> boxPlanes{{{1, 0, 0, -lo.x}, {-1, 0, 0, hi.x},
        {0, 1, 0, -lo.y}, {0, -1, 0, hi.y}, {0, 0, 1, -lo.z}, {0, 0, -1, hi.z}}};
    std::array<XMFLOAT3, 8> corners;
    for (UINT i = 0; i < 8; ++i) corners[i] = {i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z};
    Vector3 minimum(FLT_MAX, FLT_MAX, FLT_MAX), maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    bool found = false;
    const auto clip = [&](const XMFLOAT3& a, const XMFLOAT3& b, const auto& clippingPlanes) {
        float first = 0, last = 1;
        for (const auto& plane : clippingPlanes)
        {
            const float da = Distance(plane, a), db = Distance(plane, b);
            if (da < 0 && db < 0) return;
            if (da < 0) first = (std::max)(first, da / (da - db));
            if (db < 0) last = (std::min)(last, da / (da - db));
            if (first > last) return;
        }
        for (float t : {first, last})
        {
            const Vector3 point = Vector3(a) + (Vector3(b) - Vector3(a)) * t;
            minimum = Vector3::Min(minimum, point);
            maximum = Vector3::Max(maximum, point);
            found = true;
        }
    };
    // Intersection vertices lie on box edges, camera edges, or their endpoints.
    for (UINT i = 0; i < 8; ++i)
        for (UINT axis = 1; axis <= 4; axis <<= 1)
            if (!(i & axis))
            {
                clip(corners[i], corners[i | axis], planes);
                clip(cameraCorners[i], cameraCorners[i | axis], boxPlanes);
            }
    if (!found) return box; // Keep uncertain numerical intersections conservative.
    BoundingBox result;
    BoundingBox::CreateFromPoints(result, minimum, maximum);
    result.Extents = Vector3(result.Extents) + Vector3(0.01f, 0.01f, 0.01f);
    return result;
}
}

SceneLocalShadows::SceneLocalShadows(GDX12DeviceResources& r, RenderPipelineCommonData& common)
    : _resources(r), _common(common), _slots(r.FrameConstants.size())
{
    GDX12RootSignatureDesc root;
    root.Constants = {2};
    root.NumSingleSRVSlots = 4;
    root.InstancedGeometry = true;
    root.StaticSamplers = GetStaticSamplers();
    root.SRVRanges.emplace_back(Texture2D_RangeLength);
    _root = std::make_unique<GDX12RootSignature>(r.Device, root);
    _signature = GDX12RenderHelpers::CreateIndexedDrawSignature(r.Device, _root.get());
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const D3D_SHADER_MACRO defines[] = {{"SHADOW_UNBATCHED", "1"}, {nullptr, nullptr}};
    auto vs = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", defines, "VS", "vs");
    auto ps = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", nullptr, "PS", "ps");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = _root->GetRootSignature().Get();
    p.InputLayout = {r.InputLayouts.at("Default").data(), UINT(r.InputLayouts.at("Default").size())};
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    p.RasterizerState.FrontCounterClockwise = TRUE;
    p.RasterizerState.DepthBias = 250;
    p.RasterizerState.SlopeScaledDepthBias = 1.5f;
    p.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    p.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    p.SampleMask = UINT_MAX;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    p.SampleDesc.Count = 1;
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_maskedDraw)));
    const D3D_SHADER_MACRO depthDefines[] = {{"SHADOW_UNBATCHED", "1"}, {"SHADOW_DEPTH_ONLY", "1"}, {nullptr, nullptr}};
    auto depthVS = compiler.CompileShader(r.Device, SHADERS_FOLDER "LocalShadowPass.hlsl", depthDefines, "VS", "vs");
    p.VS = {depthVS->GetBufferPointer(), depthVS->GetBufferSize()};
    p.PS = {};
    ThrowIfFailed(r.Device->GetDevice()->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&_opaqueDraw)));
    for (auto& frame : _slots)
    {
        frame.Cull.Owner = this;
        frame.Cull.Frame = frame.Pack.Frame = &frame;
    }
    r.IndirectCommandsCache->TrackElementChanges();
    for (auto& frame : r.FrameConstants)
    {
        frame->InstanceCache->TrackElementChanges();
        frame->TransformCache->TrackElementChanges();
        frame->MaterialCache->TrackElementChanges();
    }
}

SceneLocalShadows::~SceneLocalShadows()
{
    for (auto& frame : _slots)
        try { frame.Ready.Complete(); } catch (...) {}
}

UINT SceneLocalShadows::BuildNode(UINT first, UINT count, UINT parent)
{
    const UINT index = UINT(_nodes.size());
    _nodes.emplace_back();
    auto bounds = _casters[first].Bounds;
    for (UINT i = first + 1; i < first + count; ++i) BoundingBox::CreateMerged(bounds, bounds, _casters[i].Bounds);
    if (count <= 32)
    {
        _nodes[index] = {bounds, first, count, 0, 0, parent};
        for (UINT i = first; i < first + count; ++i) _leaves[i] = index;
    }
    else
    {
        const auto& e = bounds.Extents;
        const UINT axis = e.x >= e.y && e.x >= e.z ? 0 : e.y >= e.z ? 1 : 2;
        const UINT half = count / 2;
        const auto coordinate = [axis](const Caster& value) {
            return axis == 0 ? value.Bounds.Center.x : axis == 1 ? value.Bounds.Center.y : value.Bounds.Center.z;
        };
        std::nth_element(_casters.begin() + first, _casters.begin() + first + half, _casters.begin() + first + count,
            [&](const auto& a, const auto& b) { return coordinate(a) < coordinate(b); });
        const UINT left = BuildNode(first, half, index), right = BuildNode(first + half, count - half, index);
        _nodes[index] = {bounds, 0, 0, left, right, parent};
    }
    return index;
}

SceneLocalShadows::Input SceneLocalShadows::ReadInput(UINT slot, UINT commandIndex) const
{
    const auto& frame = *_resources.FrameConstants.at(slot);
    Input result;
    result.Command = _resources.IndirectCommandsCache->GetCPUData()[commandIndex];
    if (!result.Command.InstanceCount) return result;
    if (result.Command.InstanceID >= frame.InstanceCache->GetElementCount())
        throw std::out_of_range("Local shadow command references a missing instance");
    result.Instance = frame.InstanceCache->GetCPUData()[result.Command.InstanceID];
    if (result.Instance.MaterialIndex >= frame.MaterialCache->GetElementCount() ||
        result.Instance.TransformIndex >= frame.TransformCache->GetElementCount())
        throw std::out_of_range("Local shadow instance references missing scene data");
    result.Material = frame.MaterialCache->GetCPUData()[result.Instance.MaterialIndex];
    result.World = frame.TransformCache->GetCPUData()[result.Instance.TransformIndex].WorldMatrix;
    return result;
}

void SceneLocalShadows::Refit(UINT caster)
{
    for (UINT index = _leaves[caster]; index != UINT_MAX; index = _nodes[index].Parent)
    {
        auto& node = _nodes[index];
        if (node.Count)
        {
            node.Bounds = _casters[node.First].Bounds;
            for (UINT i = node.First + 1; i < node.First + node.Count; ++i)
                BoundingBox::CreateMerged(node.Bounds, node.Bounds, _casters[i].Bounds);
        }
        else BoundingBox::CreateMerged(node.Bounds, _nodes[node.Left].Bounds, _nodes[node.Right].Bounds);
    }
}

void SceneLocalShadows::Invalidate(const Caster& caster)
{
    if (!caster.CastsShadow) return;
    for (auto& slot : _slots) for (auto& cache : slot.Cache)
    {
        if (!cache.HasLight || !LocalShadowRangeIntersects(cache.Light, caster.Bounds)) continue;
        for (UINT face = 0; face < LocalShadowFaceCount(cache.Light); ++face)
            if (GDX12LocalShadowFrustum(cache.Light.ShadowViewProj[face]).Intersects(caster.Bounds))
            {
                cache.ListsValid &= ~(1u << face);
                cache.MapsValid &= ~(1u << face);
            }
    }
}

void SceneLocalShadows::InvalidateGrass(const GDX12RenderPrimitive& grass)
{
    if (!grass.Visible || !grass.CastsShadows || !grass.InstanceCount) return;
    for (auto& slot : _slots) for (auto& cache : slot.Cache)
    {
        if (!cache.HasLight || !LocalShadowRangeIntersects(cache.Light, grass.Bounds)) continue;
        for (UINT face = 0; face < LocalShadowFaceCount(cache.Light); ++face)
            if (GDX12LocalShadowFrustum(cache.Light.ShadowViewProj[face]).Intersects(grass.Bounds))
                cache.MapsValid &= ~(1u << face);
    }
}

void SceneLocalShadows::UpdateGrass(const std::vector<GDX12RenderPrimitive>& grass)
{
    bool receiversChanged = _grass.size() != grass.size();
    for (size_t i = 0; i < (std::max)(_grass.size(), grass.size()); ++i)
    {
        const auto* before = i < _grass.size() ? &_grass[i] : nullptr;
        const auto* after = i < grass.size() ? &grass[i] : nullptr;
        if (before && after && before->Revision() == after->Revision()) continue;
        if (before) InvalidateGrass(*before);
        if (after) InvalidateGrass(*after);
        receiversChanged |= !before || !after || before->Visible != after->Visible ||
            before->InstanceCount != after->InstanceCount ||
            std::memcmp(&before->Bounds, &after->Bounds, sizeof(BoundingBox)) != 0;
    }
    _grass = grass;
    if (receiversChanged) ++_grassReceiverRevision;
}

void SceneLocalShadows::UpdateCasters(UINT slot)
{
    auto& frame = *_resources.FrameConstants.at(slot);
    auto changed = _resources.IndirectCommandsCache->ConsumeChangedElements();
    const auto instances = frame.InstanceCache->ConsumeChangedElements();
    const auto transforms = frame.TransformCache->ConsumeChangedElements();
    const auto materials = frame.MaterialCache->ConsumeChangedElements();
    if (!_treeReady || _inputs.size() != _resources.IndirectCommandsCache->GetElementCount() ||
        _geometryRevision != _resources.GeometryRevision || _meshRevision != _resources.GeometryBuffer->GetMeshCacheRevision())
    {
        RebuildCasters(slot);
        return;
    }
    // Resource rebinding is rare; no texture-registry scan on ordinary frames.
    if (_textureRevision != _resources.TextureRevision)
    {
        for (auto& cachedSlot : _slots) for (auto& cache : cachedSlot.Cache) cache.MapsValid = 0;
        _textureRevision = _resources.TextureRevision;
        ++_sceneRevision;
    }
    const auto append = [&](const auto& indices, const auto& references) {
        for (UINT index : indices)
            if (const auto found = references.find(index); found != references.end())
                changed.insert(changed.end(), found->second.begin(), found->second.end());
    };
    append(instances, _instances);
    append(transforms, _transforms);
    append(materials, _materials);
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    for (UINT index : changed)
    {
        const auto current = ReadInput(slot, index);
        auto& old = _inputs.at(index);
        const auto included = [](const Input& input) {
            return input.Command.InstanceCount && !input.Material.ReflectionProbePreview;
        };
        // Insertion/removal changes BVH topology; edits to an existing object only refit its ancestors.
        if (included(current) != included(old))
        {
            RebuildCasters(slot);
            return;
        }
        const bool materialDirty = std::find(materials.begin(), materials.end(), current.Instance.MaterialIndex) != materials.end();
        if (!materialDirty && std::memcmp(&current.Command, &old.Command, sizeof(current.Command)) == 0 &&
            std::memcmp(&current.Instance, &old.Instance, sizeof(current.Instance)) == 0 &&
            std::memcmp(&current.Material, &old.Material, sizeof(current.Material)) == 0 &&
            std::memcmp(&current.World, &old.World, sizeof(current.World)) == 0) continue;
        const auto rebind = [index](auto& references, UINT previous, UINT next) {
            if (previous == next) return;
            if (auto found = references.find(previous); found != references.end())
            {
                auto& commands = found->second;
                commands.erase(std::remove(commands.begin(), commands.end(), index), commands.end());
                if (commands.empty()) references.erase(found);
            }
            references[next].push_back(index);
        };
        rebind(_instances, old.Command.InstanceID, current.Command.InstanceID);
        if (current.Command.InstanceCount && old.Command.InstanceCount)
        {
            rebind(_transforms, old.Instance.TransformIndex, current.Instance.TransformIndex);
            rebind(_materials, old.Instance.MaterialIndex, current.Instance.MaterialIndex);
        }
        else if (current.Command.InstanceCount != old.Command.InstanceCount)
        {
            RebuildCasters(slot);
            return;
        }
        old = current;
        const UINT position = _positions[index];
        if (position == UINT_MAX) continue;
        auto& caster = _casters[position];
        Invalidate(caster);
        caster.Bounds = BoundingBox(current.Instance.BoundingBoxCenter, current.Instance.BoundingBoxExtents);
        caster.Command = current.Command;
        caster.CastsShadow = current.Instance.CastsShadows != 0 && current.Material.RenderLayer != 2;
        caster.Masked = current.Material.RenderLayer == 1;
        Invalidate(caster);
        Refit(position);
        ++_sceneRevision;
    }
}

void SceneLocalShadows::RebuildCasters(UINT slot)
{
    _casters.clear();
    _nodes.clear();
    _instances.clear(); _transforms.clear(); _materials.clear();
    _inputs.resize(_resources.IndirectCommandsCache->GetElementCount());
    _positions.assign(_inputs.size(), UINT_MAX);
    for (UINT i = 0; i < _resources.IndirectCommandsCache->GetElementCount(); ++i)
    {
        const auto& input = _inputs[i] = ReadInput(slot, i);
        const auto& command = input.Command;
        _instances[command.InstanceID].push_back(i);
        if (!command.InstanceCount) continue;
        const auto& instance = input.Instance;
        _transforms[instance.TransformIndex].push_back(i);
        _materials[instance.MaterialIndex].push_back(i);
        const auto& material = input.Material;
        if (material.ReflectionProbePreview) continue;
        // Blended bounds receive shadows, but never enter opaque/masked caster lists.
        _casters.push_back({BoundingBox(instance.BoundingBoxCenter, instance.BoundingBoxExtents), command,
            instance.CastsShadows != 0 && material.RenderLayer != 2, material.RenderLayer == 1, i});
    }
    _leaves.resize(_casters.size());
    if (!_casters.empty()) BuildNode(0, UINT(_casters.size()));
    for (UINT i = 0; i < _casters.size(); ++i) _positions[_casters[i].InputIndex] = i;
    for (auto& frame : _slots) for (auto& cache : frame.Cache) cache.ListsValid = cache.MapsValid = 0;
    _treeReady = true;
    ++_sceneRevision;
    _geometryRevision = _resources.GeometryRevision;
    _meshRevision = _resources.GeometryBuffer->GetMeshCacheRevision();
    _textureRevision = _resources.TextureRevision;
}

void SceneLocalShadows::Gather(UINT index, const GDX12LocalLightConstants& light,
    const std::array<GDX12LocalShadowFrustum, 6>& faces, UINT faceMask, LightCache& output) const
{
    const auto& node = _nodes[index];
    if (!LocalShadowRangeIntersects(light, node.Bounds)) return;
    for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
        if ((faceMask & (1u << face)) && !faces[face].Intersects(node.Bounds)) faceMask &= ~(1u << face);
    if (!faceMask) return;
    if (!node.Count)
    {
        Gather(node.Left, light, faces, faceMask, output);
        Gather(node.Right, light, faces, faceMask, output);
        return;
    }
    for (UINT i = node.First; i < node.First + node.Count; ++i)
    {
        const auto& caster = _casters[i];
        if (!caster.CastsShadow || !LocalShadowRangeIntersects(light, caster.Bounds)) continue;
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
            if ((faceMask & (1u << face)) && faces[face].Intersects(caster.Bounds))
                (caster.Masked ? output.Faces[face].Masked : output.Faces[face].Opaque).push_back(caster.Command);
    }
}

UINT SceneLocalShadows::NeededFaces(UINT index, const GDX12LocalLightConstants& light,
    const std::array<GDX12LocalShadowFrustum, 6>& faces, UINT faceMask, const Slot& frame) const
{
    const auto& node = _nodes[index];
    if (!frame.Camera->Intersects(node.Bounds) || !LocalShadowRangeIntersects(light, node.Bounds)) return 0;
    if (!node.Count)
    {
        const UINT left = NeededFaces(node.Left, light, faces, faceMask, frame);
        return left | ((left == faceMask) ? 0 : NeededFaces(node.Right, light, faces, faceMask & ~left, frame));
    }
    UINT needed = 0;
    // Include the largest normal offset used by LocalVisibility when selecting cube faces.
    const float margin = light.Range / frame.Resolution * 0.3f + 0.01f;
    for (UINT i = node.First; i < node.First + node.Count && needed != faceMask; ++i)
        if (const auto visible = VisibleBounds(_casters[i].Bounds, frame.CameraPlanes, frame.CameraCorners);
            visible && LocalShadowRangeIntersects(light, *visible))
        {
            auto bounds = *visible;
            bounds.Extents = Vector3(bounds.Extents) + Vector3(margin, margin, margin);
            for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
                if ((faceMask & ~needed & (1u << face)) && faces[face].Intersects(bounds)) needed |= 1u << face;
        }
    return needed;
}

void SceneLocalShadows::CullJob::Execute(size_t index)
{
    const auto& light = Frame->Lights[index];
    auto& output = Frame->Cache[index];
    if (!Frame->SelectionChanged && !output.SelectionDirty && (output.Needed & ~output.ListsValid) == 0) return;
    output.SelectionDirty = false;
    output.Needed = 0;
    if (!(light.ShadowFlags & 1u) || (light.Type != 1 && light.Type != 2) ||
        !Owner->_resources.ShadowQuality.LocalResolution) return;
    const BoundingBox influence(light.Position, XMFLOAT3(light.Range, light.Range, light.Range));
    if (!Frame->Camera->Intersects(influence)) return;
    const std::array<GDX12LocalShadowFrustum, 6> faces = {
        GDX12LocalShadowFrustum(light.ShadowViewProj[0]), GDX12LocalShadowFrustum(light.ShadowViewProj[1]),
        GDX12LocalShadowFrustum(light.ShadowViewProj[2]), GDX12LocalShadowFrustum(light.ShadowViewProj[3]),
        GDX12LocalShadowFrustum(light.ShadowViewProj[4]), GDX12LocalShadowFrustum(light.ShadowViewProj[5])};
    const UINT allFaces = (1u << LocalShadowFaceCount(light)) - 1;
    if (!Owner->_nodes.empty()) output.Needed = Owner->NeededFaces(0, light, faces, allFaces, *Frame);
    const float margin = light.Range / Frame->Resolution * 0.3f + 0.01f;
    for (const auto& grass : Owner->_grass)
    {
        if (!grass.Visible || !grass.InstanceCount) continue;
        const auto visible = VisibleBounds(grass.Bounds, Frame->CameraPlanes, Frame->CameraCorners);
        if (!visible || !LocalShadowRangeIntersects(light, *visible)) continue;
        auto bounds = *visible;
        bounds.Extents = Vector3(bounds.Extents) + Vector3(margin, margin, margin);
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face)
            if (faces[face].Intersects(bounds)) output.Needed |= 1u << face;
    }
    const UINT missing = output.Needed & ~output.ListsValid;
    for (UINT face = 0; face < 6; ++face)
        if (missing & (1u << face))
        {
            output.Faces[face].Opaque.clear();
            output.Faces[face].Masked.clear();
        }
    if (missing && !Owner->_nodes.empty()) Owner->Gather(0, light, faces, missing, output);
    output.ListsValid |= missing;
}

void SceneLocalShadows::PackJob::Execute(size_t)
{
    Frame->Views.clear();
    Frame->Packed.clear();
    UINT additional = 0;
    for (UINT i = 0; i < Frame->Lights.size(); ++i)
        if (Frame->Cache[i].Needed && Frame->Cache[i].FirstSlice == UINT_MAX) additional += LocalShadowFaceCount(Frame->Lights[i]);
    if (Frame->NextSlice + additional > Frame->Slices)
    {
        Frame->NextSlice = 0;
        for (auto& cached : Frame->Cache) { cached.FirstSlice = UINT_MAX; cached.MapsValid = 0; }
    }
    for (UINT lightIndex = 0; lightIndex < Frame->Lights.size(); ++lightIndex)
    {
        auto& light = Frame->Lights[lightIndex];
        auto& cached = Frame->Cache[lightIndex];
        light.ShadowFirstSlice = UINT_MAX;
        light.ShadowFlags |= 4u;
        light._padding[2] = cached.Needed;
        if (!cached.Needed) continue;
        const UINT faces = LocalShadowFaceCount(light);
        if (cached.FirstSlice == UINT_MAX)
        {
            if (Frame->NextSlice + faces > Frame->Slices)
                throw std::length_error("Visible local shadow maps exceed the D3D12 array slice limit");
            cached.FirstSlice = Frame->NextSlice;
            Frame->NextSlice += faces;
        }
        light.ShadowFirstSlice = cached.FirstSlice;
        const UINT update = cached.Needed & ((light.ShadowFlags & 2u) ? UINT_MAX : ~cached.MapsValid);
        for (UINT face = 0; face < faces; ++face)
        {
            if (!(update & (1u << face))) continue;
            const auto& commands = cached.Faces[face];
            if (Frame->Packed.size() + commands.Opaque.size() + commands.Masked.size() > UINT_MAX / sizeof(GDX12IndirectDrawArgs))
                throw std::length_error("Local shadow indirect buffer exceeds the buffer size limit");
            Frame->Views.push_back({lightIndex, face, cached.FirstSlice + face, UINT(Frame->Packed.size()),
                UINT(commands.Opaque.size()), UINT(commands.Masked.size())});
            Frame->Packed.insert(Frame->Packed.end(), commands.Opaque.begin(), commands.Opaque.end());
            Frame->Packed.insert(Frame->Packed.end(), commands.Masked.begin(), commands.Masked.end());
        }
    }
}

void SceneLocalShadows::Freeze(UINT slot, const std::vector<GDX12RenderPrimitive>& grass)
{
    if (!_common.Jobs) throw std::logic_error("Local shadow preparation requires JobSystem");
    UpdateCasters(slot);
    UpdateGrass(grass);
    auto& frame = _slots.at(slot);
    frame.Lights = _resources.LocalLights;
    for (const auto& light : frame.Lights)
        if (!std::isfinite(light.Position.x) || !std::isfinite(light.Position.y) || !std::isfinite(light.Position.z) ||
            !std::isfinite(light.Range) || light.Range <= 0)
            throw std::invalid_argument("Invalid local shadow light bounds");
    bool sameLights = frame.Cache.size() == frame.Lights.size();
    for (UINT i = 0; sameLights && i < frame.Lights.size(); ++i)
        sameLights = frame.Cache[i].HasLight && frame.Cache[i].Light.Type == frame.Lights[i].Type &&
            GetLocalShadowKey(frame.Cache[i].Light) == GetLocalShadowKey(frame.Lights[i]);
    if (!sameLights)
    {
        frame.Cache.clear();
        frame.Cache.resize(frame.Lights.size());
        frame.NextSlice = 0;
    }
    UINT slices = 0;
    for (UINT i = 0; i < frame.Lights.size(); ++i)
    {
        auto source = frame.Lights[i];
        source.ShadowFlags &= ~2u;
        source.ShadowFirstSlice = 0;
        auto& cached = frame.Cache[i];
        if (ShouldRenderLocalShadow(source, cached.HasLight ? &cached.Light : nullptr) ||
            (source.ShadowFlags & 1u) != (cached.Light.ShadowFlags & 1u))
        {
            cached.ListsValid = cached.MapsValid = 0;
            cached.SelectionDirty = true;
        }
        cached.Light = source;
        cached.HasLight = true;
        if (source.ShadowFlags & 1u) slices += LocalShadowFaceCount(source);
    }
    frame.SelectionChanged = !frame.Camera || frame.SceneRevision != _sceneRevision ||
        frame.GrassReceiverRevision != _grassReceiverRevision ||
        frame.Resolution != (std::max)(1u, _resources.ShadowQuality.LocalResolution);
    frame.SceneRevision = _sceneRevision;
    frame.GrassReceiverRevision = _grassReceiverRevision;
    PrepareAtlas(frame, (std::min)(slices, UINT(D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION)));
    XMFLOAT4X4 viewProjection;
    XMStoreFloat4x4(&viewProjection, XMMatrixTranspose(_common.CameraView * _common.CameraProjection));
    frame.SelectionChanged |= std::memcmp(&frame.CameraMatrix, &viewProjection, sizeof(viewProjection)) != 0;
    frame.CameraMatrix = viewProjection;
    frame.Camera.emplace(viewProjection);
    const auto matrix = XMLoadFloat4x4(&viewProjection);
    const XMVECTOR planes[] = {matrix.r[3] + matrix.r[0], matrix.r[3] - matrix.r[0], matrix.r[3] + matrix.r[1],
        matrix.r[3] - matrix.r[1], matrix.r[2], matrix.r[3] - matrix.r[2]};
    for (UINT i = 0; i < 6; ++i) XMStoreFloat4(&frame.CameraPlanes[i], planes[i]);
    const auto inverse = XMMatrixInverse(nullptr, _common.CameraView * _common.CameraProjection);
    for (UINT i = 0; i < 8; ++i)
        XMStoreFloat3(&frame.CameraCorners[i], XMVector3TransformCoord(
            XMVectorSet(i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, i & 4 ? 1.f : 0.f, 1), inverse));
    frame.Ready = frame.Cull.Schedule(*_common.Jobs, frame.Lights.size(), 1);
    frame.Ready = frame.Pack.Schedule(*_common.Jobs, 1, frame.Ready);
}

void SceneLocalShadows::Complete(UINT slot)
{
    auto& frame = _slots.at(slot);
    frame.Ready.Complete();
    const UINT lightCount = (std::max)(1u, UINT(frame.Lights.size()));
    if (!frame.LightUpload)
    {
        frame.LightUpload = std::make_unique<GDX12UploadBuffer<GDX12LocalLightConstants>>(_resources.Device, lightCount, EBufferType::Upload, false);
        frame.LightUpload->TrackChanges();
    }
    frame.LightUpload->Resize(lightCount);
    if (frame.Lights.empty()) frame.LightUpload->CopyData(0, GDX12LocalLightConstants{});
    else frame.LightUpload->CopyData(0, frame.Lights.data(), UINT(frame.Lights.size()));
    const UINT commandCount = (std::max)(1u, UINT(frame.Packed.size()));
    if (!frame.CommandUpload)
    {
        frame.CommandUpload = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device, commandCount, EBufferType::Upload, false);
        frame.Commands = std::make_unique<GDX12UploadBuffer<GDX12IndirectDrawArgs>>(_resources.Device, commandCount, EBufferType::Default, false);
    }
    frame.CommandUpload->Resize(commandCount);
    frame.Commands->Resize(commandCount);
    if (!frame.Packed.empty()) frame.CommandUpload->CopyData(0, frame.Packed.data(), UINT(frame.Packed.size()));
    std::vector<bool> updated(frame.Slices, false);
    for (const auto& view : frame.Views) updated[view.Slice] = true;
    for (UINT slice = 0; slice < frame.Slices; ++slice)
        if (!updated[slice]) frame.Dsvs[slice].reset();
    for (const auto& view : frame.Views)
    {
        auto& descriptor = frame.Dsvs[view.Slice];
        if (descriptor) continue;
        descriptor = std::make_unique<GDX12Descriptor>();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = view.Slice;
        dsv.Texture2DArray.ArraySize = 1;
        descriptor->InitAsDSV(frame.Atlas.GetResource()->D3DResource.Get(), &dsv, _resources.DSVHeap.get(), _resources.DSVHeap->GetAvailableIndex());
    }
}

void SceneLocalShadows::Submitted(UINT slot)
{
    auto& frame = _slots.at(slot);
    for (const auto& view : frame.Views) frame.Cache[view.Light].MapsValid |= 1u << view.Face;
}

void SceneLocalShadows::PrepareAtlas(Slot& frame, UINT requestedSlices)
{
    const UINT resolution = (std::max)(1u, _resources.ShadowQuality.LocalResolution);
    const UINT slices = (std::max)(1u, requestedSlices);
    if (frame.Resolution == resolution && frame.Slices >= slices) return;
    for (auto& cached : frame.Cache) cached.MapsValid = 0;
    frame.NextSlice = 0;
    for (auto& cached : frame.Cache) cached.FirstSlice = UINT_MAX;
    GDX12TextureDesc texture;
    texture.Width = texture.Height = resolution;
    texture.Format = DXGI_FORMAT_D32_FLOAT;
    texture.SRVDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texture.SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    texture.SRVDesc.Texture2DArray.MipLevels = 1;
    texture.SRVDesc.Texture2DArray.ArraySize = slices;
    texture.SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.SRV_UAV_Heap = _resources.SRV_UAV_Heap.get();
    texture.SRVHeapIndex = frame.Atlas.IsInitialized() ? frame.Atlas.GetSRV()->HeapIndex : texture.SRV_UAV_Heap->GetAvailableIndex(105000);
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, resolution, resolution,
        UINT16(slices), 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D32_FLOAT;
    clear.DepthStencil.Depth = 1;
    texture.ExternalResource = _resources.Device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON, &clear);
    frame.Atlas.Initialize(texture);
    frame.Dsvs.resize(slices);
    for (UINT slice = 0; slice < slices; ++slice)
    {
        auto& view = frame.Dsvs[slice];
        if (!view) continue;
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = slice;
        dsv.Texture2DArray.ArraySize = 1;
        auto* heap = _resources.DSVHeap.get();
        view->InitAsDSV(texture.ExternalResource.Get(), &dsv, heap, view->HeapIndex == UINT_MAX ? heap->GetAvailableIndex() : view->HeapIndex);
    }
    frame.Resolution = resolution;
    frame.Slices = slices;
}

void SceneLocalShadows::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings, UINT slot,
    const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene, GDX12GeometryBuffer::RecordingBinding geometry,
    GDX12UploadBuffer<GDX12LocalLightConstants>& lights,
    const std::function<void(const GeometryTarget&, Graph&, std::vector<ID3D12Resource*>&)>& appendGrass)
{
    auto& frame = _slots.at(slot);
    auto& work = frame.Work;
    work.Commands.clear();
    if (frame.Views.empty()) return;
    const auto resource = [&](ID3D12Resource* value, const char* name) { return NativeWork::ResourceFor(graph, bindings, value, name); };
    const Id commands = resource(frame.Commands->GetResource().D3DResource.Get(), "CPU-culled local shadow commands");
    auto* target = frame.Commands->GetResource().D3DResource.Get();
    if (!frame.Packed.empty())
    {
        auto* source = frame.CommandUpload->GetResource().D3DResource.Get();
        const UINT64 bytes = UINT64(frame.Packed.size()) * sizeof(GDX12IndirectDrawArgs);
        work.Add(graph, "local shadow packed command upload",
            {{resource(source, "local shadow command upload"), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ},
             {commands, Mode::Write, D3D12_RESOURCE_STATE_COPY_DEST}},
            [=](auto* cmd) { cmd->CopyBufferRegion(target, 0, source, 0, bytes); });
    }
    const Id atlas = resource(frame.Atlas.GetResource()->D3DResource.Get(), "local shadow depth array");
    const auto views = frame.Views;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dsvs;
    for (const auto& view : views) dsvs.push_back(frame.Dsvs[view.Slice]->CPUHandle);
    const State read = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
    std::vector<Access> accesses{{0, Mode::Read, read}, {2, Mode::Read, read}, {3, Mode::Read, read},
        {21, Mode::Read, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
        {22, Mode::Read, D3D12_RESOURCE_STATE_INDEX_BUFFER}, {commands, Mode::Read, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT},
        {resource(lights.GetResource().D3DResource.Get(), "local shadow light data"), Mode::Read, read},
        {atlas, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}};
    auto* heap = _resources.SRV_UAV_Heap->GetHeap().Get();
    auto* root = _root.get();
    auto* opaquePipeline = _opaqueDraw.Get();
    auto* maskedPipeline = _maskedDraw.Get();
    auto* signature = _signature.Get();
    const auto lightSrv = lights.GetSRV()->GPUHandle;
    const auto textures = _resources.SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex);
    const UINT resolution = frame.Resolution;
    work.Add(graph, "local shadow CPU lists ExecuteIndirect", std::move(accesses), [=](auto* cmd) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetGraphicsRootSignature(root->GetRootSignature().Get());
        const UINT inputs[] = {2, 3, 0};
        for (UINT i = 0; i < 3; ++i) cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), scene[inputs[i]]);
        cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(3), lightSrv);
        cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(4), textures);
        const auto geometryRoot = root->GetGeometryRootParamIndex();
        cmd->SetGraphicsRootShaderResourceView(geometryRoot, geometry.Vertices.BufferLocation);
        cmd->SetGraphicsRootShaderResourceView(geometryRoot + 1, geometry.Vertices.BufferLocation);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->IASetVertexBuffers(0, 1, &geometry.Vertices);
        cmd->IASetIndexBuffer(&geometry.Indices);
        const D3D12_VIEWPORT viewport{0, 0, float(resolution), float(resolution), 0, 1};
        const D3D12_RECT rect{0, 0, LONG(resolution), LONG(resolution)};
        cmd->RSSetViewports(1, &viewport);
        cmd->RSSetScissorRects(1, &rect);
        for (UINT i = 0; i < views.size(); ++i)
        {
            const auto& view = views[i];
            const auto dsv = dsvs[i];
            cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
            cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
            cmd->SetGraphicsRoot32BitConstant(0, view.Light * 6 + view.Face, 1);
            if (view.OpaqueCount)
            {
                cmd->SetPipelineState(opaquePipeline);
                cmd->ExecuteIndirect(signature, view.OpaqueCount, target, UINT64(view.First) * sizeof(GDX12IndirectDrawArgs), nullptr, 0);
            }
            if (view.MaskedCount)
            {
                cmd->SetPipelineState(maskedPipeline);
                cmd->SetGraphicsRoot32BitConstant(0, view.Light * 6 + view.Face, 1);
                cmd->ExecuteIndirect(signature, view.MaskedCount, target,
                    UINT64(view.First + view.OpaqueCount) * sizeof(GDX12IndirectDrawArgs), nullptr, 0);
            }
        }
    });
    if (appendGrass)
    {
        auto& camera = *_resources.FrameConstants.at(slot)->CameraCB;
        const Id cameraResource = resource(camera.GetResource().D3DResource.Get(), "local grass shadow camera");
        for (UINT i = 0; i < views.size(); ++i)
        {
            const auto& view = views[i];
            GeometryTarget grassTarget;
            grassTarget.View = {GDX12GeometryStage::Shadow,
                Matrix(frame.Lights[view.Light].ShadowViewProj[view.Face]).Transpose(),
                _common.CameraView.Invert().Translation(), camera.GetElementAddress(_common.ActiveCameraCBufferIndex)};
            grassTarget.View.Key = (2ull << 60) | (uint64_t(view.Light) * 6 + view.Face);
            grassTarget.View.FrameSlot = slot;
            grassTarget.View.DepthFormat = DXGI_FORMAT_D32_FLOAT;
            grassTarget.Depth = atlas;
            grassTarget.Camera = cameraResource;
            grassTarget.Dsv = dsvs[i];
            grassTarget.Width = grassTarget.Height = resolution;
            appendGrass(grassTarget, graph, bindings);
        }
    }
}
}
