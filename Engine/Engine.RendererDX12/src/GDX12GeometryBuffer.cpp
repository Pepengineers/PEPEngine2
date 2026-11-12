#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"
#include <stdexcept>
#include <climits>

GDX12GeometryBuffer::GDX12GeometryBuffer(GDX12Device* device) : _device(device), _vertexBufferView({}), _indexBufferView({}),
_totalVertices(0), _totalIndices(0)
{

}

GDX12GeometryBuffer::~GDX12GeometryBuffer()
{
    Clear();
}

void GDX12GeometryBuffer::Clear()
{
    _vertexDataCPU.clear();
    _indexDataCPU.clear();
    _meshCache.clear();
    ++_meshCacheRevision;
    _freeSkinRanges.clear();
    _skinnedRanges.clear();
    _freeIndexRanges.clear();
    _vertexBuffer.reset();
    _indexBuffer.reset();
    _vertexBufferView = {};
    _indexBufferView = {};
    _totalIndices = 0;
    _totalVertices = 0;
}

void GDX12GeometryBuffer::ResizeVertexBuffer(const UINT newSize, const UINT updateStart, const UINT updateCount)
{
    if (newSize > UINT_MAX / VERTEX_SIZE) throw std::overflow_error("Vertex buffer exceeds 32-bit addressing");
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * VERTEX_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto newBuffer = std::make_shared<GDX12Resource>();
    newBuffer->D3DResource = _device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
    newBuffer->SetCurrentState(D3D12_RESOURCE_STATE_COPY_DEST);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(newSize * VERTEX_SIZE);
    auto uploadBuffer = _device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);
    void* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped));
    memcpy(mapped, _vertexDataCPU.data(), newSize * VERTEX_SIZE);
    uploadBuffer->Unmap(0, nullptr);

    auto queue = _device->GetCommandQueue();
    auto commands = queue->GetCommandList();
    auto* list = commands->GetCommandList().Get();
    list->CopyBufferRegion(newBuffer->D3DResource.Get(), 0, uploadBuffer.Get(), 0, newSize * VERTEX_SIZE);
    if (_vertexBuffer)
    {
        commands->ResourceBarrier({ _vertexBuffer->GetCopySourceBarrier() });
        // Existing dynamic slices contain the last rendered pose, not their CPU initialization.
        list->CopyBufferRegion(newBuffer->D3DResource.Get(), 0, _vertexBuffer->D3DResource.Get(), 0, _vertexBufferView.SizeInBytes);
    }
    if (updateCount)
        list->CopyBufferRegion(newBuffer->D3DResource.Get(), updateStart * VERTEX_SIZE, uploadBuffer.Get(),
            updateStart * VERTEX_SIZE, updateCount * VERTEX_SIZE);
    commands->ResourceBarrier({ newBuffer->GetTransitionBarrier(VertexReadState) });
    queue->ExecuteCommandList(commands);
    queue->Flush();
    _vertexBuffer = std::move(newBuffer);
    _totalVertices = newSize;
    _vertexBufferView.BufferLocation = GetVertexAddress();
    _vertexBufferView.SizeInBytes = static_cast<UINT>(_totalVertices * VERTEX_SIZE);
    _vertexBufferView.StrideInBytes = static_cast<UINT>(VERTEX_SIZE);
}

void GDX12GeometryBuffer::ReserveSkinnedVertices(const UINT count)
{
    if (!count) return;
    const UINT offset = AllocateSkinnedVertices(count);
    ReleaseSkinnedVertices(offset, count);
}

UINT GDX12GeometryBuffer::AllocateSkinnedVertices(const UINT count)
{
    if (!count) throw std::invalid_argument("Empty skin slice");
    for (auto it = _freeSkinRanges.begin(); it != _freeSkinRanges.end(); ++it)
    {
        if (it->second < count) continue;
        const UINT offset = it->first;
        const UINT remaining = it->second - count;
        _freeSkinRanges.erase(it);
        if (remaining) _freeSkinRanges.emplace(offset + count, remaining);
        _skinnedRanges.emplace(offset, count);
        return offset;
    }
    if (count > UINT_MAX / VERTEX_SIZE - _totalVertices)
        throw std::overflow_error("Skin slice exceeds vertex buffer addressing");
    const UINT offset = _totalVertices;
    _vertexDataCPU.resize(_totalVertices + count);
    ResizeVertexBuffer(_totalVertices + count);
    _skinnedRanges.emplace(offset, count);
    return offset;
}

