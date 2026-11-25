#pragma once

#include "ShadowDepthTransfer.h"
#include "Engine.RendererDX12/GDX12LocalLights.h"
#include "Engine.RendererDX12/GDX12RenderHelpers.h"
#include "Engine.RendererDX12/GDX12ShaderCompiler.h"

namespace BistroRender
{
class LocalShadowTileCopy
{
public:
	struct Region
	{
		GDX12LocalShadowTile From, To;
		D3D12_CPU_DESCRIPTOR_HANDLE Dsv;
	};

	explicit LocalShadowTileCopy(GDX12Device& device)
	{
		GDX12RootSignatureDesc desc;
		desc.Constants = {7};
		desc.NumSingleSRVSlots = desc.NumSingleUAVSlots = 1;
		_packRoot = std::make_unique<GDX12RootSignature>(&device, desc);
		const D3D_SHADER_MACRO defines[] = {{"PACK_LOCAL_TILE", "1"}, {nullptr, nullptr}};
		_pack = GDX12RenderHelpers::CreateComputePipeline(&device, _packRoot.get(),
		    APP_SHADERS_FOLDER "LocalShadowTileCopy.hlsl", "Pack", defines);
		desc.NumSingleUAVSlots = 0;
		_drawRoot = std::make_unique<GDX12RootSignature>(&device, desc);
		auto& compiler = GDX12ShaderCompiler::GetInstance();
		auto vs = compiler.CompileShader(&device, APP_SHADERS_FOLDER "LocalShadowTileCopy.hlsl", nullptr, "VS", "vs");
		auto ps = compiler.CompileShader(&device, APP_SHADERS_FOLDER "LocalShadowTileCopy.hlsl", nullptr, "PS", "ps");
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
		pipeline.pRootSignature = _drawRoot->GetRootSignature().Get();
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
		ThrowIfFailed(device.GetDevice()->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&_draw)));
	}

	Id Pack(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	        ID3D12Resource* from, ID3D12Resource* to, GDX12DescriptorHeap& heap,
	        std::vector<GDX12LocalShadowTile> tiles) const
	{
		const Id source = NativeWork::ResourceFor(graph, bindings, from, "local shadow atlas");
		const Id packed = NativeWork::ResourceFor(graph, bindings, to, "packed local shadow tiles");
		return work.Add(graph, "pack local shadow tiles", {{source, Mode::Read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE},
		    {packed, Mode::Write, UnorderedAccess}}, [this, &heap, tiles = std::move(tiles)](auto* cmd)
		{
			auto* descriptors = heap.GetHeap().Get();
			cmd->SetDescriptorHeaps(1, &descriptors);
			cmd->SetComputeRootSignature(_packRoot->GetRootSignature().Get());
			cmd->SetPipelineState(_pack.Get());
			cmd->SetComputeRootDescriptorTable(_packRoot->GetTRootParamIndex(0), heap.GetGPUHandle(0));
			cmd->SetComputeRootDescriptorTable(_packRoot->GetURootParamIndex(0), heap.GetGPUHandle(1));
			for (UINT slice = 0; slice < tiles.size(); ++slice)
			{
				const auto& tile = tiles[slice];
				const UINT constants[]{tile.X, tile.Y, tile.Page, tile.Side, 0, 0, slice};
				cmd->SetComputeRoot32BitConstants(0, std::size(constants), constants, 0);
				cmd->Dispatch((tile.Side + 7) / 8, (tile.Side + 7) / 8, 1);
			}
		});
	}

	Id Draw(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	        ID3D12Resource* from, ID3D12Resource* to, ID3D12DescriptorHeap* heap,
	        D3D12_GPU_DESCRIPTOR_HANDLE srv, std::vector<Region> regions, const char* name) const
	{
		const Id source = NativeWork::ResourceFor(graph, bindings, from, "local shadow tile source");
		const Id target = NativeWork::ResourceFor(graph, bindings, to, "local shadow atlas");
		return work.Add(graph, name, {{source, Mode::Read, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE},
		    {target, Mode::Write, D3D12_RESOURCE_STATE_DEPTH_WRITE}}, [this, heap, srv, regions = std::move(regions)](auto* cmd)
		{
			cmd->SetDescriptorHeaps(1, &heap);
			cmd->SetGraphicsRootSignature(_drawRoot->GetRootSignature().Get());
			cmd->SetPipelineState(_draw.Get());
			cmd->SetGraphicsRootDescriptorTable(_drawRoot->GetTRootParamIndex(0), srv);
			cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			for (const auto& region : regions)
			{
				const auto& [from, to, dsv] = region;
				const D3D12_VIEWPORT viewport{float(to.X), float(to.Y), float(to.Side), float(to.Side), 0, 1};
				const D3D12_RECT rect{LONG(to.X), LONG(to.Y), LONG(to.X + to.Side), LONG(to.Y + to.Side)};
				const UINT constants[]{from.X, from.Y, from.Page, to.Side, to.X, to.Y, to.Page};
				cmd->RSSetViewports(1, &viewport);
				cmd->RSSetScissorRects(1, &rect);
				cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
				cmd->SetGraphicsRoot32BitConstants(0, std::size(constants), constants, 0);
				cmd->DrawInstanced(3, 1, 0, 0);
			}
		});
	}

