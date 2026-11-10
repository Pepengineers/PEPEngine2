#include "App.Base/Rendering/CrossAdapterShadowScene.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace CrossAdapterSceneReplica;
using namespace GDX12Submission;
using Graph = GDX12RenderGraph;
using Stage = PreparedShadowReplica::Stage;
using Engine::Core::Vertex;
using DirectX::SimpleMath::Matrix;

void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

class FixturePass final : public GDX12RenderPass
{
public:
    FixturePass(Stage stage, AdapterId adapter) : _stage(std::move(stage)), _adapter(adapter) {}
    std::vector<Graph::Resource> Views;
    const std::vector<Access>& Accesses() const { return _stage.Accesses; }

    bool SupportsGraphQueue(QueueKind kind) const override { return kind == QueueKind::Direct; }

    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName(_stage.Name);
        graph.SetPlacement({ _adapter, QueueKind::Direct });
        graph.DeclareCompleteAccesses();
        graph.HasSideEffects();
        for (size_t i = 0; i < Views.size(); ++i)
            graph.Use(Views[i], _stage.Accesses[i].Mode, _stage.Accesses[i].State);
    }

    void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& bound) override
    {
        for (size_t i = 0; i < Views.size(); ++i)
            Require(bound.Get(Views[i]) == _stage.Accesses[i].Handle.Get(), "Shadow preparation bound a different resource");
        _stage.Record(*list);
    }

private:
    Stage _stage;
    AdapterId _adapter;
};

void Submit(GDX12Device& device, Graph::PlanExecutor& executor, const PreparedShadowReplica& frame)
{
    Graph graph;
    Graph::ExecutionBindings bindings;
    bindings.Queues.push_back({ &device, device.GetCommandQueue() });
    bindings.KeepAlive.push_back(frame.Replica);
    std::vector<std::unique_ptr<FixturePass>> passes;
    std::map<ResourceId, Graph::Resource> views;
    const auto add = [&](Stage stage)
    {
        auto pass = std::make_unique<FixturePass>(std::move(stage), device.GetSubmissionAdapterId());
        for (const auto& access : pass->Accesses())
        {
            const auto& identity = access.View.Identity;
            auto found = views.find(identity.Id);
            if (found == views.end())
            {
                const auto view = graph.Import("Shadow scene fixture resource");
                graph.RegisterResource(view, identity);
                bindings.Views.push_back({ view, identity, access.View.Native });
                found = views.emplace(identity.Id, view).first;
            }
            pass->Views.push_back(found->second);
        }
        graph.AddPass(*pass);
        passes.push_back(std::move(pass));
    };
    for (const auto& stage : frame.Preparation) add(stage);
    add({ "Shadow scene tables", frame.Replica->UploadAccesses,
        [slot = frame.Upload](GDX12CommandList& list) { RecordUploads(list, *slot); } });
    add({ "Shadow scene raster access", frame.Replica->RasterAccesses, [](GDX12CommandList&) {} });
    graph.CompilePlan();
    const auto receipt = executor.TrySubmit(graph, bindings);
    Require(receipt.has_value() && !receipt->Batches.empty(), "Missing shadow scene GPU receipt");
    if (frame.Commit) frame.Commit();
}

template<class T>
std::vector<T> Read(GDX12Device& device, const ResourceView& view, size_t count)
{
    const UINT64 bytes = count * sizeof(T);
    Require(view.Native->D3DResource->GetDesc().Width == bytes, "Unexpected shadow buffer size");
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto& queue = *device.GetCommandQueue();
    auto* list = queue.GetCommandList();
    list->ResourceBarrier({ view.Native->GetCopySourceBarrier() });
    list->GetCommandList()->CopyBufferRegion(readback.Get(), 0, view.Native->D3DResource.Get(), 0, bytes);
    list->ResourceBarrier({ view.Native->GetCommonBarrier() });
    queue.ExecuteCommandList(list);
    queue.Flush();
    void* mapped = nullptr;
    const D3D12_RANGE read{ 0, static_cast<SIZE_T>(bytes) }, noWrite{ 0, 0 };
    ThrowIfFailed(readback->Map(0, &read, &mapped));
    std::vector<T> result(count);
    std::memcpy(result.data(), mapped, static_cast<size_t>(bytes));
    readback->Unmap(0, &noWrite);
    return result;
}

