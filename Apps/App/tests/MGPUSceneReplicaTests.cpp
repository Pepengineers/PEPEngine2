#include "App.Base/Rendering/CrossAdapterSceneReplica.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/RenderPasses/GDX12RenderPass.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace
{
using namespace CrossAdapterSceneReplica;
using namespace GDX12Submission;
using Graph = GDX12RenderGraph;
using Vertex = Engine::Core::Vertex;

void Require(bool ok, const char* text)
{
    if (!ok) throw std::runtime_error(text);
}

template<class F>
void Reject(F callback, const char* text)
{
    try { callback(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(text);
}

ResourceView CreateNativeBuffer(GDX12Device& device, const void* data, UINT64 bytes, ResourceIdentity id)
{
    auto target = std::make_shared<GDX12Resource>();
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    target->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    auto upload = device.CreateResource(D3D12_HEAP_TYPE_UPLOAD, desc, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    ThrowIfFailed(upload->Map(0, &noRead, &mapped));
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);
    auto& queue = *device.GetCommandQueue();
    auto* commands = queue.GetCommandList();
    commands->ResourceBarrier({ target->GetCopyDestBarrier() });
    commands->GetCommandList()->CopyBufferRegion(target->D3DResource.Get(), 0, upload.Get(), 0, bytes);
    commands->ResourceBarrier({ target->GetCommonBarrier() });
    queue.ExecuteCommandList(commands);
    queue.Flush();
    return { target, id, {}, target };
}

template<class T>
std::vector<T> ReadBuffer(GDX12Device& device, const ResourceView& view, size_t count)
{
    const UINT64 bytes = count * sizeof(T);
    Require(view.Native && view.Native->D3DResource->GetDesc().Width == bytes, "Wrong packed buffer size");
    auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto& queue = *device.GetCommandQueue();
    auto* list = queue.GetCommandList();
    list->ResourceBarrier({ view.Native->GetCopySourceBarrier() });
    list->GetCommandList()->CopyBufferRegion(readback.Get(), 0, view.Native->D3DResource.Get(), 0, bytes);
    list->ResourceBarrier({ view.Native->GetCommonBarrier() });
    queue.ExecuteCommandList(list);
    queue.Flush();
    std::vector<T> result(count);
    void* mapped = nullptr;
    const D3D12_RANGE read{ 0, static_cast<SIZE_T>(bytes) }, noWrite{ 0, 0 };
    ThrowIfFailed(readback->Map(0, &read, &mapped));
    std::memcpy(result.data(), mapped, static_cast<size_t>(bytes));
    readback->Unmap(0, &noWrite);
    return result;
}

class UploadPass final : public GDX12RenderPass
{
public:
    UploadPass(const Slot& slot, AdapterId adapter) : _slot(slot), _adapter(adapter) {}
    std::vector<Graph::Resource> Views;

    bool SupportsGraphQueue(QueueKind kind) const override { return kind == QueueKind::Direct; }
    void RecordRenderGraph(GDX12RenderGraphBuilder& graph) override
    {
        graph.SetName("Replica fixture uploads");
        graph.SetPlacement({ _adapter, QueueKind::Direct });
        graph.DeclareCompleteAccesses();
        graph.HasSideEffects();
        const auto& accesses = _slot.Get().UploadAccesses;
        for (size_t i = 0; i < accesses.size(); ++i)
            graph.Use(Views[i], accesses[i].Mode, accesses[i].State);
    }

    void RecordPlacedCommands(GDX12CommandList* list, const Graph::BoundResources& bound) override
    {
        const auto& accesses = _slot.Get().UploadAccesses;
        for (size_t i = 0; i < accesses.size(); ++i)
            Require(bound.Get(Views[i]) == accesses[i].Handle.Get(), "Replica upload used the wrong graph binding");
        RecordUploads(*list, _slot);
    }

private:
    const Slot& _slot;
    AdapterId _adapter;
};

void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessages(); ++i)
    {
        SIZE_T bytes = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &bytes));
        std::vector<std::byte> storage(bytes);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info.GetMessage(i, message, &bytes));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            std::cerr << message->pDescription << '\n';
            throw std::runtime_error("Scene replica D3D12 validation warning or error");
        }
    }
    info.ClearStoredMessages();
}

