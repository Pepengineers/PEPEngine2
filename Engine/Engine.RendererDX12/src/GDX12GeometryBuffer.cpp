#include "Engine.RendererDX12/GDX12GeometryBuffer.h"
#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include "Engine.RendererDX12/GDX12DescriptorHeap.h"
#include <numeric>
#include <stdexcept>

GDX12GeometryBuffer::GDX12GeometryBuffer(GDX12Device* device) : _device(device), _vertexBufferView({}), _indexBufferView({}), _totalVertices(0), _totalIndices(0) {}
GDX12GeometryBuffer::~GDX12GeometryBuffer() = default;
void GDX12GeometryBuffer::Clear()
{
    _meshCache.clear(); _freeRanges.clear();
    _vertexBuffer.Reset(); _indexBuffer.Reset(); _previousPositions.Reset();
    _vertexBufferView = {}; _indexBufferView = {};
    _totalVertices = _totalIndices = _previousCount = 0;
}
void GDX12GeometryBuffer::Grow(GDX12Resource& buffer, UINT64 bytes, D3D12_RESOURCE_FLAGS flags)
{
    if (buffer.D3DResource && buffer.D3DResource->GetDesc().Width >= bytes) return;
    auto* queue = _device->GetCommandQueue(); queue->Flush();
    GDX12Resource replacement;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes, flags);
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&replacement.D3DResource)));
    if (buffer.D3DResource)
    {
        auto* commands = queue->GetCommandList();
        commands->ResourceBarrier({buffer.GetCopySourceBarrier(),replacement.GetCopyDestBarrier()});
        commands->GetCommandList()->CopyBufferRegion(replacement.D3DResource.Get(),0,buffer.D3DResource.Get(),0,buffer.D3DResource->GetDesc().Width);
        commands->ResourceBarrier({replacement.GetGenericReadBarrier()});
        queue->ExecuteCommandList(commands); queue->Flush();
    }
    buffer = replacement;
}
void GDX12GeometryBuffer::Upload(GDX12Resource& buffer, UINT64 offset, const void* data, UINT64 bytes)
{
    if (!bytes) return;
    const auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(_device->GetDevice()->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));
    void* mapped = nullptr; ThrowIfFailed(upload->Map(0,nullptr,&mapped)); memcpy(mapped,data,static_cast<size_t>(bytes)); upload->Unmap(0,nullptr);
    auto* queue = _device->GetCommandQueue(); auto* commands = queue->GetCommandList();
    commands->ResourceBarrier({buffer.GetCopyDestBarrier()});
    commands->GetCommandList()->CopyBufferRegion(buffer.D3DResource.Get(),offset,upload.Get(),0,bytes);
    commands->ResourceBarrier({buffer.GetGenericReadBarrier()}); queue->ExecuteCommandList(commands); queue->Flush();
}
void GDX12GeometryBuffer::InitializeViews(GDX12DescriptorHeap* heap)
{
    _heap = heap;
    _vertexUAVIndex = heap->GetAvailableIndex(ConstantsResources);
    _previousSRVIndex = heap->GetAvailableIndex(ConstantsResources);
    _previousUAVIndex = heap->GetAvailableIndex(ConstantsResources);
    UpdateViews();
}
void GDX12GeometryBuffer::UpdateViews()
{
    if (!_heap) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Buffer.NumElements = (std::max)(_previousCount,1u); srv.Buffer.StructureByteStride = 12;
    _previousSRV.InitAsSRV(_previousPositions.D3DResource.Get(),&srv,_heap,_previousSRVIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = (std::max)(_previousCount,1u); uav.Buffer.StructureByteStride = 12;
    _previousUAV.InitAsUAV(_previousPositions.D3DResource.Get(),nullptr,&uav,_heap,_previousUAVIndex);
    uav.Buffer.NumElements = (std::max)(_totalVertices,1u); uav.Buffer.StructureByteStride = sizeof(Vertex);
    _vertexUAV.InitAsUAV(_vertexBuffer.D3DResource.Get(),nullptr,&uav,_heap,_vertexUAVIndex);
    if (_vertexBuffer.D3DResource) _vertexBufferView = {_vertexBuffer.D3DResource->GetGPUVirtualAddress(),_totalVertices*static_cast<UINT>(sizeof(Vertex)),sizeof(Vertex)};
    if (_indexBuffer.D3DResource) _indexBufferView = {_indexBuffer.D3DResource->GetGPUVirtualAddress(),_totalIndices*4,DXGI_FORMAT_R32_UINT};
}
GDX12GeneratedGeometryRange GDX12GeometryBuffer::AllocateGenerated(UINT capacity)
{
    if (!capacity || capacity > UINT_MAX / sizeof(Vertex)) throw std::invalid_argument("Invalid generated geometry capacity");
    _device->GetCommandQueue()->Flush();
    for (auto it = _freeRanges.begin(); it != _freeRanges.end(); ++it)
        if (it->Capacity >= capacity)
        {
            auto range = *it; range.Capacity = capacity;
            it->VertexBase += capacity; it->IndexBase += capacity; it->PreviousBase += capacity; it->Capacity -= capacity;
            if (!it->Capacity) _freeRanges.erase(it);
            std::vector<UINT> indices(capacity); std::iota(indices.begin(),indices.end(),0u);
            Upload(_indexBuffer,UINT64(range.IndexBase)*4,indices.data(),UINT64(capacity)*4);
            return range;
        }
    if (_totalVertices > UINT_MAX/sizeof(Vertex)-capacity || _totalIndices > UINT_MAX/4-capacity || _previousCount > UINT_MAX/12-capacity)
        throw std::overflow_error("Generated geometry arena exceeds a DX12 buffer view");
    GDX12GeneratedGeometryRange range{_totalVertices,_totalIndices,_previousCount,capacity};
    _totalVertices += capacity; _totalIndices += capacity; _previousCount += capacity;
    Grow(_vertexBuffer,UINT64(_totalVertices)*sizeof(Vertex),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    Grow(_indexBuffer,UINT64(_totalIndices)*4,D3D12_RESOURCE_FLAG_NONE);
    Grow(_previousPositions,UINT64(_previousCount)*12,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    std::vector<UINT> indices(capacity); std::iota(indices.begin(),indices.end(),0u);
    Upload(_indexBuffer,UINT64(range.IndexBase)*4,indices.data(),UINT64(capacity)*4); UpdateViews();
    return range;
}
void GDX12GeometryBuffer::ReleaseGenerated(GDX12GeneratedGeometryRange range)
{
    if (!range.Capacity) return;
    _device->GetCommandQueue()->Flush(); _freeRanges.push_back(range);
    std::sort(_freeRanges.begin(),_freeRanges.end(),[](auto a, auto b){return a.VertexBase < b.VertexBase;});
    for (size_t i = 1; i < _freeRanges.size();)
    {
        auto& a = _freeRanges[i-1]; const auto& b = _freeRanges[i];
        if (a.VertexBase+a.Capacity==b.VertexBase && a.IndexBase+a.Capacity==b.IndexBase && a.PreviousBase+a.Capacity==b.PreviousBase)
        { a.Capacity += b.Capacity; _freeRanges.erase(_freeRanges.begin()+i); }
        else ++i;
    }
}
void GDX12GeometryBuffer::AddMesh(const Mesh* mesh, MeshHandle handle)
{
    if (_meshCache.find(handle.GetValue()) != _meshCache.end()) return;
    auto gpu = std::make_unique<GPUMesh>(mesh);
    std::vector<Vertex> vertices; std::vector<UINT> indices;
    for (size_t i = 0; i < mesh->GetSubMeshCount(); ++i)
    {
        const auto& part = mesh->GetSubMesh(i); auto& output = gpu->SubMeshes[i];
        output.StartVertexLocation = _totalVertices+static_cast<UINT>(vertices.size());
        output.StartIndexLocation = _totalIndices+static_cast<UINT>(indices.size()); output.MaterialIndex = part.MaterialIndex;
        vertices.insert(vertices.end(),part.Vertices.begin(),part.Vertices.end()); indices.insert(indices.end(),part.Indices.begin(),part.Indices.end());
    }
    const UINT oldVertices = _totalVertices, oldIndices = _totalIndices;
    if (vertices.size() > UINT_MAX/sizeof(Vertex)-_totalVertices || indices.size() > UINT_MAX/4-_totalIndices) throw std::overflow_error("Static geometry exceeds a DX12 buffer view");
    _totalVertices += static_cast<UINT>(vertices.size()); _totalIndices += static_cast<UINT>(indices.size());
    if (_totalVertices) Grow(_vertexBuffer,UINT64(_totalVertices)*sizeof(Vertex),D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (_totalIndices) Grow(_indexBuffer,UINT64(_totalIndices)*4,D3D12_RESOURCE_FLAG_NONE);
    Upload(_vertexBuffer,UINT64(oldVertices)*sizeof(Vertex),vertices.data(),vertices.size()*sizeof(Vertex));
    Upload(_indexBuffer,UINT64(oldIndices)*4,indices.data(),indices.size()*4); UpdateViews(); _meshCache.emplace(handle.GetValue(),std::move(gpu));
}
const GPUMesh* GDX12GeometryBuffer::GetGPUMeshByHandle(MeshHandle handle)
{
    const auto it = _meshCache.find(handle.GetValue()); return it==_meshCache.end()?nullptr:it->second.get();
}