const ResourceView& FindView(const PreparedFrame& frame, D3D12_GPU_VIRTUAL_ADDRESS address)
{
    const auto found = std::find_if(frame.RasterAccesses.begin(), frame.RasterAccesses.end(), [&](const Access& access)
    {
        return access.Handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
            access.Handle->GetGPUVirtualAddress() == address;
    });
    Require(found != frame.RasterAccesses.end(), "Shadow raster did not declare the IA resource");
    return found->View;
}

const ResourceView& AlphaView(const PreparedFrame& frame)
{
    const auto found = std::find_if(frame.RasterAccesses.begin(), frame.RasterAccesses.end(), [](const Access& access)
        { return access.Handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D; });
    Require(found != frame.RasterAccesses.end(), "Shadow raster did not declare the alpha resource");
    return found->View;
}

void ExpectSkin(GDX12Device& device, const PreparedFrame& frame, float x, float previousX, float nx, float ny)
{
    const auto& draw = frame.Draws.at(1);
    const auto vertices = Read<Vertex>(device, FindView(frame, draw.Vertices.BufferLocation), 3);
    const auto& first = vertices.front();
    Require(std::abs(first.Position.x - x) < 0.0001f && std::abs(first.PreviousPosition.x - previousX) < 0.0001f &&
        std::abs(first.Normal.x - nx) < 0.0001f && std::abs(first.Normal.y - ny) < 0.0001f,
        "Secondary skin output differs from the frozen palette");
}

void TestReplicaAccessIndex(GDX12Device& device)
{
    auto slot = std::make_shared<Slot>();
    auto heap = std::make_shared<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        Texture2D_StartIndex + Texture2D_RangeLength, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    std::uint64_t id = 1;
    const auto identity = [&]
    {
        return ResourceIdentity{ ResourceId{id++}, device.GetSubmissionAdapterId(), Epoch{1},
            TransferId::Invalid, AllocationId{id++} };
    };
    const auto buffer = [&]
    {
        auto native = std::make_shared<GDX12Resource>();
        native->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT,
            CD3DX12_RESOURCE_DESC::Buffer(3 * sizeof(Vertex)), D3D12_RESOURCE_STATE_COMMON);
        return ResourceView{ std::move(native), identity(), {}, std::make_shared<int>(0) };
    };
    Snapshot snapshot;
    snapshot.Stamp = { FrameSubmissionId{6}, ProducerInvocationId{6}, 6 };
    snapshot.Groups.resize(1);
    snapshot.Materials.resize(1);
    const auto append = [&](const ResourceView& vertices, const ResourceView& indices)
    {
        const auto index = static_cast<UINT>(snapshot.Meshes.size());
        MeshRange mesh;
        mesh.Mesh = Engine::Core::MeshHandle{index + 1, 1};
        mesh.ContentRevision = 1;
        mesh.VertexCount = mesh.IndexCount = 3;
        mesh.Resident = ResidentGeometry{ vertices, indices, 3, 3, 2 };
        snapshot.Meshes.push_back(std::move(mesh));

        Caster caster;
        caster.SourceId = index;
        caster.Range = index;
        caster.GeometryRevision = 1;
        caster.MaterialIndex = 0;
        caster.Transform.emplace();
        caster.Bounds.emplace();
        snapshot.Casters.push_back(caster);
        snapshot.Groups[0].push_back(index);
    };

    const auto first = buffer();
    const auto second = buffer();
    append(first, second);
    const auto reject = [&](const ResourceView& vertices, const ResourceView& indices, const char* message)
    {
        snapshot.Meshes[0].Resident = ResidentGeometry{ vertices, indices, 3, 3, 2 };
        try { Prepare(slot, device, heap, snapshot.Stamp, snapshot, identity); }
        catch (const std::invalid_argument& error)
        {
            Require(std::string_view(error.what()) == "Conflicting scene replica view identity", message);
            return;
        }
        throw std::runtime_error(message);
    };

    auto collision = second;
    collision.Identity.Id = first.Identity.Id;
    reject(first, collision, "ResourceId-only collision was accepted");
    collision = second;
    collision.Identity.Allocation = first.Identity.Allocation;
    reject(first, collision, "AllocationId-only collision was accepted");
    collision = second;
    collision.Native = std::make_shared<GDX12Resource>();
    collision.Native->D3DResource = first.Native->D3DResource;
    reject(first, collision, "Native-resource-only collision was accepted");
    collision.Identity = first.Identity;
    reject(first, collision, "A different tracker was accepted for an existing resource");
    collision = first;
    collision.Identity.ResourceEpoch = Epoch{2};
    reject(first, collision, "An identity change was accepted for an existing resource");
    auto transferred = first;
    transferred.Identity.Transfer = TransferId{1};
    transferred.Version = { TransferId{1}, Epoch{1}, ProducerInvocationId{1} };
    collision = transferred;
    collision.Version.Invocation = ProducerInvocationId{2};
    reject(transferred, collision, "A transfer version change was accepted for an existing resource");

    snapshot.Meshes[0].Resident = ResidentGeometry{ first, second, 3, 3, 2 };
    const auto duplicateUpload = identity();
    UINT uploadRequests = 0;
    try
    {
        Prepare(slot, device, heap, snapshot.Stamp, snapshot, [&]
        {
            ++uploadRequests;
            return duplicateUpload;
        });
    }
    catch (const std::invalid_argument& error)
    {
        Require(uploadRequests == 2 && std::string_view(error.what()) == "Conflicting scene replica view identity",
            "Wrong upload collision failure");
        uploadRequests = 0;
    }
    Require(uploadRequests == 0, "Duplicate upload identity was accepted");

    snapshot.Meshes.clear();
    snapshot.Casters.clear();
    snapshot.Groups[0].clear();
    constexpr UINT accessCount = 512;
    std::vector<ResourceId> expectedIds;
    std::weak_ptr<const void> firstOwner;
    for (UINT i = 0; i < accessCount; ++i)
    {
        const auto view = i == 0 ? first : buffer();
        if (i == 1) firstOwner = view.Owner;
        append(view, view);
        expectedIds.push_back(view.Identity.Id);
    }
    Prepare(slot, device, heap, snapshot.Stamp, snapshot, identity);
    const auto& frame = slot->Get();
    Require(frame.Draws.size() == accessCount && frame.UploadAccesses.size() == 4 &&
        frame.RasterAccesses.size() == accessCount + 4, "Access list did not merge repeated resources");
    for (UINT i = 0; i < accessCount; ++i)
    {
        const auto& access = frame.RasterAccesses[i];
        Require(access.View.Identity.Id == expectedIds[i] && access.Mode == Graph::Mode::Read &&
            access.State == (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER),
            "Access order or combined states changed");
    }
    snapshot.Meshes.clear();
    Require(!firstOwner.expired(), "Prepared access dropped its resource owner");
    slot.reset();
    Require(firstOwner.expired(), "Retired access still retained its resource owner");
}

