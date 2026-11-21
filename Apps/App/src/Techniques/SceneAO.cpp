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
constexpr State CopyRead = D3D12_RESOURCE_STATE_COPY_SOURCE;
constexpr State CopyWrite = D3D12_RESOURCE_STATE_COPY_DEST;

void CopyTexture(ID3D12GraphicsCommandList* cmd, const D3D12_TEXTURE_COPY_LOCATION& target,
                 const D3D12_TEXTURE_COPY_LOCATION& source)
{
    cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
}
} // namespace

struct SceneAO::Remote
{
    std::shared_ptr<QueueSubmission> Submit;
    GDX12ResourceHandle Depth, Visibility;
    QueueSubmission::TransferBuffer DepthTransfer, VisibilityTransfer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
    std::unique_ptr<GDX12RootSignature> Root;
    ComPtr<ID3D12PipelineState> Pipeline;
    std::unique_ptr<GDX12DescriptorHeap> Heap;
    GDX12UploadBuffer<GDX12AmbientOcclusionPass::ShaderConstants> Constants;
    GDX12UploadBuffer<XeGTAO::GTAOConstants> Projection;
    Schedule ScheduleGPU1;
    Executor GPU1;
    CompletionToken DepthReady, VisibilityReady;
    UINT Width, Height;
    bool Logged = false;

    Remote(std::shared_ptr<QueueSubmission> submit, UINT width, UINT height)
        : Submit(std::move(submit)), Constants(&Submit->Device(1), 1), Projection(&Submit->Device(1), 1),
          GPU1(Submit, 1, 1, 1), Width(width), Height(height)
    {
        auto& device = Submit->Device(1);
        auto* native = device.GetDevice().Get();
        constexpr auto format = DXGI_FORMAT_R32_FLOAT;
        auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1);
        uint64_t bytes = 0, primaryBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT primaryFootprint{};
        native->GetCopyableFootprints(&desc, 0, 1, 0, &Footprint, nullptr, nullptr, &bytes);
        Submit->Device(0).GetDevice()->GetCopyableFootprints(&desc, 0, 1, 0, &primaryFootprint, nullptr, nullptr,
                                                          &primaryBytes);
        if (!bytes || bytes == UINT64_MAX || bytes != primaryBytes ||
            Footprint.Footprint.RowPitch != primaryFootprint.Footprint.RowPitch)
            throw std::runtime_error("HBAO adapters need matching R32_FLOAT footprints");

