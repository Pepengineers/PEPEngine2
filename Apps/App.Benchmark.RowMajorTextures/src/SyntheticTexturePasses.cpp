#include "../SyntheticTexturePasses.h"

#include <array>
#include <stdexcept>

namespace
{
    constexpr UINT PixelsPerGroupSide = 32;
    constexpr UINT ValidationGroupCount = 3;

    // Matches PixelValue in SyntheticTextures.hlsl.
    UINT PixelValue(UINT index, UINT seed)
    {
        UINT value = (index ^ seed) * 1664525u + 1013904223u;
        value ^= value >> 16;
        value *= 2246822519u;
        return value ^ (value >> 13);
    }
}

SyntheticTexturePass::SyntheticTexturePass(UINT extent)
    : _extent(extent), _groupsPerRow(extent / PixelsPerGroupSide)
{
    if (!extent || extent > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || extent % PixelsPerGroupSide)
    {
        throw std::invalid_argument("Synthetic texture extent must be a nonzero multiple of 32 within the D3D12 texture limit.");
    }
}

void SyntheticTexturePass::InitializeShader(PassInitContext& context, const char* entry, bool readsTexture)
{
    auto* device = context.GetDevice();
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(
        device, std::wstring(PROJECT_DIR) + L"SyntheticTextures.hlsl", nullptr, entry, "cs");
    GDX12RootSignatureDesc rootDescription;
    rootDescription.Constants = {2};
    rootDescription.NumSingleSRVSlots = readsTexture ? 1 : 0;
    rootDescription.NumSingleUAVSlots = 1;
    _rootSignature = std::make_unique<GDX12RootSignature>(device, rootDescription);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineDescription{};
    pipelineDescription.pRootSignature = _rootSignature->GetRootSignature().Get();
    pipelineDescription.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipelineDescription, IID_PPV_ARGS(&_pipeline)));
}

GDX12CommandList* SyntheticTexturePass::Bind(PassExecutionContext& context)
{
    auto* commands = context.GetCommandList();
    commands->SetComputeRootSignature(_rootSignature.get());
    commands->SetPipelineState(_pipeline);
    commands->SetDescriptorHeaps({context.GetDeviceResources()->SRV_UAV_Heap.get()});
    return commands;
}

void SyntheticTexturePass::Dispatch(GDX12CommandList& commands, UINT seed) const
{
    const UINT constants[] = {_extent, seed};
    commands.GetCommandList()->SetComputeRoot32BitConstants(0, 2, constants, 0);
    commands.Dispatch(_groupsPerRow, _groupsPerRow, 1);
}

void SyntheticTextureWritePass::Describe(PassDescription& description) const
{
    auto& texture = description.AddOutputTexture("Data");
    texture.Formats = {SyntheticTextureFormat};
    texture.Size = Extent::Fixed({_extent, _extent});
    texture.AllowShared = true;
    texture.Usages = ResourceUsage::UnorderedAccess;
}

void SyntheticTextureWritePass::Initialize(PassInitContext& context)
{
    InitializeShader(context, "Produce", false);
}

void SyntheticTextureWritePass::Execute(PassExecutionContext& context)
{
    auto* commands = Bind(context);
    commands->BeginPixEvent("Synthetic texture write", Colors::Gold);
    commands->SetComputeUAV(0, context.GetOutputTexture("Data").GetUAV()->GPUHandle);
    Dispatch(*commands, static_cast<UINT>(context.GetFrameId()));
    commands->EndPixEvent();
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
    InitializeShader(context, "Consume", true);
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
    auto* commands = Bind(context);
    commands->BeginPixEvent("Synthetic texture read", Colors::CornflowerBlue);
    commands->SetComputeSRV(0, context.GetInputTexture("Data").GetSRV()->GPUHandle);
    commands->SetComputeUAV(0, checksum.GetUAV()->GPUHandle);
    Dispatch(*commands);
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