void GDX12GeometryBuffer::ReleaseSkinnedVertices(UINT offset, UINT count)
{
    if (!count) return;
    _skinnedRanges.erase(offset);
    auto next = _freeSkinRanges.lower_bound(offset);
    if (next != _freeSkinRanges.begin())
    {
        auto previous = std::prev(next);
        if (previous->first + previous->second == offset)
        {
            offset = previous->first;
            count += previous->second;
            _freeSkinRanges.erase(previous);
        }
    }
    if (next != _freeSkinRanges.end() && offset + count == next->first)
    {
        count += next->second;
        _freeSkinRanges.erase(next);
    }
    _freeSkinRanges.emplace(offset, count);
}

ID3D12Resource* GDX12GeometryBuffer::BeginSkinning(GDX12CommandList* commands)
{
    if (!_vertexBuffer) throw std::logic_error("Skinning requires an allocated vertex buffer");
    if (_vertexBuffer->GetCurrentState() != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        commands->ResourceBarrier({ _vertexBuffer->GetUnorderedAccessBarrier() });
    return _vertexBuffer->D3DResource.Get();
}

void GDX12GeometryBuffer::EndSkinning(GDX12CommandList* commands)
{
    if (_vertexBuffer && _vertexBuffer->GetCurrentState() == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        commands->ResourceBarrier({ _vertexBuffer->GetTransitionBarrier(VertexReadState) });
}

void GDX12GeometryBuffer::ResizeIndexBuffer(const UINT newSize)
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * INDEX_SIZE);

    auto newBuffer = std::make_shared<GDX12Resource>();
    newBuffer->D3DResource = _device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr);

    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(newSize * INDEX_SIZE);
    auto uploadBuffer = _device->CreateResource(D3D12_HEAP_TYPE_UPLOAD, uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr);

    void* mapped = nullptr;
    ThrowIfFailed(uploadBuffer->Map(0, nullptr, &mapped));
    memcpy(mapped, _indexDataCPU.data(), newSize * INDEX_SIZE);
    uploadBuffer->Unmap(0, nullptr);

    auto cmdQueue = _device->GetCommandQueue();
    auto cmdList = cmdQueue->GetCommandList();

    cmdList->CopyResource(newBuffer->D3DResource.Get(), uploadBuffer.Get());

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    _indexBuffer = std::move(newBuffer);
    _totalIndices = newSize;

    _indexBufferView.BufferLocation = _indexBuffer->D3DResource->GetGPUVirtualAddress();
    _indexBufferView.SizeInBytes = static_cast<UINT>(_totalIndices * INDEX_SIZE);
    _indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

