#include "SceneAO.h"
#include "Common/ConsoleVariables.h"
#include "Engine.Core/Logging/Logger.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "../../../../External/External.Libraries/XeGTAO/XeGTAO.h"
#define FFX_CACAO_ENABLE_D3D12
#include "../../../../External/External.Libraries/cacao/inc/ffx_cacao_impl.h"
#include <cassert>
#include <cstring>
namespace BistroRender
{
namespace
{
constexpr State Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

AutoConsoleVariable<bool> AsyncAO(L"App.Async.AO", false,
                                  L"Run ambient occlusion passes on the local COMPUTE queue.",
                                  EConsoleVariableFlags::Default);

AutoConsoleVariable<int> CrossSync(L"App.AO.CrossSync", 1,
    L"Remote AO: 1 exact current, 0 exact previous with reprojection (first uses current).",
    EConsoleVariableFlags::Default, [](int value) { return value == 0 || value == 1; });
AutoConsoleVariable<bool> BlurOnSecondary(L"App.AO.BlurOnSecondary", false,
    L"Run SSAO/HBAO blur or XeGTAO denoise on GPU1. CACAO filters inside its SDK context.");

void CheckCACAO(FFX_CACAO_Status result)
{
    if (result != FFX_CACAO_STATUS_OK)
        throw std::runtime_error("CACAO preparation/recording failed");
}
void Expand(Matrix& p, float x, float y)
{
    p._11 *= x;
    p._22 *= y;
    p._31 = x * p._31 + (1 - x);
    p._32 = y * p._32 - (1 - y);
}
const char* AOName(int implementation)
{
    constexpr const char* names[] = {"SSAO", "HBAO", "XeGTAO", "CACAO"};
    return names[implementation];
}

bool PadCacao(UINT width, UINT height) { return (std::max)(width, height) < 16; }

FFX_CACAO_Settings CacaoSettings(const GDX12AOSettings& s)
{
    auto settings = FFX_CACAO_DEFAULT_SETTINGS;
    settings.radius = s.RadiusCm * .01f;
    settings.shadowMultiplier = s.Intensity;
    settings.shadowPower = s.CACAOPower;
    settings.shadowClamp = s.CACAOClamp;
    settings.horizonAngleThreshold = s.HorizonBias;
    settings.fadeOutFrom = s.FadeStartCm * .01f;
    settings.fadeOutTo = s.FadeEndCm * .01f;
    settings.qualityLevel = FFX_CACAO_Quality(s.CACAOQuality);
    settings.adaptiveQualityLimit = s.CACAOAdaptiveLimit;
    settings.blurPassCount = s.DenoisePasses;
    settings.sharpness = s.Sharpness;
    settings.detailShadowStrength = s.CACAODetail;
    return settings;
}

void InitCacaoScreen(FFX_CACAO_D3D12Context* context, ID3D12Resource* depth,
                     ID3D12Resource* normals, ID3D12Resource* output)
{
    FFX_CACAO_D3D12ScreenSizeInfo info{};
    info.width = UINT(depth->GetDesc().Width);
    info.height = depth->GetDesc().Height;
    info.depthBufferResource = depth;
    info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
    info.normalBufferResource = normals;
    info.normalBufferSrvDesc = info.depthBufferSrvDesc;
    info.normalBufferSrvDesc.Format = normals->GetDesc().Format;
    info.outputResource = output;
    info.outputUavDesc.Format = output->GetDesc().Format;
    info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(context, &info));
}

void DrawCacao(FFX_CACAO_D3D12Context* context, ID3D12GraphicsCommandList* cmd,
               const Matrix& projection, ID3D12Resource* output)
{
    FFX_CACAO_Matrix4x4 proj{}, identity{};
    std::memcpy(&proj, &projection, sizeof(proj));
    proj.elements[3][2] *= .01f;
    for (UINT i = 0; i < 4; ++i)
        identity.elements[i][i] = 1;
    CheckCACAO(FFX_CACAO_D3D12Draw(context, cmd, &proj, &identity));
    const auto outputState = cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_GENERIC_READ;
    const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd->ResourceBarrier(1, &barrier);
}

struct CacaoContext
{
    std::vector<std::byte> Memory;
    FFX_CACAO_D3D12Context* Context = nullptr;
    bool ScreenReady = false;

    void Initialize(ID3D12Device* device, ID3D12Resource* depth, ID3D12Resource* normals, ID3D12Resource* output)
    {
        Memory.resize(FFX_CACAO_D3D12GetContextSize());
        auto* context = reinterpret_cast<FFX_CACAO_D3D12Context*>(Memory.data());
        CheckCACAO(FFX_CACAO_D3D12InitContext(context, device, 1));
        Context = context;
        InitCacaoScreen(Context, depth, normals, output);
        ScreenReady = true;
    }

    ~CacaoContext()
    {
        if (ScreenReady) FFX_CACAO_D3D12DestroyScreenSizeDependentResources(Context);
        if (Context) FFX_CACAO_D3D12DestroyContext(Context);
    }
};
constexpr State CopyRead = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr State CopyWrite = D3D12_RESOURCE_STATE_COPY_DEST;

void CopyTexture(ID3D12GraphicsCommandList* cmd, const D3D12_TEXTURE_COPY_LOCATION& target,
                 const D3D12_TEXTURE_COPY_LOCATION& source)
{
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
}
struct SharedImage
{
    QueueSubmission::TransferBuffer Buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Layout{};

    void Initialize(QueueSubmission& submit, const D3D12_RESOURCE_DESC& desc)
    {
        uint64_t bytes = 0, primaryBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT primary{};
        submit.Device(1).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &Layout, nullptr, nullptr, &bytes);
        submit.Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &primary, nullptr, nullptr, &primaryBytes);
        if (!bytes || bytes == UINT64_MAX || bytes != primaryBytes ||
            Layout.Footprint.RowPitch != primary.Footprint.RowPitch)
            throw std::runtime_error("AO adapters need matching texture footprints");
        Buffer = submit.CreateTransferBuffer(bytes);
    }
    ID3D12Resource* Resource(Id device) const { return Buffer.Resources.at(device).Get(); }

    void Export(ID3D12GraphicsCommandList* cmd, Id device, ID3D12Resource* source) const
    {
        CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(Resource(device), Layout),
                    CD3DX12_TEXTURE_COPY_LOCATION(source, 0u));
    }
    void Import(ID3D12GraphicsCommandList* cmd, Id device, ID3D12Resource* target) const
    {
        CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(target, 0u),
                    CD3DX12_TEXTURE_COPY_LOCATION(Resource(device), Layout));
    }
};
} // namespace

