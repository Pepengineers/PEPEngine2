#include "Engine.RendererDX12/RenderPasses/GDX12FFXSSSRPass.h"
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <cmath>

namespace
{
    void CheckFFX(FfxErrorCode result)
    {
        if (result != FFX_OK)
        { throw std::runtime_error("ERROR: FidelityFX SSSR failed, status " + std::to_string(result)); }
    }

    FfxResource GetSDKResource(GDX12Texture* texture)
    {
        auto* resource = texture->GetResource();
        const auto state = resource->GetCurrentState();
        FfxResourceStates sdkState;
        if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) { sdkState = FFX_RESOURCE_STATE_UNORDERED_ACCESS; }
        else if (state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) { sdkState = FFX_RESOURCE_STATE_COMPUTE_READ; }
        else if (state == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        { sdkState = FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ; }
        else { throw std::logic_error("ERROR: SSSR resources must enter the SDK in shader-read or UAV state."); }
        return ffxGetResourceDX12(resource->D3DResource.Get(), ffxGetResourceDescriptionDX12(resource->D3DResource.Get()), L"SSSR", sdkState);
    }
}

GDX12FFXSSSRPass::GDX12FFXSSSRPass()
{ _flags = RENDER_PASS_FLAG_USE_CAMERAS; }

GDX12FFXSSSRPass::~GDX12FFXSSSRPass()
{ if (_context) { ffxSssrContextDestroy(_context.get()); } }

