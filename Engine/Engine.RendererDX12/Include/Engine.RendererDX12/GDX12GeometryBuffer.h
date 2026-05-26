#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"

#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.Core/AssetHandles.h"

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

private:
	friend class RenderModule;
	friend class GDX12CommandList;

	void ResizeVertexBuffer(UINT newSize);
	void ResizeIndexBuffer(UINT newSize);

	GDX12Device* _device;

	//MeshHandle -> GPUMesh Map
	std::unordered_map<std::uint32_t, std::unique_ptr<GPUMesh>> _meshCache;

	std::vector<Vertex> _vertexDataCPU;
	std::vector<uint32_t> _indexDataCPU;

	ComPtr<ID3D12Resource> _vertexBuffer;
	ComPtr<ID3D12Resource> _indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW _indexBufferView;

	UINT _totalVertices;
	UINT _totalIndices;

	static constexpr size_t VERTEX_SIZE = sizeof(Vertex);
	static constexpr size_t INDEX_SIZE = sizeof(std::uint32_t);
};