struct SceneAO::Remote
{
    std::shared_ptr<QueueSubmission> Submit;
    GDX12ResourceHandle Depth;
    std::array<GDX12ResourceHandle, 2> Visibility;
    GDX12Texture InputDepth;
    std::vector<ID3D12Resource*> Bindings;
    SharedImage DepthTransfer, VisibilityTransfer, NormalTransfer, EdgeTransfer;
    GDX12ResourceHandle Normals, Pyramid, Edges;
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline, Blur, Prefilter;
    std::unique_ptr<GDX12DescriptorHeap> Heap;
    GDX12UploadBuffer<GDX12AmbientOcclusionPass::ShaderConstants> Constants;
    GDX12UploadBuffer<XeGTAO::GTAOConstants> Projection;
    std::unique_ptr<GDX12UploadBuffer<XeGTAO::GTAOConstants>> HistoryProjection;
    CacaoContext Cacao;
    Matrix CacaoProjection;
    Schedule ScheduleGPU1;
    Executor GPU1;
    CompletionToken DepthReady, VisibilityReady;
    std::array<CompletionToken, 3> Readers{};
    XMFLOAT2 Jitter{}, DepthUnpack{};
    uint64_t Version = 0;
    UINT Width, Height, FullWidth, FullHeight;
    bool Delayed, SecondaryBlur, Half;
    int Implementation, DenoisePasses, BlurPasses, LocalXePasses;

    bool Xe() const { return Implementation == 2; }
    bool UsesNormals() const { return Implementation != 1; }
    bool CacaoPadded() const { return Implementation == 3 && PadCacao(Width, Height); }

