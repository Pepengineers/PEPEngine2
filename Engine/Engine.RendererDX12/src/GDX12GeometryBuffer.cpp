#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"

#include <map>
#include <limits>
#include <stdexcept>

GDX12GeometryBuffer::GDX12GeometryBuffer(GDX12Device* device) : _device(device), _vertexBufferView({}), _indexBufferView({}),
_totalVertices(0), _totalIndices(0)
{

}

GDX12GeometryBuffer::~GDX12GeometryBuffer() = default;

void GDX12GeometryBuffer::Clear()
{
    _meshCache.clear();
    _vertexDataCPU.clear();
    _indexDataCPU.clear();
    _vertexBuffer.Reset();
    _indexBuffer.Reset();
    _vertexBufferView = {};
    _indexBufferView = {};
    _totalIndices = 0;
    _totalVertices = 0;
    if (_vertexSRV._heap) { UpdateVertexSRV(_vertexSRV._heap); }
    if (_indexSRV._heap) { UpdateIndexSRV(_indexSRV._heap); }
}

void GDX12GeometryBuffer::ResizeVertexBuffer(UINT newSize)
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * VERTEX_SIZE);
    CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);

    ComPtr<ID3D12Resource> newBuffer;
    _device->GetDevice()->CreateCommittedResource(
        &heapPropsDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&newBuffer));

    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(newSize * VERTEX_SIZE);
    ComPtr<ID3D12Resource> uploadBuffer;
    _device->GetDevice()->CreateCommittedResource(
        &heapPropsUpload,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, _vertexDataCPU.data(), newSize * VERTEX_SIZE);
    uploadBuffer->Unmap(0, nullptr);

    auto cmdQueue = _device->GetCommandQueue();
    auto cmdList = cmdQueue->GetCommandList();

    cmdList->GetCommandList()->CopyResource(newBuffer.Get(), uploadBuffer.Get());

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    _vertexBuffer = newBuffer;
    _totalVertices = newSize;

    _vertexBufferView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
    _vertexBufferView.SizeInBytes = static_cast<UINT>(_totalVertices * VERTEX_SIZE);
    _vertexBufferView.StrideInBytes = static_cast<UINT>(VERTEX_SIZE);
    if (_vertexSRV._heap) { UpdateVertexSRV(_vertexSRV._heap); }
}

void GDX12GeometryBuffer::ResizeIndexBuffer(UINT newSize)
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * INDEX_SIZE);
    CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);
    CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);

    ComPtr<ID3D12Resource> newBuffer;
    _device->GetDevice()->CreateCommittedResource(
        &heapPropsDefault,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&newBuffer));

    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(newSize * INDEX_SIZE);
    ComPtr<ID3D12Resource> uploadBuffer;
    _device->GetDevice()->CreateCommittedResource(
        &heapPropsUpload,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, _indexDataCPU.data(), newSize * INDEX_SIZE);
    uploadBuffer->Unmap(0, nullptr);

    auto cmdQueue = _device->GetCommandQueue();
    auto cmdList = cmdQueue->GetCommandList();

    cmdList->GetCommandList()->CopyResource(newBuffer.Get(), uploadBuffer.Get());

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    _indexBuffer = newBuffer;
    _totalIndices = newSize;

    _indexBufferView.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
    _indexBufferView.SizeInBytes = static_cast<UINT>(_totalIndices * INDEX_SIZE);
    _indexBufferView.Format = DXGI_FORMAT_R32_UINT;
    if (_indexSRV._heap) { UpdateIndexSRV(_indexSRV._heap); }
}