        DepthTransfer = Submit->CreateTransferBuffer(bytes);
        VisibilityTransfer = Submit->CreateTransferBuffer(bytes);
        Depth = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Visibility = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);

        GDX12RootSignatureDesc root;
        root.NumSingleCBVSlots = 4; // AOParameters at b2 and projection at b3, as in AmbientOcclusion.hlsl.
        root.NumSingleSRVSlots = 1;
        root.NumSingleUAVSlots = 1;
        Root = std::make_unique<GDX12RootSignature>(&device, root);
        Pipeline = GDX12RenderHelpers::CreateComputePipeline(&device, Root.get(),
            SHADERS_FOLDER "AmbientOcclusion.hlsl", "HBAOHalfDepthCS");
        Heap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        native->CreateShaderResourceView(Depth.Get(), nullptr, Heap->GetCPUHandle(0));
        native->CreateUnorderedAccessView(Visibility.Get(), nullptr, nullptr, Heap->GetCPUHandle(1));

        Graph graph;
        graph.Device = 1;
        graph.FrameBoundary = false;
        graph.TerminalQueue = Queue::Compute;
        graph.Resources = {{"shared HBAO depth", 0, 0, false, true}, {"GPU1 HBAO depth", 0, 0},
                           {"GPU1 HBAO visibility", 0, 0, true}, {"shared HBAO visibility", 0, 0, false, true}};
        graph.Operations = {
            {"HBAO GPU1 import half-depth", {{0, Mode::Read, CopyRead}, {1, Mode::Write, CopyWrite}}},
            {"HBAO GPU1 depth-only visibility", {{1, Mode::Read, Read}, {2, Mode::Write, UnorderedAccess}}},
            {"HBAO GPU1 export visibility", {{2, Mode::Read, CopyRead}, {3, Mode::Write, CopyWrite}}}};
        for (auto& operation : graph.Operations) operation.Target = Queue::Compute;
        ScheduleGPU1 = Compile(graph);
        assert(ScheduleGPU1.Packets.front().Operation == 0);
    }

    CompletionToken Execute(CompletionToken depthReady)
    {
        DepthReady = depthReady;
        auto* depthShared = DepthTransfer.Resources[1].Get();
        auto* visibilityShared = VisibilityTransfer.Resources[1].Get();
        GPU1.Execute(ScheduleGPU1, {depthShared, Depth.Get(), Visibility.Get(), visibilityShared},
            [&](Id operation, uint32_t, auto* cmd) {
                if (operation == 0)
                {
                    CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(Depth.Get(), 0u),
                                CD3DX12_TEXTURE_COPY_LOCATION(depthShared, Footprint));
                }
                else if (operation == 1)
                {
                    auto* heap = Heap->GetHeap().Get();
                    cmd->SetDescriptorHeaps(1, &heap);
                    cmd->SetComputeRootSignature(Root->GetRootSignature().Get());
                    cmd->SetPipelineState(Pipeline.Get());
                    cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(2), Constants.GetElementAddress(0));
                    cmd->SetComputeRootConstantBufferView(Root->GetBRootParamIndex(3), Projection.GetElementAddress(0));
                    cmd->SetComputeRootDescriptorTable(Root->GetTRootParamIndex(0), Heap->GetGPUHandle(0));
                    cmd->SetComputeRootDescriptorTable(Root->GetURootParamIndex(0), Heap->GetGPUHandle(1));
                    cmd->Dispatch((Width + 7) / 8, (Height + 7) / 8, 1);
                }
                else
                {
                    CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(visibilityShared, Footprint),
                                CD3DX12_TEXTURE_COPY_LOCATION(Visibility.Get(), 0u));
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
    try { RetireRemote(); }
    catch (...) { _remote.release(); } // A quarantined queue can still reference the transfer resources.
    _pass.ClearDenendencies();
}
void SceneAO::RetireRemote()
{
    if (_remote) _remote->GPU1.Drain();
    _remote.reset();
}
void SceneAO::Freeze(GDX12Texture* depth, GDX12Texture* normals, UINT& temporalIndex,
                     const std::shared_ptr<QueueSubmission>& remote)
{
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
    const auto domain = (remote && _half && s.IsActive() && s.Implementation == 1 && (_width > 1 || _height > 1))
        ? remote->Incarnation() : 0;
    if (domain != _attemptedDomain || _width != _attemptedWidth || _height != _attemptedHeight)
    {
        RetireRemote();
        _attemptedDomain = domain;
        _attemptedWidth = _width;
        _attemptedHeight = _height;
        if (domain)
        {
            try { _remote = std::make_unique<Remote>(remote, _width, _height); }
            catch (const DxException& e) { Log::Warning("RenderGraph", "Local HBAO fallback: ", e.ToString()); }
            catch (const std::exception& e) { Log::Warning("RenderGraph", "Local HBAO fallback: ", e.what()); }
        }
    }
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
        // GPU0 retired this frame slot, including its import of the GPU1 terminal token.
        if (!_remote->GPU1.TryBegin(0)) throw std::logic_error("HBAO GPU1 frame slot still in flight");
        _remote->GPU1.Reserve(_remote->ScheduleGPU1);
        _remote->Constants.CopyData(0, constants);
        _remote->Projection.CopyData(0, xe);
    }
    // SDK context creation and settings updates are owner-only prerequisites.
    // Native CACAO keeps its private descriptor/scratch ownership; the graph
    // owns imported textures and the entire dispatch is one exclusive packet.
    if (s.Implementation == 3)
    {
        auto& inputDepth = _half ? grid.Depth : *depth;
        const bool pad = (std::max)(_width, _height) < 16;
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
            FFX_CACAO_D3D12ScreenSizeInfo info{};
            info.width = cacaoDepth.GetWidth();
            info.height = cacaoDepth.GetHeight();
            info.depthBufferResource = cacaoDepth.GetResource()->D3DResource.Get();
            info.depthBufferSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            info.depthBufferSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            info.depthBufferSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            info.depthBufferSrvDesc.Texture2D.MipLevels = 1;
            info.normalBufferResource = cacaoNormals.GetResource()->D3DResource.Get();
            info.normalBufferSrvDesc = info.depthBufferSrvDesc;
            info.normalBufferSrvDesc.Format = cacaoNormals.GetFormat();
            info.outputResource = output.GetResource()->D3DResource.Get();
            info.outputUavDesc.Format = output.GetFormat();
            info.outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            CheckCACAO(FFX_CACAO_D3D12InitScreenSizeDependentResources(grid.Cacao, &info));
            grid.CacaoScreenReady = true;
        }
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
                  std::vector<Access> accesses, std::array<GDX12Texture*, 7> srvs,
                  std::array<GDX12Descriptor*, 10> uavs, UINT axis = 0, UINT final = 0,
                  float scale = 1.f)
    {
        auto& p = ao._pass;
        const UINT slot = ao._resources.CurrFrameConstantsIndex;
        auto* root = p._root.get();
        auto* heap = ao._resources.SRV_UAV_Heap->GetHeap().Get();
        const auto camera = ao._resources.FrameConstants[slot]->CameraCB->GetElementAddress(ao._common.ActiveCameraCBufferIndex),
                   constants = p._constants->GetElementAddress(slot), xe = p._xeConstants->GetElementAddress(slot);
        accesses.push_back({id(&ao._resources.FrameConstants[slot]->CameraCB->GetResource()), Mode::Read,
                            D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&p._constants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        accesses.push_back({id(&p._xeConstants->GetResource()), Mode::Read, D3D12_RESOURCE_STATE_GENERIC_READ});
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 7> reads{};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 10> writes{};
        for (UINT i = 0; i < 7; ++i)
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
                UINT Padding;
            } c{axis, final, scale, 0};
            cmd->SetComputeRoot32BitConstants(0, 4, &c, 0);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(1), camera);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(2), constants);
            cmd->SetComputeRootConstantBufferView(root->GetBRootParamIndex(3), xe);
            for (UINT i = 0; i < 7; ++i)
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
    _work.Commands.clear();
    _depthExport = _visibilityImport = _importPacket = Invalid;
    if (!_offload) return;
    auto& pass = _pass;
    Plan plan{*this, graph, bindings, AsyncAO.GetValue() ? Queue::Compute : Queue::Direct};
    const auto inputs = AppendPreparation(plan, pass._inputs[0]->GetTexture(), pass._inputs[1]->GetTexture());
    const auto footprint = _remote->Footprint;
    auto* source = inputs.Depth->GetResource()->D3DResource.Get();
    auto* shared = _remote->DepthTransfer.Resources[0].Get();
    const Id buffer = NativeWork::ResourceFor(graph, bindings, shared, "HBAO shared half-depth");
    _depthExport = _work.Add(graph, "HBAO GPU0 export half-depth",
        {plan.access(inputs.Depth, Mode::Read, CopyRead), {buffer, Mode::Write, CopyWrite}},
        [=](auto* cmd) {
            CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(shared, footprint), CD3DX12_TEXTURE_COPY_LOCATION(source, 0u));
        }, {}, Queue::Copy);
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
    const auto footprint = _remote->Footprint;
    auto* target = visibility.GetResource()->D3DResource.Get();
    auto* shared = _remote->VisibilityTransfer.Resources[0].Get();
    const Id buffer = NativeWork::ResourceFor(plan.graph, plan.bindings, shared, "HBAO shared raw AO");
    _visibilityImport = _work.Add(plan.graph, "HBAO GPU0 import raw AO",
        {{buffer, Mode::Read, CopyRead}, plan.access(&visibility, Mode::Write, CopyWrite)},
        [=](auto* cmd) {
            CopyTexture(cmd, CD3DX12_TEXTURE_COPY_LOCATION(target, 0u), CD3DX12_TEXTURE_COPY_LOCATION(shared, footprint));
        }, {_depthExport}, Queue::Copy);
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
    const auto inputs = _offload ? PreparedInputs{&p._grids[1].Depth, &p._grids[1].Normals}
                                 : AppendPreparation(plan, fullDepth, fullNormals);
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
    auto* depth = _half ? &grid.Depth : fullDepth;
    std::vector<Access> prepare{plan.access(fullDepth, Mode::Read, Read), plan.access(fullNormals, Mode::Read, Read),
                                plan.access(&grid.Normals, Mode::Write, UnorderedAccess)};
    if (_half)
        prepare.push_back(plan.access(depth, Mode::Write, UnorderedAccess));
    plan.dispatch("AO prepare normals/depth", _half ? p.PrepareHalf : p.PrepareNormals, _width, _height, prepare,
             {fullDepth, fullNormals},
             {nullptr, grid.Normals.GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
              _half ? depth->GetUAV() : nullptr});
    return {depth, &grid.Normals};
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
        const bool pad = (std::max)(_width, _height) < 16;
        auto* inputDepth = pad ? &grid.CacaoDepth : depth;
        auto* inputNormals = pad ? &grid.CacaoNormals : normals;
        visibility = pad ? &grid.CacaoVisibility : &grid.Scratch[0];
        if (pad)
            plan.dispatch("CACAO pad small grid", p.PadCACAO, inputDepth->GetWidth(), inputDepth->GetHeight(),
                     {plan.access(depth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                      plan.access(inputDepth, Mode::Write, UnorderedAccess),
                      plan.access(inputNormals, Mode::Write, UnorderedAccess)},
                     {depth, normals},
                     {nullptr, inputNormals->GetUAV(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                      inputDepth->GetUAV()});
        Matrix projection = _projection;
        if (pad)
            Expand(projection, float(depth->GetWidth()) / inputDepth->GetWidth(), 1);
        FFX_CACAO_Matrix4x4 proj{}, identity{};
        std::memcpy(&proj, &projection, sizeof(proj));
        proj.elements[3][2] *= .01f;
        for (UINT i = 0; i < 4; ++i)
            identity.elements[i][i] = 1;
        auto* context = grid.Cacao;
        auto* output = visibility->GetResource()->D3DResource.Get();
        _work.Add(plan.graph, "CACAO SDK dispatch (exclusive persistent context)",
                  {plan.access(inputDepth, Mode::Read, Read), plan.access(inputNormals, Mode::Read, Read),
                   plan.access(visibility, Mode::Write, UnorderedAccess)},
                  [context, proj, identity, output](auto* cmd) {
                      CheckCACAO(FFX_CACAO_D3D12Draw(context, cmd, &proj, &identity));
                      const auto outputState = cmd->GetType() == D3D12_COMMAND_LIST_TYPE_COMPUTE
                          ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_GENERIC_READ;
                      const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                          output, outputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                      cmd->ResourceBarrier(1, &barrier);
                  }, {}, plan.target);
    }
    else if (_settings.Implementation == 2)
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
        UINT source = 0;
        for (int pass = 0; pass < _settings.DenoisePasses; ++pass)
        {
            const UINT target = 1 - source;
            plan.dispatch("XeGTAO denoise", p.XeDenoise, (_width + 1) / 2, _height,
                     {plan.access(&grid.XeTerms[source], Mode::Read, Read), plan.access(&grid.XeEdges, Mode::Read, Read),
                      plan.access(&grid.XeTerms[target], Mode::Write, UnorderedAccess)},
                     {depth, normals, &grid.XeDepth, &grid.XeTerms[source], &grid.XeEdges},
                     {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, grid.XeTerms[target].GetUAV()}, 0,
                     pass + 1 == _settings.DenoisePasses);
            source = target;
        }
        terms = &grid.XeTerms[source];
        visibility = &grid.XeDepth;
        scale = _settings.DenoisePasses ? 1.f : XE_GTAO_OCCLUSION_TERM_SCALE;
    }
    else
    {
        AppendRawVisibility(plan, inputs, grid.Scratch[0]);
        UINT source = 0;
        for (int pass = 0; pass < _settings.DenoisePasses; ++pass)
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
    if (!terms)
        terms = &grid.XeTerms[0];
    auto* resolved = _half ? &grid.Visibility : &p._output;
    std::vector<Access> resolve{plan.access(depth, Mode::Read, Read), plan.access(normals, Mode::Read, Read),
                                plan.access(visibility, Mode::Read, Read), plan.access(resolved, Mode::Write, UnorderedAccess)};
    if (terms)
        resolve.push_back(plan.access(terms, Mode::Read, Read));
    plan.dispatch("AO resolve", p.Resolve, _width, _height, resolve, {depth, normals, visibility, terms},
             {resolved->GetUAV()}, 0, 0, scale);
    return resolved;
}