Snapshot MakeSnapshot(FrameStamp stamp)
{
    Snapshot snapshot;
    snapshot.Stamp = stamp;
    MeshRange absolute;
    absolute.Mesh = Engine::Core::MeshHandle{1, 3};
    absolute.ContentRevision = 4;
    absolute.Vertices.resize(4);
    absolute.Vertices[1].Position.x = 5;
    absolute.Vertices[2].Position.y = 6;
    absolute.Vertices[3].Position.z = 7;
    absolute.FirstVertex = 1;
    absolute.VertexCount = 3;
    absolute.Indices = { 999, 1, 2, 3 };
    absolute.FirstIndex = 1;
    absolute.IndexCount = 3;
    absolute.Addressing = IndexAddressing::AbsoluteInPayload;
    snapshot.Meshes.push_back(absolute);
    auto relative = absolute;
    relative.Indices = { 0, 1, 2 };
    relative.FirstIndex = 0;
    relative.Addressing = IndexAddressing::RelativeToRange;
    snapshot.Meshes.push_back(relative);
    relative.Mesh = Engine::Core::MeshHandle{2, 1};
    relative.Vertices[1].Position.x = 19;
    snapshot.Meshes.push_back(relative);
    absolute.Mesh = Engine::Core::MeshHandle{3, 1};
    absolute.Indices[1] = 0; // Absolute index below the vertex range.
    snapshot.Meshes.push_back(absolute);

    AlphaTexture alpha;
    alpha.Texture = Engine::Core::TextureHandle{1, 2};
    alpha.ContentRevision = 9;
    alpha.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    alpha.Width = alpha.Height = alpha.MipLevels = 2;
    alpha.Mips = { {std::vector<std::byte>(24), 12}, {std::vector<std::byte>(8), 8} };
    alpha.Mips[0].Bytes[3] = std::byte{73};
    alpha.Mips[0].Bytes[15] = std::byte{191};
    alpha.Mips[1].Bytes[3] = std::byte{42};
    snapshot.AlphaTextures = { alpha, alpha };
    Material opaque, masked;
    opaque.Value.DiffuseIndex = 123;
    masked.Value.RenderLayer = 1;
    masked.Value.DiffuseIndex = 123;
    masked.Value.NormalIndex = 124;
    masked.AlphaTextureIndex = 1;
    masked.AlphaRevision = 9;
    auto missing = masked;
    missing.AlphaTextureIndex = UINT_MAX;
    snapshot.Materials = { opaque, masked, missing };

    for (UINT i = 0; i < 12; ++i)
    {
        Caster caster;
        caster.SourceId = i;
        caster.Kind = GeometryKind::Static;
        caster.Range = i == 1 ? 1 : i == 2 ? 2 : i == 11 ? 3 : 0;
        caster.GeometryRevision = 4;
        caster.MaterialIndex = i == 1 ? 1 : i == 3 ? 2 : 0;
        caster.Transform.emplace().WorldMatrix._14 = float(i) * 7;
        caster.Bounds = DirectX::BoundingBox{ XMFLOAT3(float(i), 0, 0), XMFLOAT3(1, 2, 3) };
        caster.ShadowFlags = i + 1;
        snapshot.Casters.push_back(caster);
    }
    snapshot.Casters[4].Kind = snapshot.Casters[7].Kind = snapshot.Casters[9].Kind = GeometryKind::Skinned;
    snapshot.Casters[5].Kind = snapshot.Casters[8].Kind = GeometryKind::ExpandedGrass;
    snapshot.Casters[4].Range = snapshot.Casters[5].Range = UINT_MAX;
    snapshot.Casters[6].GeometryRevision = 3;
    snapshot.Casters[7].Range = 0;
    snapshot.Casters[8].Range = 0;
    snapshot.Casters[9].Range = 1;
    snapshot.Casters[10].Transform.reset();
    snapshot.Groups = { {0, 1, 2}, {0, 3}, {0, 4}, {0, 5}, {6}, {7, 8}, {}, {9}, {10}, {11} };
    return snapshot;
}

