#include "Engine.RendererDX12/RenderPasses/GDX12RTReflectionsPass.h"
#include <cmath>

GDX12RTReflectionsPass::GDX12RTReflectionsPass()
{ _flags = RENDER_PASS_FLAG_USE_RAYTRACING | RENDER_PASS_FLAG_USE_CAMERAS | RENDER_PASS_FLAG_USE_LIGHTING; }

void GDX12RTReflectionsPass::Describe(PassDescription& description) const
{
    description.AddParameter("SamplesPerPixel", 1);
    description.AddParameter("MaxDistance", 10000.0f);
    description.AddParameter("MaxRoughness", 0.6f);
    description.AddParameter("RoughnessFade", 0.1f);
    description.AddParameter("Bias", 0.5f);
    description.AddParameter("Intensity", 1.0f);

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

    description.RequireSameTextureSize({"InColorTexture", "InDepthBuffer", "InNormalRoughness", "InF0Baked", "InAmbientOcclusion"});
    auto& output = description.AddOutputTexture("OutColorTexture");
    output.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    output.Size = Extent::SameAs("InColorTexture");
    output.Usages = ResourceUsage::UnorderedAccess;
    output.AllowShared = true;
}

void GDX12RTReflectionsPass::Initialize(PassInitContext& context)
{
    const auto samplesPerPixel = context.GetParameter<int>("SamplesPerPixel");
    _maxDistance = context.GetParameter<float>("MaxDistance");
    _maxRoughness = context.GetParameter<float>("MaxRoughness");
    _roughnessFade = context.GetParameter<float>("RoughnessFade");
    _bias = context.GetParameter<float>("Bias");
    _intensity = context.GetParameter<float>("Intensity");
    if (samplesPerPixel < 1 || samplesPerPixel > 64 || !std::isfinite(_maxDistance) || _maxDistance <= 0
        || !std::isfinite(_maxRoughness) || _maxRoughness <= 0 || _maxRoughness > 1
        || !std::isfinite(_roughnessFade) || _roughnessFade < 0 || _roughnessFade > 1
        || !std::isfinite(_bias) || _bias < 0 || !std::isfinite(_intensity) || _intensity < 0)
    { throw std::invalid_argument("ERROR: RT reflections require SamplesPerPixel in [1,64], positive MaxDistance, MaxRoughness in (0,1], RoughnessFade in [0,1] and nonnegative Bias/Intensity."); }
    _samplesPerPixel = static_cast<UINT>(samplesPerPixel);

    auto* resources = context.GetDeviceResources();
    GDX12RootSignatureDesc root;
    root.Constants = {13};
    root.NumSingleCBVSlots = 1;
    root.NumSingleSRVSlots = 21;
    root.NumSingleUAVSlots = 2;
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
    pipeline.Exports = {L"RayGeneration", L"ClosestHit", L"AnyHit", L"Miss"};
    pipeline.GlobalRootSignature = _rootSignature.get();
    pipeline.MaxPayloadSize = 5 * sizeof(float);
    D3D12_HIT_GROUP_DESC hitGroup{};
    hitGroup.HitGroupExport = L"ReflectionHitGroup";
    hitGroup.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    hitGroup.ClosestHitShaderImport = L"ClosestHit";
    hitGroup.AnyHitShaderImport = L"AnyHit";
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
    CreateReflectionTexture(context);
}

void GDX12RTReflectionsPass::CreateReflectionTexture(PassInitContext& context)
{
    const auto size = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    if (_reflections) { _reflections->Resize(size.x, size.y); return; }
    auto* resources = context.GetDeviceResources();
    GDX12TextureDesc texture;
    texture.Device = resources->Device;
    texture.Width = size.x;
    texture.Height = size.y;
    texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texture.CreateUAV = true;
    texture.SRV_UAV_Heap = resources->SRV_UAV_Heap.get();
    texture.SRVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
    texture.UAVHeapIndex = resources->SRV_UAV_Heap->GetAvailableIndex(RendererResources_StartIndex);
    _reflections = std::make_unique<GDX12Texture>();
    _reflections->Initialize(texture);
}

void GDX12RTReflectionsPass::OnResourcesRecreated(PassInitContext& context)
{ CreateReflectionTexture(context); }

std::vector<ExternalResourceUse> GDX12RTReflectionsPass::GetExternalResources(PassExecutionContext& context)
{
    auto uses = GDX12RenderPass::GetExternalResources(context);
    uses.push_back({_reflections->GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, {}});
    return uses;
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
    } parameters = {_maxDistance, _maxRoughness, _roughnessFade, _bias, _intensity, _samplesPerPixel, _frameIndex++,
        directionalMaps ? 1u : 0u, pointSpotMaps ? 1u : 0u, occlusion ? 1u : 0u, prefiltered ? 1u : 0u, brdfLut ? 1u : 0u,
        context.GetFrameData()->LightCount};

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
    commands->SetComputeSRV(21, resources->SRV_UAV_Heap->GetGPUHandle(Texture2D_StartIndex));
    commands->SetComputeUAV(0, _reflections->GetUAV()->GPUHandle);
    commands->SetComputeUAV(1, context.GetOutputTexture("OutColorTexture").GetUAV()->GPUHandle);
    const auto size = context.GetOutputResourceDescription("OutColorTexture").GetSize();
    commands->BeginPixEvent("Trace Reflections", Colors::SteelBlue);
    commands->SetRayTracingPipeline(*_reflectionPipeline);
    commands->DispatchRays(*_shaderTable, size.x, size.y);
    commands->EndPixEvent();

    commands->ResourceBarrier({_reflections->GetResource()->GetNonPixelShaderResourceBarrier()});
    commands->BeginPixEvent("Compose Reflections", Colors::LightSteelBlue);
    commands->SetComputeSRV(20, _reflections->GetSRV()->GPUHandle);
    commands->SetPipelineState(_compositionPipeline);
    commands->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    commands->ResourceBarrier({_reflections->GetResource()->GetUnorderedAccessBarrier()});
    commands->EndPixEvent();
    commands->EndPixEvent();
}
