#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"

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