void GDX12GeometryBuffer::AddMesh(const Mesh* mesh, MeshHandle handle)
{
    if (_meshCache.find(handle.GetValue()) != _meshCache.end())
    {
        std::string errorMsg = "ERROR: Mesh with name " + std::to_string(handle.GetValue()) 
            + " already exists in GPUMesh directory. Skipping...\n";
        OutputDebugStringA(errorMsg.c_str());
        return;
    }
    
    _meshCache[handle.GetValue()] = std::make_unique<GPUMesh>(mesh);

    UINT totalNewVertices = 0;
    UINT totalNewIndices = 0;

    for (const auto& subMesh : mesh->GetSubMeshes())
    {
        totalNewVertices += static_cast<UINT>(subMesh.GetVertexCount());
        totalNewIndices += static_cast<UINT>(subMesh.GetIndexCount());
    }

    _vertexDataCPU.resize(_totalVertices + totalNewVertices);
    _indexDataCPU.resize(_totalIndices + totalNewIndices);

    UINT currentVertexOffset = _totalVertices;
    UINT currentIndexOffset = _totalIndices;

    for (int i = 0; i < mesh->GetSubMeshCount(); i++)
    {
        const auto& CPUSubMesh = mesh->GetSubMesh(i);
        auto& GPUSubMesh = _meshCache[handle.GetValue()]->SubMeshes[i];

        std::copy(CPUSubMesh.Vertices.begin(), CPUSubMesh.Vertices.end(),
            _vertexDataCPU.begin() + currentVertexOffset);

        std::copy(CPUSubMesh.Indices.begin(), CPUSubMesh.Indices.end(),
            _indexDataCPU.begin() + currentIndexOffset);

        GPUSubMesh.StartVertexLocation = currentVertexOffset;
        GPUSubMesh.StartIndexLocation = currentIndexOffset;
        GPUSubMesh.IndexCount = CPUSubMesh.GetIndexCount();
        GPUSubMesh.MaterialIndex = CPUSubMesh.MaterialIndex;

        currentVertexOffset += CPUSubMesh.GetVertexCount();
        currentIndexOffset += CPUSubMesh.GetIndexCount();
    }

    _totalVertices = currentVertexOffset;
    _totalIndices = currentIndexOffset;

    ResizeVertexBuffer(_totalVertices);
    ResizeIndexBuffer(_totalIndices);
}

const GPUMesh* GDX12GeometryBuffer::GetGPUMeshByHandle(MeshHandle handle)
{
    return _meshCache[handle.GetValue()].get();
}

void GDX12GeometryBuffer::CreateSRVs(GDX12DescriptorHeap* heap)
{
    if (!heap || (_vertexSRV._heap && _vertexSRV._heap != heap))
    { throw std::invalid_argument("ERROR: Geometry SRVs require the same renderer heap for their lifetime."); }
    UpdateVertexSRV(heap);
    UpdateIndexSRV(heap);
}

void GDX12GeometryBuffer::UpdateVertexSRV(GDX12DescriptorHeap* heap)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = (std::max)(1u, _totalVertices);
    view.Buffer.StructureByteStride = sizeof(Vertex);
    const auto index = _vertexSRV.HeapIndex == UINT_MAX ? heap->GetAvailableIndex(RendererResources_StartIndex) : _vertexSRV.HeapIndex;
    _vertexSRV.InitAsSRV(_vertexBuffer.Get(), &view, heap, index);
}

void GDX12GeometryBuffer::UpdateIndexSRV(GDX12DescriptorHeap* heap)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Format = DXGI_FORMAT_R32_UINT;
    view.Buffer.NumElements = (std::max)(1u, _totalIndices);
    const auto index = _indexSRV.HeapIndex == UINT_MAX ? heap->GetAvailableIndex(RendererResources_StartIndex) : _indexSRV.HeapIndex;
    _indexSRV.InitAsSRV(_indexBuffer.Get(), &view, heap, index);
}

std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> GDX12GeometryBuffer::GetRayTracingGeometryDescriptions(MeshHandle handle) const
{
    const auto found = _meshCache.find(handle.GetValue());
    if (found == _meshCache.end() || !found->second || !_vertexBuffer || !_indexBuffer)
    { throw std::invalid_argument("ERROR: Ray tracing geometry requires an uploaded mesh: " + std::to_string(handle.GetValue())); }
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> descriptions;
    descriptions.reserve(found->second->SubMeshes.size());
    for (const auto& subMesh : found->second->SubMeshes)
    {
        D3D12_RAYTRACING_GEOMETRY_DESC description{};
        description.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        description.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        description.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        description.Triangles.VertexBuffer.StartAddress = _vertexBuffer->GetGPUVirtualAddress()
            + UINT64(subMesh.StartVertexLocation) * sizeof(Vertex);
        description.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        description.Triangles.VertexCount = static_cast<UINT>(subMesh.CPUSubmesh->GetVertexCount());
        description.Triangles.IndexBuffer = _indexBuffer->GetGPUVirtualAddress() + UINT64(subMesh.StartIndexLocation) * sizeof(UINT);
        description.Triangles.IndexCount = subMesh.IndexCount;
        descriptions.push_back(description);
    }
    return descriptions;
}