void TestGrassMaterials(GDX12Device& device, CrossAdapterShadowScene& scene, Graph::PlanExecutor& executor)
{
    Snapshot grass;
    grass.Stamp = { FrameSubmissionId{4}, ProducerInvocationId{4}, 4 };
    grass.Groups = {{0, 1}};
    const auto owner = std::make_shared<int>(0);
    for (UINT i = 0; i < 2; ++i)
    {
        Caster caster;
        caster.SourceId = i;
        caster.Kind = GeometryKind::ExpandedGrass;
        caster.Range = i;
        caster.Transform.emplace();
        caster.Bounds.emplace();
        grass.Casters.push_back(caster);
        grass.GrassBatches.push_back({ grass.Stamp, 0, caster.SourceId, &device, owner, true });
    }

    {
        const auto legacy = scene.Prepare(grass, {});
        Require(legacy.Replica->Groups[0].Reason == Rejection::None && legacy.Replica->Groups[0].Grass.size() == 2 &&
            !legacy.Replica->Materials.View.Native && legacy.Replica->UploadAccesses.empty(),
            "Application-owned grass unexpectedly needed replica materials");
    }

    AlphaTexture alpha;
    alpha.Texture = Engine::Core::TextureHandle{2, 1};
    alpha.ContentRevision = 1;
    alpha.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    alpha.Width = alpha.Height = alpha.MipLevels = 2;
    grass.AlphaTextures = { alpha };
    alpha.Mips = {{std::vector<std::byte>(16), 8}, {std::vector<std::byte>(4), 4}};
    alpha.Mips[0].Bytes[3] = std::byte{37};
    alpha.Mips[1].Bytes[3] = std::byte{83};
    grass.Materials.resize(1);
    grass.Materials[0].Value.RenderLayer = 1;
    grass.Materials[0].Value.DiffuseIndex = 123;
    grass.Materials[0].AlphaTextureIndex = 0;
    grass.Materials[0].AlphaRevision = 1;
    grass.Casters[1].MaterialIndex = 0;

    const auto rejected = [&](const ShadowSceneSources& sources, Rejection reason)
    {
        const auto frame = scene.Prepare(grass, sources);
        const auto& group = frame.Replica->Groups[0];
        Require(group.Reason == reason && group.Grass.empty() && group.Draws.empty() &&
            frame.Replica->MaterialToLocal[0] == UINT_MAX && frame.Replica->UploadAccesses.empty() &&
            frame.Preparation.empty() && !frame.Commit, "Rejected grass group leaked part of its recipe");
    };
    rejected({}, Rejection::MissingAlpha);
    auto incomplete = alpha;
    incomplete.Mips.pop_back();
    rejected({{}, {&incomplete}, {}}, Rejection::MissingAlpha);
    grass.Casters[1].MaterialIndex = 1;
    rejected({{}, {&alpha}, {}}, Rejection::MissingMaterial);
    grass.Casters[1].MaterialIndex = 0;

    const auto cold = scene.Prepare(grass, {{}, {&alpha}, {}});
    const auto& replica = *cold.Replica;
    Require(replica.Groups[0].Reason == Rejection::None && replica.Groups[0].Grass.size() == 2 &&
        replica.Draws.empty() && !replica.Indirect.View.Native && replica.MaterialToLocal == std::vector<UINT>{0} &&
        replica.Materials.SRV.ptr && replica.AlphaTable.ptr && cold.Commit && cold.Preparation.size() == 1,
        "Grass-only group did not produce its remapped material bindings");
    Submit(device, executor, cold);
    executor.Drain();
    const auto material = Read<GDX12MaterialConstants>(device, replica.Materials.View, 1)[0];
    Require(material.RenderLayer == 1 && material.DiffuseIndex > 0 && material.DiffuseIndex != 123,
        "Grass material retained the primary diffuse descriptor index");
    const auto& resident = AlphaView(replica);
    DirectX::ScratchImage image;
    ThrowIfFailed(DirectX::CaptureTexture(device.GetCommandQueue()->GetCommandQueue().Get(),
        resident.Native->D3DResource.Get(), false, image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON));
    const auto* top = image.GetImage(0, 0, 0);
    const auto* last = image.GetImage(1, 0, 0);
    Require(top && last && top->pixels[3] == 37 && last->pixels[3] == 83, "Grass alpha lost part of its mip chain");

    grass.Stamp = { FrameSubmissionId{5}, ProducerInvocationId{5}, 5 };
    for (auto& batch : grass.GrassBatches) batch.Stamp = grass.Stamp;
    const auto warm = scene.Prepare(grass, {});
    Require(warm.Replica->Groups[0].Reason == Rejection::None && warm.Preparation.empty() && !warm.Commit &&
        AlphaView(*warm.Replica).Identity.Id == resident.Identity.Id, "Grass alpha did not reuse the resident upload");
}

