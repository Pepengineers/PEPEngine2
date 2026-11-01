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
    _freeSkinRanges.clear();
    _freeIndexRanges.clear();
    _vertexState = D3D12_RESOURCE_STATE_COMMON;
    _vertexBuffer.Reset();
    _indexBuffer.Reset();
    _vertexBufferView = {};
    _indexBufferView = {};
    _totalIndices = 0;
    _totalVertices = 0;
}

void GDX12GeometryBuffer::ResizeVertexBuffer(UINT newSize, UINT updateStart, UINT updateCount)
{
    if (newSize > UINT_MAX / VERTEX_SIZE) throw std::overflow_error("Vertex buffer exceeds 32-bit addressing");
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * VERTEX_SIZE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto newBuffer = _device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
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
    list->CopyBufferRegion(newBuffer.Get(), 0, uploadBuffer.Get(), 0, newSize * VERTEX_SIZE);
    if (_vertexBuffer)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_vertexBuffer.Get(), _vertexState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        // Existing dynamic slices contain the last rendered pose, not their CPU initialization.
        list->CopyBufferRegion(newBuffer.Get(), 0, _vertexBuffer.Get(), 0, _vertexBufferView.SizeInBytes);
    }
    if (updateCount)
        list->CopyBufferRegion(newBuffer.Get(), updateStart * VERTEX_SIZE, uploadBuffer.Get(),
            updateStart * VERTEX_SIZE, updateCount * VERTEX_SIZE);
    auto ready = CD3DX12_RESOURCE_BARRIER::Transition(newBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    list->ResourceBarrier(1, &ready);
    queue->ExecuteCommandList(commands);
    queue->Flush();
    _vertexBuffer = newBuffer;
    _vertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    _totalVertices = newSize;
    _vertexBufferView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
    _vertexBufferView.SizeInBytes = static_cast<UINT>(_totalVertices * VERTEX_SIZE);
    _vertexBufferView.StrideInBytes = static_cast<UINT>(VERTEX_SIZE);
}

UINT GDX12GeometryBuffer::AllocateSkinnedVertices(UINT count)
{
    if (!count) throw std::invalid_argument("Empty skin slice");
    for (auto it = _freeSkinRanges.begin(); it != _freeSkinRanges.end(); ++it)
    {
        if (it->second < count) continue;
        const UINT offset = it->first;
        const UINT remaining = it->second - count;
        _freeSkinRanges.erase(it);
        if (remaining) _freeSkinRanges.emplace(offset + count, remaining);
        return offset;
    }
    if (count > UINT_MAX / VERTEX_SIZE - _totalVertices)
        throw std::overflow_error("Skin slice exceeds vertex buffer addressing");
    const UINT offset = _totalVertices;
    _vertexDataCPU.resize(_totalVertices + count);
    ResizeVertexBuffer(_totalVertices + count);
    return offset;
}

void GDX12GeometryBuffer::ReleaseSkinnedVertices(UINT offset, UINT count)
{
    if (!count) return;
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
    if (_vertexState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_vertexBuffer.Get(), _vertexState,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->GetCommandList()->ResourceBarrier(1, &barrier);
        _vertexState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    return _vertexBuffer.Get();
}

void GDX12GeometryBuffer::EndSkinning(GDX12CommandList* commands)
{
    if (_vertexState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) return;
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(_vertexBuffer.Get(), _vertexState,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    commands->GetCommandList()->ResourceBarrier(1, &barrier);
    _vertexState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
}

void GDX12GeometryBuffer::ResizeIndexBuffer(UINT newSize)
{
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(newSize * INDEX_SIZE);

    auto newBuffer = _device->CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc,
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

    cmdList->GetCommandList()->CopyResource(newBuffer.Get(), uploadBuffer.Get());

    cmdQueue->ExecuteCommandList(cmdList);
    cmdQueue->Flush();

    _indexBuffer = newBuffer;
    _totalIndices = newSize;

    _indexBufferView.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
    _indexBufferView.SizeInBytes = static_cast<UINT>(_totalIndices * INDEX_SIZE);
    _indexBufferView.Format = DXGI_FORMAT_R32_UINT;
}

void GDX12GeometryBuffer::AddMesh(const Mesh* mesh, MeshHandle handle, std::uint64_t contentRevision)
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
}

const GPUMesh* GDX12GeometryBuffer::GetGPUMeshByHandle(MeshHandle handle)
{
    const auto it = _meshCache.find(handle.GetValue());
    return it != _meshCache.end() && it->second->SourceHandle == handle ? it->second.get() : nullptr;
}
