#pragma once

#include "Engine.RendererDX12/D3DHelpers.h"
#include "Engine.RendererDX12/GDX12Device.h"
#include "Engine.RendererDX12/GDX12Resource.h"

#include "Engine.Core/Types/MeshTypes.h"
#include "Engine.Core/AssetHandles.h"
#include <map>

using namespace Engine::Core;
class GDX12CommandList;

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
	MeshHandle SourceHandle;
	std::uint64_t ContentRevision = 0;
	UINT VertexStart = 0;
	UINT VertexCapacity = 0;
	UINT IndexStart = 0;
	UINT IndexCapacity = 0;

	GPUMesh(const Mesh* mesh) : CPUMesh(mesh)
	{
		for (int i = 0; i < CPUMesh->GetSubMeshCount(); i++)
		{
			auto& CPUSubMesh = CPUMesh->GetSubMesh(i);
			SubMeshes.push_back(GPUSubMesh(CPUSubMesh));
			SubMeshes[i].IndexCount = static_cast<std::uint32_t>(CPUSubMesh.GetIndexCount());
		}
	}
};

class GDX12GeometryBuffer
{
public:
	GDX12GeometryBuffer(GDX12Device* device);
	~GDX12GeometryBuffer();
	void Clear();
	void AddMesh(const Mesh* mesh, MeshHandle handle, std::uint64_t contentRevision = 0);
	void ReserveSkinnedVertices(UINT count);
	UINT AllocateSkinnedVertices(UINT count);
	void ReleaseSkinnedVertices(UINT offset, UINT count);
    const std::map<UINT, UINT>& GetSkinnedVertexRanges() const { return _skinnedRanges; }
	ID3D12Resource* BeginSkinning(GDX12CommandList* commands);
	void EndSkinning(GDX12CommandList* commands);
	const GPUMesh* GetGPUMeshByHandle(MeshHandle handle);
    uint64_t GetMeshCacheRevision() const { return _meshCacheRevision; }
    D3D12_GPU_VIRTUAL_ADDRESS GetVertexAddress() const
    { return _vertexBuffer ? _vertexBuffer->D3DResource->GetGPUVirtualAddress() : 0; }
    std::shared_ptr<GDX12Resource> ShareVertexResource() const { return _vertexBuffer; }
    std::shared_ptr<GDX12Resource> ShareIndexResource() const { return _indexBuffer; }
    void SetSnapshotBuffers(std::shared_ptr<GDX12Resource> vertices, std::shared_ptr<GDX12Resource> indices);

private:
	friend class RenderModule;
	friend class GDX12CommandList;

	void ResizeVertexBuffer(UINT newSize, UINT updateStart = 0, UINT updateCount = 0);
	void ResizeIndexBuffer(UINT newSize);

	GDX12Device* _device;

	//MeshHandle -> GPUMesh Map
	std::unordered_map<std::uint32_t, std::unique_ptr<GPUMesh>> _meshCache;
    uint64_t _meshCacheRevision = 0;

	std::vector<Vertex> _vertexDataCPU;
	std::vector<uint32_t> _indexDataCPU;

	std::shared_ptr<GDX12Resource> _vertexBuffer;
	std::shared_ptr<GDX12Resource> _indexBuffer;
	D3D12_VERTEX_BUFFER_VIEW _vertexBufferView;
	D3D12_INDEX_BUFFER_VIEW _indexBufferView;

	static constexpr auto VertexReadState = static_cast<D3D12_RESOURCE_STATES>(
        UINT(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) | UINT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
	std::map<UINT, UINT> _freeSkinRanges;
    std::map<UINT, UINT> _skinnedRanges;
	std::map<UINT, UINT> _freeIndexRanges;
	UINT _totalVertices;
	UINT _totalIndices;

	static constexpr size_t VERTEX_SIZE = sizeof(Vertex);
	static constexpr size_t INDEX_SIZE = sizeof(std::uint32_t);
};
