#include "Engine.RendererDX12/RenderPasses/GDX12IBLPrecomputePass.h"

namespace
{
    ComPtr<ID3D12PipelineState> ComputePipeline(GDX12Device* device, GDX12RootSignature& root,
        const wchar_t* file, const char* entry)
    {
        auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device, file, nullptr, entry, "cs");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = root.GetRootSignature().Get();
        desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
        ComPtr<ID3D12PipelineState> pipeline;
        ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
        return pipeline;
    }

    auto LinearClamp()
    {
        return CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    }
}

void GDX12IBLPrecomputePass::Describe(PassDescription& description) const
{
    description.RunOnceDuringSetup = true;
    description.AddInputTexture("InSkyBoxMap", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;

    auto& irradiance = description.AddOutputTexture("OutIrradiance", ResourceType::TextureCube);
    irradiance.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    irradiance.Size = Extent::Fixed({32, 32});
    irradiance.MipLevels = 1;
    irradiance.Usages = ResourceUsage::UnorderedAccess;

    auto& prefilteredEnvironment = description.AddOutputTexture("OutPrefilteredEnvironment", ResourceType::TextureCube);
    prefilteredEnvironment.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    prefilteredEnvironment.Size = Extent::Fixed({128, 128});
    prefilteredEnvironment.MipLevels = SpecularMipCount;
    prefilteredEnvironment.Usages = ResourceUsage::UnorderedAccess;

    auto& brdfLut = description.AddOutputTexture("OutBRDFLut", ResourceType::Texture2D);
    brdfLut.Formats = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    brdfLut.Size = Extent::Fixed({128, 128});
    brdfLut.MipLevels = 1;
    brdfLut.Usages = ResourceUsage::UnorderedAccess;
}

void GDX12IBLPrecomputePass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    GDX12RootSignatureDesc desc;
    desc.Constants = {2};
    desc.NumSingleSRVSlots = 1;
    desc.NumSingleUAVSlots = 2;
    desc.StaticSamplers = {LinearClamp()};
    _filterRoot = std::make_unique<GDX12RootSignature>(device, desc);
    const auto file = SHADERS_FOLDER "IBLPrecomputePass.hlsl";
    _irradiancePipeline = ComputePipeline(device, *_filterRoot, file, "IrradianceCS");
    _prefilterPipeline = ComputePipeline(device, *_filterRoot, file, "PrefilterCS");
    _brdfPipeline = ComputePipeline(device, *_filterRoot, file, "IntegrateBRDFCS");
}

void GDX12IBLPrecomputePass::Execute(PassExecutionContext& context)
{
    const auto& irradiance = context.GetOutputTexture("OutIrradiance");
    const auto& prefiltered = context.GetOutputTexture("OutPrefilteredEnvironment");
    const auto& brdf = context.GetOutputTexture("OutBRDFLut");
    auto* cmd = context.GetCommandList();
    cmd->BeginPixEvent("Filter Environment Map", Colors::Gold);
    cmd->SetComputeRootSignature(_filterRoot.get());
    cmd->SetDescriptorHeaps({context.GetDeviceResources()->SRV_UAV_Heap.get()});
    cmd->SetComputeSRV(0, context.GetInputTexture("InSkyBoxMap").GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, irradiance.GetUAV()->GPUHandle);
    cmd->SetComputeUAV(1, brdf.GetUAV()->GPUHandle);
    auto dispatch = [&](UINT size, float roughness, UINT faces)
    {
        const struct { UINT Size; float Roughness; } parameters = {size, roughness};
        cmd->GetCommandList()->SetComputeRoot32BitConstants(0, 2, &parameters, 0);
        cmd->Dispatch((size + 7) / 8, (size + 7) / 8, faces);
    };
    cmd->SetPipelineState(_irradiancePipeline);
    dispatch(irradiance.GetDescription().Width, 0, 6);
    cmd->SetPipelineState(_prefilterPipeline);
    auto* heap = context.GetDeviceResources()->SRV_UAV_Heap.get();
    for (UINT mip = 0; mip < SpecularMipCount; ++mip)
    {
        auto* view = prefiltered.GetUAV();
        if (mip > 0)
        {
            view = &_mipViews[mip - 1];
            auto* texture = prefiltered.GetTexture();
            auto desc = texture->GetDescription().UAVDesc;
            desc.Texture2DArray.MipSlice = mip;
            const UINT index = view->HeapIndex == UINT_MAX ? heap->GetAvailableIndex(RendererResources_StartIndex) : view->HeapIndex;
            view->InitAsUAV(texture->GetResource()->D3DResource.Get(), nullptr, &desc, heap, index);
        }
        cmd->SetComputeUAV(0, view->GPUHandle);
        dispatch(prefiltered.GetDescription().Width >> mip, float(mip) / (SpecularMipCount - 1), 6);
    }
    cmd->SetPipelineState(_brdfPipeline);
    dispatch(brdf.GetDescription().Width, 0, 1);
    cmd->EndPixEvent();
}