void GDX12FFXSSSRPass::Describe(PassDescription& description) const
{
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("MaxRoughness", 0.6f);
    description.AddParameter("RoughnessFade", 0.1f);
    description.AddParameter("SamplesPerQuad", 4);
    description.AddParameter("MaxTraversalIntersections", 128);
    description.AddParameter("MinTraversalOccupancy", 4);
    description.AddParameter("MostDetailedMip", 1);
    description.AddParameter("DepthBufferThickness", 8.0f);
    description.AddParameter("TemporalStability", 0.9f);
    description.AddParameter("VarianceThreshold", 0.05f);
    description.AddParameter("VarianceGuidedTracing", true);

    auto& color = description.AddInputTexture("InColorTexture");
    color.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    color.Usages = ResourceUsage::NonPixelShaderRead;
    color.AllowShared = true;

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::NonPixelShaderRead;

    auto& normals = description.AddInputTexture("InNormalRoughness");
    normals.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normals.Usages = ResourceUsage::NonPixelShaderRead;
    normals.AllowShared = true;

    auto& f0 = description.AddInputTexture("InF0Baked");
    f0.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    f0.Usages = ResourceUsage::NonPixelShaderRead;
    f0.AllowShared = true;

    auto& velocity = description.AddInputTexture("InVelocityBuffer");
    velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
    velocity.Usages = ResourceUsage::NonPixelShaderRead;
    velocity.AllowShared = true;

    auto& environment = description.AddInputTexture("InPrefilteredEnvironment", ResourceType::TextureCube);
    environment.Usages = ResourceUsage::NonPixelShaderRead;

    auto& brdf = description.AddInputTexture("InBRDFLut");
    brdf.Usages = ResourceUsage::NonPixelShaderRead;

    auto& occlusion = description.AddInputTexture("InAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Usages = ResourceUsage::NonPixelShaderRead;
    occlusion.IsOptionalInput = true;
    occlusion.AllowShared = true;

    description.RequireSameTextureSize({"InColorTexture", "InDepthBuffer", "InNormalRoughness", "InF0Baked", "InVelocityBuffer", "InAmbientOcclusion"});
    auto& output = description.AddOutputTexture("OutColorTexture");
    output.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    output.Size = Extent::SameAs("InColorTexture");
    output.Usages = ResourceUsage::UnorderedAccess;
    output.AllowShared = true;
}

void GDX12FFXSSSRPass::Initialize(PassInitContext& context)
{
    const int samples = context.GetParameter<int>("SamplesPerQuad");
    const int intersections = context.GetParameter<int>("MaxTraversalIntersections");
    const int occupancy = context.GetParameter<int>("MinTraversalOccupancy");
    const int mip = context.GetParameter<int>("MostDetailedMip");
    _intensity = context.GetParameter<float>("Intensity");
    _maxRoughness = context.GetParameter<float>("MaxRoughness");
    _roughnessFade = context.GetParameter<float>("RoughnessFade");
    _settings.depthBufferThickness = context.GetParameter<float>("DepthBufferThickness");
    _settings.temporalStabilityFactor = context.GetParameter<float>("TemporalStability");
    _settings.varianceThreshold = context.GetParameter<float>("VarianceThreshold");
    if ((samples != 1 && samples != 2 && samples != 4) || intersections < 1 || occupancy < 0 || occupancy > 64 || mip < 0 || mip > 15
        || !std::isfinite(_intensity) || _intensity < 0 || !std::isfinite(_maxRoughness) || _maxRoughness <= 0 || _maxRoughness > 1
        || !std::isfinite(_roughnessFade) || _roughnessFade < 0 || _roughnessFade > 1
        || !std::isfinite(_settings.depthBufferThickness) || _settings.depthBufferThickness <= 0
        || !std::isfinite(_settings.temporalStabilityFactor) || _settings.temporalStabilityFactor < 0 || _settings.temporalStabilityFactor > 1
        || !std::isfinite(_settings.varianceThreshold) || _settings.varianceThreshold < 0 || _settings.varianceThreshold > 1)
    { throw std::invalid_argument("ERROR: SSSR requires SamplesPerQuad 1/2/4, positive intersections/thickness, occupancy in [0,64], mip in [0,15], nonnegative intensity, roughness in (0,1], fade/stability/variance in [0,1]."); }
    _settings.samplesPerQuad = samples;
    _settings.maxTraversalIntersections = intersections;
    _settings.minTraversalOccupancy = occupancy;
    _settings.mostDetailedMip = mip;
    _settings.temporalVarianceGuidedTracingEnabled = context.GetParameter<bool>("VarianceGuidedTracing");
    _settings.roughnessThreshold = _maxRoughness * _maxRoughness;
    _settings.isRoughnessPerceptual = false;
    _settings.iblFactor = 0;
    _settings.normalUnPackMul = 1;
    _settings.motionVectorScale = {1, 1};

    auto* resources = context.GetDeviceResources();
    const auto& features = resources->Device->GetDeviceFeatures().Features;
    if (features.HighestShaderModel() < D3D_SHADER_MODEL_6_2 || !features.WaveOps())
    { throw std::runtime_error("ERROR: FidelityFX SSSR requires Shader Model 6.2 and wave operations."); }
    GDX12RootSignatureDesc root;
    root.Constants = {6};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 10;
    root.NumSingleUAVSlots = 4;
    root.StaticSamplers = GetStaticSamplers();
    _rootSignature = std::make_unique<GDX12RootSignature>(resources->Device, root);
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    const auto prepare = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "SSSRPass.hlsl", nullptr, "PrepareCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipeline.CS = {prepare->GetBufferPointer(), prepare->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_preparePipeline)));
    const auto compose = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "SSSRPass.hlsl", nullptr, "ComposeCS", "cs");
    pipeline.CS = {compose->GetBufferPointer(), compose->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_compositionPipeline)));
    CreateResources(context);
}