std::optional<Executor::PacketExchange> SceneAO::Exchange(const Schedule& schedule)
{
    if (!_offload) return {};
    Id exportPacket = Invalid;
    for (Id i = 0; i < schedule.Packets.size(); ++i)
    {
        if (schedule.Packets[i].Operation == _depthExport) exportPacket = i;
        if (schedule.Packets[i].Operation == _visibilityImport) _importPacket = i;
    }
    if (_importPacket == Invalid || exportPacket >= _importPacket)
        throw std::logic_error("HBAO cross-adapter packets missing or out of order");
    return Executor::PacketExchange{exportPacket, _importPacket,
        [this](CompletionToken depthReady) { return _remote->Execute(depthReady); }};
}

void SceneAO::Presented(uint64_t frame, const std::vector<CompletionToken>& completions)
{
    if (!_offload || _remote->Logged) return;
    const auto imported = completions.at(_importPacket);
    assert(imported && _remote->DepthReady && _remote->VisibilityReady);
    const auto gpu0 = _remote->Submit->Device(0).GetAdapterLuid();
    const auto gpu1 = _remote->Submit->Device(1).GetAdapterLuid();
    Log::Info("RenderGraph", "HBAO MGPU frame=", frame, " slot=", _resources.CurrFrameConstantsIndex,
        " half=", _width, "x", _height, " GPU0 LUID=", gpu0.HighPart, ":", gpu0.LowPart,
        " GPU1 LUID=", gpu1.HighPart, ":", gpu1.LowPart, " depthFence=", _remote->DepthReady.Value,
        " visibilityFence=", _remote->VisibilityReady.Value, " importFence=", imported.Value,
        " blurPasses=", _settings.DenoisePasses);
    _remote->Logged = true;
}
} // namespace BistroRender
