#include "../SyntheticBufferPasses.h"

#include <stdexcept>
#include <string>

namespace
{
    constexpr UINT ThreadsPerGroup = 128;
    constexpr UINT ElementsPerGroup = ThreadsPerGroup * 16;
    constexpr UINT StreamStride = 4 * sizeof(UINT);
    constexpr UINT GroupsPerMiB = (1u << 20) / (ElementsPerGroup * StreamStride);

    UINT ValidateSize(UINT sizeMiB)
    {
        if (sizeMiB == 0 || sizeMiB > 1024)
        {
            throw std::invalid_argument("Synthetic buffer size must be between 1 and 1024 MiB.");
        }
        return sizeMiB;
    }
}

void SyntheticBufferComputePass::InitializeShader(PassInitContext& context, const char* entryPoint, bool readsBuffer)
{
    auto* device = context.GetDevice();
    const auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(
        device, std::wstring(PROJECT_DIR) + L"/SyntheticBuffers.hlsl", nullptr, entryPoint, "cs");

    GDX12RootSignatureDesc rootDesc;
    rootDesc.Constants = {1};
    rootDesc.NumSingleSRVSlots = readsBuffer ? 1 : 0;
    rootDesc.NumSingleUAVSlots = 1;
    _rootSignature = std::make_unique<GDX12RootSignature>(device, rootDesc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = _rootSignature->GetRootSignature().Get();
    desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&_pipeline)));
}

GDX12CommandList* SyntheticBufferComputePass::Bind(PassExecutionContext& context)
{
    auto* list = context.GetCommandList();
    list->SetComputeRootSignature(_rootSignature.get());
    list->SetPipelineState(_pipeline);
    list->SetDescriptorHeaps({context.GetDeviceResources()->SRV_UAV_Heap.get()});
    return list;
}

SyntheticBufferWritePass::SyntheticBufferWritePass(UINT sizeMiB) : _sizeMiB(ValidateSize(sizeMiB)) {}

void SyntheticBufferWritePass::Describe(PassDescription& description) const
{
    auto& data = description.AddOutputBuffer("Data");
    data.Stride = StreamStride;
    data.Capacity = _sizeMiB * GroupsPerMiB * ElementsPerGroup;
    data.Layout = "uint4";
    data.AllowShared = true;
    data.Usages = ResourceUsage::UnorderedAccess;
}

void SyntheticBufferWritePass::Initialize(PassInitContext& context)
{
    InitializeShader(context, "Produce", false);
}

void SyntheticBufferWritePass::Execute(PassExecutionContext& context)
{
    auto* list = Bind(context);
    list->SetComputeUAV(0, context.GetOutputBuffer("Data").GetUAV()->GPUHandle);
    list->GetCommandList()->SetComputeRoot32BitConstant(0, static_cast<UINT>(context.GetFrameId()), 0);
    list->BeginPixEvent("Synthetic buffer write", Colors::Gold);
    list->Dispatch(GroupsPerMiB, _sizeMiB, 1);
    list->EndPixEvent();
}

SyntheticBufferReadPass::SyntheticBufferReadPass(UINT sizeMiB, UINT readCount, bool* verified)
    : _sizeMiB(ValidateSize(sizeMiB)), _readCount(readCount), _verified(verified)
{
    if (_readCount == 0)
    {
        throw std::invalid_argument("At least one synthetic buffer read is required.");
    }
}

void SyntheticBufferReadPass::Describe(PassDescription& description) const
{
    auto& data = description.AddInputBuffer("Data");
    data.Stride = StreamStride;
    data.Layout = "uint4";
    data.AllowShared = true;
    data.Usages = ResourceUsage::NonPixelShaderRead;

    auto& checksum = description.AddOutputBuffer("Checksum");
    checksum.Stride = sizeof(UINT);
    checksum.Capacity = _sizeMiB * GroupsPerMiB;
    checksum.Layout = "uint";
    checksum.Usages = ResourceUsage::UnorderedAccess;
}

void SyntheticBufferReadPass::Initialize(PassInitContext& context)
{
    const auto& data = context.GetInputResourceDescription("Data");
    if (data.Capacity != _sizeMiB * GroupsPerMiB * ElementsPerGroup)
    {
        throw std::invalid_argument("Synthetic buffer producer and consumer sizes differ.");
    }

    InitializeShader(context, "Consume", true);
    if (_verified)
    {
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_READBACK);
        const auto desc = CD3DX12_RESOURCE_DESC::Buffer(_sizeMiB * GroupsPerMiB * sizeof(UINT));
        ThrowIfFailed(context.GetDevice()->GetDevice()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readback)));
    }
}

void SyntheticBufferReadPass::Execute(PassExecutionContext& context)
{
    if (_verificationRecorded && _readback)
    {
        VerifyChecksums(*context.GetDevice()->GetCommandQueue());
    }

    const auto& checksum = context.GetOutputBuffer("Checksum");
    auto* list = Bind(context);
    list->SetComputeSRV(0, context.GetInputBuffer("Data").GetSRV()->GPUHandle);
    list->SetComputeUAV(0, checksum.GetUAV()->GPUHandle);
    list->BeginPixEvent("Synthetic buffer read", Colors::CornflowerBlue);
    for (UINT read = 0; read < _readCount; ++read)
    {
        if (read > 0)
        {
            list->ResourceBarrier({checksum.Resource->GetUAVBarrier()});
        }
        list->GetCommandList()->SetComputeRoot32BitConstant(0, read, 0);
        list->Dispatch(GroupsPerMiB, _sizeMiB, 1);
    }
    list->EndPixEvent();

    if (_readback)
    {
        _verificationSeed = static_cast<UINT>(context.GetFrameId());
        list->ResourceBarrier({checksum.Resource->GetCopySourceBarrier()});
        list->GetCommandList()->CopyBufferRegion(_readback.Get(), 0, checksum.Resource->D3DResource.Get(),
            0, _sizeMiB * GroupsPerMiB * sizeof(UINT));
        _verificationRecorded = true;
    }
}

UINT SyntheticBufferReadPass::ExpectedChecksum(UINT group) const
{
    // The shader adds one read index and thread index per thread before the group sum.
    UINT sum = ThreadsPerGroup * (_readCount - 1) + ThreadsPerGroup * (ThreadsPerGroup - 1) / 2;
    for (UINT element = 0; element < ElementsPerGroup; ++element)
    {
        const UINT index = group * ElementsPerGroup + element;
        const UINT value = (index ^ _verificationSeed) * 1664525u + 1013904223u;
        sum += value ^ ((value ^ index) * 3u) ^ (~value * 11u) ^ ((value * 17u) * 13u);
    }
    return sum;
}

void SyntheticBufferReadPass::VerifyChecksums(GDX12CommandQueue& queue)
{
    // FrameGraph submitted the readback copy during the previous frame.
    queue.Flush();
    const UINT groupCount = _sizeMiB * GroupsPerMiB;
    const D3D12_RANGE readRange{0, groupCount * sizeof(UINT)};
    void* mapped = nullptr;
    ThrowIfFailed(_readback->Map(0, &readRange, &mapped));

    const auto* actual = static_cast<const UINT*>(mapped);
    UINT group = 0;
    while (group < groupCount && actual[group] == ExpectedChecksum(group))
    {
        ++group;
    }
    const D3D12_RANGE noWrites{0, 0};
    _readback->Unmap(0, &noWrites);
    if (group != groupCount)
    {
        throw std::runtime_error("Synthetic buffer checksum mismatch at group " + std::to_string(group));
    }

    _readback.Reset();
    *_verified = true;
}