    Remote(std::shared_ptr<QueueSubmission> submit, UINT width, UINT height, UINT fullWidth, UINT fullHeight,
           bool delayed, bool secondaryBlur, const GDX12AOSettings& settings)
        : Submit(std::move(submit)), Constants(&Submit->Device(1), 1), Projection(&Submit->Device(1), 1),
          GPU1(Submit, 1, 1, 1), Width(width), Height(height), FullWidth(fullWidth), FullHeight(fullHeight),
          Delayed(delayed), SecondaryBlur(secondaryBlur), Half(settings.Resolution == GDX12AOResolution::Half),
          Implementation(settings.Implementation), DenoisePasses(settings.DenoisePasses),
          BlurPasses(secondaryBlur && Implementation != 3 ? DenoisePasses : 0),
          LocalXePasses(Xe() && !secondaryBlur ? DenoisePasses : 0)
    {
        if (Xe())
        {
            if (Delayed && LocalXePasses)
                HistoryProjection = std::make_unique<GDX12UploadBuffer<XeGTAO::GTAOConstants>>(&Submit->Device(0), 1);
            InitializeXe();
            return;
        }
        auto& device = Submit->Device(1);
        auto* native = device.GetDevice().Get();
        const UINT transferWidth = CacaoPadded() ? 16 : Width;
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, transferWidth, Height, 1, 1);
        DepthTransfer.Initialize(*Submit, desc);
        VisibilityTransfer.Initialize(*Submit, desc);
        Depth = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Visibility[0] = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
        if (BlurPasses)
            Visibility[1] = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);

        Graph graph;
        graph.Device = 1;
        graph.FrameBoundary = false;
        graph.TerminalQueue = Queue::Compute;
        graph.Resources = {{"shared AO depth", 0, 0, false, true}, {"GPU1 AO depth", 0, 0},
                           {"GPU1 AO visibility", 0, 0, true}, {"shared AO visibility", 0, 0, false, true}};
        Bindings = {DepthTransfer.Resource(1), Depth.Get(), Visibility[0].Get(), VisibilityTransfer.Resource(1)};
        graph.Operations = {
            {"GPU1 import AO inputs", {{0, Mode::Read, CopyRead}, {1, Mode::Write, CopyWrite}}},
            {"GPU1 compute AO visibility", {{1, Mode::Read, Read}, {2, Mode::Write, UnorderedAccess}}}};
        constexpr Id pingPong[] = {2, 4};
        if (BlurPasses)
        {
            graph.Resources.push_back({"GPU1 AO blur scratch", 0, 0, true});
            Bindings.push_back(Visibility[1].Get());
        }
        Id normalId = Invalid;
        if (UsesNormals())
        {
            const auto normalDesc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT, transferWidth, Height, 1, 1);
            Normals = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, normalDesc, D3D12_RESOURCE_STATE_COMMON);
            NormalTransfer.Initialize(*Submit, normalDesc);
            const Id shared = Id(graph.Resources.size());
            normalId = shared + 1;
            graph.Resources.push_back({"shared AO normals", 0, 0, false, true});
            graph.Resources.push_back({"GPU1 AO normals", 0, 0});
            Bindings.push_back(NormalTransfer.Resource(1));
            Bindings.push_back(Normals.Get());
            graph.Operations[0].Accesses.push_back({shared, Mode::Read, CopyRead});
            graph.Operations[0].Accesses.push_back({normalId, Mode::Write, CopyWrite});
            graph.Operations[1].Accesses.push_back({normalId, Mode::Read, Read});
        }
        for (int pass = 0; pass < BlurPasses; ++pass)
        {
            graph.Operations.push_back({"GPU1 AO bilateral blur", {{1, Mode::Read, Read},
                {pingPong[pass % 2], Mode::Read, Read}, {pingPong[1 - pass % 2], Mode::Write, UnorderedAccess}}});
            if (normalId != Invalid)
                graph.Operations.back().Accesses.push_back({normalId, Mode::Read, Read});
        }
        graph.Operations.push_back({"GPU1 export AO visibility",
            {{pingPong[BlurPasses % 2], Mode::Read, CopyRead}, {3, Mode::Write, CopyWrite}}});
        for (auto& operation : graph.Operations) operation.Target = Queue::Compute;
        ScheduleGPU1 = Compile(graph);
        assert(ScheduleGPU1.Packets.front().Operation == 0);

        if (Implementation == 3)
        {
            Cacao.Initialize(native, Depth.Get(), Normals.Get(), Visibility[0].Get());
            return;
        }

        GDX12RootSignatureDesc root;
        root.Constants = {8};
        root.NumSingleCBVSlots = 3;
        root.NumSingleSRVSlots = BlurPasses ? 3 : UsesNormals() ? 2 : 1;
        root.NumSingleUAVSlots = 1;
        if (UsesNormals())
            root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        Root = std::make_unique<GDX12RootSignature>(&device, root);
        Pipeline = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
            SHADERS_FOLDER "AmbientOcclusion.hlsl", UsesNormals() ? "SSAOCS" : "HBAOHalfDepthCS");
        if (BlurPasses)
            Blur = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
                SHADERS_FOLDER "AmbientOcclusion.hlsl", UsesNormals() ? "BlurCS" : "BlurHalfDepthCS");
        Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            6, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        native->CreateShaderResourceView(Depth.Get(), nullptr, Heap->GetCPUHandle(0));
        native->CreateUnorderedAccessView(Visibility[0].Get(), nullptr, nullptr, Heap->GetCPUHandle(1));
        if (UsesNormals())
            native->CreateShaderResourceView(Normals.Get(), nullptr, Heap->GetCPUHandle(5));
        if (BlurPasses)
        {
            native->CreateUnorderedAccessView(Visibility[1].Get(), nullptr, nullptr, Heap->GetCPUHandle(2));
            for (UINT i = 0; i < 2; ++i)
                native->CreateShaderResourceView(Visibility[i].Get(), nullptr, Heap->GetCPUHandle(3 + i));
        }
    }

    enum XeResource : Id
    {
        SharedDepth, SharedNormals, LocalDepth, LocalNormals, DepthPyramid, Terms0, Terms1, LocalEdges,
        SharedTerms, SharedEdges
    };

    void InitializeXe()
    {
        auto& device = Submit->Device(1);
        auto* native = device.GetDevice().Get();
        const auto texture = [this](DXGI_FORMAT format) {
            return CD3DX12_RESOURCE_DESC::Tex2D(format, Width, Height, 1, 1);
        };
        const auto depth = texture(DXGI_FORMAT_R32_FLOAT);
        const auto normals = texture(DXGI_FORMAT_R16G16B16A16_FLOAT);
        auto terms = texture(DXGI_FORMAT_R8_UINT);
        auto edges = texture(DXGI_FORMAT_R8_UNORM);
        for (auto format : {depth.Format, terms.Format, edges.Format})
        {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
            ThrowIfFailed(native->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
            if (!(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
                throw std::runtime_error("XeGTAO GPU1 requires typed UAV stores for its depth, terms and edges");
        }
        DepthTransfer.Initialize(*Submit, depth);
        NormalTransfer.Initialize(*Submit, normals);
        VisibilityTransfer.Initialize(*Submit, terms);
        if (LocalXePasses) EdgeTransfer.Initialize(*Submit, edges);

        const auto allocate = [&device](const D3D12_RESOURCE_DESC& desc) {
            return device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
        };
        Depth = allocate(depth);
        Normals = allocate(normals);
        terms.Flags = edges.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Visibility[0] = allocate(terms);
        Visibility[1] = allocate(terms);
        Edges = allocate(edges);
        Pyramid = allocate(CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT,
            (Width + 15) / 16 * 16, (Height + 15) / 16 * 16, 1, 5, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));

        GDX12RootSignatureDesc root;
        root.Constants = {8};
        root.NumSingleCBVSlots = 3;
        root.NumSingleSRVSlots = 5;
        root.NumSingleUAVSlots = 9;
        root.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        Root = std::make_unique<GDX12RootSignature>(&device, root);
        const auto kernel = [&](const char* entry) {
            return GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
                SHADERS_FOLDER "AmbientOcclusion.hlsl", entry);
        };
        Prefilter = kernel("XePrefilterCS");
        Pipeline = kernel("XeMainCS");
        if (BlurPasses) Blur = kernel("XeDenoiseCS");
        Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 14,
                                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        ID3D12Resource* srvs[] = {Depth.Get(), Normals.Get(), Pyramid.Get(),
                                  Visibility[0].Get(), Visibility[1].Get(), Edges.Get()};
        for (UINT i = 0; i < std::size(srvs); ++i)
            native->CreateShaderResourceView(srvs[i], nullptr, Heap->GetCPUHandle(i));
        D3D12_UNORDERED_ACCESS_VIEW_DESC mip{};
        mip.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        mip.Format = DXGI_FORMAT_R32_FLOAT;
        for (UINT i = 0; i < 5; ++i)
        {
            mip.Texture2D.MipSlice = i;
            native->CreateUnorderedAccessView(Pyramid.Get(), nullptr, &mip, Heap->GetCPUHandle(6 + i));
        }
        for (UINT i = 0; i < 2; ++i)
            native->CreateUnorderedAccessView(Visibility[i].Get(), nullptr, nullptr, Heap->GetCPUHandle(11 + i));
        native->CreateUnorderedAccessView(Edges.Get(), nullptr, nullptr, Heap->GetCPUHandle(13));

        Graph graph;
        graph.Device = 1;
        graph.FrameBoundary = false;
        graph.TerminalQueue = Queue::Compute;
        graph.Resources = {{"Xe shared depth", 0, 0, false, true}, {"Xe shared normals", 0, 0, false, true},
            {"Xe depth", 0, 0}, {"Xe normals", 0, 0}, {"Xe depth pyramid", 0, 0, true},
            {"Xe terms 0", 0, 0, true}, {"Xe terms 1", 0, 0, true}, {"Xe edges", 0, 0, true},
            {"Xe shared terms", 0, 0, false, true}};
        Bindings = {DepthTransfer.Resource(1), NormalTransfer.Resource(1), Depth.Get(), Normals.Get(),
                      Pyramid.Get(), Visibility[0].Get(), Visibility[1].Get(), Edges.Get(),
                      VisibilityTransfer.Resource(1)};
        graph.Operations = {
            {"XeGTAO GPU1 import inputs", {{SharedDepth, Mode::Read, CopyRead}, {SharedNormals, Mode::Read, CopyRead},
                {LocalDepth, Mode::Write, CopyWrite}, {LocalNormals, Mode::Write, CopyWrite}}},
            {"XeGTAO GPU1 depth pyramid", {{LocalDepth, Mode::Read, Read}, {DepthPyramid, Mode::Write, UnorderedAccess}}},
            {"XeGTAO GPU1 main", {{LocalDepth, Mode::Read, Read}, {LocalNormals, Mode::Read, Read},
                {DepthPyramid, Mode::Read, Read}, {Terms0, Mode::Write, UnorderedAccess},
                {LocalEdges, Mode::Write, UnorderedAccess}}}};
        for (int pass = 0; pass < BlurPasses; ++pass)
            graph.Operations.push_back({"XeGTAO GPU1 denoise", {{LocalDepth, Mode::Read, Read},
                {Id(Terms0 + pass % 2), Mode::Read, Read}, {LocalEdges, Mode::Read, Read},
                {Id(Terms0 + 1 - pass % 2), Mode::Write, UnorderedAccess}}});
        graph.Operations.push_back({"XeGTAO GPU1 export terms",
            {{Id(Terms0 + BlurPasses % 2), Mode::Read, CopyRead}, {SharedTerms, Mode::Write, CopyWrite}}});
        if (LocalXePasses)
        {
            graph.Resources.push_back({"Xe shared edges", 0, 0, false, true});
            Bindings.push_back(EdgeTransfer.Resource(1));
            graph.Operations.back().Accesses.push_back({LocalEdges, Mode::Read, CopyRead});
            graph.Operations.back().Accesses.push_back({SharedEdges, Mode::Write, CopyWrite});
        }
        for (auto& operation : graph.Operations) operation.Target = Queue::Compute;
        ScheduleGPU1 = Compile(graph);
        assert(ScheduleGPU1.Packets.front().Operation == 0);
    }

    void ExecuteXe(CompletionToken depthReady)
    {
        GPU1.Execute(ScheduleGPU1, Bindings, [&](Id operation, uint32_t, auto* cmd) {
            if (operation == 0)
            {
                DepthTransfer.Import(cmd, 1, Depth.Get());
                NormalTransfer.Import(cmd, 1, Normals.Get());
                return;
            }
            if (operation == Id(3 + BlurPasses))
            {
                VisibilityTransfer.Export(cmd, 1, Visibility[BlurPasses % 2].Get());
                if (LocalXePasses) EdgeTransfer.Export(cmd, 1, Edges.Get());
                return;
            }
            const auto srv = [&](UINT reg, UINT descriptor) {
                cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(reg), Heap->GetGPUHandle(descriptor));
            };
            const auto uav = [&](UINT reg, UINT descriptor) {
                cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(reg), Heap->GetGPUHandle(descriptor));
            };
            auto* heap = Heap->GetHeap().Get();
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
            cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(3), Projection.GetElementAddress(0));
            srv(0, 0);
            if (operation == 1)
            {
                cmd->SetPipelineState(Prefilter.Get());
                for (UINT mip = 0; mip < 5; ++mip) uav(2 + mip, 6 + mip);
                cmd->Dispatch((Width + 15) / 16, (Height + 15) / 16, 1);
            }
            else if (operation == 2)
            {
                cmd->SetPipelineState(Pipeline.Get());
                cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(2), Constants.GetElementAddress(0));
                srv(1, 1);
                srv(2, 2);
                uav(7, 11);
                uav(8, 13);
                cmd->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
            }
            else
            {
                const UINT pass = operation - 3;
                const UINT constants[8] = {0, UINT(pass + 1 == UINT(BlurPasses))};
                cmd->SetPipelineState(Blur.Get());
                cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
                srv(3, 3 + pass % 2);
                srv(4, 5);
                uav(7, 12 - pass % 2);
                cmd->Dispatch((Width + 15) / 16, (Height + 7) / 8, 1);
            }
        }, nullptr, nullptr, nullptr, {{0, depthReady}});
    }

    void RetireReaders()
    {
        for (auto token : Readers)
            if (token) Submit->Wait(token);
        Readers = {};
    }

    void NoteReader(CompletionToken token)
    {
        if (!token) return;
        auto& last = Readers.at(static_cast<size_t>(token.Target));
        if (token.Value > last.Value) last = token;
    }

    void Drain()
    {
        GPU1.Drain();
        RetireReaders();
    }

    CompletionToken Execute(CompletionToken depthReady)
    {
        DepthReady = depthReady;
        if (Xe())
        {
            ExecuteXe(depthReady);
            VisibilityReady = GPU1.Completions().back();
            return VisibilityReady;
        }
        GPU1.Execute(ScheduleGPU1, Bindings,
            [&](Id operation, uint32_t, auto* cmd) {
                if (operation == 0)
                {
                    DepthTransfer.Import(cmd, 1, Depth.Get());
                    if (UsesNormals()) NormalTransfer.Import(cmd, 1, Normals.Get());
                }
                else if (operation == 1 && Cacao.Context)
                {
                    DrawCacao(Cacao.Context, cmd, CacaoProjection, Visibility[0].Get());
                }
                else if (operation < Id(2 + BlurPasses))
                {
                    const bool blur = operation > 1;
                    const UINT axis = blur ? (operation - 2) % 2 : 0;
                    auto* heap = Heap->GetHeap().Get();
                    cmd->SetDescriptorHeaps(1, &heap);
                    cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
                    cmd->SetPipelineState(blur ? Blur.Get() : Pipeline.Get());
                    if (blur)
                    {
                        const UINT dispatch[8] = {axis};
                        cmd->SetComputeRoot32BitConstants(0, 8, dispatch, 0);
                        cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(2), Heap->GetGPUHandle(3 + axis));
                    }
                    cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(2), Constants.GetElementAddress(0));
                    cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(3), Projection.GetElementAddress(0));
                    cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(0), Heap->GetGPUHandle(0));
                    if (UsesNormals())
                        cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(1), Heap->GetGPUHandle(5));
                    cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), Heap->GetGPUHandle(blur ? 2 - axis : 1));
                    cmd->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
                }
                else
                {
                    VisibilityTransfer.Export(cmd, 1, Visibility[BlurPasses % 2].Get());
                }
            }, nullptr, nullptr, nullptr, {{0, depthReady}});
        VisibilityReady = GPU1.Completions().back();
        return VisibilityReady;
    }
};