void TestScene(GDX12Device& device)
{
    std::uint64_t id = 1;
    CrossAdapterShadowScene scene(device, [&]
    {
        return ResourceIdentity{ ResourceId{id++}, device.GetSubmissionAdapterId(), Epoch{1},
            TransferId::Invalid, AllocationId{id++} };
    });
    Graph::PlanExecutor executor(3);
    Engine::Core::SubMesh mesh;
    mesh.Vertices.resize(3);
    mesh.Vertices[0].Position.x = 1;
    mesh.Vertices[0].Normal = {1, 1, 0};
    mesh.Vertices[1].Position.y = mesh.Vertices[2].Position.z = 1;
    mesh.Indices = {0, 1, 2};
    mesh.Bones.push_back({ "bone", Matrix::Identity });
    mesh.SkinVertices = {{0, 1}, {}, {}};
    mesh.Influences = {{0, 1.0f}};

    Snapshot snapshot;
    snapshot.Stamp = { FrameSubmissionId{1}, ProducerInvocationId{1}, 1 };
    snapshot.Meshes.resize(1);
    snapshot.Meshes[0].Mesh = Engine::Core::MeshHandle{1, 1};
    snapshot.Meshes[0].ContentRevision = 1;
    AlphaTexture alpha;
    alpha.Texture = Engine::Core::TextureHandle{1, 1};
    alpha.ContentRevision = 1;
    alpha.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    alpha.Width = alpha.Height = alpha.MipLevels = 2;
    snapshot.AlphaTextures = { alpha };
    alpha.Mips = {{std::vector<std::byte>(16), 8}, {std::vector<std::byte>(4), 4}};
    alpha.Mips[0].Bytes[3] = std::byte{73};
    alpha.Mips[1].Bytes[3] = std::byte{42};
    snapshot.Materials.resize(1);
    snapshot.Materials[0].Value.RenderLayer = 1;
    snapshot.Materials[0].AlphaTextureIndex = 0;
    snapshot.Materials[0].AlphaRevision = 1;
    Caster rigid;
    rigid.SourceId = 1;
    rigid.Range = 0;
    rigid.MaterialIndex = 0;
    rigid.GeometryRevision = 1;
    rigid.Transform.emplace();
    rigid.Bounds.emplace();
    auto skin = rigid;
    skin.SourceId = 2;
    skin.Kind = GeometryKind::Skinned;
    skin.Range = UINT_MAX;
    snapshot.Casters = { rigid, skin };
    snapshot.Groups = {{0, 1}};

    const auto pose = [&](const Engine::Core::SubMesh* source, const Matrix& current, const Matrix& previous)
    {
        return FrozenShadowSkin{ 1, Engine::Core::MeshHandle{2, 1}, 1, 0, source,
            {{current, previous}, {Matrix::Identity, Matrix::Identity}} };
    };
    ShadowSceneSources cold{{&mesh}, {&alpha},
        {pose(&mesh, Matrix::CreateScale(2, 1, 1) * Matrix::CreateTranslation(5, 0, 0), Matrix::CreateTranslation(2, 0, 0))}};
    const auto baseline = device.GetCommandQueue()->GetLastCompletionPoint().Value();
    {
        auto canceled = scene.Prepare(snapshot, cold);
        Require(canceled.Replica->Groups[0].Reason == Rejection::None && canceled.Commit, "Cold scene did not stage");
        bool rejected = false;
        try { canceled.Commit(); }
        catch (const std::logic_error&) { rejected = true; }
        Require(rejected, "Shadow assets published without a native receipt");
    }
    Require(device.GetCommandQueue()->GetLastCompletionPoint().Value() == baseline, "Preparing submitted GPU work");
    Require(scene.Prepare(snapshot, {}).Replica->Groups[0].Reason != Rejection::None, "Discarded work populated the cache");

    {
        auto stale = cold;
        stale.Skin.front().ContentRevision = 2;
        const auto rejected = scene.Prepare(snapshot, stale);
        Require(rejected.Replica->Groups[0].Reason != Rejection::None && rejected.Replica->Draws.empty() &&
            rejected.Preparation.empty() && !rejected.Commit, "Incomplete group retained upload work");
    }

    auto first = scene.Prepare(snapshot, cold);
    Require(first.Replica->Groups[0].Draws.size() == 2 && first.Preparation.size() == 2, "Cold frame is incomplete");
    auto& queue = *device.GetCommandQueue();
    ComPtr<ID3D12Fence> gate;
    ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    ThrowIfFailed(queue.GetCommandQueue()->Wait(gate.Get(), 1));
    PreparedShadowReplica second;
    try
    {
        Submit(device, executor, first);
        snapshot.Stamp = { FrameSubmissionId{2}, ProducerInvocationId{2}, 2 };
        snapshot.Casters[0].Transform->WorldMatrix._41 = 29;
        ShadowSceneSources warm{{}, {}, {pose(nullptr, Matrix::CreateScale(1, 2, 1) * Matrix::CreateTranslation(11, 0, 0),
            Matrix::CreateTranslation(7, 0, 0))}};
        second = scene.Prepare(snapshot, warm);
        Require(second.Replica->Groups[0].Draws.size() == 2 && !second.Commit, "Resident frame required immutable CPU inputs");
        Require(second.Preparation.size() == 2 && second.Preparation.front().Accesses.size() == 1,
            "Resident frame restaged immutable assets");
        const auto& a = first.Replica->Draws[0];
        const auto& b = second.Replica->Draws[0];
        Require(FindView(*first.Replica, a.Vertices.BufferLocation).Identity.Id ==
                FindView(*second.Replica, b.Vertices.BufferLocation).Identity.Id &&
            FindView(*first.Replica, a.Indices.BufferLocation).Identity.Id ==
                FindView(*second.Replica, b.Indices.BufferLocation).Identity.Id &&
            AlphaView(*first.Replica).Identity.Id == AlphaView(*second.Replica).Identity.Id,
            "Immutable resource identity changed between frames");
        Require(first.Replica->Draws[1].Vertices.BufferLocation != second.Replica->Draws[1].Vertices.BufferLocation &&
            first.Replica->Transforms.View.Identity.Id != second.Replica->Transforms.View.Identity.Id,
            "Frame-local resources were reused in flight");
        Submit(device, executor, second);
        auto third = scene.Prepare(snapshot, warm);
        Require(third.Replica && !scene.Prepare(snapshot, warm).Replica, "Frame slots were not bounded");
    }
    catch (...)
    {
        gate->Signal(1);
        executor.Drain();
        throw;
    }
    ThrowIfFailed(gate->Signal(1));
    executor.Drain();
    const float invSqrtFive = 1.0f / std::sqrt(5.0f);
    ExpectSkin(device, *first.Replica, 7, 3, invSqrtFive, 2 * invSqrtFive);
    ExpectSkin(device, *second.Replica, 12, 8, 2 * invSqrtFive, invSqrtFive);
    Require(Read<GDX12TransformConstants>(device, second.Replica->Transforms.View, 2)[0].WorldMatrix._41 == 29,
        "Current transform was not uploaded");
    const auto materialA = Read<GDX12MaterialConstants>(device, first.Replica->Materials.View, 1)[0];
    const auto materialB = Read<GDX12MaterialConstants>(device, second.Replica->Materials.View, 1)[0];
    Require(materialA.DiffuseIndex != 0 && materialA.DiffuseIndex == materialB.DiffuseIndex,
        "Resident alpha SRV was reallocated");
    DirectX::ScratchImage image;
    ThrowIfFailed(DirectX::CaptureTexture(queue.GetCommandQueue().Get(), AlphaView(*second.Replica).Native->D3DResource.Get(),
        false, image, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON));
    const auto* topMip = image.GetImage(0, 0, 0);
    const auto* lastMip = image.GetImage(1, 0, 0);
    Require(topMip && lastMip && topMip->pixels[3] == 73 && lastMip->pixels[3] == 42,
        "Resident alpha did not retain the mip payload");

    snapshot.Stamp = { FrameSubmissionId{3}, ProducerInvocationId{3}, 3 };
    snapshot.Meshes[0].ContentRevision = snapshot.Casters[0].GeometryRevision = 2;
    snapshot.AlphaTextures[0].ContentRevision = snapshot.Materials[0].AlphaRevision = alpha.ContentRevision = 2;
    mesh.Vertices[0].Position.x = 19;
    alpha.Mips[1].Bytes[3] = std::byte{95};
    auto changed = scene.Prepare(snapshot, cold);
    Require(changed.Replica->Groups[0].Reason == Rejection::None && changed.Commit &&
        changed.Replica->Draws[0].Vertices.BufferLocation != first.Replica->Draws[0].Vertices.BufferLocation &&
        AlphaView(*changed.Replica).Identity.Id != AlphaView(*first.Replica).Identity.Id,
        "Changed content revision reused old geometry or alpha");
    Submit(device, executor, changed);
    executor.Drain();
    const auto& changedDraw = changed.Replica->Draws[0];
    Require(Read<Vertex>(device, FindView(*changed.Replica, changedDraw.Vertices.BufferLocation), 3)[0].Position.x == 19,
        "Changed rigid vertex payload was not uploaded");
    first = {};
    second = {};
    changed = {};
    TestGrassMaterials(device, scene, executor);
    std::cout << "Shadow scene residency, canceled preparation, in-flight slots, grass materials, alpha mips and exact skin palettes passed\n";
}
}

int SceneReplicaRegressionMain(int argc, char** argv);

int main(int argc, char** argv)
{
    if (argc > 2 && std::string_view(argv[1]) == "--legacy-replica")
        return SceneReplicaRegressionMain(argc - 1, argv + 1);
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc > 1 ? argv[1] : "--debug-layer";
        Require(mode == "--debug-layer" || mode == "--gpu-validation", "Expected --debug-layer or --gpu-validation");
        ComPtr<ID3D12Debug1> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (mode == "--gpu-validation") debug->SetEnableGPUBasedValidation(TRUE);
        GDX12Device device;
        device.Role = DEVICE_ROLE_SECONDARY;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        TestScene(device);
        TestReplicaAccessIndex(device);
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T bytes = 0;
            ThrowIfFailed(info->GetMessage(i, nullptr, &bytes));
            std::vector<std::byte> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(info->GetMessage(i, message, &bytes));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
