#include "Engine.RendererDX12/RenderPasses/GDX12RTAOPass.h"
#include <cmath>
#include <filesystem>
#include <sstream>

namespace
{
    constexpr nrd::Identifier DenoiserId = 0;

    void CheckNRD(nrd::Result result)
    {
        if (result != nrd::Result::SUCCESS)
        { throw std::runtime_error("ERROR: NRD operation failed, result " + std::to_string(static_cast<unsigned>(result))); }
    }

    DXGI_FORMAT GetDenoisingFormat(nrd::Format format)
    {
        switch (format)
        {
        case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
        case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
        default: throw std::runtime_error("ERROR: Unsupported NRD AO texture format.");
        }
    }
}


GDX12RTAOPass::GDX12RTAOPass()
{ _flags = RENDER_PASS_FLAG_USE_RAYTRACING | RENDER_PASS_FLAG_USE_CAMERAS; }

GDX12RTAOPass::~GDX12RTAOPass()
{ if (_denoiser) { nrd::DestroyInstance(*_denoiser); } }

void GDX12RTAOPass::Describe(PassDescription& description) const
{
    description.AddParameter("Radius", 50.0f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("Bias", 0.5f);
    description.AddParameter("Samples", 2);
    description.AddParameter("FadeStart", 5000.0f);
    description.AddParameter("FadeEnd", 15000.0f);
    description.AddParameter("MaxAccumulatedFrames", 30);
    description.AddParameter("DenoisingRadius", 30.0f);

    auto& depth = description.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::NonPixelShaderRead;

    auto& normals = description.AddInputTexture("InNormalRoughness");
    normals.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normals.Usages = ResourceUsage::NonPixelShaderRead;
    normals.AllowShared = true;

    auto& velocity = description.AddInputTexture("InVelocityBuffer");
    velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
    velocity.Usages = ResourceUsage::NonPixelShaderRead;
    velocity.AllowShared = true;

    auto& occlusion = description.AddOutputTexture("OutAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Size = Extent::SameAs("InDepthBuffer");
    occlusion.Usages = ResourceUsage::UnorderedAccess;
    occlusion.AllowShared = true;
    description.RequireSameTextureSize({"InDepthBuffer", "InNormalRoughness", "InVelocityBuffer"});
}

void GDX12RTAOPass::Initialize(PassInitContext& context)
{
    _radius = context.GetParameter<float>("Radius");
    _intensity = context.GetParameter<float>("Intensity");
    _bias = context.GetParameter<float>("Bias");
    _fadeStart = context.GetParameter<float>("FadeStart");
    _fadeEnd = context.GetParameter<float>("FadeEnd");
    const auto samples = context.GetParameter<int>("Samples");
    if (!std::isfinite(_radius) || _radius <= 0 || !std::isfinite(_intensity) || _intensity < 0
        || !std::isfinite(_bias) || _bias < 0 || samples < 1 || samples > 64
        || !std::isfinite(_fadeStart) || _fadeStart < 0 || !std::isfinite(_fadeEnd) || _fadeEnd <= _fadeStart)
    { throw std::invalid_argument("ERROR: RTAO requires positive Radius, nonnegative Intensity/Bias, Samples in [1,64], and 0 <= FadeStart < FadeEnd. Distances are in scene centimetres."); }
    _samples = static_cast<UINT>(samples);

    auto* resources = context.GetDeviceResources();
    GDX12RootSignatureDesc root;
    root.Constants = {7};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 10;
    root.NumSingleUAVSlots = 4;
    root.SRVRanges.emplace_back(Texture2D_RangeLength);
    root.StaticSamplers = GetStaticSamplers();
    _rootSignature = std::make_unique<GDX12RootSignature>(resources->Device, root);
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    const auto shader = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "RTAOPass.hlsl", nullptr, "CS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_occlusionPipeline)));
    const auto resolve = shaderCompiler.CompileShader(resources->Device, SHADERS_FOLDER "RTAOPass.hlsl", nullptr, "ResolveCS", "cs");
    pipeline.CS = {resolve->GetBufferPointer(), resolve->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_resolvePipeline)));
    InitializeDenoiser(context);
    CreateDenoisingTextures(context);
}