SceneAO::SceneAO(GDX12DeviceResources& r, RenderPipelineCommonData& c, GDX12Texture* depth, GDX12Texture* normals)
    : _resources(r), _common(c)
{
    _pass.Setup(&r, &c);
    _pass.SetInputs({depth, normals});
    _pass.Initialize();
}
SceneAO::~SceneAO()
{
    _pass.ClearDenendencies();
}
bool SceneAO::SupportsRemote(const GDX12AOSettings& settings, bool depthNormals)
{
    return settings.IsActive() &&
        ((settings.Implementation == 1 && settings.Resolution == GDX12AOResolution::Half) ||
         (depthNormals && (settings.Implementation == 0 || settings.Implementation == 3 ||
          (settings.Implementation == 2 && settings.Resolution == GDX12AOResolution::Full))));
}

void SceneAO::Freeze(GDX12Texture* depth, GDX12Texture* normals, GDX12Texture* motion,
                     GDX12Texture* earlyNormals, UINT& temporalIndex,
                     const std::shared_ptr<QueueSubmission>& remote, History& history)
{
    _import.reset();
    _motion = motion;
    _earlyNormals = earlyNormals;
    _work.Commands.clear();
    _depthExport = _visibilityImport = _reproject = _importPacket = _reprojectPacket = Invalid;
    auto& p = _pass;
    p.SetInputs({depth, normals});
    if (p._output.GetWidth() != depth->GetWidth() || p._output.GetHeight() != depth->GetHeight())
        p.Resize();
    _settings = GetAmbientOcclusionSettings();
    const auto& s = _settings;
    if (!p._hasPrevious || std::memcmp(&p._previous, &s, sizeof(s)))
        _common.ResetUpscalerHistory = true;
    p._previous = s;
    p._hasPrevious = true;
    _half = s.Resolution == GDX12AOResolution::Half;
    _width = _half ? (depth->GetWidth() + 1) / 2 : depth->GetWidth();
    _height = _half ? (depth->GetHeight() + 1) / 2 : depth->GetHeight();
    auto& grid = p._grids[_half];
    const auto domain = (remote && SupportsRemote(s, earlyNormals != nullptr) && (_width > 1 || _height > 1))
        ? remote->Incarnation() : 0;
    const UINT fullWidth = depth->GetWidth(), fullHeight = depth->GetHeight();
    const bool delayed = CrossSync.GetValue() == 0;
    const bool secondaryBlur = s.Implementation != 3 && BlurOnSecondary.GetValue();
    if (!domain || history.CameraRevision != _common.HistoryRevision ||
        (history.Last && (history.Last->Submit->Incarnation() != domain || history.Last->FullWidth != fullWidth ||
                         history.Last->FullHeight != fullHeight || history.Last->Delayed != delayed ||
                         history.Last->SecondaryBlur != secondaryBlur ||
                         std::memcmp(&history.Settings, &s, sizeof(s)))))
        history.Last.reset();
    history.CameraRevision = _common.HistoryRevision;
    history.Settings = s;

    const auto createRemote = [&]() -> std::shared_ptr<Remote> {
        try
        {
            std::shared_ptr<Remote> slot(new Remote(remote, _width, _height, fullWidth, fullHeight, delayed, secondaryBlur, s),
                [](Remote* value) {
                    try { value->Drain(); }
                    catch (...) { return; } // Quarantined queues can still reference this slot.
                    delete value;
                });
            if (delayed || slot->UsesNormals())
                p.CreateTexture(slot->InputDepth, DXGI_FORMAT_R32_FLOAT, _width, _height);
            return slot;
        }
        catch (const DxException& e) { Log::Warning("RenderGraph", "Local ", AOName(s.Implementation), " fallback: ", e.ToString()); }
        catch (const std::exception& e) { Log::Warning("RenderGraph", "Local ", AOName(s.Implementation), " fallback: ", e.what()); }
        return {};
    };
    if (domain != _attemptedDomain || fullWidth != _attemptedWidth || fullHeight != _attemptedHeight ||
        s.Implementation != _attemptedImplementation || _half != _attemptedHalf ||
        delayed != _attemptedDelayed || secondaryBlur != _attemptedSecondary || s.DenoisePasses != _attemptedBlur)
    {
        _remote.reset();
        _spare.reset();
        _attemptedDomain = domain;
        _attemptedWidth = fullWidth;
        _attemptedHeight = fullHeight;
        _attemptedDelayed = delayed;
        _attemptedSecondary = secondaryBlur;
        _attemptedImplementation = s.Implementation;
        _attemptedHalf = _half;
        _attemptedBlur = s.DenoisePasses;
        if (domain) _remote = createRemote();
    }
    if (_remote && delayed && _remote == history.Last)
    {
        // A one-frame renderer still needs a separate slot for the next publication.
        if (!_spare) _spare = createRemote();
        if (_spare) std::swap(_remote, _spare);
        else history.Last.reset();
    }

    if (_remote)
    {
        _version = history.Last ? history.Last->Version + 1 : 1;
        _import = delayed && history.Last ? history.Last : _remote;
    }
    else
        history.Last.reset();
    if (_offload != bool(_remote)) _common.ResetUpscalerHistory = true;
    _offload = bool(_remote);
    if (!s.IsActive() || (_width == 1 && _height == 1))
        return;
    if (!grid.Normals.IsInitialized())
        p.CreateGrid(grid, _width, _height, _half);
    const UINT slot = _resources.CurrFrameConstantsIndex;
    GDX12AmbientOcclusionPass::ShaderConstants constants = {s.RadiusCm,
                                                            s.Intensity,
                                                            s.BiasCm,
                                                            s.HorizonBias,
                                                            s.FadeStartCm,
                                                            s.FadeEndCm,
                                                            s.Sharpness,
                                                            UINT(s.Samples),
                                                            UINT(s.Directions),
                                                            UINT(s.Steps),
                                                            UINT(s.DenoisePasses),
                                                            UINT(s.Implementation),
                                                            float(s.GTAOSlices),
                                                            float(s.GTAOSteps),
                                                            _half,
                                                            0};
    p._constants->CopyData(slot, constants);
    _projection = _common.CameraViewToClip;
    _projection._31 -= _common.ActiveCameraJitterOffsetX * 2 / depth->GetWidth();
    _projection._32 += _common.ActiveCameraJitterOffsetY * 2 / depth->GetHeight();
    if (_half)
        Expand(_projection, depth->GetWidth() / (2.f * _width), depth->GetHeight() / (2.f * _height));
    XeGTAO::GTAOSettings gtao;
    gtao.Radius = s.RadiusCm;
    gtao.DenoisePasses = s.DenoisePasses;
    gtao.FalloffRange = s.GTAOFalloff;
    gtao.ThinOccluderCompensation = s.GTAOThinOccluder;
    XeGTAO::GTAOConstants xe{};
    XeGTAO::GTAOUpdateConstants(xe, _width, _height, gtao, &_projection._11, true,
                                _common.Upscaler ? temporalIndex++ : 0);
    xe.NDCToViewAdd.x += _projection._31 / _projection._11;
    xe.NDCToViewAdd.y += _projection._32 / _projection._22;
    xe.DenoiseBlurBeta = s.DenoisePasses ? .2f + s.Sharpness : 1e4f;
    p._xeConstants->CopyData(slot, xe);
    if (_offload)
    {
        // Only prior uses of this transfer slot can block. The current order is queued later.
        _remote->RetireReaders();
        _remote->GPU1.Begin(0);
        _remote->Jitter = {_common.ActiveCameraJitterOffsetX, _common.ActiveCameraJitterOffsetY};
        _remote->DepthUnpack = {xe.DepthUnpackConsts.x, xe.DepthUnpackConsts.y};
        _remote->GPU1.Reserve(_remote->ScheduleGPU1);
        _remote->Constants.CopyData(0, constants);
        _remote->Projection.CopyData(0, xe);
        if (_remote->HistoryProjection) _remote->HistoryProjection->CopyData(0, xe);
        if (_remote->Cacao.Context)
        {
            const auto settings = CacaoSettings(s);
            CheckCACAO(FFX_CACAO_D3D12UpdateSettings(_remote->Cacao.Context, &settings));
            _remote->CacaoProjection = _projection;
            if (_remote->CacaoPadded()) Expand(_remote->CacaoProjection, float(_width) / 16, 1);
        }
    }
    // SDK context creation and settings updates are owner-only prerequisites.
    // Native CACAO keeps its private descriptor/scratch ownership; the graph
    // owns imported textures and the entire dispatch is one exclusive packet.
    if (s.Implementation == 3 && !_offload)
    {
        auto& inputDepth = _half ? grid.Depth : *depth;
        const bool pad = PadCacao(_width, _height);
        auto& cacaoDepth = pad ? grid.CacaoDepth : inputDepth;
        auto& cacaoNormals = pad ? grid.CacaoNormals : grid.Normals;
        auto& output = pad ? grid.CacaoVisibility : grid.Scratch[0];
        if (!grid.Cacao)
        {
            grid.CacaoMemory.resize(FFX_CACAO_D3D12GetContextSize());
            grid.Cacao = reinterpret_cast<FFX_CACAO_D3D12Context*>(grid.CacaoMemory.data());
            CheckCACAO(FFX_CACAO_D3D12InitContext(grid.Cacao, _resources.Device->GetDevice().Get(),
                                                  UINT(_resources.FrameConstants.size())));
        }
        if (!grid.CacaoScreenReady)
        {
            InitCacaoScreen(grid.Cacao, cacaoDepth.GetResource()->D3DResource.Get(),
                            cacaoNormals.GetResource()->D3DResource.Get(), output.GetResource()->D3DResource.Get());
            grid.CacaoScreenReady = true;
        }
        const auto settings = CacaoSettings(s);
        CheckCACAO(FFX_CACAO_D3D12UpdateSettings(grid.Cacao, &settings));
    }
}
struct SceneAO::Plan
{
    SceneAO& ao;
    Graph& graph;
    std::vector<ID3D12Resource*>& bindings;
    Queue target;

