#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <cstddef>
#include <cmath>
#include <stdexcept>

static_assert(sizeof(Vertex) == 56 && offsetof(Vertex, PreviousPosition) == 44);
static_assert(sizeof(SkinVertexRange) == 8 && sizeof(SkinInfluence) == 8);
static_assert(sizeof(GDX12SkinPalette) == 128);

GDX12Skinning::GDX12Skinning(GDX12Device* device) : _device(device)
{
    CD3DX12_ROOT_PARAMETER parameters[6];
    parameters[0].InitAsConstants(3, 0);
    for (UINT i = 0; i < 4; ++i) parameters[i + 1].InitAsShaderResourceView(i);
    parameters[5].InitAsUnorderedAccessView(0);
    CD3DX12_ROOT_SIGNATURE_DESC desc(6, parameters);
    ComPtr<ID3DBlob> serialized, errors;
    ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &errors));
    ThrowIfFailed(device->GetDevice()->CreateRootSignature(0, serialized->GetBufferPointer(),
        serialized->GetBufferSize(), IID_PPV_ARGS(&_rootSignature)));
    auto shader = GDX12ShaderCompiler::GetInstance().CompileShader(device,
        SHADERS_FOLDER "Skinning.hlsl", nullptr, "CS", "cs");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = _rootSignature.Get();
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    ThrowIfFailed(device->GetDevice()->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&_pipeline)));
}

std::unique_ptr<GDX12SkinMesh> GDX12Skinning::CreateMesh(const SubMesh& source, UINT frameCount)
{
    if (source.Vertices.empty() || source.Vertices.size() > UINT_MAX / sizeof(Vertex) || !frameCount ||
        source.Bones.size() >= UINT_MAX / sizeof(GDX12SkinPalette) ||
        source.Influences.size() > UINT_MAX / sizeof(SkinInfluence))
        throw std::invalid_argument("Invalid skin buffer size");
    if (!source.SkinVertices.empty() && source.SkinVertices.size() != source.Vertices.size())
        throw std::invalid_argument("Skin range count differs from vertex count");
    for (const auto& bone : source.Bones)
    {
        const auto* values = reinterpret_cast<const float*>(&bone.InverseBind);
        for (size_t i = 0; i < 16; ++i)
            if (!std::isfinite(values[i])) throw std::invalid_argument("Non-finite inverse bind");
        if (bone.InverseBind._14 != 0 || bone.InverseBind._24 != 0 ||
            bone.InverseBind._34 != 0 || bone.InverseBind._44 != 1)
            throw std::invalid_argument("Inverse bind must be affine");
    }
    for (const auto& vertex : source.Vertices)
    {
        const float values[] = {vertex.Position.x, vertex.Position.y, vertex.Position.z,
            vertex.Normal.x, vertex.Normal.y, vertex.Normal.z, vertex.TexCoord.x, vertex.TexCoord.y,
            vertex.Tangent.x, vertex.Tangent.y, vertex.Tangent.z};
        for (const float value : values)
            if (!std::isfinite(value)) throw std::invalid_argument("Non-finite skin vertex");
    }
    for (const auto index : source.Indices)
        if (index >= source.Vertices.size()) throw std::invalid_argument("Skin index exceeds vertex buffer");
    std::vector<SkinVertexRange> ranges = source.SkinVertices;
    if (ranges.empty()) ranges.resize(source.Vertices.size());
    for (const auto& range : ranges)
    {
        if (range.Offset > source.Influences.size() || range.Count > source.Influences.size() - range.Offset)
            throw std::invalid_argument("Skin range exceeds influence buffer");
        double sum = 0;
        for (size_t i = range.Offset; i < static_cast<size_t>(range.Offset) + range.Count; ++i)
        {
            const auto& influence = source.Influences[i];
            if (influence.BoneIndex >= source.Bones.size() || !std::isfinite(influence.Weight) || influence.Weight < 0)
                throw std::invalid_argument("Invalid skin influence");
            sum += influence.Weight;
        }
        if (range.Count && std::abs(sum - 1.0) > 0.0001)
            throw std::invalid_argument("Skin weights must be normalized");
    }
    auto result = std::make_unique<GDX12SkinMesh>();
    result->VertexCount = static_cast<UINT>(source.Vertices.size());
    result->Vertices = std::make_unique<GDX12UploadBuffer<Vertex>>(_device, result->VertexCount, EBufferType::Default, false);
    result->Ranges = std::make_unique<GDX12UploadBuffer<SkinVertexRange>>(_device, result->VertexCount, EBufferType::Default, false);
    result->Influences = std::make_unique<GDX12UploadBuffer<SkinInfluence>>(_device,
        (std::max)(1u, static_cast<UINT>(source.Influences.size())), EBufferType::Default, false);
    GDX12UploadBuffer<Vertex> verticesUpload(_device, result->VertexCount, EBufferType::Upload, false);
    GDX12UploadBuffer<SkinVertexRange> rangesUpload(_device, result->VertexCount, EBufferType::Upload, false);
    GDX12UploadBuffer<SkinInfluence> influencesUpload(_device, result->Influences->GetElementCount(), EBufferType::Upload, false);
    verticesUpload.CopyData(0, source.Vertices.data(), result->VertexCount);
    rangesUpload.CopyData(0, ranges.data(), result->VertexCount);
    if (!source.Influences.empty())
        influencesUpload.CopyData(0, source.Influences.data(), static_cast<UINT>(source.Influences.size()));
    for (UINT i = 0; i < frameCount; ++i)
    {
        result->Palettes.push_back(std::make_unique<GDX12UploadBuffer<GDX12SkinPalette>>(
            _device, static_cast<UINT>(source.Bones.size()) + 1, EBufferType::Default, false));
        result->PaletteUploads.push_back(std::make_unique<GDX12UploadBuffer<GDX12SkinPalette>>(
            _device, static_cast<UINT>(source.Bones.size()) + 1, EBufferType::Upload, false));
    }
    auto* queue = _device->GetCommandQueue();
    auto* commands = queue->GetCommandList();
    const auto upload = [commands](auto& target, auto& staging)
    {
        commands->ResourceBarrier({target->GetResource().GetCopyDestBarrier()});
        commands->GetCommandList()->CopyBufferRegion(target->GetResource().D3DResource.Get(), 0,
            staging.GetResource().D3DResource.Get(), 0, UINT64(staging.GetElementCount()) * staging.GetElementSize());
        commands->ResourceBarrier({target->GetResource().GetNonPixelShaderResourceBarrier()});
    };
    upload(result->Vertices, verticesUpload);
    upload(result->Ranges, rangesUpload);
    upload(result->Influences, influencesUpload);
    queue->ExecuteCommandList(commands);
    // Mesh creation precedes frame recording; keep static staging alive through its upload fence.
    queue->Flush();
    return result;
}

