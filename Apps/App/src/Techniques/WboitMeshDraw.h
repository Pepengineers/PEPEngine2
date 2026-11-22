#pragma once
#include "SceneGeometry.h"

namespace BistroRender
{
struct WboitMeshDraw
{
	GDX12RootSignature* Root;
	ID3D12PipelineState* Pipeline;
	ID3D12CommandSignature* Signature;
	ID3D12DescriptorHeap* Heap;
	D3D12_GPU_VIRTUAL_ADDRESS Main, Camera, Shadow, Sky;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 11> Views;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 3> Atmosphere;
	D3D12_GPU_DESCRIPTOR_HANDLE Textures;
	GDX12GeometryBuffer::RecordingBinding Geometry;
	D3D12_CPU_DESCRIPTOR_HANDLE Depth;
	D3D12_VIEWPORT Viewport;
	D3D12_RECT Rect;
	std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> Targets;
	UINT Draws;
	ID3D12Resource* Args;
	ID3D12Resource* Count;
	void operator()(ID3D12GraphicsCommandList* cmd) const

	{
		const float zero[4]{}, one[4]{1, 1, 1, 1};
		cmd->ClearRenderTargetView(Targets[0], zero, 0, nullptr);
		cmd->ClearRenderTargetView(Targets[1], one, 0, nullptr);
		cmd->SetDescriptorHeaps(1, &Heap);
		cmd->SetGraphicsRootSignature(Root->GetRootSignature().Get());
		cmd->SetPipelineState(Pipeline);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(1), Main);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(2), Camera);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(3), Shadow);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(4), Sky);
		for (UINT i = 0; i < Views.size(); ++i)
		{
			cmd->SetGraphicsRootDescriptorTable(Root->GetTRootParamIndex(i), Views[i]);
		}
		for (UINT i = 0; i < Atmosphere.size(); ++i)
		{
			cmd->SetGraphicsRootDescriptorTable(Root->GetTRootParamIndex(11 + i), Atmosphere[i]);
		}
		cmd->SetGraphicsRootDescriptorTable(Root->GetTRootParamIndex(16), Textures);
		cmd->SetGraphicsRootShaderResourceView(Root->GetGeometryRootParamIndex(),
		                                       Geometry.Vertices.BufferLocation);
		cmd->RSSetViewports(1, &Viewport);
		cmd->RSSetScissorRects(1, &Rect);
		cmd->OMSetRenderTargets(UINT(Targets.size()), Targets.data(), FALSE, &Depth);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		cmd->IASetVertexBuffers(0, 1, &Geometry.Vertices);
		cmd->IASetIndexBuffer(&Geometry.Indices);
		if (Draws)
		{
			cmd->ExecuteIndirect(Signature, Draws, Args, 0, Count, 0);
		}
	}
};
} // namespace BistroRender