    Id id(GDX12Resource* resource) const
    {
        return NativeWork::ResourceFor(graph, bindings, resource->D3DResource.Get(), "AO resource");
    }

    Access access(GDX12Texture* texture, Mode mode, State state) const
    {
        return {id(texture->GetResource()), mode, state};
    }

    void dispatch(const std::string& name, UINT pipeline, UINT width, UINT height,
                  std::vector<Access> accesses, std::array<GDX12Texture*, 9> srvs,
                  std::array<GDX12Descriptor*, 10> uavs, UINT axis = 0, UINT final = 0,
                  float scale = 1.f, XMFLOAT4 history = {},
                  GDX12UploadBuffer<XeGTAO::GTAOConstants>* sourceProjection = nullptr, bool decodedVisibility = false)
    {
        auto& p = ao._pass;
        const UINT slot = ao._resources.CurrFrameConstantsIndex;
        auto* root = p._root.get();
        auto* heap = ao._resources.SRV_UAV_Heap->GetHeap().Get();
        auto* projection = sourceProjection ? sourceProjection : p._xeConstants.get();
        const auto camera = ao._resources.FrameConstants[slot]->CameraCB->GetElementAddress(ao._common.ActiveCameraCBufferIndex),
                   constants = p._constants->GetElementAddress(slot), xe = projection->GetElementAddress(sourceProjection ? 0 : slot);
        accesses.push_back({id(&ao._resources.FrameConstants[slot]->CameraCB->GetResource()), Mode::Read,
                            D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&p._constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&projection->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 9> reads{};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 10> writes{};
        for (UINT i = 0; i < reads.size(); ++i)
            if (srvs[i])
                reads[i] = srvs[i]->GetSRV()->GPUHandle;
        for (UINT i = 0; i < 10; ++i)
            if (uavs[i])
                writes[i] = uavs[i]->GPUHandle;
        auto* pso = p._pipelines[pipeline].Get();
        ao._work.Add(graph, name, std::move(accesses), [=](auto* cmd) {
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(root->GetRootSignature().Get());
            cmd->SetPipelineState(pso);
            const struct
            {
                UINT Axis, Final;
                float Scale;
                UINT DecodedVisibility;
                XMFLOAT4 History;
            } c{axis, final, scale, decodedVisibility, history};
            cmd->SetComputeRoot32BitConstants(0, sizeof(c) / sizeof(UINT), &c, 0);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(2), constants);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(3), xe);
            for (UINT i = 0; i < reads.size(); ++i)
                if (reads[i].ptr)
                    cmd->SetComputeRootDescriptorTable(root->GetTRootParamIndex(i), reads[i]);
            for (UINT i = 0; i < 10; ++i)
                if (writes[i].ptr)
                    cmd->SetComputeRootDescriptorTable(root->GetURootParamIndex(i), writes[i]);
            cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
        }, {}, target);
    }
};