void GDX12Skinning::Dispatch(GDX12CommandList* commands, GDX12GeometryBuffer& geometry,
    GDX12SkinMesh& mesh, const UINT frameSlot, const std::vector<GDX12SkinPalette>& palette)
{
    auto& upload = mesh.PaletteUploads.at(frameSlot);
    auto& snapshot = mesh.Palettes.at(frameSlot);
    if (palette.size() != upload->GetElementCount())
        throw std::invalid_argument("Skin palette size changed");
    upload->CopyData(0, palette.data(), static_cast<UINT>(palette.size()));
    auto* list = commands->GetCommandList().Get();
    commands->ResourceBarrier({snapshot->GetResource().GetCopyDestBarrier()});
    list->CopyBufferRegion(snapshot->GetResource().D3DResource.Get(), 0,
        upload->GetResource().D3DResource.Get(), 0, UINT64(palette.size()) * sizeof(GDX12SkinPalette));
    commands->ResourceBarrier({snapshot->GetResource().GetNonPixelShaderResourceBarrier()});
    auto* output = geometry.BeginSkinning(commands);
    list->SetComputeRootSignature(_rootSignature.Get());
    list->SetPipelineState(_pipeline.Get());
    const UINT constants[] = {mesh.VertexCount, mesh.OutputVertex, static_cast<UINT>(palette.size())};
    list->SetComputeRoot32BitConstants(0, 3, constants, 0);
    list->SetComputeRootShaderResourceView(1, mesh.Vertices->GetElementAddress(0));
    list->SetComputeRootShaderResourceView(2, mesh.Ranges->GetElementAddress(0));
    list->SetComputeRootShaderResourceView(3, mesh.Influences->GetElementAddress(0));
    list->SetComputeRootShaderResourceView(4, snapshot->GetElementAddress(0));
    list->SetComputeRootUnorderedAccessView(5, output->GetGPUVirtualAddress());
    const UINT groups = (mesh.VertexCount + 63) / 64;
    list->Dispatch((std::min)(groups, 65535u), (groups + 65534) / 65535, 1);
}
