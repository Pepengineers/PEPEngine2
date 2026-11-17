#pragma once
#include "Engine.RendererDX12/GDX12DeviceResources.h"
namespace BistroRender
{
enum SceneBufferSlot : UINT
{
	SceneInstances,
	SceneCommands,
	SceneMaterials,
	SceneTransforms
};
constexpr UINT SceneVertices = 18;
constexpr UINT SceneIndices = SceneVertices + 1;
constexpr UINT ShadowEnabled = 1u;
constexpr UINT ShadowEveryFrame = 2u;

inline void BindShadowGeometry(ID3D12GraphicsCommandList* cmd, GDX12RootSignature* root,
                               const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 4>& scene,
                               const GDX12GeometryBuffer::RecordingBinding& geometry)
{
	constexpr SceneBufferSlot inputs[] = {SceneMaterials, SceneTransforms, SceneInstances};
	for (UINT i = 0; i < std::size(inputs); ++i)
	{
		cmd->SetGraphicsRootDescriptorTable(root->GetTRootParamIndex(i), scene[inputs[i]]);
	}
	const auto geometryRoot = root->GetGeometryRootParamIndex();
	cmd->SetGraphicsRootShaderResourceView(geometryRoot, geometry.Vertices.BufferLocation);
	cmd->SetGraphicsRootShaderResourceView(geometryRoot + 1, geometry.Vertices.BufferLocation);
	cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	cmd->IASetVertexBuffers(0, 1, &geometry.Vertices);
	cmd->IASetIndexBuffer(&geometry.Indices);
}
} // namespace BistroRender