void SceneAO::AppendRemoteDepth(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
    if (!_offload || _remote->UsesNormals()) return;
    auto& pass = _pass;
    Plan plan{*this, graph, bindings, AsyncAO.GetValue() ? Queue::Compute : Queue::Direct};
    const auto inputs = AppendPreparation(plan, pass._inputs[0]->GetTexture(), pass._inputs[1]->GetTexture());
    auto* source = inputs.Depth->GetResource()->D3DResource.Get();
    auto* remote = _remote.get();
    const Id buffer = NativeWork::ResourceFor(graph, bindings, remote->DepthTransfer.Resource(0), "HBAO shared half-depth");
    _depthExport = _work.Add(graph, "HBAO GPU0 export half-depth",
        {plan.access(inputs.Depth, Mode::Read, CopyRead), {buffer, Mode::Write, CopyWrite}},
        [=](auto* cmd) { remote->DepthTransfer.Export(cmd, 0, source); }, {}, Queue::Copy);
}

void SceneAO::AppendEarlyInputs(Graph& graph, std::vector<ID3D12Resource*>& bindings, Id ready)
{
    if (!_offload || !_remote->UsesNormals()) return;
    Plan plan{*this, graph, bindings, AsyncAO.GetValue() ? Queue::Compute : Queue::Direct};
    auto inputs = AppendPreparation(plan, _pass._inputs[0]->GetTexture(), _earlyNormals);
    graph.Operations.back().Dependencies.push_back(ready);
    auto* remote = _remote.get();
    if (remote->CacaoPadded()) inputs = AppendCacaoPadding(plan, inputs);
    const auto sharedDepth = NativeWork::ResourceFor(graph, bindings, remote->DepthTransfer.Resource(0), "AO shared depth");
    const auto sharedNormals = NativeWork::ResourceFor(graph, bindings, remote->NormalTransfer.Resource(0), "AO shared normals");
    auto* depthSource = inputs.Depth->GetResource()->D3DResource.Get();
    auto* normalSource = inputs.Normals->GetResource()->D3DResource.Get();
    _depthExport = _work.Add(graph, std::string(AOName(remote->Implementation)) + " GPU0 export inputs",
        {plan.access(inputs.Depth, Mode::Read, CopyRead), plan.access(inputs.Normals, Mode::Read, CopyRead),
         {sharedDepth, Mode::Write, CopyWrite}, {sharedNormals, Mode::Write, CopyWrite}},
        [=](auto* cmd) {
            remote->DepthTransfer.Export(cmd, 0, depthSource);
            remote->NormalTransfer.Export(cmd, 0, normalSource);
        }, {}, Queue::Copy);
}

void SceneAO::AppendXeImport(Plan& plan, GDX12Texture& terms, GDX12Texture& edges)
{
    auto* remote = _import.get();
    const auto sharedTerms = NativeWork::ResourceFor(plan.graph, plan.bindings, remote->VisibilityTransfer.Resource(0),
                                                    "Xe shared terms");
    std::vector<Access> accesses{{sharedTerms, Mode::Read, CopyRead}, plan.access(&terms, Mode::Write, CopyWrite)};
    if (remote->LocalXePasses)
    {
        const auto sharedEdges = NativeWork::ResourceFor(plan.graph, plan.bindings, remote->EdgeTransfer.Resource(0),
                                                        "Xe shared edges");
        accesses.push_back({sharedEdges, Mode::Read, CopyRead});
        accesses.push_back(plan.access(&edges, Mode::Write, CopyWrite));
    }
    auto* termsTarget = terms.GetResource()->D3DResource.Get();
    auto* edgeTarget = edges.GetResource()->D3DResource.Get();
    _visibilityImport = _work.Add(plan.graph, "XeGTAO GPU0 import terms", std::move(accesses),
        [=](auto* cmd) {
            remote->VisibilityTransfer.Import(cmd, 0, termsTarget);
            if (remote->LocalXePasses) remote->EdgeTransfer.Import(cmd, 0, edgeTarget);
        }, {_depthExport}, Queue::Copy);
}

void SceneAO::AppendRawVisibility(Plan& plan, const PreparedInputs& inputs, GDX12Texture& visibility)
{
    if (!_offload)
    {
        plan.dispatch("SSAO/HBAO visibility", _settings.Implementation == 0 ? _pass.SSAO : _pass.HBAO, _width, _height,
            {plan.access(inputs.Depth, Mode::Read, Read), plan.access(inputs.Normals, Mode::Read, Read),
             plan.access(&visibility, Mode::Write, UnorderedAccess)},
            {inputs.Depth, inputs.Normals}, {visibility.GetUAV()});
        return;
    }
    AppendVisibilityImport(plan, visibility);
}

