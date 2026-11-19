#include "Engine.RendererDX12/FrameGraph/ComputeCopyResources.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

#include <stdexcept>

ComputeCopyResources::TexelType ComputeCopyResources::GetTexelType(DXGI_FORMAT format)
{
    switch (DirectX::FormatDataType(format))
    {
    case DirectX::FORMAT_TYPE_UINT: return Uint4;
    case DirectX::FORMAT_TYPE_SINT: return Int4;
    default: return Float4;
    }
}

bool ComputeCopyResources::SupportsFormat(GDX12Device* device, DXGI_FORMAT format)
{
    const auto& specs = device->GetDeviceFeatures();
    if (!specs.Features.CrossAdapterRowMajorTextureSupported() ||
        DirectX::IsTypeless(format) || IsDepthFormat(format)) { return false; }
    UINT8 planeCount = 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
    const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD
        | D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    return SUCCEEDED(specs.GetFormatSupport(support))
        && (support.Support1 & required) == required && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE)
        && SUCCEEDED(specs.Features.FormatInfo(format, planeCount)) && planeCount == 1;
}

void ComputeCopyResources::Prepare(GDX12Device* device, DXGI_FORMAT format)
{
    const auto type = GetTexelType(format);
    auto& pipeline = _pipelines[type];
    if (pipeline) { return; }
    if (!_root)
    {
        GDX12RootSignatureDesc desc;
        desc.NumSingleSRVSlots = 1;
        desc.NumSingleUAVSlots = 1;
        _root = std::make_unique<GDX12RootSignature>(device, desc);
    }
    static constexpr std::array TexelNames{"float4", "uint4", "int4"};
    static_assert(TexelNames.size() == TexelType::Count, "Provide an HLSL type name for every copy shader variant.");
    const D3D_SHADER_MACRO defines[] = {{"TEXEL", TexelNames[type]}, {nullptr, nullptr}};
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device, SHADERS_FOLDER "ComputeCopy.hlsl", defines, "CopyTexture", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = _root->GetRootSignature().Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
}

FrameGraphCopyStatistics ComputeCopyResources::Dispatch(GDX12CommandList* commands, FrameGraphPhysicalResource& source,
    FrameGraphPhysicalResource& destination) const
{
    const auto& pipeline = _pipelines[GetTexelType(source.Description.Format)];
    if (!pipeline) { throw std::logic_error("ComputeCopy requires Prepare for the source format before Dispatch."); }
    commands->SetComputeRootSignature(_root.get());
    commands->SetPipelineState(pipeline);
    commands->SetDescriptorHeaps({source.DeviceResources->SRV_UAV_Heap.get()});
    auto* sourceTexture = source.ImportedTexture ? source.ImportedTexture : source.Texture.get();
    commands->SetComputeSRV(0, sourceTexture->GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, destination.Texture->GetUAV()->GPUHandle);
    commands->Dispatch((source.Description.Width + 15) / 16, (source.Description.Height + 7) / 8, 1);
    return {source.TransferBytes ? source.TransferBytes : destination.TransferBytes, 1};
}