private:
	std::unique_ptr<GDX12RootSignature> _packRoot, _drawRoot;
	ComPtr<ID3D12PipelineState> _pack, _draw;
};

// Depth rectangles must travel through ordinary R32 surfaces. The cross-adapter
// buffer copies whole packed layers, while the atlas itself only sees shaders.
class LocalShadowTileTransfer
{
public:
	struct Bytes
	{
		UINT64 Primary = 0, Owner = 0;
		Bytes& operator+=(Bytes other)
		{
			Primary += (std::min)(other.Primary, UINT64_MAX - Primary);
			Owner += (std::min)(other.Owner, UINT64_MAX - Owner);
			return *this;
		}
	};

	static Bytes Cost(GDX12Device& primary, GDX12Device& owner, UINT side, UINT count, bool fromPrimary)
	{
		const auto packed = ShadowMemory::Depth(side, count, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto received = ShadowMemory::Depth(side, count, D3D12_RESOURCE_FLAG_NONE);
		const auto shared = ShadowMemory::TransferBytes(primary, owner, side, count);
		const auto withShared = [shared](UINT64 resource) { return resource > UINT64_MAX - shared ? UINT64_MAX : resource + shared; };
		return {withShared(ShadowMemory::Size(primary, fromPrimary ? packed : received)),
		        withShared(ShadowMemory::Size(owner, fromPrimary ? received : packed))};
	}

	LocalShadowTileTransfer(const QueueSubmission& submit, Id owner, UINT side, UINT count, bool fromPrimary)
	    : _side(side), _count(count), _transfer(submit, owner, side, count),
	      _sender(submit.Device(fromPrimary ? 0 : owner)), _receiver(submit.Device(fromPrimary ? owner : 0))
	{
		const auto packed = ShadowMemory::Depth(side, count, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
		const auto received = ShadowMemory::Depth(side, count, D3D12_RESOURCE_FLAG_NONE);
		_packed = _sender.CreateResource(D3D12_HEAP_TYPE_DEFAULT, packed, D3D12_RESOURCE_STATE_COMMON);
		_received = _receiver.CreateResource(D3D12_HEAP_TYPE_DEFAULT, received, D3D12_RESOURCE_STATE_COMMON);
		_packMemory.Set(_sender, ShadowMemory::Size(_sender, packed));
		_receiveMemory.Set(_receiver, ShadowMemory::Size(_receiver, received));
		_sendHeap = std::make_unique<GDX12DescriptorHeap>(&_sender, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		    2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		_receiveHeap = std::make_unique<GDX12DescriptorHeap>(&_receiver, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		    1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = DXGI_FORMAT_R32_FLOAT;
		uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
		uav.Texture2DArray.ArraySize = count;
		_sender.GetDevice()->CreateUnorderedAccessView(_packed.Get(), nullptr, &uav, _sendHeap->GetCPUHandle(1));
		const auto srv = ShadowMemory::View(count);
		_receiver.GetDevice()->CreateShaderResourceView(_received.Get(), &srv, _receiveHeap->GetCPUHandle(0));
	}

	Id Export(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, const LocalShadowTileCopy& copy,
	          ID3D12Resource* atlas, std::vector<GDX12LocalShadowTile> tiles)
	{
		const auto srv = ShadowMemory::View(atlas->GetDesc().DepthOrArraySize);
		_sender.GetDevice()->CreateShaderResourceView(atlas, &srv, _sendHeap->GetCPUHandle(0));
		copy.Pack(graph, bindings, work, atlas, _packed.Get(), *_sendHeap, std::move(tiles));
		return _transfer.Append(graph, bindings, work, _packed.Get(), Slices(), true);
	}

	Id Stage(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work) const
	{
		return _transfer.Append(graph, bindings, work, _received.Get(), Slices(), false);
	}

	Id Publish(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, const LocalShadowTileCopy& copy,
	           ID3D12Resource* atlas, std::vector<LocalShadowTileCopy::Region> regions) const
	{
		return copy.Draw(graph, bindings, work, _received.Get(), atlas, _receiveHeap->GetHeap().Get(),
		    _receiveHeap->GetGPUHandle(0), std::move(regions), "publish local shadow tiles");
	}

	GDX12LocalShadowTile PackedTile(UINT page) const { return {page, 0, 0, _side}; }

private:
	std::vector<UINT> Slices() const
	{
		std::vector<UINT> slices(_count);
		for (UINT i = 0; i < _count; ++i) slices[i] = i;
		return slices;
	}
	UINT _side, _count;
	ShadowDepthTransfer _transfer;
	GDX12Device& _sender;
	GDX12Device& _receiver;
	ShadowMemory::Charge _packMemory, _receiveMemory;
	GDX12ResourceHandle _packed, _received;
	std::unique_ptr<GDX12DescriptorHeap> _sendHeap, _receiveHeap;
};

// A full depth layer needs only a shared buffer when the receiver can replace its page.
class LocalShadowPageTransfer
{
public:
	static LocalShadowTileTransfer::Bytes Cost(GDX12Device& primary, GDX12Device& owner, UINT side, bool allowPartial)
	{
		const auto shared = ShadowMemory::TransferBytes(primary, owner, side, 1);
		LocalShadowTileTransfer::Bytes bytes{shared, shared};
		if (allowPartial) bytes += {ShadowMemory::Size(primary, StagingShape(side)), 0};
		return bytes;
	}

	LocalShadowPageTransfer(const QueueSubmission& submit, Id owner, UINT side, bool allowPartial)
	    : _transfer(submit, owner, side, 1)
	{
		if (!allowPartial) return;
		auto& device = submit.Device(0);
		const auto shape = StagingShape(side);
		_staging = device.CreateResource(D3D12_HEAP_TYPE_DEFAULT, shape, D3D12_RESOURCE_STATE_COMMON);
		_stagingMemory.Set(device, ShadowMemory::Size(device, shape));
		_stagingHeap = std::make_unique<GDX12DescriptorHeap>(&device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
		    1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
		const auto srv = ShadowMemory::View(1);
		device.GetDevice()->CreateShaderResourceView(_staging.Get(), &srv, _stagingHeap->GetCPUHandle(0));
	}

	Id Export(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	          ID3D12Resource* ownerAtlas, UINT ownerSlice) const
	{
		return _transfer.Append(graph, bindings, work, ownerAtlas, {ownerSlice}, true);
	}

	Id Import(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work,
	          ID3D12Resource* targetAtlas, UINT targetSlice) const
	{
		return _transfer.Append(graph, bindings, work, targetAtlas, {targetSlice}, false);
	}

	Id Publish(Graph& graph, std::vector<ID3D12Resource*>& bindings, NativeWork& work, const LocalShadowTileCopy& copy,
	           ID3D12Resource* targetAtlas, std::vector<LocalShadowTileCopy::Region> regions) const
	{
		if (!_staging) throw std::logic_error("Local shadow page has no staging texture");
		std::vector<D3D12_BOX> boxes;
		boxes.reserve(regions.size());
		for (auto& region : regions)
		{
			auto& tile = region.From;
			boxes.push_back({tile.X, tile.Y, 0, tile.X + tile.Side, tile.Y + tile.Side, 1});
			tile.Page = 0;
		}
		_transfer.ImportPageRegions(graph, bindings, work, _staging.Get(), std::move(boxes));
		return copy.Draw(graph, bindings, work, _staging.Get(), targetAtlas, _stagingHeap->GetHeap().Get(),
		    _stagingHeap->GetGPUHandle(0), std::move(regions), "publish selected local shadow page tiles");
	}

private:
	static CD3DX12_RESOURCE_DESC StagingShape(UINT side)
	{
		return ShadowMemory::Depth(side, 1, D3D12_RESOURCE_FLAG_NONE);
	}

	ShadowDepthTransfer _transfer;
	ShadowMemory::Charge _stagingMemory;
	GDX12ResourceHandle _staging;
	std::unique_ptr<GDX12DescriptorHeap> _stagingHeap;
};
}
