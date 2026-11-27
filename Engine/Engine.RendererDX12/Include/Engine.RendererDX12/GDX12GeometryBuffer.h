#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Descriptor.h"

#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.Core/AssetHandles.h"

using namespace Engine::Core;

static_assert(sizeof(Vertex) == 44 && offsetof(Vertex, Position) == 0 && offsetof(Vertex, Normal) == 12
    && offsetof(Vertex, TexCoord) == 24 && offsetof(Vertex, Tangent) == 32, "Geometry vertex layout must match CBufferStructures.hlsli");

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
			SubMeshes[i].IndexCount = CPUSubMesh.GetIndexCount();
		}
	}
};

class GDX12GeometryBuffer
{
public:
	GDX12GeometryBuffer(GDX12Device* device);
	~GDX12GeometryBuffer();
	void Clear();
	void AddMesh(const Mesh* mesh, MeshHandle handle);
	const GPUMesh* GetGPUMeshByHandle(MeshHandle handle);
	// Call during setup after prior GPU users complete, uploads refresh views after their existing synchronization.
	void CreateSRVs(GDX12DescriptorHeap* heap);
	const GDX12Descriptor* GetVertexSRV() const { return _vertexSRV.HeapIndex == UINT_MAX ? nullptr : &_vertexSRV; }
	const GDX12Descriptor* GetIndexSRV() const { return _indexSRV.HeapIndex == UINT_MAX ? nullptr : &_indexSRV; }

	// Return BLAS triangle inputs referencing the current device-local vertex and index buffers.
	// GeometryIndex follows SubMeshes order, no geometries are filtered or reordered.
	// Flags remain NONE so instance material overrides can use alpha-test, callers may mark proven opaque geometries.
	// Request descriptors again after buffer growth, their GPU addresses are not permanent.
	std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> GetRayTracingGeometryDescriptions(MeshHandle handle) const;

private:
	friend class RenderModule;
	friend class GDX12CommandList;
	friend class GDX12RenderPass;

	void ResizeVertexBuffer(UINT newSize);
	void ResizeIndexBuffer(UINT newSize);
	void UpdateVertexSRV(GDX12DescriptorHeap* heap);
	void UpdateIndexSRV(GDX12DescriptorHeap* heap);

	GDX12Device* _device;

	//MeshHandle -> GPUMesh Map
	std::unordered_map<std::uint32_t, std::unique_ptr<GPUMesh>> _meshCache;

	std::vector<Vertex> _vertexDataCPU;
	std::vector<uint32_t> _indexDataCPU;

	ComPtr<ID3D12Resource> _vertexBuffer;
	ComPtr<ID3D12Resource> _indexBuffer;
	GDX12Descriptor _vertexSRV;
	GDX12Descriptor _indexSRV;
	D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW _indexBufferView;

	UINT _totalVertices;
	UINT _totalIndices;

	static constexpr size_t VERTEX_SIZE = sizeof(Vertex);
	static constexpr size_t INDEX_SIZE = sizeof(std::uint32_t);
};
