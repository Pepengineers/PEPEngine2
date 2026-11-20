#include "Engine.RendererDX12/RenderPasses/GDX12DeferredLightingPass.h"

namespace
{
    struct Input { const char* Name; DXGI_FORMAT Format; };
    constexpr Input Inputs[] = {
        {"InAlbedoMetallic", DXGI_FORMAT_R8G8B8A8_UNORM},
        {"InNormalRoughness", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"InF0Baked", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"InEmissive", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {"InDepthBuffer", DXGI_FORMAT_D32_FLOAT}
    };

}

GDX12DeferredLightingPass::GDX12DeferredLightingPass()
{ _flags = RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12DeferredLightingPass::Describe(PassDescription& s) const
{
    for (const auto& input : Inputs)
    {
        auto& pin = s.AddInputTexture(input.Name);
        pin.Formats = {input.Format};
        pin.Usages = ResourceUsage::NonPixelShaderRead;
        pin.AllowShared = !IsDepthFormat(input.Format);
    }
    s.AddInputTexture("InIrradiance", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;
    s.AddInputTexture("InPrefilteredEnvironment", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;
    s.AddInputTexture("InBRDFLut", ResourceType::Texture2D).Usages = ResourceUsage::NonPixelShaderRead;
    s.RequireSameTextureSize({"InAlbedoMetallic", "InNormalRoughness", "InF0Baked", "InEmissive", "InDepthBuffer"});
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
    desc.Constants = {sizeof(GDX12LightGridConstants) / sizeof(UINT)};
    desc.NumSingleCBVSlots = 1;
    desc.NumSingleSRVSlots = 10;
    desc.NumSingleUAVSlots = 1;
    desc.StaticSamplers = GetStaticSamplers();
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
    cmd->BeginPixEvent("Deferred Lighting", Colors::Gold);
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetPipelineState(_pipeline);
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, sizeof(frame.ClusteredLightingConstants) / sizeof(UINT), &frame.ClusteredLightingConstants, 0);
    for (UINT i = 0; i < std::size(Inputs); ++i)
    { cmd->SetComputeSRV(i, context.GetInputTexture(Inputs[i].Name).GetSRV()->GPUHandle); }
    cmd->SetComputeSRV(5, context.GetInputTexture("InIrradiance").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(6, context.GetInputTexture("InPrefilteredEnvironment").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(7, context.GetInputTexture("InBRDFLut").GetSRV()->GPUHandle);
    cmd->SetComputeSRV(8, frame.LightsCache->GetSRV()->GPUHandle);
    cmd->SetComputeSRV(9, frame.ClusterLightMaskBuffer->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, color.GetUAV()->GPUHandle);
    const auto size = color.GetDescription().GetSize();
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->EndPixEvent();
}
