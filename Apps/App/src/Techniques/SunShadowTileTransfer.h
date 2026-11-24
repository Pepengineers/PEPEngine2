#pragma once

#include "ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

namespace BistroRender
{
class SunShadowTileTransfer
{
  public:
	SunShadowTileTransfer(const QueueSubmission& submit, Id owner, GDX12DescriptorHeap& primaryHeap,
	                      GDX12DescriptorHeap& ownerHeap, const SunShadowTileTransfer* previous = nullptr)
	    : _submit(submit), _owner(owner), _primaryHeap(primaryHeap), _ownerHeap(ownerHeap)
	{
		if (previous)
		{
			_packRoot = previous->_packRoot;
			_publishRoot = previous->_publishRoot;
			_pack = previous->_pack;
			_publish = previous->_publish;
			return;
		}
		auto& producer = submit.Device(owner);
		auto& primary = submit.Device(0);
		GDX12RootSignatureDesc desc;
		desc.Constants = {4};
		desc.NumSingleSRVSlots = desc.NumSingleUAVSlots = 1;
		_packRoot = std::make_shared<GDX12RootSignature>(&producer, desc);
		const D3D_SHADER_MACRO defines[] = {{"PACK_SUN_TILE", "1"}, {nullptr, nullptr}};
		_pack = GDX12RenderHelpers::CreateComputePipeline(&producer, _packRoot.get(),
		    APP_SHADERS_FOLDER "SunShadowTileTransfer.hlsl", "Pack", defines);
		desc.NumSingleUAVSlots = 0;
		_publishRoot = std::make_shared<GDX12RootSignature>(&primary, desc);
		auto& compiler = GDX12ShaderCompiler::GetInstance();
		auto vs = compiler.CompileShader(&primary, APP_SHADERS_FOLDER "SunShadowTileTransfer.hlsl", nullptr, "VS", "vs");
		auto ps = compiler.CompileShader(&primary, APP_SHADERS_FOLDER "SunShadowTileTransfer.hlsl", nullptr, "PS", "ps");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = _publishRoot->GetRootSignature().Get();
		pipeline.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pipeline.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		pipeline.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pipeline.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		pipeline.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		pipeline.SampleMask = UINT_MAX;
		pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
		pipeline.SampleDesc.Count = 1;
		ThrowIfFailed(primary.GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_publish)));
	}

	void Prepare(UINT resolution, UINT mask)
	{
		_tiles.clear();
		for (UINT cascade = 0; cascade < 4; ++cascade)
			if (mask & (1u << cascade)) _tiles.push_back(cascade);
		const UINT count = UINT(_tiles.size());
		if (_resolution == resolution && _count == count) return;
		_resolution = resolution;
		_count = count;
		auto desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_TYPELESS, resolution, resolution, UINT16(count), 1);
		_primaryTiles = _submit.Device(0).CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		_ownerTiles = _submit.Device(_owner).CreateResource(D3D12_HEAP_TYPE_DEFAULT, desc, D3D12_RESOURCE_STATE_COMMON);
		D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = DXGI_FORMAT_R32_FLOAT;
		srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv.Texture2DArray.ArraySize = count;
		srv.Texture2DArray.MipLevels = 1;
		_srv.InitAsSRV(_primaryTiles.Get(), &srv, &_primaryHeap, _srv.HeapIndex == UINT_MAX
		    ? _primaryHeap.GetAvailableIndex(SceneDescriptorStart) : _srv.HeapIndex);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_FLOAT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		uav.Texture2DArray.ArraySize = count;
		_uav.InitAsUAV(_ownerTiles.Get(), nullptr, &uav, &_ownerHeap, _uav.HeapIndex == UINT_MAX
		    ? _ownerHeap.GetAvailableIndex(SceneDescriptorStart) : _uav.HeapIndex);
		_transfer = std::make_unique<ShadowDepthTransfer>(_submit, _owner, resolution, count, false);
	}

	void Export(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, GDX12Texture& atlas)
	{
		const Id source = NativeWork::ResourceFor(graph, bindings, atlas.GetResource()->D3DResource.Get(), "owner sun atlas");
		const Id packed = NativeWork::ResourceFor(graph, bindings, _ownerTiles.Get(), "packed owner sun tiles");
		const auto srv = atlas.GetSRV()->GPUHandle;
		work.Add(graph, "pack dirty owner CSM tiles", {{source, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
		         {packed, Mode::Write, UnorderedAccess}}, [this, srv](auto* cmd)
		{
			auto* heap = _ownerHeap.GetHeap().Get();
			cmd->SetDescriptorHeaps(1, &heap);
			cmd->SetComputeRootSignature(_packRoot->GetRootSignature().Get());
			cmd->SetPipelineState(_pack.Get());
			cmd->SetComputeRootDescriptorTable(_packRoot->GetTRootParamIndex(0), srv);
			cmd->SetComputeRootDescriptorTable(_packRoot->GetURootParamIndex(0), _uav.GPUHandle);
			for (UINT slice = 0; slice < _count; ++slice)
			{
				const auto constants = Constants(slice);
				cmd->SetComputeRoot32BitConstants(0, 4, constants.data(), 0);
				cmd->Dispatch((_resolution + 7) / 8, (_resolution + 7) / 8, 1);
				const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(_ownerTiles.Get());
				cmd->ResourceBarrier(1, &barrier);
			}
		});
		_transfer->Append(graph, bindings, work, _ownerTiles.Get(), Slices(), true);
	}

	Id Import(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, GDX12Texture& atlas)
	{
		const Id imported = Stage(graph, bindings, work);
		Publish(graph, bindings, work, atlas, UINT_MAX);
		return imported;
	}

	Id Stage(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work)
	{
		return _transfer->Append(graph, bindings, work, _primaryTiles.Get(), Slices(), false);
	}

	void Publish(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, GDX12Texture& atlas, UINT mask)
	{
		const Id packed = NativeWork::ResourceFor(graph, bindings, _primaryTiles.Get(), "GPU0 received sun tiles");
		const Id depth = NativeWork::ResourceFor(graph, bindings, atlas.GetResource()->D3DResource.Get(), "GPU0 sun atlas");
		const auto dsv = atlas.GetDSV()->CPUHandle;
		work.Add(graph, "publish dirty remote CSM tiles on GPU0",
		         {{packed, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		          {depth, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}}, [this, dsv, mask](auto* cmd)
		{
			auto* heap = _primaryHeap.GetHeap().Get();
			cmd->SetDescriptorHeaps(1, &heap);
			cmd->SetGraphicsRootSignature(_publishRoot->GetRootSignature().Get());
			cmd->SetPipelineState(_publish.Get());
			cmd->SetGraphicsRootDescriptorTable(_publishRoot->GetTRootParamIndex(0), _srv.GPUHandle);
			cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			for (UINT slice = 0; slice < _count; ++slice)
			{
				if (!(mask & (1u << _tiles[slice]))) continue;
				const auto constants = Constants(slice);
				const LONG x = LONG(constants[0]), y = LONG(constants[1]), size = LONG(_resolution);
				const D3D12_VIEWPORT viewport{float(x), float(y), float(size), float(size), 0, 1};
				const D3D12_RECT rect{x, y, x + size, y + size};
				cmd->RSSetViewports(1, &viewport);
				cmd->RSSetScissorRects(1, &rect);
				cmd->SetGraphicsRoot32BitConstants(0, 4, constants.data(), 0);
				cmd->DrawInstanced(3, 1, 0, 0);
			}
		});
	}

  private:
	std::array<UINT, 4> Constants(UINT slice) const
	{
		const UINT cascade = _tiles[slice];
		return {(cascade & 1u) * _resolution, (cascade >> 1u) * _resolution, slice, _resolution};
	}
	std::vector<UINT> Slices() const
	{
		std::vector<UINT> slices(_count);
		for (UINT i = 0; i < _count; ++i) slices[i] = i;
		return slices;
	}
	const QueueSubmission& _submit;
	Id _owner;
	GDX12DescriptorHeap& _primaryHeap;
	GDX12DescriptorHeap& _ownerHeap;
	std::shared_ptr<GDX12RootSignature> _packRoot, _publishRoot;
	ComPtr<ID3D12PipelineState> _pack, _publish;
	GDX12ResourceHandle _primaryTiles, _ownerTiles;
	GDX12Descriptor _srv, _uav;
	std::unique_ptr<ShadowDepthTransfer> _transfer;
	std::vector<UINT> _tiles;
	UINT _resolution = 0, _count = 0;
};
} // namespace BistroRender
