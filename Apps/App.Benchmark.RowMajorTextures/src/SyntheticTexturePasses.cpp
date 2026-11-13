#include "../SyntheticTexturePasses.h"

#include <array>
#include <stdexcept>

namespace
{
    constexpr UINT PixelsPerGroupSide = 32;
    constexpr UINT ValidationGroupCount = 3;

    UINT ValidateExtent(UINT extent)
    {
        if (!extent || extent > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || extent % PixelsPerGroupSide)
        {
            throw std::invalid_argument("Synthetic texture extent must be a nonzero multiple of 32 within the D3D12 texture limit.");
        }
        return extent;
    }

    // Matches PixelValue in SyntheticTextures.hlsl.
    UINT PixelValue(UINT index, UINT seed)
    {
        UINT value = (index ^ seed) * 1664525u + 1013904223u;
        value ^= value >> 16;
        value *= 2246822519u;
        return value ^ (value >> 13);
    }
}

SyntheticTextureWritePass::SyntheticTextureWritePass(UINT extent)
    : _extent(ValidateExtent(extent))
{
}

void SyntheticTextureWritePass::Describe(PassDescription& description) const
{
    auto& texture = description.AddOutputTexture("Data");
    texture.Formats = {SyntheticTextureFormat};
    texture.Size = Extent::Fixed({_extent, _extent});
    texture.AllowShared = true;
    texture.Usages = ResourceUsage::RenderTarget;
}

void SyntheticTextureWritePass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    auto& compiler = GDX12ShaderCompiler::GetInstance();
    const auto vertex = compiler.CompileShader(device, SHADERS_FOLDER "FullScreenVS.hlsl", nullptr, "VS", "vs");
    const D3D_SHADER_MACRO writeDefines[] = {{"WRITE_TEXTURE", "1"}, {nullptr, nullptr}};
    const auto pixel = compiler.CompileShader(device, std::wstring(PROJECT_DIR) + L"SyntheticTextures.hlsl",
        writeDefines, "Produce", "ps");

    GDX12RootSignatureDesc rootDescription;
    rootDescription.Constants = {2};
    _rootSignature = std::make_unique<GDX12RootSignature>(device, rootDescription);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipelineDescription.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    pipelineDescription.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    pipelineDescription.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pipelineDescription.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pipelineDescription.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pipelineDescription.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pipelineDescription.DepthStencilState.DepthEnable = FALSE;
    pipelineDescription.SampleMask = UINT_MAX;
    pipelineDescription.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipelineDescription.NumRenderTargets = 1;
    pipelineDescription.RTVFormats[0] = context.GetOutputResourceDescription("Data").Format;
    pipelineDescription.SampleDesc.Count = 1;
    ThrowIfFailed(device->GetDevice()->CreateGraphicsPipelineState(&pipelineDescription, IID_PPV_ARGS(&_pipeline)));
}

void SyntheticTextureWritePass::Execute(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    auto* target = context.GetOutputTexture("Data").GetTexture();
    commands->BeginPixEvent("Synthetic texture write", Colors::Gold);
    const auto slot = context.GetFrameSlotResourcesIndex();
    if (slot >= _initializedSlots.size()) { _initializedSlots.resize(slot + 1); }
    if (!_initializedSlots[slot])
    {
        // Shared render targets are placed resources and need initialization before the first draw.
        commands->GetCommandList()->DiscardResource(target->GetResource()->D3DResource.Get(), nullptr);
        _initializedSlots[slot] = true;
    }
    commands->SetGraphicsRootSignature(_rootSignature.get());
    commands->SetPipelineState(_pipeline);
    commands->SetViewport(target->GetViewport());
    commands->SetScissorRect(target->GetScissorRect());
    commands->SetRenderTargets({target}, nullptr);
    const UINT constants[] = {_extent, static_cast<UINT>(context.GetFrameId())};
    commands->GetCommandList()->SetGraphicsRoot32BitConstants(0, 2, constants, 0);
    commands->SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    commands->EndPixEvent();
}

SyntheticTextureReadPass::SyntheticTextureReadPass(UINT extent, bool* verified)
    : _extent(ValidateExtent(extent)), _groupsPerRow(_extent / PixelsPerGroupSide), _verified(verified)
{
}