void GDX12RTAOPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    const struct { float Radius, Intensity, Bias; UINT Samples; float FadeStart, FadeEnd; UINT FrameIndex; }
        parameters = {_radius, _intensity, _bias, _samples, _fadeStart, _fadeEnd, _denoisingSettings.frameIndex};

    commands->BeginPixEvent("Ray Traced Ambient Occlusion", Colors::DarkSlateBlue);
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetPipelineState(_occlusionPipeline);
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetComputeRootConstants(0, parameters);
    commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, resources->RayTracingScene->GetTLASSRV(context.GetFrameSlotResourcesIndex())->GPUHandle);
    commands->SetComputeSRV(3, resources->GeometryBuffer->GetVertexSRV()->GPUHandle);
    commands->SetComputeSRV(4, resources->GeometryBuffer->GetIndexSRV()->GPUHandle);
    commands->SetComputeSRV(5, resources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(6, frame.InstanceCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(7, frame.TransformCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(8, frame.MaterialCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(10, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetComputeUAV(0, _hitDistance->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, _packedNormals->GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _viewDepth->GetUAV()->GPUHandle);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    Denoise(context);
    commands->ResourceBarrier({_denoisedOcclusion->GetResource()->GetNonPixelShaderResourceBarrier()});
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetPipelineState(_resolvePipeline);
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetComputeRootConstants(0, parameters);
    commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, context.GetInputTexture("InDepthBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(9, _denoisedOcclusion->GetSRV()->GPUHandle);
    commands->SetComputeUAV(3, context.GetOutputTexture("OutAmbientOcclusion").GetUAV()->GPUHandle);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    for (const auto& texture : _permanentTextures) { commands->ResourceBarrier({texture->GetResource()->GetUnorderedAccessBarrier()}); }
    for (const auto& texture : _transientTextures) { commands->ResourceBarrier({texture->GetResource()->GetUnorderedAccessBarrier()}); }
    commands->ResourceBarrier({_hitDistance->GetResource()->GetUnorderedAccessBarrier(), _packedNormals->GetResource()->GetUnorderedAccessBarrier(),
        _viewDepth->GetResource()->GetUnorderedAccessBarrier(), _denoisedOcclusion->GetResource()->GetUnorderedAccessBarrier()});
    commands->EndPixEvent();
}

void GDX12RTAOPass::InitializeDenoiser(PassInitContext& context)
{
    const auto* library = nrd::GetLibraryDesc();
    if (library->normalEncoding != nrd::NormalEncoding::RGBA16_SNORM || library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
    { throw std::runtime_error("ERROR: NRD AO requires the bundled RGBA16_SNORM / LINEAR build."); }
    const nrd::DenoiserDesc denoiser{DenoiserId, nrd::Denoiser::REBLUR_DIFFUSE_OCCLUSION};
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = &denoiser;
    creation.denoisersNum = 1;
    CheckNRD(nrd::CreateInstance(creation, _denoiser));
    const int historyFrames = context.GetParameter<int>("MaxAccumulatedFrames");
    const float blurRadius = context.GetParameter<float>("DenoisingRadius");
    if (historyFrames < 1 || UINT(historyFrames) > nrd::REBLUR_MAX_HISTORY_FRAME_NUM || !std::isfinite(blurRadius) || blurRadius < 1)
    { throw std::invalid_argument("ERROR: NRD AO requires MaxAccumulatedFrames in [1,63] and DenoisingRadius >= 1 pixel."); }
    nrd::ReblurSettings settings{};
    settings.hitDistanceParameters = {_radius, 0.0f, 1.0f};
    settings.maxAccumulatedFrameNum = historyFrames;
    settings.maxFastAccumulatedFrameNum = (std::min)(6u, UINT(historyFrames));
    settings.historyFixFrameNum = (std::min)(3u, settings.maxFastAccumulatedFrameNum - 1);
    settings.maxBlurRadius = blurRadius;
    CheckNRD(nrd::SetDenoiserSettings(*_denoiser, DenoiserId, &settings));

    const auto* description = nrd::GetInstanceDesc(*_denoiser);
    const auto& pool = description->descriptorPoolDesc;
    GDX12RootSignatureDesc root;
    root.NumSingleCBVSlots = 1;
    root.CBVBaseRegister = description->constantBufferRegisterIndex;
    root.CBVRegisterSpace = description->constantBufferAndSamplersSpaceIndex;
    root.NumSingleSRVSlots = pool.perSetTexturesMaxNum;
    root.SRVBaseRegister = description->resourcesBaseRegisterIndex;
    root.SRVRegisterSpace = description->resourcesSpaceIndex;
    root.NumSingleUAVSlots = pool.perSetStorageTexturesMaxNum;
    root.UAVBaseRegister = description->resourcesBaseRegisterIndex;
    root.UAVRegisterSpace = description->resourcesSpaceIndex;
    // NRD rewrites transient textures between dispatches in the same command list.
    root.SingleSRVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    root.SingleUAVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    CD3DX12_STATIC_SAMPLER_DESC pointSampler;
    pointSampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK,
        0, D3D12_FLOAT32_MAX, D3D12_SHADER_VISIBILITY_ALL, description->constantBufferAndSamplersSpaceIndex);
    auto linearSampler = pointSampler;
    linearSampler.ShaderRegister = 1;
    linearSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    root.StaticSamplers = {pointSampler, linearSampler};
    auto* resources = context.GetDeviceResources();
    _denoisingRootSignature = std::make_unique<GDX12RootSignature>(resources->Device, root);
    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    for (UINT i = 0; i < description->pipelinesNum; ++i)
    {
        std::istringstream identifier(description->pipelines[i].shaderIdentifier);
        std::string filename, define;
        std::getline(identifier, filename, '|');
        std::vector<std::pair<std::string, std::string>> defines{{"NRD_INTERNAL", "1"}};
        while (std::getline(identifier, define, '|'))
        {
            const auto separator = define.find('=');
            defines.emplace_back(define.substr(0, separator), define.substr(separator + 1));
        }
        std::vector<D3D_SHADER_MACRO> macros;
        for (const auto& item : defines) { macros.push_back({item.first.c_str(), item.second.c_str()}); }
        macros.push_back({nullptr, nullptr});
        const auto path = std::filesystem::path(NRD_SHADERS_FOLDER) / filename;
        const auto shader = shaderCompiler.CompileShader(resources->Device, path.wstring(), macros.data(), description->shaderEntryPoint, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = _denoisingRootSignature->GetRootSignature().Get();
        pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        ComPtr<ID3D12PipelineState> state;
        ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&state)));
        _denoisingPipelines.push_back(state);
    }
    const auto stride = (description->constantBufferMaxDataSize + 255) & ~255u;
    _denoisingConstants = std::make_unique<GDX12UploadBuffer<BYTE>>(resources->Device,
        stride * pool.setsMaxNum * context.GetFrameSlotResourcesCount(), EBufferType::Upload, false);
}

void GDX12RTAOPass::CreateDenoisingTextures(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    if (size.x > UINT16_MAX || size.y > UINT16_MAX) { throw std::invalid_argument("ERROR: NRD texture dimensions exceed 65535."); }
    auto* resources = context.GetDeviceResources();
    auto create = [&](std::unique_ptr<GDX12Texture>& texture, DXGI_FORMAT format, UINT downsample = 1)
    {
        const auto width = (size.x + downsample - 1) / downsample, height = (size.y + downsample - 1) / downsample;
        if (texture)
        {
            if (texture->GetWidth() != width || texture->GetHeight() != height) { texture->Resize(width, height); }
            return;
        }
        GDX12TextureDesc description;
        description.Device = resources->Device;
        description.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
        description.Width = width;
        description.Height = height;
        description.Format = format;
        description.CreateUAV = true;
        description.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        description.UAVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
        texture = std::make_unique<GDX12Texture>();
        texture->Initialize(description);
    };
    create(_hitDistance, DXGI_FORMAT_R16_FLOAT);
    create(_packedNormals, DXGI_FORMAT_R16G16B16A16_FLOAT);
    create(_viewDepth, DXGI_FORMAT_R32_FLOAT);
    create(_denoisedOcclusion, DXGI_FORMAT_R16_FLOAT);
    const auto* description = nrd::GetInstanceDesc(*_denoiser);
    _permanentTextures.resize(description->permanentPoolSize);
    for (UINT i = 0; i < description->permanentPoolSize; ++i)
    { create(_permanentTextures[i], GetDenoisingFormat(description->permanentPool[i].format), description->permanentPool[i].downsampleFactor); }
    _transientTextures.resize(description->transientPoolSize);
    for (UINT i = 0; i < description->transientPoolSize; ++i)
    { create(_transientTextures[i], GetDenoisingFormat(description->transientPool[i].format), description->transientPool[i].downsampleFactor); }
    ResetHistory();
}

void GDX12RTAOPass::OnResourcesRecreated(PassInitContext& context)
{ CreateDenoisingTextures(context); }

void GDX12RTAOPass::ResetHistory()
{ _historyCameraIndex = UINT_MAX; }

std::vector<ExternalResourceUse> GDX12RTAOPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_hitDistance->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_packedNormals->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_viewDepth->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_denoisedOcclusion->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    for (const auto& texture : _permanentTextures) { uses.push_back({texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}}); }
    for (const auto& texture : _transientTextures) { uses.push_back({texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}}); }
    uses.push_back({&_denoisingConstants->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
    return uses;
}

void GDX12RTAOPass::Denoise(PassExecutionContext& context)
{
    const auto* common = context.GetFrameData();
    const auto size = context.GetOutputResourceDescription("OutAmbientOcclusion").GetSize();
    nrd::CommonSettings settings{};
    // Row-vector SimpleMath matrices already have NRD's column-vector, column-major byte layout.
    memcpy(settings.viewToClipMatrix, &common->CameraViewToClip, sizeof(Matrix));
    memcpy(settings.worldToViewMatrix, &common->WorldToCameraView, sizeof(Matrix));
    const bool reset = _historyCameraIndex != common->ActiveCameraCBufferIndex
        || memcmp(_denoisingSettings.viewToClipMatrix, settings.viewToClipMatrix, sizeof(Matrix)) != 0;
    memcpy(settings.viewToClipMatrixPrev, reset ? settings.viewToClipMatrix : _denoisingSettings.viewToClipMatrix, sizeof(Matrix));
    memcpy(settings.worldToViewMatrixPrev, reset ? settings.worldToViewMatrix : _denoisingSettings.worldToViewMatrix, sizeof(Matrix));
    settings.motionVectorScale[0] = 0.5f;
    settings.motionVectorScale[1] = -0.5f;
    settings.cameraJitter[0] = common->ActiveCameraJitterOffsetX;
    settings.cameraJitter[1] = common->ActiveCameraJitterOffsetY;
    settings.cameraJitterPrev[0] = reset ? settings.cameraJitter[0] : _denoisingSettings.cameraJitter[0];
    settings.cameraJitterPrev[1] = reset ? settings.cameraJitter[1] : _denoisingSettings.cameraJitter[1];
    settings.resourceSize[0] = settings.resourceSizePrev[0] = settings.rectSize[0] = settings.rectSizePrev[0] = static_cast<uint16_t>(size.x);
    settings.resourceSize[1] = settings.resourceSizePrev[1] = settings.rectSize[1] = settings.rectSizePrev[1] = static_cast<uint16_t>(size.y);
    settings.denoisingRange = _fadeEnd;
    settings.strandThickness *= 100.0f;
    settings.frameIndex = _denoisingSettings.frameIndex;
    settings.accumulationMode = reset ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
    CheckNRD(nrd::SetCommonSettings(*_denoiser, settings));
    const nrd::DispatchDesc* dispatches = nullptr;
    UINT dispatchCount = 0;
    CheckNRD(nrd::GetComputeDispatches(*_denoiser, &DenoiserId, 1, dispatches, dispatchCount));
    const auto* description = nrd::GetInstanceDesc(*_denoiser);
    auto& constants = *_denoisingConstants;
    const auto stride = (description->constantBufferMaxDataSize + 255) & ~255u;
    const auto slotOffset = context.GetFrameSlotResourcesIndex() * description->descriptorPoolDesc.setsMaxNum * stride;
    auto* commands = context.GetCommandList();
    auto native = commands->GetCommandList();
    commands->SetComputeRootSignature(_denoisingRootSignature.get());
    commands->BeginPixEvent("NRD REBLUR AO", Colors::MediumPurple);
    for (UINT i = 0; i < dispatchCount; ++i)
    {
        const auto& dispatch = dispatches[i];
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        std::vector<std::pair<UINT, D3D12_GPU_DESCRIPTOR_HANDLE>> bindings;
        UINT textureIndex = 0, storageIndex = 0;
        for (UINT j = 0; j < dispatch.resourcesNum; ++j)
        {
            const auto& resource = dispatch.resources[j];
            GDX12Texture* texture = nullptr;
            switch (resource.type)
            {
            case nrd::ResourceType::IN_MV: texture = context.GetInputTexture("InVelocityBuffer")->GetTexture(); break;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS: texture = _packedNormals.get(); break;
            case nrd::ResourceType::IN_VIEWZ: texture = _viewDepth.get(); break;
            case nrd::ResourceType::IN_DIFF_HITDIST: texture = _hitDistance.get(); break;
            case nrd::ResourceType::OUT_DIFF_HITDIST: texture = _denoisedOcclusion.get(); break;
            case nrd::ResourceType::PERMANENT_POOL: texture = _permanentTextures.at(resource.indexInPool).get(); break;
            case nrd::ResourceType::TRANSIENT_POOL: texture = _transientTextures.at(resource.indexInPool).get(); break;
            default: throw std::runtime_error("ERROR: Unsupported NRD AO binding.");
            }
            const bool storage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            if (resource.type != nrd::ResourceType::IN_MV)
            {
                const auto state = storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                if (texture->GetResource()->GetCurrentState() != state) { barriers.push_back(texture->GetResource()->GetTransitionBarrier(state)); }
                else if (storage) { barriers.push_back(texture->GetResource()->GetUAVBarrier()); }
            }
            const auto rootIndex = storage ? 1 + description->descriptorPoolDesc.perSetTexturesMaxNum + storageIndex++ : 1 + textureIndex++;
            bindings.emplace_back(rootIndex, storage ? texture->GetUAV()->GPUHandle : texture->GetSRV()->GPUHandle);
        }
        if (!barriers.empty()) { native->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data()); }
        for (const auto& binding : bindings) { native->SetComputeRootDescriptorTable(binding.first, binding.second); }
        if (dispatch.constantBufferDataSize)
        {
            const auto offset = slotOffset + i * stride;
            constants.CopyData(offset, dispatch.constantBufferData, dispatch.constantBufferDataSize);
            native->SetComputeRootConstantBufferView(0, constants.GetElementAddress(offset));
        }
        commands->BeginPixEvent(dispatch.name, Colors::MediumPurple);
        native->SetPipelineState(_denoisingPipelines.at(dispatch.pipelineIndex).Get());
        native->Dispatch(dispatch.gridWidth, dispatch.gridHeight, 1);
        commands->EndPixEvent();
    }
    commands->EndPixEvent();
    commands->InvalidateCachedState();
    _denoisingSettings = settings;
    ++_denoisingSettings.frameIndex;
    _historyCameraIndex = common->ActiveCameraCBufferIndex;
}
