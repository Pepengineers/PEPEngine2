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
	D3D12_GPU_VIRTUAL_ADDRESS Main, Camera;
	std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 9> Views;
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
		if (!Draws) return;
		cmd->SetDescriptorHeaps(1, &Heap);
		cmd->SetGraphicsRootSignature(Root->GetRootSignature().Get());
		cmd->SetPipelineState(Pipeline);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(1), Main);
		cmd->SetGraphicsRootConstantBufferView(Root->GetBRootParamIndex(2), Camera);
		for (UINT i = 0; i < Views.size(); ++i)
		{
			if (Views[i].ptr) cmd->SetGraphicsRootDescriptorTable(Root->GetTRootParamIndex(i), Views[i]);
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
		cmd->ExecuteIndirect(Signature, Draws, Args, 0, Count, 0);
	}
};
} // namespace BistroRender
