#include "Engine.RendererDX12/RenderPasses/GDX12RTReflectionsPass.h"
#include <cmath>
#include <filesystem>
#include <sstream>

namespace
{
    constexpr nrd::Identifier DenoiserId = 0;
    constexpr auto HitDistanceParameters = [] {
        nrd::ReblurHitDistanceParameters parameters;
        parameters.A *= 100.0f;
        return parameters;
    }();

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
        case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        default: throw std::runtime_error("ERROR: Unsupported NRD reflections texture format.");
        }
    }
}


GDX12RTReflectionsPass::GDX12RTReflectionsPass()
{ _flags = RENDER_PASS_FLAG_USE_RAYTRACING | RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_LIGHTING; }

GDX12RTReflectionsPass::~GDX12RTReflectionsPass()
{ if (_denoiser) { nrd::DestroyInstance(*_denoiser); } }

void GDX12RTReflectionsPass::Describe(PassDescription& description) const
{
    description.AddParameter("SamplesPerPixel", 1);
    description.AddParameter("CheckerboardTracing", true);
    description.AddParameter("ResolutionScale", 0.5f);
    description.AddParameter("MaxDistance", 10000.0f);
    description.AddParameter("MaxRoughness", 0.6f);
    description.AddParameter("RoughnessFade", 0.1f);
    description.AddParameter("Bias", 0.5f);
    description.AddParameter("Intensity", 1.0f);
    description.AddParameter("MaxAccumulatedFrames", 30);
    description.AddParameter("DenoisingRadius", 30.0f);

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

    auto& velocity = description.AddInputTexture("InVelocityBuffer");
    velocity.Formats = {DXGI_FORMAT_R16G16_FLOAT};
    velocity.Usages = ResourceUsage::NonPixelShaderRead;
    velocity.AllowShared = true;

    auto& f0 = description.AddInputTexture("InF0Baked");
    f0.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    f0.Usages = ResourceUsage::NonPixelShaderRead;
    f0.AllowShared = true;

    auto& skyBox = description.AddInputTexture("InSkyBoxMap", ResourceType::TextureCube);
    skyBox.Usages = ResourceUsage::NonPixelShaderRead;

    auto& irradiance = description.AddInputTexture("InIrradiance", ResourceType::TextureCube);
    irradiance.Usages = ResourceUsage::NonPixelShaderRead;
    irradiance.IsOptionalInput = true;

    auto& prefiltered = description.AddInputTexture("InPrefilteredEnvironment", ResourceType::TextureCube);
    prefiltered.Usages = ResourceUsage::NonPixelShaderRead;
    prefiltered.IsOptionalInput = true;

    auto& brdfLut = description.AddInputTexture("InBRDFLut");
    brdfLut.Usages = ResourceUsage::NonPixelShaderRead;
    brdfLut.IsOptionalInput = true;

    auto& directionalMaps = description.AddInputTexture("InDirectionalMaps", ResourceType::Texture2DArray);
    directionalMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    directionalMaps.Usages = ResourceUsage::NonPixelShaderRead;
    directionalMaps.IsOptionalInput = true;

    auto& pointSpotMaps = description.AddInputTexture("InPointSpotMaps", ResourceType::Texture2DArray);
    pointSpotMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    pointSpotMaps.Usages = ResourceUsage::NonPixelShaderRead;
    pointSpotMaps.IsOptionalInput = true;

    auto& occlusion = description.AddInputTexture("InAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Usages = ResourceUsage::NonPixelShaderRead;
    occlusion.AllowShared = true;
    occlusion.IsOptionalInput = true;

    description.RequireSameTextureSize({"InColorTexture", "InDepthBuffer", "InNormalRoughness", "InVelocityBuffer", "InF0Baked", "InAmbientOcclusion"});
    auto& output = description.AddOutputTexture("OutColorTexture");
    output.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    output.Size = Extent::SameAs("InColorTexture");
    output.Usages = ResourceUsage::UnorderedAccess;
    output.AllowShared = true;
}

void GDX12RTReflectionsPass::Initialize(PassInitContext& context)
{
    const auto samplesPerPixel = context.GetParameter<int>("SamplesPerPixel");
    _checkerboardTracing = context.GetParameter<bool>("CheckerboardTracing");
    _resolutionScale = context.GetParameter<float>("ResolutionScale");
    _maxDistance = context.GetParameter<float>("MaxDistance");
    _maxRoughness = context.GetParameter<float>("MaxRoughness");
    _roughnessFade = context.GetParameter<float>("RoughnessFade");
    _bias = context.GetParameter<float>("Bias");
    _intensity = context.GetParameter<float>("Intensity");
    if (!std::isfinite(_resolutionScale) || _resolutionScale <= 0 || _resolutionScale > 1)
    { throw std::invalid_argument("ERROR: RT reflections require ResolutionScale in (0,1]."); }
    if (samplesPerPixel < 1 || samplesPerPixel > 64 || !std::isfinite(_maxDistance) || _maxDistance <= 0
        || !std::isfinite(_maxRoughness) || _maxRoughness <= 0 || _maxRoughness > 1
        || !std::isfinite(_roughnessFade) || _roughnessFade < 0 || _roughnessFade > 1
        || !std::isfinite(_bias) || _bias < 0 || !std::isfinite(_intensity) || _intensity < 0)
    { throw std::invalid_argument("ERROR: RT reflections require SamplesPerPixel in [1,64], positive MaxDistance, MaxRoughness in (0,1], RoughnessFade in [0,1] and nonnegative Bias/Intensity."); }
    _samplesPerPixel = static_cast<UINT>(samplesPerPixel);

    auto* resources = context.GetDeviceResources();
    GDX12RootSignatureDesc root;
    root.Constants = {17};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 22;
    root.NumSingleUAVSlots = 5;
    root.SRVRanges.emplace_back(Texture2D_RangeLength);
    root.SingleSRVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    root.SingleUAVRangeFlags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    root.StaticSamplers = GetStaticSamplers();
    CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    root.StaticSamplers.push_back(shadowSampler);
    _rootSignature = std::make_unique<GDX12RootSignature>(resources->Device, root);

    auto& shaderCompiler = GDX12ShaderCompiler::GetInstance();
    const auto library = shaderCompiler.CompileRayTracingLibrary(resources->Device, SHADERS_FOLDER "RTReflectionsPass.hlsl");
    GDX12RayTracingPipelineStateDesc pipeline;
    pipeline.Library = {library->GetBufferPointer(), library->GetBufferSize()};
    pipeline.ShaderFunctionsNames = {L"RayGeneration", L"ClosestHit", L"AnyHit", L"Miss"};
    pipeline.GlobalRootSignature = _rootSignature.get();
    pipeline.MaxPayloadSize = 6 * sizeof(float);
    GDX12RayTracingHitGroupDesc hitGroup;
    hitGroup.Name = L"ReflectionHitGroup";
    hitGroup.ClosestHitFunctionName = L"ClosestHit";
    hitGroup.AnyHitFunctionName = L"AnyHit";
    pipeline.HitGroups = {hitGroup};
    _reflectionPipeline = std::make_unique<GDX12RayTracingPipelineState>(resources->Device, pipeline);
    GDX12ShaderTableDesc table;
    table.RayGeneration = L"RayGeneration";
    table.Miss = {L"Miss"};
    table.HitGroups = {L"ReflectionHitGroup"};
    _shaderTable = std::make_unique<GDX12ShaderTable>(*_reflectionPipeline, table);

    const auto composition = shaderCompiler.CompileShader(resources->Device,
        SHADERS_FOLDER "RTReflectionsPass.hlsl", nullptr, "ComposeCS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
    compute.pRootSignature = _rootSignature->GetRootSignature().Get();
    compute.CS = {composition->GetBufferPointer(), composition->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&compute, IID_PPV_ARGS(&_compositionPipeline)));
    InitializeDenoiser(context);
    CreateDenoisingTextures(context);
}

void GDX12RTReflectionsPass::InitializeDenoiser(PassInitContext& context)
{
    const auto* library = nrd::GetLibraryDesc();
    if (library->normalEncoding != nrd::NormalEncoding::RGBA16_SNORM || library->roughnessEncoding != nrd::RoughnessEncoding::LINEAR)
    { throw std::runtime_error("ERROR: NRD reflections requires the bundled RGBA16_SNORM / LINEAR build."); }
    const nrd::DenoiserDesc denoiser{DenoiserId, nrd::Denoiser::REBLUR_SPECULAR};
    nrd::InstanceCreationDesc creation{};
    creation.denoisers = &denoiser;
    creation.denoisersNum = 1;
    CheckNRD(nrd::CreateInstance(creation, _denoiser));
    const int historyFrames = context.GetParameter<int>("MaxAccumulatedFrames");
    const float blurRadius = context.GetParameter<float>("DenoisingRadius");
    if (historyFrames < 1 || UINT(historyFrames) > nrd::REBLUR_MAX_HISTORY_FRAME_NUM || !std::isfinite(blurRadius) || blurRadius < 1)
    { throw std::invalid_argument("ERROR: NRD reflections requires MaxAccumulatedFrames in [1,63] and DenoisingRadius >= 1 pixel."); }
    nrd::ReblurSettings settings{};
    settings.hitDistanceParameters = HitDistanceParameters;
    settings.maxAccumulatedFrameNum = historyFrames;
    settings.maxFastAccumulatedFrameNum = (std::min)(6u, UINT(historyFrames));
    settings.historyFixFrameNum = (std::min)(3u, settings.maxFastAccumulatedFrameNum - 1);
    settings.maxBlurRadius = blurRadius;
    // NRD's WHITE mode selects specular pixels with even (x ^ y ^ frameIndex) parity.
    settings.checkerboardMode = _checkerboardTracing ? nrd::CheckerboardMode::WHITE : nrd::CheckerboardMode::OFF;
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

void GDX12RTReflectionsPass::CreateDenoisingTextures(PassInitContext& context)
{
    const auto outputSize = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    const UInt2 size{
        (std::max)(1u, static_cast<UINT>(std::ceil(outputSize.x * _resolutionScale))),
        (std::max)(1u, static_cast<UINT>(std::ceil(outputSize.y * _resolutionScale)))};
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
    create(_reflections, DXGI_FORMAT_R16G16B16A16_FLOAT);
    create(_packedNormals, DXGI_FORMAT_R16G16B16A16_FLOAT);
    create(_viewDepth, DXGI_FORMAT_R32_FLOAT);
    create(_motionVectors, DXGI_FORMAT_R16G16_FLOAT);
    create(_denoisedReflections, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const auto* description = nrd::GetInstanceDesc(*_denoiser);
    _permanentTextures.resize(description->permanentPoolSize);
    for (UINT i = 0; i < description->permanentPoolSize; ++i)
    { create(_permanentTextures[i], GetDenoisingFormat(description->permanentPool[i].format), description->permanentPool[i].downsampleFactor); }
    _transientTextures.resize(description->transientPoolSize);
    for (UINT i = 0; i < description->transientPoolSize; ++i)
    { create(_transientTextures[i], GetDenoisingFormat(description->transientPool[i].format), description->transientPool[i].downsampleFactor); }
    ResetHistory();
}

void GDX12RTReflectionsPass::OnResourcesRecreated(PassInitContext& context)
{ CreateDenoisingTextures(context); }

void GDX12RTReflectionsPass::ResetHistory()
{ _historyCameraIndex = UINT_MAX; }

std::vector<ExternalResourceUse> GDX12RTReflectionsPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_reflections->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_packedNormals->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_viewDepth->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_motionVectors->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    uses.push_back({_denoisedReflections->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    for (const auto& texture : _permanentTextures) { uses.push_back({texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}}); }
    for (const auto& texture : _transientTextures) { uses.push_back({texture->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}}); }
    uses.push_back({&_denoisingConstants->GetResource(), D3D12_RESOURCE_STATE_GENERIC_READ, {}});
    return uses;
}

void GDX12RTReflectionsPass::Denoise(PassExecutionContext& context)
{
    const auto* common = context.GetFrameData();
    const auto outputSize = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    const UInt2 size{_reflections->GetWidth(), _reflections->GetHeight()};
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
    settings.cameraJitter[0] = common->ActiveCameraJitterOffsetX * size.x / outputSize.x;
    settings.cameraJitter[1] = common->ActiveCameraJitterOffsetY * size.y / outputSize.y;
    settings.cameraJitterPrev[0] = reset ? settings.cameraJitter[0] : _denoisingSettings.cameraJitter[0];
    settings.cameraJitterPrev[1] = reset ? settings.cameraJitter[1] : _denoisingSettings.cameraJitter[1];
    settings.resourceSize[0] = settings.resourceSizePrev[0] = settings.rectSize[0] = settings.rectSizePrev[0] = static_cast<uint16_t>(size.x);
    settings.resourceSize[1] = settings.resourceSizePrev[1] = settings.rectSize[1] = settings.rectSizePrev[1] = static_cast<uint16_t>(size.y);
    settings.denoisingRange = common->ActiveCameraFarPlane;
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
    commands->BeginPixEvent("NRD REBLUR Reflections", Colors::MediumPurple);
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<std::pair<UINT, D3D12_GPU_DESCRIPTOR_HANDLE>> bindings;
    const auto maxResources = description->descriptorPoolDesc.perSetTexturesMaxNum + description->descriptorPoolDesc.perSetStorageTexturesMaxNum;
    barriers.reserve(maxResources);
    bindings.reserve(maxResources);
    for (UINT i = 0; i < dispatchCount; ++i)
    {
        const auto& dispatch = dispatches[i];
        barriers.clear();
        bindings.clear();
        UINT textureIndex = 0, storageIndex = 0;
        for (UINT j = 0; j < dispatch.resourcesNum; ++j)
        {
            const auto& resource = dispatch.resources[j];
            GDX12Texture* texture = nullptr;
            switch (resource.type)
            {
            case nrd::ResourceType::IN_MV: texture = _motionVectors.get(); break;
            case nrd::ResourceType::IN_NORMAL_ROUGHNESS: texture = _packedNormals.get(); break;
            case nrd::ResourceType::IN_VIEWZ: texture = _viewDepth.get(); break;
            case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST: texture = _reflections.get(); break;
            case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST: texture = _denoisedReflections.get(); break;
            case nrd::ResourceType::PERMANENT_POOL: texture = _permanentTextures.at(resource.indexInPool).get(); break;
            case nrd::ResourceType::TRANSIENT_POOL: texture = _transientTextures.at(resource.indexInPool).get(); break;
            default: throw std::runtime_error("ERROR: Unsupported NRD reflections binding.");
            }
            const bool storage = resource.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            const auto state = storage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (texture->GetResource()->GetCurrentState() != state) { barriers.push_back(texture->GetResource()->GetTransitionBarrier(state)); }
            else if (storage) { barriers.push_back(texture->GetResource()->GetUAVBarrier()); }
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

void GDX12RTReflectionsPass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto* irradiance = context.GetInputTexture("InIrradiance");
    const auto* prefiltered = context.GetInputTexture("InPrefilteredEnvironment");
    const auto* brdfLut = context.GetInputTexture("InBRDFLut");
    const auto* directionalMaps = context.GetInputTexture("InDirectionalMaps");
    const auto* pointSpotMaps = context.GetInputTexture("InPointSpotMaps");
    const auto* occlusion = context.GetInputTexture("InAmbientOcclusion");
    const struct
    {
        float MaxDistance, MaxRoughness, RoughnessFade, Bias, Intensity;
        UINT SamplesPerPixel, FrameIndex, HasDirectionalShadows, HasPointSpotShadows, HasAmbientOcclusion, HasPrefiltered, HasBRDF, LightCount;
        XMFLOAT3 HitDistanceParameters;
        UINT CheckerboardTracing;
    } parameters = {_maxDistance, _maxRoughness, _roughnessFade, _bias, _intensity, _samplesPerPixel, _denoisingSettings.frameIndex,
        directionalMaps ? 1u : 0u, pointSpotMaps ? 1u : 0u, occlusion ? 1u : 0u, prefiltered ? 1u : 0u, brdfLut ? 1u : 0u,
        context.GetFrameData()->LightCount, {HitDistanceParameters.A, HitDistanceParameters.B, HitDistanceParameters.C},
        _checkerboardTracing ? 1u : 0u};

    commands->BeginPixEvent("Ray Traced Reflections", Colors::SteelBlue);
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    commands->SetComputeRootConstants(0, parameters);
    commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, context.GetInputTexture("InColorTexture")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, context.GetInputTexture("InDepthBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, context.GetInputTexture("InNormalRoughness")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(3, context.GetInputTexture("InF0Baked")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(4, resources->RayTracingScene->GetTLASSRV(context.GetFrameSlotResourcesIndex())->GPUHandle);
    commands->SetComputeSRV(5, resources->GeometryBuffer->GetVertexSRV()->GPUHandle);
    commands->SetComputeSRV(6, resources->GeometryBuffer->GetIndexSRV()->GPUHandle);
    commands->SetComputeSRV(7, resources->IndirectCommandsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(8, frame.InstanceCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(9, frame.TransformCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(10, frame.MaterialCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(11, frame.LightsCache->GetSRV()->GPUHandle);
    commands->SetComputeSRV(12, context.GetInputTexture("InSkyBoxMap")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(13, irradiance ? irradiance->GetSRV()->GPUHandle : resources->NullViews.TextureCubeSRV.GPUHandle);
    commands->SetComputeSRV(14, prefiltered ? prefiltered->GetSRV()->GPUHandle : resources->NullViews.TextureCubeSRV.GPUHandle);
    commands->SetComputeSRV(15, brdfLut ? brdfLut->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    commands->SetComputeSRV(16, directionalMaps ? directionalMaps->GetSRV()->GPUHandle : resources->NullViews.Texture2DArraySRV.GPUHandle);
    commands->SetComputeSRV(17, pointSpotMaps ? pointSpotMaps->GetSRV()->GPUHandle : resources->NullViews.Texture2DArraySRV.GPUHandle);
    commands->SetComputeSRV(18, frame.ShadowViews->GetSRV()->GPUHandle);
    commands->SetComputeSRV(19, occlusion ? occlusion->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    commands->SetComputeSRV(21, context.GetInputTexture("InVelocityBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(22, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetComputeUAV(0, _reflections->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, context.GetOutputTexture("OutColorTexture").GetUAV()->GPUHandle);
    commands->SetComputeUAV(2, _packedNormals->GetUAV()->GPUHandle);
    commands->SetComputeUAV(3, _viewDepth->GetUAV()->GPUHandle);
    commands->SetComputeUAV(4, _motionVectors->GetUAV()->GPUHandle);
    const auto size = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    commands->BeginPixEvent("Trace Reflections", Colors::SteelBlue);
    commands->SetRayTracingPipeline(*_reflectionPipeline);
    commands->DispatchRays(*_shaderTable, _reflections->GetWidth(), _reflections->GetHeight());
    commands->EndPixEvent();

    Denoise(context);
    commands->ResourceBarrier({_denoisedReflections->GetResource()->GetNonPixelShaderResourceBarrier()});
    commands->BeginPixEvent("Compose Reflections", Colors::LightSteelBlue);
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetComputeRootConstants(0, parameters);
    commands->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    commands->SetComputeSRV(0, context.GetInputTexture("InColorTexture")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(1, context.GetInputTexture("InDepthBuffer")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(2, context.GetInputTexture("InNormalRoughness")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(3, context.GetInputTexture("InF0Baked")->GetSRV()->GPUHandle);
    commands->SetComputeSRV(14, prefiltered ? prefiltered->GetSRV()->GPUHandle : resources->NullViews.TextureCubeSRV.GPUHandle);
    commands->SetComputeSRV(15, brdfLut ? brdfLut->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    commands->SetComputeSRV(19, occlusion ? occlusion->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    commands->SetComputeSRV(20, _denoisedReflections->GetSRV()->GPUHandle);
    commands->SetComputeUAV(1, context.GetOutputTexture("OutColorTexture").GetUAV()->GPUHandle);
    commands->SetPipelineState(_compositionPipeline);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    for (const auto& texture : _permanentTextures) { commands->ResourceBarrier({texture->GetResource()->GetUnorderedAccessBarrier()}); }
    for (const auto& texture : _transientTextures) { commands->ResourceBarrier({texture->GetResource()->GetUnorderedAccessBarrier()}); }
    commands->ResourceBarrier({_reflections->GetResource()->GetUnorderedAccessBarrier(), _packedNormals->GetResource()->GetUnorderedAccessBarrier(),
        _viewDepth->GetResource()->GetUnorderedAccessBarrier(), _motionVectors->GetResource()->GetUnorderedAccessBarrier(),
        _denoisedReflections->GetResource()->GetUnorderedAccessBarrier()});
    commands->EndPixEvent();
    commands->EndPixEvent();
}