void GDX12GeometryBuffer::BuildBLAS(const std::vector<MeshHandle>& meshes)
{
    std::map<UINT, std::unique_ptr<GDX12AccelerationStructure>> pending;
    for (const auto handle : meshes)
    {
        const auto found = _meshCache.find(handle.GetValue());
        if (found == _meshCache.end() || !found->second || !_vertexBuffer || !_indexBuffer)
        { throw std::invalid_argument("ERROR: BLAS preparation requires an uploaded mesh: " + std::to_string(handle.GetValue())); }
        if (!found->second->BLAS && !pending.count(handle.GetValue()))
        { pending.emplace(handle.GetValue(), std::make_unique<GDX12AccelerationStructure>(_device)); }
    }
    if (pending.empty()) { return; }
    if (pending.size() > (std::numeric_limits<UINT>::max)())
    { throw std::length_error("ERROR: Too many BLAS in one build batch."); }

    const auto sizeBytes = UINT64(pending.size()) * sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);
    const auto description = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto gpuHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    GDX12Resource sizes;
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(&gpuHeap, D3D12_HEAP_FLAG_NONE, &description,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&sizes.D3DResource)));
    sizes.SetCurrentState(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const auto readbackDescription = CD3DX12_RESOURCE_DESC::Buffer(sizeBytes);
    const auto readbackHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readbackDescription,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));

    auto* queue = _device->GetCommandQueue();
    auto* commands = queue->GetCommandList();
    GDX12Resource vertices, indices;
    vertices.D3DResource = _vertexBuffer;
    indices.D3DResource = _indexBuffer;
    const auto vertexState = vertices.GetCurrentState();
    const auto indexState = indices.GetCurrentState();
    commands->ResourceBarrier({vertices.GetNonPixelShaderResourceBarrier(), indices.GetNonPixelShaderResourceBarrier()});
    commands->KeepResourceAlive(_vertexBuffer.Get());
    commands->KeepResourceAlive(_indexBuffer.Get());
    commands->KeepResourceAlive(sizes.D3DResource.Get());
    commands->KeepResourceAlive(readback.Get());
    GDX12RayTracingScratchBuffer scratch(_device);
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
    addresses.reserve(pending.size());
    commands->BeginPixEvent("Build Static BLAS Batch", Colors::Orange);
    for (auto& [handle, structure] : pending)
    {
        const auto geometries = GetRayTracingGeometryDescriptions(MeshHandle(handle));
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = static_cast<UINT>(geometries.size());
        inputs.pGeometryDescs = geometries.data();
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
            | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        structure->Build(commands, inputs, scratch);
        addresses.push_back(structure->GetGPUVirtualAddress());
    }
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild{};
    postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
    postbuild.DestBuffer = sizes.D3DResource->GetGPUVirtualAddress();
    commands->EmitRaytracingAccelerationStructurePostbuildInfo(&postbuild, static_cast<UINT>(addresses.size()), addresses.data());
    commands->ResourceBarrier({sizes.GetCopySourceBarrier(), vertices.GetTransitionBarrier(vertexState), indices.GetTransitionBarrier(indexState)});
    commands->CopyResource(readback.Get(), sizes.D3DResource.Get());
    commands->EndPixEvent();
    queue->ExecuteCommandList(commands);
    GDX12CommandQueue::CPUwait(queue->GetCompletionPoint());

    std::vector<UINT64> compactedSizes(pending.size());
    void* mapped;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(sizeBytes)};
    ThrowIfFailed(readback->Map(0, &readRange, &mapped));
    memcpy(compactedSizes.data(), mapped, static_cast<SIZE_T>(sizeBytes));
    const D3D12_RANGE noWrites{0, 0};
    readback->Unmap(0, &noWrites);

    commands = queue->GetCommandList();
    commands->BeginPixEvent("Compact Static BLAS Batch", Colors::Orange);
    size_t index = 0;
    for (auto& [handle, structure] : pending) { structure->Compact(commands, compactedSizes[index++]); }
    commands->EndPixEvent();
    queue->ExecuteCommandList(commands);
    for (auto& [handle, structure] : pending) { _meshCache.at(handle)->BLAS = std::move(structure); }
}