void GDX12FFXSSSRPass::CreateContext(PassInitContext& context)
{
    if (_context)
    {
        // Only wait for submitted work, a new flush signal could depend on this frame's pending copies.
        GDX12CommandQueue::CPUwait(context.GetDevice()->GetCommandQueue()->GetCompletionPoint());
        CheckFFX(ffxSssrContextDestroy(_context.get()));
        _context.reset();
    }
    const auto bytes = ffxGetScratchMemorySizeDX12(FFX_SSSR_CONTEXT_COUNT);
    _scratch.assign((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    FfxSssrContextDescription description{};
    description.flags = FFX_SSSR_ENABLE_DEPTH_INVERTED;
    description.renderSize = {_radiance->GetWidth(), _radiance->GetHeight()};
    description.normalsHistoryBufferFormat = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    CheckFFX(ffxGetInterfaceDX12(&description.backendInterface, ffxGetDeviceDX12(context.GetDevice()->GetDevice().Get()),
        _scratch.data(), bytes, FFX_SSSR_CONTEXT_COUNT));
    auto sdk = std::make_unique<FfxSssrContext>();
    CheckFFX(ffxSssrContextCreate(sdk.get(), &description));
    _context = std::move(sdk);
}

void GDX12FFXSSSRPass::CreateResources(PassInitContext& context)
{
    if (_context)
    {
        CheckFFX(ffxSssrContextDestroy(_context.get()));
        _context.reset();
    }
    const auto size = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    auto* resources = context.GetDeviceResources();
    auto create = [&](std::unique_ptr<GDX12Texture>& texture, DXGI_FORMAT format)
    {
        if (texture)
        {
            if (texture->GetWidth() != size.x || texture->GetHeight() != size.y) { texture->Resize(size.x, size.y); }
            return;
        }
        GDX12TextureDesc description;
        description.Device = resources->Device;
        description.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
        description.Width = size.x;
        description.Height = size.y;
        description.Format = format;
        description.CreateUAV = true;
        description.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        description.UAVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        texture = std::make_unique<GDX12Texture>();
        texture->Initialize(description);
    };
    create(_roughness, DXGI_FORMAT_R16_FLOAT);
    create(_motionVectors, DXGI_FORMAT_R16G16_FLOAT);
    create(_radiance, DXGI_FORMAT_R16G16B16A16_FLOAT);
    create(_fallback, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResetHistory();
}

void GDX12FFXSSSRPass::OnResourcesRecreated(PassInitContext& context)
{ CreateResources(context); }

void GDX12FFXSSSRPass::ResetHistory()
{ _historyCameraIndex = UINT_MAX; }

std::vector<ExternalResourceUse> GDX12FFXSSSRPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_roughness->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_motionVectors->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_radiance->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_fallback->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return uses;
}

void GDX12FFXSSSRPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    const auto* common = context.GetFrameData();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto size = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    const XMFLOAT2 jitter{common->ActiveCameraJitterOffsetX, common->ActiveCameraJitterOffsetY};
    const bool reset = _historyCameraIndex != common->ActiveCameraCBufferIndex
        || memcmp(&_previousProjection, &common->CameraViewToClip, sizeof(Matrix)) != 0;
    if (reset) { CreateContext(context); }
    Matrix projection = common->CameraViewToClip;
    projection._31 += jitter.x * 2.0f / size.x;
    projection._32 -= jitter.y * 2.0f / size.y;
    const Matrix viewProjection = common->WorldToCameraView * projection;
    const auto* color = context.GetInputTexture("InColorTexture");
    const auto* depth = context.GetInputTexture("InDepthBuffer");
    const auto* normals = context.GetInputTexture("InNormalRoughness");
    const auto* f0 = context.GetInputTexture("InF0Baked");
    const auto* velocity = context.GetInputTexture("InVelocityBuffer");
    const auto* environment = context.GetInputTexture("InPrefilteredEnvironment");
    const auto* brdf = context.GetInputTexture("InBRDFLut");
    const auto* occlusion = context.GetInputTexture("InAmbientOcclusion");
    // Projection moves rasterized geometry by -jitter pixels, motion points to the previous frame.
    const struct { XMFLOAT2 JitterDeltaUV; float MaxRoughness, RoughnessFade, Intensity; UINT HasAmbientOcclusion; }
        parameters = {{reset ? 0 : (jitter.x - _previousJitter.x) / size.x, reset ? 0 : (jitter.y - _previousJitter.y) / size.y},
        _maxRoughness, _roughnessFade, _intensity, occlusion ? 1u : 0u};
    auto bind = [&]
    {
        commands->SetComputeRootSignature(_rootSignature.get());
        commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
        commands->SetComputeRootConstants(0, parameters);
        commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(common->ActiveCameraCBufferIndex));
        commands->SetComputeSRV(0, color->GetSRV()->GPUHandle);
        commands->SetComputeSRV(1, depth->GetSRV()->GPUHandle);
        commands->SetComputeSRV(2, normals->GetSRV()->GPUHandle);
        commands->SetComputeSRV(3, f0->GetSRV()->GPUHandle);
        commands->SetComputeSRV(4, velocity->GetSRV()->GPUHandle);
        commands->SetComputeSRV(5, environment->GetSRV()->GPUHandle);
        commands->SetComputeSRV(6, brdf->GetSRV()->GPUHandle);
        commands->SetComputeSRV(9, occlusion ? occlusion->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    };
    commands->BeginPixEvent("FidelityFX SSSR", Colors::SteelBlue);
    bind();
    commands->SetPipelineState(_preparePipeline);
    commands->SetComputeUAV(0, _roughness->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _motionVectors->GetUAV()->GPUHandle);
    commands->SetComputeUAV(3, _fallback->GetUAV()->GPUHandle);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    commands->ResourceBarrier({_roughness->GetResource()->GetNonPixelShaderResourceBarrier(),
        _motionVectors->GetResource()->GetNonPixelShaderResourceBarrier(), _fallback->GetResource()->GetNonPixelShaderResourceBarrier()});
    auto dispatch = _settings;
    dispatch.commandList = ffxGetCommandListDX12(commands->GetCommandList().Get());
    dispatch.color = GetSDKResource(color->GetTexture());
    dispatch.depth = GetSDKResource(depth->GetTexture());
    dispatch.normal = GetSDKResource(normals->GetTexture());
    dispatch.materialParameters = GetSDKResource(_roughness.get());
    dispatch.motionVectors = GetSDKResource(_motionVectors.get());
    dispatch.environmentMap = GetSDKResource(_fallback.get());
    dispatch.brdfTexture = GetSDKResource(brdf->GetTexture());
    dispatch.output = GetSDKResource(_radiance.get());
    dispatch.renderSize = {size.x, size.y};
    const UINT maxMip = (std::min)(6u, static_cast<UINT>(std::log2((std::max)(size.x, size.y))));
    dispatch.mostDetailedMip = (std::min)(_settings.mostDetailedMip, maxMip);
    const Matrix inverseViewProjection = viewProjection.Invert(), inverseProjection = projection.Invert();
    const Matrix inverseView = common->WorldToCameraView.Invert();
    memcpy(dispatch.invViewProjection, &inverseViewProjection, sizeof(Matrix));
    memcpy(dispatch.projection, &projection, sizeof(Matrix));
    memcpy(dispatch.invProjection, &inverseProjection, sizeof(Matrix));
    memcpy(dispatch.view, &common->WorldToCameraView, sizeof(Matrix));
    memcpy(dispatch.invView, &inverseView, sizeof(Matrix));
    memcpy(dispatch.prevViewProjection, reset ? &viewProjection : &_previousViewProjection, sizeof(Matrix));
    const auto result = ffxSssrContextDispatch(_context.get(), &dispatch);
    commands->InvalidateCachedState();
    CheckFFX(result);
    commands->ResourceBarrier({_radiance->GetResource()->GetNonPixelShaderResourceBarrier()});
    bind();
    commands->SetPipelineState(_compositionPipeline);
    commands->SetComputeSRV(7, _radiance->GetSRV()->GPUHandle);
    commands->SetComputeSRV(8, _fallback->GetSRV()->GPUHandle);
    commands->SetComputeUAV(2, context.GetOutputTexture("OutColorTexture").GetUAV()->GPUHandle);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    commands->ResourceBarrier({_roughness->GetResource()->GetUnorderedAccessBarrier(), _motionVectors->GetResource()->GetUnorderedAccessBarrier(),
        _radiance->GetResource()->GetUnorderedAccessBarrier(), _fallback->GetResource()->GetUnorderedAccessBarrier()});
    commands->EndPixEvent();
    _previousProjection = common->CameraViewToClip;
    _previousViewProjection = viewProjection;
    _previousJitter = jitter;
    _historyCameraIndex = common->ActiveCameraCBufferIndex;
}
