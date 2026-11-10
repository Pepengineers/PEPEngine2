#include "Engine.RendererDX12/GDX12ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12DeviceFactory.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <d3d12sdklayers.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static GDX12ShadowOutput MakeLocal(UINT entity, uint64_t generation, UINT slices, bool point)
{
    GDX12LocalLightConstants light;
    light.EntityId = entity;
    SetLocalShadowGeneration(light, generation);
    light.ShadowFirstSlice = slices;
    light.Type = point ? 1 : 2;
    light.ShadowFlags = 1;
    BuildLocalLightShadowMatrices(light);
    return BuildLocalShadowOutput(light, {}, 17, false);
}

static void CheckLayouts()
{
    const auto point = MakeLocal(2, 10, 2, true);
    const auto spot = MakeLocal(3, 11, 0, false);
    const auto primaryPoint = MakeLocal(2, 10, 0, true);
    const auto primarySpot = MakeLocal(3, 11, 7, false);
    const auto untouched = MakeLocal(4, 12, 6, false);
    const auto onlySpot = GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { spot });
    Require(onlySpot && onlySpot->Layers() == 1 && onlySpot->Extent() == 256, "Spot payload must have one layer");
    const auto onlyPoint = GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { point });
    Require(onlyPoint && onlyPoint->Layers() == 6, "Point payload must have six layers");
    const auto mixed = GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { spot, point });
    Require(mixed && mixed->Layers() == 7 && mixed->SourceViews()[1].Region.Slice == 2 &&
        mixed->SourceViews()[6].Region.Slice == 7, "Source atlas holes must not enter the compact payload");
    const auto remapped = mixed->ResolveDestination({ primaryPoint, untouched, primarySpot });
    Require(remapped && remapped->size() == 7 && remapped->front().Region.Slice == 7 &&
        (*remapped)[1].Region.Slice == 0 && remapped->back().Region.Slice == 5,
        "A changed atlas layout must remap by stable key and preserve payload order");
    Require(!mixed->ResolveDestination({ primarySpot }), "A missing point must reject the entire restore mapping");
    auto recycled = primaryPoint;
    ++recycled.Light.Generation;
    Require(!mixed->ResolveDestination({ recycled, primarySpot }), "A reused ECS slot must not claim the old payload");
    auto stale = primaryPoint;
    ++stale.Views[5].CasterRevision;
    Require(!mixed->ResolveDestination({ stale, primarySpot }), "One stale point face must reject the full group");
    auto overlap = untouched;
    overlap.Views[0].Region.Slice = 5;
    Require(!mixed->ResolveDestination({ primaryPoint, overlap, primarySpot }),
        "Restore must never overwrite a slice owned by an unselected light");
    auto partial = point;
    partial.Views[5].Region = {};
    const auto outOfRange = MakeLocal(2, 10, D3D12_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION - 5, true);
    Require(!GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { partial }) &&
        !GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { spot, spot }) &&
        !GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, { outOfRange }) &&
        !GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Local, {}),
        "Partial, duplicate, out-of-range and empty publications must not create a transfer");

    auto sun = BuildSunShadowOutput({}, 1, 17, false);
    const auto sunLayout = GDX12ShadowTransferLayout::Build(GDX12ShadowAtlas::Sun, { sun });
    Require(sunLayout && sunLayout->Extent() == 2048 && sunLayout->Layers() == 4 &&
        sunLayout->SourceViews()[3].Region.X == 2048 && sunLayout->SourceViews()[3].Region.Y == 2048,
        "Sun must pack all four quadrants");
    sun.Views[3].Region.X = 0;
    Require(!sunLayout->ResolveDestination({ sun }), "Sun must reject an overlapping quadrant");
    std::cout << "Shadow transfer layout: whole groups, compact layers, stable-key relocation and stale rejection passed.\n";
}

static std::shared_ptr<GDX12Resource> CreateTexture(GDX12Device& device, UINT extent, UINT layers, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags)
{
    auto texture = std::make_shared<GDX12Resource>();
    const auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, extent, extent, static_cast<UINT16>(layers), 1, 1, 0, flags);
    texture->D3DResource = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
    return texture;
}

static void MakeDepthViews(GDX12Device& device, GDX12Resource& depth, GDX12DescriptorHeap& heap, UINT first,
    UINT slices, GDX12ShadowAtlas atlas)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_D32_FLOAT;
    view.ViewDimension = atlas == GDX12ShadowAtlas::Sun ? D3D12_DSV_DIMENSION_TEXTURE2D : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    for (UINT slice = 0; slice < slices; ++slice)
    {
        if (atlas == GDX12ShadowAtlas::Local)
        {
            view.Texture2DArray.FirstArraySlice = slice;
            view.Texture2DArray.ArraySize = 1;
        }
        device.GetDevice()->CreateDepthStencilView(depth.D3DResource.Get(), &view, heap.GetCPUHandle(first + slice));
    }
}

struct DepthSample
{
    UINT Slice, X, Y;
    float Value;
};

