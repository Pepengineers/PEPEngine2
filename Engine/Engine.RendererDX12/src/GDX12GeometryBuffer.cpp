#include "Engine.RendererDX12/GDX12GeometryBuffer.h"

#include "Engine.RendererDX12/GDX12CommandQueue.h"
#include "Engine.RendererDX12/GDX12CommandList.h"

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
    _vertexBuffer.Reset();
    _indexBuffer.Reset();
    _vertexBufferView = {};
    _indexBufferView = {};
    _totalIndices = 0;
    _totalVertices = 0;
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

    // Copy old data if it exists
    if (_vertexBuffer)
    {
        ComPtr<ID3D12Resource> uploadBuffer;
        _device->GetDevice()->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&uploadBuffer));

        void* mapped = nullptr;
        uploadBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, _vertexDataCPU.data(), _totalVertices * VERTEX_SIZE);
        uploadBuffer->Unmap(0, nullptr);

        auto cmdQueue = _device->GetCommandQueue();
        auto cmdList = cmdQueue->GetCommandList();

        CD3DX12_RESOURCE_BARRIER Barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                _vertexBuffer.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE)
        };

        cmdList->GetCommandList()->ResourceBarrier(1, Barriers);

        cmdList->GetCommandList()->CopyResource(newBuffer.Get(), uploadBuffer.Get());

        CD3DX12_RESOURCE_BARRIER antiBarriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                _vertexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON)
        };

        cmdList->GetCommandList()->ResourceBarrier(1, antiBarriers);

        cmdQueue->ExecuteCommandList(cmdList);
        cmdQueue->Flush();
    }

    _vertexBuffer = newBuffer;
    _totalVertices = newSize;

    _vertexBufferView.BufferLocation = _vertexBuffer->GetGPUVirtualAddress();
    _vertexBufferView.SizeInBytes = static_cast<UINT>(_totalVertices * VERTEX_SIZE);
    _vertexBufferView.StrideInBytes = static_cast<UINT>(VERTEX_SIZE);
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

    // Copy old data if it exists
    if (_indexBuffer)
    {
        ComPtr<ID3D12Resource> uploadBuffer;
        auto desc = CD3DX12_RESOURCE_DESC::Buffer(_totalIndices * INDEX_SIZE);
        _device->GetDevice()->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&uploadBuffer));

        void* mapped = nullptr;
        uploadBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, _indexDataCPU.data(), _totalIndices * INDEX_SIZE);
        uploadBuffer->Unmap(0, nullptr);

        auto cmdQueue = _device->GetCommandQueue();
        auto cmdList = cmdQueue->GetCommandList();

        CD3DX12_RESOURCE_BARRIER Barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                _indexBuffer.Get(),
                D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_COPY_SOURCE)
        };

        cmdList->GetCommandList()->ResourceBarrier(1, Barriers);

        cmdList->GetCommandList()->CopyResource(newBuffer.Get(), uploadBuffer.Get());

        CD3DX12_RESOURCE_BARRIER antiBarriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(
                _indexBuffer.Get(),
                D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_COMMON)
        };

        cmdList->GetCommandList()->ResourceBarrier(1, antiBarriers);

        cmdQueue->ExecuteCommandList(cmdList);
        cmdQueue->Flush();
    }

    _indexBuffer = newBuffer;
    _totalIndices = newSize;

    _indexBufferView.BufferLocation = _indexBuffer->GetGPUVirtualAddress();
    _indexBufferView.SizeInBytes = static_cast<UINT>(_totalIndices * INDEX_SIZE);
    _indexBufferView.Format = DXGI_FORMAT_R32_UINT;
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

        for (size_t idx = 0; idx < CPUSubMesh.Indices.size(); ++idx)
        {
            _indexDataCPU[currentIndexOffset + idx] = CPUSubMesh.Indices[idx] + currentVertexOffset;
        }

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