void GDX12GeometryBuffer::AddMesh(const Mesh* mesh, const MeshHandle handle, const std::uint64_t contentRevision)
{
    if (!mesh || !handle.IsValid()) throw std::invalid_argument("Invalid geometry source");
    auto found = _meshCache.find(handle.GetValue());
    if (found != _meshCache.end() && found->second->SourceHandle == handle &&
        found->second->ContentRevision == contentRevision && found->second->CPUMesh == mesh) return;
    size_t vertices = 0;
    size_t indices = 0;
    for (const auto& part : mesh->GetSubMeshes())
    {
        vertices += part.GetVertexCount();
        indices += part.GetIndexCount();
    }
    if (!vertices || !indices || vertices > UINT_MAX / VERTEX_SIZE || indices > UINT_MAX / INDEX_SIZE)
        throw std::overflow_error("Invalid geometry size");
    auto replacement = std::make_unique<GPUMesh>(mesh);
    replacement->SourceHandle = handle;
    replacement->ContentRevision = contentRevision;
    UINT vertexStart = _totalVertices;
    UINT indexStart = _totalIndices;
    if (found != _meshCache.end())
    {
        // Reimporting a registry slot must not append another permanent geometry allocation.
        ReleaseSkinnedVertices(found->second->VertexStart, found->second->VertexCapacity);
        _freeIndexRanges.emplace(found->second->IndexStart, found->second->IndexCapacity);
    }
    const UINT vertexCount = static_cast<UINT>(vertices);
    const UINT indexCount = static_cast<UINT>(indices);
    bool reusedVertices = false;
    for (auto it = _freeSkinRanges.begin(); it != _freeSkinRanges.end(); ++it)
    {
        if (it->second < vertexCount) continue;
        vertexStart = it->first;
        const UINT remaining = it->second - vertexCount;
        _freeSkinRanges.erase(it);
        if (remaining) _freeSkinRanges.emplace(vertexStart + vertexCount, remaining);
        reusedVertices = true;
        break;
    }
    if (!reusedVertices)
    {
        if (vertexCount > UINT_MAX / VERTEX_SIZE - _totalVertices)
            throw std::overflow_error("Vertex buffer exceeds 32-bit addressing");
        _vertexDataCPU.resize(_totalVertices + vertexCount);
    }
    bool reusedIndices = false;
    for (auto it = _freeIndexRanges.begin(); it != _freeIndexRanges.end(); ++it)
    {
        if (it->second < indexCount) continue;
        indexStart = it->first;
        const UINT remaining = it->second - indexCount;
        _freeIndexRanges.erase(it);
        if (remaining) _freeIndexRanges.emplace(indexStart + indexCount, remaining);
        reusedIndices = true;
        break;
    }
    if (!reusedIndices)
    {
        if (indexCount > UINT_MAX / INDEX_SIZE - _totalIndices)
            throw std::overflow_error("Index buffer exceeds 32-bit addressing");
        _indexDataCPU.resize(_totalIndices + indexCount);
    }
    replacement->VertexStart = vertexStart;
    replacement->VertexCapacity = vertexCount;
    replacement->IndexStart = indexStart;
    replacement->IndexCapacity = indexCount;
    UINT vertexOffset = vertexStart;
    UINT indexOffset = indexStart;
    for (size_t i = 0; i < mesh->GetSubMeshCount(); ++i)
    {
        const auto& source = mesh->GetSubMesh(i);
        auto& gpu = replacement->SubMeshes[i];
        std::copy(source.Vertices.begin(), source.Vertices.end(), _vertexDataCPU.begin() + vertexOffset);
        std::copy(source.Indices.begin(), source.Indices.end(), _indexDataCPU.begin() + indexOffset);
        for (size_t vertex = 0; vertex < source.GetVertexCount(); ++vertex)
            _vertexDataCPU[vertexOffset + vertex].PreviousPosition = source.Vertices[vertex].Position;
        gpu.StartVertexLocation = static_cast<INT>(vertexOffset);
        gpu.StartIndexLocation = indexOffset;
        gpu.IndexCount = static_cast<UINT>(source.GetIndexCount());
        gpu.MaterialIndex = source.MaterialIndex;
        vertexOffset += static_cast<UINT>(source.GetVertexCount());
        indexOffset += static_cast<UINT>(source.GetIndexCount());
    }
    ResizeVertexBuffer(static_cast<UINT>(_vertexDataCPU.size()), vertexStart, vertexCount);
    ResizeIndexBuffer(static_cast<UINT>(_indexDataCPU.size()));
    _meshCache[handle.GetValue()] = std::move(replacement);
    ++_meshCacheRevision;
}

const GPUMesh* GDX12GeometryBuffer::GetGPUMeshByHandle(const MeshHandle handle)
{
    const auto it = _meshCache.find(handle.GetValue());
    return it != _meshCache.end() && it->second->SourceHandle == handle ? it->second.get() : nullptr;
}

void GDX12GeometryBuffer::SetSnapshotBuffers(std::shared_ptr<GDX12Resource> vertices,
    std::shared_ptr<GDX12Resource> indices)
{
    for (const auto& buffer : {vertices, indices})
    {
        if (!buffer || !buffer->D3DResource || buffer->D3DResource->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
            buffer->D3DResource->GetDesc().Width > UINT_MAX) throw std::invalid_argument("Invalid geometry snapshot buffer");
        ComPtr<ID3D12Device> owner;
        ThrowIfFailed(buffer->D3DResource->GetDevice(IID_PPV_ARGS(&owner)));
        if (owner.Get() != static_cast<ID3D12Device*>(_device->GetDevice().Get()))
            throw std::invalid_argument("Geometry snapshot belongs to another adapter");
    }
    _vertexBuffer = std::move(vertices);
    _indexBuffer = std::move(indices);
    _vertexBufferView = {_vertexBuffer->D3DResource->GetGPUVirtualAddress(),
        static_cast<UINT>(_vertexBuffer->D3DResource->GetDesc().Width), sizeof(Vertex)};
    _indexBufferView = {_indexBuffer->D3DResource->GetGPUVirtualAddress(),
        static_cast<UINT>(_indexBuffer->D3DResource->GetDesc().Width), DXGI_FORMAT_R32_UINT};
}