void SceneAO::AppendVisibilityImport(Plan& plan, GDX12Texture& visibility)
{
    auto* target = visibility.GetResource()->D3DResource.Get();
    auto* remote = _import.get();
    const Id buffer = NativeWork::ResourceFor(plan.graph, plan.bindings, remote->VisibilityTransfer.Resource(0),
                                              "AO shared visibility");
    _visibilityImport = _work.Add(plan.graph, std::string(AOName(remote->Implementation)) + " GPU0 import visibility",
        {{buffer, Mode::Read, CopyRead}, plan.access(&visibility, Mode::Write, CopyWrite)},
        [=](auto* cmd) { remote->VisibilityTransfer.Import(cmd, 0, target); }, {_depthExport}, Queue::Copy);
}

void SceneAO::Append(Graph& graph, std::vector<ID3D12Resource*>& bindings)
{
    auto& p = _pass;
    Plan plan{*this, graph, bindings, AsyncAO.GetValue() ? Queue::Compute : Queue::Direct};
    if (!_settings.IsActive() || (_width == 1 && _height == 1))
    {
        plan.dispatch("AO neutral fallback", p.Neutral, p._output.GetWidth(), p._output.GetHeight(),
                 {plan.access(&p._output, Mode::Write, UnorderedAccess)}, {}, {p._output.GetUAV()});
        return;
    }
    auto* fullDepth = p._inputs[0]->GetTexture();
    auto* fullNormals = p._inputs[1]->GetTexture();
    const auto inputs = !_offload ? AppendPreparation(plan, fullDepth, fullNormals)
        : PreparedInputs{_remote->InputDepth.IsInitialized() ? &_remote->InputDepth : &p._grids[_half].Depth,
                         &p._grids[_half].Normals};
    auto* resolved = AppendVisibility(plan, inputs);
    if (_half)
    {
        plan.dispatch("AO depth/normal aware upsample", p.Upsample, p._output.GetWidth(), p._output.GetHeight(),
                      {plan.access(inputs.Depth, Mode::Read, Read), plan.access(inputs.Normals, Mode::Read, Read),
                       plan.access(resolved, Mode::Read, Read), plan.access(fullDepth, Mode::Read, Read),
                       plan.access(fullNormals, Mode::Read, Read), plan.access(&p._output, Mode::Write, UnorderedAccess)},
                      {inputs.Depth, inputs.Normals, resolved, nullptr, nullptr, fullDepth, fullNormals}, {p._output.GetUAV()});
    }
}

SceneAO::PreparedInputs SceneAO::AppendPreparation(Plan& plan, GDX12Texture* fullDepth, GDX12Texture* fullNormals)
{
    auto& p = _pass;
    auto& grid = p._grids[_half];
    const bool packDepth = _offload && _remote->InputDepth.IsInitialized();
    auto* depth = packDepth ? &_remote->InputDepth : _half ? &grid.Depth : fullDepth;
    std::vector<Access> prepare{plan.access(fullDepth, Mode::Read, Read), plan.access(fullNormals, Mode::Read, Read),
                                plan.access(&grid.Normals, Mode::Write, UnorderedAccess)};
    if (_half || packDepth)
        prepare.push_back(plan.access(depth, Mode::Write, UnorderedAccess));
    plan.dispatch("AO prepare normals/depth", _half ? p.PrepareHalf : packDepth ? p.PackFull : p.PrepareNormals,
             _width, _height, std::move(prepare),
             {fullDepth, fullNormals},
             {nullptr, grid.Normals.GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              _half || packDepth ? depth->GetUAV() : nullptr});
    return {depth, &grid.Normals};
}

SceneAO::PreparedInputs SceneAO::AppendCacaoPadding(Plan& plan, const PreparedInputs& inputs)
{
    auto& grid = _pass._grids[_half];
    auto* depth = &grid.CacaoDepth;
    auto* normals = &grid.CacaoNormals;
    plan.dispatch("CACAO pad small grid", _pass.PadCACAO, depth->GetWidth(), depth->GetHeight(),
        {plan.access(inputs.Depth, Mode::Read, Read), plan.access(inputs.Normals, Mode::Read, Read),
         plan.access(depth, Mode::Write, UnorderedAccess), plan.access(normals, Mode::Write, UnorderedAccess)},
        {inputs.Depth, inputs.Normals},
        {nullptr, normals->GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, depth->GetUAV()});
    return {depth, normals};
}

void SceneAO::AppendReprojection(Plan& plan, const PreparedInputs& inputs, GDX12Texture& source,
                                 GDX12Texture& target, float scale, bool terms)
{
    auto* oldDepth = &_import->InputDepth;
    const XMFLOAT4 history{_import->Jitter.x - _remote->Jitter.x, _import->Jitter.y - _remote->Jitter.y,
                          _import->DepthUnpack.x, _import->DepthUnpack.y};
    _reproject = Id(plan.graph.Operations.size());
    plan.dispatch(std::string(AOName(_import->Implementation)) + " GPU0 reproject previous visibility",
        terms ? _pass.ReprojectXe : _pass.Reproject, _width, _height,
        {plan.access(inputs.Depth, Mode::Read, Read), plan.access(inputs.Normals, Mode::Read, Read),
         plan.access(&source, Mode::Read, Read), plan.access(_motion, Mode::Read, Read),
         plan.access(oldDepth, Mode::Read, Read), plan.access(&target, Mode::Write, UnorderedAccess)},
        {inputs.Depth, inputs.Normals, terms ? nullptr : &source, terms ? &source : nullptr,
         nullptr, nullptr, nullptr, _motion, oldDepth}, {target.GetUAV()}, 0, 0, scale, history);
}

