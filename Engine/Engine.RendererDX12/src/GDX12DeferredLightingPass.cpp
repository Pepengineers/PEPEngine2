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

GDX12DeferredLightingPass::GDX12DeferredLightingPass()
{ _flags = RENDER_PASS_FLAG_USE_LIGHTING | RENDER_PASS_FLAG_USE_CAMERAS; }

void GDX12DeferredLightingPass::InitializeEnvironmentMaps(GDX12DeviceResources& resources)
{
    auto* device = resources.Device;
    auto* heap = resources.SRV_UAV_Heap.get();
    auto initializeTexture = [&](GDX12Texture& texture, UINT size, UINT mips, bool cube)
    {
        GDX12TextureDesc desc;
        desc.Device = device;
        desc.Width = desc.Height = size;
        desc.ArraySize = cube ? 6 : 1;
        desc.IsCube = cube;
        desc.MipLevels = mips;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SRV_UAV_Heap = heap;
        desc.SRVHeapIndex = heap->GetAvailableIndex(TextureResources_StartIndex);
        desc.CreateUAV = true;
        desc.UAVHeapIndex = heap->GetAvailableIndex(TextureResources_StartIndex);
        texture.Initialize(desc);
    };
    initializeTexture(_irradianceMap, 32, 1, true);
    initializeTexture(_prefilteredEnvironmentMap, 128, SpecularMipCount, true);
    initializeTexture(_brdfLut, 128, 1, false);

    for (UINT mip = 1; mip < SpecularMipCount; ++mip)
    {
        auto uav = _prefilteredEnvironmentMap.GetDescription().UAVDesc;
        uav.Texture2DArray.MipSlice = mip;
        _mipViews[mip - 1].InitAsUAV(_prefilteredEnvironmentMap.GetResource()->D3DResource.Get(), nullptr, &uav,
            heap, heap->GetAvailableIndex(TextureResources_StartIndex));
    }

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

void GDX12DeferredLightingPass::FilterEnvironmentMap(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    cmd->ResourceBarrier({_irradianceMap.GetResource()->GetUnorderedAccessBarrier(),
        _prefilteredEnvironmentMap.GetResource()->GetUnorderedAccessBarrier(), _brdfLut.GetResource()->GetUnorderedAccessBarrier()});
    cmd->SetComputeRootSignature(_filterRoot.get());
    cmd->SetDescriptorHeaps({context.GetDeviceResources()->SRV_UAV_Heap.get()});
    cmd->SetComputeSRV(0, context.GetInputTexture("InEnvironmentMap").GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, _irradianceMap.GetUAV()->GPUHandle);
    cmd->SetComputeUAV(1, _brdfLut.GetUAV()->GPUHandle);
    auto dispatch = [&](UINT size, float roughness, UINT faces)
    {
        const struct { UINT Size; float Roughness; } parameters = {size, roughness};
        cmd->GetCommandList()->SetComputeRoot32BitConstants(0, 2, &parameters, 0);
        cmd->Dispatch((size + 7) / 8, (size + 7) / 8, faces);
    };
    cmd->SetPipelineState(_irradiancePipeline);
    dispatch(_irradianceMap.GetWidth(), 0, 6);
    cmd->SetPipelineState(_prefilterPipeline);
    for (UINT mip = 0; mip < SpecularMipCount; ++mip)
    {
        const auto* view = mip == 0 ? _prefilteredEnvironmentMap.GetUAV() : &_mipViews[mip - 1];
        cmd->SetComputeUAV(0, view->GPUHandle);
        dispatch(_prefilteredEnvironmentMap.GetWidth() >> mip, float(mip) / (SpecularMipCount - 1), 6);
    }
    cmd->SetPipelineState(_brdfPipeline);
    dispatch(_brdfLut.GetWidth(), 0, 1);
    cmd->ResourceBarrier({_irradianceMap.GetResource()->GetNonPixelShaderResourceBarrier(),
        _prefilteredEnvironmentMap.GetResource()->GetNonPixelShaderResourceBarrier(), _brdfLut.GetResource()->GetNonPixelShaderResourceBarrier()});
}

void GDX12DeferredLightingPass::Describe(PassDescription& s) const
{
    for (const auto& input : Inputs)
    {
        auto& pin = s.AddInputTexture(input.Name);
        pin.Formats = {input.Format};
        pin.Usages = ResourceUsage::NonPixelShaderRead;
        pin.AllowShared = !IsDepthFormat(input.Format);
    }
    s.AddInputTexture("InEnvironmentMap", ResourceType::TextureCube).Usages = ResourceUsage::NonPixelShaderRead;
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
    InitializeEnvironmentMaps(*resources);

    GDX12RootSignatureDesc desc;
    desc.Constants = {sizeof(GDX12LightGridConstants) / sizeof(UINT)};
    desc.NumSingleCBVSlots = 1;
    desc.NumSingleSRVSlots = 10;
    desc.NumSingleUAVSlots = 1;
    desc.StaticSamplers = {LinearClamp()};
    _root = std::make_unique<GDX12RootSignature>(resources->Device, desc);
    _pipeline = ComputePipeline(resources->Device, *_root, SHADERS_FOLDER "DeferredLightingPass.hlsl", "CS");
}

void GDX12DeferredLightingPass::Execute(PassExecutionContext& context)
{
    auto* cmd = context.GetCommandList();
    auto* resources = context.GetDeviceResources();
    auto& frame = *resources->FrameConstants[context.GetFrameSlotResourcesIndex()];
    const auto& color = context.GetOutputTexture("OutColorTexture");
    cmd->BeginPixEvent("Deferred Lighting", Colors::Gold);
    if (!_environmentMapsReady)
    {
        cmd->BeginPixEvent("Filter Environment Map", Colors::Gold);
        FilterEnvironmentMap(context);
        cmd->EndPixEvent();
        _environmentMapsReady = true;
    }
    cmd->SetComputeRootSignature(_root.get());
    cmd->SetPipelineState(_pipeline);
    cmd->SetDescriptorHeaps({resources->SRV_UAV_Heap.get()});
    cmd->SetComputeRootConstantBufferView(1, frame.CameraCB->GetElementAddress(context.GetFrameData()->ActiveCameraCBufferIndex));
    cmd->GetCommandList()->SetComputeRoot32BitConstants(0, sizeof(frame.ClusteredLightingConstants) / sizeof(UINT), &frame.ClusteredLightingConstants, 0);
    for (UINT i = 0; i < std::size(Inputs); ++i)
    { cmd->SetComputeSRV(i, context.GetInputTexture(Inputs[i].Name).GetSRV()->GPUHandle); }
    cmd->SetComputeSRV(5, _irradianceMap.GetSRV()->GPUHandle);
    cmd->SetComputeSRV(6, _prefilteredEnvironmentMap.GetSRV()->GPUHandle);
    cmd->SetComputeSRV(7, _brdfLut.GetSRV()->GPUHandle);
    cmd->SetComputeSRV(8, frame.LightsCache->GetSRV()->GPUHandle);
    cmd->SetComputeSRV(9, frame.ClusterLightMaskBuffer->GetSRV()->GPUHandle);
    cmd->SetComputeUAV(0, color.GetUAV()->GPUHandle);
    const auto size = color.GetDescription().GetSize();
    cmd->Dispatch((size.x + 7) / 8, (size.y + 7) / 8, 1);
    cmd->EndPixEvent();
}