static void CheckReadback(GDX12Device& device, GDX12Resource& texture, const std::vector<DepthSample>& samples)
{
    const auto desc = texture.D3DResource->GetDesc();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(desc.DepthOrArraySize);
    UINT64 bytes = 0;
    device.GetDevice()->GetCopyableFootprints(&desc, 0, desc.DepthOrArraySize, 0, footprints.data(), nullptr, nullptr, &bytes);
    const auto readback = device.CreateResource(D3D12_HEAP_TYPE_READBACK, CD3DX12_RESOURCE_DESC::Buffer(bytes),
        D3D12_RESOURCE_STATE_COPY_DEST);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({ texture.GetCopySourceBarrier() });
    for (UINT i = 0; i < desc.DepthOrArraySize; ++i)
    {
        const CD3DX12_TEXTURE_COPY_LOCATION source(texture.D3DResource.Get(), i);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(readback.Get(), footprints[i]);
        commands->GetCommandList()->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    commands->ResourceBarrier({ texture.GetCommonBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    void* mapped = nullptr;
    const D3D12_RANGE range{ 0, static_cast<SIZE_T>(bytes) };
    ThrowIfFailed(readback->Map(0, &range, &mapped));
    bool match = true;
    for (const auto& sample : samples)
    {
        const auto& footprint = footprints[sample.Slice];
        const auto offset = footprint.Offset + UINT64(sample.Y) * footprint.Footprint.RowPitch + UINT64(sample.X) * sizeof(float);
        float value;
        std::memcpy(&value, static_cast<const unsigned char*>(mapped) + offset, sizeof(value));
        match &= value == sample.Value;
    }
    const D3D12_RANGE noWrites{ 0, 0 };
    readback->Unmap(0, &noWrites);
    Require(match, "Shadow transfer readback differs from the expected selected depth or unselected sentinel");
}

static void CheckAtlas(GDX12Device& device, GDX12ShadowDepthTransfer& transfer, GDX12ShadowAtlas atlas, UINT atlasSlices,
    const std::vector<GDX12ShadowOutput>& selected, const std::vector<GDX12ShadowOutput>& primary)
{
    const auto layout = GDX12ShadowTransferLayout::Build(atlas, selected);
    Require(layout.has_value(), "Invalid fixture layout");
    const bool sun = atlas == GDX12ShadowAtlas::Sun;
    const UINT atlasExtent = layout->Extent() * (sun ? 2u : 1u);
    auto source = CreateTexture(device, atlasExtent, atlasSlices, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto destination = CreateTexture(device, atlasExtent, atlasSlices, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto packed = CreateTexture(device, layout->Extent(), layout->Layers(), DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto imported = CreateTexture(device, layout->Extent(), layout->Layers(), DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE);
    auto pack = transfer.PreparePack(*layout, source, packed);
    auto stale = primary;
    ++stale[0].SceneRevision;
    Require(!transfer.PrepareRestore(*layout, stale, destination, imported), "Stale group must fall back before recording");
    auto restore = transfer.PrepareRestore(*layout, primary, destination, imported);
    Require(static_cast<bool>(restore), "Matching fixture inputs must admit restore");
    const auto mapped = layout->ResolveDestination(primary);
    Require(mapped.has_value(), "Fixture lost its destination mapping");
    GDX12ShadowDepthTransfer::InitializedSlices initialized;
    for (const auto& view : *mapped) initialized.set(view.Region.Slice);
    auto repeat = transfer.PrepareRestore(*layout, primary, destination, imported, initialized);
    Require(static_cast<bool>(repeat), "An initialized atlas must admit another restore");

    GDX12DescriptorHeap views(&device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 * atlasSlices, D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
    MakeDepthViews(device, *source, views, 0, atlasSlices, atlas);
    MakeDepthViews(device, *destination, views, atlasSlices, atlasSlices, atlas);
    auto* queue = device.GetCommandQueue();
    auto* commands = queue->GetCommandList();
    bool wrongBindingRejected = false;
    try { transfer.RecordPack(*commands, *pack, destination->D3DResource.Get(), packed->D3DResource.Get()); }
    catch (const std::logic_error&) { wrongBindingRejected = true; }
    Require(wrongBindingRejected, "Pack must reject a bound resource that differs from the prepared descriptors");
    commands->ResourceBarrier({ source->GetDepthWriteBarrier(), destination->GetDepthWriteBarrier() });
    auto* list = commands->GetCommandList().Get();
    constexpr float sentinel = 0.0625f;
    for (UINT slice = 0; slice < atlasSlices; ++slice)
    {
        list->ClearDepthStencilView(views.GetCPUHandle(slice), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->ClearDepthStencilView(views.GetCPUHandle(atlasSlices + slice), D3D12_CLEAR_FLAG_DEPTH, sentinel, 0, 0, nullptr);
    }
    std::vector<DepthSample> expectedPacked;
    for (const auto& view : layout->SourceViews())
    {
        const auto& r = view.Region;
        const float depth = view.PayloadLayer == 0 ? 1.0f : static_cast<float>(view.PayloadLayer + 1) / 16.0f;
        const D3D12_RECT rect{ static_cast<LONG>(r.X), static_cast<LONG>(r.Y),
            static_cast<LONG>(r.X + r.Width), static_cast<LONG>(r.Y + r.Height) };
        const D3D12_RECT corner{ rect.right - 1, rect.bottom - 1, rect.right, rect.bottom };
        const auto dsv = views.GetCPUHandle(r.Slice);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth, 0, 1, &rect);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.9375f, 0, 1, &corner);
        expectedPacked.push_back({ view.PayloadLayer, 0, 0, depth });
        expectedPacked.push_back({ view.PayloadLayer, r.Width - 1, r.Height - 1, 0.9375f });
    }
    commands->ResourceBarrier({ source->GetNonPixelShaderResourceBarrier(), packed->GetUnorderedAccessBarrier() });
    transfer.RecordPack(*commands, *pack, source->D3DResource.Get(), packed->D3DResource.Get());
    commands->ResourceBarrier({ source->GetCommonBarrier(), packed->GetCopySourceBarrier(), imported->GetCopyDestBarrier() });
    list->CopyResource(imported->D3DResource.Get(), packed->D3DResource.Get());
    commands->ResourceBarrier({ packed->GetCommonBarrier(), imported->GetSRVBarrier() });
    transfer.RecordRestore(*commands, *restore, destination->D3DResource.Get(), imported->D3DResource.Get());
    for (UINT slice = 0; slice < atlasSlices; ++slice)
        if (initialized[slice])
            list->ClearDepthStencilView(views.GetCPUHandle(atlasSlices + slice), D3D12_CLEAR_FLAG_DEPTH, sentinel, 0, 0, nullptr);
    transfer.RecordRestore(*commands, *repeat, destination->D3DResource.Get(), imported->D3DResource.Get());
    commands->ResourceBarrier({ imported->GetCommonBarrier(), destination->GetCommonBarrier() });
    queue->ExecuteCommandList(commands);
    queue->Flush();

    CheckReadback(device, *imported, expectedPacked);
    std::vector<DepthSample> expectedDepth;
    for (const auto& view : *mapped)
    {
        const auto& r = view.Region;
        expectedDepth.push_back({ r.Slice, r.X, r.Y, expectedPacked[2 * view.PayloadLayer].Value });
        expectedDepth.push_back({ r.Slice, r.X + r.Width - 1, r.Y + r.Height - 1, 0.9375f });
    }
    for (UINT slice = 0; slice < atlasSlices; ++slice)
        if (!initialized[slice])
            for (UINT y = 0; y < atlasExtent; ++y)
                for (UINT x = 0; x < atlasExtent; ++x)
                    expectedDepth.push_back({ slice, x, y, sentinel });
    CheckReadback(device, *destination, expectedDepth);
}

static void CheckDebug(ID3D12InfoQueue& info)
{
    for (UINT64 i = 0; i < info.GetNumStoredMessagesAllowedByRetrievalFilter(); ++i)
    {
        SIZE_T size = 0;
        ThrowIfFailed(info.GetMessage(i, nullptr, &size));
        std::vector<BYTE> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(info.GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) throw std::runtime_error(message->pDescription);
    }
}

int main(int argc, char** argv)
{
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    try
    {
        const std::string_view mode = argc == 2 ? argv[1] : "";
        Require(argc == 1 || (argc == 2 && (mode == "--cpu" || mode == "--gpu-validation")), "Unknown test argument");
        CheckLayouts();
        if (mode == "--cpu") return 0;

        ComPtr<ID3D12Debug> debug;
        ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
        debug->EnableDebugLayer();
        if (mode == "--gpu-validation")
        {
            ComPtr<ID3D12Debug1> validation;
            ThrowIfFailed(debug.As(&validation));
            validation->SetEnableGPUBasedValidation(TRUE);
        }
        GDX12Device device;
        ThrowIfFailed(device.Initialize(GDX12DeviceFactory::GetMostPerformantAdapter()));
        ComPtr<ID3D12InfoQueue> info;
        ThrowIfFailed(device.GetDevice().As(&info));
        GDX12ShadowDepthTransfer transfer(device);
        CheckAtlas(device, transfer, GDX12ShadowAtlas::Local, 3,
            { MakeLocal(3, 11, 2, false) }, { MakeLocal(3, 11, 1, false), MakeLocal(4, 12, 0, false) });
        CheckAtlas(device, transfer, GDX12ShadowAtlas::Local, 9,
            { MakeLocal(3, 11, 0, false), MakeLocal(2, 10, 2, true) },
            { MakeLocal(2, 10, 0, true), MakeLocal(3, 11, 7, false), MakeLocal(4, 12, 6, false) });
        const auto sun = BuildSunShadowOutput({}, 1, 17, false);
        CheckAtlas(device, transfer, GDX12ShadowAtlas::Sun, 1, { sun }, { sun });
        CheckDebug(*info.Get());
        GDX12ShaderCompiler::Shutdown();
        std::cout << "Typed pack/copy/SV_Depth restore passed with D3D12 debug layer.\n";
        return 0;
    }
    catch (const DxException& error) { std::wcerr << error.ToString() << '\n'; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    return 1;
}