void SyntheticTextureReadPass::Describe(PassDescription& description) const
{
    auto& texture = description.AddInputTexture("Data");
    texture.Formats = {SyntheticTextureFormat};
    texture.Size = Extent::Fixed({_extent, _extent});
    texture.AllowShared = true;
    texture.Usages = ResourceUsage::NonPixelShaderRead;

    auto& checksum = description.AddOutputBuffer("Checksum");
    checksum.Stride = sizeof(UINT);
    checksum.Capacity = _groupsPerRow * _groupsPerRow;
    checksum.Layout = "uint";
    checksum.Usages = ResourceUsage::UnorderedAccess;
}

void SyntheticTextureReadPass::Initialize(PassInitContext& context)
{
    auto* device = context.GetDevice();
    const auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(
        device, std::wstring(PROJECT_DIR) + L"SyntheticTextures.hlsl", nullptr, "Consume", "cs");
    GDX12RootSignatureDesc rootDescription;
    rootDescription.Constants = {1};
    rootDescription.NumSingleSRVSlots = 1;
    rootDescription.NumSingleUAVSlots = 1;
    rootDescription.StaticSamplers.emplace_back(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    _rootSignature = std::make_unique<GDX12RootSignature>(device, rootDescription);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipelineDescription.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&_pipeline)));
    if (!_verified) { return; }

    *_verified = false;
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(ValidationGroupCount * sizeof(UINT));
    ThrowIfFailed(context.GetDevice()->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
        &description, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readback)));
}

void SyntheticTextureReadPass::Execute(PassExecutionContext& context)
{
    const auto& checksum = context.GetOutputBuffer("Checksum");
    auto* commands = context.GetCommandList();
    commands->BeginPixEvent("Synthetic texture read", Colors::CornflowerBlue);
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetPipelineState(_pipeline);
    commands->SetDescriptorHeaps({context.GetDeviceResources()->SRV_UAV_Heap.get()});
    commands->GetCommandList()->SetComputeRoot32BitConstant(0, _extent, 0);
    commands->SetComputeSRV(0, context.GetInputTexture("Data").GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, checksum.GetUAV()->GPUHandle);
    commands->Dispatch(_groupsPerRow, _groupsPerRow, 1);
    commands->EndPixEvent();

    if (_readback) { ValidateChecksum(context, *commands, *checksum.Resource); }
}

UINT SyntheticTextureReadPass::ExpectedChecksum(UINT group) const
{
    const UINT startX = (group % _groupsPerRow) * PixelsPerGroupSide;
    const UINT startY = (group / _groupsPerRow) * PixelsPerGroupSide;
    UINT sum = 0;
    for (UINT y = startY; y < startY + PixelsPerGroupSide; ++y)
    {
        for (UINT x = startX; x < startX + PixelsPerGroupSide; ++x)
        {
            sum += PixelValue(y * _extent + x, _validationSeed);
        }
    }
    return sum;
}

void SyntheticTextureReadPass::ValidateChecksum(PassExecutionContext& context, GDX12CommandList& commands, GDX12Resource& checksum)
{
    const UINT groupCount = _groupsPerRow * _groupsPerRow;
    const UINT middle = _groupsPerRow / 2;
    const std::array<UINT, ValidationGroupCount> groups{0, middle * _groupsPerRow + middle, groupCount - 1};
    if (!_validationRecorded)
    {
        _validationSeed = static_cast<UINT>(context.GetFrameId());
        commands.ResourceBarrier({checksum.GetCopySourceBarrier()});
        for (size_t i = 0; i < groups.size(); ++i)
        {
            commands.GetCommandList()->CopyBufferRegion(_readback.Get(), i * sizeof(UINT),
                checksum.D3DResource.Get(), groups[i] * sizeof(UINT), sizeof(UINT));
        }
        _validationRecorded = true;
        return;
    }

    // The previous frame submitted the readback copy on this queue.
    context.GetDevice()->GetCommandQueue()->Flush();
    const D3D12_RANGE readRange{0, groups.size() * sizeof(UINT)};
    void* mapped = nullptr;
    ThrowIfFailed(_readback->Map(0, &readRange, &mapped));
    const auto* values = static_cast<const UINT*>(mapped);
    const std::array<UINT, ValidationGroupCount> actual{values[0], values[1], values[2]};
    const D3D12_RANGE noWrites{0, 0};
    _readback->Unmap(0, &noWrites);

    for (size_t i = 0; i < groups.size(); ++i)
    {
        const UINT expected = ExpectedChecksum(groups[i]);
        if (actual[i] != expected)
        {
            throw std::runtime_error("Synthetic texture checksum mismatch at group " + std::to_string(groups[i])
                + ", expected " + std::to_string(expected) + ", got " + std::to_string(actual[i]));
        }
    }
    _readback.Reset();
    *_verified = true;
}
