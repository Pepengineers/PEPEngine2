#include "App.Base/Rendering/CrossAdapterShadowRaster.h"
#include "Engine.Core/ConsoleModule.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
    using namespace GDX12Submission;
    using GDX12Snapshot::PhysicalView;
    namespace Replica = CrossAdapterSceneReplica;
    using Mode = GDX12ShadowSelection::Mode;

    void Require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    GDX12ShadowOutput Local(UINT entity, UINT generation, UINT slice, bool point)
    {
        GDX12LocalLightConstants light;
        light.EntityId = entity;
        SetLocalShadowGeneration(light, generation);
        light.Type = point ? 1 : 2;
        light.ShadowFlags = 1;
        light.ShadowFirstSlice = slice;
        for (UINT face = 0; face < LocalShadowFaceCount(light); ++face) light.ShadowViewProj[face] = Identity4x4();
        return BuildLocalShadowOutput(light, {}, 100, false);
    }

    void CheckSelection()
    {
        const auto point = Local(8, 1, 1, true);
        const auto spot = Local(6, 1, 0, false);
        const auto sun = BuildSunShadowOutput({}, 1, 100, false);
        const auto selectLocal = [&](GDX12ShadowSelection selection, std::vector<bool> ready)
            { return CrossAdapterShadows::Select(GDX12ShadowAtlas::Local, selection, { spot, point }, ready); };
        const auto all = selectLocal({}, { true, true });
        const auto partial = selectLocal({ Mode::AllLocal, {} }, { false, true });
        const auto one = selectLocal({ Mode::SpecificLight, spot.Light }, { true, true });
        Require(all && all->Layout.Layers() == 7 && all->PrimaryGroups.empty(), "All did not select both local lights");
        Require(partial && partial->Layout.Layers() == 6 && partial->SecondaryGroups == std::vector<UINT>{1} &&
            partial->PrimaryGroups == std::vector<UINT>{0} && partial->SecondaryOutput[0].Views[0].Region.Slice == 0,
            "An incomplete replica must leave its whole spot on primary and compact the admitted point");
        Require(one && one->Layout.Layers() == 1 && one->PrimaryGroups == std::vector<UINT>{1},
            "Specific spot selection included another light");
        Require(!selectLocal({ Mode::SpecificLight, { spot.Light.EntityId, spot.Light.Generation + 1 } }, { true, true }) &&
            !selectLocal({ Mode::Sun, {} }, { true, true }), "Wrong light generation or scope admitted a local transfer");
        const auto cascades = CrossAdapterShadows::Select(GDX12ShadowAtlas::Sun, { Mode::Sun, {} }, { sun }, { true });
        Require(cascades && cascades->Layout.Layers() == 4 &&
            !CrossAdapterShadows::Select(GDX12ShadowAtlas::Sun, { Mode::AllLocal, {} }, { sun }, { true }),
            "Sun mode must move all cascades together");
        Require(!IsMultiGPUShadowsEnabled(), "S2 must default off");
    }

    ComPtr<IDXGIAdapter4> SecondAdapter(LUID first)
    {
        auto& factory = GDX12DeviceFactory::GetFactory();
        for (UINT i = 0;; ++i)
        {
            ComPtr<IDXGIAdapter4> adapter;
            const auto result = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            if (result == DXGI_ERROR_NOT_FOUND) return {};
            ThrowIfFailed(result);
            DXGI_ADAPTER_DESC1 desc{};
            ThrowIfFailed(adapter->GetDesc1(&desc));
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                (desc.AdapterLuid.HighPart != first.HighPart || desc.AdapterLuid.LowPart != first.LowPart)) return adapter;
        }
    }

    Replica::MeshRange Triangle(UINT index, float depth)
    {
        Replica::MeshRange mesh;
        mesh.Mesh = Engine::Core::MeshHandle{ index, 1 };
        mesh.ContentRevision = 1;
        mesh.Vertices.resize(3);
        mesh.Vertices[0].Position = { -0.75f, -0.75f, depth };
        mesh.Vertices[1].Position = { 0.75f, -0.75f, depth };
        mesh.Vertices[2].Position = { 0.0f, 0.75f, depth };
        mesh.Indices = { 0, 1, 2, 2, 1, 0 };
        mesh.VertexCount = static_cast<UINT>(mesh.Vertices.size());
        mesh.IndexCount = static_cast<UINT>(mesh.Indices.size());
        return mesh;
    }

    struct DynamicPointCaster
    {
        Vector3 Center;
        GDX12ShadowOutput Static;
    };

    CrossAdapterShadowRaster::Input PrepareInput(GDX12Device& device, RenderMGPURuntime& runtime, PhysicalView atlas,
        const std::vector<GDX12ShadowOutput>& outputs, uint64_t frame, bool maskedCaster,
        const DynamicPointCaster* dynamicPoint = nullptr)
    {
        CrossAdapterShadowRaster::Input input;
        input.Atlas = std::move(atlas);
        input.PerFaceVisibility = dynamicPoint != nullptr;
        input.Stamp = { FrameSubmissionId{frame}, ProducerInvocationId{frame}, 100 };
        input.MaterialHeap = std::make_shared<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            Texture2D_StartIndex + Texture2D_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        Replica::Snapshot snapshot;
        snapshot.Stamp = input.Stamp;
        snapshot.Materials.emplace_back();
        if (maskedCaster)
        {
            Replica::AlphaTexture alpha;
            alpha.Texture = Engine::Core::TextureHandle{ 1, 1 };
            alpha.ContentRevision = 1;
            alpha.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            alpha.Width = alpha.Height = alpha.MipLevels = 1;
            alpha.Mips.push_back({ { std::byte{255}, std::byte{255}, std::byte{255}, std::byte{0} }, 4 });
            snapshot.AlphaTextures.push_back(std::move(alpha));
            Replica::Material material;
            material.Value.RenderLayer = 1;
            material.AlphaTextureIndex = 0;
            material.AlphaRevision = 1;
            snapshot.Materials.push_back(material);
            snapshot.Meshes.push_back(Triangle(9, 0.125f));
            Replica::Caster caster;
            caster.SourceId = 9;
            caster.Range = 0;
            caster.MaterialIndex = 1;
            caster.GeometryRevision = 1;
            caster.Transform = GDX12TransformConstants{};
            caster.Bounds = BoundingBox({ 0, 0, 0.125f }, { 0.75f, 0.75f, 0 });
            snapshot.Casters.push_back(caster);
        }
        for (UINT i = 0; i < outputs.size(); ++i)
        {
            const auto& output = outputs[i];
            const float depth = output.Type == GDX12ShadowOutput::Kind::Spot ? 0.5f : 0.25f;
            const UINT index = static_cast<UINT>(snapshot.Meshes.size());
            auto mesh = Triangle(100 + i, depth);
            if (dynamicPoint)
                for (auto& vertex : mesh.Vertices)
                    vertex.Position = {dynamicPoint->Center.x, dynamicPoint->Center.y + 2 * vertex.Position.y,
                        dynamicPoint->Center.z + 2 * vertex.Position.x};
            snapshot.Meshes.push_back(std::move(mesh));
            Replica::Caster caster;
            caster.SourceId = 100 + i;
            caster.Range = index;
            caster.MaterialIndex = 0;
            caster.GeometryRevision = 1;
            caster.Transform = GDX12TransformConstants{};
            caster.Bounds = BoundingBox({ 0, 0, depth }, { 0.75f, 0.75f, 0 });
            if (dynamicPoint)
            {
                caster.Bounds = BoundingBox(dynamicPoint->Center, {0, 1.5f, 1.5f});
                caster.Dynamic = true;
            }
            snapshot.Groups.push_back({ static_cast<UINT>(snapshot.Casters.size()) });
            if (maskedCaster) snapshot.Groups.back().push_back(0);
            snapshot.Casters.push_back(caster);
            CrossAdapterShadowRaster::Group group;
            group.Output = output;
            group.ReplicaGroup = i;
            if (output.Type != GDX12ShadowOutput::Kind::Sun)
            {
                group.Static = dynamicPoint ? dynamicPoint->Static : output;
                group.Static->StaticOnly = true;
                group.Static->EveryFrame = false;
            }
            input.Groups.push_back(std::move(group));
        }
        auto slot = std::make_shared<Replica::Slot>();
        Replica::Prepare(slot, device, input.MaterialHeap, input.Stamp, snapshot, [&]
        {
            return ResourceIdentity{ runtime.NewResourceId(), device.GetSubmissionAdapterId(), runtime.NewEpoch(),
                TransferId::Invalid, runtime.NewAllocationId() };
        });
        input.Replica = std::shared_ptr<const Replica::PreparedFrame>(slot, &slot->Get());
        input.Upload = std::move(slot);
        return input;
    }

    void CheckDepth(GDX12Device& device, const PhysicalView& atlas, const std::vector<GDX12ShadowOutput>& outputs,
        const DynamicPointCaster* dynamicPoint)
    {
        const auto desc = atlas.Resource->D3DResource->GetDesc();
        UINT64 bytes = 0;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(desc.DepthOrArraySize);
        device.GetDevice()->GetCopyableFootprints(&desc, 0, desc.DepthOrArraySize, 0, footprints.data(), nullptr, nullptr, &bytes);
        const auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
            D3D12_RESOURCE_STATE_COPY_DEST);
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({ atlas.Resource->GetCopySourceBarrier() });
        for (UINT slice = 0; slice < desc.DepthOrArraySize; ++slice)
        {
            const CD3DX12_TEXTURE_COPY_LOCATION src(atlas.Resource->D3DResource.Get(), slice);
            const CD3DX12_TEXTURE_COPY_LOCATION dst(readback.Get(), footprints[slice]);
            commands->GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        commands->ResourceBarrier({ atlas.Resource->GetCommonBarrier() });
        queue->ExecuteCommandList(commands);
        queue->Flush();
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
        void* mapped = nullptr;
        ThrowIfFailed(readback->Map(0, &range, &mapped));
        bool correct = true;
        for (const auto& output : outputs)
            for (UINT face = 0; face < (dynamicPoint ? 1u : output.ViewCount()); ++face)
            {
                const auto& region = output.Views[face].Region;
                const auto& footprint = footprints[region.Slice];
                UINT x = region.Width / 2, y = region.Height / 2;
                if (dynamicPoint)
                {
                    const auto& center = dynamicPoint->Center;
                    const auto clip = Vector4::Transform(Vector4(center.x, center.y, center.z, 1),
                        Matrix(output.Views[face].ViewProj).Transpose());
                    x = static_cast<UINT>(region.Width * (0.5f + 0.5f * clip.x / clip.w));
                    y = static_cast<UINT>(region.Height * (0.5f - 0.5f * clip.y / clip.w));
                    Require(x < region.Width && y < region.Height, "Point caster sample is outside its face");
                }
                const auto offset = footprint.Offset + UINT64(region.Y + y) * footprint.Footprint.RowPitch +
                    UINT64(region.X + x) * sizeof(float);
                float depth;
                std::memcpy(&depth, static_cast<const unsigned char*>(mapped) + offset, sizeof(float));
                if (dynamicPoint)
                {
                    const bool empty = output.Views[face].CasterRevision == dynamicPoint->Static.Views[face].CasterRevision;
                    correct &= empty ? depth == 1.0f : (depth >= 0 && depth < 1.0f);
                    std::cout << "Native Local point +X " << (empty ? "out-of-range" : "in-range")
                        << " dynamic depth=" << depth << '\n';
                }
                else
                {
                    const float expected = output.Type == GDX12ShadowOutput::Kind::Spot ? 0.5f : 0.25f;
                    const bool matches = std::abs(depth - expected) < 0.01f;
                    correct &= matches;
                    if (!matches)
                        std::cerr << "Shadow depth mismatch: kind=" << static_cast<int>(output.Type)
                            << " entity=" << output.Light.EntityId << " generation=" << output.Light.Generation
                            << " face=" << face << " slice=" << region.Slice << " x=" << region.X + x
                            << " y=" << region.Y + y << " actual=" << depth << " expected=" << expected << '\n';
                }
            }
        const D3D12_RANGE noWrites{ 0, 0 };
        readback->Unmap(0, &noWrites);
        Require(correct, "Real raster/restore depth differs, an alpha cutout wrote depth, or a view was lost");
    }

    void CheckDebug(GDX12Device& device)
    {
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &size));
            std::vector<std::byte> data(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(data.data());
            ThrowIfFailed(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
        }
    }

    template<class T>
    std::vector<T> ReadDiagnosticBuffer(GDX12Device& device, const std::shared_ptr<GDX12Resource>& source)
    {
        const auto bytes = source->D3DResource->GetDesc().Width;
        Require(bytes % sizeof(T) == 0, "Diagnostic buffer stride mismatch");
        const auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
            D3D12_RESOURCE_STATE_COPY_DEST);
        auto* queue = device.GetCommandQueue();
        auto* commands = queue->GetCommandList();
        const auto previous = source->GetCurrentState();
        commands->ResourceBarrier({ source->GetCopySourceBarrier() });
        commands->GetCommandList()->CopyBufferRegion(readback.Get(), 0, source->D3DResource.Get(), 0, bytes);
        commands->ResourceBarrier({ source->GetTransitionBarrier(previous) });
        queue->ExecuteCommandList(commands);
        queue->Flush();
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
        void* data = nullptr;
        ThrowIfFailed(readback->Map(0, &range, &data));
        std::vector<T> result(static_cast<size_t>(bytes / sizeof(T)));
        std::memcpy(result.data(), data, static_cast<size_t>(bytes));
        const D3D12_RANGE noWrites{ 0, 0 };
        readback->Unmap(0, &noWrites);
        return result;
    }

    void DiagnoseReplica(GDX12Device& device, const Replica::PreparedFrame& replica,
        const std::vector<CrossAdapterShadowRaster::Group>& groups)
    {
        const auto arguments = ReadDiagnosticBuffer<GDX12IndirectDrawArgs>(device, replica.Indirect.View.Native);
        const auto instances = ReadDiagnosticBuffer<GDX12InstanceData>(device, replica.Instances.View.Native);
        const auto transforms = ReadDiagnosticBuffer<GDX12TransformConstants>(device, replica.Transforms.View.Native);
        const auto materials = ReadDiagnosticBuffer<GDX12MaterialConstants>(device, replica.Materials.View.Native);
        for (const auto& group : groups)
        {
            const auto& output = group.Output;
            const auto& recipe = replica.Groups.at(group.ReplicaGroup);
            const auto& matrix = output.Views[0].ViewProj;
            std::cerr << "Secondary recipe entity=" << output.Light.EntityId << " draws=" << recipe.Draws.size()
                << " matrix diagonal=" << matrix._11 << ',' << matrix._22 << ',' << matrix._33 << ',' << matrix._44 << '\n';
            for (const auto index : recipe.Draws)
            {
                const auto& draw = replica.Draws.at(index);
                const auto& args = arguments.at(static_cast<size_t>(draw.IndirectByteOffset / sizeof(GDX12IndirectDrawArgs)));
                const auto& instance = instances.at(args.InstanceID);
                const auto& transform = transforms.at(instance.TransformIndex).WorldMatrix;
                const auto& material = materials.at(instance.MaterialIndex);
                std::cerr << "Secondary draw source=" << draw.SourceId << " CPU instance=" << draw.Arguments.InstanceID
                    << " GPU instance=" << args.InstanceID << " indexCount=" << args.IndexCountPerInstance
                    << " instanceCount=" << args.InstanceCount << " firstIndex=" << args.StartIndexLocation
                    << " baseVertex=" << args.BaseVertexLocation << " casts=" << instance.CastsShadows
                    << " material=" << instance.MaterialIndex << " layer=" << material.RenderLayer
                    << " preview=" << material.ReflectionProbePreview << " transform diagonal=" << transform._11
                    << ',' << transform._22 << ',' << transform._33 << ',' << transform._44
                    << " face0Visible=" << GDX12LocalShadowFrustum(matrix).Intersects(draw.Bounds) << '\n';
                const auto findBuffer = [&](D3D12_GPU_VIRTUAL_ADDRESS address)
                {
                    const auto found = std::find_if(replica.RasterAccesses.begin(), replica.RasterAccesses.end(), [&](const auto& access)
                    { return access.View.Native->D3DResource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                        access.View.Native->D3DResource->GetGPUVirtualAddress() == address; });
                    Require(found != replica.RasterAccesses.end(), "Diagnostic geometry buffer is missing");
                    return found->View.Native;
                };
                const auto vertices = ReadDiagnosticBuffer<Engine::Core::Vertex>(device, findBuffer(draw.Vertices.BufferLocation));
                const auto indices = ReadDiagnosticBuffer<UINT>(device, findBuffer(draw.Indices.BufferLocation));
                for (UINT corner = 0; corner < 3; ++corner)
                {
                    const auto vertexIndex = INT64(indices.at(args.StartIndexLocation + corner)) + args.BaseVertexLocation;
                    const auto& vertex = vertices.at(static_cast<size_t>(vertexIndex)).Position;
                    std::cerr << "Secondary vertex=" << vertexIndex << " position=" << vertex.x << ',' << vertex.y << ',' << vertex.z << '\n';
                }
            }
        }
    }

    void CheckRouting(GDX12Device& primary, GDX12Device& secondary, RenderMGPURuntime& runtime)
    {
        auto& direct = *primary.GetCommandQueue();
        CrossAdapterShadows routing(primary, secondary, runtime);
        CrossAdapterShadowRaster localA(primary, runtime, GDX12ShadowAtlas::Local);
        CrossAdapterShadowRaster localB(secondary, runtime, GDX12ShadowAtlas::Local);
        CrossAdapterShadowRaster sunA(primary, runtime, GDX12ShadowAtlas::Sun);
        CrossAdapterShadowRaster sunB(secondary, runtime, GDX12ShadowAtlas::Sun);
        const auto localAtlas = localA.CreateDepth(7);
        const auto sunAtlas = sunA.CreateDepth();
        const auto spot = Local(6, 1, 0, false);
        const auto point = Local(8, 1, 1, true);
        const auto sun = BuildSunShadowOutput({}, 1, 100, false);
        Require(ConsoleModule{}.FindConsoleVariable(L"Render.Shadows.MultiGPU.Entity").SetFromString("8") &&
            ConsoleModule{}.FindConsoleVariable(L"Render.Shadows.MultiGPU.Selection").SetFromString("3"),
            "Unable to set a stable specific-light selection");
        const auto chosen = routing.ReadSelection({ spot, point });
        const auto replacement = Local(8, 2, 1, true);
        Require(chosen.Light == point.Light && routing.ReadSelection({ spot, replacement }).Light == point.Light,
            "Reusing an ECS slot must not retarget the chosen generation");
        unsigned commits = 0;
        uint64_t sequence = 0;
        const auto run = [&](CrossAdapterShadowRaster& rasterA, CrossAdapterShadowRaster& rasterB, const PhysicalView& atlas,
            const std::vector<GDX12ShadowOutput>& outputs, GDX12ShadowSelection selection, bool fail, bool retire = true,
            const DynamicPointCaster* dynamicPoint = nullptr)
        {
            ++sequence;
            std::cout << "Exact shadow fixture: frame=" << sequence << " selection=" << static_cast<int>(selection.Scope)
                << " entity=" << selection.Light.EntityId << " groups=" << outputs.size()
                << " reject=" << fail << " dynamic=" << (dynamicPoint != nullptr) << std::endl;
            auto a = PrepareInput(primary, runtime, atlas, outputs, sequence, !dynamicPoint, dynamicPoint);
            auto b = PrepareInput(secondary, runtime, {}, outputs, sequence, !dynamicPoint, dynamicPoint);
            b.Trace = std::make_shared<CrossAdapterShadowRaster::SubmissionTrace>();
            const auto trace = b.Trace;
            const auto diagnosticReplica = b.Replica;
            const auto diagnosticGroups = b.Groups;
            a.CommitLegacy = [&] { ++commits; };
            bool rejectedRecorder = false;
            if (fail)
                b.Preparation.push_back({ "Fail shadow recording before submit", {},
                    [&](GDX12CommandList&) { rejectedRecorder = true; throw std::logic_error("Expected shadow recorder rejection"); } });
            const auto prefix = direct.SignalCompletion();
            const auto before = commits;
            const auto receipt = SubmitPreparedShadows(routing, selection, rasterA, std::move(a), rasterB, std::move(b), prefix);
            if (fail)
            {
                Require(rejectedRecorder && !receipt && commits == before && direct.GetLastCompletionPoint().Value() == prefix.Value() &&
                    !runtime.IsStopped(), "Pre-submit fallback advanced a queue or committed a cache");
                return;
            }
            Require(receipt && receipt->Timeline() == prefix.Timeline() && receipt->Value() > prefix.Value() && commits == before + 1,
                "Strict shadow plan did not return the actual primary restore receipt");
            try
            {
                CheckDepth(primary, atlas, outputs, dynamicPoint);
            }
            catch (...)
            {
                CheckDebug(primary);
                CheckDebug(secondary);
                DiagnoseReplica(secondary, *diagnosticReplica, diagnosticGroups);
                std::vector<GDX12ShadowOutput> compactOutputs;
                for (const auto& group : trace->Groups)
                {
                    std::cerr << "Secondary recorded mesh draws=" << group.MeshDraws << " entity=" << group.Light.EntityId << '\n';
                    const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output)
                    { return output.Type == group.Kind && output.Light == group.Light; });
                    Require(found != outputs.end(), "Secondary diagnostic lost its source light");
                    auto output = *found;
                    for (UINT face = 0; face < group.ViewCount; ++face) output.Views[face].Region = group.CompactViews[face];
                    compactOutputs.push_back(output);
                }
                std::cerr << "Checking secondary raster before transport: groups=" << compactOutputs.size() << '\n';
                Require(trace->SecondaryDepth != nullptr, "Secondary diagnostic has no raster depth");
                CheckDepth(secondary, { trace->SecondaryDepth, {} }, compactOutputs, dynamicPoint);
                std::cerr << "Secondary raster depth matches; primary import/restore does not\n";
                throw;
            }
            if (retire) routing.RetireFrame(direct.GetLastCompletionPoint());
            runtime.Poll();
        };

        run(localA, localB, localAtlas, { spot, point }, { Mode::SpecificLight, point.Light }, true);
        run(localA, localB, localAtlas, { spot, point }, { Mode::SpecificLight, point.Light }, false);
        const auto compact = Local(8, 1, 0, true);
        auto cacheProbe = PrepareInput(secondary, runtime, localB.ReuseDepth(6), { compact }, ++sequence, true);
        Require(localB.Prepare(cacheProbe).Stages.empty(), "An unchanged compact point must reuse both rendered layers");
        auto missingGrass = cacheProbe;
        auto grassReplica = std::make_shared<Replica::PreparedFrame>(*missingGrass.Replica);
        Replica::GrassBatch grass;
        grass.Stamp = missingGrass.Stamp;
        grass.GroupIndex = 0;
        grass.SourceId = 50;
        grass.Device = &secondary;
        grass.Owner = cacheProbe.Upload;
        grass.SamePlan = true;
        grassReplica->Groups[0].Grass.push_back(std::move(grass));
        missingGrass.Replica = std::move(grassReplica);
        Require(!localB.IsReady(missingGrass, missingGrass.Groups[0]),
            "Mesh-only readiness must not admit a light with an unrecordable grass caster");
        ++cacheProbe.Groups[0].Output.Views[5].CasterRevision;
        const auto dirty = localB.Prepare(std::move(cacheProbe));
        Require(std::none_of(dirty.Stages.begin(), dirty.Stages.end(), [](const auto& stage)
            { return std::string_view(stage.Name) == "Shadow static depth"; }) &&
            std::any_of(dirty.Stages.begin(), dirty.Stages.end(), [](const auto& stage)
            { return std::string_view(stage.Name) == "Shadow final depth"; }),
            "A dynamic face change must leave the committed static depth reusable");
        run(localA, localB, localAtlas, { spot, point }, { Mode::SpecificLight, point.Light }, false);
        run(localA, localB, localAtlas, { spot, point }, { Mode::SpecificLight, spot.Light }, false);
        run(localA, localB, localAtlas, { spot, point }, { Mode::AllLocal, {} }, false);
        run(sunA, sunB, sunAtlas, { sun }, { Mode::Sun, {} }, false);
        run(localA, localB, localAtlas, { spot, point }, { Mode::All, {} }, false, false);
        run(sunA, sunB, sunAtlas, { sun }, { Mode::All, {} }, false);
        Require(commits == 7, "Unexpected number of accepted shadow cache transactions");
        GDX12LocalLightConstants light;
        light.EntityId = 9;
        SetLocalShadowGeneration(light, 1);
        light.Type = 1;
        light.ShadowFlags = 1;
        light.ShadowFirstSlice = 1;
        light.Range = 100;
        BuildLocalLightShadowMatrices(light);
        for (const bool inRange : {false, true})
        {
            const Vector3 center = Vector3(80, 65, 0) * (inRange ? 0.9f : 1.0f);
            const BoundingBox bounds(center, {0, 1.5f, 1.5f});
            Require(LocalShadowRangeIntersects(light, bounds) == inRange &&
                LocalShadowFaceIntersects(light.ShadowViewProj[0], bounds), "Point test did not isolate range from frustum");
            const auto visible = BuildLocalShadowVisibility(light, {{bounds, 1, true}});
            const auto output = BuildLocalShadowOutput(light, visible.AllCasters, 100, false);
            const DynamicPointCaster caster{center, BuildLocalShadowOutput(light, visible.StaticCasters, 100, true)};
            Require((visible.AllCasters[0] != visible.StaticCasters[0]) == inRange, "Unexpected legacy dynamic-face classification");
            run(localA, localB, localAtlas, {output}, {Mode::SpecificLight, output.Light}, false, true, &caster);
        }
        for (const auto atlasKind : { GDX12ShadowAtlas::Sun, GDX12ShadowAtlas::Local })
        {
            CrossAdapterShadows latest(primary, secondary, runtime);
            CrossAdapterShadowRaster aRaster(primary, runtime, atlasKind), bRaster(secondary, runtime, atlasKind);
            const auto atlas = aRaster.CreateDepth(6);
            auto output = atlasKind == GDX12ShadowAtlas::Sun ? sun : compact;
            const GDX12ShadowSelection selection{ atlasKind == GDX12ShadowAtlas::Sun ? Mode::Sun : Mode::SpecificLight,
                output.Light };
            const auto step = [&](bool update, double sourceTime, bool drain = true)
            {
                std::cout << "Latest shadow fixture: atlas=" << static_cast<int>(atlasKind) << " frame=" << sequence + 1
                    << " update=" << update << " sourceTime=" << sourceTime << std::endl;
                auto a = PrepareInput(primary, runtime, atlas, { output }, ++sequence, false);
                auto b = PrepareInput(secondary, runtime, {}, { output }, sequence, false);
                a.CommitLegacy = [] {};
                const auto prefix = direct.SignalCompletion();
                const auto receipt = SubmitPreparedShadows(latest, selection, aRaster, std::move(a), bRaster,
                    std::move(b), prefix, true, update, sourceTime);
                Require(receipt && receipt->Timeline() == prefix.Timeline() && receipt->Value() >= prefix.Value(),
                    "Latest shadow lost the primary continuation receipt");
                CheckDepth(primary, atlas, { output }, nullptr);
                latest.RetireFrame(direct.GetLastCompletionPoint());
                if (drain)
                    for (auto adapter : { primary.GetSubmissionAdapterId(), secondary.GetSubmissionAdapterId() })
                        for (auto queue : { QueueKind::Direct, QueueKind::Copy }) runtime.GetQueue({ adapter, queue })->Flush();
                runtime.Poll();
            };
            step(true, 100);
            Require(latest.GetDisplayedSourceTime(atlasKind) == 100, "Latest did not bootstrap a complete snapshot");
            output.Views[0].ViewProj._41 += 0.01f;
            ++output.Views[0].CasterRevision;
            step(true, 200);
            Require(latest.GetDisplayedSourceTime(atlasKind) == 100 &&
                latest.GetDisplayedOutputs(atlasKind).front().Views[0].ViewProj._41 != output.Views[0].ViewProj._41,
                "Latest published producer metadata before the matching depth was imported");
            for (unsigned i = 0; i < 4; ++i) step(false, 200);
            Require(latest.GetDisplayedSourceTime(atlasKind) == 200 &&
                latest.GetDisplayedOutputs(atlasKind).front().Views[0].ViewProj._41 == output.Views[0].ViewProj._41,
                "Latest did not publish the completed depth and its source matrices together");
            const auto stats = latest.GetStatistics(atlasKind);
            Require(stats.Produced == 2 && stats.ExactJoins == 1 && stats.ExportBytes == stats.ImportBytes &&
                stats.ExportBytes > 0 && stats.Pending == 0,
                "Latest shadow diagnostics lost a transfer or counted it as another Exact join");
            for (unsigned i = 0; i < 4; ++i) step(true, 300 + i, false);
            Require(latest.GetStatistics(atlasKind).Pending <= 3,
                "Latest shadow queued an unbounded producer backlog");
            latest.RetireAtlas(atlasKind);
            Require(latest.GetDisplayedOutputs(atlasKind).empty(), "Retired shadow epoch retained a displayed snapshot");
        }
        runtime.Shutdown();
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view option = argc == 2 ? argv[1] : "";
        Require(argc == 1 || option == "--cpu" || option == "--gpu-debug" || option == "--gpu-validation", "Unknown shadow routing option");
        CheckSelection();
        if (option == "--cpu") return 0;
        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (option == "--gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device primary, secondary;
        ThrowIfFailed(primary.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        auto second = SecondAdapter(primary.GetAdapterLuid());
        Require(second.Get() != nullptr, "S2 fixture requires a second physical adapter");
        secondary.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(secondary.Initialize(second));
        GDX12CommandQueue copyA(&primary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        GDX12CommandQueue copyB(&secondary, D3D12_COMMAND_LIST_TYPE_COPY, true);
        RenderMGPURuntime runtime({ &primary, &secondary, &copyA, &copyB });
        Require(ConsoleModule{}.FindConsoleVariable(L"Render.Shadows.MultiGPU").SetFromString("true"),
            "Unable to enable S2 for the fixture");
        CheckRouting(primary, secondary, runtime);
        CheckDebug(primary);
        CheckDebug(secondary);
        GDX12ShaderCompiler::Shutdown();
        std::cout << "S2 real two-adapter shadow routing, masked raster, exact restore, and no-submit fallback passed.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
