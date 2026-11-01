#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.Core/AssetHandles.h"
#include "Engine.RendererDX12/GDX12Resource.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"

using namespace Engine::Core;

struct GPUSubMesh
{
	std::uint32_t IndexCount = 0;
	std::uint32_t StartIndexLocation = 0;
	std::int32_t StartVertexLocation = 0;
	std::uint32_t MaterialIndex = 0;
	const SubMesh* CPUSubmesh;

	GPUSubMesh(const SubMesh& subMesh) : CPUSubmesh(&subMesh)
	{

	}
};

struct GPUMesh
{
	std::vector<GPUSubMesh> SubMeshes;
	const Mesh* CPUMesh;

	GPUMesh(const Mesh* mesh) : CPUMesh(mesh)
	{
		for (int i = 0; i < CPUMesh->GetSubMeshCount(); i++)
		{
			auto& CPUSubMesh = CPUMesh->GetSubMesh(i);
			SubMeshes.push_back(GPUSubMesh(CPUSubMesh));
			SubMeshes[i].IndexCount = static_cast<uint32_t>(CPUSubMesh.GetIndexCount());
		}
	}
};

struct GDX12GeneratedGeometryRange
{
    UINT VertexBase = 0, IndexBase = 0, PreviousBase = 0, Capacity = 0;
};

class GDX12GeometryBuffer
{
public:
	GDX12GeometryBuffer(GDX12Device* device);
	~GDX12GeometryBuffer();
	void Clear();
	void AddMesh(const Mesh* mesh, MeshHandle handle);
	const GPUMesh* GetGPUMeshByHandle(MeshHandle handle);

    void InitializeViews(GDX12DescriptorHeap* heap);
    GDX12GeneratedGeometryRange AllocateGenerated(UINT vertexCapacity);
    void ReleaseGenerated(GDX12GeneratedGeometryRange range);
    GDX12Resource& Vertices() { return _vertexBuffer; }
    GDX12Resource& PreviousPositions() { return _previousPositions; }
    GDX12Descriptor* VertexUAV() { return &_vertexUAV; }
    GDX12Descriptor* PreviousSRV() { return &_previousSRV; }
    GDX12Descriptor* PreviousUAV() { return &_previousUAV; }

private:
	friend class RenderModule;
	friend class GDX12CommandList;

    void Grow(GDX12Resource& buffer, UINT64 bytes, D3D12_RESOURCE_FLAGS flags);
    void Upload(GDX12Resource& buffer, UINT64 offset, const void* data, UINT64 bytes);
    void UpdateViews();

	GDX12Device* _device;

	//MeshHandle -> GPUMesh Map
	std::unordered_map<std::uint32_t, std::unique_ptr<GPUMesh>> _meshCache;

    GDX12DescriptorHeap* _heap = nullptr;
    GDX12Descriptor _vertexUAV, _previousSRV, _previousUAV;
    UINT _vertexUAVIndex = UINT_MAX, _previousSRVIndex = UINT_MAX, _previousUAVIndex = UINT_MAX;
    GDX12Resource _previousPositions;
    UINT _previousCount = 0;
    std::vector<GDX12GeneratedGeometryRange> _freeRanges;

	GDX12Resource _vertexBuffer;
	GDX12Resource _indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW _indexBufferView;

	UINT _totalVertices;
	UINT _totalIndices;

	static constexpr size_t VERTEX_SIZE = sizeof(Vertex);
	static constexpr size_t INDEX_SIZE = sizeof(std::uint32_t);
};