void TestReplica(GDX12Device& device)
{
    std::uint64_t nextId = 1;
    const auto newIdentity = [&]
    {
        return ResourceIdentity{ ResourceId{nextId++}, device.GetSubmissionAdapterId(), Epoch{1},
            TransferId::Invalid, AllocationId{nextId++} };
    };
    const FrameStamp stamp{ FrameSubmissionId{1}, ProducerInvocationId{1}, 21 };
    auto snapshot = MakeSnapshot(stamp);
    const auto heapCount = Texture2D_StartIndex + Texture2D_RangeLength;
    auto secondaryHeap = std::make_shared<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        heapCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    GDX12DescriptorHeap differentHeap(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        heapCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    secondaryHeap->GetAvailableIndex(Texture2D_StartIndex, Texture2D_RangeLength);

    Vertex deformed[6]{};
    deformed[0].Position.x = 22;
    deformed[3].Position.z = 89;
    const UINT dynamicIndices[]{ 0, 1, 2, 0, 2, 1 };
    const auto dynamicVB = CreateNativeBuffer(device, deformed, sizeof(deformed), newIdentity());
    const auto dynamicIB = CreateNativeBuffer(device, dynamicIndices, sizeof(dynamicIndices), newIdentity());
    const DynamicRange skin{ stamp, dynamicVB, dynamicIB, 3 * sizeof(Vertex), 3 * sizeof(UINT), 3, 3, 2 };
    auto stale = skin;
    stale.Stamp.Invocation = ProducerInvocationId{2};
    snapshot.DynamicGeometry = { skin, stale };
    std::byte nativeGrassVertex[44]{};
    const auto grassView = CreateNativeBuffer(device, nativeGrassVertex, sizeof(nativeGrassVertex), newIdentity());
    GrassBatch grass{ stamp, 5, 8, &device, grassView.Owner, true, {},
        {{ grassView, grassView.Native->D3DResource, Graph::Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE }} };
    snapshot.GrassBatches = { grass };

    auto slot = std::make_shared<Slot>();
    auto wrongStamp = stamp;
    wrongStamp.SceneRevision++;
    Reject([&] { Prepare(slot, device, secondaryHeap, wrongStamp, snapshot, newIdentity); }, "Stale scene accepted");
    Prepare(slot, device, secondaryHeap, stamp, snapshot, newIdentity);
    const auto& frame = slot->Get();
    Require(frame.Stamp == stamp && frame.PackedRanges.size() == 2 && frame.Draws.size() == 4, "Wrong replica pack counts");
    Require(frame.MeshToLocal == std::vector<UINT>{0, 0, 1, UINT_MAX} &&
        frame.MaterialToLocal == std::vector<UINT>{0, 1, UINT_MAX}, "Replica mapping did not deduplicate");
    const std::array<Rejection, 10> reasons{ Rejection::None, Rejection::MissingAlpha, Rejection::MissingGeometry,
        Rejection::MissingGeometry, Rejection::StaleGeometry, Rejection::None, Rejection::None,
        Rejection::StaleGeometry, Rejection::MissingTransform, Rejection::MissingGeometry };
    for (size_t i = 0; i < reasons.size(); ++i)
    {
        Require(frame.Groups[i].Reason == reasons[i], "Unexpected shadow group admission");
        if (reasons[i] != Rejection::None)
            Require(frame.Groups[i].Draws.empty() && frame.Groups[i].Grass.empty(), "Rejected group leaked a draw");
    }
    Require(frame.Groups[0].Draws == std::vector<UINT>{0, 1, 2} &&
        frame.Groups[5].Draws == std::vector<UINT>{3} && frame.Groups[5].Grass.size() == 1 &&
        frame.Groups[5].Grass[0].Reads[0].Handle.Get() == grassView.Native->D3DResource.Get() &&
        frame.Groups[6].Draws.empty(), "Wrong accepted mesh and grass recipes");
    Require(frame.Draws[1].Arguments.BaseVertexLocation == 0 && frame.Draws[2].Arguments.BaseVertexLocation == 3 &&
        frame.Draws[2].Arguments.StartIndexLocation == 3, "Static rebase did not preserve submesh addressing");
    Require(frame.Draws[3].Kind == GeometryKind::Skinned &&
        frame.Draws[3].Vertices.BufferLocation == dynamicVB.Native->D3DResource->GetGPUVirtualAddress() + 3 * sizeof(Vertex) &&
        frame.Draws[3].Indices.BufferLocation == dynamicIB.Native->D3DResource->GetGPUVirtualAddress() + 3 * sizeof(UINT),
        "Native skin geometry lost its invocation-owned IA range");
    Require(frame.AlphaTable.ptr == secondaryHeap->GetGPUHandle(Texture2D_StartIndex).ptr &&
        frame.AlphaTable.ptr != differentHeap.GetGPUHandle(Texture2D_StartIndex).ptr, "Alpha table is not secondary-owned");

    auto& queue = *device.GetCommandQueue();
    Graph graph;
    UploadPass upload(*slot, device.GetSubmissionAdapterId());
    Graph::ExecutionBindings bindings;
    bindings.Queues.push_back({ &device, &queue });
    bindings.KeepAlive.push_back(slot);
    for (const auto& access : frame.UploadAccesses)
    {
        const auto view = graph.Create("Scene replica upload target");
        upload.Views.push_back(view);
        graph.RegisterResource(view, access.View.Identity);
        bindings.Views.push_back({ view, access.View.Identity, access.View.Native });
    }
    graph.AddPass(upload);
    graph.CompilePlan();

    const auto before = queue.GetLastCompletionPoint();
    {
        GDX12CommandList canceled(&device);
        for (const auto& access : frame.UploadAccesses)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(access.Handle.Get(), D3D12_RESOURCE_STATE_COMMON, access.State);
            canceled.GetCommandList()->ResourceBarrier(1, &barrier);
        }
        RecordUploads(canceled, *slot);
        for (const auto& access : frame.UploadAccesses)
        {
            const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(access.Handle.Get(), access.State, D3D12_RESOURCE_STATE_COMMON);
            canceled.GetCommandList()->ResourceBarrier(1, &barrier);
            Require(access.View.Native->GetCurrentState() == D3D12_RESOURCE_STATE_COMMON, "Record-only upload changed tracker");
        }
        ThrowIfFailed(canceled.GetCommandList()->Close());
        Require(queue.GetLastCompletionPoint().Value() == before.Value(), "Record-only upload advanced the native queue");
    }

    Graph::PlanExecutor executor(1);
    ComPtr<ID3D12Fence> gate;
    ThrowIfFailed(device.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    ThrowIfFailed(queue.GetCommandQueue()->Wait(gate.Get(), 1));
    std::exception_ptr error;
    try
    {
        const auto receipt = executor.TrySubmit(graph, bindings);
        Require(receipt && receipt->Batches.size() == 1 &&
            !receipt->Batches[0].IsReachedBy(queue.GetCompletionProgress()), "Replica upload was not held in flight");
        bindings.KeepAlive.clear();
        Reject([&] { Prepare(slot, device, secondaryHeap, stamp, snapshot, newIdentity); }, "Retained replica was rebound");
    }
    catch (...) { error = std::current_exception(); }
    ThrowIfFailed(gate->Signal(1));
    executor.Drain();
    if (error) std::rethrow_exception(error);
    Require(slot.use_count() == 1, "Replica slot did not retire");

    const auto gpuMaterials = ReadBuffer<GDX12MaterialConstants>(device, frame.Materials.View, 2);
    Require(gpuMaterials[0].DiffuseIndex == 0 && gpuMaterials[1].DiffuseIndex == 1 &&
        gpuMaterials[1].NormalIndex == 0 && gpuMaterials[1].RenderLayer == 1, "Uploaded alpha material remap is wrong");
    const auto gpuTransforms = ReadBuffer<GDX12TransformConstants>(device, frame.Transforms.View, 4);
    Require(gpuTransforms[0].WorldMatrix._14 == 0 && gpuTransforms[1].WorldMatrix._14 == 7 &&
        gpuTransforms[3].WorldMatrix._14 == 49, "Uploaded transforms lost instance identity");
    const auto gpuInstances = ReadBuffer<GDX12InstanceData>(device, frame.Instances.View, 4);
    Require(gpuInstances[1].MaterialIndex == 1 && gpuInstances[3].TransformIndex == 3 &&
        gpuInstances[3].SkinOutputVertex == UINT_MAX && gpuInstances[3].SkinBatch == UINT_MAX &&
        gpuInstances[3].ReflectionDynamic == 1,
        "Uploaded instances requested rest-pose skin data or lost rebasing");
    const auto gpuIndirect = ReadBuffer<GDX12IndirectDrawArgs>(device, frame.Indirect.View, 4);
    Require(gpuIndirect[2].BaseVertexLocation == 3 && gpuIndirect[2].StartIndexLocation == 3 &&
        gpuIndirect[3].InstanceID == 3, "Uploaded indirect template rebase is wrong");
    const auto findBuffer = [&](D3D12_GPU_VIRTUAL_ADDRESS address) -> const ResourceView&
    {
        const auto found = std::find_if(frame.RasterAccesses.begin(), frame.RasterAccesses.end(), [&](const Access& access)
        {
            return access.Handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
                access.Handle->GetGPUVirtualAddress() == address;
        });
        Require(found != frame.RasterAccesses.end(), "Packed IA buffer not declared for raster");
        return found->View;
    };
    const auto gpuVertices = ReadBuffer<Vertex>(device, findBuffer(frame.Draws[0].Vertices.BufferLocation), 6);
    Require(gpuVertices[0].Position.x == 5 && gpuVertices[3].Position.x == 19, "Uploaded packed vertices differ");
    Require(ReadBuffer<UINT>(device, findBuffer(frame.Draws[0].Indices.BufferLocation), 6) ==
        std::vector<UINT>{0, 1, 2, 0, 1, 2}, "Uploaded indices were not normalized and deduplicated");

    const auto texture = std::find_if(frame.RasterAccesses.begin(), frame.RasterAccesses.end(), [](const Access& access)
    { return access.Handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D; });
    Require(texture != frame.RasterAccesses.end() && texture->State == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        "Masked alpha texture not declared for raster");
    ScratchImage alpha;
    ThrowIfFailed(DirectX::CaptureTexture(queue.GetCommandQueue().Get(), texture->Handle.Get(), false, alpha,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON));
    const auto* base = alpha.GetImage(0, 0, 0);
    const auto* mip = alpha.GetImage(1, 0, 0);
    Require(base && mip && base->pixels[3] == 73 && base->pixels[base->rowPitch + 3] == 191 && mip->pixels[3] == 42,
        "Uploaded alpha subresources or row pitches are wrong");

    auto nativeAlphaSlot = std::make_shared<Slot>();
    auto nativeAlpha = MakeSnapshot(stamp);
    nativeAlpha.AlphaTextures.resize(1);
    nativeAlpha.AlphaTextures[0].Mips.clear();
    nativeAlpha.AlphaTextures[0].SecondaryView = texture->View;
    nativeAlpha.AlphaTextures[0].SecondaryView.Owner = slot;
    nativeAlpha.Materials[1].AlphaTextureIndex = 0;
    nativeAlpha.Groups = { {1} };
    Prepare(nativeAlphaSlot, device, secondaryHeap, stamp, nativeAlpha, newIdentity);
    const auto& borrowed = nativeAlphaSlot->Get();
    Require(borrowed.Groups[0].Reason == Rejection::None &&
        std::none_of(borrowed.UploadAccesses.begin(), borrowed.UploadAccesses.end(), [](const Access& access)
        { return access.Handle->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D; }) &&
        std::any_of(borrowed.RasterAccesses.begin(), borrowed.RasterAccesses.end(), [&](const Access& access)
        { return access.Handle.Get() == texture->Handle.Get() && access.State == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }),
        "Secondary-owned alpha did not preserve its canonical texture read");
    nativeAlphaSlot.reset();
    nativeAlpha.AlphaTextures[0].SecondaryView.Owner.reset();

    const FrameStamp next{ FrameSubmissionId{2}, ProducerInvocationId{2}, 22 };
    Snapshot empty;
    empty.Stamp = next;
    empty.Groups = {{}};
    Prepare(slot, device, secondaryHeap, next, empty, newIdentity);
    Require(slot->Get().Stamp == next && slot->Get().Groups[0].Reason == Rejection::None &&
        slot->Get().UploadAccesses.empty(), "Retired slot could not prepare an empty group");
    std::cout << "Scene replica dedup, native ranges, group rejection, GPU uploads and slot retirement passed\n";
}
}

int main(int argc, char** argv)
{
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
        CheckDebug(*info.Get());
        TestReplica(device);
        device.GetCommandQueue()->Flush();
        CheckDebug(*info.Get());
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
