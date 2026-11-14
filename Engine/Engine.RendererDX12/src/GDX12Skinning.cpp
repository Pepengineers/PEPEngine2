#include "Engine.RendererDX12/GDX12Skinning.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"
#include <cstddef>
#include <cmath>
#include <stdexcept>
#include "Common/ConsoleVariables.h"

namespace
{
    AutoConsoleVariable<bool> Instancing(L"Render.Skinning.Instancing", false,
        L"Batch visible skeletal parts while preserving individual animation times.");
}

bool GDX12SkeletalInstancingEnabled() { return Instancing.GetValue(); }

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

std::vector<SkinVertexRange> GDX12Skinning::ValidateSource(const SubMesh& source)
{
    if (source.Vertices.empty() || source.Vertices.size() > UINT_MAX / sizeof(Vertex) ||
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
    return ranges;
}

std::unique_ptr<GDX12SkinMesh> GDX12Skinning::CreateMesh(const SubMesh& source, UINT frameCount,
    MeshHandle handle, std::uint64_t revision)
{
    if (!frameCount) throw std::invalid_argument("Skinning needs at least one frame slot");
    auto result = std::make_unique<GDX12SkinMesh>();
    const bool share = handle.IsValid() && revision != 0;
    const auto key = std::make_tuple(reinterpret_cast<std::uintptr_t>(&source), handle.GetValue(), handle.GetGeneration(), revision);
    if (share)
    {
        const auto found = _sources.find(key);
        if (found != _sources.end()) result->Source = found->second.lock();
    }
    if (!result->Source)
    {
        for (auto entry = _sources.begin(); entry != _sources.end();)
            if (entry->second.expired()) entry = _sources.erase(entry);
            else ++entry;
        auto ranges = ValidateSource(source);
        auto inputs = std::make_shared<GDX12SkinSource>();
        const auto vertexCount = static_cast<UINT>(source.Vertices.size());
        inputs->Vertices = std::make_unique<GDX12UploadBuffer<Vertex>>(_device, vertexCount, EBufferType::Default, false);
        inputs->Ranges = std::make_unique<GDX12UploadBuffer<SkinVertexRange>>(_device, vertexCount, EBufferType::Default, false);
        inputs->Influences = std::make_unique<GDX12UploadBuffer<SkinInfluence>>(_device,
            (std::max)(1u, static_cast<UINT>(source.Influences.size())), EBufferType::Default, false);
        GDX12UploadBuffer<Vertex> verticesUpload(_device, vertexCount, EBufferType::Upload, false);
        GDX12UploadBuffer<SkinVertexRange> rangesUpload(_device, vertexCount, EBufferType::Upload, false);
        GDX12UploadBuffer<SkinInfluence> influencesUpload(_device, inputs->Influences->GetElementCount(), EBufferType::Upload, false);
        verticesUpload.CopyData(0, source.Vertices.data(), vertexCount);
        rangesUpload.CopyData(0, ranges.data(), vertexCount);
        if (!source.Influences.empty())
            influencesUpload.CopyData(0, source.Influences.data(), static_cast<UINT>(source.Influences.size()));
        auto* queue = _device->GetCommandQueue();
        auto* commands = queue->GetCommandList();
        const auto upload = [commands](auto& target, auto& staging)
        {
            commands->ResourceBarrier({target->GetResource().GetCopyDestBarrier()});
            commands->GetCommandList()->CopyBufferRegion(target->GetResource().D3DResource.Get(), 0,
                staging.GetResource().D3DResource.Get(), 0, UINT64(staging.GetElementCount()) * staging.GetElementSize());
            commands->ResourceBarrier({target->GetResource().GetNonPixelShaderResourceBarrier()});
        };
        upload(inputs->Vertices, verticesUpload);
        upload(inputs->Ranges, rangesUpload);
        upload(inputs->Influences, influencesUpload);
        queue->ExecuteCommandList(commands);
        // Mesh creation precedes frame recording; keep static staging alive through its upload fence.
        queue->Flush();
        result->Source = std::move(inputs);
        if (share) _sources[key] = result->Source;
    }
    result->VertexCount = result->Source->Vertices->GetElementCount();
    for (UINT i = 0; i < frameCount; ++i)
    {
        result->Palettes.push_back(std::make_unique<GDX12UploadBuffer<GDX12SkinPalette>>(
            _device, static_cast<UINT>(source.Bones.size()) + 1, EBufferType::Default, false));
        result->PaletteUploads.push_back(std::make_unique<GDX12UploadBuffer<GDX12SkinPalette>>(
            _device, static_cast<UINT>(source.Bones.size()) + 1, EBufferType::Upload, false));
    }
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
    RecordDispatch(*commands, mesh.VertexCount, mesh.OutputVertex, static_cast<UINT>(palette.size()),
        mesh.Source->Vertices->GetElementAddress(0), mesh.Source->Ranges->GetElementAddress(0),
        mesh.Source->Influences->GetElementAddress(0), snapshot->GetElementAddress(0), output->GetGPUVirtualAddress());
}

void GDX12Skinning::RecordDispatch(GDX12CommandList& commands, UINT vertexCount, UINT outputVertex, UINT boneCount,
    D3D12_GPU_VIRTUAL_ADDRESS vertices, D3D12_GPU_VIRTUAL_ADDRESS ranges, D3D12_GPU_VIRTUAL_ADDRESS influences,
    D3D12_GPU_VIRTUAL_ADDRESS palette, D3D12_GPU_VIRTUAL_ADDRESS output) const
{
    auto* list = commands.GetCommandList().Get();
    list->SetComputeRootSignature(_rootSignature.Get());
    list->SetPipelineState(_pipeline.Get());
    const UINT constants[] = {vertexCount, outputVertex, boneCount};
    list->SetComputeRoot32BitConstants(0, 3, constants, 0);
    list->SetComputeRootShaderResourceView(1, vertices);
    list->SetComputeRootShaderResourceView(2, ranges);
    list->SetComputeRootShaderResourceView(3, influences);
    list->SetComputeRootShaderResourceView(4, palette);
    list->SetComputeRootUnorderedAccessView(5, output);
    const UINT groups = (vertexCount + 63) / 64;
    list->Dispatch((std::min)(groups, 65535u), (groups + 65534) / 65535, 1);
}

GDX12SkinningWork GDX12Skinning::Freeze(GDX12SkinMesh& mesh,UINT slot,const std::vector<GDX12SkinPalette>& palette) {
    auto& upload=*mesh.PaletteUploads.at(slot);
    if(palette.size()!=upload.GetElementCount()) throw std::invalid_argument("Skin palette size changed");
    upload.CopyData(0,palette.data(),UINT(palette.size()));
    return {_rootSignature,_pipeline,{upload.GetResource().D3DResource.Get(),mesh.Palettes.at(slot)->GetResource().D3DResource.Get(),
        mesh.Source->Vertices->GetResource().D3DResource.Get(),mesh.Source->Ranges->GetResource().D3DResource.Get(),
        mesh.Source->Influences->GetResource().D3DResource.Get()},mesh.VertexCount,mesh.OutputVertex,UINT(palette.size())};
}
void GDX12SkinningWork::Record(ID3D12GraphicsCommandList* list,D3D12_GPU_VIRTUAL_ADDRESS output) const {
    list->SetComputeRootSignature(Root.Get()); list->SetPipelineState(Pipeline.Get());
    const UINT constants[]={VertexCount,OutputVertex,BoneCount}; list->SetComputeRoot32BitConstants(0,3,constants,0);
    for(UINT i=0;i<3;++i) list->SetComputeRootShaderResourceView(i+1,Resources[i+2]->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(4,Resources[1]->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(5,output);
    const UINT groups=(VertexCount+63)/64; list->Dispatch((std::min)(groups,65535u),(groups+65534)/65535,1);
}