GDX12Texture* SceneAO::AppendVisibility(Plan& plan, const PreparedInputs& inputs)
{
    auto& p = _pass;
    auto& grid = p._grids[_half];
    auto* depth = inputs.Depth;
    auto* normals = inputs.Normals;
    assert(depth->GetWidth() == _width && depth->GetHeight() == _height &&
           normals->GetWidth() == _width && normals->GetHeight() == _height);
    GDX12Texture* visibility = nullptr;
    GDX12Texture* terms = nullptr;
    float scale = 1.f;
    if (_settings.Implementation == 3)
    {
        const bool pad = PadCacao(_width, _height);
        visibility = pad ? &grid.CacaoVisibility : &grid.Scratch[0];
        if (_offload)
            AppendVisibilityImport(plan, *visibility);
        else
        {
            const auto cacaoInputs = pad ? AppendCacaoPadding(plan, inputs) : inputs;
            Matrix projection = _projection;
            if (pad) Expand(projection, float(depth->GetWidth()) / cacaoInputs.Depth->GetWidth(), 1);
            auto* context = grid.Cacao;
            auto* output = visibility->GetResource()->D3DResource.Get();
            _work.Add(plan.graph, "CACAO SDK dispatch (exclusive persistent context)",
                      {plan.access(cacaoInputs.Depth, Mode::Read, Read), plan.access(cacaoInputs.Normals, Mode::Read, Read),
                       plan.access(visibility, Mode::Write, UnorderedAccess)},
                      [context, projection, output](auto* cmd) { DrawCacao(context, cmd, projection, output); },
                      {}, plan.target);
        }
        if (UsesHistory())
        {
            auto& target = grid.Scratch[pad ? 0 : 1];
            AppendReprojection(plan, inputs, *visibility, target);
            visibility = &target;
        }
    }
    else if (_settings.Implementation == 2)
    {
        const bool previous = UsesHistory();
        auto* denoiseDepth = previous ? &_import->InputDepth : depth;
        if (_offload)
            AppendXeImport(plan, grid.XeTerms[0], grid.XeEdges);
        else
        {
            std::array<GDX12Descriptor*, 10> uavs{};
            for (UINT mip = 0; mip < 5; ++mip)
                uavs[2 + mip] = &grid.DepthUAVs[mip];
            plan.dispatch("XeGTAO depth pyramid", p.XePrefilter, grid.XeDepth.GetWidth() / 2, grid.XeDepth.GetHeight() / 2,
                     {plan.access(depth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                      plan.access(&grid.XeDepth, Mode::Write, UnorderedAccess)},
                     {depth, normals}, uavs);
            plan.dispatch("XeGTAO main", p.XeMain, _width, _height,
                     {plan.access(&grid.XeDepth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                      plan.access(&grid.XeTerms[0], Mode::Write, UnorderedAccess),
                      plan.access(&grid.XeEdges, Mode::Write, UnorderedAccess)},
                     {depth, normals, &grid.XeDepth},
                     {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, grid.XeTerms[0].GetUAV(),
                      grid.XeEdges.GetUAV()});
        }
        const int denoisePasses = _offload ? _remote->LocalXePasses : _settings.DenoisePasses;
        UINT source = 0;
        for (int pass = 0; pass < denoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            plan.dispatch("XeGTAO denoise", p.XeDenoise, (_width + 1) / 2, _height,
                     {plan.access(&grid.XeTerms[source], Mode::Read, Read), plan.access(&grid.XeEdges, Mode::Read, Read),
                      plan.access(denoiseDepth, Mode::Read, Read),
                      plan.access(&grid.XeTerms[target], Mode::Write, UnorderedAccess)},
                     {denoiseDepth, nullptr, nullptr, &grid.XeTerms[source], &grid.XeEdges},
                     {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, grid.XeTerms[target].GetUAV()}, 0,
                     pass + 1 == denoisePasses, 1.f, {}, previous ? _import->HistoryProjection.get() : nullptr);
            source = target;
        }
        terms = &grid.XeTerms[source];
        visibility = _offload ? depth : &grid.XeDepth;
        scale = _settings.DenoisePasses ? 1.f : XE_GTAO_OCCLUSION_TERM_SCALE;
        if (previous)
        {
            AppendReprojection(plan, inputs, *terms, grid.Scratch[0], scale, true);
            visibility = &grid.Scratch[0];
            terms = nullptr;
        }
    }
    else
    {
        AppendRawVisibility(plan, inputs, grid.Scratch[0]);
        UINT source = 0;
        if (UsesHistory())
        {
            AppendReprojection(plan, inputs, grid.Scratch[0], grid.Scratch[1]);
            source = 1;
        }
        const int localBlurPasses = _offload && _remote->SecondaryBlur ? 0 : _settings.DenoisePasses;
        for (int pass = 0; pass < localBlurPasses; ++pass)
        {
            const UINT target = 1 - source;
            plan.dispatch("AO bilateral blur", p.Blur, _width, _height,
                     {plan.access(depth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                      plan.access(&grid.Scratch[source], Mode::Read, Read),
                      plan.access(&grid.Scratch[target], Mode::Write, UnorderedAccess)},
                     {depth, normals, &grid.Scratch[source]}, {grid.Scratch[target].GetUAV()}, pass % 2);
            source = target;
        }
        visibility = &grid.Scratch[source];
    }
    const bool decodedVisibility = _settings.Implementation == 2 && !terms;
    if (!terms) terms = &grid.XeTerms[0];
    auto* resolved = _half ? &grid.Visibility : &p._output;
    std::vector<Access> resolve{plan.access(depth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                                plan.access(resolved, Mode::Write, UnorderedAccess)};
    if (visibility != depth)
        resolve.push_back(plan.access(visibility, Mode::Read, Read));
    resolve.push_back(plan.access(terms, Mode::Read, Read));
    plan.dispatch("AO resolve", p.Resolve, _width, _height, resolve, {depth, normals, visibility, terms},
             {resolved->GetUAV()}, 0, 0, scale, {}, nullptr, decodedVisibility);
    return resolved;
}

std::optional<Executor::PacketExchange> SceneAO::Exchange(const Schedule& schedule, History& history)
{
    if (!_offload) return {};
    Id exportPacket = Invalid;
    for (Id i = 0; i < schedule.Packets.size(); ++i)
    {
        if (schedule.Packets[i].Operation == _depthExport) exportPacket = i;
        if (schedule.Packets[i].Operation == _visibilityImport) _importPacket = i;
        if (_reproject != Invalid && schedule.Packets[i].Operation == _reproject) _reprojectPacket = i;
    }
    if (_importPacket == Invalid || exportPacket >= _importPacket)
        throw std::logic_error("AO cross-adapter packets missing or out of order");
    return Executor::PacketExchange{exportPacket, _importPacket,
        [this, &history](CompletionToken depthReady) {
            _remote->Execute(depthReady);
            _remote->Version = _version;
            history.Last = _remote;
            return _import->VisibilityReady;
        }};
}

void SceneAO::Submitted(const std::vector<CompletionToken>& completions)
{
    if (!_offload) return;
    for (const auto packet : {_importPacket, _reprojectPacket})
        if (packet < completions.size()) _import->NoteReader(completions[packet]);
}

void SceneAO::Presented(uint64_t frame, const std::vector<CompletionToken>& completions)
{
    if (!_offload || _version > 3) return;
    const auto imported = completions.at(_importPacket);
    assert(imported && _remote->DepthReady && _import->VisibilityReady);
    assert(_import->Version == (_remote->Delayed && _version > 1 ? _version - 1 : _version));
    const auto gpu0 = _remote->Submit->Device(0).GetAdapterLuid();
    const auto gpu1 = _remote->Submit->Device(1).GetAdapterLuid();
    Log::Info("RenderGraph", AOName(_remote->Implementation), " MGPU frame=", frame, " slot=", _resources.CurrFrameConstantsIndex,
        " grid=", _width, "x", _height, " GPU0 LUID=", gpu0.HighPart, ":", gpu0.LowPart,
        " GPU1 LUID=", gpu1.HighPart, ":", gpu1.LowPart, " depthFence=", _remote->DepthReady.Value,
        " visibilityFence=", _import->VisibilityReady.Value, " importFence=", imported.Value,
        " ordered=", _version, " imported=", _import->Version, " CrossSync=", _remote->Delayed ? 0 : 1,
        " blurGPU=", _remote->SecondaryBlur || _remote->Implementation == 3 ? 1 : 0,
        " blurPasses=", _settings.DenoisePasses, " filter=", _remote->Implementation == 3 ? "CACAO SDK" : "native");
    if (_remote->Xe())
        Log::Info("RenderGraph", "XeGTAO MGPU payload frame=", frame, " terms=R8_UINT edges=",
            _remote->LocalXePasses ? "R8_UNORM" : "none", " denoiseGPU0=", _remote->LocalXePasses,
            " denoiseGPU1=", _remote->BlurPasses);
}
} // namespace BistroRender
