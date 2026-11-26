#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"

GDX12DeferredLightingPass::GDX12DeferredLightingPass()
{ _flags = RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12DeferredLightingPass::Describe(PassDescription& s) const
{
    auto& albedoMetallic = s.AddInputTexture("InAlbedoMetallic");
    albedoMetallic.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
    albedoMetallic.Usages = ResourceUsage::NonPixelShaderRead;
    albedoMetallic.AllowShared = true;

    auto& normalRoughness = s.AddInputTexture("InNormalRoughness");
    normalRoughness.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    normalRoughness.Usages = ResourceUsage::NonPixelShaderRead;
    normalRoughness.AllowShared = true;

    auto& f0Baked = s.AddInputTexture("InF0Baked");
    f0Baked.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    f0Baked.Usages = ResourceUsage::NonPixelShaderRead;
    f0Baked.AllowShared = true;

    auto& emissive = s.AddInputTexture("InEmissive");
    emissive.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    emissive.Usages = ResourceUsage::NonPixelShaderRead;
    emissive.AllowShared = true;

    auto& depth = s.AddInputTexture("InDepthBuffer");
    depth.Formats = {DXGI_FORMAT_D32_FLOAT};
    depth.Usages = ResourceUsage::NonPixelShaderRead;
    depth.AllowShared = false;

    s.AddInputTexture("InIrradiance", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;
    s.AddInputTexture("InPrefilteredEnvironment", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;
    s.AddInputTexture("InBRDFLut", ResourceType::Texture2D).Usages = ResourceUsage::NonPixelShaderRead;

    auto& directionalMaps = s.AddInputTexture("InDirectionalMaps", ResourceType::Texture2DArray);
    directionalMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    directionalMaps.Usages = ResourceUsage::NonPixelShaderRead;
    directionalMaps.IsOptionalInput = true;

    auto& pointSpotMaps = s.AddInputTexture("InPointSpotMaps", ResourceType::Texture2DArray);
    pointSpotMaps.Formats = {DXGI_FORMAT_D32_FLOAT};
    pointSpotMaps.Usages = ResourceUsage::NonPixelShaderRead;
    pointSpotMaps.IsOptionalInput = true;

    auto& occlusion = s.AddInputTexture("InAmbientOcclusion");
    occlusion.Formats = {DXGI_FORMAT_R32_FLOAT};
    occlusion.Usages = ResourceUsage::NonPixelShaderRead;
    occlusion.AllowShared = true;
    occlusion.IsOptionalInput = true;

    s.RequireSameTextureSize({"InAlbedoMetallic", "InNormalRoughness", "InF0Baked", "InEmissive", "InDepthBuffer", "InAmbientOcclusion"});
    auto& output = s.AddOutputTexture("OutColorTexture");
    output.Formats = {DXGI_FORMAT_R8G8B8A8_UNORM};
    output.Size = Extent::SameAs("InAlbedoMetallic");
    output.Usages = ResourceUsage::UnorderedAccess;
    output.AllowShared = true;
}

void GDX12DeferredLightingPass::Initialize(PassInitContext& context)
{
    auto* resources = context.GetDeviceResources();

    GDX12RootSignatureDesc desc;
    desc.Constants = {sizeof(GDX12LightGridConstants) / sizeof(UINT), 3};
    desc.NumSingleCBVSlots = 1;
    desc.NumSingleSRVSlots = 14;
    desc.NumSingleUAVSlots = 1;
    desc.StaticSamplers = GetStaticSamplers();
    CD3DX12_STATIC_SAMPLER_DESC shadowSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT);
    shadowSampler.AddressU = shadowSampler.AddressV = shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    shadowSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    shadowSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    desc.StaticSamplers.push_back(shadowSampler);
    _root = std::make_unique<GDX12RootSignature>(resources->Device, desc);
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(resources->Device, SHADERS_FOLDER "DeferredLightingPass.hlsl", nullptr, "CS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = _root->GetRootSignature().Get();
    pso.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(resources->Device->GetDevice()->CreateComputePipelineState(&pso, IID_PPV_ARGS(&_pipeline)));
}

void GDX12DeferredLightingPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto& color = context.GetOutputTexture("OutColorTexture");
    const auto* directionalMaps = context.TryGetInputTexture("InDirectionalMaps");
    const auto* pointSpotMaps = context.TryGetInputTexture("InPointSpotMaps");
    const auto* occlusion = context.TryGetInputTexture("InAmbientOcclusion");
    const struct { UINT HasDirectionalShadows, HasPointSpotShadows, HasAmbientOcclusion; } optionalInputs = {
        directionalMaps ? 1u : 0u, pointSpotMaps ? 1u : 0u, occlusion ? 1u : 0u};
    cmd->BeginPixEvent("Deferred Lighting", Colors::Gold);
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetPipelineState(_pipeline);
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->SetComputeRootConstantBufferView(2, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, sizeof(frame.ClusteredLightingConstants) / sizeof(UINT), &frame.ClusteredLightingConstants, 0);
    cmd->GetCommandList()->SetComputeRoot32BitConstants(1, 3, &optionalInputs, 0);
    cmd->SetComputeSRV(0, context.GetInputTexture("InAlbedoMetallic").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(1, context.GetInputTexture("InNormalRoughness").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(2, context.GetInputTexture("InF0Baked").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(3, context.GetInputTexture("InEmissive").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(4, context.GetInputTexture("InDepthBuffer").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(5, context.GetInputTexture("InIrradiance").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(6, context.GetInputTexture("InPrefilteredEnvironment").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(7, context.GetInputTexture("InBRDFLut").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(8, frame.LightsCache->GetSRV()->GPUHandle);
    cmd->SetComputeSRV(9, frame.ClusterLightMaskBuffer->GetSRV()->GPUHandle);
    cmd->SetComputeSRV(10, directionalMaps ? directionalMaps->GetSRV()->GPUHandle : resources->NullViews.Texture2DArraySRV.GPUHandle);
    cmd->SetComputeSRV(11, pointSpotMaps ? pointSpotMaps->GetSRV()->GPUHandle : resources->NullViews.Texture2DArraySRV.GPUHandle);
    cmd->SetComputeSRV(12, frame.ShadowViews->GetSRV()->GPUHandle);
    cmd->SetComputeSRV(13, occlusion ? occlusion->GetSRV()->GPUHandle : resources->NullViews.Texture2DSRV.GPUHandle);
    cmd->SetComputeUAV(0, color.GetUAV()->GPUHandle);
    const auto size = color.GetDescription().GetSize();
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->EndPixEvent();
}
